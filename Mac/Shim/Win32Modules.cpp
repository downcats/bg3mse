// Modules. An HMODULE is the address of a loaded Mach-O image's header, as on
// Windows it is the address the module is loaded at. dyld is asked for
// everything: which images are loaded, their paths, their exported symbols.
#include <windows.h>
#include <psapi.h>

#include "Win32Images.h"

#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <string>

using bg3se::shim::Image;
using bg3se::shim::ImageOf;

namespace {

Image ImageAt(uint32_t index)
{
    auto header = reinterpret_cast<mach_header_64 const*>(_dyld_get_image_header(index));
    char const* path = _dyld_get_image_name(index);
    if (!header || !path || header->magic != MH_MAGIC_64) {
        return {};
    }
    return { header, path };
}

Image MainExecutable()
{
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        Image image = ImageAt(i);
        if (image && image.IsExecutable()) return image;
    }
    return {};
}

char const* FileNameOf(char const* path)
{
    char const* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// A loaded image by name: a name with a directory is compared with the whole
// path, a bare name with the file name. Without regard to case, as on Windows.
Image ImageNamed(std::string const& name)
{
    bool wholePath = name.find('/') != std::string::npos;
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        Image image = ImageAt(i);
        if (!image) continue;
        char const* candidate = wholePath ? image.Path : FileNameOf(image.Path);
        if (strcasecmp(candidate, name.c_str()) == 0) return image;
    }
    return {};
}

// UTF-8, with '\' as a separator like '/'.
std::string PathFromWide(LPCWSTR wide)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string path((size_t)(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) {
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, path.data(), n, nullptr, nullptr);
    }
    for (char& c : path) {
        if (c == '\\') c = '/';
    }
    return path;
}

HMODULE HandleOf(Image const& image)
{
    return const_cast<mach_header_64*>(image.Header);
}

HMODULE ModuleNamed(std::string const& name)
{
    Image image = ImageNamed(name);
    if (!image) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    return HandleOf(image);
}

// dyld's handle for a loaded image. It holds a reference: the caller closes it.
// RTLD_FIRST: dlsym on the handle looks in that image only.
void* OpenLoaded(Image const& image)
{
    return dlopen(image.Path, RTLD_LAZY | RTLD_NOLOAD | RTLD_FIRST);
}

// The image a dlopen handle is for. dyld gives the same image the same handle
// (for the same flags), so the image whose own handle equals `handle` is it.
// The candidates most likely to be it are tried first: the newest images.
Image ImageOfHandle(void* handle)
{
    for (uint32_t i = _dyld_image_count(); i-- > 0;) {
        Image image = ImageAt(i);
        if (!image || image.IsExecutable()) continue;
        void* candidate = OpenLoaded(image);
        if (!candidate) continue;
        dlclose(candidate);
        if (candidate == handle) return image;
    }
    return {};
}

}

Image bg3se::shim::ImageOf(HMODULE module)
{
    if (!module) {
        return MainExecutable();
    }
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        Image image = ImageAt(i);
        if (image && image.Header == module) return image;
    }
    return {};
}

HMODULE GetModuleHandleW(LPCWSTR moduleName)
{
    if (!moduleName) {
        Image image = MainExecutable();
        if (!image) SetLastError(ERROR_MOD_NOT_FOUND);
        return HandleOf(image);
    }
    return ModuleNamed(PathFromWide(moduleName));
}

HMODULE GetModuleHandleA(LPCSTR moduleName)
{
    if (!moduleName) {
        return GetModuleHandleW(nullptr);
    }
    std::string name(moduleName);
    for (char& c : name) {
        if (c == '\\') c = '/';
    }
    return ModuleNamed(name);
}

DWORD GetModuleFileNameW(HMODULE module, LPWSTR fileName, DWORD size)
{
    Image image = ImageOf(module);
    if (!image) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return 0;
    }
    // Fully qualified, as Windows returns it: dyld has the main executable
    // under the path it was started with, which may be relative.
    std::string path = image.Path;
    if (path.empty() || path[0] != '/') {
        char resolved[PATH_MAX];
        if (realpath(image.Path, resolved)) path = resolved;
    }
    int length = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
    std::wstring wide((size_t)length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), wide.data(), length);
    if (size == 0) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (wide.size() < size) {
        wmemcpy(fileName, wide.c_str(), wide.size() + 1);
        return (DWORD)wide.size();
    }
    // Too small: what fits, terminated, and the size of the buffer as the result.
    wmemcpy(fileName, wide.data(), size - 1);
    fileName[size - 1] = L'\0';
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return size;
}

