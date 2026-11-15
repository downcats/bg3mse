#pragma once

// A libbg3mse.dylib built with the graphics module (BG3MSE_GFX, Mac/Gfx)
// starts it when it loads, and says so in its arm64 code: the section
// __TEXT,__bg3mse_gfx holds BG3MSE_GFX_MARK, put there by the lines of
// Mac/Main.cpp that start the module. bg3mse-install offers MetalFX
// upscaling (gfx_temporal in bg3mse.conf) only for a libbg3mse.dylib that
// carries the mark (Mac/Installer/Extras.h, Updater/Stamp.h); a build made
// before there was a mark does not carry it, and is not offered it either.
// For C (the installer tests' stand-ins) and C++ alike.

#define BG3MSE_GFX_SEGMENT "__TEXT"
#define BG3MSE_GFX_SECTION "__bg3mse_gfx"
#define BG3MSE_GFX_MARK "bg3mse graphics module: gfx_temporal"
