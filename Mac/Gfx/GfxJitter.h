#pragma once

#include <cstdint>
#include <string>

// BG3 feeds TAA a fixed 8-sample sequence. MetalFX reconstructs much more from
// a longer one, sized to the scale factor. While Temporal is on, views with an
// enabled FSR step take their sample from a Halton sequence of that length;
// every other view keeps BG3's own.
//
// Research: bg3mfx, https://github.com/AndrewTriesToCode/bg3mfx (MIT,
// Copyright (c) 2026 AndrewTriesToCode). The code is written for bg3mse.

namespace bg3se::mac {
class GameImage;
}

namespace bg3se::mac::gfx {

struct JitterSites;

// Branch BG3's three sample loads to the stubs in GfxJitterStubs.S. Call
// before BG3 starts its threads. Empty when installed, else why not.
std::string InstallJitter(GameImage const& image, JitterSites const& sites);

// Temporal has a frame path taken over (or has none, or is off for good): the samples follow.
void SetJitterActive(bool active);
bool JitterActive();

// The sample `view` gets, given the value BG3 just loaded (the stubs' callee).
uint64_t JitterSample(void* view, uint64_t original);

// What the stubs hard-wire: the register each load fills, and where each finds
// the view (x20, or the stage in x19 holding it at +8).
constexpr uint32_t kStubProjectionSample = 3, kStubCameraSample = 1, kStubVelocitySample = 0;
constexpr uint32_t kStubViewReg = 20, kStubStageReg = 19, kStubViewInStage = 8;

}
