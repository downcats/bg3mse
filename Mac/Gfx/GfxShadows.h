#pragma once

#include <string>

// Sun shadow fixes. Both work through BG3's vtables: no game code changes.
// Vtables live in read-only data, opened only for the single store.

namespace bg3se::mac {
class GameImage;
}

namespace bg3se::mac::gfx {

struct StagePackSites;
struct ShadowMaskSites;
struct ProgramSites;

// gfx_duplicate_shadow_fix: render the sun's shadow maps once a frame, not
// twice. Call before BG3 builds its render stages. Empty when installed, else why not.
std::string InstallDuplicateShadowFix(GameImage const& image, StagePackSites const& sites);

// gfx_improved_shadows: the sun's shadow mask drawn by programs of bg3mse's
// own, which also read the surface normals (the G-buffer's, at a texture slot
// the mask stage leaves free). Where that goes in the game is found and
// checked (ShadowMaskSites, ProgramSites): the mask stage's Load, Unload and
// StartRecording slots, its table of programs, and the functions that make a
// program from a .bshd and bind a texture. The programs themselves are not
// written: a shadow filter cannot be judged without seeing it in the game, and
// none is installed on a guess. So the key changes nothing and says why: what
// the sites say when one is not found, else that the programs are missing.
std::string InstallImprovedShadows(ShadowMaskSites const& mask, ProgramSites const& programs);

}
