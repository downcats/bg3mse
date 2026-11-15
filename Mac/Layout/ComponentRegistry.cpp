#include <Layout/ComponentRegistry.h>

#include <Engine/TypeIndices.h>
#include <Layout/FunctionCode.h>
#include <Layout/GameEntityWorld.h>
#include <Platform/CodeSearch.h>
#include <Platform/CodeShape.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>

namespace bg3se::mac {

namespace {

constexpr std::string_view kEmplaceEntry = "__ZN2ls11SparseArrayIN3ecs4core18ComponentTypeEntryENS_16DefaultAllocatorEE11EmplaceImplI";
constexpr char kComponentContext[] = "ecs::ComponentTypeIdContext";
constexpr char kOneFrameContext[] = "ecs::OneFrameComponentTypeIdContext";
constexpr uint32_t kIndexMask = 0x7fff;         // and wI, wS, #0x7fff: a component that persists
constexpr uint32_t kOneFrameBit = 0x8000;       // orr wI, wS, #0x8000: a one-frame component
constexpr uint32_t kStackPointer = 31;

struct IndexVariable
{
    engine::TypeIndex const* Type;
    bool OneFrame;
};

// A store through the stack pointer: where, how many bytes, and the general
// register stored (31: the zero register). Stores of other registers, or of
// two at once, carry no register here: their bytes are known to be written
// and not with what.
struct StackStore
{
    int64_t Offset;
    uint32_t Bytes;
    std::optional<uint32_t> Register;
};

std::optional<StackStore> StackStoreOf(uint32_t i)
{
    bool simd = (i >> 26) & 1;
    // Register, unsigned scaled offset.
    if ((i & 0x3b000000) == 0x39000000) {
        uint32_t opc = (i >> 22) & 3, size = i >> 30;
        bool store = simd ? (opc & 1) == 0 : opc == 0;
        if (!store || RegN(i) != kStackPointer) return {};
        uint32_t bytes = simd && (opc & 2) ? 16 : 1u << size;
        StackStore s{ (int64_t)*LoadStoreOffset(i), bytes, {} };
        if (!simd) s.Register = RegD(i);
        return s;
    }
    // Register, unscaled or pre- or post-indexed.
    if ((i & 0x3b200000) == 0x38000000) {
        uint32_t opc = (i >> 22) & 3, size = i >> 30, kind = (i >> 10) & 3;
        bool store = simd ? (opc & 1) == 0 : opc == 0;
        if (!store || RegN(i) != kStackPointer || kind == 2) return {};
        int64_t offset = (i >> 12) & 0x1ff;
        if (offset & 0x100) offset -= 0x200;
        if (kind == 1) offset = 0;          // post-indexed: the store is at the base
        uint32_t bytes = simd && (opc & 2) ? 16 : 1u << size;
        StackStore s{ offset, bytes, {} };
        if (!simd && kind == 0) s.Register = RegD(i);
        return s;
    }
    // Pair.
    if ((i & 0x3a000000) == 0x28000000) {
        bool load = (i >> 22) & 1;
        if (load || RegN(i) != kStackPointer) return {};
        uint32_t opc = i >> 30;
        uint32_t each = simd ? 4u << opc : (opc & 2 ? 8 : 4);
        int64_t offset = (i >> 15) & 0x7f;
        if (offset & 0x40) offset -= 0x80;
        offset *= each;
        if (((i >> 23) & 3) == 1) offset = 0;       // post-indexed
        return StackStore{ offset, 2 * each, {} };
    }
    return {};
}

// The nearest instruction before `from` (exclusive) down to `floor` that may
// write general register `reg`.
std::optional<uint64_t> LastWriteBefore(FunctionCode const& code, uint64_t from, uint64_t floor, uint32_t reg)
{
    for (uint64_t va = from; va > floor;) {
        va -= 4;
        if (MayWriteRegister(code.At(va), reg)) return va;
    }
    return {};
}

std::string At(uint64_t va, char const* what)
{
    char text[160];
    snprintf(text, sizeof(text), "the registration at %#llx: %s", (unsigned long long)va, what);
    return text;
}

}

RegisteredComponent const* ComponentRegistryScan::Find(std::string_view name) const
{
    auto it = std::lower_bound(Components.begin(), Components.end(), name,
        [](RegisteredComponent const& c, std::string_view n) { return c.Name < n; });
    return it != Components.end() && it->Name == name ? &*it : nullptr;
}

ComponentRegistryScan DecodeComponentRegistry(GameImage const& image)
{
    ComponentRegistryScan scan;
    uint64_t emplace = image.FindVAByPrefix(kEmplaceEntry);
    if (!emplace) {
        scan.Problem = "ls::SparseArray<ecs::core::ComponentTypeEntry>::EmplaceImpl is not in the symbol table, or is there more than once";
        return scan;
    }

    // The index variables of the two component contexts, by address.
    auto types = engine::TypeIndices::Collect(image);
    std::map<uint64_t, IndexVariable> variables;
    for (auto const& type : types.All()) {
        if (type.Context == kComponentContext) variables[type.VA] = { &type, false };
        else if (type.Context == kOneFrameContext) variables[type.VA] = { &type, true };
    }
    if (variables.empty()) {
        scan.Problem = "the image names no component type index variable";
        return scan;
    }

    auto sites = CallsTo(image, image.TextBegin(), image.TextEnd(), emplace);
    scan.Sites = (uint32_t)sites.size();
    if (sites.empty()) {
        scan.Problem = "nothing calls ls::SparseArray<ecs::core::ComponentTypeEntry>::EmplaceImpl";
        return scan;
    }

    auto symbols = image.TextSymbols();
    std::optional<FunctionCode> code;
    uint64_t previous = 0;
    std::vector<RegisteredComponent> found;
    for (uint64_t site : sites) {
        auto next = std::upper_bound(symbols.begin(), symbols.end(), site, [](uint64_t va, GameImage::NamedVA const& s) { return va < s.VA; });
        if (next == symbols.begin()) {
            scan.Undecoded.push_back(At(site, "it is in no function"));
            continue;
        }
        uint64_t begin = std::prev(next)->VA, end = next != symbols.end() ? next->VA : image.TextEnd();
        if (!code || code->Begin() != begin) {
            code.emplace(image, begin, end);
            previous = 0;
        }
        // One registration's code lies between the call before it and its own.
        uint64_t floor = previous >= begin ? previous + 4 : begin;
        previous = site;

        // The entry is the call's third argument: add x2, sp, #entry.
        std::optional<int64_t> entry;
        for (uint64_t va = site; va > floor && va + 4 * 8 > site;) {
            va -= 4;
            uint32_t i = code->At(va);
            if (!MayWriteRegister(i, 2)) continue;
            if ((i & 0xff8003ff) == 0x910003e2) entry = (int64_t)*AddImmediate(i);
            break;
        }
        if (!entry) {
            scan.Undecoded.push_back(At(site, "its entry is not passed as an address on the stack"));
            continue;
        }

        // Every store into the entry's first field and its two sizes, in
        // the order the code has them.
        struct Written
        {
            bool Is{ false };
            std::optional<uint8_t> Value;
        };
        Written sizes[4];
        std::optional<uint64_t> typeIdStore;
        bool readable = true;
        for (uint64_t va = floor; va < site; va += 4) {
            auto store = StackStoreOf(code->At(va));
            if (!store) continue;
            int64_t from = store->Offset - *entry, to = from + store->Bytes;
            if (from == 0 && store->Bytes == 2 && store->Register && *store->Register != 31) typeIdStore = va;
            else if (from < 2 && to > 0 && typeIdStore) typeIdStore.reset();       // overwritten by something else
            if (to <= (int64_t)kComponentTypeEntrySizes || from >= (int64_t)kComponentTypeEntrySizes + 4) continue;
            std::optional<uint64_t> value;
            if (store->Register) value = *store->Register == 31 ? std::optional<uint64_t>(0) : code->ConstantAt(va, *store->Register);
            for (uint32_t b = 0; b < store->Bytes; b++) {
                int64_t at = from + b - (int64_t)kComponentTypeEntrySizes;
                if (at < 0 || at >= 4) continue;
                sizes[at].Is = true;
                sizes[at].Value = value ? std::optional<uint8_t>((uint8_t)(*value >> (8 * b))) : std::nullopt;
            }
        }
        for (auto const& byte : sizes) readable = readable && byte.Is && byte.Value;
        if (!readable) {
            scan.Undecoded.push_back(At(site, "the entry's sizes are not constants stored on every path"));
            continue;
        }
        if (!typeIdStore) {
            scan.Undecoded.push_back(At(site, "nothing stores a type index into the entry's first field"));
            continue;
        }

        // The type index: the 15 low bits of what the type's index variable
        // holds, or that with the one-frame bit, loaded through the
        // variable's address (formed directly, or loaded from its pointer).
        uint32_t stored = RegD(code->At(*typeIdStore));
        auto maskAt = LastWriteBefore(*code, *typeIdStore, floor, stored);
        uint32_t mask = maskAt ? code->At(*maskAt) : 0;
        bool persists = (mask & 0xffc00000) == 0x12000000, oneFrame = (mask & 0xffc00000) == 0x32000000;
        auto bits = maskAt && (persists || oneFrame) ? LogicalImmediate(mask) : std::nullopt;
        if (!bits || (persists && *bits != kIndexMask) || (oneFrame && *bits != kOneFrameBit)) {
            scan.Undecoded.push_back(At(site, "the stored type index is not a masked index variable"));
            continue;
        }
        auto loadAt = LastWriteBefore(*code, *maskAt, floor, RegN(mask));
        uint32_t load = loadAt ? code->At(*loadAt) : 0;
        bool loads = ((load & 0xffc00000) == 0x79400000 || (load & 0xffc00000) == 0xb9400000) && *LoadStoreOffset(load) == 0;
        auto addressAt = loads ? LastWriteBefore(*code, *loadAt, floor, RegN(load)) : std::nullopt;
        uint64_t variable = 0;
        if (addressAt && *addressAt >= floor + 4) {
            uint32_t second = code->At(*addressAt), first = code->At(*addressAt - 4);
            uint64_t formed = PageAddress(*addressAt - 4, first, second);
            if (formed && (second & 0xffc00000) == 0xf9400000) {            // ldr x, [x, #slot]: the variable's pointer
                if (auto pointer = image.PointerAt(formed)) variable = *pointer;
            } else if (formed && (second & 0xff800000) == 0x91000000) {     // add x, x, #offset: the variable
                variable = formed;
            }
        }
        auto named = variables.find(variable);
        if (named == variables.end()) {
            scan.Undecoded.push_back(At(site, "the stored type index is not read from a component type's index variable"));
            continue;
        }
        if (named->second.OneFrame != oneFrame) {
            scan.Undecoded.push_back(At(site, "the one-frame bit does not agree with the index variable's context"));
            continue;
        }

        uint16_t inlineSize = (uint16_t)(*sizes[0].Value | (*sizes[1].Value << 8));
        uint16_t totalSize = (uint16_t)(*sizes[2].Value | (*sizes[3].Value << 8));
        found.push_back({ named->second.Type->Type, inlineSize, totalSize, oneFrame });
    }

    // One entry per type: a type registered in two places must be the same in both.
    std::sort(found.begin(), found.end(), [](RegisteredComponent const& a, RegisteredComponent const& b) { return a.Name < b.Name; });
    for (size_t n = 0; n < found.size();) {
        size_t last = n + 1;
        bool same = true;
        while (last < found.size() && found[last].Name == found[n].Name) {
            same = same && found[last].InlineSize == found[n].InlineSize && found[last].TotalSize == found[n].TotalSize
                && found[last].OneFrame == found[n].OneFrame;
            last++;
        }
        if (same) scan.Components.push_back(std::move(found[n]));
        else scan.Undecoded.push_back(found[n].Name + ": its registrations do not agree");
        n = last;
    }
    return scan;
}

}
