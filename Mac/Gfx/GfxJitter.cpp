#include <stdafx.h>
#include <Gfx/GfxJitter.h>
#include <Gfx/GfxPatch.h>
#include <Gfx/GfxSites.h>
#include <Gfx/GfxTemporalLogic.h>
#include <Platform/CodeWriter.h>
#include <Platform/Islands.h>


#include <os/lock.h>

extern "C" {
void gfx_jitter_stub_projection();
void gfx_jitter_stub_camera();
void gfx_jitter_stub_velocity();
}

namespace bg3se::mac::gfx {

namespace {

std::atomic<bool> gActive{ false };

// The view layout, as JitterSites read it, with live addresses.
struct Layout
{
    uintptr_t PostStageVtable, FsrStepVtable;
    void* const* RenderSettings;
    uint32_t ViewStages, ViewStageCount, ViewFrame, ViewSize, ViewCamera;
    uint32_t StageSteps, StageStepCount, NodeEnabled;
    uint32_t FrameIndex, FrameIndexAdd, CameraUnjittered, SettingsQuality;
};
Layout gLayout{};

template <class T>
T& Field(void* object, uint32_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

bool ViewHasEnabledFsr(void* view)
{
    auto const& l = gLayout;
    auto stages = Field<void**>(view, l.ViewStages);
    uint32_t count = Field<uint32_t>(view, l.ViewStageCount);
    if (!stages || count > 256) return false;
    for (uint32_t i = 0; i < count; i++) {
        void* stage = stages[i];
        if (!stage || Field<uintptr_t>(stage, 0) != l.PostStageVtable) continue;
        if (!Field<uint8_t>(stage, l.NodeEnabled)) continue;
        auto steps = Field<void**>(stage, l.StageSteps);
        uint32_t stepCount = Field<uint32_t>(stage, l.StageStepCount);
        if (!steps || stepCount > 256) return false;
        for (uint32_t j = 0; j < stepCount; j++) {
            if (steps[j] && Field<uintptr_t>(steps[j], 0) == l.FsrStepVtable) return Field<uint8_t>(steps[j], l.NodeEnabled) != 0;
        }
    }
    return false;
}

// A view asks for its sample several times a frame and must get one answer,
// even if a setting changes while the frame records. So: decide once per view,
// frame, camera and size. Views come and go on transitions; the table is just
// cleared when it fills.
struct ViewChoice
{
    void *View, *Frame, *Camera;
    uint64_t Size;
    uint32_t Index;
    unsigned Phases;    // 0: keep BG3's sample
};

constexpr int kMaxViews = 32;
ViewChoice gViews[kMaxViews];
int gViewCount = 0;
os_unfair_lock gViewsLock = OS_UNFAIR_LOCK_INIT;

unsigned ChoosePhases(void* view, void* camera)
{
    if (!gActive.load(std::memory_order_acquire)) return 0;
    if (Field<uint8_t>(camera, gLayout.CameraUnjittered)) return 0;
    if (!ViewHasEnabledFsr(view)) return 0;
    void* settings = *gLayout.RenderSettings;
    return settings ? JitterPhases(Field<uint8_t>(settings, gLayout.SettingsQuality)) : 0;
}

}

void SetJitterActive(bool active)
{
    gActive.store(active, std::memory_order_release);
}

bool JitterActive()
{
    return gActive.load(std::memory_order_acquire);
}

// The velocity load sits where BG3 reads the frame through the stage (stage +
// VelocityFrame), the other two through the view (view + ViewFrame); both hold
// the same frame object. The view's is used for all three: the
// per-view decision is keyed on it and must agree across the three loads.
uint64_t JitterSample(void* view, uint64_t original)
{
    if (!view) return original;
    void* frame = Field<void*>(view, gLayout.ViewFrame);
    void* camera = Field<void*>(view, gLayout.ViewCamera);
    if (!frame || !camera) return original;
    uint32_t index = Field<uint32_t>(frame, gLayout.FrameIndex) + Field<uint32_t>(frame, gLayout.FrameIndexAdd);
    uint64_t size = Field<uint64_t>(view, gLayout.ViewSize);

    os_unfair_lock_lock(&gViewsLock);
    ViewChoice* choice = nullptr;
    for (int i = 0; i < gViewCount; i++) {
        if (gViews[i].View == view) {
            choice = &gViews[i];
            break;
        }
    }
    if (!choice) {
        if (gViewCount == kMaxViews) gViewCount = 0;
        choice = &gViews[gViewCount++];
        choice->View = nullptr;
    }
    if (choice->View != view || choice->Frame != frame || choice->Camera != camera || choice->Index != index || choice->Size != size) {
        *choice = { view, frame, camera, size, index, ChoosePhases(view, camera) };
    }
    unsigned phases = choice->Phases;
    os_unfair_lock_unlock(&gViewsLock);

    return phases ? JitterSampleBits(index, phases) : original;
}

std::string InstallJitter(GameImage const& game, JitterSites const& s)
{
    if (!s.Ok()) return s.Problem;
    GameImage const* image = &game;
    if (s.Projection.SampleReg != kStubProjectionSample || s.Camera.SampleReg != kStubCameraSample || s.Velocity.SampleReg != kStubVelocitySample ||
        s.Projection.ViewReg != kStubViewReg || s.Camera.ViewReg != kStubViewReg || s.VelocityStageReg != kStubStageReg ||
        s.VelocityViewInStage != kStubViewInStage) {
        return "the loads use other registers than the stubs are written for";
    }
    auto live = [&](uint64_t va) { return (uintptr_t)image->LoadedAt(va); };

    // The view walk depends on this vtable being the post-process stage's.
    if (*reinterpret_cast<uintptr_t const*>(live(s.PostStageMethodSlot)) != live(s.PostStageMethod)) {
        return "the post-process stage's vtable does not hold GetType where it was found";
    }
    gLayout = {
        live(s.PostStageVtable), live(s.FsrStepVtable), reinterpret_cast<void* const*>(live(s.RenderSettings)),
        s.ViewStages, s.ViewStageCount, s.ViewFrame, s.ViewSize, s.ViewCamera,
        s.StageSteps, s.StageStepCount, s.NodeEnabled,
        s.FrameIndex, s.FrameIndexAdd, s.CameraUnjittered, s.SettingsQuality,
    };

    struct Site
    {
        JitterSites::Load const& Load;
        void (*Stub)();
    } const sites[3] = {
        { s.Projection, gfx_jitter_stub_projection },
        { s.Camera, gfx_jitter_stub_camera },
        { s.Velocity, gfx_jitter_stub_velocity },
    };
    uint32_t islands[3][kJitterIslandWords];
    uint32_t branches[3];
    void* islandAt[3];
    for (int i = 0; i < 3; i++) {
        void* site = image->LoadedAt(sites[i].Load.Site);
        if (*static_cast<uint32_t const*>(site) != sites[i].Load.Word) return "a load changed since it was found";
        std::string why;
        islandAt[i] = ReserveIsland(site, kJitterIslandWords * 4, why);
        if (!islandAt[i]) return "no room for its islands: " + why;
        branches[i] = EncodeB((uint64_t)(uintptr_t)site, (uint64_t)(uintptr_t)islandAt[i]);
        if (!branches[i] || !WriteJitterIsland(islands[i], (uint64_t)(uintptr_t)islandAt[i], (uint64_t)(uintptr_t)sites[i].Stub,
                                                (uint64_t)(uintptr_t)site + 4)) {
            return "islands out of range";
        }
    }
    // The islands first, while nothing branches to them; then each load's one-word branch.
    for (int i = 0; i < 3; i++) {
        if (auto problem = WriteCodeBlock(islandAt[i], islands[i], BlockContract::Unreachable); !problem.empty()) {
            return "could not write an island: " + problem;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (auto problem = WriteCodeWord(image->LoadedAt(sites[i].Load.Site), branches[i]); !problem.empty()) {
            return "could not write a load's branch: " + problem;
        }
    }
    return {};
}

}
