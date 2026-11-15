#pragma once

#include <cstdint>
#include <string>

#include <Overlay/Dependencies.h>

// The pointer as the overlay asks for it: its shape, a move to where ImGui
// wants it, and its leaving the game's view.
//
// On Windows ImGui's SDL backend sets the cursor ImGui asks for each frame,
// moves the pointer when ImGui asks (io.WantSetMousePos), and takes
// SDL_WINDOWEVENT_LEAVE as the mouse having no position. The Mac game sets
// its own cursor (-[LSMTLView setCursor:] keeps it in the view, and
// cursorUpdate: sets it again), so the overlay sets its cursor only while it
// has the pointer, and gives the view's own back once it lets the pointer
// go. A hidden cursor is the overlay's own [NSCursor hide], undone by its own
// unhide: the game's hides are counted apart from it.
//
// The client thread asks before each ImGui frame (Overlay/OverlayState.h);
// the main thread applies it once a frame, before the game's frame is
// presented. The view's own tracking area only updates its cursor, so the
// overlay adds one for the pointer leaving the view; its event comes through
// the event tap as CocoaEventKind::MouseExited.

namespace bg3se::mac::overlay {

// OverlayState::MouseCursor: the overlay leaves the cursor as it is
// (ImGuiConfigFlags_NoMouseCursorChange, or no overlay).
inline constexpr int32_t kKeepCursor = -2;

enum class CursorStep : uint8_t
{
    Nothing,
    Show,           // the cursor ImGui asks for
    Hide,           // no cursor (ImGuiMouseCursor_None, or ImGui draws its own)
    GiveBack,       // the view's own cursor again, shown
};

// What the main thread does with the cursor this frame: the overlay's while
// it has the pointer and asks for one; the game's back, once, after it did.
CursorStep NextCursorStep(bool overlayHasPointer, int32_t cursor, bool overlayCursorShown);

// The pointer is the overlay's to set from now on. Empty when it is;
// otherwise why not.
std::string InstallPointer(Dependencies const& dependencies);
// Gives the game its cursor back and takes the tracking area off, on the
// main thread (at once when called there, soon otherwise).
void RemovePointer();
bool PointerInstalled();

// Main thread, once a frame: the client thread's requests made on the view
// (an LSMTLView*), whose tracking area is put on it first.
void ApplyPointer(void* view);

// Whether an NSTrackingArea is the one the overlay put on the view.
bool IsPointerTrackingArea(void* trackingArea);

// The NSCursor shown for an ImGui cursor; the arrow for one AppKit has no
// cursor for.
void* CursorFor(int32_t imguiCursor);

}
