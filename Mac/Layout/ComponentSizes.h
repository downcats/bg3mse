#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <Platform/Symbols.h>

// The size the game uses for a component type, read from the game's own code.
// ecs::legacy::ImmediateWorldCache::AddComponent<T> exists once per component
// type that is added through the immediate cache; it passes sizeof(T) to
// ecs::ComponentFrameStorageAllocRaw and stores the same number into the
// frame storage it sets up for T. The component's name is the template
// argument in the function's symbol. Nothing here is a table: every size is
// decoded from the image each time.

namespace bg3se::mac {

struct ComponentSize
{
    std::string Name;       // as the symbol spells it, e.g. "eoc::HealthComponent"
    uint32_t Size;
};

struct ComponentSizeScan
{
    std::vector<ComponentSize> Sizes;       // one per component type, sorted by name
    uint32_t Functions{ 0 };                // AddComponent<T, Args...> functions in the symbol table
    // Those in which both witnesses of the size were found and agree: the
    // argument of the allocation call and the store into the frame storage.
    uint32_t FunctionsDecoded{ 0 };
    // Component types with no size: none of their functions has both
    // witnesses, or two of their functions give different sizes.
    std::vector<std::string> Undecoded;
    // The offset in the frame storage the size is stored at, as the functions
    // agree on it (0 when they do not).
    uint32_t SizeFieldOffset{ 0 };
    std::string Problem;                    // empty when the scan ran

    bool Ok() const { return Problem.empty(); }
    ComponentSize const* Find(std::string_view name) const;
};

ComponentSizeScan DecodeComponentSizes(GameImage const& image);

}
