#include <Platform/Process.h>

#include <climits>
#include <crt_externs.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <strings.h>

namespace bg3se::mac {

bool IsCrashReporterCommandLine(int argc, char const* const* argv)
{
    if (!argv) return false;
    for (int i = 0; i < argc; i++) {
        if (argv[i] && strcasecmp(argv[i], "-crash") == 0) return true;
    }
    return false;
}

bool IsGameProcess()
{
    char exe[4096];
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0) return false;
    std::string_view path = exe;
    if (!path.ends_with("/Baldur's Gate 3")) return false;

    // The arguments where the game's own test reads them. The C library has
    // them from dyld before any image's initializer runs, so they are there
    // when a dylib's constructor asks. A process whose arguments cannot be
    // read is not taken for the game.
    int const* argc = _NSGetArgc();
    char** const* argv = _NSGetArgv();
    if (!argc || !argv || !*argv) return false;
    return !IsCrashReporterCommandLine(*argc, *argv);
}

namespace {

// The install name an image gives itself (LC_ID_DYLIB), or nullptr.
char const* InstallName(mach_header const* header)
{
    if (!header || header->magic != MH_MAGIC_64) return nullptr;
    auto command = reinterpret_cast<load_command const*>(reinterpret_cast<mach_header_64 const*>(header) + 1);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (command->cmd == LC_ID_DYLIB) {
            auto id = reinterpret_cast<dylib_command const*>(command);
            return reinterpret_cast<char const*>(command) + id->dylib.name.offset;
        }
        command = reinterpret_cast<load_command const*>(reinterpret_cast<char const*>(command) + command->cmdsize);
    }
    return nullptr;
}

mach_header const* HeaderOf(void const* address)
{
    Dl_info info{};
    if (!dladdr(address, &info)) return nullptr;
    return static_cast<mach_header const*>(info.dli_fbase);
}

std::string RealPath(char const* path)
{
    char resolved[PATH_MAX];
    return path && realpath(path, resolved) ? std::string(resolved) : std::string();
}

}

bool InsertedByEnvironment(char const* image)
{
    char const* variable = getenv("DYLD_INSERT_LIBRARIES");
    std::string target = RealPath(image);
    if (!variable || target.empty()) return false;
    std::string_view rest = variable;
    while (!rest.empty()) {
        size_t colon = rest.find(':');
        std::string entry(rest.substr(0, colon));
        if (!entry.empty() && RealPath(entry.c_str()) == target) return true;
        if (colon == std::string_view::npos) break;
        rest.remove_prefix(colon + 1);
    }
    return false;
}

unsigned InitializeLibrariesLoadedBefore(void const* self)
{
    auto own = HeaderOf(self);
    // What is loaded now: the libraries dyld loaded with the game. A library
    // already initialized is only counted again; dlopen never closes it.
    std::vector<std::string> paths;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        auto header = _dyld_get_image_header(i);
        if (header == own || header->filetype != MH_DYLIB) continue;
        if (auto name = _dyld_get_image_name(i)) paths.emplace_back(name);
    }
    unsigned asked = 0;
    for (auto const& path : paths) {
        if (dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL)) asked++;
    }
    return asked;
}

