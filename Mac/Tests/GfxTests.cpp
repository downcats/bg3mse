// Tests for the graphics fixes' instruction words and decisions (Gfx/GfxPatch).
// No game needed. The live words below are the game's own at each site in
// build 4.1.1.7631656 (arm64), and the expected outputs are the exact words
// each patch is known to write there, so a change in what any patch emits
// fails here.
// That the sites are found in a game binary is `bg3mse-shaders check`'s job.

#include <stdafx.h>
#include <Gfx/Gfx.h>
#include <clocale>
#include <Gfx/GfxJitter.h>
#include <Gfx/GfxMetal.h>
#include <Gfx/GfxMetalCalls.h>
#include <Gfx/GfxPatch.h>
#include <Gfx/GfxShadows.h>
#include <Gfx/GfxSites.h>
#include <Gfx/GfxStall.h>
#include <Gfx/GfxTemporalLogic.h>
#include <Platform/Log.h>
#include <Platform/Symbols.h>

#include "FakeBinary.h"

using namespace bg3se::mac::gfx;
using bg3se::mac::GameImage;

// The tests on Metal's own objects (GfxMetalTests.mm).
void RunGfxMetalTests(int* tests, int* failures);

namespace {

int gFailures = 0;
int gTests = 0;
char const* gTest = "";

#define CHECK(cond) do { if (!(cond)) { printf("FAIL gfx %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; gTests++; name(); } static void name()

// The patch sites in build 4.1.1.7631656, and the bottom and top of that
// build's header padding (its lowest and highest 16-byte block), where the islands go.
constexpr uint64_t kLongjmpCall = 0x100769e8c, kWorkerBudget = 0x10655389c, kCapIdleYield = 0x1064d1e94;
constexpr uint64_t kBindAlloc = 0x105d07dc0, kBindResume = 0x105d07ffc, kSetupAlloc = 0x1060e0174, kSetupResume = 0x1060e01a8;
constexpr uint64_t kFrameReset = 0x10645da54, kAoBlurBind = 0x1061086c8, kJitterCameraLoad = 0x105f8af84;
constexpr uint64_t kPaddingBottom = 0x100001c80, kPaddingTop = 0x10000dc40;

constexpr uint32_t kBindLive[kBindAllocLiveWords] = {
    0xf9401c39, 0x91002334, 0xaa1403e0, 0x946b1509, 0xf9504b28, 0x4b0803e9, 0x92400d29,
    0x8b080138, 0x8b150308, 0xf9104b28, 0xaa1403e0, 0x946b1504, 0xf9406fe1, 0x14000082,
};
constexpr uint32_t kSetupLive[kSetupAllocLiveWords] = {
    0xf9500277, 0x9107e2fb, 0xaa1b03e0, 0x945bb41c, 0xf95142e8, 0x4b0803e9, 0x92400d29,
    0x8b08013c, 0x8b180388, 0xf91142e8, 0xaa1303f7, 0xaa1b03e0, 0x945bb416,
};
constexpr uint32_t kAoBlurLive[kAoBlurBindWords] = {
    0x91002260, 0x52800115, 0x52800501, 0x52800102, 0x940e7a50, 0xf9000414, 0x9000bb48,
    0x3dc2e500, 0x3d800400, 0x52801fe8, 0x39008008, 0x39000015, 0x1400000e,
};

TEST(branch_encoding_and_range)
{
    // The game's own bl pthread_yield_np at CapFrame's idle step.
    CHECK(EncodeBl(kCapIdleYield, kCapIdleYield + 4 * 0x4bed7fll) == 0x944bed7fu);
    CHECK(EncodeB(0x1000, 0x1004) == 0x14000001u);
    CHECK(EncodeB(0x1004, 0x1000) == 0x17ffffffu);
    CHECK(EncodeBl(0x1000, 0x1002) == 0u);
    CHECK(EncodeBl(0, 128ull * 1024 * 1024) == 0u);
    CHECK(EncodeBl(0, 128ull * 1024 * 1024 - 4) != 0u);
}

TEST(header_padding_reaches_every_site)
{
    for (uint64_t site : { kLongjmpCall, kWorkerBudget, kCapIdleYield, kBindAlloc + 4 * 4, kSetupAlloc + 3 * 4, kFrameReset, kAoBlurBind + 5 * 4, kJitterCameraLoad }) {
        CHECK(EncodeBl(site, kPaddingBottom) != 0 && EncodeBl(site, kPaddingTop) != 0);
    }
}

TEST(multi_word_sites_must_lie_on_one_page)
{
    CHECK(OnOnePage(0x100003ff8, 2, 0x4000));
    CHECK(!OnOnePage(0x100003ffc, 2, 0x4000));
    CHECK(OnOnePage(0x100004000, 13, 0x4000));
    CHECK(!OnOnePage(0x100003fd0, 13, 0x4000));
    CHECK(!OnOnePage(0x100004000, 0, 0x4000));
    // The sites in build 4.1.1.7631656: each sequence on one 16 KB page.
    CHECK(OnOnePage(kBindAlloc, kBindAllocWords, 0x4000) && OnOnePage(kSetupAlloc, kSetupAllocWords, 0x4000) &&
          OnOnePage(kAoBlurBind, kAoBlurBindWords, 0x4000));
}

TEST(far_jump_words)
{
    uint32_t w[kFarJumpWords];
    WriteFarJump(w, 0x1122334455667788ull);
    CHECK(w[0] == 0x58000050u);
    CHECK(w[1] == 0xd61f0200u);
    CHECK(w[2] == 0x55667788u);
    CHECK(w[3] == 0x11223344u);
}

TEST(idle_words_match_the_expected_words)
{
    CHECK(ZeroInsteadOfLoad(0xf9401669) == 0xd2800009u);      // ldr x9, [x19, #0x28] -> mov x9, #0
    uint32_t window[2];
    ZeroInsteadOfConstant(window, 0x529150e9);                 // mov w9, #0x8a87 -> mov w9, #0; nop
    CHECK(window[0] == 0x52800009u && window[1] == 0xd503201fu);

    uint32_t w[kCapIdleIslandWords];
    WriteCapIdleIsland(w, 19, 27, 0x1000);
    CHECK(w[0] == 0xaa1303e0u);    // mov x0, x19
    CHECK(w[1] == 0xaa1b03e1u);    // mov x1, x27
    CHECK(w[2] == 0x58000050u);
    CHECK(w[4] == 0x1000u && w[5] == 0u);
}

TEST(cap_idle_steps_then_sleeps_to_margin_then_yields)
{
    constexpr uint64_t kPoll = 250, kTail = 1000, kMargin = 200;
    uint64_t wake = 0;
    // Far from the deadline: one poll.
    CHECK(CapIdleStep(1000, 11000, true, kPoll, kTail, kMargin, &wake) == IdleAction::Sleep);
    CHECK(wake == 1250u);
    // The next poll would land in CapFrame's spin: sleep to just before it ends.
    CHECK(CapIdleStep(10000, 11000, true, kPoll, kTail, kMargin, &wake) == IdleAction::Sleep);
    CHECK(wake == 10800u);
    // Inside the margin, or past the deadline: BG3's yield.
    CHECK(CapIdleStep(10900, 11000, true, kPoll, kTail, kMargin, &wake) == IdleAction::Yield);
    CHECK(CapIdleStep(20000, 11000, true, kPoll, kTail, kMargin, &wake) == IdleAction::Yield);
    // A clock that has not started has no deadline.
    CHECK(CapIdleStep(5000, 0, false, kPoll, kTail, kMargin, &wake) == IdleAction::Sleep);
    CHECK(wake == 5250u);
}

TEST(slice_alignment_refill_and_retire)
{
    int rendererA, rendererB;
    UniformSlice s;
    uint64_t at = 0;
    // A fresh thread has no slice.
    CHECK(!SliceTake(s, &rendererA, 0, 16, &at));

    s = { &rendererA, 3, 0x100, 0x200 };
    CHECK(SliceTake(s, &rendererA, 3, 0x11, &at) && at == 0x100u);
    CHECK(SliceTake(s, &rendererA, 3, 0x10, &at) && at == 0x120u);   // aligned up from 0x111
    // Exactly fills it, then nothing is left.
    CHECK(SliceTake(s, &rendererA, 3, 0xd0, &at) && at == 0x130u);
    CHECK(!SliceTake(s, &rendererA, 3, 1, &at));

    // A new frame or another renderer never reuses the slice.
    s = { &rendererA, 3, 0x100, 0x200 };
    CHECK(!SliceTake(s, &rendererA, 4, 16, &at));
    CHECK(!SliceTake(s, &rendererB, 3, 16, &at));
    CHECK(s.Next == 0x100u);

    CHECK(SliceReserveBytes(16384, 64) == 16384u);
    CHECK(SliceReserveBytes(16384, 20001) == 20016u);
}

TEST(bind_and_setup_replacements_match_the_expected_words)
{
    uint32_t bind[kBindAllocWords], setup[kSetupAllocWords];
    const uint64_t island = 0x10000b000;
    CHECK(WriteBindAlloc(bind, kBindLive, kBindAlloc, island, kBindResume));
    uint32_t const expectedBind[kBindAllocWords] = {
        0xf9401c39, 0x91002334, 0xaa1903e0, 0xaa1503e1, EncodeBl(kBindAlloc + 16, island), 0xaa0003f8, 0xf9406fe1, EncodeB(kBindAlloc + 28, kBindResume),
    };
    CHECK(memcmp(bind, expectedBind, sizeof(bind)) == 0);
    // BG3's own branch out of the block lands at the same place.
    CHECK(kBindAlloc + 13 * 4 + 4 * (uint64_t)(kBindLive[13] & 0x03ffffffu) == kBindResume);

    CHECK(WriteSetupAlloc(setup, kSetupLive, kSetupAlloc, island, kSetupResume, 0x1f0));
    uint32_t const oldSetup[kSetupAllocWords] = {
        0xf9500268, 0x9107c100, 0xaa1803e1, EncodeBl(kSetupAlloc + 12, island), 0xaa0003fc, 0xaa1303f7, EncodeB(kSetupAlloc + 24, kSetupResume),
    };
    CHECK(memcmp(setup, oldSetup, sizeof(setup)) == 0);
    CHECK(kSetupAlloc + 13 * 4 == kSetupResume);

    CHECK(!WriteBindAlloc(bind, kBindLive, kBindAlloc, 0x200000000, kBindResume));
    CHECK(!WriteSetupAlloc(setup, kSetupLive, kSetupAlloc, 0x200000000, kSetupResume, 0x1f0));
    CHECK(!WriteSetupAlloc(setup, kSetupLive, kSetupAlloc, island, kSetupResume, 0x1000));   // not an add immediate
}

TEST(frame_reset_island_keeps_bg3s_store)
{
    uint32_t w[kFrameResetIslandWords];
    WriteFrameResetIsland(w, 0x3d882680, 0x1122334455667788ull);
    CHECK(w[0] == 0x3d882680u);
    // ldr x16, <literal> reads the two words at the end: word 1 + imm19 == 6.
    CHECK(((w[1] >> 5) & 0x7ffffu) == 5u);
    CHECK((w[1] & 0x1fu) == 16u);
    CHECK(w[2] == 0xf9400211u && w[3] == 0x91000631u && w[4] == 0xc89ffe11u);
    CHECK(w[5] == 0xd65f03c0u);
    CHECK(w[6] == 0x55667788u && w[7] == 0x11223344u);
}

TEST(ao_blur_changes_only_kind_and_store)
{
    uint32_t w[kAoBlurBindWords];
    CHECK(WriteAoBlurBind(w, kAoBlurLive, kAoBlurBind, 0x10000b000, 7));
    for (int i = 0; i < kAoBlurBindWords; i++) {
        if (i != 1 && i != 5) CHECK(w[i] == kAoBlurLive[i]);
    }
    CHECK(w[1] == 0x528000f5u);                                        // mov w21, #7
    CHECK(w[5] == EncodeBl(kAoBlurBind + 20, 0x10000b000));
    CHECK(!WriteAoBlurBind(w, kAoBlurLive, kAoBlurBind, 0x200000000, 7));

    // From the function's own ring update and the bind's store, exactly the expected island.
    uint32_t island[kAoBlurIslandWords];
    WriteAoBlurIsland(island, 0xb9402a88, 0x12000508, 0xf8685a81, kAoBlurLive[5]);
    uint32_t const expectedIsland[kAoBlurIslandWords] = { 0xb9402a89, 0x12000529, 0xf8695a89, 0xf9000409, 0xd65f03c0 };
    CHECK(memcmp(island, expectedIsland, sizeof(island)) == 0);
}

TEST(jitter_island_shape)
{
    uint32_t w[kJitterIslandWords];
    const uint64_t island = 0x10000b000, stub = 0x1122334455667788ull;
    CHECK(WriteJitterIsland(w, island, stub, kJitterCameraLoad + 4));
    // adr x17 lands on the ldp, word 7: from word 1 that is 24 bytes.
    CHECK(w[1] == 0x100000d1u);
    // ldr x16 reads the literal at word 5: from word 2 that is 12 bytes.
    CHECK(((w[2] >> 5) & 0x7ffffu) == 3u);
    CHECK(w[5] == 0x55667788u && w[6] == 0x11223344u);
    CHECK(w[0] == 0xa9bf47f0u && w[7] == 0xa8c147f0u);
    CHECK(w[8] == EncodeB(island + 8 * 4, kJitterCameraLoad + 4));
    CHECK(!WriteJitterIsland(w, 0x300000000, stub, kJitterCameraLoad + 4));
}


// --- Temporal: the pure decisions -----------------------------------------------------

TEST(pipeline_classification)
{
    CHECK(ClassifyFragment("PPTAA_PS") == PipelineKind::Taa);
    CHECK(ClassifyFragment("LinearizeDepth") == PipelineKind::Linearize);
    CHECK(ClassifyFragment("VelocityBufferCamera_PS") == PipelineKind::Velocity);
    CHECK(ClassifyFragment("FSR_Tonemap") == PipelineKind::FsrTonemap);
    // Passes Temporal does not need to know: the frame is not followed through them.
    CHECK(ClassifyFragment("PPBloomUpsampleBlurAndCombine_PS") == PipelineKind::None);
    CHECK(ClassifyFragment("PPColorGradingApplyLUTPS") == PipelineKind::None);
    CHECK(ClassifyFragment("PPTAA_PS2") == PipelineKind::None);
    CHECK(ClassifyFragment(nullptr) == PipelineKind::None);
    CHECK(ClassifyCompute("FSR") == PipelineKind::Fsr);
    CHECK(ClassifyCompute("FSR_RCAS") == PipelineKind::Rcas);
    CHECK(ClassifyCompute("CAS") == PipelineKind::None);
    // The variants of the two kernels for GPUs without 16-bit floats bind their own way: not taken over.
    CHECK(ClassifyCompute("FSR_SLOW") == PipelineKind::None && ClassifyCompute("FSR_RCAS_SLOW") == PipelineKind::None);
    for (auto kind : { PipelineKind::Taa, PipelineKind::Linearize, PipelineKind::Velocity, PipelineKind::FsrTonemap }) {
        CHECK(ClassifyFragment(PipelineKindName(kind)) == kind);
    }
    CHECK(ClassifyCompute(PipelineKindName(PipelineKind::Fsr)) == PipelineKind::Fsr);
}

TEST(pipeline_bindings_must_be_the_ones_temporal_reads)
{
    // PPTAA_PS as the game's Core.metallib describes it (4.1.1.7631656).
    MemberSeen const temporal[] = { { "g_ViewMat_prev", 0 }, { "g_ProjectionMat_prev", 64 }, { "g_Jitter", 208 }, { "g_PreviousJitter", 216 } };
    MemberSeen const viewport[] = { { "g_ViewportScale", 0 }, { "g_ViewportOffset", 8 }, { "g_ViewportSize", 16 }, { "g_ViewportMin", 24 } };
    std::vector<BindingSeen> taa = {
        { true, "Current", 0, false, 0, nullptr, 0 }, { true, "History", 1, false, 0, nullptr, 0 }, { true, "Velocity", 2, false, 0, nullptr, 0 },
        { false, "TAAProperties", 0, false, 32, nullptr, 0 }, { false, "TemporalConstants", 8, false, 224, temporal, 4 },
        { false, "Viewport", 9, false, 168, viewport, 4 }, { false, "PerRT", 10, false, 16, nullptr, 0 },
    };
    CHECK(CheckBindings(PipelineKind::Taa, taa.data(), (uint32_t)taa.size()).empty());

    // Each thing Temporal reads, moved or gone: refused, naming it.
    auto broken = [&](size_t at, auto&& change, char const* names) {
        auto copy = taa;
        change(copy[at]);
        auto problem = CheckBindings(PipelineKind::Taa, copy.data(), (uint32_t)copy.size());
        CHECK(problem.find(names) != std::string::npos);
    };
    broken(2, [](BindingSeen& b) { b.Index = 3; }, "Velocity");
    broken(0, [](BindingSeen& b) { b.Name = "Colour"; }, "Current");
    broken(4, [](BindingSeen& b) { b.Bytes = 240; }, "TemporalConstants");
    broken(4, [](BindingSeen& b) { b.Index = 7; }, "TemporalConstants");
    broken(5, [](BindingSeen& b) { b.MemberCount = 2; }, "g_ViewportSize");
    MemberSeen const moved[] = { { "g_Jitter", 216 } };
    broken(4, [&](BindingSeen& b) { b.Members = moved; b.MemberCount = 1; }, "g_Jitter");
    // A buffer of the name is not the texture.
    broken(0, [](BindingSeen& b) { b.Texture = false; }, "Current");

    MemberSeen const camera[] = { { "g_ViewMat", 0 }, { "g_ProjectionMat", 64 }, { "g_NearPlane", 192 }, { "g_FarPlane", 196 } };
    BindingSeen const velocity[] = { { true, "LinearDepth", 0, false, 0, nullptr, 0 }, { false, "PerCamera", 11, false, 224, camera, 4 } };
    CHECK(CheckBindings(PipelineKind::Velocity, velocity, 2).empty());
    CHECK(!CheckBindings(PipelineKind::Velocity, velocity, 1).empty());

    BindingSeen const fsr[] = { { true, "InputTexture", 0, false, 0, nullptr, 0 }, { true, "OutputTexture", 1, true, 0, nullptr, 0 } };
    BindingSeen const rcas[] = { { true, "InputTexture", 0, false, 0, nullptr, 0 }, { true, "HairMask", 1, false, 0, nullptr, 0 },
                                 { true, "OutputTexture", 2, true, 0, nullptr, 0 } };
    CHECK(CheckBindings(PipelineKind::Fsr, fsr, 2).empty() && CheckBindings(PipelineKind::Rcas, rcas, 3).empty());
    // FSR's arguments are not RCAS's: the destination is elsewhere.
    CHECK(CheckBindings(PipelineKind::Rcas, fsr, 2).find("OutputTexture") != std::string::npos);
    BindingSeen const readOnly[] = { { true, "InputTexture", 0, false, 0, nullptr, 0 }, { true, "OutputTexture", 1, false, 0, nullptr, 0 } };
    CHECK(CheckBindings(PipelineKind::Fsr, readOnly, 2).find("not written") != std::string::npos);
    CHECK(CheckBindings(PipelineKind::None, nullptr, 0).empty());
}

// A reversed-Z projection with these planes and a 60 degree vertical view.
void MakeCamera(float camera[56], float nearPlane, float farPlane)
{
    memset(camera, 0, 56 * sizeof(float));
    camera[21] = 1.0f / tanf(30.0f * (float)M_PI / 180.0f);
    camera[48] = nearPlane;
    camera[49] = farPlane;
    // depth(v) = [26] + [30] / v: 1 at near, 0 at far.
    camera[30] = nearPlane * farPlane / (farPlane - nearPlane);
    camera[26] = 1.0f - camera[30] / nearPlane;
}

TEST(temporal_constants)
{
    float camera[56], temporal[56] = {};
    MakeCamera(camera, 0.1f, 1000.0f);
    temporal[52] = 0.25f;
    temporal[53] = -0.125f;
    TemporalParams p;
    CHECK(TemporalParamsFrom(camera, temporal, &p));
    // X is negated, Y kept: BG3 gives the sample's move, MetalFX wants the image's.
    CHECK(p.JitterX == -0.25f);
    CHECK(p.JitterY == -0.125f);

    // A forward-Z camera must be refused, not scaled wrongly.
    float forward[56];
    memcpy(forward, camera, sizeof(forward));
    forward[30] = -forward[30];
    forward[26] = 1.0f - forward[26];
    CHECK(!TemporalParamsFrom(forward, temporal, &p));

    float bad[56];
    memcpy(bad, camera, sizeof(bad));
    bad[49] = bad[48];
    CHECK(!TemporalParamsFrom(bad, temporal, &p));

    temporal[52] = 0.75f;   // more than half a pixel is not a jitter
    CHECK(!TemporalParamsFrom(camera, temporal, &p));
    temporal[52] = NAN;
    CHECK(!TemporalParamsFrom(camera, temporal, &p));
}

TEST(viewport_and_scale)
{
    uint8_t viewport[168] = {};
    float scaleOffset[4] = { 1, 1, 0, 0 };
    uint32_t size[2] = { 1280, 720 };
    memcpy(viewport, scaleOffset, sizeof(scaleOffset));
    memcpy(viewport + 16, size, sizeof(size));
    CHECK(ViewportIsWhole(viewport, 1280, 720));
    CHECK(!ViewportIsWhole(viewport, 1280, 721));
    scaleOffset[2] = 0.5f;
    memcpy(viewport, scaleOffset, sizeof(scaleOffset));
    CHECK(!ViewportIsWhole(viewport, 1280, 720));

    CHECK(ScaleSupported(1280, 720, 2560, 1440));
    CHECK(ScaleSupported(1000, 500, 3000, 1500));
    CHECK(!ScaleSupported(1000, 500, 3001, 1500));
    CHECK(!ScaleSupported(2560, 1440, 2560, 1440));   // no scale
    CHECK(!ScaleSupported(0, 0, 100, 100));
}

TEST(size_settling)
{
    SizeSettle s = {};
    // The first scaler, and any size already made, need no wait.
    CHECK(SizeSettled(&s, 1, 1, 2, 2, 10.0, false, false));
    CHECK(SizeSettled(&s, 1, 1, 2, 2, 10.0, true, true));
    // A new size waits.
    CHECK(!SizeSettled(&s, 3, 3, 6, 6, 10.0, true, false));
    CHECK(!SizeSettled(&s, 3, 3, 6, 6, 10.2, true, false));
    // A window drag keeps restarting the wait, so it never makes a scaler.
    CHECK(!SizeSettled(&s, 4, 4, 8, 8, 10.25, true, false));
    CHECK(!SizeSettled(&s, 4, 4, 8, 8, 10.5, true, false));
    CHECK(SizeSettled(&s, 4, 4, 8, 8, 10.55, true, false));
}

TEST(jitter_sequence)
{
    CHECK(Halton(1, 2) == 0.5f);
    CHECK(Halton(2, 2) == 0.25f);
    CHECK(Halton(3, 2) == 0.75f);
    CHECK(fabsf(Halton(1, 3) - 1.0f / 3.0f) < 1e-6f);
    CHECK(JitterPhases(0) == 14u);
    CHECK(JitterPhases(3) == 32u);
    CHECK(JitterPhases(4) == 0u);
    // A cycle of the right length, and packed x then y, as a load of the table.
    CHECK(JitterSampleBits(0, 14) == JitterSampleBits(14, 14));
    CHECK(JitterSampleBits(0, 14) != JitterSampleBits(1, 14));
    uint64_t bits = JitterSampleBits(0, 14);
    float pair[2];
    memcpy(pair, &bits, sizeof(pair));
    CHECK(pair[0] == 0.5f);
    CHECK(fabsf(pair[1] - 1.0f / 3.0f) < 1e-6f);
}

TEST(strength_parsing)
{
    CHECK(ParseStrength("0.5", 0.3f) == 0.5f);
    CHECK(ParseStrength("0", 0.3f) == 0.0f);
    CHECK(ParseStrength("1", 0.3f) == 1.0f);
    CHECK(ParseStrength("1.5", 0.3f) == 0.3f);
    CHECK(ParseStrength("-1", 0.3f) == 0.3f);
    CHECK(ParseStrength("sharp", 0.3f) == 0.3f);
    CHECK(ParseStrength(nullptr, 0.3f) == 0.3f);
    // Under the locale a German Mac gives the game.
    std::string before = setlocale(LC_ALL, nullptr);
    CHECK(setlocale(LC_ALL, "de_DE.UTF-8") != nullptr);
    CHECK(ParseStrength("0.5", 0.3f) == 0.5f);
    setlocale(LC_ALL, before.c_str());
}

}

