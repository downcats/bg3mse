#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include <Overlay/EventTap.h>
#include <Overlay/KeyCodes.h>
#include <Overlay/KeyboardLayout.h>
#include <Overlay/OverlayState.h>
#include <Overlay/Pointer.h>
#include <Overlay/TextInput.h>
#include <Platform/Log.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

#if !__has_feature(objc_arc)
#error "EventTap.mm is written for automatic reference counting"
#endif

namespace bg3se::mac::overlay {

namespace {

using SendEventFn = void (*)(id self, SEL selector, NSEvent* event);
// An initialiser takes over the reference to self and returns one.
typedef id (*ViewInitFn)(__attribute__((ns_consumed)) id self, SEL selector, CGRect frame, id device, int display, BOOL hdr, BOOL vsync)
    __attribute__((ns_returns_retained));

std::atomic<EventSink*> gSink{ nullptr };
std::atomic<SendEventFn> gSendEvent{ nullptr };
std::atomic<ViewInitFn> gViewInit{ nullptr };
std::atomic<bool> gInstalled{ false };
id gActiveObserver = nil, gResignObserver = nil;

// The game's view. Weak: the game owns it, and may make another.
__weak NSView* gView = nil;
// Events to let pass before the windows are searched for the view again.
unsigned gSearchIn = 0;
// Whether the log has said where the view came from, once for each way.
std::atomic<bool> gToldInitialiser{ false }, gToldSearch{ false };

// The class's own method for the selector (not an inherited one), or null.
Method OwnMethod(Class cls, SEL selector)
{
    unsigned count = 0;
    Method* methods = class_copyMethodList(cls, &count);
    Method found = nullptr;
    for (unsigned i = 0; i < count && !found; i++) {
        if (sel_isEqual(method_getName(methods[i]), selector)) found = methods[i];
    }
    free(methods);
    return found;
}

NSView* SearchFor(NSView* view, Class cls)
{
    if (!view) return nil;
    if ([view isKindOfClass:cls]) return view;
    for (NSView* child in view.subviews) {
        if (NSView* found = SearchFor(child, cls)) return found;
    }
    return nil;
}

// The view, when it was made before its initialiser was replaced.
NSView* FindGameView()
{
    if (gSearchIn) {
        gSearchIn--;
        return nil;
    }
    gSearchIn = 120;
    Class cls = objc_lookUpClass(kLSMTLView);
    if (!cls || !NSApp) return nil;
    for (NSWindow* window in NSApp.windows) {
        if (NSView* found = SearchFor(window.contentView, cls)) {
            gView = found;
            if (!gToldSearch.exchange(true)) {
                Log(LogLevel::Info, "overlay: the game's view was found among its windows (it was made before the overlay's hooks went in)");
            }
            return found;
        }
    }
    return nil;
}

// The view's Metal layer, through the instance variable the class data was
// checked for; nil for a view without it.
CAMetalLayer* LayerOf(NSView* view)
{
    Ivar variable = class_getInstanceVariable(object_getClass(view), kViewLayerVariable);
    if (!variable) return nil;
    char const* type = ivar_getTypeEncoding(variable);
    if (!type || strcmp(type, kViewLayerType) != 0) return nil;
    id layer = object_getIvar(view, variable);
    return [layer isKindOfClass:[CAMetalLayer class]] ? (CAMetalLayer*)layer : nil;
}

ViewGeometry GeometryOf(NSView* view)
{
    ViewGeometry geometry;
    geometry.ViewWidth = view.bounds.size.width;
    geometry.ViewHeight = view.bounds.size.height;
    geometry.Flipped = view.isFlipped;
    // The drawable is the size the game chose: the layer's own word for it,
    // and before a layer is known, the size of the drawable last presented.
    if (CAMetalLayer* layer = LayerOf(view)) {
        geometry.DrawableWidth = layer.drawableSize.width;
        geometry.DrawableHeight = layer.drawableSize.height;
    } else {
        geometry.DrawableWidth = State().DrawableWidth();
        geometry.DrawableHeight = State().DrawableHeight();
    }
    return geometry;
}

bool PositionOf(NSEvent* event, NSView* view, float& x, float& y)
{
    NSWindow* window = view.window;
    if (!window) return false;
    NSPoint point = event.locationInWindow;
    if (!event.window) point = [window convertPointFromScreen:point];
    point = [view convertPoint:point fromView:nil];
    return ToDrawablePixels(GeometryOf(view), point.x, point.y, x, y);
}

CocoaEvent Describe(NSEvent* event, NSView* view)
{
    CocoaEvent described;
    // An event of another window (a panel, an alert) is not the game view's.
    if (view && event.window && event.window != view.window) return described;

    switch (event.type) {
    case NSEventTypeKeyDown:
    case NSEventTypeKeyUp:
        described.Kind = event.type == NSEventTypeKeyDown ? CocoaEventKind::KeyDown : CocoaEventKind::KeyUp;
        described.KeyCode = event.keyCode;
        described.Repeat = event.isARepeat;
        described.LayoutCharacter = KeyboardLayout::Current().CharacterOf(event.keyCode);
        described.IsoKeyboard = KeyboardLayout::Current().Iso();
        if (event.type == NSEventTypeKeyDown) {
            if (char const* text = event.characters.UTF8String) strlcpy(described.Text, text, sizeof(described.Text));
        }
        break;
    case NSEventTypeFlagsChanged:
        described.Kind = CocoaEventKind::FlagsChanged;
        described.KeyCode = event.keyCode;
        break;
    case NSEventTypeLeftMouseDown:
    case NSEventTypeRightMouseDown:
    case NSEventTypeOtherMouseDown:
    case NSEventTypeLeftMouseUp:
    case NSEventTypeRightMouseUp:
    case NSEventTypeOtherMouseUp: {
        bool down = event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeRightMouseDown || event.type == NSEventTypeOtherMouseDown;
        described.Kind = down ? CocoaEventKind::MouseDown : CocoaEventKind::MouseUp;
        // The event's type says left or right, as it says which of the view's
        // methods the game is sent; only another button has to be asked for.
        bool left = event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeLeftMouseUp;
        bool right = event.type == NSEventTypeRightMouseDown || event.type == NSEventTypeRightMouseUp;
        described.Button = left ? 0 : right ? 1 : (int32_t)event.buttonNumber;
        described.Clicks = (int32_t)event.clickCount;
        break;
    }
    case NSEventTypeMouseMoved:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeOtherMouseDragged:
        described.Kind = CocoaEventKind::MouseMoved;
        break;
    case NSEventTypeMouseExited:
        // Only the overlay's own area says that the pointer left the view.
        if (!IsPointerTrackingArea((__bridge void*)event.trackingArea)) return described;
        described.Kind = CocoaEventKind::MouseExited;
        described.PressedButtons = (uint32_t)NSEvent.pressedMouseButtons;
        break;
    case NSEventTypeScrollWheel:
        described.Kind = CocoaEventKind::ScrollWheel;
        described.ScrollX = (float)event.scrollingDeltaX;
        described.ScrollY = (float)event.scrollingDeltaY;
        described.PreciseScroll = event.hasPreciseScrollingDeltas;
        described.ScrollCancelled = event.phase == NSEventPhaseCancelled;
        break;
    default:
        return described;
    }
    described.ModifierFlags = (uint32_t)event.modifierFlags;
    bool mouse = described.Kind == CocoaEventKind::MouseDown || described.Kind == CocoaEventKind::MouseUp
        || described.Kind == CocoaEventKind::MouseMoved || described.Kind == CocoaEventKind::ScrollWheel;
    if (mouse && view) described.HasPosition = PositionOf(event, view, described.X, described.Y);
    return described;
}

// --- the replacements -------------------------------------------------------

void SendEvent(id self, SEL selector, NSEvent* event)
{
    SendEventFn original = gSendEvent.load();
    EventSink* sink = gSink.load();
    bool deliver = true;
    if (sink && event) {
        @try {
            NSView* view = gView;
            if (!view) view = FindGameView();
            CocoaEvent described = Describe(event, view);
            // A key-down for the overlay's text client: its text comes from the input system.
            bool forText = described.Kind == CocoaEventKind::KeyDown && TextInputActive() && !(described.ModifierFlags & kFlagCommand);
            if (forText) {
                described.TextByInputSystem = true;
                described.Composing = TextInputComposing();
            }
            if (described.Kind != CocoaEventKind::Other) deliver = sink->OnEvent(described);
            // Taken from the game for the overlay's text field: the input system has it.
            if (forText && !deliver) InterpretKey((__bridge void*)event);
        } @catch (NSException*) {
            // An event that cannot be described is the game's as it is.
            deliver = true;
        }
    }
    if (deliver) original(self, selector, event);
}

__attribute__((ns_returns_retained))
id ViewInit(__attribute__((ns_consumed)) id self, SEL selector, CGRect frame, id device, int display, BOOL hdr, BOOL vsync)
{
    id view = gViewInit.load()(self, selector, frame, device, display, hdr, vsync);
    if ([view isKindOfClass:[NSView class]]) {
        gView = (NSView*)view;
        if (!gToldInitialiser.exchange(true)) Log(LogLevel::Info, "overlay: the game's view came from its initialiser, which the overlay replaces");
    }
    return view;
}

}

bool ReplaceMethod(char const* className, char const* selector, void* expected, void* replacement, std::string& problem)
{
    Class cls = objc_lookUpClass(className);
    if (!cls) {
        problem = std::string("there is no class ") + className;
        return false;
    }
    Method method = OwnMethod(cls, sel_registerName(selector));
    if (!method) {
        problem = std::string(className) + " has no method " + selector + " of its own";
        return false;
    }
    if (!expected || (void*)method_getImplementation(method) != expected) {
        problem = std::string("-[") + className + " " + selector + "] is no longer implemented by the game's code";
        return false;
    }
    method_setImplementation(method, (IMP)replacement);
    return true;
}

bool RestoreMethod(char const* className, char const* selector, void* replacement, void* original)
{
    Class cls = objc_lookUpClass(className);
    Method method = cls ? OwnMethod(cls, sel_registerName(selector)) : nullptr;
    if (!method || (void*)method_getImplementation(method) != replacement) return false;
    method_setImplementation(method, (IMP)original);
    return true;
}

CocoaEvent DescribeEvent(void* nsEvent, void* nsView)
{
    if (!nsEvent) return {};
    return Describe((__bridge NSEvent*)nsEvent, (__bridge NSView*)nsView);
}

bool ViewGeometryOf(void* nsView, ViewGeometry& out)
{
    NSView* view = (__bridge NSView*)nsView;
    if (!view) return false;
    out = GeometryOf(view);
    return true;
}

void* GameView()
{
    NSView* view = gView;
    return (__bridge void*)view;
}

std::string InstallEventTap(Dependencies const& dependencies, EventSink* sink)
{
    if (gInstalled) return {};
    if (!sink) return "no sink for the events";
    if (!dependencies.Live) return "the dependencies were not checked on the running game";
    if (!dependencies.Usable(Part::Events)) return dependencies.Problem(Part::Events);

    // The originals first: a replacement may run as soon as it is in place.
    gSink.store(sink);
    gSendEvent.store((SendEventFn)(uintptr_t)dependencies.SendEvent);
    gViewInit.store((ViewInitFn)(uintptr_t)dependencies.ViewInit);

    std::string problem;
    if (!ReplaceMethod(kLSMTLView, kViewInitSelector, (void*)(uintptr_t)dependencies.ViewInit, (void*)&ViewInit, problem)) {
        gSink.store(nullptr);
        return problem;
    }
    if (!ReplaceMethod(kBG3Application, kSendEventSelector, (void*)(uintptr_t)dependencies.SendEvent, (void*)&SendEvent, problem)) {
        RestoreMethod(kLSMTLView, kViewInitSelector, (void*)&ViewInit, (void*)(uintptr_t)dependencies.ViewInit);
        gSink.store(nullptr);
        return problem;
    }

    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    gActiveObserver = [center addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification*) {
        if (EventSink* current = gSink.load()) current->OnFocus(true);
    }];
    gResignObserver = [center addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification*) {
        if (EventSink* current = gSink.load()) current->OnFocus(false);
    }];
    gInstalled = true;
    return {};
}

void RemoveEventTap()
{
    if (!gInstalled) return;
    gInstalled = false;
    RestoreMethod(kBG3Application, kSendEventSelector, (void*)&SendEvent, (void*)gSendEvent.load());
    RestoreMethod(kLSMTLView, kViewInitSelector, (void*)&ViewInit, (void*)gViewInit.load());
    NSNotificationCenter* center = [NSNotificationCenter defaultCenter];
    if (gActiveObserver) [center removeObserver:gActiveObserver];
    if (gResignObserver) [center removeObserver:gResignObserver];
    gActiveObserver = gResignObserver = nil;
    // A replacement that is running has the sink already and goes on to the
    // game's method: the originals stay set for good.
    gSink.store(nullptr);
    gView = nil;
}

bool EventTapInstalled()
{
    return gInstalled;
}

}
