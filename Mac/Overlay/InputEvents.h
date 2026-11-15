#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include <SDL_events.h>
#include <imgui.h>

#include <Overlay/GameInput.h>

// The overlay's input, from a Cocoa event to its three consumers, as plain
// functions of plain values: nothing here touches AppKit, the game or the
// ImGui context of another thread, so all of it runs in the tests.
//
// The game's events are taken on the main thread, where
// -[BG3Application sendEvent:] is replaced (Overlay/EventTap.h). For each one:
//
//   the overlay   gets the event as ImGui inputs, queued here and replayed
//                 into ImGui on the client thread before ImGui::NewFrame(),
//                 so that the ImGui context is only ever used from that thread;
//   Norbyte's     code (the console key, the Lua input events) gets it as the
//                 SDL event SDL would have made of it: his code and every
//                 mod's Lua are written against SDL's values;
//   the game      gets it, unless the overlay takes it (OverlayTakes) or a
//                 Lua handler prevents it.
//
// Positions are in pixels of the game's drawable, from its top left corner:
// the overlay is drawn in those (the drawable is the size the game chose,
// which is neither the view's size in points nor that times the screen's
// scale), and so does the game count its own mouse position.

namespace bg3se::mac::overlay {

enum class CocoaEventKind : uint8_t
{
    Other,
    KeyDown,
    KeyUp,
    FlagsChanged,       // a modifier key went down or up
    MouseDown,          // any button
    MouseUp,
    MouseMoved,         // moved or dragged
    ScrollWheel,
    MouseExited,        // the pointer left the game's view (the overlay's own tracking area, Overlay/Pointer.h)
};

// What the overlay's input needs of an NSEvent.
struct CocoaEvent
{
    CocoaEventKind Kind{ CocoaEventKind::Other };
    uint16_t KeyCode{ 0 };          // key events and FlagsChanged: the virtual key code
    uint32_t ModifierFlags{ 0 };
    bool Repeat{ false };           // KeyDown: the key is being held
    int32_t Button{ 0 };            // mouse buttons: 0 left, 1 right, 2 middle, then the others
    int32_t Clicks{ 0 };
    bool HasPosition{ false };      // mouse events: X and Y are set
    float X{ 0 }, Y{ 0 };
    float ScrollX{ 0 }, ScrollY{ 0 };   // the scrolling deltas
    bool PreciseScroll{ false };    // ... which are in pixels (a trackpad), not in lines (a wheel)
    bool ScrollCancelled{ false };  // the scroll gesture was cancelled: its deltas mean nothing
    char Text[32]{};                // KeyDown: the characters the key gives, UTF-8, zero-terminated
    uint32_t PressedButtons{ 0 };   // MouseExited: the buttons held, a bit for each ([NSEvent pressedMouseButtons])
    uint32_t LayoutCharacter{ 0 };  // KeyDown, KeyUp: the key's character in the keyboard layout (Overlay/KeyboardLayout.h), 0 for none
    bool IsoKeyboard{ false };      // KeyDown, KeyUp: the key is of an ISO keyboard
    bool TextByInputSystem{ false };    // KeyDown: the text input system makes the text of it (Overlay/TextInput.h), not Text
    bool Composing{ false };        // KeyDown: the input method holds a composition, and the key is its
};

// A point of the game's view in pixels of its drawable. The view's y runs up
// unless it is flipped; the result's runs down. False when the view or the
// drawable has no size.
struct ViewGeometry
{
    double ViewWidth{ 0 }, ViewHeight{ 0 };
    double DrawableWidth{ 0 }, DrawableHeight{ 0 };
    bool Flipped{ false };
};
bool ToDrawablePixels(ViewGeometry const& geometry, double viewX, double viewY, float& x, float& y);
// The reverse: a point in pixels of the drawable as a point of the view.
bool ToViewPoint(ViewGeometry const& geometry, float x, float y, double& viewX, double& viewY);

// Copies the text of a key-down into `out` (zero-terminated, cut at a
// character boundary), leaving out what is not text: control characters, the
// private-use characters AppKit gives for function and arrow keys, and
// everything while Command or Control is held. Returns the bytes written.
size_t TextOfKeyDown(char const* characters, uint32_t modifierFlags, char* out, size_t outSize);

// --- the overlay ------------------------------------------------------------

struct OverlayInput
{
    enum class Kind : uint8_t { Key, Text, MousePosition, MouseButton, MouseWheel, Focus };

