#include <Binding/CodePatches.h>
#include <Binding/HookBridges.h>
#include <Binding/Layouts.h>
#include <Binding/OsirisBinding.h>
#include <Binding/StaticSymbolBinding.h>
#include <Binding/StatusVfx.h>
#include <Binding/TypeIndexBinding.h>
#include <Engine/EngineResolver.h>
#include <Engine/EngineTable.h>
#include <Platform/FunctionExtents.h>
#include <Platform/Symbols.h>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <set>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// bg3mse-binding check <Game.app>: what StaticSymbols would hold on this game,
// read from its binaries on disk, which of this layer's hooks would go on,
// and what the story layer (OsirisWrappers) would be given: its globals, the
// functions it wraps, the node classes' vtables, and whether the story
// library's structures are proven. Exits non-zero when the rows do not hold
// together, when a member that has a source in the game would stay null,
// when a hook would be refused, or when the story layer would stay off or
// without one of its parts.
//
// bg3mse-binding types <Game.app>: what the type index maps of StaticSymbols
// would hold: for the components, systems and static data types Norbyte's
// definitions look up by name, how many reach one index variable of the game
// (mapped), none (unmapped) or more than one (ambiguous), with a line for
// each that is not mapped. Exits non-zero when a context is refused, when a
// name is not mapped, or when a variable's type has a name the game's own
// code does not generate (the check in the running game would then refuse it).

using namespace bg3se::mac;
using namespace bg3se::mac::binding;

namespace {

struct Binary
{
    GameImage Symbols;
    FunctionTable Functions;
    std::span<uint8_t const> File;

