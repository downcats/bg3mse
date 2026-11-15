#import <AppKit/AppKit.h>

#include <Overlay/EventTap.h>
#include <Overlay/OverlayState.h>
#include <Overlay/TextInput.h>
#include <Platform/Log.h>

#include <atomic>
#include <pthread.h>

#if !__has_feature(objc_arc)
#error "TextInput.mm is written for automatic reference counting"
#endif

namespace bg3se::mac::overlay {
namespace {

std::atomic<bool> gInstalled{ false };
std::atomic<TextSink*> gSink{ nullptr };

}
}

using namespace bg3se::mac::overlay;

// The overlay's text client. Its marked text is the input method's alone:
// ImGui has no composition of its own to show it in, as with SDL's.
@interface OverlayTextClient : NSView <NSTextInputClient>
@property (nonatomic, weak) NSView* gameView;       // where what the client is sent and does not take goes on to
@end

@implementation OverlayTextClient
{
    NSString* _marked;
    NSRange _selected;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

// What the window sends the first responder that the overlay let through
// to the game: the game's view has it, as it would have.
- (void)keyDown:(NSEvent*)event
{
    [self.gameView keyDown:event];
}

- (void)keyUp:(NSEvent*)event
{
    [self.gameView keyUp:event];
}

- (void)flagsChanged:(NSEvent*)event
{
    [self.gameView flagsChanged:event];
}

- (void)insertText:(id)text replacementRange:(NSRange)replacementRange
{
    NSString* string = [text isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString*)text string] : (NSString*)text;
    // The composition is committed.
    if (_marked) [self unmarkText];
    char const* utf8 = string.UTF8String;
    if (TextSink* sink = gSink.load(); sink && utf8 && *utf8) sink->OnText(utf8);
}

// Return, tab, the arrows: ImGui has them as keys already.
- (void)doCommandBySelector:(SEL)selector
{
}

- (void)setMarkedText:(id)text selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange
{
    NSString* string = [text isKindOfClass:[NSAttributedString class]] ? [(NSAttributedString*)text string] : (NSString*)text;
    if (string.length == 0) {
        [self unmarkText];
        return;
    }
    _marked = [string copy];
    _selected = selectedRange;
}

- (void)unmarkText
{
    _marked = nil;
    _selected = NSMakeRange(0, 0);
}

- (BOOL)hasMarkedText
{
    return _marked != nil;
}

- (NSRange)markedRange
{
    return _marked ? NSMakeRange(0, _marked.length) : NSMakeRange(NSNotFound, 0);
}

