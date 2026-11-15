#include <Overlay/ColourSpace.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <xlocale.h>

namespace bg3se::mac::overlay {

char const* Describe(ColourSpace space)
{
    switch (space) {
    case ColourSpace::SRGB: return "sRGB";
    case ColourSpace::DisplayP3: return "Display P3";
    case ColourSpace::Bt2020Pq: return "BT.2020 PQ";
    default: return "an unknown colour space";
    }
}

bool UsablePaperWhite(float nits)
{
    return std::isfinite(nits) && nits > 0.0f && nits <= (float)kPqPeakNits;
}

double SrgbToLinear(double encoded)
{
    double a = std::fabs(encoded);
    double linear = a <= 0.04045 ? a / 12.92 : std::pow((a + 0.055) / 1.055, 2.4);
    return std::copysign(linear, encoded);
}

double LinearToSrgb(double linear)
{
    double a = std::fabs(linear);
    double encoded = a <= 0.0031308 ? a * 12.92 : 1.055 * std::pow(a, 1.0 / 2.4) - 0.055;
    return std::copysign(encoded, linear);
}

double PqEncode(double luminance)
{
    double p = std::pow(std::clamp(luminance, 0.0, 1.0), kPqM1);
    return std::pow((kPqC1 + kPqC2 * p) / (1.0 + kPqC3 * p), kPqM2);
}

double PqDecode(double encoded)
{
    double p = std::pow(std::clamp(encoded, 0.0, 1.0), 1.0 / kPqM2);
    return std::pow(std::max(p - kPqC1, 0.0) / (kPqC2 - kPqC3 * p), 1.0 / kPqM1);
}

Rgb Multiply(double const (&m)[3][3], Rgb c)
{
    return {
        m[0][0] * c.R + m[0][1] * c.G + m[0][2] * c.B,
        m[1][0] * c.R + m[1][1] * c.G + m[1][2] * c.B,
        m[2][0] * c.R + m[2][1] * c.G + m[2][2] * c.B,
    };
}

Rgb CompositeOverPq(Rgb game, Rgb overlay, double coverage, double whiteNits)
{
    if (coverage <= 0.0) return game;
    double white = whiteNits / kPqPeakNits;
    // The game's pixel in the overlay's terms.
    Rgb linear = Multiply(kBt2020ToBt709, { PqDecode(game.R) / white, PqDecode(game.G) / white, PqDecode(game.B) / white });
    Rgb under{ LinearToSrgb(linear.R), LinearToSrgb(linear.G), LinearToSrgb(linear.B) };
    // The overlay over it, as a standard target blends it.
    Rgb blended{ overlay.R + (1.0 - coverage) * under.R, overlay.G + (1.0 - coverage) * under.G, overlay.B + (1.0 - coverage) * under.B };
    Rgb out = Multiply(kBt709ToBt2020, { SrgbToLinear(blended.R), SrgbToLinear(blended.G), SrgbToLinear(blended.B) });
    return { PqEncode(out.R * white), PqEncode(out.G * white), PqEncode(out.B * white) };
}

std::string CompositeShaderSource()
{
    auto matrix = [](double const (&m)[3][3]) {
        // Metal's matrices are made of columns.
        char text[512];
        snprintf_l(text, sizeof(text), LC_C_LOCALE, "float3x3(float3(%.9g, %.9g, %.9g), float3(%.9g, %.9g, %.9g), float3(%.9g, %.9g, %.9g))",
            m[0][0], m[1][0], m[2][0], m[0][1], m[1][1], m[2][1], m[0][2], m[1][2], m[2][2]);
        return std::string(text);
    };
    char constants[512];
    // In the "C" locale: a decimal comma is no Metal.
    snprintf_l(constants, sizeof(constants), LC_C_LOCALE,
        "constant float kM1 = %.9g, kM2 = %.9g, kC1 = %.9g, kC2 = %.9g, kC3 = %.9g;\n",
        kPqM1, kPqM2, kPqC1, kPqC2, kPqC3);

    std::string source = R"(#include <metal_stdlib>
using namespace metal;
)";
    source += constants;
    source += "constant float3x3 kToBt2020 = " + matrix(kBt709ToBt2020) + ";\n";
    source += "constant float3x3 kToBt709 = " + matrix(kBt2020ToBt709) + ";\n";
    source += R"(
static float3 srgb_to_linear(float3 c)
{
    float3 a = abs(c);
    float3 linear = select(precise::pow((a + 0.055f) / 1.055f, float3(2.4f)), a / 12.92f, a <= 0.04045f);
    return copysign(linear, c);
}

static float3 linear_to_srgb(float3 l)
{
    float3 a = abs(l);
    float3 encoded = select(1.055f * precise::pow(a, float3(1.0f / 2.4f)) - 0.055f, a * 12.92f, a <= 0.0031308f);
    return copysign(encoded, l);
}

static float3 pq_encode(float3 y)
{
    float3 p = precise::pow(clamp(y, 0.0f, 1.0f), float3(kM1));
    return precise::pow((kC1 + kC2 * p) / (1.0f + kC3 * p), float3(kM2));
}

static float3 pq_decode(float3 e)
{
    float3 p = precise::pow(clamp(e, 0.0f, 1.0f), float3(1.0f / kM2));
    return precise::pow(max(p - kC1, 0.0f) / (kC2 - kC3 * p), float3(1.0f / kM1));
}

vertex float4 composite_vertex(uint id [[vertex_id]])
{
    float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * 2.0f - 1.0f, 0.0f, 1.0f);
}

fragment float4 composite_fragment(float4 position [[position]], float4 game [[color(0)]],
                                   texture2d<float, access::read> overlay [[texture(0)]],
                                   constant float& white [[buffer(0)]])
{
    float4 over = overlay.read(uint2(position.xy));
    if (over.a <= 0.0f) return game;
    float3 under = linear_to_srgb(kToBt709 * (pq_decode(game.rgb) / white));
    float3 blended = over.rgb + (1.0f - over.a) * under;
    return float4(pq_encode((kToBt2020 * srgb_to_linear(blended)) * white), game.a);
}
)";
    return source;
}

}
