#pragma once

#include <cstdint>
#include <span>

// Every component type Norbyte's definitions bind
// (GameDefinitions/Components/AllComponentTypes.inl), with the size this
// build gives it: what EntitySystemHelpersBase::MapComponentIndices is handed
// for each. Compiled as Norbyte's sources are (BoundComponents.cpp).

namespace bg3se::mac {

struct BoundComponent
{
    char const* EngineClass;    // the game's name for the type, e.g. "eoc::HealthComponent"
    char const* Type;           // the C++ type in namespace bg3se, as the list spells it
    uint32_t Size;              // sizeof(Type)
    bool Proxy;                 // stored by pointer: the entity storage holds a pointer of 8 bytes
    bool OneFrame;
};

std::span<BoundComponent const> BoundComponents();

}
