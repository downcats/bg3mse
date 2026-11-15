#pragma once

#include <cstdint>
#include <span>

// One virtual function of a game class as Norbyte declares it, for the check
// of Layout/VirtualSlots.h. This header is all the rows need: they are
// compiled as Norbyte's sources are (Layout/DeclaredVirtuals.cpp).

namespace bg3se::mac {

struct DeclaredVirtual
{
    char const* Class;          // Norbyte's class
    char const* Method;         // the virtual function as he names it
    uint16_t Slot;              // the slot this build calls it through
    char const* Vtable;         // a vtable of the game for that class (mangled)
    char const* Function;       // the game function that is Norbyte's `Method` (mangled)
    // For a function whose result is not one integer or pointer: the proof
    // of how it is handed back (FunctionProof::Function, Layout/GameShapes.h),
    // which must hold too.
    char const* ResultProof{ nullptr };
    // For a function of a base that is not the class's first (an interface):
    // the offset to the top of the object that the vtable's secondary table
    // for that base starts with, whose slots `Slot` counts. Zero for the
    // primary table.
    int16_t OffsetToTop{ 0 };
};

// Every virtual function the check covers, compiled as Norbyte's sources are
// (Layout/DeclaredVirtuals.cpp).
std::span<DeclaredVirtual const> DeclaredVirtuals();

}
