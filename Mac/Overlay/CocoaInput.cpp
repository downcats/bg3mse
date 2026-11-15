// The overlay's input on macOS: the definitions of Norbyte's SDLManager
// (Extender/Client/SDLManager.h) for a game that has no SDL. On Windows they
// are in Extender/Client/SDL.cpp, which hooks SDL's functions; the Mac game
// takes its input from Cocoa, and what SDL.cpp does in its hook of
// SDL_PollEvent is done here for each event the game's application object is
// sent (Mac/Overlay/EventTap.h):
//
//   the console key           gExtender->OnSDLEvent
//   the overlay               the event as ImGui inputs, and the decision
//                             whether the overlay takes it from the game
//   Lua's input events        ecl::ExtensionState::OnInputEvent, whose handlers
//                             may prevent the game from getting the event;
//                             not while that state is being replaced
//                             (Overlay/StateGate.h)
//
// Controllers are not Cocoa events: their SDL events come from the hooks of
// Overlay/ControllerHooks.h, on the thread the game reads its controllers
// on, and go to the console key and to Lua the same way; the overlay's
// gamepad navigation reads the first controller's state at the start of each
// frame, as ImGui's SDL backend polls SDL's.
//
// Norbyte's code is given SDL events, as on Windows: the SDL event SDL would
// have made of the Cocoa event, built from SDL's headers. Nothing of SDL is
// linked or loaded, so SDL_GetTicks, which gives those events (and the ones
// Lua injects) their time, is defined here.
//
// What is not SDL's way: the events arrive on the main thread, and ImGui
// runs on the client thread, so the ImGui inputs are queued and replayed at
// the start of the client's frame (SDLManager::NewFrame) instead of being
// handed to ImGui where the event is taken; and an injected key goes to the
// game's own input queue at once (ls::InputManager::InjectInput) instead of
// being held for the game's next poll.

#include <stdafx.h>
#include <Extender/ScriptExtender.h>
#include <Extender/Client/ExtensionStateClient.h>
#include <Extender/Client/SDLManager.h>
#include <imgui.h>

#include <Overlay/CocoaInput.h>
#include <Overlay/ConsoleWindow.h>
#include <Overlay/ControllerHooks.h>
#include <Overlay/Dependencies.h>
#include <Overlay/EventTap.h>
#include <Overlay/InputEvents.h>
#include <Overlay/KeyCodes.h>
#include <Overlay/OverlayState.h>
#include <Overlay/Pointer.h>
#include <Overlay/StateGate.h>
#include <Overlay/TextInput.h>

#include <atomic>
#include <chrono>

extern "C" Uint32 SDL_GetTicks(void)
{
    return bg3se::mac::overlay::InputTicks();
}

BEGIN_SE()

namespace {

using namespace mac::overlay;

// The Mac state of the one SDLManager, which has no members for it.
class CocoaInput final : public EventSink, public ControllerSink, public TextSink
{
public:
    static CocoaInput& Get()
    {
        static CocoaInput input;
        return input;
    }

    void EnableHooks()
    {
        if (hooked_) return;
        // Says in the log, once, which part is off and why.
        auto const& dependencies = GameDependencies();
        if (!dependencies.Usable(Part::Events)) return;
        auto problem = InstallEventTap(dependencies, this);
        if (!problem.empty()) {
            ERR("Input: the game's events are not taken: %s", problem.c_str());
            return;
        }
        hooked_ = true;
        INFO("Input: the game's events are taken for the overlay");

        // The text client, with the events: without it a key's characters are the overlay's text.
        problem = InstallTextInput(dependencies, this);
        if (problem.empty()) INFO("Input: the overlay's text fields take text from the text input system");
        else ERR("Input: the overlay's text fields take a key's characters as they are: %s", problem.c_str());

        // The pointer, with the events: without it the game keeps its cursor and the overlay its position.
        if (dependencies.Usable(Part::Pointer)) {
            problem = InstallPointer(dependencies);
            if (problem.empty()) INFO("Input: the overlay sets the pointer's shape and position while it has the pointer");
            else ERR("Input: the overlay does not set the pointer: %s", problem.c_str());
        }

        // Controllers stand alone: without them the keyboard and the mouse still work.
        if (!dependencies.Usable(Part::Controllers)) return;
        problem = InstallControllerHooks(dependencies, this);
        if (!problem.empty()) {
            ERR("Input: the game's controllers are not read for Lua and the overlay: %s", problem.c_str());
            return;
        }
        INFO("Input: the game's controllers are read for Lua and the overlay");
    }

    void DisableHooks()
    {
        if (!hooked_) return;
        RemoveControllerHooks();
        RemovePointer();
        RemoveTextInput();
        RemoveEventTap();
        hooked_ = false;
    }

    // --- the client thread, which owns the ImGui context --------------------

