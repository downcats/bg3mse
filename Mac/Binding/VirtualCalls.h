#pragma once

// For Norbyte's code, outside MSVC, where it calls virtual functions of a
// game class it reaches only through pointers the game hands out: whether the
// game's vtables were found, when the extender started, to hold every
// function of the class that this build calls in the slot it calls it
// through (Layout/VirtualSlots.h). `cls` is the class as the check names it
// (Layout/DeclaredVirtuals.cpp). False before the start-up sequence has run.
// This header is the one his files include, and includes nothing of bg3mse's.

namespace bg3se::mac::binding {

bool VirtualsConfirmed(char const* cls);

}
