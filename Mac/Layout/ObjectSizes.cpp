#include <Layout/ObjectSizes.h>

#include <Layout/FunctionCode.h>
#include <Platform/CodeShape.h>

#include <algorithm>
#include <map>
#include <optional>
#include <unordered_map>

namespace bg3se::mac {

namespace {

// How far after the allocation the object is made.
constexpr uint32_t kWindow = 64;

bool IsCall(uint32_t i) { return (i & 0xfc000000) == 0x94000000; }
bool IsMoveRegister(uint32_t i) { return (i & 0xffe0ffe0) == 0xaa0003e0; }     // mov xd, xm
bool IsAdrp(uint32_t i) { return (i & 0x9f000000) == 0x90000000; }
bool IsAddImmediate64(uint32_t i) { return (i & 0xffc00000) == 0x91000000; }   // add xd, xn, #imm
bool IsStore64(uint32_t i) { return (i & 0xffc00000) == 0xf9000000; }          // str xt, [xn, #offset]
bool IsStorePair64(uint32_t i) { return (i & 0xffc00000) == 0xa9000000; }      // stp xt, xt2, [xn, #offset]
bool EndsBlock(uint32_t i)
{
    return (i & 0xfc000000) == 0x14000000                                      // b
        || (i & 0xfffffc1f) == 0xd65f0000 || (i & 0xfffffc1f) == 0xd61f0000;   // ret, br
}

bool IsConstructor(std::string_view name)
{
    // ...C1E<parameters> or ...C2E<parameters> of a nested name.
    if (!name.starts_with("__ZN")) return false;
    size_t at = name.find("C1E");
    if (at == std::string_view::npos) at = name.find("C2E");
    return at != std::string_view::npos;
}

}

ObjectSize const* ObjectSizeScan::Find(std::string_view witness) const
{
    auto it = std::lower_bound(Sizes.begin(), Sizes.end(), witness, [](ObjectSize const& s, std::string_view n) { return s.Witness < n; });
    return it != Sizes.end() && it->Witness == witness ? &*it : nullptr;
}

bool ObjectSizeScan::Conflicts(std::string_view witness) const
{
    return std::binary_search(Conflicting.begin(), Conflicting.end(), witness);
}

ObjectSizeScan DecodeObjectSizes(GameImage const& image)
{
    ObjectSizeScan scan;

    // The allocation functions: operator new, the game's operator new with a
    // tag, and malloc.
    std::vector<uint64_t> allocators;
    for (char const* name : { "__Znwm", "__Znwmi" }) {
        if (uint64_t va = image.FindVA(name)) allocators.push_back(va);
    }
    if (uint64_t stub = image.StubOf("_malloc")) allocators.push_back(stub);
    if (uint64_t stub = image.StubOf("__Znwm")) allocators.push_back(stub);
    if (allocators.empty()) {
        scan.Problem = "no allocation function (operator new, malloc) was found in the image";
        return scan;
    }
    auto isAllocator = [&](uint64_t va) { return std::find(allocators.begin(), allocators.end(), va) != allocators.end(); };

    // Vtables by the address an object's first word holds (two words into
    // the vtable), and constructors by address.
    std::unordered_map<uint64_t, std::string_view> vtables, constructors;
    for (auto const& symbol : image.SymbolsNamed("__ZTV", "")) vtables.emplace(symbol.VA + 0x10, symbol.Name);
    auto text = image.TextSymbols();
    for (auto const& symbol : text) {
        if (IsConstructor(symbol.Name)) constructors.emplace(symbol.VA, symbol.Name);
    }

    std::map<std::string_view, std::map<uint32_t, uint32_t>> found;     // witness -> size -> sites
    uint64_t begin = image.TextBegin(), end = image.TextEnd();
    for (uint64_t va = begin + 4; va < end; va += 4) {
        uint32_t call = image.Word(va);
        if (!IsCall(call) || !isAllocator(CallTarget(va, call))) continue;
        auto size = MovzValue(image.Word(va - 4));      // mov w0, #size
        if (!size || RegD(image.Word(va - 4)) != 0 || *size == 0 || *size > 0xffff) continue;
        scan.Allocations++;

        // From the call on: which registers hold the block, and which an
        // address formed by adrp and add.
        uint32_t block = 1u << 0;
        std::optional<uint64_t> address[31];
        std::string_view witness;
        uint64_t at = va;
        for (uint32_t n = 1; n <= kWindow && at + 4 < end; n++) {
            at += 4;
            uint32_t i = image.Word(at);
            // cbnz xBlock, ahead: what lies between reports a failed
            // allocation; the object is made from `ahead` on.
            if ((i & 0xff000000) == 0xb5000000 && (block & (1u << RegD(i)))) {
                uint64_t ahead = at + (uint64_t)(((int64_t)(int32_t)(i << 8) >> 13) * 4);
                if (ahead > at && ahead < at + 4ull * kWindow && ahead < end) {
                    for (uint32_t r = 0; r <= 18; r++) {
                        if (r != RegD(i)) block &= ~(1u << r);
                        address[r].reset();
                    }
                    at = ahead - 4;
                    continue;
                }
            }
            if (IsCall(i)) {
                // A constructor called on the block: the object is of its
                // class, unless a vtable is stored into the block after it
                // (the constructor was a base's, and the class is the
                // vtable's). Any other call ends the search.
                auto constructor = constructors.find(CallTarget(at, i));
                if (constructor == constructors.end() || !(block & 1) || !witness.empty()) break;
                witness = constructor->second;
                block &= ~0x7ffffu;         // what the call leaves in x0..x18 is not known
                for (uint32_t r = 0; r <= 18; r++) address[r].reset();
                if (!block) break;
                continue;
            }
            if (EndsBlock(i)) break;
            if (IsStore64(i) && *LoadStoreOffset(i) == 0 && RegN(i) < 31 && (block & (1u << RegN(i))) && RegD(i) < 31 && address[RegD(i)]) {
                auto vtable = vtables.find(*address[RegD(i)]);
                if (vtable != vtables.end()) witness = vtable->second;
                break;
            }
            if (IsStorePair64(i) && *PairOffset(i) == 0 && RegN(i) < 31 && (block & (1u << RegN(i))) && RegD(i) < 31 && address[RegD(i)]) {
                auto vtable = vtables.find(*address[RegD(i)]);
                if (vtable != vtables.end()) witness = vtable->second;
                break;
            }
            if (IsMoveRegister(i)) {
                uint32_t from = (i >> 16) & 0x1f, to = RegD(i);
                if (to < 31) {
                    block = (from < 31 && (block & (1u << from))) ? block | (1u << to) : block & ~(1u << to);
                    address[to] = from < 31 ? address[from] : std::nullopt;
                }
                continue;
            }
            if (IsAdrp(i)) {
                uint32_t to = RegD(i);
                if (to < 31) {
                    int64_t pages = (int64_t)(((uint64_t)((i >> 5) & 0x7ffff) << 2) | ((i >> 29) & 3));
                    if (pages & 0x100000) pages -= 0x200000;
                    address[to] = (uint64_t)((int64_t)(at & ~0xfffull) + pages * 0x1000);
                    block &= ~(1u << to);
                }
                continue;
            }
            if (IsAddImmediate64(i) && RegD(i) < 31 && RegN(i) < 31 && address[RegN(i)]) {
                address[RegD(i)] = *address[RegN(i)] + ((i >> 10) & 0xfff);
                block &= ~(1u << RegD(i));
                continue;
            }
            for (uint32_t r = 0; r < 31; r++) {
                if (!MayWriteRegister(i, r)) continue;
                block &= ~(1u << r);
                address[r].reset();
            }
        }
        if (!witness.empty()) found[witness][(uint32_t)*size]++;
    }

    for (auto const& [witness, sizes] : found) {
        if (sizes.size() == 1) scan.Sizes.push_back({ std::string(witness), sizes.begin()->first, sizes.begin()->second });
        else scan.Conflicting.emplace_back(witness);
    }
    std::sort(scan.Sizes.begin(), scan.Sizes.end(), [](ObjectSize const& a, ObjectSize const& b) { return a.Witness < b.Witness; });
    std::sort(scan.Conflicting.begin(), scan.Conflicting.end());
    if (scan.Sizes.empty()) scan.Problem = "no allocation in the image is followed by the making of an object";
    return scan;
}

}
