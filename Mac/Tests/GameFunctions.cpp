// bg3mse-game-functions <Game.app>
//
// Checks the function extent lookup (Platform/FunctionExtents.h) on the game
// binary and on libOsiris.dylib, both read from disk (the game is not
// started), against their symbol tables:
//   - every address in __TEXT,__text that a symbol names must be a function
//     start in the image's function starts table, except a static initialiser
//     the compiler generated (___cxx_global_var_init*, __GLOBAL__sub_I_*,
//     ___cxx_global_array_dtor), which the game's table leaves out;
//   - a function's end must be the next named address, an earlier start the
//     symbol table has no name for (a function without a symbol), or a later
//     start when every named address passed over is such a static initialiser;
//   - the exports of libOsiris.dylib that BG3Extender/GameHooks/OsirisWrappers.cpp
//     wraps must each be found, and the entry hook planner must accept each
//     with its extent and an island in the image's header padding, which is
//     what DetourAttach on it needs (the refusals are printed with the reason).
// Exits non-zero when any of this does not hold.

#include <Platform/EntryHook.h>
#include <Platform/FunctionExtents.h>
#include <Platform/HookPlanner.h>
#include <Platform/Log.h>
#include <Platform/Symbols.h>

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace bg3se::mac;

namespace {

// The exports OsirisWrappers::Initialize wraps, by their names in libOsiris.dylib.
char const* const kOsirisWrapped[] = {
    "__ZN7COsiris20RegisterDIVFunctionsEP19TOsirisInitFunction",
    "__ZN7COsiris8InitGameEv",
    "__ZN7COsiris13DeleteAllDataEb",
    "__ZN7COsiris19GetFunctionMappingsEPP11MappingInfoPj",
    "__ZN7COsiris11OpenLogFileEPKwj",
    "__ZN7COsiris12CloseLogFileEv",
    "__ZN7COsiris4LoadER12COsiSmartBuf",
    "__ZN7COsiris7CompileEPKwS1_",
    "__ZN7COsiris5MergeEPKw",
    "__ZN7COsiris5EventEjP16COsiArgumentDesc",
};

// A binary mapped read-only for the rest of the process.
std::span<uint8_t const> MapFile(std::string const& path)
{
    int fd = open(path.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0) close(fd);
        fprintf(stderr, "cannot open %s\n", path.c_str());
        return {};
    }
    void* data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        fprintf(stderr, "cannot map %s\n", path.c_str());
        return {};
    }
    return { static_cast<uint8_t const*>(data), (size_t)st.st_size };
}

struct Binary
{
    std::string Name;
    GameImage Symbols;
    FunctionTable Functions;
};

bool Open(std::string const& path, std::string name, Binary& binary)
{
    binary.Name = std::move(name);
    auto file = MapFile(path);
    if (file.empty()) return false;
    if (!binary.Symbols.OpenFile(file)) {
        fprintf(stderr, "%s: no readable arm64 symbol table\n", path.c_str());
        return false;
    }
    std::string problem;
    if (!binary.Functions.OpenFile(file, problem)) {
        fprintf(stderr, "%s: %s\n", path.c_str(), problem.c_str());
        return false;
    }
    printf("%s: %zu function starts from %s\n", binary.Name.c_str(), binary.Functions.Count(), Describe(binary.Functions.Source()));
    return true;
}

// A function the compiler generates to run static initialisers.
bool IsStaticInitialiser(std::string_view name)
{
    return name.starts_with("___cxx_global_var_init") || name.starts_with("__GLOBAL__sub_I_") || name.starts_with("___cxx_global_array_dtor");
}

