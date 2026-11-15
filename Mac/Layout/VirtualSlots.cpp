#include <Layout/VirtualSlots.h>

#include <Platform/CodeSearch.h>

#include <algorithm>
#include <cstring>

namespace bg3se::mac {

namespace {

constexpr uint64_t kAddressPoint = 0x10;        // after the offset to top and the type information pointer

// The address point of the table in the vtable group at `vtable` whose
// offset to top is `offsetToTop`: the primary one for zero; otherwise the
// first table after the primary one that starts with that offset and the
// group's type information.
uint64_t TableOf(GameImage const& image, uint64_t vtable, uint64_t end, int16_t offsetToTop)
{
    if (!offsetToTop) return vtable + kAddressPoint;
    auto type = image.PointerAt(vtable + 8);
    if (!type || !*type) return 0;
    for (uint64_t at = vtable + kAddressPoint; at + kAddressPoint <= end; at += 8) {
        auto data = image.DataAt(at, sizeof(int64_t));
        int64_t word = 0;
        if (data) memcpy(&word, data, sizeof(word));
        if (!data || word != offsetToTop) continue;
        auto next = image.PointerAt(at + 8);
        if (next && *next == *type) return at + kAddressPoint;
    }
    return 0;
}

}

std::vector<VirtualSlotResult> CheckVirtualSlots(GameImage const& image, std::span<DeclaredVirtual const> declared)
{
    std::vector<VirtualSlotResult> results;
    std::vector<char const*> names;
    for (auto const& row : declared) {
        names.push_back(row.Vtable);
        names.push_back(row.Function);
    }
    std::vector<uint64_t> vas(names.size(), 0);
    image.FindVAs(names, vas);
    std::vector<uint64_t> vtables, ends;
    for (size_t n = 0; n < declared.size(); n++) vtables.push_back(vas[2 * n]);
    ends.assign(vtables.size(), 0);
    image.NextSymbolVAs(vtables, ends);

    for (size_t n = 0; n < declared.size(); n++) {
        VirtualSlotResult result;
        result.Of = &declared[n];
        uint64_t vtable = vtables[n], function = vas[2 * n + 1];
        if (!vtable || ends[n] <= vtable + kAddressPoint) {
            result.Problem = "the game has no vtable of that name";
        } else if (!function) {
            result.Problem = "the game has no function of that name";
        } else if (image.PointersOnlyAtRunTime()) {
            result.Problem = "the vtable's slots cannot be read from this file";
        } else if (uint64_t first = TableOf(image, vtable, ends[n], declared[n].OffsetToTop); !first) {
            result.Problem = "the game's vtable has no table for the base at " + std::to_string(declared[n].OffsetToTop);
        } else {
            auto slots = VtableSlotsOf(image, first, ends[n], function);
            if (slots.empty()) {
                result.Problem = "the game's vtable does not hold the function";
            } else {
                result.GameSlot = (int)((slots[0] - first) / 8);
                bool held = std::any_of(slots.begin(), slots.end(), [&](uint64_t slot) { return (slot - first) / 8 == declared[n].Slot; });
                if (!held) {
                    result.Problem = "this build calls it through slot " + std::to_string(declared[n].Slot) + ", and the game's vtable holds it in slot "
                        + std::to_string(result.GameSlot);
                }
            }
        }
        results.push_back(std::move(result));
    }
    return results;
}

void RequireResultProofs(std::vector<VirtualSlotResult>& slots, GameShapes const& shapes)
{
    for (auto& slot : slots) {
        if (!slot.Problem.empty() || !slot.Of || !slot.Of->ResultProof) continue;
        auto proof = std::find_if(shapes.Proofs.begin(), shapes.Proofs.end(),
            [&](FunctionProof const& p) { return strcmp(p.Function, slot.Of->ResultProof) == 0; });
        if (proof == shapes.Proofs.end()) {
            slot.Problem = std::string("how its result comes back is not proven: there is no proof named ") + slot.Of->ResultProof;
        } else if (!proof->Problem.empty()) {
            slot.Problem = std::string("how its result comes back is not confirmed: ") + proof->Function + ": " + proof->Problem;
        }
    }
}

std::span<ClassMembers const> ClassMemberShapes()
{
    // Ext.Audio reads the sound objects of the manager it calls.
    static constexpr ClassMembers kClasses[] = {
        { "SoundManager", GameShape::Audio },
        // Lua reads a physics resource's objects by their members.
        { "resource::PhysicsObject", GameShape::PhysicsObjects },
    };
    return kClasses;
}

void RequireMemberShapes(std::vector<VirtualSlotResult>& slots, GameShapes const& shapes, std::span<ClassMembers const> classes)
{
    for (auto& slot : slots) {
        if (!slot.Problem.empty() || !slot.Of) continue;
        for (auto const& cls : classes) {
            if (strcmp(cls.Class, slot.Of->Class) != 0 || shapes.Confirmed(cls.Shape)) continue;
            slot.Problem = std::string("the class's members are not confirmed: the game's ") + Describe(cls.Shape) + " is not";
            for (auto const& proof : shapes.Proofs) {
                if (proof.Shape == cls.Shape && !proof.Problem.empty()) {
                    slot.Problem += std::string(" (") + proof.Function + ": " + proof.Problem + ")";
                    break;
                }
            }
        }
    }
}

}
