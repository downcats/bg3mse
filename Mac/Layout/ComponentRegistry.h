#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Platform/Symbols.h>

// The sizes the game registers for every component type, read from the
// game's own code.
//
// A component type is registered by building an ecs::core::ComponentTypeEntry
// on the stack and handing it to
// ls::SparseArray<ecs::core::ComponentTypeEntry>::EmplaceImpl; the game does
// this once per type, inline in a handful of large functions
// (eoc::RegisterComponents, esv::RegisterComponents, ...). Each registration
// reads the type's index variable into the entry's first field, which names
// the type, and stores the two sizes of the entry as constants: the size the
// entity storage keeps per entity, and the total. They are equal unless the
// storage keeps a pointer to the component; the total is then that pointer
// and the component behind it.
//
// This covers every component type, where the AddComponent<T> functions of
// Layout/ComponentSizes.h exist for about half of them; where both give a
// size, the two are independent statements of it. Nothing here is a table:
// every size is decoded from the image each time.

namespace bg3se::mac {

struct RegisteredComponent
{
    std::string Name;           // as the index variable's symbol spells it, e.g. "eoc::HealthComponent"
    uint16_t InlineSize;        // what the entity storage holds per entity
    uint16_t TotalSize;         // the same, or a stored pointer and the component it points to
    bool OneFrame;              // registered as a one-frame component
};

struct ComponentRegistryScan
{
    std::vector<RegisteredComponent> Components;    // one per component type, sorted by name
    uint32_t Sites{ 0 };                            // calls that register an entry
    // Calls whose entry could not be read in full (the type, both sizes as
    // constants on every path, nothing else stored over them), each with
    // where it is and what was missing.
    std::vector<std::string> Undecoded;
    std::string Problem;                            // empty when the scan ran

    bool Ok() const { return Problem.empty(); }
    RegisteredComponent const* Find(std::string_view name) const;
};

ComponentRegistryScan DecodeComponentRegistry(GameImage const& image);

}
