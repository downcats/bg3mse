#pragma once

#include <cstdint>
#include <string>

// The overlay's colours on the game's output.
//
// Dear ImGui's colours are sRGB values, and Norbyte's backends write them as
// they are onto whatever swap chain the game has, an HDR one included. The
// Mac game's layer is BGRA, 8 or 10 bits, in Display P3 for standard dynamic
// range; there the overlay is drawn as on Windows, its values as they are.
//
// For HDR output -[LSMTLView updateHDRState:] asks the layer for extended
// range and gives it the colour space kCGColorSpaceITUR_2020_PQ_EOTF: BT.2020
// primaries, encoded with the PQ curve of SMPTE ST 2084, in which a value
// stands for an absolute luminance up to 10000 nits. The same values there
// would be far too bright and of the wrong hue, and blending them would be
// blending PQ codes. So in HDR the overlay is drawn by ImGui's renderer as on
// a standard target, into a texture of its own cleared to nothing, which then
// holds the overlay's colour premultiplied by its coverage, and that texture
// is put over the game's pixels in a pass that converts: each of the game's
// pixels is taken into the overlay's terms (sRGB primaries, sRGB-encoded,
// 1.0 = the white of the game's UI, beyond 1.0 for what is brighter), the
// overlay is put over it there exactly as it would have been drawn over a
// standard target, and the result goes back to BT.2020 and PQ. Where the
// overlay covers nothing the game's pixel is left as it was.
//
// The white is the one the game's own UI has. The game puts its UI over the
// frame with the fragment shader CombineUI_HDR (Data/Shaders/Metal/
// Core.metallib), which reads one of the four floats it is given, the first,
// UIPaperWhite: it decodes the UI's sRGB colour with the sRGB curve, takes it
// from BT.709 to BT.2020 primaries (ITU-R BT.2087, to four places), multiplies
// it by UIPaperWhite and PQ-encodes it over 10000 nits. An opaque UI colour
// is therefore what the pass below makes of the same overlay colour when its
// white is UIPaperWhite. ls::CombineUIRenderStage::ExecuteWTKernel() gives
// it a constant, 300 in HDR on the build read, which the overlay reads from
// that code (Engine/Entries/Overlay.cpp). The setting "HDRPaperWhite" is
// given too, as the fourth float, and that shader does not read it.
//
// Where the UI is partly transparent the game blends in linear light (its UI
// texture premultiplied, the frame under it attenuated by (1 - alpha) twice);
// the overlay blends as ImGui does on a standard target, sRGB-encoded, so
// that its translucent colours look as they do in SDR.
//
// The functions here are the pass's arithmetic in C++, for the tests; the
// pass's shader is made from the same constants.

namespace bg3se::mac::overlay {

enum class ColourSpace : uint8_t
{
    Unknown,            // none, or one the overlay has no conversion for
    SRGB,
    DisplayP3,
    Bt2020Pq,           // BT.2020 primaries, the PQ transfer function
};

char const* Describe(ColourSpace space);

// SMPTE ST 2084.
inline constexpr double kPqM1 = 2610.0 / 16384.0;
inline constexpr double kPqM2 = 2523.0 / 4096.0 * 128.0;
inline constexpr double kPqC1 = 3424.0 / 4096.0;
inline constexpr double kPqC2 = 2413.0 / 4096.0 * 32.0;
inline constexpr double kPqC3 = 2392.0 / 4096.0 * 32.0;
// The luminance PQ's 1.0 stands for.
inline constexpr double kPqPeakNits = 10000.0;

// Linear BT.709 (sRGB) primaries to linear BT.2020 (ITU-R BT.2087), and back.
inline constexpr double kBt709ToBt2020[3][3] = {
    { 0.627403895934699, 0.329283038377884, 0.043313065687417 },
    { 0.069097289358232, 0.919540395075459, 0.011362315566309 },
    { 0.016391438875150, 0.088013307877226, 0.895595253247624 },
};
inline constexpr double kBt2020ToBt709[3][3] = {
    { 1.660491002108435, -0.587641138788550, -0.072849863319885 },
    { -0.124550474521591, 1.132899897125960, -0.008349422604369 },
    { -0.018150763354905, -0.100578898008007, 1.118729661362913 },
};

// Whether a white (in nits) is one a display can show: more than nothing, at
// most PQ's peak.
bool UsablePaperWhite(float nits);

struct Rgb
{
    double R{ 0 }, G{ 0 }, B{ 0 };
};

// The sRGB curve, extended to values beyond [0, 1] by its mirror and its
// continuation, as an extended-range sRGB colour space does.
double SrgbToLinear(double encoded);
double LinearToSrgb(double linear);
// PQ: `luminance` 1.0 is 10000 nits; values outside [0, 1] are clamped.
double PqEncode(double luminance);
double PqDecode(double encoded);
Rgb Multiply(double const (&matrix)[3][3], Rgb colour);

// One pixel of the pass: `game` is the game's PQ-encoded value, `overlay` the
// overlay's premultiplied sRGB-encoded colour and `coverage` its alpha.
Rgb CompositeOverPq(Rgb game, Rgb overlay, double coverage, double whiteNits);

// The pass's Metal source: a vertex function "composite_vertex" for a
// triangle that covers the target (three vertices, no buffers), and a
// fragment function "composite_fragment" that reads the target's pixel
// ([[color(0)]]), the overlay's texture (texture 0) and the white over PQ's
// peak (one float, buffer 0).
std::string CompositeShaderSource();

}
