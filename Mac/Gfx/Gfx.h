#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Graphics fixes: changes to how BG3 renders and spends CPU time, each off
// until turned on in ~/Library/Application Support/bg3mse/bg3mse.conf:
//
//   gfx_lua_jump_fix = 1      BG3's Lua uses _setjmp/_longjmp, skipping two syscalls per protected call
//   gfx_thread_idling = 1     idle workers, the job scheduler and CapFrame sleep rather than spin
//   gfx_reduced_locking = 1   the two hottest uniform allocations use a per-thread slice, not the renderer's lock
//   gfx_ao_blur_fix = 1       SSAO blurs horizontally then vertically, not vertically twice
//   gfx_ui_ring_fix = 1       BG3's Noesis vertex and index rings grow from 2 MB and 1 MB to 32 MB and 16 MB.
//                             They wrap with no fence, so a heavy UI frame overwrote geometry the GPU had
//                             not drawn yet, scrambling the whole UI in big fights.
//                             gfx_ui_ring_observe = 1 only measures
//   gfx_duplicate_shadow_fix = 1  renders the sun's shadow maps once a frame, not twice
//   gfx_improved_shadows = 1  not installed yet: its place in the game is found, its shadow programs are not written
//   gfx_bind_dedup = 1        a texture or pipeline bound again to the slot that already holds it is not sent to the driver again
//   gfx_cluster_guard = 1     a clustered-lighting kernel dispatched with no cluster buffer bound reads a zeroed
//                             stand-in, where it would have read through no buffer and wedged the GPU
//   gfx_temporal = 1          MetalFX temporal upscaling in place of BG3's TAA and FSR 1.0, when FSR is on in the
//                             game's settings. It takes over only a frame path it has first watched BG3's own
//                             FSR complete, and needs macOS 13 or later and a GPU MetalFX supports.
//                             gfx_temporal_sharpen = 0..1 sets the sharpening after it (default 0.3; 0 is none)
//   gfx_stall_log = 1         logs frames and command buffers that take over 250 ms, and a hang while it lasts
//
// Each logs "[Gfx] <key>: installed" or "[Gfx] <key>: SKIPPED (<reason>)". A
// fix whose sites cannot be found exactly as expected (Gfx/GfxSites.h)
// changes nothing; the others still install.
//
// The research behind these fixes (what to patch, instruction sequences and
// layouts) is bg3mfx's, https://github.com/AndrewTriesToCode/bg3mfx (MIT,
// Copyright (c) 2026 AndrewTriesToCode). The code is written for bg3mse.

namespace bg3se::mac {
class GameImage;
}

namespace bg3se::mac::gfx {

struct GfxSites;

// Installs the enabled fixes. Call from the dylib constructor, before BG3
// starts its threads: several fixes replace sequences of more than one of the
// game's instructions, which is only safe while no thread can be running them
// (Platform/CodeWriter.h).
void StartGfx();

// A gfx switch in bg3mse.conf (Platform/Config.h): off unless set to 1/true/yes/on.
bool FeatureEnabled(std::string_view key);

// What StartGfx does, with the image and settings passed in (tests). Each
// enabled key's outcome: an empty Problem when it installed. The sites are
// resolved once, when the first enabled key is seen, and stay available
// (ResolvedSites) to hooks that run later.
struct GfxOutcome
{
    std::string Key;
    std::string Problem;
};
struct MetalChoices;
struct MetalOutcome;
struct GfxHost
{
    std::function<bool(std::string_view)> Enabled;                          // a switch, by key
    std::function<MetalOutcome(MetalChoices const&)> InstallMetal;          // InstallMetalHooks (GfxMetal.h)
    std::function<std::optional<std::string>(std::string_view)> Value;      // any other setting
};
std::vector<GfxOutcome> StartGfx(GameImage const* image, GfxHost const& host);
std::span<char const* const> GfxKeys();
GfxSites const* ResolvedSites();

}
