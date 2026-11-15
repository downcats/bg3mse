#pragma once

#include <string>

#include <Overlay/Dependencies.h>
#include <Overlay/InputEvents.h>

// The overlay's text on the Mac's text input system: dead keys, Option
// compositions and input methods, with their candidate window where ImGui
// says the text cursor is.
//
// On Windows SDL gives ImGui the text the input system composes
// (SDL_TEXTINPUT), and ImGui's SDL backend places the input method's window
// (ImGui_ImplSDL2_PlatformSetImeData). The Mac game's view is no text client
// (-[LSMTLView keyDown:] reads the event's characters), so the overlay has
// one of its own, as SDL's Cocoa code has (SDL_cocoakeyboard.m): a view of
// no size in the game's view, the window's first responder while a text
// field of the overlay has the keyboard, and the game's view again after.
// The event tap hands it each key-down the overlay takes for text
// (-[NSResponder interpretKeyEvents:]); what the input system makes of the
// keys comes back as committed text, or as marked text, the composition the
// input method still holds, during which a key is the input method's and not
// ImGui's. What the window still sends its first responder while the client
// is it (a key with Command, a key up or modifier change the overlay lets
// through) goes on to the game's view.
//
// Main thread, all of it.

namespace bg3se::mac::overlay {

class TextSink
{
public:
    virtual ~TextSink() = default;
    // Text the input system committed, UTF-8.
    virtual void OnText(char const* utf8) = 0;
};

// The text client is the overlay's from now on. Empty when it is; otherwise
// why not. `sink` must outlive it.
std::string InstallTextInput(Dependencies const& dependencies, TextSink* sink);
// The client is no longer the first responder and leaves the view (at once
// on the main thread, soon on another).
void RemoveTextInput();
bool TextInputInstalled();

// Once a frame: the client is the first responder of the view's window while
// a text field of the overlay has the keyboard, and gives it back to the
// responder it took it from when none has any more.
void ApplyTextInput(void* view);

// The client is the first responder: key-downs are its.
bool TextInputActive();
// The input method holds marked text: the next key is the input method's.
bool TextInputComposing();
// A key-down (an NSEvent*) for the client: its text and marked text follow.
void InterpretKey(void* nsEvent);

// The client itself (an NSView<NSTextInputClient>*), for the tests.
void* TextClient();

// Where the input method's window goes for ImGui's text cursor at (x, y),
// `lineHeight` high, in the drawable's pixels: the rectangle of the view's
// points from the line's bottom left, one point wide.
bool ImeRectInView(ViewGeometry const& geometry, float x, float y, float lineHeight, double& viewX, double& viewY, double& width,
    double& height);

}
