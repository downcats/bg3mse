// bg3mse-engine: the engine table (Engine/EngineTable.h) against a game.
//
//   bg3mse-engine check <Game.app>              every entry: how it is found, each check and its outcome,
//                                               and the totals by kind and by status
//   bg3mse-engine types <Game.app> [context]    the type index contexts of the game and how many types
//                                               each has; with a context, its types
//
// Both read the game executable and libOsiris.dylib from disk; the game is not
// started. `check` exits non-zero when an entry is not in the state the table
// expects of it (resolved, resolved with a caveat, without callable code, or
// not settled).
//
// A tool of its own rather than a subcommand of bg3mse-shaders: it needs the
// platform layer and the engine table only, so it builds without Norbyte's
// headers and says the same whatever state the rest of the build is in.

#include <Engine/EngineResolver.h>
#include <Engine/EngineTable.h>
#include <Platform/FunctionExtents.h>
#include <Platform/Log.h>
#include <Platform/Symbols.h>

#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace bg3se::mac;
using namespace bg3se::mac::engine;

namespace {

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
    GameImage Symbols;
    FunctionTable Functions;

    bool Open(std::string const& path)
    {
        auto file = MapFile(path);
        if (file.empty()) return false;
        if (!Symbols.OpenFile(file)) {
            fprintf(stderr, "%s: no readable arm64 symbol table\n", path.c_str());
            return false;
        }
        std::string problem;
        if (!Functions.OpenFile(file, problem)) {
            fprintf(stderr, "%s: %s\n", path.c_str(), problem.c_str());
            return false;
        }
        return true;
    }
    ImageView View() const { return { &Symbols, &Functions }; }
};

struct Game
{
    Binary Executable, Osiris;

    bool Open(std::string const& app)
    {
        return Executable.Open(app + "/Contents/MacOS/Baldur's Gate 3") && Osiris.Open(app + "/Contents/Frameworks/libOsiris.dylib");
    }
    Images View() const { return { Executable.View(), Osiris.View() }; }
};

void PrintSignature(Signature const& call)
{
    for (auto const& arg : call.Arguments()) {
        char place[32];
        if (arg.At == Place::Stack) snprintf(place, sizeof(place), "[sp, #0x%x]", arg.StackOffset);
        else snprintf(place, sizeof(place), "%s", Describe(arg.At));
        char norbyte[40] = "no parameter of Norbyte's";
        if (arg.Norbyte >= 0) snprintf(norbyte, sizeof(norbyte), "Norbyte's parameter %d", arg.Norbyte + 1);
        char const* how = arg.How == Passing::Pointer ? "pointer to" : arg.How == Passing::Unread ? "passed and not read:" : "value,";
        if (arg.How == Passing::Value) printf("      %-12s %s %u byte(s): %s (%s)\n", place, how, arg.Bytes, arg.Type, norbyte);
        else printf("      %-12s %s %s (%s)\n", place, how, arg.Type, norbyte);
    }
    printf("      result       %s%s%s\n", Describe(call.Result), call.ResultType ? ": " : "", call.ResultType ? call.ResultType : "");
    if (call.Differs) printf("      differs from Norbyte's prototype: %s\n", call.Differs);
}

void PrintFound(Resolution const& resolution, Resolved const& r)
{
    auto const& entry = *r.Of;
    if (entry.Derived) {
        auto const& d = *entry.Derived;
        printf("    found from the code of %s%s", d.Host.Symbol, d.Host.How == Find::NamePrefix ? "..." : "");
        if (d.Anchor) printf(", at its one call to %s%s", d.Anchor.Import ? d.Anchor.Import : d.Anchor.Function.Symbol,
            !d.Anchor.Import && d.Anchor.Function.How == Find::NamePrefix ? "..." : "");
        if (!d.Pattern.empty()) printf(", by a sequence of %zu instructions", d.Pattern.size());
        printf("\n");
    } else if (entry.Where) {
        printf("    found by %s %s%s\n", entry.Where.How == Find::NamePrefix ? "the name starting" : "the name", entry.Where.Symbol,
            entry.Where.How == Find::NamePrefix ? "..." : "");
    } else if (entry.Context) {
        printf("    found by the names ls::TypeId<T, %s>::m_TypeIndex\n", entry.Context);
    }
    if (r.SeenVA) {
        printf("    at %#llx", (unsigned long long)r.SeenVA);
        if (r.End > r.SeenVA && entry.What != Kind::CallSite && entry.What != Kind::CodeSite) printf(" (%llu bytes)", (unsigned long long)(r.End - r.SeenVA));
        printf(" in %s\n", Describe(entry.In));
    }
    (void)resolution;
}

