#pragma once

#include <string>

// Metal-level graphics features (GfxMetal.mm); see Gfx.h for the keys.
//
// These change none of the game's code. Metal's concrete classes are private,
// so the features replace methods on the classes of real objects: the
// device's now, then each command queue's, command buffer's and encoder's
// class as its first object appears. Every replacement calls the method it
// replaced. That the game reaches Metal only through methods replaced here is
// checked on its symbol table before anything is installed (GfxMetalCalls.h).
//
// The overlay (Mac/Overlay) hooks two of the game's functions and no Metal
// method, so the two meet only in the objects: the overlay's own pass is
// encoded through the methods replaced here, on the command buffer the game
// commits last, after everything of the frame these features take part in.
//
// Research: bg3mfx, https://github.com/AndrewTriesToCode/bg3mfx (MIT,
// Copyright (c) 2026 AndrewTriesToCode). The code is written for bg3mse.

namespace bg3se::mac::gfx {

struct MetalChoices
{
    bool StallLog{ false };
    bool BindDedup{ false };
    bool ClusterGuard{ false };
    bool Temporal{ false };
    float Sharpen{ 0.3f };      // gfx_temporal_sharpen, 0 to 1
};

// What became of the choices: why no hook is on (then nothing is), or else
// why a feature that was chosen is off while the others are on.
struct MetalOutcome
{
    std::string All;
    std::string ClusterGuard;
    std::string Temporal;
};

// Hook Metal for the chosen features. Call once, before BG3 creates its Metal queue.
MetalOutcome InstallMetalHooks(MetalChoices const& choices);

// What the stall log has counted (tests).
struct StallCounts
{
    unsigned long long Presents, Commits;
    int BuffersInFlight;
};
StallCounts StallLogCounts();

// What bind dedup has seen in the encoders that have ended (tests): fragment
// texture and pipeline binds, and those of them it did not pass on.
struct BindCounts
{
    unsigned long long Seen, Dropped;
};
BindCounts BindDedupCounts();

// How many dispatches the cluster guard bound its stand-in for (tests), and
// the buffer slot it watches for a compute pipeline (an
// id<MTLComputePipelineState>), -1 when it does not watch that pipeline.
// What Temporal has done (tests): frames MetalFX scaled, frames left to
// BG3's FSR, frame paths watched and found fit to take over, and whether
// Temporal is still on.
struct TemporalCounts
{
    unsigned Scaled, LeftToFsr, RoutesProven;
    unsigned Missed;        // frames whose TAA was replaced and that MetalFX never got
    unsigned Copies;        // pipelines drawn in BG3's TAA's place: one for each TAA pipeline alive, up to 8
    bool On;
};
TemporalCounts TemporalStats();

unsigned long long ClusterGuardCount();
int ClusterGuardSlot(void* computePipeline);

}
