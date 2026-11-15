#pragma once

#include <string>
#include <vector>

// What gfx_temporal and gfx_cluster_guard rely on in the game's shaders,
// checked on the game's own shader library on disk
// (Contents/Data/Shaders/Metal/Core.metallib): a pipeline is made from each
// function they know by name, on this machine's GPU, and its description is
// held against the same expectations the running game's pipelines are
// (GfxTemporalLogic.h, GfxMetal.mm). Nothing of the game runs.

namespace bg3se::mac::gfx {

struct ShaderCheck
{
    std::string What;       // the function, and what was read from it
    std::string Problem;    // empty when it is as expected
};

// The checks, one per function. `unavailable` says why none could be made
// here (no GPU Metal can use, or a system before macOS 13), and is empty otherwise.
std::vector<ShaderCheck> CheckGameShaders(std::string const& metallib, std::string& unavailable);

}
