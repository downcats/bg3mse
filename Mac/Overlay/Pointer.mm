#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <Overlay/EventTap.h>
#include <Overlay/InputEvents.h>
#include <Overlay/OverlayState.h>
#include <Overlay/Pointer.h>

#include <imgui.h>

#include <atomic>
#include <cstring>
#include <pthread.h>

#if !__has_feature(objc_arc)
#error "Pointer.mm is written for automatic reference counting"
#endif

// The owner of the overlay's tracking area: AppKit sends it the area's
// events, which the event tap has seen already.
@interface OverlayPointerTracking : NSObject
@end

@implementation OverlayPointerTracking
- (void)mouseEntered:(NSEvent*)event {}
- (void)mouseExited:(NSEvent*)event {}
@end

namespace bg3se::mac::overlay {

namespace {

std::atomic<bool> gInstalled{ false };

// The main thread's.
OverlayPointerTracking* gOwner = nil;
NSTrackingArea* gArea = nil;
__weak NSView* gTracked = nil;
bool gShown = false;            // the overlay's cursor is on screen in place of the view's
bool gHidden = false;           // ... and it is none: the overlay's one [NSCursor hide]
NSCursor* gSet = nil;           // the cursor the overlay set last,
NSCursor* gCurrent = nil;       // ... and what AppKit said was current after it
uint32_t gWarpSeen = 0;

// An instance variable of the view, where it has the type the class data was
// checked for.
Ivar VariableOf(NSView* view, char const* name, char const* type)
{
    Ivar variable = class_getInstanceVariable(object_getClass(view), name);
    if (!variable) return nullptr;
    char const* found = ivar_getTypeEncoding(variable);
    return found && strcmp(found, type) == 0 ? variable : nullptr;
}

NSCursor* ViewCursor(NSView* view)
{
    Ivar variable = VariableOf(view, kViewCursorVariable, kViewCursorType);
    id cursor = variable ? object_getIvar(view, variable) : nil;
    return [cursor isKindOfClass:[NSCursor class]] ? (NSCursor*)cursor : nil;
}

bool PointerCaptured(NSView* view)
{
    Ivar variable = VariableOf(view, kViewCaptureVariable, kViewCaptureType);
    if (!variable) return true;     // not known: not moved
    auto bytes = reinterpret_cast<uint8_t const*>((__bridge void const*)view);
    return bytes[ivar_getOffset(variable)] != 0;
}

void Unhide()
{
    if (!gHidden) return;
    gHidden = false;
    [NSCursor unhide];
}

void Track(NSView* view)
{
    NSView* tracked = gTracked;
    if (tracked == view && gArea) return;
    if (tracked && gArea) [tracked removeTrackingArea:gArea];
    if (!gOwner) gOwner = [[OverlayPointerTracking alloc] init];
    gArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                         options:NSTrackingMouseEnteredAndExited | NSTrackingActiveInActiveApp | NSTrackingInVisibleRect
                                           owner:gOwner
                                        userInfo:nil];
    [view addTrackingArea:gArea];
    gTracked = view;
}

// What ImGui_ImplSDL2_UpdateMouseData does when ImGui asks: the pointer at
// the point of the drawable, unless the game holds it.
void Warp(NSView* view, float x, float y)
{
    if (PointerCaptured(view)) return;
    NSWindow* window = view.window;
    NSScreen* main = NSScreen.screens.firstObject;
    ViewGeometry geometry;
    double viewX, viewY;
    if (!window || !main || !ViewGeometryOf((__bridge void*)view, geometry) || !ToViewPoint(geometry, x, y, viewX, viewY)) return;
    NSPoint inWindow = [view convertPoint:NSMakePoint(viewX, viewY) toView:nil];
    NSPoint onScreen = [window convertPointToScreen:inWindow];
    // The global display space runs down from the top of the main display.
    CGWarpMouseCursorPosition(CGPointMake(onScreen.x, main.frame.size.height - onScreen.y));
    // As SDL's Cocoa_WarpMouseGlobal: without it, the pointer stands still for a moment after the move.
    CGAssociateMouseAndMouseCursorPosition(true);
}

void TakeOff()
{
    if (gShown) {
        Unhide();
        NSView* view = gTracked;
        NSCursor* own = view ? ViewCursor(view) : nil;
        [own ?: NSCursor.arrowCursor set];
        gShown = false;
        gSet = gCurrent = nil;
    }
    NSView* tracked = gTracked;
    if (tracked && gArea) [tracked removeTrackingArea:gArea];
    gArea = nil;
    gTracked = nil;
}

NSCursor* MakeCursor(int32_t imguiCursor)
{
    NSCursor* cursor = nil;
    switch (imguiCursor) {
    case ImGuiMouseCursor_TextInput: cursor = NSCursor.IBeamCursor; break;
    case ImGuiMouseCursor_ResizeAll: cursor = NSCursor.closedHandCursor; break;
    case ImGuiMouseCursor_ResizeNS: cursor = NSCursor.resizeUpDownCursor; break;
    case ImGuiMouseCursor_ResizeEW: cursor = NSCursor.resizeLeftRightCursor; break;
    case ImGuiMouseCursor_ResizeNESW:
    case ImGuiMouseCursor_ResizeNWSE:
        if (@available(macOS 15.0, *)) {
            auto position = imguiCursor == ImGuiMouseCursor_ResizeNESW ? NSCursorFrameResizePositionTopRight : NSCursorFrameResizePositionTopLeft;
            cursor = [NSCursor frameResizeCursorFromPosition:position inDirections:NSCursorFrameResizeDirectionsAll];
        } else {
            cursor = NSCursor.closedHandCursor;     // as imgui_impl_osx.mm without the private cursors
        }
        break;
    case ImGuiMouseCursor_Hand: cursor = NSCursor.pointingHandCursor; break;
    case ImGuiMouseCursor_NotAllowed: cursor = NSCursor.operationNotAllowedCursor; break;
    default: cursor = NSCursor.arrowCursor; break;      // the arrow, and the waits AppKit has no cursor for
    }
    return cursor;
}

}

