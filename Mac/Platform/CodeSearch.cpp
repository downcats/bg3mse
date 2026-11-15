#include <Platform/CodeSearch.h>

namespace bg3se::mac {

std::vector<uint64_t> FindPattern(GameImage const& image, uint64_t begin, uint64_t end, std::span<MaskedWord const> pattern)
{
    std::vector<uint64_t> found;
    uint64_t length = 4 * (uint64_t)pattern.size();
    if (pattern.empty() || end < begin || end - begin < length) return found;
    for (uint64_t va = begin; va + length <= end; va += 4) {
        size_t n = 0;
        while (n < pattern.size() && pattern[n].Matches(image.Word(va + 4 * n))) n++;
        if (n == pattern.size()) found.push_back(va);
    }
    return found;
}

std::vector<uint64_t> CallsTo(GameImage const& image, uint64_t begin, uint64_t end, uint64_t callee)
{
    std::vector<uint64_t> sites;
    for (uint64_t va = begin; va + 4 <= end; va += 4) {
        if (CallTarget(va, image.Word(va)) == callee) sites.push_back(va);
    }
    return sites;
}

std::vector<uint64_t> VtableSlotsOf(GameImage const& image, uint64_t vtable, uint64_t vtableEnd, uint64_t method)
{
    std::vector<uint64_t> slots;
    for (uint64_t slot = vtable; method && slot + 8 <= vtableEnd; slot += 8) {
        auto value = image.PointerAt(slot);
        if (value && *value == method) slots.push_back(slot);
    }
    return slots;
}

bool CallsImport(GameImage const& image, uint64_t siteVA, std::string_view stubName)
{
    uint64_t target = CallTarget(siteVA, image.Word(siteVA));
    return target && image.StubName(target) == stubName;
}

}
