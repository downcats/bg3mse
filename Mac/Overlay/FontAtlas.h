#pragma once

#include <imgui.h>

// ImGui's font atlas, kept within what a Metal texture can be.
//
// ImGui rasterises a glyph when it is first drawn and packs it into the
// atlas, whose texture grows by doubling one side at a time (and is made
// again, the old one destroyed a frame later) until it is as large as the
// atlas allows: 8192 pixels each way unless that is changed. At that size
// ImGui drops the glyphs of sizes no longer in use and packs again; what
// still finds no room is not drawn.
//
// A Metal texture is 16384 pixels at most each way, 8192 on the oldest GPUs
// that have Metal, and one over that cannot be made at all. The atlas is
// therefore told the device's limit before its first texture is made: it
// then never asks the renderer for a texture the device does not have.

namespace bg3se::mac::overlay {

// The smallest limit an atlas is bounded to: ImGui's own smallest texture.
inline constexpr int kLeastAtlasLimit = 512;

// Lets `atlas` grow no further than `limit` pixels each way: its largest
// width and height become the largest power of two within the limit, where
// they were more. The side the atlas may now reach at most, or 0 when the
// limit is less than an atlas can be bounded to (nothing is changed then).
int BoundFontAtlas(ImFontAtlas& atlas, int limit);

}
