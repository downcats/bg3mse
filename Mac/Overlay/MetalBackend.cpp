#include <stdafx.h>
#include <Mac/Overlay/MetalBackend.h>

#include <Extender/Client/IMGUI/IMGUI.h>
#include <Extender/Client/IMGUI/TextureMirror.inl>

#include <Overlay/CocoaInput.h>
#include <Overlay/Dependencies.h>
#include <Overlay/EventTap.h>
#include <Overlay/FrameHandover.h>
#include <Overlay/FrameHooks.h>
#include <Overlay/MetalOverlay.h>
#include <Overlay/OverlayState.h>
#include <Overlay/Pointer.h>
#include <Overlay/TextInput.h>

#include <array>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

BEGIN_NS(extui)

using namespace mac::overlay;

struct MetalBackend::State : public FrameListener
{
    explicit State(IMGUIManager& ui) : UI(ui) {}

    struct ViewportInfo
    {
        ImGuiViewportP Viewport;
        std::vector<ImDrawList*> ClonedDrawLists;
    };

    // --- the main thread ----------------------------------------------------

    void OnFrame(FrameInfo const& frame) override
    {
        Device.store(frame.Device);
        FrameSize.store((uint64_t)frame.Width << 32 | frame.Height);
        Format.store(frame.Format.PixelFormat << 16 | (uint64_t)frame.Format.Space << 8 | (frame.Format.ExtendedRange ? 1 : 0));
        if (frame.Target == TargetKind::HighDynamicRange) UiWhite.store(GameUiWhite());
        Target.store(frame.Target);
        FrameSeen.store(true);
        // Once a frame, on the main thread: the pointer and the text client as the overlay asked for them.
        ApplyPointer(GameView());
        ApplyTextInput(GameView());
    }

    // The white of the game's own UI on HDR output, in nits: the constant
    // ls::CombineUIRenderStage::ExecuteWTKernel() gives CombineUI_HDR as its
    // UIPaperWhite, read from that code. Not the setting HDRPaperWhite, which
    // that shader does not read. NaN when it cannot be read.
    static float GameUiWhite()
    {
        auto const& d = GameDependencies();
        if (!d.Usable(Part::HighDynamicRange)) return NAN;
        return d.UiWhite;
    }

    TargetFormat FormatSeen() const
    {
        uint64_t packed = Format.load();
        TargetFormat format;
        format.PixelFormat = packed >> 16;
        format.Space = (ColourSpace)((packed >> 8) & 0xff);
        format.ExtendedRange = (packed & 1) != 0;
        format.Kind = ClassifyTarget(format.PixelFormat, format.Space, format.ExtendedRange);
        return format;
    }

    void OnCommit(void* commandBuffer, void* drawable) override
    {
        Handover.Commit([&](int32_t slot) {
            auto& vp = Viewports[slot].Viewport;
            if (!vp.DrawDataP.Valid) return;

            // A frame nearer the destruction of each mirrored texture that is
            // no longer drawn with.
            Mirrors.BeginRender();

            TargetKind target;
            std::string problem;
            auto encoded = MetalEncode(&vp.DrawDataP, commandBuffer, drawable, UiWhite.load(), target, problem);
            if (encoded == Encoded::Failed && !EncodeFailureReported) {
                EncodeFailureReported = true;
                ERR("IMGUI: the overlay could not be drawn: %s", problem.c_str());
            } else if (encoded == Encoded::Drawn && !FirstFrameReported) {
                FirstFrameReported = true;
                INFO("IMGUI: the overlay's first frame was drawn onto the game's");
            }
        });
    }

    // --- the client thread --------------------------------------------------

    void FreeClonedDrawLists(ViewportInfo& info)
    {
        for (auto list : info.ClonedDrawLists) {
            list->~ImDrawList();
            IM_FREE(list);
        }
        info.ClonedDrawLists.clear();
    }

    IMGUIManager& UI;
    // Which of the frames below is drawn, and whether the renderer can draw
    // it: what the two threads share.
    FrameHandover Handover;
    std::array<ViewportInfo, 3> Viewports;
    int32_t CurViewport{ 0 };
    // The copies of ImGui's own textures that the main thread draws with and
    // uploads from: both threads', under the handover's lock.
    TextureMirrorSet Mirrors;

    bool Hooked{ false };
    // The main thread's, with the handover's lock held.
    bool EncodeFailureReported{ false };
    bool FirstFrameReported{ false };
    // The client thread's.
    bool NoInputReported{ false };
    bool InitialisedReported{ false };

    // What the main thread saw of the frame last presented.
    std::atomic<void*> Device{ nullptr };
    std::atomic<uint64_t> FrameSize{ 0 };
    std::atomic<TargetKind> Target{ TargetKind::Unsupported };
    std::atomic<uint64_t> Format{ 0 };      // TargetFormat: pixel format, colour space, extended range
    std::atomic<float> UiWhite{ NAN };      // on HDR output
    std::atomic<bool> FrameSeen{ false };

