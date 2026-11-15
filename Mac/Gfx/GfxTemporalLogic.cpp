#include <Gfx/GfxTemporalLogic.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <xlocale.h>

namespace bg3se::mac::gfx {

PipelineKind ClassifyFragment(char const* name)
{
    if (!name) return PipelineKind::None;
    if (!strcmp(name, "PPTAA_PS")) return PipelineKind::Taa;
    if (!strcmp(name, "LinearizeDepth")) return PipelineKind::Linearize;
    if (!strcmp(name, "VelocityBufferCamera_PS")) return PipelineKind::Velocity;
    if (!strcmp(name, "FSR_Tonemap")) return PipelineKind::FsrTonemap;
    return PipelineKind::None;
}

PipelineKind ClassifyCompute(char const* name)
{
    if (!name) return PipelineKind::None;
    if (!strcmp(name, "FSR")) return PipelineKind::Fsr;
    if (!strcmp(name, "FSR_RCAS")) return PipelineKind::Rcas;
    return PipelineKind::None;
}

char const* PipelineKindName(PipelineKind kind)
{
    switch (kind) {
    case PipelineKind::Taa: return "PPTAA_PS";
    case PipelineKind::Linearize: return "LinearizeDepth";
    case PipelineKind::Velocity: return "VelocityBufferCamera_PS";
    case PipelineKind::FsrTonemap: return "FSR_Tonemap";
    case PipelineKind::Fsr: return "FSR";
    case PipelineKind::Rcas: return "FSR_RCAS";
    case PipelineKind::None: break;
    }
    return "";
}

namespace {

// What Temporal relies on, as the game's Core.metallib has it (4.1.1.7631656).
struct Expected
{
    PipelineKind Kind;
    bool Texture;
    char const* Name;
    uint32_t Index;
    bool Written;
    uint32_t Bytes;
    MemberSeen Members[3];
};

constexpr Expected kExpected[] = {
    { PipelineKind::Taa, true, "Current", 0, false, 0, {} },
    { PipelineKind::Taa, true, "Velocity", 2, false, 0, {} },
    { PipelineKind::Taa, false, "TemporalConstants", 8, false, kTemporalConstantBytes, { { "g_Jitter", 208 } } },
    { PipelineKind::Taa, false, "Viewport", 9, false, kViewportConstantBytes,
      { { "g_ViewportScale", 0 }, { "g_ViewportOffset", 8 }, { "g_ViewportSize", 16 } } },
    { PipelineKind::Linearize, true, "_sceneDepth", 0, false, 0, {} },
    { PipelineKind::Velocity, false, "PerCamera", 11, false, kCameraConstantBytes,
      { { "g_ProjectionMat", 64 }, { "g_NearPlane", 192 }, { "g_FarPlane", 196 } } },
    { PipelineKind::FsrTonemap, true, "InputTexture", 0, false, 0, {} },
    { PipelineKind::Fsr, true, "InputTexture", 0, false, 0, {} },
    { PipelineKind::Fsr, true, "OutputTexture", 1, true, 0, {} },
    { PipelineKind::Rcas, true, "InputTexture", 0, false, 0, {} },
    { PipelineKind::Rcas, true, "OutputTexture", 2, true, 0, {} },
};

}

std::string CheckBindings(PipelineKind kind, BindingSeen const* seen, uint32_t count)
{
    for (auto const& want : kExpected) {
        if (want.Kind != kind) continue;
        BindingSeen const* found = nullptr;
        for (uint32_t i = 0; i < count && !found; i++) {
            if (seen[i].Texture == want.Texture && seen[i].Name && !strcmp(seen[i].Name, want.Name)) found = &seen[i];
        }
        std::string what = std::string(want.Texture ? "texture " : "buffer ") + want.Name;
        if (!found) return "it has no " + what;
        if (found->Index != want.Index) return what + " is at " + std::to_string(found->Index) + ", not " + std::to_string(want.Index);
        if (found->Written != want.Written) return what + (want.Written ? " is not written" : " is written");
        if (want.Texture) continue;
        if (found->Bytes != want.Bytes) return what + " is " + std::to_string(found->Bytes) + " bytes, not " + std::to_string(want.Bytes);
        for (auto const& member : want.Members) {
            if (!member.Name) break;
            MemberSeen const* at = nullptr;
            for (uint32_t i = 0; i < found->MemberCount && !at; i++) {
                if (found->Members[i].Name && !strcmp(found->Members[i].Name, member.Name)) at = &found->Members[i];
            }
            if (!at) return what + " has no " + member.Name;
            if (at->Offset != member.Offset) {
                return std::string(member.Name) + " is at " + std::to_string(at->Offset) + " of " + want.Name + ", not " + std::to_string(member.Offset);
            }
        }
    }
    return {};
}

bool TemporalParamsFrom(void const* cameraBytes, void const* temporalBytes, TemporalParams* out)
{
    float camera[kCameraConstantBytes / 4], temporal[kTemporalConstantBytes / 4];
    memcpy(camera, cameraBytes, sizeof(camera));
    memcpy(temporal, temporalBytes, sizeof(temporal));

    float nearPlane = camera[48], farPlane = camera[49];
    if (!(nearPlane > 0) || !(farPlane > nearPlane) || !std::isfinite(farPlane)) return false;

    // Depth at each plane through the projection: reversed Z puts near at 1 and far at 0.
    float nearDepth = camera[26] + camera[30] / nearPlane;
    float farDepth = camera[26] + camera[30] / farPlane;
    if (!std::isfinite(nearDepth) || !std::isfinite(farDepth)) return false;
    if (fabsf(nearDepth - 1.0f) > 0.001f || fabsf(farDepth) > 0.001f) return false;

    float fovDegrees = 2.0f * atanf(1.0f / camera[21]) * 180.0f / (float)M_PI;
    if (!std::isfinite(fovDegrees) || fovDegrees <= 0 || fovDegrees >= 180) return false;

    // BG3's constants give the sample's displacement in clip-space Y-up. MetalFX
    // wants the image's displacement in framebuffer pixels, Y down.
    float jitterX = -temporal[52], jitterY = temporal[53];
    if (!std::isfinite(jitterX) || !std::isfinite(jitterY)) return false;
    if (fabsf(jitterX) > 0.5f || fabsf(jitterY) > 0.5f) return false;

    *out = { jitterX, jitterY, nearPlane, farPlane };
    return true;
}

bool ViewportIsWhole(void const* viewport, uint32_t width, uint32_t height)
{
    float scaleOffset[4];
    uint32_t size[2];
    memcpy(scaleOffset, viewport, sizeof(scaleOffset));
    memcpy(size, static_cast<uint8_t const*>(viewport) + 16, sizeof(size));
    return scaleOffset[0] == 1 && scaleOffset[1] == 1 && scaleOffset[2] == 0 && scaleOffset[3] == 0 && size[0] == width && size[1] == height;
}

bool ScaleSupported(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH)
{
    if (!inW || !inH || outW <= inW || outH <= inH) return false;
    return outW <= (uint64_t)inW * 3 && outH <= (uint64_t)inH * 3;
}

bool SizeSettled(SizeSettle* s, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, double now, bool haveAnyScaler, bool haveThisSize)
{
    if (haveThisSize || !haveAnyScaler) return true;
    if (!s->Waiting || s->InW != inW || s->InH != inH || s->OutW != outW || s->OutH != outH) {
        *s = { inW, inH, outW, outH, now, true };
        return false;
    }
    return now - s->Since >= kSizeSettleSeconds;
}

float ParseStrength(char const* text, float fallback)
{
    if (!text || !*text) return fallback;
    char* end = nullptr;
    // In the "C" locale, whatever the game sets.
    float value = strtof_l(text, &end, LC_C_LOCALE);
    if (*end || !(value >= 0.0f) || value > 1.0f) return fallback;
    return value;
}

float Halton(unsigned index, unsigned base)
{
    float value = 0, weight = 1;
    while (index) {
        weight /= (float)base;
        value += weight * (float)(index % base);
        index /= base;
    }
    return value;
}

unsigned JitterPhases(unsigned quality)
{
    static constexpr unsigned kPhases[4] = { 14, 18, 24, 32 };  // Ultra to Performance
    return quality < 4 ? kPhases[quality] : 0;
}

uint64_t JitterSampleBits(uint32_t frameIndex, unsigned phases)
{
    unsigned i = frameIndex % phases + 1;
    float sample[2] = { Halton(i, 2), Halton(i, 3) };
    uint64_t bits;
    memcpy(&bits, sample, sizeof(bits));
    return bits;
}

}
