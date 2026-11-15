#include <Layout/ComponentSizes.h>

#include <Layout/FunctionCode.h>
#include <Platform/CodeShape.h>

#include <algorithm>
#include <cstdlib>
#include <cxxabi.h>
#include <map>
#include <optional>

namespace bg3se::mac {

namespace {

constexpr std::string_view kAddComponentPrefix = "__ZN3ecs6legacy19ImmediateWorldCache12AddComponentI";
constexpr char kAllocRaw[] = "__ZN3ecs29ComponentFrameStorageAllocRawERNS_21ComponentFrameStorageEiRNS_26ComponentFrameStorageIndexE";
constexpr std::string_view kAddComponent = "ecs::legacy::ImmediateWorldCache::AddComponent<";

bool IsStoreHalf(uint32_t i) { return (i & 0xffc00000) == 0x79000000; }    // strh wT, [xN, #offset]

// The first template argument of AddComponent in a symbol of the game
// (Mach-O spelling, one underscore more than the Itanium name).
std::string ComponentNameOf(std::string_view symbol)
{
    std::string mangled(symbol.substr(1));
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    if (!demangled) return {};
    std::string text(demangled);
    free(demangled);

    size_t at = text.find(kAddComponent);
    if (at == std::string::npos) return {};
    at += kAddComponent.size();
    int depth = 0;
    for (size_t end = at; end < text.size(); end++) {
        char c = text[end];
        if (c == '<' || c == '(') depth++;
        else if ((c == '>' || c == ')') && depth > 0) depth--;
        else if ((c == '>' || c == ',') && depth == 0) return text.substr(at, end - at);
    }
    return {};
}

struct Candidate
{
    std::string Name;
    std::optional<uint32_t> Argument;                       // the constant every allocation call passes
    std::vector<std::pair<uint32_t, uint32_t>> Stores;      // strh of a known constant: offset, value
    std::vector<uint32_t> UnknownStores;                    // strh of something else: offset
};

}

ComponentSize const* ComponentSizeScan::Find(std::string_view name) const
{
    auto it = std::lower_bound(Sizes.begin(), Sizes.end(), name, [](ComponentSize const& s, std::string_view n) { return s.Name < n; });
    return it != Sizes.end() && it->Name == name ? &*it : nullptr;
}

ComponentSizeScan DecodeComponentSizes(GameImage const& image)
{
    ComponentSizeScan scan;
    uint64_t alloc = image.FindVA(kAllocRaw);
    if (!alloc) {
        scan.Problem = "ecs::ComponentFrameStorageAllocRaw is not in the symbol table under the expected signature";
        return scan;
    }

    auto symbols = image.TextSymbols();
    std::vector<Candidate> candidates;
    for (size_t n = 0; n < symbols.size(); n++) {
        if (!symbols[n].Name.starts_with(kAddComponentPrefix)) continue;
        scan.Functions++;
        Candidate c;
        c.Name = ComponentNameOf(symbols[n].Name);
        if (c.Name.empty()) {
            scan.Undecoded.emplace_back(symbols[n].Name);
            continue;
        }

        uint64_t end = n + 1 < symbols.size() ? symbols[n + 1].VA : image.TextEnd();
        FunctionCode code(image, symbols[n].VA, end);
        auto calls = code.CallsTo(alloc);
        bool agreed = !calls.empty();
        for (uint64_t site : calls) {
            auto value = code.ConstantAt(site, 1);      // w1: the size argument
            if (!value || *value > 0xffff || (c.Argument && *c.Argument != *value)) {
                agreed = false;
                break;
            }
            c.Argument = (uint32_t)*value;
        }
        if (!agreed) c.Argument.reset();

        for (uint64_t va = code.Begin(); va < code.End(); va += 4) {
            uint32_t i = code.At(va);
            if (!IsStoreHalf(i)) continue;
            uint32_t offset = *LoadStoreOffset(i);
            if (auto value = code.ConstantAt(va, RegD(i)); value && *value <= 0xffff) c.Stores.emplace_back(offset, (uint32_t)*value);
            else c.UnknownStores.push_back(offset);
        }
        candidates.push_back(std::move(c));
    }
    if (candidates.empty()) {
        scan.Problem = "no ecs::legacy::ImmediateWorldCache::AddComponent<T> in the symbol table";
        return scan;
    }

    // The field the functions store the size into: the offset at which most
    // of them store the constant they also pass to the allocation.
    std::map<uint32_t, uint32_t> votes;
    uint32_t withArgument = 0;
    for (auto const& c : candidates) {
        if (!c.Argument) continue;
        withArgument++;
        std::vector<uint32_t> seen;
        for (auto const& [offset, value] : c.Stores) {
            if (value == *c.Argument && std::find(seen.begin(), seen.end(), offset) == seen.end()) {
                votes[offset]++;
                seen.push_back(offset);
            }
        }
    }
    uint32_t best = 0;
    for (auto const& [offset, count] : votes) {
        if (count > best) {
            best = count;
            scan.SizeFieldOffset = offset;
        }
    }
    if (best * 2 <= withArgument) {
        scan.SizeFieldOffset = 0;
        scan.Problem = "the AddComponent<T> functions do not agree on where they store the component size";
        return scan;
    }

    std::vector<std::string> failed;
    for (auto& c : candidates) {
        // Both witnesses, and nothing else stored into the size field.
        bool stored = false, conflict = false;
        for (auto const& [offset, value] : c.Stores) {
            if (offset != scan.SizeFieldOffset) continue;
            if (c.Argument && value == *c.Argument) stored = true;
            else conflict = true;
        }
        for (uint32_t offset : c.UnknownStores) conflict = conflict || offset == scan.SizeFieldOffset;
        if (c.Argument && stored && !conflict) {
            scan.Sizes.push_back({ std::move(c.Name), *c.Argument });
            scan.FunctionsDecoded++;
        } else {
            failed.push_back(std::move(c.Name));
        }
    }

    // One size per component type. AddComponent<T, Args...> exists once per
    // argument list, so a type can have several functions: they must agree.
    std::sort(scan.Sizes.begin(), scan.Sizes.end(), [](ComponentSize const& a, ComponentSize const& b) { return a.Name < b.Name; });
    std::vector<ComponentSize> unique;
    for (size_t n = 0; n < scan.Sizes.size();) {
        size_t last = n + 1;
        bool same = true;
        while (last < scan.Sizes.size() && scan.Sizes[last].Name == scan.Sizes[n].Name) {
            same = same && scan.Sizes[last].Size == scan.Sizes[n].Size;
            last++;
        }
        if (same) unique.push_back(std::move(scan.Sizes[n]));
        else failed.push_back(scan.Sizes[n].Name);
        n = last;
    }
    scan.Sizes = std::move(unique);

    std::sort(failed.begin(), failed.end());
    failed.erase(std::unique(failed.begin(), failed.end()), failed.end());
    for (auto& name : failed) {
        if (!scan.Find(name)) scan.Undecoded.push_back(std::move(name));
    }
    return scan;
}

}