    // What the client thread has acted on.
    uint64_t ReportedSize{ 0 };
    TargetKind ReportedTarget{ TargetKind::Unsupported };
    std::string ReportedRefusal;
    bool Reported{ false };

    // The Metal texture retained for each game texture that is registered.
    std::mutex TextureLock;
    std::unordered_map<TextureOpaqueHandle, void*> Textures;
};

MetalBackend::MetalBackend(IMGUIManager& ui)
    : state_(std::make_unique<State>(ui))
{}

MetalBackend::~MetalBackend()
{
    DestroyUI();
    DisableHooks();
    for (auto& viewport : state_->Viewports) state_->FreeClonedDrawLists(viewport);
    for (auto const& texture : state_->Textures) MetalReleaseTexture(texture.second);
}

void MetalBackend::EnableHooks()
{
    auto& s = *state_;
    if (s.Hooked) return;

    // Says in the log, once, which part is off and why.
    auto const& dependencies = GameDependencies();
    if (!dependencies.Drawable()) return;

    auto problem = InstallFrameHooks(dependencies, &s);
    if (!problem.empty()) {
        ERR("IMGUI: the overlay is off: %s", problem.c_str());
        return;
    }
    s.Hooked = true;
    INFO("IMGUI: the overlay's frame hooks are on; it is initialised when the game presents its first frame");
}

void MetalBackend::DisableHooks()
{
    auto& s = *state_;
    if (!s.Hooked) return;
    RemoveFrameHooks();
    s.Hooked = false;
}

void MetalBackend::ClientUpdate()
{
    auto& s = *state_;
    // No overlay without its input: one that is drawn and cannot be used is none.
    if (!s.Hooked || !s.FrameSeen.load()) return;
    if (!EventTapInstalled()) {
        if (!s.NoInputReported) {
            s.NoInputReported = true;
            WARN("IMGUI: the game presents frames, but its events are not taken; the overlay is not drawn");
        }
        return;
    }

    auto target = s.Target.load();
    if (!s.Reported) {
        auto size = s.FrameSize.load();
        INFO("IMGUI: the game's first frame was seen (%u x %u pixels, %s)", (uint32_t)(size >> 32), (uint32_t)size, Describe(target));
    }
    // Why the overlay is not drawn on this target, or nothing.
    std::string refusal;
    if (target == TargetKind::Unsupported) {
        refusal = "the overlay draws on BGRA in sRGB or Display P3, or in BT.2020 PQ with extended range, only";
    } else if (target == TargetKind::HighDynamicRange) {
        auto const& dependencies = GameDependencies();
        float white = s.UiWhite.load();
        if (!dependencies.Usable(Part::HighDynamicRange)) {
            refusal = "the white of the game's UI is not known: " + dependencies.Problem(Part::HighDynamicRange);
        } else if (!UsablePaperWhite(white)) {
            char text[96];
            snprintf_l(text, sizeof(text), LC_C_LOCALE, "the white of the game's UI (%g) is not a luminance a display can show", white);
            refusal = text;
        } else if (s.Handover.Initialized() && !MetalDrawsHighDynamicRange()) {
            refusal = "the pass that draws it there could not be made";
        }
    }
    if (!s.Reported || target != s.ReportedTarget || refusal != s.ReportedRefusal) {
        auto format = Describe(s.FormatSeen());
        if (!refusal.empty()) {
            WARN("IMGUI: the game presents in %s (%s); the overlay is not drawn: %s", Describe(target), format.c_str(), refusal.c_str());
        } else if (target == TargetKind::HighDynamicRange) {
            INFO("IMGUI: the game presents in high dynamic range (%s); the overlay is drawn with its white at the game's UI white, %g nits",
                format.c_str(), s.UiWhite.load());
        } else if (s.Reported) {
            INFO("IMGUI: the game presents in standard dynamic range again (%s); the overlay is drawn", format.c_str());
        }
        s.ReportedTarget = target;
        s.ReportedRefusal = refusal;
        s.Handover.SetTargetDrawable(refusal.empty());
    }

    auto size = s.FrameSize.load();
    if (size != s.ReportedSize) {
        bool resized = s.ReportedSize != 0;
        s.ReportedSize = size;
        // Initialises the overlay the first time; every time, computes its
        // scale from the size.
        s.UI.OnRenderBackendInitialized();
        if (!s.InitialisedReported && s.Handover.Initialized()) {
            s.InitialisedReported = true;
            INFO("IMGUI: the overlay is initialised; a window is drawn once a script has made one and opened it");
        }
        if (resized) ReportViewportResized((uint32_t)(size >> 32), (uint32_t)size);
    }
    s.Reported = true;
}