// Every named address in __text against the function starts table.
bool CheckNamed(Binary const& b)
{
    auto named = b.Symbols.TextSymbols();
    std::vector<uint64_t> vas(named.size()), ends(named.size());
    for (size_t i = 0; i < named.size(); i++) vas[i] = named[i].VA;
    b.Symbols.NextSymbolVAs(vas, ends);

    size_t same = 0, unnamedAfter = 0, overInitialisers = 0, initialisers = 0, notStart = 0, later = 0;
    auto print = [](char const* what, uint64_t va, std::string_view name) {
        printf("  %s: %#llx %.*s\n", what, (unsigned long long)va, (int)name.size(), name.data());
    };
    for (size_t i = 0; i < named.size(); i++) {
        uint64_t symbolEnd = ends[i] && ends[i] <= b.Symbols.TextEnd() ? ends[i] : b.Symbols.TextEnd();
        auto extent = b.Functions.At(vas[i]);
        auto name = named[i].Name;
        if (!extent.Ok()) {
            if (IsStaticInitialiser(name) && extent.Refusal == ExtentRefusal::NotAStart) initialisers++;
            else if (notStart++ < 5) print(Describe(extent.Refusal), vas[i], name);
        } else if (extent.End == symbolEnd) {
            same++;
        } else if (extent.End < symbolEnd) {
            unnamedAfter++;     // what lies between has no name
        } else {
            // Passes over named addresses: each must be a static initialiser the table leaves out.
            bool explained = true;
            for (size_t j = i + 1; j < named.size() && named[j].VA < extent.End; j++) {
                explained = explained && IsStaticInitialiser(named[j].Name) && !b.Functions.At(named[j].VA).Ok();
            }
            if (explained) overInitialisers++;
            else if (later++ < 5) print("ends past a named function", vas[i], name);
        }
    }
    printf("%s: %zu named functions in __text:\n"
           "  %zu end at the next symbol\n"
           "  %zu end at a function start without a name, before the next symbol\n"
           "  %zu end past static initialisers the function starts table leaves out\n"
           "  %zu are static initialisers the table leaves out\n"
           "  %zu end past another named function, %zu other named addresses are not function starts\n",
        b.Name.c_str(), named.size(), same, unnamedAfter, overInitialisers, initialisers, later, notStart);
    return notStart == 0 && later == 0 && !named.empty();
}

bool CheckOsirisExports(Binary const& osiris)
{
    bool ok = true;
    printf("%s: the exports OsirisWrappers wraps:\n", osiris.Name.c_str());
    for (char const* name : kOsirisWrapped) {
        uint64_t va = osiris.Symbols.FindVA(name);
        if (!va) {
            printf("  %-60s NOT FOUND\n", name);
            ok = false;
            continue;
        }
        uint64_t next = osiris.Symbols.NextSymbolVA(va);
        uint64_t symbolEnd = next && next <= osiris.Symbols.TextEnd() ? next : osiris.Symbols.TextEnd();
        auto extent = osiris.Functions.At(va);
        if (!extent.Ok()) {
            printf("  %-60s %#llx: %s\n", name, (unsigned long long)va, Describe(extent.Refusal));
            ok = false;
            continue;
        }
        bool agrees = extent.End <= symbolEnd;
        auto refusal = CheckEntryHook(osiris.Symbols, va, extent.End);
        printf("  %-60s %#llx-%#llx (%llu bytes; next symbol %#llx)%s: %s\n", name, (unsigned long long)va, (unsigned long long)extent.End,
            (unsigned long long)extent.Bytes(), (unsigned long long)symbolEnd, agrees ? "" : " ENDS AFTER THE NEXT SYMBOL",
            refusal == HookRefusal::None ? "an entry hook would go in" : Describe(refusal));
        ok = ok && agrees && refusal == HookRefusal::None;
    }
    return ok;
}

}

int main(int argc, char** argv)
{
    SetLogStream(stdout);
    if (argc != 2) {
        fprintf(stderr, "usage: bg3mse-game-functions <Game.app>\n");
        return 2;
    }
    std::string app = argv[1];
    Binary game, osiris;
    if (!Open(app + "/Contents/MacOS/Baldur's Gate 3", "game", game)) return 1;
    if (!Open(app + "/Contents/Frameworks/libOsiris.dylib", "libOsiris.dylib", osiris)) return 1;
    bool ok = game.Functions.Source() == FunctionSource::FunctionStarts && osiris.Functions.Source() == FunctionSource::FunctionStarts;
    ok = CheckNamed(game) && ok;
    ok = CheckNamed(osiris) && ok;
    ok = CheckOsirisExports(osiris) && ok;
    printf("%s\n", ok ? "function extents agree with the symbol tables, and every wrapped Osiris export would take an entry hook"
                       : "FUNCTION EXTENTS DO NOT AGREE, OR A WRAPPED OSIRIS EXPORT WOULD NOT TAKE AN ENTRY HOOK");
    return ok ? 0 : 1;
}
