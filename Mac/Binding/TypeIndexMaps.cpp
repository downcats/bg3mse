#include <stdafx.h>
#include <GameDefinitions/Symbols.h>

#include <Binding/TypeIndexBinding.h>

// The one place that puts entries into the type index maps of Norbyte's
// StaticSymbols, in the form his engine callbacks give them on Windows
// (GameHooks/DataLibrariesBG3Game.cpp).

BEGIN_NS(mac::binding)

namespace {

struct StaticSymbolsMaps : TypeIndexSink
{
    explicit StaticSymbolsMaps(StaticSymbols& symbols) : Symbols(symbols) {}

    void Context(int32_t* state, char const* norbyteName) override
    {
        Symbols.IndexSymbolToContextMaps.insert(std::make_pair(state, norbyteName));
    }

    void Type(int32_t* variable, char const* name, int32_t* state) override
    {
        Symbols.IndexSymbolToNameMaps.insert(std::make_pair(variable, IndexSymbolInfo{ name, state }));
    }

    StaticSymbols& Symbols;
};

}

TypeIndexFill FillStaticSymbolsTypeIndices(void* staticSymbols, TypeIndexPlan const& plan, std::function<void*(uint64_t)> const& at)
{
    auto& symbols = *static_cast<StaticSymbols*>(staticSymbols);
    if (!symbols.IndexSymbolToContextMaps.empty() || !symbols.IndexSymbolToNameMaps.empty()) return {};
    StaticSymbolsMaps maps(symbols);
    return FillTypeIndices(plan, at, maps);
}

END_NS()