namespace {

// The directory of a path, without its last slash.
std::string DirectoryOf(std::string const& path)
{
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// What a dyld path of the main executable's names, with @executable_path,
// @loader_path (both the executable's directory here) and each of its
// LC_RPATH entries for @rpath. Paths that do not exist are left out.
std::vector<std::string> Resolve(std::string_view name, std::string const& exeDir, std::vector<std::string> const& rpaths)
{
    std::vector<std::string> found;
    auto add = [&](std::string const& path) {
        if (auto real = RealPath(path.c_str()); !real.empty()) found.push_back(std::move(real));
    };
    auto replace = [&](std::string_view prefix, std::string const& with) {
        if (!name.starts_with(prefix)) return false;
        add(with + std::string(name.substr(prefix.size())));
        return true;
    };
    if (replace("@executable_path", exeDir) || replace("@loader_path", exeDir)) return found;
    if (name.starts_with("@rpath")) {
        for (auto const& rpath : rpaths) {
            std::string base = rpath;
            if (base.starts_with("@executable_path") || base.starts_with("@loader_path")) {
                base = exeDir + base.substr(base.find('/') == std::string::npos ? base.size() : base.find('/'));
            }
            add(base + std::string(name.substr(6)));
        }
        return found;
    }
    add(std::string(name));
    return found;
}

// Whether the main executable names `target` (a real path) in a load command.
bool NamedByTheExecutable(std::string const& target)
{
    mach_header const* exe = nullptr;
    std::string exePath;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        auto header = _dyld_get_image_header(i);
        if (header && header->filetype == MH_EXECUTE) {
            exe = header;
            if (auto name = _dyld_get_image_name(i)) exePath = RealPath(name);
            break;
        }
    }
    if (!exe || exe->magic != MH_MAGIC_64 || exePath.empty()) return false;
    std::string exeDir = DirectoryOf(exePath);

    std::vector<std::string> rpaths;
    std::vector<std::string_view> libraries;
    auto command = reinterpret_cast<load_command const*>(reinterpret_cast<mach_header_64 const*>(exe) + 1);
    for (uint32_t i = 0; i < exe->ncmds; i++) {
        auto text = [&](uint32_t offset) { return reinterpret_cast<char const*>(command) + offset; };
        switch (command->cmd) {
        case LC_RPATH:
            rpaths.emplace_back(text(reinterpret_cast<rpath_command const*>(command)->path.offset));
            break;
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB:
        case LC_LAZY_LOAD_DYLIB:
        case LC_LOAD_UPWARD_DYLIB:
            libraries.emplace_back(text(reinterpret_cast<dylib_command const*>(command)->dylib.name.offset));
            break;
        default:
            break;
        }
        command = reinterpret_cast<load_command const*>(reinterpret_cast<char const*>(command) + command->cmdsize);
    }
    for (auto name : libraries) {
        for (auto const& path : Resolve(name, exeDir, rpaths)) {
            if (path == target) return true;
        }
    }
    return false;
}

}

bool LoadedThroughUpdater(void const* self)
{
    Dl_info info{};
    if (!dladdr(self, &info) || !info.dli_fname) return false;
    std::string own = RealPath(info.dli_fname);
    if (own.empty()) return false;

    bool updater = false;
    for (uint32_t i = 0; i < _dyld_image_count() && !updater; i++) {
        char const* name = _dyld_get_image_name(i);
        std::string_view path = name ? name : "";
        size_t slash = path.rfind('/');
        updater = path.substr(slash == std::string_view::npos ? 0 : slash + 1) == kUpdaterFileName;
    }
    return updater && !InsertedByEnvironment(own.c_str()) && !NamedByTheExecutable(own);
}

void* NativeModuleHandle(void const* self)
{
    Dl_info info{};
    if (!dladdr(self, &info) || !info.dli_fname || !info.dli_fbase) return nullptr;
    // One reference for each image asked about, however often: the handle is
    // remembered with the image it is of.
    static std::mutex lock;
    static std::vector<std::pair<void const*, void*>> handles;
    std::lock_guard guard(lock);
    for (auto const& known : handles) {
        if (known.first == info.dli_fbase) return known.second;
    }
    void* handle = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) return nullptr;
    handles.emplace_back(info.dli_fbase, handle);
    return handle;
}

bool FirstCopyOfThisLibrary(void const* self)
{
    auto own = HeaderOf(self);
    char const* name = InstallName(own);
    if (!name) return true;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        auto header = _dyld_get_image_header(i);
        if (header == own) return true;
        if (auto other = InstallName(header); other && strcmp(other, name) == 0) return false;
    }
    return true;
}

}