void MetalBackend::InitializeUI()
{
    auto& s = *state_;
    if (s.Handover.Initialized()) return;
    auto device = s.Device.load();
    if (!device) return;

    if (!s.Handover.Initialize([&] { return MetalInitialize(device); })) {
        ERR("IMGUI: the Metal renderer could not be initialised");
    }
}

void MetalBackend::DestroyUI()
{
    auto& s = *state_;
    if (!s.Handover.Initialized()) return;

    s.Handover.ShutDown([&] {
        // Before the renderer goes: ImGui's textures are mirrored again once
        // it is initialised again.
        s.Mirrors.Destroy(&MetalUpdateTexture);
        if (!MetalShutdown()) {
            ERR("IMGUI: a frame with the overlay on it has not finished; the Metal renderer's objects are left alive instead of destroyed");
        }
    });
}

void MetalBackend::NewFrame()
{
    auto& s = *state_;
    s.CurViewport = (s.CurViewport + 1) % (int32_t)s.Viewports.size();
    GImGui->Viewports[0] = &s.Viewports[s.CurViewport].Viewport;

    if (!s.Handover.HasObjects()) {
        bool made = s.Handover.MakeObjects([&] { return MetalCreateDeviceObjects(s.Device.load()); });
        if (!made) ERR("Failed to initialize IMGUI Metal backend!");
    }
}

void MetalBackend::FinishFrame()
{
    auto& s = *state_;
    // What ImGui wants of the input after this frame, for the thread that
    // takes the game's events.
    auto& io = ImGui::GetIO();
    auto& shared = mac::overlay::State();
    shared.WantTextInput.store(io.WantTextInput);
    shared.WantCaptureMouse.store(io.WantCaptureMouse);

    s.Handover.Publish(s.CurViewport, [&] {
        auto& info = s.Viewports[s.CurViewport];
        auto& vp = info.Viewport;

        s.FreeClonedDrawLists(info);
        for (int i = 0; i < vp.DrawDataP.CmdLists.Size; i++) {
            auto drawList = vp.DrawDataP.CmdLists[i]->CloneOutput();
            vp.DrawDataP.CmdLists[i] = drawList;
            info.ClonedDrawLists.push_back(drawList);
        }

        // What ImGui asked of its textures while this frame was built goes
        // to their mirrors, and the frame draws with those.
        s.Mirrors.Sync(vp.DrawDataP);
    });
}

void MetalBackend::ClearFrame()
{
    state_->Handover.Withdraw();
}

bool MetalBackend::IsInitialized()
{
    // On a target the overlay does not draw on, no frames are built either.
    // An HDR target is one only while the pass for it exists: that is known
    // from the renderer's initialisation on (ClientUpdate).
    return state_->Handover.Initialized() && state_->Handover.TargetDrawable();
}

glm::ivec2 MetalBackend::GetViewportSize()
{
    auto size = state_->FrameSize.load();
    return glm::ivec2((int)(size >> 32), (int)(uint32_t)size);
}

std::optional<TextureLoadResult> MetalBackend::RegisterTexture(TextureDescriptor* descriptor)
{
    auto const& dependencies = GameDependencies();
    if (!descriptor || !dependencies.Usable(Part::Textures)) return {};

    uint32_t width = 0, height = 0;
    auto texture = MetalRetainTexture(descriptor, dependencies.MetalTexture, width, height);
    if (!texture) {
        ERR("The game's texture has no two-dimensional Metal texture that the overlay can draw");
        return {};
    }

    auto& s = *state_;
    std::lock_guard _(s.TextureLock);
    auto& held = s.Textures[descriptor];
    // Registered again before it was unregistered: the texture loader does
    // that for a texture whose release was still pending.
    if (held) MetalReleaseTexture(held);
    held = texture;
    return TextureLoadResult{ reinterpret_cast<TextureOpaqueHandle>(descriptor), width, height };
}

void MetalBackend::UnregisterTexture(TextureOpaqueHandle opaqueHandle)
{
    auto& s = *state_;
    std::lock_guard _(s.TextureLock);
    auto it = s.Textures.find(opaqueHandle);
    if (it == s.Textures.end()) return;
    MetalReleaseTexture(it->second);
    s.Textures.erase(it);
}

std::optional<ImTextureID> MetalBackend::BindTexture(TextureOpaqueHandle opaqueHandle)
{
    auto& s = *state_;
    std::lock_guard _(s.TextureLock);
    auto it = s.Textures.find(opaqueHandle);
    if (it == s.Textures.end()) return {};
    // What ImGui's Metal renderer takes a texture id to be: the id<MTLTexture>.
    return (ImTextureID)(intptr_t)it->second;
}

END_NS()
