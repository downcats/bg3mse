#pragma once

#include <cstdint>
#include <span>
#include <vector>

// Every structure Norbyte's definitions give a property map
// (GameDefinitions/Generated/PropertyMaps.inl), with the place this build
// gives each of its mapped members: what a script reads and writes through
// the structure's proxy. Compiled as Norbyte's sources are
// (BoundMembers.cpp). Members the definitions hide from scripts are not
// listed.

namespace bg3se::mac {

struct BoundMember
{
    char const* Name;
    uint32_t Offset;            // offsetof in the structure that declares it
    uint32_t Size;              // sizeof the member
    int32_t Class;              // the member's own structure in BoundClasses(), or -1
    // For a std::array member: the size of one element, its structure or -1,
    // and whether it is a scalar. 0 otherwise.
    uint32_t ElementSize;
    int32_t ElementClass;
    bool ElementScalar;
    bool Scalar;                // an arithmetic type, an enumeration or a pointer
};

struct BoundClass
{
    char const* Name{ nullptr };            // as the property maps spell the type; nullptr for an unused id
    char const* EngineClass{ nullptr };     // the game's name when the structure is a component type
    uint32_t Size{ 0 };
    int32_t Parent{ -1 };                   // the structure it inherits its other members from
    uint32_t ParentOffset{ 0 };
    std::vector<BoundMember> Members;
};

// Indexed by the structure's id in the property maps.
std::span<BoundClass const> BoundClasses();

}