void PrintEntry(Resolution const& resolution, Resolved const& r)
{
    auto const& entry = *r.Of;
    printf("%s  [%s; %s]  %s%s\n", entry.Name, r.Area, Describe(entry.What), Describe(r.Is), r.AsExpected() ? "" : "  ** NOT AS THE TABLE EXPECTS **");
    if (entry.Mac) printf("    on macOS: %s\n", entry.Mac);
    if (entry.Use) printf("    Norbyte's use: %s\n", entry.Use);
    PrintFound(resolution, r);
    bool function = entry.What == Kind::FunctionToCall || entry.What == Kind::FunctionToHook || entry.What == Kind::FunctionToCallAndHook;
    if (function) {
        printf("    argument passing:\n");
        PrintSignature(entry.Call);
        if (entry.CallsItself) printf("    calls itself: a hook on it also runs for the inner call\n");
    }
    if (entry.What == Kind::NoCallableCode) {
        if (entry.Gone.Fate) printf("    where the code is: %s\n", entry.Gone.Fate);
        for (auto replacement : entry.Gone.Replacements) printf("    its place is taken by the entry: %s\n", replacement);
    }
    for (auto const& check : r.Checks) {
        printf("    %s %-14s %s\n", check.Passed ? "ok  " : "FAIL", check.What, check.Detail.c_str());
    }
    if (entry.Caveat) printf("    caveat: %s\n", entry.Caveat);
    if (r.Is == Status::Unresolved) printf("    unresolved: %s\n", r.Reason.c_str());
    printf("\n");
}

int Check(std::string const& app)
{
    Game game;
    if (!game.Open(app)) return 1;
    auto resolution = Resolve(EngineTable(), game.View());

    for (auto const& r : resolution.Entries) PrintEntry(resolution, r);

    size_t counts[(size_t)Kind::Count][(size_t)Status::Count]{};
    size_t unexpected = 0;
    for (auto const& r : resolution.Entries) {
        counts[(size_t)r.Of->What][(size_t)r.Is]++;
        unexpected += !r.AsExpected();
    }
    printf("%-28s %9s %9s %9s %11s %7s\n", "kind", "resolved", "caveat", "no code", "unresolved", "total");
    size_t totals[(size_t)Status::Count]{};
    for (size_t kind = 0; kind < (size_t)Kind::Count; kind++) {
        size_t row = 0;
        for (size_t status = 0; status < (size_t)Status::Count; status++) {
            row += counts[kind][status];
            totals[status] += counts[kind][status];
        }
        if (!row) continue;
        printf("%-28s %9zu %9zu %9zu %11zu %7zu\n", Describe((Kind)kind), counts[kind][(size_t)Status::Resolved],
            counts[kind][(size_t)Status::ResolvedWithCaveat], counts[kind][(size_t)Status::NoCallableCode],
            counts[kind][(size_t)Status::Unresolved], row);
    }
    printf("%-28s %9zu %9zu %9zu %11zu %7zu\n", "all", totals[(size_t)Status::Resolved], totals[(size_t)Status::ResolvedWithCaveat],
        totals[(size_t)Status::NoCallableCode], totals[(size_t)Status::Unresolved], resolution.Entries.size());
    printf("type indices in the game: %zu, in %zu contexts\n", resolution.Types.All().size(), resolution.Types.Contexts().size());

    if (unexpected) {
        printf("%zu ENTRIES ARE NOT AS THE TABLE EXPECTS:\n", unexpected);
        for (auto const& r : resolution.Entries) {
            if (!r.AsExpected()) printf("  %s: %s, expected %s%s%s\n", r.Of->Name, Describe(r.Is), Describe(r.Expected()),
                r.Reason.empty() ? "" : ": ", r.Reason.c_str());
        }
        return 1;
    }
    printf("every entry is as the table expects\n");
    return 0;
}

int Types(std::string const& app, std::string const& context)
{
    Game game;
    if (!game.Open(app)) return 1;
    auto types = TypeIndices::Collect(game.Executable.Symbols);
    if (context.empty()) {
        for (auto name : types.Contexts()) printf("%7zu  %.*s\n", types.CountIn(name), (int)name.size(), name.data());
        printf("%7zu  in all\n", types.All().size());
        return types.All().empty() ? 1 : 0;
    }
    size_t count = 0;
    for (auto const& index : types.All()) {
        if (index.Context != context) continue;
        printf("%#llx  %s\n", (unsigned long long)index.VA, index.Type.c_str());
        count++;
    }
    printf("%zu types in %s\n", count, context.c_str());
    return count ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    SetLogStream(stdout);
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 2 && args[0] == "check") return Check(args[1]);
    if ((args.size() == 2 || args.size() == 3) && args[0] == "types") return Types(args[1], args.size() == 3 ? args[2] : std::string());
    fprintf(stderr, "usage: bg3mse-engine check <Game.app>\n"
                    "       bg3mse-engine types <Game.app> [context]\n");
    return 2;
}