HMODULE LoadLibraryW(LPCWSTR fileName)
{
    if (!fileName || !*fileName) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    std::string path = PathFromWide(fileName);

    // A bare name that a loaded module already has is that module, as on
    // Windows. The main executable is never unloaded and takes no reference.
    Image loaded = path.find('/') == std::string::npos ? ImageNamed(path) : Image{};
    if (loaded && loaded.IsExecutable()) {
        return HandleOf(loaded);
    }

    // The reference this call takes; FreeLibrary gives it back.
    void* handle = dlopen(loaded ? loaded.Path : path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
    if (!handle) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    Image image = ImageOfHandle(handle);
    if (!image) {
        dlclose(handle);
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    return HandleOf(image);
}

BOOL FreeLibrary(HMODULE module)
{
    Image image = module ? ImageOf(module) : Image{};
    if (!image) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return FALSE;
    }
    if (image.IsExecutable()) {
        return TRUE;        // never unloaded, on Windows either
    }
    void* handle = OpenLoaded(image);
    if (!handle) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return FALSE;
    }
    // One close for the reference OpenLoaded took, one for the reference being
    // freed. An image the process was started with has no count and stays.
    dlclose(handle);
    if (dlclose(handle) != 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return TRUE;
}

FARPROC GetProcAddress(HMODULE module, LPCSTR procName)
{
    Image image = ImageOf(module);
    if (!image) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    // A value below 0x10000 is an export ordinal on Windows. Mach-O images have none.
    if ((uintptr_t)procName < 0x10000) {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }
    void* address = nullptr;
    if (image.IsExecutable()) {
        address = dlsym(RTLD_MAIN_ONLY, procName);
    } else if (void* handle = OpenLoaded(image)) {
        address = dlsym(handle, procName);
        dlclose(handle);
    }
    if (!address) {
        SetLastError(ERROR_PROC_NOT_FOUND);
    }
    return reinterpret_cast<FARPROC>(address);
}

BOOL GetModuleInformation(HANDLE process, HMODULE module, LPMODULEINFO info, DWORD size)
{
    if (process != GetCurrentProcess()) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!info || size < sizeof(MODULEINFO)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    Image image = ImageOf(module);
    if (!image) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    // A library in the dyld shared cache has its segments in different parts
    // of the cache, among other libraries': no one range is that image.
    if (image.Header->flags & MH_DYLIB_IN_CACHE) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    // From the header to the end of the last segment. The segments keep the
    // distances they were linked with; __PAGEZERO lies below the header.
    uint64_t textVM = 0, endVM = 0, entryOffset = 0;
    bool haveText = false, haveEntry = false;
    auto command = reinterpret_cast<load_command const*>(image.Header + 1);
    for (uint32_t i = 0; i < image.Header->ncmds; i++) {
        if (command->cmd == LC_SEGMENT_64) {
            auto segment = reinterpret_cast<segment_command_64 const*>(command);
            if (strncmp(segment->segname, SEG_TEXT, sizeof(segment->segname)) == 0) {
                textVM = segment->vmaddr;
                haveText = true;
            }
            if (segment->vmsize != 0 && strncmp(segment->segname, SEG_PAGEZERO, sizeof(segment->segname)) != 0) {
                endVM = std::max(endVM, segment->vmaddr + segment->vmsize);
            }
        } else if (command->cmd == LC_MAIN) {
            entryOffset = reinterpret_cast<entry_point_command const*>(command)->entryoff;
            haveEntry = true;
        }
        command = reinterpret_cast<load_command const*>(reinterpret_cast<uint8_t const*>(command) + command->cmdsize);
    }
    if (!haveText || endVM <= textVM || endVM - textVM > UINT32_MAX) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    info->lpBaseOfDll = HandleOf(image);
    info->SizeOfImage = (DWORD)(endVM - textVM);
    // A dylib has no entry point (Windows gives NULL for a DLL without one).
    info->EntryPoint = haveEntry
        ? const_cast<uint8_t*>(reinterpret_cast<uint8_t const*>(image.Header)) + entryOffset
        : nullptr;
    return TRUE;
}

// A Mach-O image has no entry point that the system calls when a thread
// starts or ends, so what this asks for holds for every loaded image.
BOOL DisableThreadLibraryCalls(HMODULE module)
{
    if (!module) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!bg3se::shim::ImageOf(module)) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

bool bg3se::shim::CodeSection(HMODULE module, uint8_t const*& start, size_t& size)
{
    Image image = ImageOf(module);
    if (!image) return false;
    bool found = false;
    ForEachSegment(image, [&](segment_command_64 const& segment) {
        if (found || strncmp(segment.segname, "__TEXT", sizeof(segment.segname)) != 0) return;
        auto section = reinterpret_cast<section_64 const*>(&segment + 1);
        for (uint32_t i = 0; i < segment.nsects; i++, section++) {
            if (strncmp(section->sectname, "__text", sizeof(section->sectname)) != 0) continue;
            // The header is at the start of __TEXT: the section lies as far
            // from it in memory as in the file's addresses.
            start = reinterpret_cast<uint8_t const*>(image.Header) + (section->addr - segment.vmaddr);
            size = section->size;
            found = true;
            return;
        }
    });
    return found;
}
