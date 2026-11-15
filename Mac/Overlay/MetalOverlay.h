#pragma once

#include <cstdint>
#include <string>

#include <imgui.h>

#include <Overlay/ColourSpace.h>

// The Metal side of the overlay's rendering backend: Dear ImGui's own Metal
// renderer (External/imgui/backends/imgui_impl_metal.mm, as it is), given the
// game's device, and one render pass that draws a finished ImGui frame onto
// the drawable the game is about to present, on the game's own command
// buffer.
//
// The pass is the overlay's own: a new encoder on a buffer that has none
// open, loading and storing the drawable's texture. It uses nothing of the
// game's render state and leaves nothing behind in it.
//
// The game makes its command buffers with unretained references: what is
// encoded into one must be kept alive by whoever encoded it. Every pass
// therefore holds the drawable and each texture its draw commands name (the
// font atlas's among them) until the buffer has completed: a texture ImGui
// has the renderer destroy, as it does when the atlas grows, stays alive for
// the passes that were encoded with it. The renderer's
// own objects, its pipeline and depth-stencil states, are made once and
// live until the renderer is shut down, which happens only when no buffer
// with a pass of the overlay's is still to complete: a shutdown waits for
// that, and where the wait does not end it leaves the objects alive instead.
//
// The stock renderer writes texture times vertex colour straight to the
// target: right for the game's standard target (BGRA in Display P3, as
// Norbyte's backends draw on Windows), wrong for its HDR target (BT.2020 with
// the PQ curve), where the overlay is drawn into a texture of its own and put
// over the game's pixels by a pass of the overlay's that converts
// (Overlay/ColourSpace.h). That pass reads the target's pixels where it
// draws (programmable blending), which every Apple GPU can; its two pipeline
// states are made with the renderer and live as long as its other objects.
//
// The interface is plain C++; Metal objects cross it as void*, not retained
// unless it says so.

namespace bg3se::mac::overlay {

enum class TargetKind : uint8_t
{
    Standard,           // BGRA, 8 or 10 bits, in sRGB or Display P3, without extended range: drawn on as it is
    HighDynamicRange,   // BGRA, 8 or 10 bits, BT.2020 PQ with extended range: drawn on through the converting pass
    Unsupported,        // any other format, colour space or range: not drawn on
};

char const* Describe(TargetKind kind);

TargetKind ClassifyTarget(uint64_t pixelFormat, ColourSpace space, bool extendedRange);

// What a drawable is: its texture's MTLPixelFormat, its layer's colour space
// and whether the layer wants extended dynamic range content.
struct TargetFormat
{
    uint64_t PixelFormat{ 0 };
    ColourSpace Space{ ColourSpace::Unknown };
    bool ExtendedRange{ false };
    TargetKind Kind{ TargetKind::Unsupported };
};

// For the log: "MTLPixelFormat 94, BT.2020 PQ, extended range".
std::string Describe(TargetFormat const& format);

// The format of `drawable` (an id<CAMetalDrawable>); one without a layer or
// whose layer has no colour space is of an unknown colour space. Main thread.
TargetFormat TargetOf(void* drawable);
// The name of the colour space of the drawable's layer, for the log.
std::string ColourSpaceNameOf(void* drawable);

// --- on the thread that owns the ImGui context, while nothing is encoded

// Gives ImGui's Metal renderer the device, and makes the pipeline states of
// the converting pass for HDR targets. The ImGui context must exist. False
// when the renderer could not be initialised; when only the pass could not be
// made, true, and that is said in the log and by MetalDrawsHighDynamicRange.
bool MetalInitialize(void* device);
// Whether the converting pass was made: the renderer can draw on an HDR target.
bool MetalDrawsHighDynamicRange();
// The renderer's objects for the device. Once for each MetalInitialize: a
// buffer still to run uses these objects without holding them, so they are
// not made again while the renderer lives. The font atlas has no texture
// yet: the renderer makes and updates the textures a frame brings with it
// (ImDrawData::Textures) when the frame is drawn, in MetalEncode.
bool MetalCreateDeviceObjects(void* device);
// What ImGui's renderer does for one texture's request (to make it, to take
// changed pixels, to destroy it): ImGui_ImplMetal_UpdateTexture, for the
// mirrored textures of Norbyte's TextureMirrorSet, which are destroyed
// through it before the renderer is shut down. Nothing without a renderer.
void MetalUpdateTexture(ImTextureData* texture);
// The width and height a two-dimensional texture of the device has at most.
int MetalMaxTextureSize(void* device);
// Shuts ImGui's Metal renderer down once the buffers that hold a pass of the
// overlay's have completed, which it waits for. The caller has seen to it
// that no further pass is encoded. Every pass is on a buffer the game commits
// right after, so the wait is one of milliseconds; should it not end in
// `waitMilliseconds` all the same, the renderer's objects are not destroyed
// under the buffer that uses them: ImGui is parted from the renderer, whose
// objects then stay alive for good, and the result is false. True when the
// renderer was shut down or there was none. ImGui's textures are let go of
// either way: a pass holds the ones it draws with.
inline constexpr uint32_t kShutdownWaitMilliseconds = 1000;
bool MetalShutdown(uint32_t waitMilliseconds = kShutdownWaitMilliseconds);

// --- on the thread that commits the buffer, with the backend's lock held

enum class Encoded : uint8_t
{
    Drawn,
    NothingToDraw,      // the frame has no draw lists
    StaleSize,          // the frame was built for another drawable size: the next one will fit
    NotDrawable,        // the target is not one the overlay draws on, or for HDR there is no usable
                        // white or no converting pass; `target` says which target
    Failed,             // `problem` says why
};

// Draws `frame` in a render pass of its own on `commandBuffer` (an
// id<MTLCommandBuffer> that is not committed and has no encoder open), onto
// the texture of `drawable` (an id<CAMetalDrawable>). On an HDR target the
// frame is drawn into the overlay's own texture and put over the drawable's
// in a second pass, its white at `uiWhiteNits`, the white of the game's UI.
//
// The textures the frame brings (ImDrawData::Textures) are brought up to
// date first, as the stock renderer does when it draws: made, given their
// changed pixels, destroyed. A frame that is not drawn leaves them as they
// are for the next one.
Encoded MetalEncode(ImDrawData* frame, void* commandBuffer, void* drawable, float uiWhiteNits, TargetKind& target, std::string& problem);

// How many buffers holding a pass of the overlay's have not completed.
int MetalBuffersInFlight();

// --- textures

// The id<MTLTexture> the game's rf::Texture holds at `offset`, retained, with
// its size; null when there is none there, or when it is not one the
// renderer's shader can be given: a two-dimensional texture of one sample
// with values a float sampler reads, which a shader may read. (The type of
// the game's texture resource does not say: Norbyte's loader notes that it
// is set wrongly for some atlases.)
void* MetalRetainTexture(void const* gameTexture, uint32_t offset, uint32_t& width, uint32_t& height);
void MetalReleaseTexture(void* texture);

}
