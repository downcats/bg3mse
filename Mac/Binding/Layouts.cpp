#include <Binding/Layouts.h>

#include <Binding/VirtualCalls.h>
#include <Layout/ComponentMembers.h>
#include <Layout/DeclaredVirtuals.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace bg3se::mac::binding {

LayoutsWithheld WithheldFor(GameShapes const& shapes, std::span<LayoutDependent const> dependents)
{
    LayoutsWithheld withheld;
    for (auto const& dependent : dependents) {
        if (shapes.Confirmed(dependent.Shape)) continue;
        std::string why = std::string("the game's ") + Describe(dependent.Shape) + " is not confirmed to be laid out as this build declares it";
        for (auto const& proof : shapes.Proofs) {
            if (proof.Shape == dependent.Shape && !proof.Problem.empty()) {
                why += std::string(" (") + proof.Function + ": " + proof.Problem + ")";
                break;
            }
        }
        if (dependent.Member) withheld.Members.push_back({ dependent.Member, why });
        if (dependent.EngineClass) withheld.Types.push_back({ dependent.EngineClass, why });
    }
    return withheld;
}

void WithholdFor(LayoutsWithheld& withheld, std::span<VirtualSlotResult const> slots, std::span<VirtualDependent const> dependents)
{
    std::vector<std::string> classes;
    for (auto const& dependent : dependents) {
        if (std::find(classes.begin(), classes.end(), dependent.Class) == classes.end()) classes.push_back(dependent.Class);
    }
    for (auto const& cls : classes) {
        VirtualSlotResult const* failed = nullptr;
        bool checked = false;
        for (auto const& slot : slots) {
            if (!slot.Of || cls != slot.Of->Class) continue;
            checked = true;
            if (!slot.Problem.empty()) {
                failed = &slot;
                break;
            }
        }
        if (checked && !failed) {
            withheld.ConfirmedClasses.push_back(cls);
            continue;
        }
        std::string why = "the game's vtables are not confirmed to hold the virtual functions of " + cls + " where this build calls them";
        if (failed) why += std::string(" (") + failed->Of->Method + ": " + failed->Problem + ")";
        withheld.UnconfirmedClasses.push_back(why);
        for (auto const& dependent : dependents) {
            if (cls != dependent.Class) continue;
            if (dependent.Member) withheld.Members.push_back({ dependent.Member, why });
            if (dependent.EngineClass) withheld.Types.push_back({ dependent.EngineClass, why });
            if (dependent.Refuses) withheld.Refusing.push_back(why);
        }
    }
}

void WithholdContradicted(LayoutsWithheld& withheld, GameFieldScan const& game, std::span<BoundClass const> classes)
{
    auto compared = CompareComponentMembers(game, classes);
    std::vector<std::pair<char const*, size_t>> types;     // a type, and its contradictions
    for (auto const& difference : compared.Differences) {
        if (!difference.Contradicts() || !difference.EngineClass) continue;
        auto type = std::find_if(types.begin(), types.end(), [&](auto const& t) { return strcmp(t.first, difference.EngineClass) == 0; });
        if (type != types.end()) {
            type->second++;
            continue;
        }
        types.push_back({ difference.EngineClass, 1 });
        withheld.Types.push_back({ difference.EngineClass, "the game's code puts a member elsewhere than this build: " + difference.Describe() });
    }
    for (auto const& [type, count] : types) {
        if (count == 1) continue;
        for (auto& held : withheld.Types) {
            if (held.EngineClass == type) held.Why += " (and " + std::to_string(count - 1) + " more)";
        }
    }
}

LayoutsWithheld LayoutsWithheldOn(engine::Resolution const& resolution)
{
    auto game = resolution.In.Game.Symbols;
    auto shapes = game ? CheckGameShapes(*game) : GameShapes{};
    auto withheld = WithheldFor(shapes, LayoutDependents());
    auto slots = game ? CheckVirtualSlots(*game, DeclaredVirtuals()) : std::vector<VirtualSlotResult>{};
    RequireResultProofs(slots, shapes);
    RequireMemberShapes(slots, shapes, ClassMemberShapes());
    WithholdFor(withheld, slots, VirtualDependents());
    // Without the game's image, or on a game whose code cannot be read for
    // its members, the component types stay as they are: the comparison
    // speaks only where the game's code does.
    if (game) {
        auto classes = BoundClasses();
        auto fields = DecodeBoundComponentFields(*game, classes);
        if (fields.Ok()) {
            WithholdContradicted(withheld, fields, classes);
            withheld.MembersPartlyUnchecked = fields.NotFound;
        } else {
            withheld.MembersUnchecked = fields.Problem;
        }
    } else {
        withheld.MembersUnchecked = "the game's image is not readable";
    }
    return withheld;
}

namespace {

std::atomic<LayoutsWithheld const*> gLayoutsOfTheGame{ nullptr };

}

void KeepLayoutsOfTheGame(LayoutsWithheld const* layouts)
{
    gLayoutsOfTheGame.store(layouts, std::memory_order_release);
}

LayoutsWithheld const* LayoutsOfTheGame()
{
    return gLayoutsOfTheGame.load(std::memory_order_acquire);
}

LayoutsWithheld LayoutsFor(engine::Resolution const& resolution)
{
    auto kept = LayoutsOfTheGame();
    return kept ? *kept : LayoutsWithheldOn(resolution);
}

bool VirtualsConfirmed(char const* cls)
{
    auto kept = LayoutsOfTheGame();
    if (!kept || !cls) return false;
    return std::any_of(kept->ConfirmedClasses.begin(), kept->ConfirmedClasses.end(), [cls](std::string const& confirmed) { return confirmed == cls; });
}

}
