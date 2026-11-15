#pragma once

#include <cstdint>
#include <span>

// Classes of Norbyte's definitions whose objects the game allocates one at a
// time, with the size this build gives each and the symbol that names the
// class where the game allocates it (Layout/ObjectSizes.h): its vtable, or a
// constructor. Compiled as Norbyte's sources are (BoundObjects.cpp).
//
// A class is listed once its size here has been compared with the game's.
// Most are classes that follow a base ending in padding: this compiler
// places a derived class's first members in that padding where MSVC starts
// after it, so the size says whether the game's class is laid out the same
// way.

namespace bg3se::mac {

struct BoundObject
{
    char const* Type;       // the C++ type in namespace bg3se
    char const* Witness;    // the game's symbol
    uint32_t Size;          // sizeof(Type)
};

std::span<BoundObject const> BoundObjects();

}
