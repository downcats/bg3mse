// Resources.
//
// Windows keeps a module's resources in its resource section, each under a
// type and a name. A Mach-O image keeps each resource in a section of its own:
// the segment is named for the type and the section for the name
// (ld -sectcreate <type> <name> <file>). An integer type or name is written
// in decimal; a string is in upper case, as the resource compiler stores it.
// Segment and section names hold 16 bytes, so longer ones cannot exist.
#include "Win32Images.h"

#include <cctype>

using bg3se::shim::ForEachSegment;
using bg3se::shim::Image;
using bg3se::shim::ImageOf;

namespace {

// The Mach-O spelling of a resource type or name; false when it has none.
bool ResourceName(LPCWSTR id, char name[17])
{
    if (((uintptr_t)id >> 16) == 0) {       // MAKEINTRESOURCE
        snprintf(name, 17, "%u", (unsigned)(uintptr_t)id);
        return true;
    }
    if (id[0] == L'#') {                    // "#123" is the integer 123
        wchar_t* end = nullptr;
        unsigned long value = wcstoul(id + 1, &end, 10);
        if (end == id + 1 || *end != L'\0' || value > 0xffff) return false;
        snprintf(name, 17, "%lu", value);
        return true;
    }
    size_t length = wcslen(id);
    if (length == 0 || length > 16) return false;
    for (size_t i = 0; i < length; i++) {
        if (id[i] <= 0x20 || id[i] >= 0x7f) return false;
        name[i] = (char)toupper((int)id[i]);
    }
    name[length] = '\0';
    return true;
}

bool NameIs(char const fixed[16], char const* name)
{
    return strncmp(fixed, name, 16) == 0;
}

// The section an HRSRC names, if it is one of this image's.
section_64 const* SectionOf(Image const& image, HRSRC resource)
{
    section_64 const* found = nullptr;
    ForEachSegment(image, [&](segment_command_64 const& segment) {
        auto sections = reinterpret_cast<section_64 const*>(&segment + 1);
        for (uint32_t i = 0; i < segment.nsects; i++) {
            if (&sections[i] == resource) found = &sections[i];
        }
    });
    return found;
}

}

HRSRC FindResourceW(HMODULE module, LPCWSTR name, LPCWSTR type)
{
    Image image = ImageOf(module);
    if (!image) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    char segmentName[17], sectionName[17];
    if (!type || !ResourceName(type, segmentName)) {
        SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
        return nullptr;
    }
    if (!name || !ResourceName(name, sectionName)) {
        SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
        return nullptr;
    }
    bool haveType = false;
    section_64 const* found = nullptr;
    ForEachSegment(image, [&](segment_command_64 const& segment) {
        if (!NameIs(segment.segname, segmentName)) return;
        haveType = true;
        auto sections = reinterpret_cast<section_64 const*>(&segment + 1);
        for (uint32_t i = 0; i < segment.nsects && !found; i++) {
            if (NameIs(sections[i].sectname, sectionName)) found = &sections[i];
        }
    });
    if (!found) {
        SetLastError(haveType ? ERROR_RESOURCE_NAME_NOT_FOUND : ERROR_RESOURCE_TYPE_NOT_FOUND);
    }
    return const_cast<section_64*>(found);
}

DWORD SizeofResource(HMODULE module, HRSRC resource)
{
    Image image = ImageOf(module);
    section_64 const* section = image ? SectionOf(image, resource) : nullptr;
    if (!section) {
        SetLastError(ERROR_INVALID_HANDLE);
        return 0;
    }
    return (DWORD)section->size;
}

HGLOBAL LoadResource(HMODULE module, HRSRC resource)
{
    Image image = ImageOf(module);
    section_64 const* section = image ? SectionOf(image, resource) : nullptr;
    if (!section) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    // The image is loaded as far from its link-time addresses as its header is.
    uint64_t textVM = 0;
    ForEachSegment(image, [&](segment_command_64 const& segment) {
        if (NameIs(segment.segname, SEG_TEXT)) textVM = segment.vmaddr;
    });
    return const_cast<uint8_t*>(reinterpret_cast<uint8_t const*>(image.Header)) + (section->addr - textVM);
}

LPVOID LockResource(HGLOBAL data)
{
    // The data is in the loaded image; the handle is its address, as in Win32.
    return data;
}

BOOL FreeResource(HGLOBAL)
{
    // As in Win32, where the function is kept for 16-bit programs: there is
    // nothing to free, and the result is zero, which is its success value.
    return FALSE;
}