    void InitializeUI()
    {
        ImGuiIO& io = ImGui::GetIO();
        io.BackendPlatformName = "bg3mse_cocoa";
        // Where the input method's window goes, as ImGui_ImplSDL2_PlatformSetImeData says it.
        ImGui::GetPlatformIO().Platform_SetImeDataFn = &SetImeData;
        queue_.Clear();
        lastFrame_ = {};
        AddConsoleWindowHook(ImGui::GetCurrentContext());
        uiEnabled_.store(true);
    }

    void DestroyUI()
    {
        uiEnabled_.store(false);
        queue_.Clear();
        State().MouseCursor.store(kKeepCursor);
        State().WantTextInput.store(false);
        if (ImGui::GetCurrentContext()) ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
        if (ImGui::GetCurrentContext()) ImGui::GetIO().BackendPlatformName = nullptr;
    }

    void NewFrame()
    {
        ImGuiIO& io = ImGui::GetIO();
        // ImGui's coordinates are the drawable's pixels.
        auto const& state = State();
        io.DisplaySize = ImVec2((float)state.DrawableWidth(), (float)state.DrawableHeight());
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

        auto now = std::chrono::steady_clock::now();
        float elapsed = lastFrame_.time_since_epoch().count() ? std::chrono::duration<float>(now - lastFrame_).count() : 0.0f;
        io.DeltaTime = elapsed > 0.0f ? elapsed : 1.0f / 60.0f;
        lastFrame_ = now;

        queue_.Replay(io);

        // What ImGui_ImplSDL2_NewFrame does with the pointer, before ImGui's
        // frame, on what ImGui asked for in the last: the main thread makes it.
        constexpr ImGuiBackendFlags kPointerFlags = ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_HasSetMousePos;
        if (PointerInstalled()) {
            io.BackendFlags |= kPointerFlags;
            int32_t cursor = (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) ? kKeepCursor
                : io.MouseDrawCursor ? (int32_t)ImGuiMouseCursor_None : (int32_t)ImGui::GetMouseCursor();
            State().MouseCursor.store(cursor);
            if (io.WantSetMousePos) State().RequestWarp(io.MousePos.x, io.MousePos.y);
        } else {
            io.BackendFlags &= ~kPointerFlags;
        }

        GamepadState gamepad;
        ApplyGamepad(FirstGamepad(gamepad) ? &gamepad : nullptr, io);
    }

    void ViewportResized(uint32_t width, uint32_t height)
    {
        // On the client thread, which is also the one that replaces the
        // state when the game's state changes: no gate between the two.
        if (!gExtender || !gExtender->GetClient().HasExtensionState()) return;
        auto event = SdlViewportResized((int32_t)width, (int32_t)height, InputTicks());
        int result = 1;
        ecl::ExtensionState::Get().OnInputEvent(&event, result);
    }

    // --- the thread Lua runs on ---------------------------------------------

    void Inject(SDL_Event const& event)
    {
        auto const& dependencies = GameDependencies();
        if (!dependencies.Usable(Part::Injection)) return;      // said in the log already
        auto manager = *reinterpret_cast<void**>((uintptr_t)dependencies.InputManager);
        if (!manager) {
            WARN("Input: a key was injected before the game has an input manager; it is dropped");
            return;
        }
        std::span<uint16_t const> keys(reinterpret_cast<uint16_t const*>((uintptr_t)dependencies.KeyboardKeys), dependencies.KeyCodes);
        bool withModifiers = dependencies.Usable(Part::InjectedModifiers);
        if (event.key.keysym.mod && !withModifiers && !saidModifiers_.exchange(true)) {
            WARN("Input: the modifiers of injected keys are not queued: %s", dependencies.Problem(Part::InjectedModifiers).c_str());
        }
        std::vector<GameInputChange> changes;
        GameChangesOfInjected(event, keys, SdlModifiersOf(gameModifiers_.load()), withModifiers, isoKeyboard_.load(), changes);
        if (changes.empty()) {
            WARN("Input: the game has no input for the injected key (scancode %d); it is dropped", (int)event.key.keysym.scancode);
            return;
        }
        // As -[LSMTLView keyDown:], keyUp: and flagsChanged: queue a key.
        using InjectInputProc = void (void* manager, GameInputChange const& change, bool fromWheel);
        for (auto const& change : changes) reinterpret_cast<InjectInputProc*>((uintptr_t)dependencies.InjectInput)(manager, change, false);
    }

    // --- the main thread: every event the game is about to be sent ----------