- (NSRange)selectedRange
{
    return _selected;
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText
{
    return @[];
}

- (NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(NSRangePointer)actualRange
{
    return nil;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point
{
    return NSNotFound;
}

// The input method's window: below ImGui's text cursor, in screen points.
- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(NSRangePointer)actualRange
{
    if (actualRange) *actualRange = range;
    NSView* view = self.gameView;
    NSWindow* window = view.window;
    if (!window) return NSZeroRect;
    auto const& state = State();
    float x, y;
    state.ImePoint(x, y);
    ViewGeometry geometry;
    double viewX, viewY, width, height;
    if (!ViewGeometryOf((__bridge void*)view, geometry)
        || !ImeRectInView(geometry, x, y, state.ImeLineHeight.load(), viewX, viewY, width, height)) {
        return [window convertRectToScreen:[view convertRect:view.bounds toView:nil]];
    }
    NSRect inView = NSMakeRect(viewX, viewY, width, height);
    return [window convertRectToScreen:[view convertRect:inView toView:nil]];
}

@end

namespace bg3se::mac::overlay {

namespace {

OverlayTextClient* gClient = nil;
__weak NSResponder* gGivenBack = nil;     // the responder the client took the window's first place from
bool gActive = false;
bool gSaidRefused = false;

OverlayTextClient* Client()
{
    if (!gClient) gClient = [[OverlayTextClient alloc] initWithFrame:NSZeroRect];
    return gClient;
}

void Deactivate()
{
    if (!gActive) return;
    gActive = false;
    OverlayTextClient* client = Client();
    [client.inputContext discardMarkedText];
    [client unmarkText];
    NSWindow* window = client.window;
    if (window && window.firstResponder == client) {
        NSResponder* back = gGivenBack;
        NSView* game = client.gameView;
        // Its own responder back, when it is still of the window; the game's view otherwise.
        bool inWindow = [back isKindOfClass:[NSView class]] && ((NSView*)back).window == window;
        [window makeFirstResponder:inWindow ? back : (game.window == window ? game : nil)];
    }
    [client removeFromSuperview];
    gGivenBack = nil;
}

void Activate(NSView* view)
{
    OverlayTextClient* client = Client();
    NSWindow* window = view.window;
    if (!window) return;
    client.gameView = view;
    if (client.superview != view) {
        [client removeFromSuperview];
        [view addSubview:client];
    }
    NSResponder* before = window.firstResponder;
    if (![window makeFirstResponder:client]) {
        [client removeFromSuperview];
        if (!gSaidRefused) {
            gSaidRefused = true;
            Log(LogLevel::Warning, "overlay: the game's window does not let the overlay's text client be its first responder; the overlay "
                "takes a key's characters as they are, without dead keys and input methods");
        }
        return;
    }
    gGivenBack = before;
    gActive = true;
}

}

std::string InstallTextInput(Dependencies const& d, TextSink* sink)
{
    if (!sink) return "no sink for the text";
    if (!d.Live) return "the dependencies were not checked on the running game";
    if (!d.Usable(Part::Events)) return d.Problem(Part::Events);
    gSink.store(sink);
    // As SDL's application registers it (SDL_cocoaevents.m): a held key
    // repeats in a text field, as it does on Windows, instead of opening the
    // accent menu. In this process's registration domain only: the user's
    // defaults stay as they are.
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{ @"ApplePressAndHoldEnabled" : @NO }];
    gInstalled = true;
    return {};
}

void RemoveTextInput()
{
    if (!gInstalled.exchange(false)) return;
    if (pthread_main_np()) Deactivate();
    else dispatch_async(dispatch_get_main_queue(), ^{
        if (!gInstalled) Deactivate();
    });
}

bool TextInputInstalled()
{
    return gInstalled;
}

void ApplyTextInput(void* nsView)
{
    NSView* view = (__bridge NSView*)nsView;
    auto const& state = State();
    bool wanted = gInstalled && view && view.window && state.Presenting.load() && state.WantTextInput.load();
    OverlayTextClient* client = gClient;
    // Another view, or another window: the client goes with it.
    if (gActive && (!wanted || client.gameView != view || client.window != view.window || client.window.firstResponder != client)) Deactivate();
    if (wanted && !gActive) Activate(view);
}

bool TextInputActive()
{
    return gActive;
}

bool TextInputComposing()
{
    return gActive && [Client() hasMarkedText];
}

void InterpretKey(void* nsEvent)
{
    NSEvent* event = (__bridge NSEvent*)nsEvent;
    if (!gActive || !event) return;
    [Client() interpretKeyEvents:@[ event ]];
}

void* TextClient()
{
    return (__bridge void*)Client();
}

bool ImeRectInView(ViewGeometry const& geometry, float x, float y, float lineHeight, double& viewX, double& viewY, double& width,
    double& height)
{
    double topX, topY, bottomX, bottomY;
    if (!(lineHeight >= 0) || !ToViewPoint(geometry, x, y, topX, topY) || !ToViewPoint(geometry, x, y + lineHeight, bottomX, bottomY)) {
        return false;
    }
    viewX = topX;
    viewY = std::min(topY, bottomY);
    width = 1;
    height = std::abs(topY - bottomY);
    return true;
}

}