    Kind What{ Kind::Key };
    ImGuiKey Key{ ImGuiKey_None };
    bool Down{ false };             // Key, MouseButton; Focus: gained
    int32_t KeyCode{ -1 };          // Key: the virtual key code, -1 for a modifier state
    int32_t Button{ 0 };
    float X{ 0 }, Y{ 0 };           // MousePosition; MouseWheel: the amounts
    char Text[32]{};
};

// The ImGui inputs an event makes, appended to `out`.
void OverlayInputsOf(CocoaEvent const& event, std::vector<OverlayInput>& out);

// The ImGui inputs of text the input system committed, appended to `out`: as
// many as it takes, each cut at a character boundary, without what is not
// text (TextOfKeyDown).
void TextInputsOf(char const* utf8, std::vector<OverlayInput>& out);

// From the thread that takes events to the thread that runs ImGui. While
// that thread builds no frames (no Lua state has an overlay yet, or the
// target is one the overlay is not drawn on) nothing takes the inputs out:
// the queue then keeps the newest and lets the oldest go.
class OverlayInputQueue
{
public:
    static constexpr size_t kMost = 1024;

    void Push(std::span<OverlayInput const> inputs);
    // Hands every queued input to `io`, in order. The caller is on the thread
    // that owns the ImGui context.
    void Replay(ImGuiIO& io);
    void Clear();
    size_t Size();

private:
    std::mutex mutex_;
    std::vector<OverlayInput> queued_;
};

// What ImGui said it wants after its last frame.
struct OverlayWants
{
    bool TextInput{ false };        // a text field has the keyboard
    bool Mouse{ false };            // the pointer is over, or held by, the overlay
};

// Whether the overlay takes the event from the game: Norbyte's rule
// (Extender/Client/SDL.cpp), keys while a text field of the overlay has the
// keyboard, the mouse while the overlay has the pointer; with two exceptions
// for how the Mac game takes its input. The overlay never takes a modifier
// key's change: the game keeps the state of each modifier from those changes
// alone (-[LSMTLView flagsChanged:] sets it from the event's flags, and
// nothing else does), so a modifier let go while a text field had the
// keyboard would stay down in the game for good. A key with Command held
// always reaches the application, whose shortcuts those are.
//
// The rule is the overlay's. A Lua handler is offered a modifier's change as
// the key event SDL makes of it and may prevent it like any other key, as on
// Windows; the game then does not see that change, there as here.
bool OverlayTakes(CocoaEvent const& event, OverlayWants wants);

// --- Norbyte's code ---------------------------------------------------------

// The SDL event SDL would have made of the event: a key (also for a modifier
// key's change), a mouse button, the wheel. False for an event Norbyte's code
// has no use for.
bool SdlEventOf(CocoaEvent const& event, uint32_t timestamp, SDL_Event& out);

// The SDL event for the drawable having changed its size.
SDL_Event SdlViewportResized(int32_t width, int32_t height, uint32_t timestamp);

// --- the game ---------------------------------------------------------------

// The change the game is queued for an SDL key event that Lua injects:
// the key's input id from the game's table of key codes, pressed or
// released as the view's own key methods build it. False when the event is
// no key event, the key has no key code, or the game has no input for it.
bool GameChangeOf(SDL_Event const& event, std::span<uint16_t const> keyboardKeys, GameInputChange& out);
// The same, for a keyboard that is an ISO one or not: the scancode names the
// key's place, as the SDL events made here do (Overlay/KeyCodes.h,
// PositionalKeyCode).
bool GameChangeOf(SDL_Event const& event, std::span<uint16_t const> keyboardKeys, bool isoKeyboard, GameInputChange& out);

// The changes for an injected key event, in the order the game is to be
// queued them: the key's change (GameChangeOf), and, when `withModifiers`,
// before it a key-down for each modifier key the event's keysym.mod names
// that `heldModifiers` (SDL's mask of what the game has been sent as held)
// does not, and after it the same keys going up, last first. The game keeps
// no modifier state but these keys (-[LSMTLView flagsChanged:] queues each
// modifier key as a key), so a key with modifiers is a key with its modifier
// keys held around it. Caps lock and the other states of the mask are not
// keys and are left out. Nothing when the key itself has no change.
void GameChangesOfInjected(SDL_Event const& event, std::span<uint16_t const> keyboardKeys, uint16_t heldModifiers, bool withModifiers,
    bool isoKeyboard, std::vector<GameInputChange>& out);

// Milliseconds since the overlay's input first asked: the time of the SDL
// events made here.
uint32_t InputTicks();

}