    bool OnEvent(CocoaEvent const& event) override
    {
        bool deliver = true;
        BEGIN_GUARDED()
        deliver = Handle(event);
        END_GUARDED()
        // The modifiers the game was last sent as held: it has no others.
        if (deliver && event.Kind == CocoaEventKind::FlagsChanged) gameModifiers_.store(event.ModifierFlags);
        // The keyboard typed on last: an injected key's scancode names a place on it.
        if (event.Kind == CocoaEventKind::KeyDown || event.Kind == CocoaEventKind::KeyUp) isoKeyboard_.store(event.IsoKeyboard);
        return deliver;
    }

    // --- the thread the game reads its controllers on -----------------------

    bool OnControllerEvent(SDL_Event const& event) override
    {
        bool deliver = true;
        BEGIN_GUARDED()
        deliver = HandleController(event);
        END_GUARDED()
        return deliver;
    }

    // --- the main thread: text the input system committed ------------------

    void OnText(char const* utf8) override
    {
        if (!uiEnabled_.load()) return;
        std::vector<OverlayInput> inputs;
        TextInputsOf(utf8, inputs);
        queue_.Push(inputs);
    }

    void OnFocus(bool active) override
    {
        if (!uiEnabled_.load()) return;
        OverlayInput input;
        input.What = OverlayInput::Kind::Focus;
        input.Down = active;
        queue_.Push({ &input, 1 });
    }

private:
    // The client thread, in ImGui's EndFrame.
    static void SetImeData(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData* data)
    {
        if (data && data->WantVisible) State().SetIme(data->InputPos.x, data->InputPos.y, data->InputLineHeight);
    }

    // What SDLManager::SDLPollEventInternal does for an event that was polled.
    bool Handle(CocoaEvent const& event)
    {
        if (!gExtender) return true;
        // The console window's key is its own (Overlay/ConsoleWindow.h).
        if (ConsoleWindowTakes(event)) return false;

        SDL_Event sdlEvent;
        bool hasSdlEvent = SdlEventOf(event, InputTicks(), sdlEvent);
        if (hasSdlEvent) gExtender->OnSDLEvent(&sdlEvent);

        int result = 1;
        if (uiEnabled_.load()) {
            inputs_.clear();
            OverlayInputsOf(event, inputs_);
            queue_.Push(inputs_);

            auto const& state = State();
            OverlayWants wants;
            // An overlay that is not on screen wants nothing.
            if (state.Presenting.load()) {
                wants.TextInput = state.WantTextInput.load();
                wants.Mouse = state.WantCaptureMouse.load();
            }
            if (OverlayTakes(event, wants)) result = 0;
        }

        if (result == 1 && hasSdlEvent) {
            // Not on a state that the client thread is replacing.
            auto use = ClientStateGate().Use();
            if (use.owns_lock() && gExtender->GetClient().HasExtensionState()) {
                ecl::ExtensionState::Get().OnInputEvent(&sdlEvent, result);
            }
        }

        return result == 1;
    }

    // What SDLManager::SDLPollEventInternal does for a controller event: the
    // overlay takes none from the game, there as here.
    bool HandleController(SDL_Event const& polled)
    {
        if (!gExtender) return true;
        SDL_Event event = polled;
        gExtender->OnSDLEvent(&event);
        int result = 1;
        auto use = ClientStateGate().Use();
        if (use.owns_lock() && gExtender->GetClient().HasExtensionState()) {
            ecl::ExtensionState::Get().OnInputEvent(&event, result);
        }
        return result == 1;
    }

    bool hooked_{ false };
    std::atomic<bool> uiEnabled_{ false };
    std::atomic<uint32_t> gameModifiers_{ 0 };     // the flags of the last modifier change the game was sent
    std::atomic<bool> saidModifiers_{ false };
    std::atomic<bool> isoKeyboard_{ false };
    OverlayInputQueue queue_;
    std::vector<OverlayInput> inputs_;      // the main thread's, kept for its storage
    std::chrono::steady_clock::time_point lastFrame_{};
};

}

SDLManager::SDLManager()
{}

SDLManager::~SDLManager()
{
    DestroyUI();
    DisableHooks();
}

void SDLManager::EnableHooks()
{
    CocoaInput::Get().EnableHooks();
}

void SDLManager::DisableHooks()
{
    CocoaInput::Get().DisableHooks();
}

void SDLManager::InitializeUI()
{
    if (enableUI_) return;

    CocoaInput::Get().InitializeUI();
    enableUI_ = true;
}

void SDLManager::DestroyUI()
{
    if (!enableUI_) return;

    enableUI_ = false;
    CocoaInput::Get().DestroyUI();
}

void SDLManager::NewFrame()
{
    CocoaInput::Get().NewFrame();
}

void SDLManager::InjectEvent(SDL_Event const& evt)
{
    CocoaInput::Get().Inject(evt);
}

END_SE()

namespace bg3se::mac::overlay {

void ReportViewportResized(uint32_t width, uint32_t height)
{
    bg3se::CocoaInput::Get().ViewportResized(width, height);
}

}
