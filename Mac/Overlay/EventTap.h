#pragma once

#include <string>

#include <Overlay/Dependencies.h>
#include <Overlay/InputEvents.h>

// Where the overlay's input takes the game's events: the implementation of
// -[BG3Application sendEvent:] is replaced, as Extender/Client/SDL.cpp wraps
// SDL_PollEvent. The game's main loop sends every event it takes from the
// queue through that method, on the main thread, so the replacement sees
// each one before the game does and decides whether the game gets it.
//
// The game's view is noted where it is made, by a replacement of
// -[LSMTLView initWithFrame:device:display:hdr:vsync:] that calls the game's
// method and looks at what it returns, as SDL.cpp notes the window in a hook
// after SDL_CreateWindow. The view gives mouse positions their scale.
//
// A method is replaced only where the class data and the engine table agree
// about it (Overlay/Dependencies.h), and only while the class still has the
// game's own code for it: otherwise nothing is replaced.
//
// The interface is plain C++; Objective-C objects cross it as void*.

namespace bg3se::mac::overlay {

class EventSink
{
public:
    virtual ~EventSink() = default;
    // Main thread, before the game has the event. True: the game gets it.
    virtual bool OnEvent(CocoaEvent const& event) = 0;
    // Main thread: the application became, or stopped being, the active one.
    virtual void OnFocus(bool active) = 0;
};

// Replaces the two methods. Empty when done; otherwise why nothing was
// replaced. `sink` must outlive the tap.
std::string InstallEventTap(Dependencies const& dependencies, EventSink* sink);
// Puts the game's methods back, where the replacements are still in place.
void RemoveEventTap();
bool EventTapInstalled();

// --- the pieces, for the tap and its tests ----------------------------------

// A method of a class given the implementation `replacement`, when the class's
// own implementation is `expected`. False, with the reason, otherwise.
bool ReplaceMethod(char const* className, char const* selector, void* expected, void* replacement, std::string& problem);
// The reverse: `original` back, when the implementation is `replacement`.
bool RestoreMethod(char const* className, char const* selector, void* replacement, void* original);

// What the input needs of an NSEvent. `view` is the game's view, or null when
// it is not known: mouse events then have no position. An event that belongs
// to another window than the view's is described as Other.
CocoaEvent DescribeEvent(void* nsEvent, void* nsView);

// The view the tap noted (an NSView*, not retained), or null. Main thread.
void* GameView();

// The geometry of a view of the game's (an LSMTLView*) for ToDrawablePixels
// and ToViewPoint: its size, and its drawable's. Main thread.
bool ViewGeometryOf(void* nsView, ViewGeometry& out);

}
