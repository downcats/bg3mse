// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

#pragma once

#include <string>
#include <vector>

#include <Camera/camera_platform.h>
#include <Platform/CodeShape.h>
#include <Platform/Symbols.h>

// Where the camera tweaks attach to the game, found at start-up.
//
// Functions and globals come from the symbol table by name. A call site is
// found by searching the calling function for the call. Everything the tweaks
// know about the *inside* of the game's camera code (field offsets, which
// registers carry what) was read from a handful of functions; each of those is
// checked by shape, so the tweaks stay off when that code has changed and stay
// on when a patch has only moved it.

namespace bg3se::mac {

struct CameraFunctionCheck
{
    char const* Name;
    CodeShape Expected;
    CodeShape Found;
    bool Present{ false };
};

struct CameraSitesCheck
{
    CameraTweaksOffsets Row{};              // offsets from the image base
    uint64_t GlobalSwitchesSlotVA{ 0 };
    std::vector<CameraFunctionCheck> Functions;
    std::string Problem;                    // empty when the tweaks can be installed

    bool Ok() const { return Problem.empty(); }
};

// Runs on the loaded game and on a game binary mapped from disk alike.
CameraSitesCheck CheckCameraSites(GameImage const& image);

}