// --- Jitter stubs: entered as the island enters them, with every register known ----------

extern "C" {
void gfx_jitter_stub_projection();
void gfx_jitter_stub_camera();
void gfx_jitter_stub_velocity();
void gfx_probe_stub(void (*stub)(), uint64_t const* table, uint64_t view, uint64_t* regs, __uint128_t* vectors);

// In place of the real callee (GfxJitterSample.cpp), which reads game memory: this one shows the plumbing.
void* gProbeSeenView;
uint64_t gProbeSeenOriginal;
__attribute__((used)) uint64_t gfx_jitter_sample(void* view, uint64_t original)
{
    gProbeSeenView = view;
    gProbeSeenOriginal = original;
    return 0x1234567812345678ull;
}
}

namespace {

void ProbeStub(void (*stub)(), uint32_t reg)
{
    uint64_t const table[2] = { 0xdead, 0xfeedfacecafef00dull };
    uint64_t const view = 0xabcdef0;
    uint64_t regs[20] = {};
    __uint128_t vectors[32];
    gProbeSeenView = nullptr;
    gProbeSeenOriginal = 0;
    gfx_probe_stub(stub, table, view, regs, vectors);

    CHECK((uint64_t)gProbeSeenView == view);           // the callee got the view
    CHECK(gProbeSeenOriginal == table[1]);             // and BG3's sample
    bool intsOk = regs[8] == 1 && regs[9] == (uint64_t)table;
    for (int i = 0; i < 8; i++) intsOk = intsOk && regs[i] == (uint64_t)(100 + i);
    for (int i = 10; i < 16; i++) intsOk = intsOk && regs[i] == (uint64_t)(100 + i);
    CHECK(intsOk);                                     // x0-x15 survive
    CHECK((regs[16] & 0xf0000000) == 0x20000000);      // the flags survive
    bool vectorsOk = true;
    for (uint32_t i = 0; i < 32; i++) {
        uint64_t lo = (uint64_t)vectors[i], hi = (uint64_t)(vectors[i] >> 64);
        if (i == reg) {
            vectorsOk = vectorsOk && lo == 0x1234567812345678ull && hi == 0;
        } else {
            vectorsOk = vectorsOk && lo == (uint64_t)(0x1000 + i) && hi == (uint64_t)(0x2000 + i);
        }
    }
    CHECK(vectorsOk);                                  // only the loaded d register changes; v0-v31 survive whole
}

TEST(jitter_stubs_change_only_the_loaded_register)
{
    ProbeStub(gfx_jitter_stub_projection, kStubProjectionSample);
    ProbeStub(gfx_jitter_stub_camera, kStubCameraSample);
    ProbeStub(gfx_jitter_stub_velocity, kStubVelocitySample);
}


// --- finding the sites ---------------------------------------------------------------

TEST(gfx_sites_refuse_an_image_without_the_game)
{
    // A binary without the game's functions: every feature says why, none installs.
    static FakeBinary binary;
    GameImage image;
    CHECK(image.OpenFile(binary.File));
    auto sites = bg3se::mac::gfx::FindGfxSites(image);
    using bg3se::mac::gfx::SiteCheck;
    SiteCheck const* checks[] = { &sites.LuaJump, &sites.ThreadIdling, &sites.ReducedLocking, &sites.AoBlur, &sites.Jitter,
                                  &sites.Cluster, &sites.MetalCalls, &sites.StagePacks, &sites.ShadowMask, &sites.Programs, &sites.UiRing };
    for (SiteCheck const* check : checks) {
        CHECK(!check->Ok() && check->Problem.find("symbol table") != std::string::npos);
    }
    CHECK(sites.LuaJump.ThrowCall == 0 && sites.ReducedLocking.BindAlloc == 0);
}

// The Metal calls of build 4.1.1.7631656 that belong to a family the hooks
// follow (its _objc_msgSend$ stubs), and some that do not.
constexpr std::string_view kGameMetalCalls[] = {
    "commandBuffer", "commandBufferWithUnretainedReferences", "commit", "computeCommandEncoderWithDispatchType:",
    "dispatchThreadgroups:threadsPerThreadgroup:", "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
    "dispatchThreads:threadsPerThreadgroup:", "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:",
    "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",
    "drawPrimitives:vertexStart:vertexCount:", "endEncoding", "newCommandQueueWithMaxCommandBufferCount:",
    "newComputePipelineStateWithFunction:error:", "newRenderPipelineStateWithDescriptor:error:", "presentDrawable:",
    "presentDrawable:afterMinimumDuration:", "renderCommandEncoderWithDescriptor:", "setBuffer:offset:atIndex:", "setBytes:length:atIndex:",
    "setComputePipelineState:", "setFragmentBuffer:offset:atIndex:", "setFragmentBufferOffset:atIndex:", "setFragmentBytes:length:atIndex:",
    "setFragmentTextures:withRange:", "setRenderPipelineState:", "setTextures:withRange:",
    // not of a family: other objects' methods, or calls the hooks need not see
    "blitCommandEncoder", "setBufferIndex:", "setTexture:", "setTextureType:", "setFragmentFunction:", "setFragmentSamplerStates:withRange:",
    "setVertexBuffer:offset:atIndex:", "setVertexBufferOffset:atIndex:", "useResources:count:usage:", "dispatchType",
};

TEST(metal_calls_of_the_game_are_all_followed)
{
    std::vector<std::string_view> calls(std::begin(kGameMetalCalls), std::end(kGameMetalCalls));
    CHECK(CheckMetalCalls(calls).empty());
    for (std::string_view call : calls) CHECK(IsHooked(call) || !InHookedFamily(call));
    CHECK(IsHooked("endEncoding") && IsHooked("commit"));
    CHECK(!InHookedFamily("blitCommandEncoder") && !InHookedFamily("setVertexBuffer:offset:atIndex:"));

    // A way of doing what the hooks follow that they were not written for: refused, by name.
    for (std::string_view unseen : { "commandBufferWithDescriptor:", "parallelRenderCommandEncoderWithDescriptor:",
             "computeCommandEncoderWithDescriptor:", "newRenderPipelineStateWithDescriptor:completionHandler:",
             "newComputePipelineStateWithDescriptor:options:reflection:error:", "drawPrimitives:indirectBuffer:indirectBufferOffset:",
             "drawIndexedPatches:patchStart:patchCount:patchIndexBuffer:patchIndexBufferOffset:controlPointIndexBuffer:controlPointIndexBufferOffset:instanceCount:baseInstance:",
             "setFragmentTextures:withRange:lodMinClamps:lodMaxClamps:", "presentDrawable:options:" }) {
        auto with = calls;
        with.push_back(unseen);
        auto problem = CheckMetalCalls(with);
        CHECK(problem.find(unseen) != std::string::npos);
    }
    // Without what the hooks hang on, or without anything: refused.
    for (std::string_view needed : { "commit", "renderCommandEncoderWithDescriptor:", "newCommandQueueWithMaxCommandBufferCount:" }) {
        auto without = calls;
        std::erase(without, needed);
        CHECK(CheckMetalCalls(without).find(needed) != std::string::npos);
    }
    CHECK(CheckMetalCalls({}).find("symbol table") != std::string::npos);
}

TEST(improved_shadows_say_what_is_missing)
{
    // Its sites not found: their reason. Found: that the programs are not written. Never installed.
    ShadowMaskSites mask;
    ProgramSites programs;
    CHECK(InstallImprovedShadows(mask, programs).find("no programs") != std::string::npos);
    programs.Problem = "rf::DataLoader::CreateShader changed";
    CHECK(InstallImprovedShadows(mask, programs) == programs.Problem);
    mask.Problem = "ls::CascadedShadowMaskStage::Load changed";
    CHECK(InstallImprovedShadows(mask, programs) == mask.Problem);
}

// Run 3's burst: one buffer ran 306 ms on the GPU and some two hundred that
// waited behind it finished with it. One line for the first, one count for the rest.
TEST(stall_log_counts_the_buffers_that_waited)
{
    CHECK(ClassifyBufferStall(0.016, 0.015, false) == BufferStall::None);
    CHECK(ClassifyBufferStall(0.306, 0.306, false) == BufferStall::Own);
    CHECK(ClassifyBufferStall(0.295, 0.0, false) == BufferStall::Waited);
    CHECK(ClassifyBufferStall(0.290, 0.152, false) == BufferStall::Waited);
    CHECK(ClassifyBufferStall(0.001, 0.0, true) == BufferStall::Own);

    StallWaiters waiters;
    CHECK(waiters.Take().Count == 0);
    for (int i = 0; i < 196; i++) waiters.Add(i == 7 ? 0.304 : 0.281);
    auto taken = waiters.Take();
    CHECK(taken.Count == 196 && taken.LongestMs == 304);
    taken = waiters.Take();
    CHECK(taken.Count == 0 && taken.LongestMs == 0);
}

TEST(gfx_start_resolves_sites_once_for_any_key)
{
    using namespace bg3se::mac::gfx;
    // No game function, but the Metal calls the hooks hang on: __stubs symbols.
    static FakeBinary binary(false, {
        { "_objc_msgSend$newCommandQueueWithMaxCommandBufferCount:", FakeBinary::Stubs + 0x40, N_SECT, 2 },
        { "_objc_msgSend$renderCommandEncoderWithDescriptor:", FakeBinary::Stubs + 0x60, N_SECT, 2 },
        { "_objc_msgSend$commit", FakeBinary::Stubs + 0x80, N_SECT, 2 },
    });
    GameImage image;
    CHECK(image.OpenFile(binary.File));

    // Nothing on: nothing resolved, nothing logged.
    CHECK(StartGfx(&image, GfxHost{ [](std::string_view) { return false; } }).empty());
    CHECK(ResolvedSites() == nullptr);

    // Every key, on an image without the game's functions: each key that
    // needs one says why, and the sites stay for later hooks. The Metal keys
    // that patch nothing are asked of the Metal hooks, once for all of them
    // (which are not really put on here).
    MetalChoices asked;
    int calls = 0;
    auto fakeMetal = [&](MetalChoices const& choices) {
        asked = choices;
        calls++;
        return MetalOutcome{};
    };
    auto all = StartGfx(&image, GfxHost{ [](std::string_view) { return true; }, fakeMetal });
    CHECK(all.size() == GfxKeys().size());
    for (auto const& outcome : all) {
        bool gameFree = outcome.Key == "gfx_stall_log" || outcome.Key == "gfx_bind_dedup";
        CHECK(outcome.Problem.empty() == gameFree);
    }
    // The cluster guard needs the game's clustered-lighting pass: not asked for.
    CHECK(calls == 1 && asked.StallLog && asked.BindDedup && !asked.ClusterGuard);
    GfxSites const* sites = ResolvedSites();
    CHECK(sites != nullptr && sites->MetalCalls.Ok() && sites->MetalCalls.Calls == 3 && sites->MetalCalls.Followed == 3);

    // The Metal hooks refusing skips every Metal key with their reason; a
    // build without them says so.
    auto metalOnly = [](std::string_view key) { return key == "gfx_stall_log" || key == "gfx_bind_dedup"; };
    auto refused = StartGfx(&image, GfxHost{ metalOnly, [](MetalChoices const&) { return MetalOutcome{ "refused", {} }; } });
    CHECK(refused.size() == 2 && refused[0].Problem == "refused" && refused[1].Problem == "refused");
    auto absent = StartGfx(&image, GfxHost{ metalOnly });
    CHECK(absent.size() == 2 && absent[0].Problem.find("no Metal hooks") != std::string::npos);
    // Only what is on is asked for.
    StartGfx(&image, GfxHost{ [](std::string_view key) { return key == "gfx_bind_dedup"; }, fakeMetal });
    CHECK(calls == 2 && !asked.StallLog && asked.BindDedup);

    // Resolved once: a later start reuses them.
    auto one = StartGfx(&image, GfxHost{ [](std::string_view key) { return key == GfxKeys().front(); } });
    CHECK(one.size() == 1 && one[0].Key == GfxKeys().front() && !one[0].Problem.empty());
    CHECK(ResolvedSites() == sites);

    // No symbol table: every enabled key is skipped for that reason.
    auto none = StartGfx(nullptr, GfxHost{ [](std::string_view key) { return key == GfxKeys().front(); } });
    CHECK(none.size() == 1 && none[0].Problem.find("symbol table") != std::string::npos);
}

}

