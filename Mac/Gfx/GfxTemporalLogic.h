#pragma once

#include <cstdint>
#include <string>

// Pure decisions behind the Temporal upscaler and its jitter: what each
// pipeline is, whether BG3's constants describe a frame MetalFX can scale, and
// which sample a view should use. No Metal and no game, so the tests check them.
//
// The research behind gfx_temporal (BG3's passes, constant layouts and jitter)
// is bg3mfx's, https://github.com/AndrewTriesToCode/bg3mfx (MIT, Copyright (c)
// 2026 AndrewTriesToCode). The code is written for bg3mse.

namespace bg3se::mac::gfx {

enum class PipelineKind : uint8_t {
    None = 0,
    Taa,            // PPTAA_PS: colour at texture 0, motion at 2
    Linearize,      // LinearizeDepth: the scene's depth at texture 0
    Velocity,       // VelocityBufferCamera_PS: the camera constants
    FsrTonemap,     // FSR_Tonemap: the last pass whose input is linear HDR; FSR reads its target
    Fsr,            // compute: source at texture 0, scaled at 1
    Rcas,           // compute: scaled at texture 0, destination at 2
};

PipelineKind ClassifyFragment(char const* name);
PipelineKind ClassifyCompute(char const* name);
char const* PipelineKindName(PipelineKind kind);

// What a pipeline's own description says of one of its function's arguments,
// and what Temporal relies on of each kind of pipeline: checked when the game
// makes the pipeline, so a shader that is not the one this was written against
// turns Temporal off instead of being fed the wrong textures.
struct MemberSeen
{
    char const* Name;
    uint32_t Offset;
};
struct BindingSeen
{
    bool Texture;           // else a buffer
    char const* Name;
    uint32_t Index;
    bool Written;
    uint32_t Bytes;         // a buffer's size
    MemberSeen const* Members;
    uint32_t MemberCount;
};
// Empty when `seen` has everything Temporal relies on for `kind`, else what is missing or elsewhere.
std::string CheckBindings(PipelineKind kind, BindingSeen const* seen, uint32_t count);

// What the scaler needs to know about one frame.
struct TemporalParams
{
    float JitterX, JitterY;
    float NearPlane, FarPlane;
};

constexpr unsigned kCameraConstantBytes = 224;
constexpr unsigned kTemporalConstantBytes = 224;
constexpr unsigned kViewportConstantBytes = 168;

// Read and validate the camera (fragment slot 11) and temporal (slot 8)
// constants. False when the projection is not the reversed-Z one MetalFX is
// being told about, or the jitter is not a sub-pixel offset.
bool TemporalParamsFrom(void const* camera, void const* temporal, TemporalParams* out);

// True when the viewport constants (slot 9) say the whole texture is rendered.
bool ViewportIsWhole(void const* viewport, uint32_t width, uint32_t height);

// MetalFX scales up, by at most 3x on either axis.
bool ScaleSupported(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH);

// A new scaler is only worth making once a size has held for a moment: on
// macOS 27.0.1 scalers with outputs up to 1512 tall are never freed, and every
// window-drag size would leak one.
constexpr double kSizeSettleSeconds = 0.3;
struct SizeSettle
{
    uint32_t InW, InH, OutW, OutH;
    double Since;
    bool Waiting;
};
bool SizeSettled(SizeSettle* state, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, double now, bool haveAnyScaler,
    bool haveThisSize);

// A strength in 0..1 from text; `fallback` when it is not a number in range.
float ParseStrength(char const* text, float fallback);

// Native jitter: BG3 cycles 8 samples whatever it renders at. MetalFX needs
// about eight per output pixel, so the count follows the FSR quality.
float Halton(unsigned index, unsigned base);
// Sample count for BG3's FSR quality byte; 0 when the value is not known.
unsigned JitterPhases(unsigned quality);
// The two floats, 0..1, as BG3's own table stores them, packed like a load of it.
uint64_t JitterSampleBits(uint32_t frameIndex, unsigned phases);

}