CursorStep NextCursorStep(bool overlayHasPointer, int32_t cursor, bool overlayCursorShown)
{
    if (overlayHasPointer && cursor != kKeepCursor) return cursor == ImGuiMouseCursor_None ? CursorStep::Hide : CursorStep::Show;
    return overlayCursorShown ? CursorStep::GiveBack : CursorStep::Nothing;
}


void* CursorFor(int32_t imguiCursor)
{
    // Made once, as imgui_impl_osx.mm makes them: AppKit makes some cursors
    // (the I-beam) anew each time it is asked, and has none (nil) before
    // the process has its NSApplication.
    static NSCursor* cursors[ImGuiMouseCursor_COUNT];
    if (imguiCursor < 0 || imguiCursor >= ImGuiMouseCursor_COUNT) imguiCursor = ImGuiMouseCursor_Arrow;
    if (!cursors[imguiCursor]) cursors[imguiCursor] = MakeCursor(imguiCursor);
    NSCursor* cursor = cursors[imguiCursor] ?: NSCursor.arrowCursor;
    return (__bridge void*)cursor;
}

std::string InstallPointer(Dependencies const& d)
{
    if (!d.Live) return "the dependencies were not checked on the running game";
    if (!d.Usable(Part::Events)) return d.Problem(Part::Events);
    if (!d.Usable(Part::Pointer)) return d.Problem(Part::Pointer);
    gInstalled = true;
    return {};
}

void RemovePointer()
{
    if (!gInstalled.exchange(false)) return;
    if (pthread_main_np()) TakeOff();
    else dispatch_async(dispatch_get_main_queue(), ^{
        if (!gInstalled) TakeOff();
    });
}

bool PointerInstalled()
{
    return gInstalled;
}

void ApplyPointer(void* nsView)
{
    NSView* view = (__bridge NSView*)nsView;
    if (!gInstalled || !view) return;
    Track(view);

    auto& state = State();
    int32_t cursor = state.MouseCursor.load();
    bool hasPointer = state.Presenting.load() && state.WantCaptureMouse.load();
    switch (NextCursorStep(hasPointer, cursor, gShown)) {
    case CursorStep::Show: {
        Unhide();
        NSCursor* wanted = (__bridge NSCursor*)CursorFor(cursor);
        // -[NSCursor set] costs; set again when another cursor was set since
        // (the view's cursorUpdate: sets its own).
        NSCursor* set = gSet;
        if (set != wanted || NSCursor.currentCursor != gCurrent) {
            [wanted set];
            gSet = wanted;
            gCurrent = NSCursor.currentCursor;
        }
        gShown = true;
        break;
    }
    case CursorStep::Hide:
        if (!gHidden) {
            gHidden = true;
            [NSCursor hide];
        }
        gShown = true;
        break;
    case CursorStep::GiveBack: {
        Unhide();
        NSCursor* own = ViewCursor(view);
        [own ?: NSCursor.arrowCursor set];
        gShown = false;
        gSet = gCurrent = nil;
        break;
    }
    case CursorStep::Nothing:
        break;
    }

    uint32_t serial = state.WarpSerial.load(std::memory_order_acquire);
    if (serial != gWarpSeen) {
        gWarpSeen = serial;
        float x, y;
        state.WarpPoint(x, y);
        Warp(view, x, y);
    }
}

bool IsPointerTrackingArea(void* trackingArea)
{
    return trackingArea && trackingArea == (__bridge void*)gArea;
}

}