int main()
{
    bg3se::mac::SetLogStream(nullptr);
    for (auto test : { branch_encoding_and_range_run, header_padding_reaches_every_site_run, multi_word_sites_must_lie_on_one_page_run, far_jump_words_run, idle_words_match_the_expected_words_run,
             cap_idle_steps_then_sleeps_to_margin_then_yields_run, slice_alignment_refill_and_retire_run,
             bind_and_setup_replacements_match_the_expected_words_run, frame_reset_island_keeps_bg3s_store_run,
             ao_blur_changes_only_kind_and_store_run, jitter_island_shape_run, pipeline_classification_run,
             pipeline_bindings_must_be_the_ones_temporal_reads_run, temporal_constants_run,
             viewport_and_scale_run, size_settling_run, jitter_sequence_run, strength_parsing_run, jitter_stubs_change_only_the_loaded_register_run,
             gfx_sites_refuse_an_image_without_the_game_run, metal_calls_of_the_game_are_all_followed_run, improved_shadows_say_what_is_missing_run,
             gfx_start_resolves_sites_once_for_any_key_run, stall_log_counts_the_buffers_that_waited_run }) {
        test();
    }
    RunGfxMetalTests(&gTests, &gFailures);
    printf("%d tests, %d failures\n", gTests, gFailures);
    return gFailures ? 1 : 0;
}