    bool Open(std::string const& path)
    {
        int fd = open(path.c_str(), O_RDONLY);
        struct stat st{};
        if (fd < 0 || fstat(fd, &st) != 0) {
            if (fd >= 0) close(fd);
            fprintf(stderr, "cannot open %s\n", path.c_str());
            return false;
        }
        void* data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (data == MAP_FAILED) {
            fprintf(stderr, "cannot map %s\n", path.c_str());
            return false;
        }
        std::span<uint8_t const> file{ static_cast<uint8_t const*>(data), (size_t)st.st_size };
        File = file;
        std::string problem;
        if (!Symbols.OpenFile(file)) {
            fprintf(stderr, "%s: no readable arm64 symbol table\n", path.c_str());
            return false;
        }
        if (!Functions.OpenFile(file, problem)) {
            fprintf(stderr, "%s: %s\n", path.c_str(), problem.c_str());
            return false;
        }
        return true;
    }
    engine::ImageView View() const { return { &Symbols, &Functions }; }
};

// The story layer. Returns how many things are not as the rows expect.
int Story(engine::Resolution const& resolution)
{
    int bad = 0;
    printf("\nthe story library (OsirisWrappers):\n");
    if (auto findings = CheckOsirisRows(engine::EngineTable()); !findings.empty()) {
        printf("the story rows do not hold together:\n%s\n", findings.c_str());
        bad++;
    }

    auto plan = PlanOsiris(resolution, false);
    auto print = [&](char const* title, std::vector<MemberResult> const& members) {
        printf("  %s:\n", title);
        for (auto const& member : members) {
            auto const& row = *member.Row;
            char value[32] = "null";
            if (member.Value) snprintf(value, sizeof(value), "%#llx", (unsigned long long)reinterpret_cast<uintptr_t>(member.Value));
            if ((row.How == Bound::CallAdapter || row.How == Bound::HookBridge) && member.Value) snprintf(value, sizeof(value), "(in this image)");
            printf("    %-34s %-12s %-18s%s\n", row.Member, Describe(row.How), value, row.Critical ? " critical" : "");
            if (!member.Value) {
                printf("        ** NOT BOUND: %s\n        when null: %s\n", member.Reason.c_str(), row.WhenNull);
                bad++;
            }
        }
    };
    print("globals (OsirisStaticGlobals)", plan.Globals);
    print("functions to wrap", plan.Hooks);

    printf("  node classes (OsirisWrappers::VMTs), each with the slots NodeVMT has:\n");
    for (auto const& cls : plan.Classes) {
        if (cls.Vtable) {
            printf("    %-34s type %u      %#llx\n", cls.Row->Entry, cls.Row->Type, (unsigned long long)reinterpret_cast<uintptr_t>(cls.Vtable));
        } else {
            printf("    %-34s type %u      ** NOT HANDED OVER: %s\n", cls.Row->Entry, cls.Row->Type, cls.Reason.c_str());
            bad++;
        }
    }
    for (auto const& slot : OsirisNodeSlots()) {
        printf("      %-22s +%#x%s\n", slot.Name, slot.InNodeVMT, slot.QueryOnly ? "  (query classes)" : "");
    }

    size_t confirmed = 0;
    for (uint8_t n = 0; n < (uint8_t)osiris::Shape::Count; n++) confirmed += plan.Shapes.Confirmed((osiris::Shape)n);
    printf("  shapes of the library's structures: %zu of %u confirmed (%zu proofs)\n", confirmed, (unsigned)osiris::Shape::Count,
        plan.Shapes.Proofs.size());
    if (!plan.Shapes.AllConfirmed()) printf("        ** %s\n", plan.Shapes.FirstProblem().c_str());

    if (plan.StoryPathFromWriter) {
        printf("  the generated story's path is at +%#tx of its writer\n", plan.StoryPathFromWriter);
    } else {
        printf("  ** the generated story's path is not known from its writer\n");
        bad++;
    }

    if (plan.Available) {
        printf("  the story layer would be on, node hooks %s\n", plan.NodeHooks ? "available" : "OFF");
    } else {
        printf("  ** the story layer would stay OFF:\n%s", plan.Reason.c_str());
        bad++;
    }
    return bad;
}

int Check(std::string const& app)
{
    Binary game, osiris;
    if (!game.Open(app + "/Contents/MacOS/Baldur's Gate 3") || !osiris.Open(app + "/Contents/Frameworks/libOsiris.dylib")) return 2;

    int bad = 0;
    if (auto findings = CheckRows(engine::EngineTable()); !findings.empty()) {
        printf("the rows do not hold together:\n%s\n", findings.c_str());
        bad++;
    }

    auto resolution = engine::Resolve(engine::EngineTable(), { game.View(), osiris.View() });

    printf("hooks of this layer on the game:\n");
    for (auto const& hook : GameHooks()) {
        auto refusal = GameHookRefusal(hook, resolution);
        auto entry = resolution.Find(hook.Entry);
        printf("  %-52s %-9s %s%s\n", hook.Entry, hook.At == HookAt::Entry ? "entry" : "call site",
            refusal.empty() ? "would go on" : "REFUSED: ", refusal.c_str());
        if (refusal.empty() && entry) printf("  %-52s           at %#llx\n", "", (unsigned long long)entry->VA);
        if (!refusal.empty()) bad++;
    }

    auto report = PlanStaticSymbols(resolution, false);
    // As in the game: a member that leads to a structure not proven on this binary is taken back.
    auto layouts = LayoutsWithheldOn(resolution);
    for (auto const& why : layouts.UnconfirmedClasses) {
        printf("  %s\n", why.c_str());
        bad++;
    }
    for (auto const& held : layouts.Members) {
        for (auto& member : report.Members) {
            if (strcmp(member.Row->Member, held.Name) != 0 || !member.Value) continue;
            member.Value = nullptr;
            member.Reason = held.Why;
        }
    }
    size_t counts[(size_t)Bound::Count]{};
    printf("\nStaticSymbols:\n");
    for (auto const& member : report.Members) {
        auto const& row = *member.Row;
        counts[(size_t)row.How]++;
        bool served = member.Value != nullptr || (row.How == Bound::Handlers && member.Reason.empty());
        char value[32] = "null";
        if (member.Value) snprintf(value, sizeof(value), "%#llx", (unsigned long long)reinterpret_cast<uintptr_t>(member.Value));
        if (row.How == Bound::CallAdapter || row.How == Bound::HookBridge) snprintf(value, sizeof(value), "%s", member.Value ? "(in this image)" : "null");
        if (row.How == Bound::Handlers && served) snprintf(value, sizeof(value), "(handlers driven)");
        printf("  %-52s %-12s %-18s%s%s\n", row.Member, Describe(row.How), value, row.Critical ? " critical" : "",
            strcmp(row.Entry, row.Member) != 0 ? (std::string(" from ") + row.Entry).c_str() : "");
        if (!served) {
            if (row.How == Bound::Unavailable) {
                printf("      when null: %s\n", row.WhenNull);
            } else {
                printf("      ** NOT BOUND: %s\n      when null: %s\n", member.Reason.c_str(), row.WhenNull);
                bad++;
            }
        }
    }
    printf("\n%zu members: %zu globals, %zu functions, %zu call adapters, %zu hook bridges, %zu driven by handlers, %zu unavailable\n",
        report.Members.size(), counts[(size_t)Bound::Global], counts[(size_t)Bound::Function], counts[(size_t)Bound::CallAdapter],
        counts[(size_t)Bound::HookBridge], counts[(size_t)Bound::Handlers], counts[(size_t)Bound::Unavailable]);
    bad += Story(resolution);

    printf("\ncode patches (Norbyte's mapping, then the instructions that are its patch here):\n");
    for (auto const& mapping : CodePatchMappings()) {
        auto refusal = CodePatchRefusal(mapping.Mapping, resolution);
        printf("  %-52s %s%s\n", mapping.Mapping, refusal.empty() ? "would go in" : "REFUSED: ", refusal.c_str());
        for (auto name : mapping.Sites) {
            auto entry = resolution.Find(name);
            if (entry && entry->Usable()) {
                printf("      %#llx becomes %08x: %s\n", (unsigned long long)entry->VA, entry->Of->Becomes, name);
            } else {
                printf("      %s\n", name);
            }
        }
        if (!refusal.empty()) bad++;
    }

    printf("\nthe classes behind an ecl::StatusVFX pointer (Norbyte's MakePolymorphicRef):\n");
    for (size_t cls = 1; cls < static_cast<size_t>(StatusVfxClass::Count); cls++) {
        auto name = StatusVfxEntryName(static_cast<StatusVfxClass>(cls));
        auto entry = resolution.Find(name);
        printf("  %-52s vtable pointer %#llx\n", name, (unsigned long long)(entry && entry->Usable() ? entry->VA : 0));
    }
    if (auto refusal = StatusVfxRefusal(resolution); !refusal.empty()) {
        printf("  REFUSED: %s\n", refusal.c_str());
        bad++;
    } else {
        printf("  told apart by the vtable pointer; %s and %s stay the base class\n", kStatusVfxBaseEntryNames[0], kStatusVfxBaseEntryNames[1]);
    }

    printf("\n");
    printf("%s\n", bad ? "NOT every member, hook and patch is as the rows expect"
        : "every member with a source in the game would be bound, and every hook and patch would go on");
    return bad ? 1 : 0;
}

// The arm64 image of a file that is one, or holds one among several.
std::span<uint8_t const> ThinArm64(std::span<uint8_t const> file)
{
    if (file.size() < sizeof(fat_header)) return {};
    auto fat = reinterpret_cast<fat_header const*>(file.data());
    if (OSSwapBigToHostInt32(fat->magic) != FAT_MAGIC) return file;
    uint32_t count = OSSwapBigToHostInt32(fat->nfat_arch);
    if (count > (file.size() - sizeof(fat_header)) / sizeof(fat_arch)) return {};
    auto archs = reinterpret_cast<fat_arch const*>(fat + 1);
    for (uint32_t n = 0; n < count; n++) {
        if (OSSwapBigToHostInt32((uint32_t)archs[n].cputype) != (uint32_t)CPU_TYPE_ARM64) continue;
        uint64_t offset = OSSwapBigToHostInt32(archs[n].offset), size = OSSwapBigToHostInt32(archs[n].size);
        if (offset <= file.size() && size <= file.size() - offset) return file.subspan(offset, size);
    }
    return {};
}

// The type names the game's code generates: it keeps, for every type it
// registers anywhere, the text its compiler printed for
// ls::GetTypeName<T>() ("... [T = <type>]"), and registers the type under
// what ls::_private::GenerateTypeName makes of it.
std::set<std::string> GeneratedTypeNames(std::span<uint8_t const> file)
{
    constexpr std::string_view kBefore = "ls::StringView ls::GetTypeName() [T = ";
    std::set<std::string> names;
    // The literals are in __TEXT,__cstring of the arm64 image.
    auto image = ThinArm64(file);
    std::string_view text;
    if (image.size() >= sizeof(mach_header_64)) {
        auto header = reinterpret_cast<mach_header_64 const*>(image.data());
        size_t at = sizeof(*header);
        for (uint32_t n = 0; n < header->ncmds && at + sizeof(load_command) <= image.size(); n++) {
            auto command = reinterpret_cast<load_command const*>(image.data() + at);
            if (command->cmd == LC_SEGMENT_64 && at + sizeof(segment_command_64) <= image.size()) {
                auto segment = reinterpret_cast<segment_command_64 const*>(command);
                auto sections = reinterpret_cast<section_64 const*>(segment + 1);
                for (uint32_t i = 0; i < segment->nsects; i++) {
                    auto const& section = sections[i];
                    if (strncmp(section.segname, "__TEXT", 16) != 0 || strncmp(section.sectname, "__cstring", 16) != 0) continue;
                    if (section.offset <= image.size() && section.size <= image.size() - section.offset) {
                        text = { reinterpret_cast<char const*>(image.data()) + section.offset, (size_t)section.size };
                    }
                }
            }
            if (!command->cmdsize) break;
            at += command->cmdsize;
        }
    }
    for (size_t at = text.find(kBefore); at != std::string_view::npos; at = text.find(kBefore, at + 1)) {
        size_t end = text.find('\0', at);
        if (end == std::string_view::npos || text[end - 1] != ']') continue;
        names.insert(engine::RegisteredTypeName(text.substr(at + kBefore.size(), end - 1 - at - kBefore.size())));
    }
    return names;
}

int Types(std::string const& app)
{
    Binary game;
    if (!game.Open(app + "/Contents/MacOS/Baldur's Gate 3")) return 2;
    auto resolution = engine::Resolve(engine::EngineTable(), { game.View(), {} });
    // As in the game: a type whose structure is not proven on this binary is kept back.
    auto withheld = LayoutsWithheldOn(resolution);
    auto plan = PlanTypeIndices(resolution, BoundTypes(), withheld.Types);

    int bad = 0;
    size_t perContext[(size_t)TypeContext::Count]{}, boundIn[(size_t)TypeContext::Count]{};
    for (auto const& variable : plan.Variables) {
        perContext[(size_t)variable.Context]++;
        boundIn[(size_t)variable.Context] += variable.Bound;
    }
    printf("contexts (the game's index variables, those that go into the maps, those under a name of Norbyte's):\n");
    for (auto const& context : plan.Contexts) {
        auto c = (size_t)context.Row->Is;
        if (context.StateVA) {
            printf("  %-34s as %-34s %5zu %5zu %5zu   state at %#llx\n", context.Row->Entry, context.Row->NorbyteName, context.Variables,
                perContext[c], boundIn[c], (unsigned long long)context.StateVA);
        } else {
            printf("  %-34s REFUSED: %s\n", context.Row->Entry, context.Refused.c_str());
            bad++;
        }
    }

    constexpr char const* kKinds[] = { "components", "systems", "static data types" };
    printf("\nnames Norbyte's definitions look up:\n");
    for (size_t n = 0; n < (size_t)BoundAs::Count; n++) {
        auto const& tally = plan.Types[n];
        printf("  %-18s %5zu: %5zu mapped, %zu unmapped, %zu ambiguous\n", kKinds[n], tally.Bound, tally.Mapped, tally.Unmapped, tally.Ambiguous);
        bad += tally.Unmapped != 0 || tally.Ambiguous != 0;
    }
    printf("  %zu of the mapped names are spelled differently from the game's symbol and joined by the game's naming rule\n", plan.ByRule);
    printf("  %zu of the components also have a replication index\n", plan.Replicated);
    for (auto const& line : plan.Report) printf("  ** %s\n", line.c_str());

    // In the running game a variable is taken only when the game's list has
    // its type's name at its index. That name must then be one the game's
    // code generates, which the binary shows.
    auto generated = GeneratedTypeNames(game.File);
    size_t notGenerated = 0;
    for (auto const& variable : plan.Variables) {
        if (generated.contains(variable.Registered)) continue;
        if (notGenerated++ < 20) printf("  ** %s: the game's code generates no such type name\n", variable.Registered.c_str());
    }
    printf("\n%zu of %zu variables have a type name the game's code generates (%zu names in the binary)\n",
        plan.Variables.size() - notGenerated, plan.Variables.size(), generated.size());
    bad += notGenerated != 0;

    printf("%s\n", bad ? "NOT every name of Norbyte's definitions has its index variable" : "every name of Norbyte's definitions has one index variable");
    return bad ? 1 : 0;
}

}

int main(int argc, char** argv)
{
    if (argc == 3 && strcmp(argv[1], "check") == 0) return Check(argv[2]);
    if (argc == 3 && strcmp(argv[1], "types") == 0) return Types(argv[2]);
    fprintf(stderr, "usage: bg3mse-binding check|types <Game.app>\n");
    return 2;
}
