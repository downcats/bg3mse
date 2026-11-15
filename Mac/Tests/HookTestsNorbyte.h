#pragma once

#include <cstdint>
#include <span>

// Norbyte's hooks (CoreLib/Wrappers.h: WrappableFunction, a detour of his
// prototype that hands each call to a handler with the function to go on to),
// compiled as his sources are, behind plain functions for the hook tests.
// One probe per function of the game his code hooks directly, with his
// prototype for it: the type of his StaticSymbols member or of his
// OsirisWrappers member.

namespace bg3se::mac::test {

struct HookProbe
{
    char const* Entry;          // the engine table entry of the function hooked
    unsigned Parameters;        // of Norbyte's prototype
    // His detour is put on `function`, with a handler of the shape his own
    // have (Lua/Server/FunctorEvents.inl): it records each parameter as it
    // arrives and calls the function it is handed with them. False when the
    // detour did not go on.
    bool (*Attach)(void* function);
    void (*Detach)();
    // What the handler received, by Norbyte's parameter: the bytes of each
    // value, or the address a reference stands for, in the low bytes of a
    // word that is zero above them.
    uint64_t const* Seen;
    unsigned const* Calls;
};

std::span<HookProbe const> HookProbes();

}
