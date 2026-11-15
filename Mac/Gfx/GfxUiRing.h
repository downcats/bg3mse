#pragma once

#include <string>

// Noesis geometry rings (gfx_ui_ring_fix).
//
// BG3's Noesis render device (ns::Renderer) streams the UI's vertices through
// one 2 MB Metal buffer and its indices through one 1 MB buffer, as plain rings:
// MapVertices/MapIndices bump an offset and wrap to 0 when the request does not
// fit, with no fence against the frames the GPU is still drawing from that same
// buffer. The UI draws are recorded and executed later, so a heavy UI frame (a
// crowd of overhead numbers, status icons and vector shapes) wraps onto
// geometry in flight and every UI element draws from scrambled data.
//
// The fix swaps both buffers for ones 16 times larger (vertices 32 MB, indices
// 16 MB) and reimplements the two Map functions over the larger size.
// gfx_ui_ring_observe = 1 keeps the game's buffers and only measures.

namespace bg3se::mac {
class GameImage;
}

namespace bg3se::mac::gfx {

struct UiRingSites;

// Hook ns::Renderer::MapVertices and ::MapIndices. Empty when installed, else why not.
std::string InstallUiRing(GameImage const& image, UiRingSites const& sites, bool observe);

}
