// The overlay tests that need Objective-C objects: the class checks against
// the runtime, the replacement of methods, the description of events, the
// frame tracker and the Metal pass (on a texture of the test's own), and the
// frame hooks on functions of the test library. The two classes the overlay
// names in the game are declared here with the game's method signatures: this
// executable is not the game, and the names are free in it.

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <Overlay/ColourSpace.h>
#include <Overlay/ConsoleWindow.h>
#include <Overlay/ControllerHooks.h>
#include <Overlay/Dependencies.h>
#include <Overlay/EventTap.h>
#include <Overlay/FrameHooks.h>
#include <Overlay/KeyCodes.h>
#include <Overlay/KeyboardLayout.h>
#include <Overlay/MetalOverlay.h>
#include <Overlay/OverlayState.h>
#include <Overlay/Pointer.h>
#include <Overlay/TextInput.h>
#include <Platform/EntryHook.h>
#include <Platform/Log.h>
#include <Platform/Symbols.h>

#include <imgui.h>
#include <imgui_internal.h>

// Norbyte's mirrors of ImGui's textures, as the backend uses them
// (Tests/MirrorStandIn has the one header of his the file asks for).
#include <climits>
#include "../../BG3Extender/Extender/Client/IMGUI/TextureMirror.inl"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "OverlayTests.h"

using namespace bg3se::mac;
using namespace bg3se::mac::overlay;

// --- stand-ins for the game's two classes -----------------------------------

@interface BG3Application : NSObject
@property (nonatomic, strong) NSEvent* received;
- (void)sendEvent:(NSEvent*)event;
@end

@implementation BG3Application
- (void)sendEvent:(NSEvent*)event
{
    self.received = event;
}
@end

@interface OverlayTestDerivedApplication : BG3Application
@end

@implementation OverlayTestDerivedApplication
@end

@interface LSMTLView : NSView
{
    CAMetalLayer* metalLayer;
@public
    NSCursor* CurrentCursor;
    BOOL captureCursor;
}
- (instancetype)initWithFrame:(CGRect)frame device:(id)device display:(int)display hdr:(BOOL)hdr vsync:(BOOL)vsync;
@end

@interface LSMTLView ()
@property (nonatomic) int keysDown, keysUp, flagChanges;
@end

@implementation LSMTLView
- (instancetype)initWithFrame:(CGRect)frame device:(id)device display:(int)display hdr:(BOOL)hdr vsync:(BOOL)vsync
{
    if ((self = [super initWithFrame:frame])) {
        metalLayer = [CAMetalLayer layer];
        metalLayer.drawableSize = CGSizeMake(frame.size.width * 2, frame.size.height * 2);
    }
    return self;
}
// As the game's view: the window's first responder, which takes the keys.
- (BOOL)acceptsFirstResponder { return YES; }
- (void)keyDown:(NSEvent*)event { self.keysDown++; }
- (void)keyUp:(NSEvent*)event { self.keysUp++; }
- (void)flagsChanged:(NSEvent*)event { self.flagChanges++; }
@end

// A drawable of the test's own: a texture, and a layer or none. Its layer is
// at first one of the game's standard target, in Display P3.
@interface OverlayTestDrawable : NSObject <CAMetalDrawable>
@property (nonatomic, strong) id<MTLTexture> texture;
@property (nonatomic, strong) CAMetalLayer* layer;
@end

// A layer in the named colour space, asking for extended range or not.
static CAMetalLayer* OverlayTestLayer(CFStringRef spaceName, BOOL extended)
{
    CAMetalLayer* layer = [CAMetalLayer layer];
    if (spaceName) {
        CGColorSpaceRef space = CGColorSpaceCreateWithName(spaceName);
        layer.colorspace = space;
        CGColorSpaceRelease(space);
    }
    layer.wantsExtendedDynamicRangeContent = extended;
    return layer;
}

@implementation OverlayTestDrawable
- (instancetype)init
{
    if ((self = [super init])) _layer = OverlayTestLayer(kCGColorSpaceDisplayP3, NO);
    return self;
}
- (void)present {}
- (void)presentAtTime:(CFTimeInterval)presentationTime {}
- (void)presentAfterMinimumDuration:(CFTimeInterval)duration {}
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)block {}
- (CFTimeInterval)presentedTime { return 0; }
- (NSUInteger)drawableID { return 0; }
@end

namespace {

bool Contains(std::string const& text, char const* part) { return text.find(part) != std::string::npos; }

uint64_t ImplementationOf(Class cls, SEL selector)
{
    return (uint64_t)(uintptr_t)method_getImplementation(class_getInstanceMethod(cls, selector));
}

// --- the classes, from the runtime ------------------------------------------

TEST(the_runtime_states_classes_as_the_checks_expect)
{
    ObjcClassesOfRuntime classes;
    CHECK(classes.HasClass(kBG3Application) && classes.HasClass(kLSMTLView) && !classes.HasClass("OverlayTestNoSuchClass"));

    // What the compiler makes of the game's two signatures is what the checks expect of the game.
    auto send = classes.InstanceMethod(kBG3Application, kSendEventSelector);
    CHECK(send.Found && send.Own && send.Types == kSendEventTypes);
    CHECK(send.Implementation == ImplementationOf([BG3Application class], @selector(sendEvent:)));
    auto init = classes.InstanceMethod(kLSMTLView, kViewInitSelector);
    CHECK(init.Found && init.Own && init.Types == kViewInitTypes);

    // A method a class only inherits is not its own.
    auto inherited = classes.InstanceMethod("OverlayTestDerivedApplication", kSendEventSelector);
    CHECK(inherited.Found && !inherited.Own);
    CHECK(!classes.InstanceMethod(kBG3Application, "noSuchMethod:").Found);

    auto layer = classes.InstanceVariable(kLSMTLView, kViewLayerVariable);
    CHECK(layer.Found && layer.Type == kViewLayerType && layer.Offset > 0);
    auto cursor = classes.InstanceVariable(kLSMTLView, kViewCursorVariable);
    CHECK(cursor.Found && cursor.Type == kViewCursorType);
    auto capture = classes.InstanceVariable(kLSMTLView, kViewCaptureVariable);
    CHECK(capture.Found && capture.Type == kViewCaptureType);
    CHECK(!classes.InstanceVariable(kLSMTLView, "noSuchVariable").Found);

    CHECK(classes.DerivesFrom(kLSMTLView, "NSView") == std::optional<bool>(true));
    CHECK(classes.DerivesFrom(kBG3Application, "NSView") == std::optional<bool>(false));
    CHECK(classes.DerivesFrom("OverlayTestDerivedApplication", kBG3Application) == std::optional<bool>(true));

    CHECK(CheckReplacedMethod(classes, Part::Events, kBG3Application, kSendEventSelector, kSendEventTypes, send.Implementation).Passed);
    CHECK(!CheckReplacedMethod(classes, Part::Events, kBG3Application, kSendEventSelector, kSendEventTypes, send.Implementation + 4).Passed);
    CHECK(!CheckReplacedMethod(classes, Part::Events, "OverlayTestDerivedApplication", kSendEventSelector, kSendEventTypes, send.Implementation).Passed);
    CHECK(!CheckReplacedMethod(classes, Part::Events, kLSMTLView, kViewInitSelector, kSendEventTypes, init.Implementation).Passed);
}

// --- events, described ------------------------------------------------------

TEST(events_are_described_for_the_input)
{
    NSEvent* key = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:NSEventModifierFlagShift timestamp:0
        windowNumber:0 context:nil characters:@"A" charactersIgnoringModifiers:@"a" isARepeat:YES keyCode:0];
    auto described = DescribeEvent((__bridge void*)key, nullptr);
    CHECK(described.Kind == CocoaEventKind::KeyDown && described.KeyCode == 0 && described.Repeat && !strcmp(described.Text, "A"));
    CHECK((described.ModifierFlags & (1u << 17)) != 0);

    NSEvent* up = [NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil
        characters:@"a" charactersIgnoringModifiers:@"a" isARepeat:NO keyCode:0x24];
    described = DescribeEvent((__bridge void*)up, nullptr);
    CHECK(described.Kind == CocoaEventKind::KeyUp && described.KeyCode == 0x24 && !described.Text[0]);

    NSEvent* mouse = [NSEvent mouseEventWithType:NSEventTypeRightMouseDown location:NSMakePoint(10, 20) modifierFlags:0 timestamp:0
        windowNumber:0 context:nil eventNumber:0 clickCount:2 pressure:1];
    described = DescribeEvent((__bridge void*)mouse, nullptr);
    CHECK(described.Kind == CocoaEventKind::MouseDown && described.Button == 1 && described.Clicks == 2);
    CHECK(!described.HasPosition);      // no view, no scale

    NSEvent* dragged = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged location:NSMakePoint(10, 20) modifierFlags:0 timestamp:0
        windowNumber:0 context:nil eventNumber:0 clickCount:0 pressure:1];
    CHECK(DescribeEvent((__bridge void*)dragged, nullptr).Kind == CocoaEventKind::MouseMoved);

    // A view that is in no window gives no position either.
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 100, 50)];
    CHECK(!DescribeEvent((__bridge void*)mouse, (__bridge void*)view).HasPosition);

    // A wheel event made without a source of its own has the modifiers held
    // on the machine at that moment, and with Shift held the system gives a
    // wheel's vertical lines as horizontal ones: the event is made from a
    // private source and with the modifiers it is meant to have.
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStatePrivate);
    CGEventRef wheel = CGEventCreateScrollWheelEvent(source, kCGScrollEventUnitLine, 2, -3, 1);
    if (wheel) {
        CGEventSetFlags(wheel, (CGEventFlags)0);
        NSEvent* scroll = [NSEvent eventWithCGEvent:wheel];
        described = DescribeEvent((__bridge void*)scroll, nullptr);
        CHECK(described.Kind == CocoaEventKind::ScrollWheel && !described.PreciseScroll && !described.ScrollCancelled);
        CHECK(described.ScrollY < 0 && described.ScrollX > 0);
        CHECK((described.ModifierFlags & (1u << 17)) == 0);

        // With Shift the event is described as the system gives it, whichever axis that is.
        CGEventSetFlags(wheel, kCGEventFlagMaskShift);
        scroll = [NSEvent eventWithCGEvent:wheel];
        described = DescribeEvent((__bridge void*)scroll, nullptr);
        CHECK(described.Kind == CocoaEventKind::ScrollWheel && (described.ModifierFlags & (1u << 17)) != 0);
        CHECK(described.ScrollX == (float)scroll.scrollingDeltaX && described.ScrollY == (float)scroll.scrollingDeltaY);
        CHECK(described.ScrollX != 0 || described.ScrollY != 0);
        CFRelease(wheel);
    }
    if (source) CFRelease(source);

    // Events the input has no use for.
    NSEvent* other = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
        context:nil subtype:0 data1:0 data2:0];
    CHECK(DescribeEvent((__bridge void*)other, nullptr).Kind == CocoaEventKind::Other);
    CHECK(DescribeEvent(nullptr, nullptr).Kind == CocoaEventKind::Other);
}

// --- the tap ----------------------------------------------------------------

struct Sink : EventSink
{
    int Events{ 0 }, FocusChanges{ 0 };
    bool Deliver{ true };
    CocoaEvent Last;

    bool OnEvent(CocoaEvent const& event) override
    {
        Events++;
        Last = event;
        return Deliver;
    }
    void OnFocus(bool) override { FocusChanges++; }
};

Dependencies EventDependencies(uint64_t sendEvent, uint64_t viewInit)
{
    Dependencies d;
    d.Live = true;
    d.Checks.push_back({ Part::Events, "stated by the test", true, "" });
    d.SendEvent = sendEvent;
    d.ViewInit = viewInit;
    return d;
}

TEST(the_tap_replaces_the_methods_only_while_they_are_the_checked_code)
{
    uint64_t sendEvent = ImplementationOf([BG3Application class], @selector(sendEvent:));
    uint64_t viewInit = ImplementationOf([LSMTLView class], @selector(initWithFrame:device:display:hdr:vsync:));
    Sink sink;

    // The checks did not pass, or did not run on a loaded game: nothing is replaced.
    Dependencies off;
    off.Live = true;
    off.Checks.push_back({ Part::Events, "a check", false, "the reason" });
    CHECK(Contains(InstallEventTap(off, &sink), "the reason"));
    auto fromFile = EventDependencies(sendEvent, viewInit);
    fromFile.Live = false;
    CHECK(!InstallEventTap(fromFile, &sink).empty());
    CHECK(!InstallEventTap(EventDependencies(sendEvent, viewInit), nullptr).empty());
    // One of the two methods is not the code that was checked: neither is replaced.
    CHECK(Contains(InstallEventTap(EventDependencies(sendEvent + 4, viewInit), &sink), "no longer implemented"));
    CHECK(Contains(InstallEventTap(EventDependencies(sendEvent, viewInit + 4), &sink), "no longer implemented"));
    CHECK(!EventTapInstalled());
    CHECK(ImplementationOf([BG3Application class], @selector(sendEvent:)) == sendEvent);
    CHECK(ImplementationOf([LSMTLView class], @selector(initWithFrame:device:display:hdr:vsync:)) == viewInit);

    CHECK(InstallEventTap(EventDependencies(sendEvent, viewInit), &sink).empty());
    CHECK(EventTapInstalled());
    CHECK(ImplementationOf([BG3Application class], @selector(sendEvent:)) != sendEvent);

    // The view is noted where it is made, and comes out of its initialiser as it went in.
    CHECK(GameView() == nullptr);
    LSMTLView* view = [[LSMTLView alloc] initWithFrame:NSMakeRect(0, 0, 100, 50) device:nil display:0 hdr:NO vsync:YES];
    CHECK(view != nil && GameView() == (__bridge void*)view);
    CHECK(view.frame.size.width == 100);

    // Every event passes the sink first; the sink decides whether the application gets it.
    BG3Application* app = [[BG3Application alloc] init];
    NSEvent* key = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil
        characters:@"a" charactersIgnoringModifiers:@"a" isARepeat:NO keyCode:0];
    [app sendEvent:key];
    CHECK(sink.Events == 1 && sink.Last.Kind == CocoaEventKind::KeyDown && app.received == key);
    app.received = nil;
    sink.Deliver = false;
    [app sendEvent:key];
    CHECK(sink.Events == 2 && app.received == nil);
    // An event the input has no use for is the application's without the sink being asked.
    NSEvent* other = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
        context:nil subtype:0 data1:0 data2:0];
    [app sendEvent:other];
    CHECK(sink.Events == 2 && app.received == other);

    [[NSNotificationCenter defaultCenter] postNotificationName:NSApplicationDidBecomeActiveNotification object:nil];
    [[NSNotificationCenter defaultCenter] postNotificationName:NSApplicationDidResignActiveNotification object:nil];
    CHECK(sink.FocusChanges == 2);

    RemoveEventTap();
    CHECK(!EventTapInstalled() && GameView() == nullptr);
    CHECK(ImplementationOf([BG3Application class], @selector(sendEvent:)) == sendEvent);
    CHECK(ImplementationOf([LSMTLView class], @selector(initWithFrame:device:display:hdr:vsync:)) == viewInit);
    app.received = nil;
    [app sendEvent:key];
    CHECK(sink.Events == 2 && app.received == key);
    [[NSNotificationCenter defaultCenter] postNotificationName:NSApplicationDidBecomeActiveNotification object:nil];
    CHECK(sink.FocusChanges == 2);
}

// --- the frame --------------------------------------------------------------

// A render frame and a renderer as the tracker reads them.
struct TestRenderer
{
    void* Unused;
    void* Buffer;       // +0x08
};
struct TestFrame
{
    void* Unused[2];
    void* Drawable;     // +0x10
    void* Renderer;     // +0x18
};
constexpr uint32_t kDrawable = 0x10, kRenderer = 0x18, kBuffer = 0x08;

struct Listener : FrameListener
{
    int Frames{ 0 }, Commits{ 0 };
    FrameInfo Last{};
    ImDrawData* Draw{ nullptr };
    Encoded Result{ Encoded::Failed };
    TargetKind Target{ TargetKind::Unsupported };
    float PaperWhite{ NAN };
    std::string Problem;

    void OnFrame(FrameInfo const& frame) override
    {
        Frames++;
        Last = frame;
    }
    void OnCommit(void* commandBuffer, void* drawable) override
    {
        Commits++;
        if (Draw) Result = MetalEncode(Draw, commandBuffer, drawable, PaperWhite, Target, Problem);
    }
};

id<MTLTexture> MakeTarget(id<MTLDevice> device, MTLPixelFormat format, NSUInteger size)
{
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:size height:size mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget;     // as a layer's drawable that is only a framebuffer
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    std::vector<uint8_t> zero(size * size * 4, 0);
    [texture replaceRegion:MTLRegionMake2D(0, 0, size, size) mipmapLevel:0 withBytes:zero.data() bytesPerRow:size * 4];
    return texture;
}

// The four bytes of the pixel in the middle of a target.
std::array<uint8_t, 4> MiddleOf(id<MTLTexture> texture)
{
    std::array<uint8_t, 4> pixel{};
    [texture getBytes:pixel.data() bytesPerRow:4 fromRegion:MTLRegionMake2D(texture.width / 2, texture.height / 2, 1, 1) mipmapLevel:0];
    return pixel;
}

// A pixel of a BGR10A2Unorm target: red in the bits from 20, green from 10, blue from 0.
struct Packed
{
    uint32_t Word, R, G, B;
};
Packed PackedOf(id<MTLTexture> texture, NSUInteger x, NSUInteger y)
{
    uint32_t word = 0;
    [texture getBytes:&word bytesPerRow:4 fromRegion:MTLRegionMake2D(x, y, 1, 1) mipmapLevel:0];
    return { word, (word >> 20) & 0x3ff, (word >> 10) & 0x3ff, word & 0x3ff };
}
void FillPacked(id<MTLTexture> texture, uint32_t word)
{
    std::vector<uint32_t> words(texture.width * texture.height, word);
    [texture replaceRegion:MTLRegionMake2D(0, 0, texture.width, texture.height) mipmapLevel:0 withBytes:words.data()
        bytesPerRow:texture.width * 4];
}
uint32_t Code10(double value) { return (uint32_t)lround(value * 1023.0); }
// Within two steps of 10 bits: the overlay's own texture has 8.
bool Close10(uint32_t code, double expected) { return std::fabs((double)code - expected * 1023.0) <= 2.0; }

TEST(a_frame_is_noted_at_the_flip_and_drawn_on_at_its_commit)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
        printf("no Metal device: the tests of the Metal pass are skipped\n");
        return;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];

    // An ImGui frame that covers a 64 x 64 drawable in opaque red.
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(64, 64);
    CHECK(!MetalCreateDeviceObjects((__bridge void*)device));       // not before the renderer has the device
    CHECK(MetalInitialize((__bridge void*)device));
    CHECK(MetalCreateDeviceObjects((__bridge void*)device));
    ImGui::NewFrame();
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(64, 64), IM_COL32(255, 0, 0, 255));
    ImGui::Render();

    Listener listener;
    FrameTracker tracker(kDrawable, kRenderer, kBuffer, &listener);

    OverlayTestDrawable* drawable = [[OverlayTestDrawable alloc] init];
    drawable.texture = MakeTarget(device, MTLPixelFormatBGRA8Unorm, 64);
    id<MTLCommandBuffer> buffer = [queue commandBufferWithUnretainedReferences];
    id<MTLCommandBuffer> otherBuffer = [queue commandBufferWithUnretainedReferences];
    TestRenderer renderer{ nullptr, (__bridge void*)buffer }, otherRenderer{ nullptr, (__bridge void*)otherBuffer };
    TestFrame frame{ {}, (__bridge void*)drawable, &renderer };

    // Nothing to note: no present, no drawable, no frame.
    tracker.OnFlipBuffers(&frame, false);
    tracker.OnFlipBuffers(nullptr, true);
    TestFrame empty{ {}, nullptr, &renderer };
    tracker.OnFlipBuffers(&empty, true);
    CHECK(listener.Frames == 0 && !tracker.Pending());
    // A commit with nothing noted is the game's alone.
    tracker.OnCommitCommandBuffer(&renderer);
    CHECK(listener.Commits == 0);

    tracker.OnFlipBuffers(&frame, true);
    CHECK(listener.Frames == 1 && tracker.Pending());
    CHECK(listener.Last.Width == 64 && listener.Last.Height == 64 && listener.Last.Target == TargetKind::Standard);
    CHECK(listener.Last.Device == (__bridge void*)device);
    CHECK(State().DrawableWidth() == 64 && State().DrawableHeight() == 64);

    // Another renderer's commit, and this renderer's commit on another thread, are not the frame's.
    listener.Draw = ImGui::GetDrawData();
    tracker.OnCommitCommandBuffer(&otherRenderer);
    tracker.OnCommitCommandBuffer(nullptr);
    std::thread([&] { tracker.OnCommitCommandBuffer(&renderer); }).join();
    CHECK(listener.Commits == 0 && tracker.Pending());

    tracker.OnCommitCommandBuffer(&renderer);
    CHECK(listener.Commits == 1 && !tracker.Pending());
    CHECK(listener.Result == Encoded::Drawn && listener.Target == TargetKind::Standard);
    CHECK(MetalBuffersInFlight() == 1);
    // The frame's second commit finds nothing noted.
    tracker.OnCommitCommandBuffer(&renderer);
    CHECK(listener.Commits == 1);

    [buffer commit];
    [buffer waitUntilCompleted];
    CHECK(buffer.status == MTLCommandBufferStatusCompleted);
    for (int waited = 0; MetalBuffersInFlight() && waited < 1000; waited++) usleep(1000);
    CHECK(MetalBuffersInFlight() == 0);
    auto pixel = MiddleOf(drawable.texture);        // blue, green, red, alpha
    CHECK(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 255);

    // A frame on another thread is not noted.
    std::thread([&] { tracker.OnFlipBuffers(&frame, true); }).join();
    CHECK(listener.Frames == 1 && !tracker.Pending());

    // A buffer that is already committed is not drawn on.
    tracker.OnFlipBuffers(&frame, true);
    CHECK(listener.Frames == 2 && tracker.Pending());
    tracker.OnCommitCommandBuffer(&renderer);
    CHECK(listener.Commits == 1 && !tracker.Pending());

    // The HDR target: 10 bits, BT.2020 PQ, extended range. Without a usable
    // paper white it is noted and not drawn on.
    OverlayTestDrawable* hdr = [[OverlayTestDrawable alloc] init];
    hdr.texture = MakeTarget(device, MTLPixelFormatBGR10A2Unorm, 64);
    hdr.layer = OverlayTestLayer(kCGColorSpaceITUR_2100_PQ, YES);
    id<MTLCommandBuffer> hdrBuffer = [queue commandBufferWithUnretainedReferences];
    TestRenderer hdrRenderer{ nullptr, (__bridge void*)hdrBuffer };
    TestFrame hdrFrame{ {}, (__bridge void*)hdr, &hdrRenderer };
    tracker.OnFlipBuffers(&hdrFrame, true);
    CHECK(listener.Frames == 3 && listener.Last.Target == TargetKind::HighDynamicRange);
    CHECK(listener.Last.Format.Space == ColourSpace::Bt2020Pq && listener.Last.Format.ExtendedRange
        && listener.Last.Format.PixelFormat == MTLPixelFormatBGR10A2Unorm);
    CHECK(MetalDrawsHighDynamicRange());
    tracker.OnCommitCommandBuffer(&hdrRenderer);
    CHECK(listener.Commits == 2 && listener.Result == Encoded::NotDrawable && listener.Target == TargetKind::HighDynamicRange);
    CHECK(MetalBuffersInFlight() == 0);
    [hdrBuffer commit];
    [hdrBuffer waitUntilCompleted];
    pixel = MiddleOf(hdr.texture);
    CHECK(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 0);

    // With the white of the game's UI the opaque red frame is drawn as red at that white.
    constexpr float kPaperWhite = 250.0f;
    listener.PaperWhite = kPaperWhite;
    hdrBuffer = [queue commandBufferWithUnretainedReferences];
    hdrRenderer.Buffer = (__bridge void*)hdrBuffer;
    tracker.OnFlipBuffers(&hdrFrame, true);
    tracker.OnCommitCommandBuffer(&hdrRenderer);
    CHECK(listener.Commits == 3 && listener.Result == Encoded::Drawn && listener.Target == TargetKind::HighDynamicRange);
    [hdrBuffer commit];
    [hdrBuffer waitUntilCompleted];
    for (int waited = 0; MetalBuffersInFlight() && waited < 1000; waited++) usleep(1000);
    Rgb red = CompositeOverPq({ 0, 0, 0 }, { 1, 0, 0 }, 1.0, kPaperWhite);
    auto packed = PackedOf(hdr.texture, 32, 32);
    CHECK(Close10(packed.R, red.R) && Close10(packed.G, red.G) && Close10(packed.B, red.B));

    // A half-covering, half-transparent white over the game's grey: blended
    // as over a standard target; where it covers nothing the game's pixel is
    // as it was, to the bit.
    ImGui::NewFrame();
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(32, 64), IM_COL32(255, 255, 255, 128));
    ImGui::Render();
    listener.Draw = ImGui::GetDrawData();
    uint32_t grey = Code10(PqEncode(0.01));     // 100 nits, below the paper white
    FillPacked(hdr.texture, 3u << 30 | grey << 20 | grey << 10 | grey);
    hdrBuffer = [queue commandBufferWithUnretainedReferences];
    hdrRenderer.Buffer = (__bridge void*)hdrBuffer;
    tracker.OnFlipBuffers(&hdrFrame, true);
    tracker.OnCommitCommandBuffer(&hdrRenderer);
    CHECK(listener.Result == Encoded::Drawn);
    [hdrBuffer commit];
    [hdrBuffer waitUntilCompleted];
    for (int waited = 0; MetalBuffersInFlight() && waited < 1000; waited++) usleep(1000);
    double coverage = 128.0 / 255.0;
    double game = grey / 1023.0;
    Rgb blended = CompositeOverPq({ game, game, game }, { coverage, coverage, coverage }, coverage, kPaperWhite);
    packed = PackedOf(hdr.texture, 16, 32);
    CHECK(Close10(packed.R, blended.R) && Close10(packed.G, blended.G) && Close10(packed.B, blended.B));
    CHECK(packed.R > grey);
    auto untouched = PackedOf(hdr.texture, 48, 32);
    CHECK(untouched.Word == (3u << 30 | grey << 20 | grey << 10 | grey));

    // A standard format on a layer that asks for extended range, and a layer
    // without a colour space: refused.
    OverlayTestDrawable* extended = [[OverlayTestDrawable alloc] init];
    extended.texture = MakeTarget(device, MTLPixelFormatBGRA8Unorm, 64);
    extended.layer = OverlayTestLayer(kCGColorSpaceDisplayP3, YES);
    id<MTLCommandBuffer> extendedBuffer = [queue commandBufferWithUnretainedReferences];
    TargetKind target;
    std::string problem;
    CHECK(MetalEncode(ImGui::GetDrawData(), (__bridge void*)extendedBuffer, (__bridge void*)extended, kPaperWhite, target, problem)
        == Encoded::NotDrawable);
    CHECK(target == TargetKind::Unsupported);
    extended.layer = OverlayTestLayer(nullptr, NO);
    CHECK(MetalEncode(ImGui::GetDrawData(), (__bridge void*)extendedBuffer, (__bridge void*)extended, kPaperWhite, target, problem)
        == Encoded::NotDrawable);
    CHECK(target == TargetKind::Unsupported && TargetOf((__bridge void*)extended).Space == ColourSpace::Unknown);
    CHECK(ColourSpaceNameOf((__bridge void*)extended) == "no colour space");
    extended.layer = OverlayTestLayer(kCGColorSpaceSRGB, NO);
    CHECK(TargetOf((__bridge void*)extended).Kind == TargetKind::Standard);
    extended.layer = OverlayTestLayer(kCGColorSpaceGenericGrayGamma2_2, NO);
    CHECK(TargetOf((__bridge void*)extended).Kind == TargetKind::Unsupported);
    CHECK(Contains(ColourSpaceNameOf((__bridge void*)extended), "Gray"));
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    // The name the game gives its layer.
    extended.layer = OverlayTestLayer(kCGColorSpaceITUR_2020_PQ_EOTF, YES);
#pragma clang diagnostic pop
    CHECK(TargetOf((__bridge void*)extended).Space == ColourSpace::Bt2020Pq);

    // A frame built for another size waits for the next one; nothing is drawn of it.
    OverlayTestDrawable* smaller = [[OverlayTestDrawable alloc] init];
    smaller.texture = MakeTarget(device, MTLPixelFormatBGRA8Unorm, 32);
    CHECK(MetalEncode(ImGui::GetDrawData(), (__bridge void*)extendedBuffer, (__bridge void*)smaller, NAN, target, problem) == Encoded::StaleSize);
    CHECK(MetalEncode(nullptr, (__bridge void*)extendedBuffer, (__bridge void*)smaller, NAN, target, problem) == Encoded::Failed);
    CHECK(MetalBuffersInFlight() == 0);
    [extendedBuffer commit];

    // What is not a drawable or not a command buffer is refused, and nothing of it is kept.
    SetLogStream(nullptr);
    NSObject* notADrawable = [[NSObject alloc] init];
    TestFrame wrong{ {}, (__bridge void*)notADrawable, &renderer };
    tracker.OnFlipBuffers(&wrong, true);
    TestRenderer wrongRenderer{ nullptr, (__bridge void*)@"not a command buffer" };
    TestFrame wrongBuffer{ {}, (__bridge void*)drawable, &wrongRenderer };
    tracker.OnFlipBuffers(&wrongBuffer, true);
    SetLogStream(stdout);
    CHECK(listener.Frames == 5 && !tracker.Pending());

    // The game's texture object: the Metal texture at its offset, retained for the overlay.
    MTLTextureDescriptor* sampledDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64
                                                                                                height:48 mipmapped:NO];
    sampledDescriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> sampled = [device newTextureWithDescriptor:sampledDescriptor];
    struct { void* Before[7]; void* Texture; } gameTexture{ {}, (__bridge void*)sampled };
    uint32_t width = 0, height = 0;
    void* retained = MetalRetainTexture(&gameTexture, 0x38, width, height);
    CHECK(retained == (__bridge void*)sampled && width == 64 && height == 48);
    MetalReleaseTexture(retained);
    gameTexture.Texture = nullptr;
    CHECK(!MetalRetainTexture(&gameTexture, 0x38, width, height) && width == 0);
    gameTexture.Texture = (__bridge void*)@"not a texture";
    CHECK(!MetalRetainTexture(&gameTexture, 0x38, width, height));
    CHECK(!MetalRetainTexture(nullptr, 0x38, width, height));

    CHECK(MetalShutdown());
    CHECK(!MetalCreateDeviceObjects((__bridge void*)device));       // no renderer any more
    CHECK(io.BackendRendererUserData == nullptr);
    ImGui::DestroyContext();
}

// How many pixels of a BGRA8 target are not black.
size_t LitPixels(id<MTLTexture> texture)
{
    std::vector<uint8_t> bytes(texture.width * texture.height * 4);
    [texture getBytes:bytes.data() bytesPerRow:texture.width * 4 fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height) mipmapLevel:0];
    size_t lit = 0;
    for (size_t at = 0; at < bytes.size(); at += 4) lit += (bytes[at] | bytes[at + 1] | bytes[at + 2]) != 0;
    return lit;
}

// The id<MTLTexture> the renderer made for one of ImGui's textures, or nil.
id<MTLTexture> MetalTextureOf(ImTextureData const* texture)
{
    return texture->TexID != ImTextureID_Invalid ? (__bridge id<MTLTexture>)(void*)(intptr_t)texture->TexID : nil;
}

TEST(the_font_atlas_reaches_the_renderer_through_its_mirrors)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return;
    id<MTLCommandQueue> queue = [device newCommandQueue];

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(64, 64);
    CHECK(MetalInitialize((__bridge void*)device) && MetalCreateDeviceObjects((__bridge void*)device));
    // The renderer takes ImGui's textures as they come, and the atlas grows
    // no further than a texture of the device can be.
    CHECK((io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) != 0);
    int limit = MetalMaxTextureSize((__bridge void*)device);
    CHECK(limit >= 8192 && io.Fonts->TexMaxWidth <= limit && io.Fonts->TexMaxHeight <= limit);
    CHECK(ImGui::GetPlatformIO().Renderer_TextureMaxWidth == std::max(io.Fonts->TexMaxWidth, io.Fonts->TexMaxHeight));

    // The backend's two halves: the client thread finishes a frame and hands
    // its textures to the mirrors; the main thread draws it.
    bg3se::extui::TextureMirrorSet mirrors;
    auto build = [&](float size, char const* text) {
        ImGui::NewFrame();
        ImGui::GetForegroundDrawList()->AddText(nullptr, size, ImVec2(0, 0), IM_COL32_WHITE, text);
        ImGui::Render();
        mirrors.Sync(*ImGui::GetDrawData());
    };
    OverlayTestDrawable* drawable = [[OverlayTestDrawable alloc] init];
    TargetKind target;
    std::string problem;
    auto encode = [&](id<MTLCommandBuffer> buffer) {
        mirrors.BeginRender();
        return MetalEncode(ImGui::GetDrawData(), (__bridge void*)buffer, (__bridge void*)drawable, NAN, target, problem);
    };
    auto finish = [&](id<MTLCommandBuffer> buffer) {
        [buffer commit];
        [buffer waitUntilCompleted];
        for (int waited = 0; MetalBuffersInFlight() && waited < 1000; waited++) usleep(1000);
        return buffer.status == MTLCommandBufferStatusCompleted && buffer.error == nil;
    };

    // What the frames below make is let go of with their pool, so that
    // nothing but a pass holds a texture afterwards.
    __weak id<MTLTexture> firstTexture = nil;
    id<MTLCommandBuffer> held = nil;
    id<MTLTexture> heldTarget = nil;
    ImTextureData* larger = nullptr;
    @autoreleasepool {
        // A frame with text. ImGui's own texture is answered by the mirror
        // and never gets a Metal texture; the frame brings the mirror, not
        // yet made.
        build(24.0f, "MMMM");
        ImDrawData* frame = ImGui::GetDrawData();
        CHECK(ImGui::GetPlatformIO().Textures.Size == 1);
        ImTextureData* own = ImGui::GetPlatformIO().Textures[0];
        CHECK(own->Status == ImTextureStatus_OK && own->TexID == ImTextureID_Invalid && own->BackendUserData == nullptr);
        CHECK(frame->Textures && frame->Textures != &ImGui::GetPlatformIO().Textures && frame->Textures->Size == 1);
        ImTextureData* mirror = (*frame->Textures)[0];
        CHECK(mirror == own->QueueUserData && mirror->Status == ImTextureStatus_WantCreate && mirror->TexID == ImTextureID_Invalid);
        CHECK(mirror->Width == own->Width && mirror->Height == own->Height);

        // Drawn: the mirror's texture is made with the atlas's pixels before
        // the pass is encoded, and the text is on the target.
        drawable.texture = MakeTarget(device, MTLPixelFormatBGRA8Unorm, 64);
        id<MTLCommandBuffer> first = [queue commandBufferWithUnretainedReferences];
        CHECK(encode(first) == Encoded::Drawn);
        CHECK(mirror->Status == ImTextureStatus_OK);
        firstTexture = MetalTextureOf(mirror);
        CHECK(firstTexture != nil && (int)firstTexture.width == mirror->Width && (int)firstTexture.height == mirror->Height);
        CHECK(finish(first) && LitPixels(drawable.texture) > 100);

        // Glyphs of another size are added to the same atlas: the mirror
        // takes the changed rectangles, and the same Metal texture is given
        // them.
        build(28.0f, "WWWW");
        CHECK(own->Status == ImTextureStatus_OK && own->QueueUserData == mirror && mirror->Status == ImTextureStatus_WantUpdates);
        drawable.texture = MakeTarget(device, MTLPixelFormatBGRA8Unorm, 64);
        id<MTLCommandBuffer> second = [queue commandBufferWithUnretainedReferences];
        CHECK(encode(second) == Encoded::Drawn);
        CHECK(mirror->Status == ImTextureStatus_OK && MetalTextureOf(mirror) == firstTexture);
        CHECK(finish(second) && LitPixels(drawable.texture) > 100);

        // A pass is encoded with that texture, on a buffer that holds
        // nothing, as the game's do not, and does not run yet.
        held = [queue commandBufferWithUnretainedReferences];
        heldTarget = MakeTarget(device, MTLPixelFormatBGRA8Unorm, 64);
        drawable.texture = heldTarget;
        build(28.0f, "WWWW");
        CHECK(encode(held) == Encoded::Drawn && MetalBuffersInFlight() == 1);

        // The atlas outgrows its texture: ImGui makes a larger one and asks
        // for the old one's destruction a frame later.
        int grown = 0;
        for (float size = 64.0f; size <= 640.0f && ImGui::GetPlatformIO().Textures.Size < 2 && own->QueueUserData == mirror; size += 48.0f) {
            build(size, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
            grown++;
        }
        build(28.0f, "WWWW");
        CHECK(grown > 0 && mirror->Status == ImTextureStatus_WantDestroy);
        frame = ImGui::GetDrawData();
        for (ImTextureData* brought : *frame->Textures) {
            if (brought != mirror && brought->Status == ImTextureStatus_WantCreate) larger = brought;
        }
        CHECK(larger && (larger->Width > mirror->Width || larger->Height > mirror->Height));

        // Drawn with the larger one; the renderer lets go of the old texture.
        drawable.texture = MakeTarget(device, MTLPixelFormatBGRA8Unorm, 64);
        id<MTLCommandBuffer> third = [queue commandBufferWithUnretainedReferences];
        CHECK(encode(third) == Encoded::Drawn);
        CHECK(larger && larger->Status == ImTextureStatus_OK && MetalTextureOf(larger) != nil);
        CHECK(mirror->Status == ImTextureStatus_Destroyed && mirror->TexID == ImTextureID_Invalid && mirror->BackendUserData == nullptr);
        CHECK(finish(third) && LitPixels(drawable.texture) > 100);
        // The next frame's hand-over forgets the destroyed mirrors.
        build(28.0f, "WWWW");
        CHECK(ImGui::GetDrawData()->Textures->Size == 1 && (*ImGui::GetDrawData()->Textures)[0] == larger);
        drawable.texture = nil;
    }

    // The old texture is alive all the same: the pass that was encoded with
    // it holds it, and runs with it.
    CHECK(firstTexture != nil);
    drawable.texture = heldTarget;
    CHECK(finish(held) && LitPixels(heldTarget) > 100);

    // Before the renderer is shut down the mirrors are destroyed through it,
    // and ImGui asks the next renderer for its textures again.
    CHECK(larger && larger->BackendUserData != nullptr);
    mirrors.Destroy(&MetalUpdateTexture);
    CHECK(ImGui::GetPlatformIO().Textures.Size >= 1);
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
        CHECK(texture->QueueUserData == nullptr && texture->TexID == ImTextureID_Invalid);
        CHECK(texture->Status == ImTextureStatus_WantCreate || texture->Status == ImTextureStatus_Destroyed);
    }
    CHECK(MetalShutdown());
    MetalUpdateTexture(ImGui::GetPlatformIO().Textures[0]);     // nothing without a renderer
    CHECK(ImGui::GetPlatformIO().Textures[0]->TexID == ImTextureID_Invalid);
    ImGui::DestroyContext();
}

TEST(only_a_texture_the_shader_can_sample_is_registered)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return;
    struct { void* Before[7]; void* Texture; } gameTexture{};
    auto registered = [&](MTLTextureDescriptor* descriptor) {
        id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
        CHECK(texture != nil);
        gameTexture.Texture = (__bridge void*)texture;
        uint32_t width = 7, height = 7;
        void* retained = MetalRetainTexture(&gameTexture, 0x38, width, height);
        CHECK(retained ? width == 8 && height == 8 : width == 0 && height == 0);
        MetalReleaseTexture(retained);
        return retained != nullptr;
    };
    auto plain = [](MTLPixelFormat format) {
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:8 height:8 mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead;
        return descriptor;
    };

    // What the game's icons and atlases are: two-dimensional, also compressed, also with mip levels.
    CHECK(registered(plain(MTLPixelFormatRGBA8Unorm)) && registered(plain(MTLPixelFormatBGRA8Unorm_sRGB)));
    CHECK(registered(plain(MTLPixelFormatBC3_RGBA)) && registered(plain(MTLPixelFormatR8Unorm)));
    CHECK(registered([MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:YES]));

    // Another type than the shader's texture2d: an array, a cube, a volume, one dimension, several samples.
    MTLTextureDescriptor* array = plain(MTLPixelFormatRGBA8Unorm);
    array.textureType = MTLTextureType2DArray;
    array.arrayLength = 2;
    CHECK(!registered(array));
    CHECK(!registered([MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm size:8 mipmapped:NO]));
    MTLTextureDescriptor* volume = plain(MTLPixelFormatRGBA8Unorm);
    volume.textureType = MTLTextureType3D;
    volume.depth = 2;
    CHECK(!registered(volume));
    MTLTextureDescriptor* line = plain(MTLPixelFormatRGBA8Unorm);
    line.textureType = MTLTextureType1D;
    line.height = 1;
    id<MTLTexture> lineTexture = [device newTextureWithDescriptor:line];
    gameTexture.Texture = (__bridge void*)lineTexture;
    uint32_t width = 0, height = 0;
    CHECK(lineTexture && !MetalRetainTexture(&gameTexture, 0x38, width, height));
    MTLTextureDescriptor* multisampled = plain(MTLPixelFormatRGBA8Unorm);
    multisampled.textureType = MTLTextureType2DMultisample;
    multisampled.sampleCount = 4;
    multisampled.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    multisampled.storageMode = MTLStorageModePrivate;
    CHECK(!registered(multisampled));

    // Integer values, and a texture no shader may read.
    CHECK(!registered(plain(MTLPixelFormatR8Uint)) && !registered(plain(MTLPixelFormatRGBA8Sint)));
    MTLTextureDescriptor* target = plain(MTLPixelFormatRGBA8Unorm);
    target.usage = MTLTextureUsageRenderTarget;
    target.storageMode = MTLStorageModePrivate;
    CHECK(!registered(target));
}

// A listener whose calls stay in progress until they are let go.
struct HeldListener : FrameListener
{
    std::atomic<bool> Called{ false }, LetGo{ false };
    std::atomic<int> Calls{ 0 };

    void Hold()
    {
        Calls++;
        Called = true;
        while (!LetGo) usleep(100);
    }
    void OnFrame(FrameInfo const&) override { Hold(); }
    void OnCommit(void*, void*) override { Hold(); }
};

TEST(a_listener_is_not_let_go_of_while_it_is_being_called)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    OverlayTestDrawable* drawable = [[OverlayTestDrawable alloc] init];
    drawable.texture = MakeTarget(device, MTLPixelFormatBGRA8Unorm, 16);
    id<MTLCommandBuffer> buffer = [queue commandBufferWithUnretainedReferences];
    TestRenderer renderer{ nullptr, (__bridge void*)buffer };
    TestFrame frame{ {}, (__bridge void*)drawable, &renderer };

    // The main thread is in the listener (here, as in the game, the thread
    // of both hooks) while another thread takes the listener out, as the
    // backend does before it frees it: that thread waits for the call.
    for (bool atCommit : { false, true }) {
        HeldListener listener;
        FrameTracker tracker(kDrawable, kRenderer, kBuffer, &listener);
        if (atCommit) {
            listener.LetGo = true;
            tracker.OnFlipBuffers(&frame, true);
            listener.Called = listener.LetGo = false;
        }
        std::atomic<bool> removed{ false }, removedDuringCall{ false };
        std::thread backend([&] {
            while (!listener.Called) usleep(100);
            tracker.SetListener(nullptr);
            removed = true;
        });
        std::thread watch([&] {
            while (!listener.Called) usleep(100);
            usleep(100000);
            removedDuringCall = removed.load();
            listener.LetGo = true;
        });
        if (atCommit) tracker.OnCommitCommandBuffer(&renderer);
        else tracker.OnFlipBuffers(&frame, true);
        backend.join();
        watch.join();
        CHECK(!removedDuringCall.load() && removed.load());

        // ... and the listener is not called again.
        int calls = listener.Calls;
        tracker.OnFlipBuffers(&frame, true);
        tracker.OnCommitCommandBuffer(&renderer);
        CHECK(listener.Calls == calls);
    }
    [buffer commit];
}

TEST(a_renderer_whose_pass_has_not_run_is_not_destroyed)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return;
    id<MTLCommandQueue> queue = [device newCommandQueue];

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(64, 64);
    CHECK(MetalInitialize((__bridge void*)device) && MetalCreateDeviceObjects((__bridge void*)device));
    ImGui::NewFrame();
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(64, 64), IM_COL32(255, 0, 0, 255));
    ImGui::Render();

    // A pass of the overlay's on a buffer that is not committed yet. The
    // buffer holds nothing it uses, as the game's do not.
    OverlayTestDrawable* drawable = [[OverlayTestDrawable alloc] init];
    drawable.texture = MakeTarget(device, MTLPixelFormatBGRA8Unorm, 64);
    id<MTLCommandBuffer> buffer = [queue commandBufferWithUnretainedReferences];
    TargetKind target;
    std::string problem;
    CHECK(MetalEncode(ImGui::GetDrawData(), (__bridge void*)buffer, (__bridge void*)drawable, NAN, target, problem) == Encoded::Drawn);
    CHECK(MetalBuffersInFlight() == 1);

    // The renderer's pipeline state, which only the renderer holds.
    __weak id pipeline = nil;
    @autoreleasepool {
        id context = (__bridge id)(*reinterpret_cast<void**>(io.BackendRendererUserData));
        NSDictionary* pipelines = [context valueForKey:@"renderPipelineStateCache"];
        pipeline = pipelines.allValues.firstObject;
    }
    CHECK(pipeline != nil);

    // The shutdown gives up waiting, and destroys nothing the pass uses.
    CHECK(!MetalShutdown(50));
    CHECK(pipeline != nil);
    // ImGui is parted from the renderer as after a shutdown.
    CHECK(io.BackendRendererUserData == nullptr && io.BackendRendererName == nullptr);
    CHECK((io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) == 0);
    CHECK(!MetalCreateDeviceObjects((__bridge void*)device));
    CHECK(MetalShutdown(50));       // nothing to shut down

    // The pass runs, and draws what it was to draw.
    [buffer commit];
    [buffer waitUntilCompleted];
    CHECK(buffer.status == MTLCommandBufferStatusCompleted && buffer.error == nil);
    auto pixel = MiddleOf(drawable.texture);
    CHECK(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 255 && pixel[3] == 255);
    for (int waited = 0; MetalBuffersInFlight() && waited < 1000; waited++) usleep(1000);
    CHECK(MetalBuffersInFlight() == 0);

    // The renderer can be given the device again, and is shut down when nothing is on its way.
    CHECK(MetalInitialize((__bridge void*)device) && MetalCreateDeviceObjects((__bridge void*)device));
    CHECK((io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) != 0);
    CHECK(MetalShutdown(50));
    CHECK(io.BackendRendererUserData == nullptr);
    ImGui::DestroyContext();
}

// --- the keyboard layout ----------------------------------------------------

TEST(a_layout_gives_its_characters_as_sdl_takes_them)
{
    // Layouts loaded by their ids, not the one this machine has on; for an
    // ANSI keyboard type.
    constexpr uint32_t kAnsi = 40;
    CHECK(KBGetLayoutType(kAnsi) == kKeyboardANSI);
    void const* us = LayoutDataByID("com.apple.keylayout.US");
    void const* french = LayoutDataByID("com.apple.keylayout.French");
    CHECK(us && french);
    if (!us || !french) return;
    CHECK(LayoutCharacterOf(us, kAnsi, 0x00) == 'a' && LayoutCharacterOf(us, kAnsi, 0x0c) == 'q' && LayoutCharacterOf(us, kAnsi, 0x12) == '1');
    CHECK(LayoutCharacterOf(us, kAnsi, 0x24) == '\r' && LayoutCharacterOf(us, kAnsi, 0x33) == '\b' && LayoutCharacterOf(us, kAnsi, 0x75) == 0x7f);
    CHECK(LayoutCharacterOf(french, kAnsi, 0x00) == 'q' && LayoutCharacterOf(french, kAnsi, 0x0c) == 'a');
    CHECK(LayoutCharacterOf(french, kAnsi, 0x12) == '&' && LayoutCharacterOf(french, kAnsi, 0x2e) == ',');
    // No dead key: the circumflex key types its accent alone.
    CHECK(LayoutCharacterOf(french, kAnsi, 0x21) == '^');
    // The overlay's keys of French letters.
    CHECK(ImGuiKeyOf(0x00, LayoutCharacterOf(french, kAnsi, 0x00)) == ImGuiKey_Q);
    CHECK(ImGuiKeyOf(0x0d, LayoutCharacterOf(french, kAnsi, 0x0d)) == ImGuiKey_Z);
    CHECK(ImGuiKeyOf(0x12, LayoutCharacterOf(french, kAnsi, 0x12)) == ImGuiKey_None);
    CHECK(!LayoutCharacterOf(nullptr, kAnsi, 0x00) && !LayoutDataByID("com.apple.keylayout.NoSuchLayout"));
    // What the machine has on gives something for the A key, and says whether it is ISO.
    CHECK(KeyboardLayout::Current().CharacterOf(0x00) != 0);
    (void)KeyboardLayout::Current().Iso();
}

// --- the text client --------------------------------------------------------

struct Texts : TextSink
{
    std::vector<std::string> Got;
    void OnText(char const* utf8) override { Got.push_back(utf8); }
};

TEST(the_text_client_takes_the_keys_while_a_text_field_has_the_keyboard)
{
    Texts sink;
    Dependencies d;
    d.Live = true;
    d.Checks.push_back({ Part::Events, "stated by the test", true, "" });
    CHECK(!InstallTextInput(d, nullptr).empty());
    CHECK(InstallTextInput(d, &sink).empty() && TextInputInstalled());

    // The protocol, as the input system calls it.
    id<NSTextInputClient> client = (__bridge id<NSTextInputClient>)TextClient();
    [client setMarkedText:@"\u00b4" selectedRange:NSMakeRange(1, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
    CHECK([client hasMarkedText] && [client markedRange].length == 1 && [client selectedRange].location == 1);
    [client insertText:@"\u00e9" replacementRange:NSMakeRange(NSNotFound, 0)];
    CHECK(![client hasMarkedText] && [client markedRange].location == NSNotFound);
    CHECK(sink.Got.size() == 1 && sink.Got[0] == "\xc3\xa9");
    [client setMarkedText:[[NSAttributedString alloc] initWithString:@"\u304b"] selectedRange:NSMakeRange(1, 0)
         replacementRange:NSMakeRange(NSNotFound, 0)];
    CHECK([client hasMarkedText]);
    [client setMarkedText:@"" selectedRange:NSMakeRange(0, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
    CHECK(![client hasMarkedText]);
    [client insertText:[[NSAttributedString alloc] initWithString:@"\u6f22\u5b57"] replacementRange:NSMakeRange(NSNotFound, 0)];
    CHECK(sink.Got.size() == 2 && sink.Got[1] == "\xe6\xbc\xa2\xe5\xad\x97");
    [client doCommandBySelector:@selector(insertNewline:)];
    CHECK(sink.Got.size() == 2 && [client validAttributesForMarkedText].count == 0);
    CHECK([client attributedSubstringForProposedRange:NSMakeRange(0, 1) actualRange:nullptr] == nil);

    // In a window, with the game's view its first responder.
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 200, 100) styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered defer:YES];
    LSMTLView* view = [[LSMTLView alloc] initWithFrame:NSMakeRect(0, 0, 200, 100) device:nil display:0 hdr:NO vsync:YES];
    window.contentView = view;
    CHECK([window makeFirstResponder:view] && window.firstResponder == view);
    auto& state = State();
    state.Presenting.store(true);
    state.WantTextInput.store(false);
    ApplyTextInput((__bridge void*)view);
    CHECK(!TextInputActive() && window.firstResponder == view);

    // A text field has the keyboard: the client is the first responder.
    state.WantTextInput.store(true);
    ApplyTextInput((__bridge void*)view);
    CHECK(TextInputActive() && window.firstResponder == (__bridge NSResponder*)TextClient() && !TextInputComposing());
    // The input method's window goes where ImGui's cursor is.
    state.SetIme(100, 50, 20);
    state.SetDrawableSize(400, 200);
    NSRect rect = [client firstRectForCharacterRange:NSMakeRange(0, 0) actualRange:nullptr];
    NSRect expected = [window convertRectToScreen:NSMakeRect(50, 100 - 35, 1, 10)];
    CHECK(NSEqualRects(rect, expected));

    // Through the tap: a key-down the overlay takes is the input system's, and the game does not get it.
    uint64_t sendEvent = ImplementationOf([BG3Application class], @selector(sendEvent:));
    uint64_t viewInit = ImplementationOf([LSMTLView class], @selector(initWithFrame:device:display:hdr:vsync:));
    Sink events;
    CHECK(InstallEventTap(EventDependencies(sendEvent, viewInit), &events).empty());
    BG3Application* app = [[BG3Application alloc] init];
    NSEvent* letter = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil
        characters:@"a" charactersIgnoringModifiers:@"a" isARepeat:NO keyCode:0];
    events.Deliver = false;
    [app sendEvent:letter];
    CHECK(events.Events == 1 && events.Last.TextByInputSystem && !events.Last.Composing && app.received == nil);
    // ... and during a composition, the input method's key.
    [client setMarkedText:@"k" selectedRange:NSMakeRange(1, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
    CHECK(TextInputComposing());
    [app sendEvent:letter];
    CHECK(events.Events == 2 && events.Last.TextByInputSystem && events.Last.Composing && app.received == nil);
    // A key with Command is the application's, as before.
    events.Deliver = true;
    NSEvent* shortcut = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:NSEventModifierFlagCommand timestamp:0
        windowNumber:0 context:nil characters:@"q" charactersIgnoringModifiers:@"q" isARepeat:NO keyCode:0x0c];
    [app sendEvent:shortcut];
    CHECK(events.Events == 3 && !events.Last.TextByInputSystem && !events.Last.Composing && app.received == shortcut);
    // A key up is not the input system's.
    NSEvent* up = [NSEvent keyEventWithType:NSEventTypeKeyUp location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil
        characters:@"a" charactersIgnoringModifiers:@"a" isARepeat:NO keyCode:0];
    [app sendEvent:up];
    CHECK(events.Events == 4 && !events.Last.TextByInputSystem && app.received == up);
    RemoveEventTap();
    [client setMarkedText:@"k" selectedRange:NSMakeRange(1, 0) replacementRange:NSMakeRange(NSNotFound, 0)];
    CHECK(TextInputComposing());
    // A held key repeats in a text field, as SDL has it.
    CHECK(![[NSUserDefaults standardUserDefaults] boolForKey:@"ApplePressAndHoldEnabled"]);

    // What the window still sends its first responder goes to the game's view.
    NSEvent* command = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:NSEventModifierFlagCommand timestamp:0
        windowNumber:0 context:nil characters:@"q" charactersIgnoringModifiers:@"q" isARepeat:NO keyCode:0x0c];
    [(__bridge NSResponder*)TextClient() keyDown:command];
    NSEvent* flags = [NSEvent keyEventWithType:NSEventTypeFlagsChanged location:NSZeroPoint modifierFlags:NSEventModifierFlagShift timestamp:0
        windowNumber:0 context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:0x38];
    [(__bridge NSResponder*)TextClient() flagsChanged:flags];
    CHECK(view.keysDown == 1 && view.flagChanges == 1);

    // No text field any more: the game's view has its place back, and the composition is gone.
    state.WantTextInput.store(false);
    ApplyTextInput((__bridge void*)view);
    CHECK(!TextInputActive() && !TextInputComposing() && window.firstResponder == view && ((__bridge NSView*)TextClient()).superview == nil);
    // Taken out while active: the same.
    state.WantTextInput.store(true);
    ApplyTextInput((__bridge void*)view);
    CHECK(TextInputActive());
    RemoveTextInput();
    CHECK(!TextInputInstalled() && !TextInputActive() && window.firstResponder == view);
    ApplyTextInput((__bridge void*)view);
    CHECK(!TextInputActive());
    state.Presenting.store(false);
    state.WantTextInput.store(false);
    state.SetDrawableSize(0, 0);
}

// --- the pointer ------------------------------------------------------------

TEST(the_overlay_has_the_cursor_while_it_has_the_pointer_and_gives_the_views_back)
{
    // The game has its application; AppKit gives some cursors only then.
    [NSApplication sharedApplication];
    // The cursors AppKit has for ImGui's.
    // The I-beam is made anew each time AppKit is asked for it: the overlay's is made once.
    CHECK(CursorFor(ImGuiMouseCursor_TextInput) && CursorFor(ImGuiMouseCursor_TextInput) == CursorFor(ImGuiMouseCursor_TextInput)
        && CursorFor(ImGuiMouseCursor_TextInput) != CursorFor(ImGuiMouseCursor_Arrow));
    CHECK(CursorFor(-5) == CursorFor(ImGuiMouseCursor_Arrow) && CursorFor(ImGuiMouseCursor_COUNT) == CursorFor(ImGuiMouseCursor_Arrow));
    CHECK((__bridge NSCursor*)CursorFor(ImGuiMouseCursor_Hand) == NSCursor.pointingHandCursor);
    CHECK((__bridge NSCursor*)CursorFor(ImGuiMouseCursor_ResizeEW) == NSCursor.resizeLeftRightCursor);
    CHECK((__bridge NSCursor*)CursorFor(ImGuiMouseCursor_NotAllowed) == NSCursor.operationNotAllowedCursor);
    CHECK((__bridge NSCursor*)CursorFor(ImGuiMouseCursor_Wait) == NSCursor.arrowCursor);
    CHECK(CursorFor(ImGuiMouseCursor_ResizeNESW) != nullptr && CursorFor(ImGuiMouseCursor_ResizeNWSE) != nullptr);

    Dependencies d;
    d.Live = true;
    d.Checks.push_back({ Part::Events, "stated by the test", true, "" });
    d.Checks.push_back({ Part::Pointer, "a check", false, "the reason" });
    CHECK(Contains(InstallPointer(d), "the reason") && !PointerInstalled());
    d.Checks.back().Passed = true;

    LSMTLView* view = [[LSMTLView alloc] initWithFrame:NSMakeRect(0, 0, 100, 50) device:nil display:0 hdr:NO vsync:YES];
    view->CurrentCursor = NSCursor.crosshairCursor;
    ApplyPointer((__bridge void*)view);     // not installed: nothing
    CHECK(view.trackingAreas.count == 0);
    CHECK(InstallPointer(d).empty() && PointerInstalled());

    auto& state = State();
    state.Presenting.store(true);
    state.WantCaptureMouse.store(false);
    state.MouseCursor.store(ImGuiMouseCursor_Hand);
    uint32_t serial = state.WarpSerial.load();
    // The tracking area goes on the view; the cursor stays the game's while the overlay does not have the pointer.
    [NSCursor.crosshairCursor set];
    ApplyPointer((__bridge void*)view);
    CHECK(view.trackingAreas.count == 1 && IsPointerTrackingArea((__bridge void*)view.trackingAreas.firstObject));
    CHECK(NSCursor.currentCursor == NSCursor.crosshairCursor);
    ApplyPointer((__bridge void*)view);
    CHECK(view.trackingAreas.count == 1);

    // The overlay has the pointer: its cursor; then the view's own again.
    state.WantCaptureMouse.store(true);
    ApplyPointer((__bridge void*)view);
    CHECK(NSCursor.currentCursor == NSCursor.pointingHandCursor);
    // The view sets its own meanwhile (its cursorUpdate:): the overlay's again.
    [NSCursor.crosshairCursor set];
    ApplyPointer((__bridge void*)view);
    CHECK(NSCursor.currentCursor == NSCursor.pointingHandCursor);
    state.WantCaptureMouse.store(false);
    ApplyPointer((__bridge void*)view);
    CHECK(NSCursor.currentCursor == NSCursor.crosshairCursor);

    // The pointer leaving the view, through the overlay's area only.
    NSTrackingArea* area = view.trackingAreas.firstObject;
    NSEvent* exited = [NSEvent enterExitEventWithType:NSEventTypeMouseExited location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
        context:nil eventNumber:0 trackingNumber:(NSInteger)(__bridge void*)area userData:nullptr];
    CHECK(exited.trackingArea == area);
    CocoaEvent described = DescribeEvent((__bridge void*)exited, nullptr);
    CHECK(described.Kind == CocoaEventKind::MouseExited);
    NSTrackingArea* other = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways
        owner:view userInfo:nil];
    NSEvent* otherExited = [NSEvent enterExitEventWithType:NSEventTypeMouseExited location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0
        context:nil eventNumber:0 trackingNumber:(NSInteger)(__bridge void*)other userData:nullptr];
    CHECK(DescribeEvent((__bridge void*)otherExited, nullptr).Kind == CocoaEventKind::Other);

    // Taken out: the area goes, the cursor is the view's.
    state.WantCaptureMouse.store(true);
    ApplyPointer((__bridge void*)view);
    CHECK(NSCursor.currentCursor == NSCursor.pointingHandCursor);
    RemovePointer();
    CHECK(!PointerInstalled() && view.trackingAreas.count == 0 && NSCursor.currentCursor == NSCursor.crosshairCursor);
    // Nothing asked the pointer to move.
    CHECK(state.WarpSerial.load() == serial);
    state.Presenting.store(false);
    state.WantCaptureMouse.store(false);
    state.MouseCursor.store(kKeepCursor);
    [NSCursor.arrowCursor set];
}

// --- controllers ------------------------------------------------------------

struct ControllerEvents : ControllerSink
{
    std::vector<SDL_Event> Events;
    bool Let{ true };
    bool OnControllerEvent(SDL_Event const& event) override
    {
        Events.push_back(event);
        return Let;
    }
};

// The game's tables, as CocoaBindingSDM::Init writes them.
void FillControllerButtons(std::array<uint16_t, 0x23>& table)
{
    uint16_t const expected[][2] = { { 1, 164 }, { 2, 165 }, { 4, 166 }, { 8, 167 }, { 16, 159 }, { 17, 157 }, { 18, 160 }, { 19, 161 },
        { 20, 162 }, { 21, 163 }, { 22, 153 }, { 23, 154 }, { 24, 155 }, { 25, 156 }, { 34, 157 } };
    for (auto const& entry : expected) table[entry[0]] = entry[1];
}
std::array<uint16_t, 12> const kGameHalfAxes = { 140, 142, 143, 141, 146, 149, 150, 148, 151, 151, 152, 152 };

// A device of the game's as the tracker reads it (offsets of the test's):
// acquired at +0x10, the player at +0x11, an MFi device's GCController at
// +0x18, an IOHID device's GenData* array at +0x20 and their count at +0x28;
// a GenData's value at +0x18. An ls::InputDevice's id at +0x10.
struct TestDevice
{
    alignas(8) uint8_t Bytes[0x30] = {};
    void* GenData[6] = {};
    struct Axis { alignas(8) uint8_t Bytes[0x20] = {}; } Axes[6];

    TestDevice(int player, GCController* controller = nil)
    {
        Bytes[0x10] = 1;
        Bytes[0x11] = (uint8_t)player;
        *reinterpret_cast<void**>(Bytes + 0x18) = (__bridge void*)controller;
        *reinterpret_cast<void**>(Bytes + 0x20) = GenData;
    }
    // An IOHID device with `count` axes.
    void HasAxes(int count)
    {
        for (int n = 0; n < 6; n++) GenData[n] = n < count ? Axes[n].Bytes : nullptr;
        *reinterpret_cast<uint32_t*>(Bytes + 0x28) = (uint32_t)count;
    }
    void Set(int axis, double value) { *reinterpret_cast<double*>(Axes[axis].Bytes + 0x18) = value; }
};

ControllerPlaces TestPlaces(std::array<uint16_t, 0x23> const& buttons, std::span<uint16_t const> halfAxes = {})
{
    ControllerPlaces places;
    places.Acquired = 0x10;
    places.Player = 0x11;
    places.Controller = 0x18;
    places.HidAxes = 0x20;
    places.HidAxisCount = 0x28;
    places.HidAxisValue = 0x18;
    places.DeviceId = 0x10;
    places.Buttons = buttons;
    places.HalfAxes = halfAxes;
    return places;
}

// What RegisterDevice and UnregisterDevice are given for the controller at `player`.
void Register(ControllerTracker& tracker, int player)
{
    alignas(8) uint8_t inputDevice[0x28] = {};
    *reinterpret_cast<int16_t*>(inputDevice + 0x10) = (int16_t)(kFirstControllerDevice + player);
    tracker.BeforeRegister(inputDevice);
}
void Unregister(ControllerTracker& tracker, int player) { tracker.BeforeUnregister((int32_t)(kFirstControllerDevice + player)); }

GameInputChange Change(int player, uint16_t input, bool pressed)
{
    GameInputChange change{};
    change.Input = (uint32_t)(kFirstControllerDevice + player) << 16 | input;
    change.Pressed = pressed;
    return change;
}

TEST(a_controller_is_read_before_the_game_and_a_kept_button_is_not_queued)
{
    GCController* controller = [GCController controllerWithExtendedGamepad];
    GCExtendedGamepad* pad = controller.extendedGamepad;
    CHECK(pad != nil);
    if (!pad) return;
    TestDevice device(1, controller);
    std::array<uint16_t, 0x23> table{};
    ControllerTracker tracker(TestPlaces(table));
    ControllerEvents sink;
    tracker.SetSink(&sink);
    GamepadState state;

    // The game has not set up its table: nothing is read, nothing kept.
    CHECK(tracker.BeforeRead(device.Bytes).Player == -1 && sink.Events.empty() && !tracker.FirstGamepad(state));
    FillControllerButtons(table);

    // The first read: all centred, as SDL's first state. The controller the
    // game had before it was watched is numbered now: the first, 0.
    auto read = tracker.BeforeRead(device.Bytes);
    CHECK(read.Player == 1 && sink.Events.empty() && tracker.FirstGamepad(state));
    [pad.leftThumbstick setValueForXAxis:0.5f yAxis:1.0f];
    [pad.rightTrigger setValue:1.0f];
    [pad.buttonA setValue:1.0f];
    read = tracker.BeforeRead(device.Bytes);
    // Three axes, and no button: the game queues A itself.
    CHECK(sink.Events.size() == 3);
    if (sink.Events.size() == 3) {
        CHECK(sink.Events[0].caxis.axis == SDL_CONTROLLER_AXIS_LEFTX && sink.Events[0].caxis.value == 16383 && sink.Events[0].caxis.which == 0);
        CHECK(sink.Events[1].caxis.axis == SDL_CONTROLLER_AXIS_LEFTY && sink.Events[1].caxis.value == -32767);
        CHECK(sink.Events[2].caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT && sink.Events[2].caxis.value == 32767);
    }
    CHECK(tracker.FirstGamepad(state) && state.Buttons[SDL_CONTROLLER_BUTTON_A] && state.Axes[SDL_CONTROLLER_AXIS_LEFTY] == -32767);

    // The change the game then queues for A: told, and kept from the game when the sink says so.
    sink.Events.clear();
    auto change = Change(1, 153, true);
    CHECK(tracker.BeforeInject(change, &read));
    sink.Let = false;
    CHECK(!tracker.BeforeInject(change, &read));
    CHECK(sink.Events.size() == 2 && sink.Events[1].type == SDL_CONTROLLERBUTTONDOWN && sink.Events[1].cbutton.button == SDL_CONTROLLER_BUTTON_A
        && sink.Events[1].cbutton.which == 0);
    // A half axis of the game's, a key: not the sink's, always the game's.
    sink.Events.clear();
    change.Input = 4u << 16 | 168;
    CHECK(tracker.BeforeInject(change, &read));
    change.Input = 0x00000041;
    CHECK(tracker.BeforeInject(change, nullptr));
    CHECK(sink.Events.empty());
    sink.Let = true;

    // A device the game does not read: not read either.
    device.Bytes[0x10] = 0;
    CHECK(tracker.BeforeRead(device.Bytes).Player == -1);
    device.Bytes[0x10] = 1;
    *reinterpret_cast<void**>(device.Bytes + 0x18) = nullptr;
    CHECK(tracker.BeforeRead(device.Bytes).Player == -1);

    // A table that is not the expected one turns the tracker off.
    std::array<uint16_t, 0x23> other = table;
    other[22] = 1;
    ControllerTracker wrong(TestPlaces(other));
    wrong.SetSink(&sink);
    *reinterpret_cast<void**>(device.Bytes + 0x18) = (__bridge void*)controller;
    change.Input = 4u << 16 | 153;
    CHECK(wrong.BeforeRead(device.Bytes).Player == -1 && wrong.BeforeInject(change, nullptr) && sink.Events.empty());
    tracker.SetSink(nullptr);
    wrong.SetSink(nullptr);
}

TEST(a_controller_has_the_instance_id_sdl_would_give_it_while_the_game_has_it)
{
    std::array<uint16_t, 0x23> table{};
    FillControllerButtons(table);
    ControllerTracker tracker(TestPlaces(table));
    ControllerEvents sink;
    tracker.SetSink(&sink);
    GCController* first = [GCController controllerWithExtendedGamepad];
    GCController* second = [GCController controllerWithExtendedGamepad];
    TestDevice a(0, first), b(1, second);
    [first.extendedGamepad.leftTrigger setValue:1.0f];
    [second.extendedGamepad.leftTrigger setValue:1.0f];

    // The game opens two controllers: 0 and 1.
    Register(tracker, 0);
    Register(tracker, 1);
    auto readA = tracker.BeforeRead(a.Bytes);
    tracker.BeforeRead(b.Bytes);
    CHECK(sink.Events.size() == 2 && sink.Events[0].caxis.which == 0 && sink.Events[1].caxis.which == 1);
    // A button, an axis and the menu button's "start" (queued outside a read) of one controller: one id.
    sink.Events.clear();
    CHECK(tracker.BeforeInject(Change(0, 154, true), &readA) && tracker.BeforeInject(Change(0, 159, true), nullptr));
    [first.extendedGamepad.leftTrigger setValue:0.0f];
    tracker.BeforeRead(a.Bytes);
    CHECK(sink.Events.size() == 3);
    for (auto const& event : sink.Events) CHECK(event.cbutton.which == 0 && event.caxis.which == 0);

    // The first goes, holding its right trigger and a button: SDL centres it
    // and lets go under its id. The game's own letting go after: not told again.
    [first.extendedGamepad.rightTrigger setValue:0.5f];
    tracker.BeforeRead(a.Bytes);
    CHECK(tracker.BeforeInject(Change(0, 155, true), nullptr));
    sink.Events.clear();
    CHECK(tracker.BeforeUnregister(3) == 0);
    CHECK(sink.Events.size() == 2 && sink.Events[0].caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT && sink.Events[0].caxis.value == 0
        && sink.Events[0].caxis.which == 0 && sink.Events[1].type == SDL_CONTROLLERBUTTONUP
        && sink.Events[1].cbutton.button == SDL_CONTROLLER_BUTTON_X && sink.Events[1].cbutton.which == 0);
    sink.Let = false;
    CHECK(tracker.BeforeInject(Change(0, 155, false), nullptr) && sink.Events.size() == 2);
    sink.Let = true;
    GamepadState state;
    CHECK(tracker.FirstGamepad(state));         // the second's

    // It comes back at the same player index: a new id, 2; its first read is
    // from SDL's first state, not from the state of the one that went.
    [first.extendedGamepad.rightTrigger setValue:0.0f];
    sink.Events.clear();
    TestDevice again(0, first);
    Register(tracker, 0);
    tracker.BeforeRead(again.Bytes);
    CHECK(sink.Events.empty());                 // centred, released: nothing to tell
    [first.extendedGamepad.rightTrigger setValue:1.0f];
    tracker.BeforeRead(again.Bytes);
    CHECK(sink.Events.size() == 1 && sink.Events[0].caxis.which == 2 && sink.Events[0].caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    CHECK(tracker.BeforeInject(Change(0, 153, true), nullptr) && sink.Events.back().cbutton.which == 2);
    tracker.BeforeRead(b.Bytes);
    CHECK(tracker.BeforeInject(Change(1, 153, true), nullptr) && sink.Events.back().cbutton.which == 1);

    // Closed and opened again at once by OpenController: SDL never sees it go.
    sink.Events.clear();
    int32_t kept = tracker.BeforeUnregister(3, true);
    CHECK(kept == 2 && sink.Events.empty());
    alignas(8) uint8_t reopened[0x28] = {};
    *reinterpret_cast<int16_t*>(reopened + 0x10) = 3;
    tracker.BeforeRegister(reopened, kept);
    CHECK(tracker.BeforeInject(Change(0, 153, false), nullptr) && sink.Events.back().cbutton.which == 2);

    // All go (CloseAllControllers) and come back in the other order: 3 and 4.
    Unregister(tracker, 0);
    Unregister(tracker, 1);
    CHECK(sink.Events.back().type == SDL_CONTROLLERBUTTONUP && sink.Events.back().cbutton.which == 1);   // its A, held
    Register(tracker, 1);
    Register(tracker, 0);
    CHECK(tracker.BeforeInject(Change(1, 153, false), nullptr) && sink.Events.back().cbutton.which == 3);
    CHECK(tracker.BeforeInject(Change(0, 153, false), nullptr) && sink.Events.back().cbutton.which == 4);
    // Devices that are not controllers (the keyboard, the mouse, the touch bar) are not numbered.
    tracker.BeforeUnregister(0);
    tracker.BeforeUnregister(11);
    alignas(8) uint8_t keyboard[0x28] = {};
    tracker.BeforeRegister(keyboard);
    Register(tracker, 2);
    CHECK(tracker.BeforeInject(Change(2, 153, true), nullptr) && sink.Events.back().cbutton.which == 5);
    tracker.SetSink(nullptr);
}

TEST(an_iohid_controllers_axes_are_read_where_the_game_reads_them)
{
    std::array<uint16_t, 0x23> table{};
    FillControllerButtons(table);
    std::array<uint16_t, 12> halves{};
    ControllerTracker tracker(TestPlaces(table, halves));
    ControllerEvents sink;
    tracker.SetSink(&sink);
    TestDevice device(2);
    device.HasAxes(6);
    Register(tracker, 2);

    // The half axes not yet set up: no axes, but its buttons still give events.
    device.Set(0, 1000);
    CHECK(tracker.BeforeHidRead(device.Bytes).Player == -1 && sink.Events.empty());
    CHECK(tracker.BeforeInject(Change(2, 153, true), nullptr) && sink.Events.size() == 1 && sink.Events[0].cbutton.which == 0);
    std::copy(kGameHalfAxes.begin(), kGameHalfAxes.end(), halves.begin());

    // The values the callback stored, in SDL's values: y down as SDL's (the
    // game negates it for itself), a trigger from 0.
    sink.Events.clear();
    device.Set(1, 2000.0);
    device.Set(4, 40000.0);         // past the range, by a device's override
    auto read = tracker.BeforeHidRead(device.Bytes);
    CHECK(read.Player == 2 && !read.TouchpadChanged);
    CHECK(sink.Events.size() == 3);
    if (sink.Events.size() == 3) {
        CHECK(sink.Events[0].caxis.axis == SDL_CONTROLLER_AXIS_LEFTX && sink.Events[0].caxis.value == 1000 && sink.Events[0].caxis.which == 0);
        CHECK(sink.Events[1].caxis.axis == SDL_CONTROLLER_AXIS_LEFTY && sink.Events[1].caxis.value == 2000);
        CHECK(sink.Events[2].caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT && sink.Events[2].caxis.value == 32767);
    }
    // The navigation has its axes, and the button the game queued.
    GamepadState state;
    CHECK(tracker.FirstGamepad(state) && state.Buttons[SDL_CONTROLLER_BUTTON_A] && state.Axes[SDL_CONTROLLER_AXIS_LEFTY] == 2000);
    // A button the game does not queue is never told from the axes' reads.
    sink.Events.clear();
    tracker.BeforeHidRead(device.Bytes);
    CHECK(sink.Events.empty());

    // Four axes or fewer: the game takes no triggers, whatever the array holds.
    *reinterpret_cast<uint32_t*>(device.Bytes + 0x28) = 4;
    tracker.BeforeHidRead(device.Bytes);
    CHECK(sink.Events.size() == 1 && sink.Events[0].caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT && sink.Events[0].caxis.value == 0);
    // ... and none after the first it does not have.
    sink.Events.clear();
    device.GenData[1] = nullptr;
    tracker.BeforeHidRead(device.Bytes);
    CHECK(sink.Events.size() == 1 && sink.Events[0].caxis.axis == SDL_CONTROLLER_AXIS_LEFTY && sink.Events[0].caxis.value == 0);
    // Not acquired: not read.
    sink.Events.clear();
    device.Bytes[0x10] = 0;
    CHECK(tracker.BeforeHidRead(device.Bytes).Player == -1 && sink.Events.empty());

    // Half axes in another order: no axes, ever; its buttons still go.
    std::array<uint16_t, 12> swapped = kGameHalfAxes;
    std::swap(swapped[2], swapped[3]);
    ControllerTracker wrong(TestPlaces(table, swapped));
    wrong.SetSink(&sink);
    TestDevice other(0);
    other.HasAxes(6);
    other.Set(0, 500);
    CHECK(wrong.BeforeHidRead(other.Bytes).Player == -1 && sink.Events.empty());
    CHECK(wrong.BeforeInject(Change(0, 154, true), nullptr) && sink.Events.size() == 1);
    // A tracker without the places of the axes reads none.
    ControllerPlaces noAxes = TestPlaces(table, kGameHalfAxes);
    noAxes.HidAxes = 0;
    ControllerTracker without(noAxes);
    without.SetSink(&sink);
    sink.Events.clear();
    CHECK(without.BeforeHidRead(other.Bytes).Player == -1 && sink.Events.empty());
    tracker.SetSink(nullptr);
    wrong.SetSink(nullptr);
    without.SetSink(nullptr);
}

}

// Stand-ins for the game's two functions, with a prologue that builds a
// frame as the game's do; each counts its calls.
int gUpdateControlsCalls = 0, gInjectInputCalls = 0;
uint32_t gInjectedInput = 0;

extern "C" __attribute__((noinline)) void OverlayTestUpdateControls(void* device)
{
    volatile uintptr_t scratch[8];
    for (int i = 0; i < 8; i++) scratch[i] = (uintptr_t)device + i;
    printf("%s", "");
    gUpdateControlsCalls += scratch[3] != 0;
}

extern "C" __attribute__((noinline)) void OverlayTestInjectInput(void* manager, GameInputChange const* change, bool fromWheel)
{
    volatile uintptr_t scratch[8];
    for (int i = 0; i < 8; i++) scratch[i] = (uintptr_t)manager + i + fromWheel;
    printf("%s", "");
    gInjectedInput = change->Input;
    gInjectInputCalls += scratch[3] != 0;
}

int gRegisterCalls = 0, gUnregisterCalls = 0, gHidUpdateCalls = 0;

extern "C" __attribute__((noinline)) void OverlayTestRegisterDevice(void* manager, void const* device)
{
    volatile uintptr_t scratch[8];
    for (int i = 0; i < 8; i++) scratch[i] = (uintptr_t)device + i + (uintptr_t)manager;
    printf("%s", "");
    gRegisterCalls += scratch[3] != 0;
}

extern "C" __attribute__((noinline)) void OverlayTestUnregisterDevice(void* manager, int32_t deviceId)
{
    volatile uintptr_t scratch[8];
    for (int i = 0; i < 8; i++) scratch[i] = (uintptr_t)manager + i + (uintptr_t)deviceId;
    printf("%s", "");
    gUnregisterCalls += scratch[3] != 0;
}

// The game's OpenController(GCController*) for a controller it has open: it
// closes it (unregisters it) and opens it again (registers it), through the
// functions the hooks are on.
int gOpenCalls = 0;
void const* gReopened = nullptr;
extern "C" void OverlayTestRegisterDevice(void* manager, void const* device);
extern "C" void OverlayTestUnregisterDevice(void* manager, int32_t deviceId);

extern "C" __attribute__((noinline)) void OverlayTestOpenController(void* manager, void* controller)
{
    volatile uintptr_t scratch[8];
    for (int i = 0; i < 8; i++) scratch[i] = (uintptr_t)manager + i + (uintptr_t)controller;
    printf("%s", "");
    gOpenCalls += scratch[3] != 0;
    void (*volatile registerDevice)(void*, void const*) = &OverlayTestRegisterDevice;
    void (*volatile unregisterDevice)(void*, int32_t) = &OverlayTestUnregisterDevice;
    unregisterDevice(manager, *reinterpret_cast<int16_t const*>(static_cast<uint8_t const*>(gReopened) + 0x10));
    registerDevice(manager, gReopened);
}

extern "C" __attribute__((noinline)) void OverlayTestHidUpdateControls(void* device)
{
    volatile uintptr_t scratch[8];
    for (int i = 0; i < 8; i++) scratch[i] = (uintptr_t)device + i;
    printf("%s", "");
    gHidUpdateCalls += scratch[3] != 0;
}

namespace {

TEST(the_controller_hooks_go_in_only_on_the_game_that_was_checked)
{
    ControllerEvents sink;
    Dependencies d;
    d.Live = true;
    d.Checks.push_back({ Part::Controllers, "a check", false, "the reason" });
    CHECK(Contains(InstallControllerHooks(d, &sink), "the reason"));
    Dependencies fromFile;
    fromFile.Checks.push_back({ Part::Controllers, "stated by the test", true, "" });
    CHECK(!InstallControllerHooks(fromFile, &sink).empty());
    CHECK(!InstallControllerHooks(d, nullptr).empty());
    GamepadState state;
    CHECK(!ControllerHooksInstalled() && !FirstGamepad(state));

    // On the stand-ins, over their extents in this executable's symbol table.
    auto image = GameImage::Current();
    CHECK(image != nullptr);
    if (!image) return;
    struct StandIn
    {
        char const* Name;
        void const* Code;
        uint64_t Address{ 0 }, Bytes{ 0 };
    } standIns[] = {
        { "_OverlayTestUpdateControls", (void const*)&OverlayTestUpdateControls },
        { "_OverlayTestInjectInput", (void const*)&OverlayTestInjectInput },
        { "_OverlayTestRegisterDevice", (void const*)&OverlayTestRegisterDevice },
        { "_OverlayTestUnregisterDevice", (void const*)&OverlayTestUnregisterDevice },
        { "_OverlayTestHidUpdateControls", (void const*)&OverlayTestHidUpdateControls },
        { "_OverlayTestOpenController", (void const*)&OverlayTestOpenController },
    };
    for (auto& standIn : standIns) {
        uint64_t va = image->FindVA(standIn.Name);
        CHECK(va && image->CodeAt(va) == standIn.Code);
        if (!va) return;
        standIn.Address = (uint64_t)(uintptr_t)standIn.Code;
        standIn.Bytes = image->NextSymbolVA(va) - va;
    }
    static std::array<uint16_t, 0x23> table{};
    FillControllerButtons(table);
    static std::array<uint16_t, 12> halves = kGameHalfAxes;
    Dependencies on;
    on.Live = true;
    on.Checks.push_back({ Part::Controllers, "stated by the test", true, "" });
    on.UpdateControls = standIns[0].Address;
    on.UpdateControlsBytes = standIns[0].Bytes;
    on.InjectInput = standIns[1].Address;
    on.InjectInputBytes = standIns[1].Bytes;
    on.RegisterDevice = standIns[2].Address;
    on.RegisterDeviceBytes = standIns[2].Bytes;
    on.UnregisterDevice = standIns[3].Address;
    on.UnregisterDeviceBytes = standIns[3].Bytes;
    on.HidUpdateControls = standIns[4].Address;
    on.HidUpdateControlsBytes = standIns[4].Bytes;
    on.OpenController = standIns[5].Address;
    on.OpenControllerBytes = standIns[5].Bytes;
    on.DeviceEnabled = 0x10;
    on.DeviceController = 0x18;
    on.DevicePlayer = 0x11;
    on.RegisteredDeviceId = 0x10;
    on.ControllerButtons = (uint64_t)(uintptr_t)table.data();
    on.ControllerButtonCount = table.size();
    on.HidAxes = 0x20;
    on.HidAxisCount = 0x28;
    on.HidAxisValue = 0x18;
    on.ControllerAxes = (uint64_t)(uintptr_t)halves.data();
    on.ControllerAxisCount = halves.size();
    // The game's literal for a trigger's minimum: an NSNumber the hooks ask for its value.
    static NSNumber* minimum = @1.0f;
    on.TriggerCalibrationMin = (uint64_t)(uintptr_t)(__bridge void*)minimum;
    on.Checks.push_back({ Part::HidControllerAxes, "stated by the test", true, "" });

    // A trigger's minimum that is not 0: the IOHID axes are left out, the rest goes in.
    std::string problem = InstallControllerHooks(on, &sink);
    CHECK(problem.empty() && ControllerHooksInstalled() && !HidAxesInstalled());
    RemoveControllerHooks();
    minimum = @0.0f;
    on.TriggerCalibrationMin = (uint64_t)(uintptr_t)(__bridge void*)minimum;
    problem = InstallControllerHooks(on, &sink);
    CHECK(problem.empty() && ControllerHooksInstalled() && HidAxesInstalled());
    if (!problem.empty()) {
        printf("  %s\n", problem.c_str());
        return;
    }

    GCController* controller = [GCController controllerWithExtendedGamepad];
    TestDevice device(0, controller);
    void (*volatile updateControls)(void*) = &OverlayTestUpdateControls;
    void (*volatile hidUpdateControls)(void*) = &OverlayTestHidUpdateControls;
    void (*volatile injectInput)(void*, GameInputChange const*, bool) = &OverlayTestInjectInput;
    void (*volatile registerDevice)(void*, void const*) = &OverlayTestRegisterDevice;
    void (*volatile unregisterDevice)(void*, int32_t) = &OverlayTestUnregisterDevice;
    alignas(8) uint8_t inputDevice[0x28] = {};
    *reinterpret_cast<int16_t*>(inputDevice + 0x10) = 3;

    // The controller arrives, then is read: the hook's read first, then the game's.
    registerDevice(nullptr, inputDevice);
    updateControls(device.Bytes);
    [controller.extendedGamepad.leftTrigger setValue:1.0f];
    updateControls(device.Bytes);
    CHECK(gRegisterCalls == 1 && gUpdateControlsCalls == 2 && sink.Events.size() == 1
        && sink.Events[0].caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT && sink.Events[0].caxis.which == 0);
    CHECK(FirstGamepad(state) && state.Axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] == 32767);

    // A button Lua keeps does not reach the game's function; one it lets go, and a key, do.
    auto change = Change(0, 154, true);
    sink.Let = false;
    injectInput(nullptr, &change, false);
    CHECK(gInjectInputCalls == 0 && sink.Events.size() == 2 && sink.Events[1].cbutton.button == SDL_CONTROLLER_BUTTON_B);
    sink.Let = true;
    injectInput(nullptr, &change, false);
    CHECK(gInjectInputCalls == 1 && gInjectedInput == change.Input);
    change.Input = 0x41;
    injectInput(nullptr, &change, false);
    CHECK(gInjectInputCalls == 2 && gInjectedInput == 0x41 && sink.Events.size() == 3);

    // The game is told of it again while it has it open: closed and opened again, the same id.
    void (*volatile openController)(void*, void*) = &OverlayTestOpenController;
    gReopened = inputDevice;
    openController(nullptr, (__bridge void*)controller);
    change = Change(0, 154, false);
    injectInput(nullptr, &change, false);
    CHECK(gOpenCalls == 1 && gUnregisterCalls == 1 && gRegisterCalls == 2 && sink.Events.size() == 4
        && sink.Events[3].cbutton.button == SDL_CONTROLLER_BUTTON_B && sink.Events[3].cbutton.which == 0);

    // It goes, centred as SDL centres it; an IOHID controller arrives at its
    // player index: the next id, its axes read.
    unregisterDevice(nullptr, 3);
    CHECK(sink.Events.size() == 5 && sink.Events[4].caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT && sink.Events[4].caxis.value == 0
        && sink.Events[4].caxis.which == 0);
    registerDevice(nullptr, inputDevice);
    TestDevice hid(0);
    hid.HasAxes(4);
    hid.Set(2, -32768);
    hidUpdateControls(hid.Bytes);
    CHECK(gUnregisterCalls == 2 && gRegisterCalls == 3 && gHidUpdateCalls == 1 && sink.Events.size() == 6
        && sink.Events[5].caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX && sink.Events[5].caxis.value == -32768 && sink.Events[5].caxis.which == 1);

    // Out: the game's functions alone.
    RemoveControllerHooks();
    CHECK(!ControllerHooksInstalled() && !HidAxesInstalled() && !FirstGamepad(state));
    sink.Let = false;
    change.Input = 3u << 16 | 154;
    injectInput(nullptr, &change, false);
    updateControls(device.Bytes);
    hidUpdateControls(hid.Bytes);
    registerDevice(nullptr, inputDevice);
    unregisterDevice(nullptr, 3);
    openController(nullptr, (__bridge void*)controller);
    CHECK(gInjectInputCalls == 4 && gUpdateControlsCalls == 3 && gHidUpdateCalls == 2 && gRegisterCalls == 5 && gUnregisterCalls == 4
        && gOpenCalls == 2 && sink.Events.size() == 6);
}

// --- the hooks, on the test library's functions -----------------------------

TEST(the_frame_hooks_go_in_only_on_the_game_that_was_checked)
{
    void* library = dlopen(BG3MSE_TEST_FUNCTIONS, RTLD_NOW);
    void* addOne = library ? dlsym(library, "lib_add_one") : nullptr;
    void* twice = library ? dlsym(library, "lib_twice") : nullptr;
    CHECK(addOne && twice);
    if (!addOne || !twice) return;

    // The "vtable slot" holds lib_twice, and lib_add_one stands in for the commit.
    static void* slot = nullptr;
    slot = twice;
    Dependencies d;
    d.Live = true;
    d.Checks.push_back({ Part::Frame, "stated by the test", true, "" });
    d.FlipBuffersSlot = (uint64_t)(uintptr_t)&slot;
    d.FlipBuffers = (uint64_t)(uintptr_t)twice;
    d.CommitCommandBuffer = (uint64_t)(uintptr_t)addOne;
    d.CommitCommandBufferBytes = 8;
    d.FrameDrawable = kDrawable;
    d.FrameRenderer = kRenderer;
    d.RendererCommandBuffer = kBuffer;
    Listener listener;

    // Checks that did not pass, a file's addresses, no listener: nothing is written.
    Dependencies off = d;
    off.Checks.push_back({ Part::Frame, "a check", false, "the reason" });
    CHECK(Contains(InstallFrameHooks(off, &listener), "the reason"));
    Dependencies fromFile = d;
    fromFile.Live = false;
    CHECK(!InstallFrameHooks(fromFile, &listener).empty());
    CHECK(!InstallFrameHooks(d, nullptr).empty());
    // The slot holds something else than the function that was checked.
    Dependencies moved = d;
    moved.FlipBuffers = (uint64_t)(uintptr_t)addOne;
    CHECK(Contains(InstallFrameHooks(moved, &listener), "vtable slot"));
    CHECK(!FrameHooksInstalled() && slot == twice && !FindEntryHook(addOne));

    CHECK(InstallFrameHooks(d, &listener).empty());
    CHECK(FrameHooksInstalled() && slot != twice);
    auto hook = FindEntryHook(addOne);
    CHECK(hook && hook->Committed());

    // Both hooks pass their call on, and a frame goes through them to the listener.
    using Fn = uint64_t (*)(void*, bool);
    reinterpret_cast<Fn>(slot)(nullptr, true);
    reinterpret_cast<Fn>(addOne)(nullptr, false);
    CHECK(listener.Frames == 0 && listener.Commits == 0);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device) {
        id<MTLCommandQueue> queue = [device newCommandQueue];
        OverlayTestDrawable* drawable = [[OverlayTestDrawable alloc] init];
        drawable.texture = MakeTarget(device, MTLPixelFormatBGRA8Unorm, 16);
        id<MTLCommandBuffer> buffer = [queue commandBufferWithUnretainedReferences];
        TestRenderer renderer{ nullptr, (__bridge void*)buffer };
        TestFrame frame{ {}, (__bridge void*)drawable, &renderer };
        reinterpret_cast<Fn>(slot)(&frame, true);
        CHECK(listener.Frames == 1 && listener.Last.Width == 16);
        reinterpret_cast<Fn>(addOne)(&renderer, false);
        CHECK(listener.Commits == 1);
        [buffer commit];
    }

    RemoveFrameHooks();
    CHECK(!FrameHooksInstalled() && slot == twice && !hook->Committed());
    CHECK(reinterpret_cast<Fn>(addOne)(reinterpret_cast<void*>(41), false) == 42);
    CHECK(reinterpret_cast<Fn>(twice)(reinterpret_cast<void*>(21), false) == 42);
}

}

// --- a window used with a controller alone -----------------------------------

// One controller, the game that reads it, a script and the overlay, a frame
// at a time: the hands hold buttons and sticks; the game reads its device
// (the tracker first, as the hooks have it) and queues what changed, each
// change past the tracker; the script is the Lua handler of
// ControllerButtonInput that opens a window on Start and, while it is open,
// keeps the presses from the game, as scripts written for Norbyte's extender
// do; the overlay then builds an ImGui frame with the first controller's
// state. `Game` is what reached the game's queue.
struct PadSession : ControllerSink
{
    static constexpr int kPlayer = 0;

    explicit PadSession(bool hid)
        : Hid(hid), Controller(hid ? nil : [GCController controllerWithExtendedGamepad]), Device(kPlayer, Controller),
          Tracker((FillControllerButtons(Table), TestPlaces(Table, kGameHalfAxes)))
    {
        if (Hid) Device.HasAxes(6);
        Tracker.SetSink(this);
        Register(Tracker, kPlayer);

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 720);
        // As IMGUIManager::InitializeUI sets them.
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        // As with the overlay's renderer: ImGui's textures are taken as they come, none is built first.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    }

    ~PadSession() override
    {
        Tracker.SetSink(nullptr);
        ImGui::DestroyContext();
    }

    // --- the script

    bool OnControllerEvent(SDL_Event const& event) override
    {
        Lua.push_back(event);
        if (event.type != SDL_CONTROLLERBUTTONDOWN) return true;
        if (Scripted && event.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
            // The way in and the way out; the game has the button too.
            Open = !Open;
            Focus = Open;
            return true;
        }
        if (!Scripted || !Open) return true;
        // A button the script uses itself and lets the game have.
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) return true;
        return false;
    }

    // --- the game

    // The game's read of the device, and the changes it queues of it.
    void Read()
    {
        ControllerRead read;
        if (Hid) {
            for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; axis++) Device.Set(axis, Hands.Axes[axis]);
            read = Tracker.BeforeHidRead(Device.Bytes);
        } else {
            GCExtendedGamepad* pad = Controller.extendedGamepad;
            auto set = [&](GCControllerButtonInput* button, SDL_GameControllerButton which) { [button setValue:Hands.Buttons[which] ? 1.0f : 0.0f]; };
            set(pad.buttonA, SDL_CONTROLLER_BUTTON_A);
            set(pad.buttonB, SDL_CONTROLLER_BUTTON_B);
            set(pad.buttonX, SDL_CONTROLLER_BUTTON_X);
            set(pad.buttonY, SDL_CONTROLLER_BUTTON_Y);
            set(pad.leftShoulder, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
            set(pad.rightShoulder, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
            set(pad.leftThumbstickButton, SDL_CONTROLLER_BUTTON_LEFTSTICK);
            set(pad.rightThumbstickButton, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
            set(pad.buttonOptions, SDL_CONTROLLER_BUTTON_BACK);
            set(pad.buttonMenu, SDL_CONTROLLER_BUTTON_START);
            [pad.dpad setValueForXAxis:(Hands.Buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] ? 1.0f : 0.0f) - (Hands.Buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] ? 1.0f : 0.0f)
                yAxis:(Hands.Buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] ? 1.0f : 0.0f) - (Hands.Buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] ? 1.0f : 0.0f)];
            [pad.leftThumbstick setValueForXAxis:Hands.Axes[SDL_CONTROLLER_AXIS_LEFTX] / 32767.0f yAxis:-Hands.Axes[SDL_CONTROLLER_AXIS_LEFTY] / 32767.0f];
            [pad.rightThumbstick setValueForXAxis:Hands.Axes[SDL_CONTROLLER_AXIS_RIGHTX] / 32767.0f yAxis:-Hands.Axes[SDL_CONTROLLER_AXIS_RIGHTY] / 32767.0f];
            read = Tracker.BeforeRead(Device.Bytes);
        }
        CHECK(read.Player == kPlayer);

        constexpr uint16_t kInputs[][2] = {
            { SDL_CONTROLLER_BUTTON_A, 153 }, { SDL_CONTROLLER_BUTTON_B, 154 }, { SDL_CONTROLLER_BUTTON_X, 155 }, { SDL_CONTROLLER_BUTTON_Y, 156 },
            { SDL_CONTROLLER_BUTTON_BACK, 157 }, { SDL_CONTROLLER_BUTTON_START, 159 }, { SDL_CONTROLLER_BUTTON_LEFTSTICK, 160 },
            { SDL_CONTROLLER_BUTTON_RIGHTSTICK, 161 }, { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 162 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 163 },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, 164 }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, 165 }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, 166 },
            { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 167 },
        };
        for (auto const& input : kInputs) {
            bool now = Hands.Buttons[input[0]];
            if (now == Queued.Buttons[input[0]]) continue;
            Queued.Buttons[input[0]] = now;
            if (!Hid && input[0] == SDL_CONTROLLER_BUTTON_START) {
                // The handler of a GameController's menu button queues the press whole, outside a read.
                if (!now) continue;
                Queue(Change(kPlayer, input[1], true), nullptr);
                Queue(Change(kPlayer, input[1], false), nullptr);
                continue;
            }
            Queue(Change(kPlayer, input[1], now), &read);
        }
        // A stick, as the half axis it is past the game's dead zone (the left stick's x is enough here).
        int16_t x = Hands.Axes[SDL_CONTROLLER_AXIS_LEFTX];
        if (x != Queued.Axes[SDL_CONTROLLER_AXIS_LEFTX]) {
            Queued.Axes[SDL_CONTROLLER_AXIS_LEFTX] = x;
            GameInputChange change = Change(kPlayer, x < 0 ? 140 : 142, x != 0);
            change.Value[0] = change.Value[1] = (float)std::abs(x) / 32767.0f;
            Queue(change, &read);
        }
    }

    void Queue(GameInputChange const& change, ControllerRead* read)
    {
        if (Tracker.BeforeInject(change, read)) Game.push_back(change);
    }

    // --- the overlay

    void Frame()
    {
        Read();
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        GamepadState state;
        ApplyGamepad(Tracker.FirstGamepad(state) ? &state : nullptr, io);
        ImGui::NewFrame();
        Focused.clear();
        if (Open) {
            // As Norbyte's window does it (extui::Window::BeginRender).
            if (Focus) ImGui::SetNextWindowFocus();
            Focus = false;
            ImGui::SetNextWindowPos(ImVec2(100, 100));
            ImGui::SetNextWindowSize(ImVec2(400, 300));
            bool open = true;
            if (ImGui::Begin("a script's window", &open, ImGuiWindowFlags_NoFocusOnAppearing)) {
                auto focused = [&](char const* name) { if (ImGui::IsItemFocused()) Focused = name; };
                if (ImGui::Button("First")) First++;
                focused("First");
                if (ImGui::Button("Second")) Second++;
                focused("Second");
                ImGui::Checkbox("Check", &Check);
                focused("Check");
                ImGui::SliderInt("Slider", &Value, 0, 10);
                focused("Slider");
                if (ImGui::Button("Close")) open = false;
                focused("Close");
            }
            ImGui::End();
            if (!open) Open = false;
        }
        ImGui::EndFrame();
    }

    void Frames(int count) { for (int n = 0; n < count; n++) Frame(); }

    void Hold(SDL_GameControllerButton button, int frames)
    {
        Hands.Buttons[button] = true;
        Frames(frames);
        Hands.Buttons[button] = false;
        Frames(2);
    }
    void Press(SDL_GameControllerButton button) { Hold(button, 2); }

    // How many changes of `input` (an InputRawType) reached the game, pressed or let go.
    int Reached(uint16_t input, bool pressed) const
    {
        int count = 0;
        for (auto const& change : Game) count += InputTypeOf(change.Input) == input && (change.Pressed != 0) == pressed;
        return count;
    }
    int Told(Uint32 type, int which) const
    {
        int count = 0;
        for (auto const& event : Lua) {
            count += event.type == type && (type == SDL_CONTROLLERAXISMOTION ? event.caxis.axis == which : event.cbutton.button == which);
        }
        return count;
    }
    bool Navigating() const { return GImGui->NavWindow != nullptr && ImGui::GetIO().NavActive; }

    bool Hid;
    std::array<uint16_t, 0x23> Table{};
    GCController* Controller;
    TestDevice Device;
    ControllerTracker Tracker;
    GamepadState Hands, Queued;
    std::vector<GameInputChange> Game;
    std::vector<SDL_Event> Lua;
    bool Scripted{ true };
    bool Open{ false }, Focus{ false };
    int First{ 0 }, Second{ 0 }, Value{ 5 };
    bool Check{ false };
    std::string Focused;
};

// The whole of what a controller player does with a script's window: in by
// the button the script listens for, through its widgets, a value changed,
// out again; and what the game was queued of it.
void AScriptsWindowIsUsedWithTheControllerAlone(bool hid)
{
    PadSession s(hid);
    s.Frames(3);
    CHECK(!s.Open && !s.Navigating() && (ImGui::GetIO().BackendFlags & ImGuiBackendFlags_HasGamepad));

    // Before the window: every press is the game's, and the script is told of it.
    s.Press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    s.Press(SDL_CONTROLLER_BUTTON_A);
    CHECK(s.Reached(165, true) == 1 && s.Reached(165, false) == 1 && s.Reached(153, true) == 1 && s.Reached(153, false) == 1);
    CHECK(s.Told(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_A) == 1 && s.Told(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_A) == 1);
    s.Game.clear();

    // Start: the script opens its window and gives it the focus; the game has Start as well.
    s.Press(SDL_CONTROLLER_BUTTON_START);
    CHECK(s.Open && s.Reached(159, true) == 1 && s.Reached(159, false) == 1);
    CHECK(s.Navigating() && strcmp(GImGui->NavWindow->Name, "a script's window") == 0);
    s.Game.clear();

    // The D-pad moves through the widgets, A presses the one it is on. The
    // script kept every press from the game; a button let go is the game's.
    s.Press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(s.Focused == "First");
    s.Press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(s.Focused == "Second");
    s.Press(SDL_CONTROLLER_BUTTON_A);
    CHECK(s.First == 0 && s.Second == 1);
    s.Press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(s.Focused == "Check" && !s.Check);
    s.Press(SDL_CONTROLLER_BUTTON_A);
    CHECK(s.Check);
    // A slider: A takes it, the D-pad changes its value, B gives it back.
    s.Press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(s.Focused == "Slider" && s.Value == 5);
    s.Press(SDL_CONTROLLER_BUTTON_A);
    s.Press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    CHECK(s.Value > 5);
    s.Press(SDL_CONTROLLER_BUTTON_B);
    int value = s.Value;
    s.Press(SDL_CONTROLLER_BUTTON_DPAD_UP);
    CHECK(s.Focused == "Check" && s.Value == value);
    CHECK(s.Reached(165, true) == 0 && s.Reached(164, true) == 0 && s.Reached(167, true) == 0 && s.Reached(153, true) == 0 && s.Reached(154, true) == 0);
    CHECK(s.Reached(165, false) == 4 && s.Reached(164, false) == 1 && s.Reached(167, false) == 1 && s.Reached(153, false) == 3 && s.Reached(154, false) == 1);
    CHECK(s.Told(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN) == 5 && s.Told(SDL_CONTROLLERBUTTONUP, SDL_CONTROLLER_BUTTON_DPAD_DOWN) == 5);

    // What the script lets through, and a stick, which no script can keep: the game's, with the window open.
    s.Game.clear();
    s.Press(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    CHECK(s.Reached(161, true) == 1 && s.Reached(161, false) == 1);
    s.Hands.Axes[SDL_CONTROLLER_AXIS_LEFTX] = 32767;
    s.Frames(2);
    s.Hands.Axes[SDL_CONTROLLER_AXIS_LEFTX] = 0;
    s.Frames(2);
    CHECK(s.Reached(142, true) == 1 && s.Reached(142, false) == 1);
    CHECK(s.Told(SDL_CONTROLLERAXISMOTION, SDL_CONTROLLER_AXIS_LEFTX) == 2);
    CHECK(s.Focused == "Check" && s.Value == value);       // the left stick is not the D-pad

    // Out by the window's own button, with the controller.
    s.Game.clear();
    s.Press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    s.Press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(s.Focused == "Close");
    s.Press(SDL_CONTROLLER_BUTTON_A);
    CHECK(!s.Open && s.Reached(153, true) == 0 && s.Reached(153, false) == 1);
    s.Frames(2);
    CHECK(!s.Navigating());
    // The game has its controller whole again.
    s.Game.clear();
    s.Press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    s.Press(SDL_CONTROLLER_BUTTON_A);
    CHECK(s.Reached(165, true) == 1 && s.Reached(165, false) == 1 && s.Reached(153, true) == 1 && s.Reached(153, false) == 1);

    // In again, and out by Start, the script's way.
    s.Press(SDL_CONTROLLER_BUTTON_START);
    CHECK(s.Open && s.Navigating());
    s.Press(SDL_CONTROLLER_BUTTON_START);
    CHECK(!s.Open);
    s.Frames(2);
    CHECK(!s.Navigating());
}

TEST(a_scripts_window_is_used_with_a_gamecontroller_pad_alone)
{
    AScriptsWindowIsUsedWithTheControllerAlone(false);
}

TEST(a_scripts_window_is_used_with_an_iohid_pad_alone)
{
    AScriptsWindowIsUsedWithTheControllerAlone(true);
}

// A window no script gave the focus and whose script knows nothing of
// controllers: ImGui's own gamepad controls reach it, as with Norbyte's
// overlay, and the game is queued every press as well, there as here.
void AWindowWithoutAScriptIsReachedWithImguisOwnControls(bool hid)
{
    PadSession s(hid);
    s.Scripted = false;
    // It appears once the overlay has drawn frames without it, as a window a
    // script shows later does (in a context's first frames ImGui gives the
    // focus to whatever is there).
    s.Frames(3);
    s.Open = true;
    s.Frames(3);
    CHECK(GImGui->NavWindow == nullptr);
    s.Press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(s.Focused.empty() && GImGui->NavWindow == nullptr);

    // X (Square) held: ImGui's window chooser; let go, the window has the focus.
    s.Hold(SDL_CONTROLLER_BUTTON_X, 30);
    CHECK(s.Navigating() && strcmp(GImGui->NavWindow->Name, "a script's window") == 0);
    // ... and its first widget the highlight.
    CHECK(s.Focused == "First");
    s.Press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    CHECK(s.Focused == "Second");
    s.Press(SDL_CONTROLLER_BUTTON_A);
    CHECK(s.First == 0 && s.Second == 1);

    // X tapped: the title bar; A on its close button closes the window.
    s.Press(SDL_CONTROLLER_BUTTON_X);
    CHECK(GImGui->NavLayer == ImGuiNavLayer_Menu);
    ImGuiID close = GImGui->NavWindow->GetID("#CLOSE");
    for (int tries = 0; tries < 4 && GImGui->NavId != close; tries++) s.Press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    CHECK(GImGui->NavId == close);
    s.Press(SDL_CONTROLLER_BUTTON_A);
    CHECK(!s.Open);

    // Nothing was kept from the game.
    CHECK(s.Reached(155, true) == 2 && s.Reached(155, false) == 2 && s.Reached(153, true) == 2 && s.Reached(153, false) == 2);
    CHECK(s.Reached(165, true) == 2 && s.Reached(165, false) == 2);
}

TEST(a_window_without_a_script_is_reached_with_imguis_own_gamepad_controls)
{
    AWindowWithoutAScriptIsReachedWithImguisOwnControls(false);
    AWindowWithoutAScriptIsReachedWithImguisOwnControls(true);
}


// The console window is an overlay window on macOS (Windows has a console of
// its own): with it shown, it is the window that has the focus when no
// other does, a script's window closed included. A controller's presses do
// not start its line's editing, so the keyboard stays the game's.
TEST(a_controller_does_not_take_the_keyboard_for_the_console_window)
{
    for (bool hid : { false, true }) {
        bg3se::shim::ConsoleScreen screen;
        screen.Open();
        ConsoleWindow console(screen, KeyChord{ 0x61, 0 });
        PadSession s(hid);
        struct Hooked { static void Draw(ImGuiContext*, ImGuiContextHook* hook) { static_cast<ConsoleWindow*>(hook->UserData)->Draw(); } };
        ImGuiContextHook hook;
        hook.Type = ImGuiContextHookType_NewFramePost;
        hook.Callback = &Hooked::Draw;
        hook.UserData = &console;
        ImGui::AddContextHook(GImGui, &hook);
        s.Frames(3);
        console.ConsoleOpened();
        s.Frames(3);
        auto consoleHasFocus = [] { return GImGui->NavWindow && strstr(GImGui->NavWindow->Name, "###bg3mse.console") != nullptr; };
        auto quiet = [] { return GImGui->ActiveId == 0 && !ImGui::GetIO().WantTextInput; };
        CHECK(console.Visible() && consoleHasFocus() && quiet());
        for (auto button : { SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B }) {
            s.Hands.Buttons[button] = true;
            s.Frames(2);
            CHECK(quiet());
            s.Hands.Buttons[button] = false;
            s.Frames(2);
            CHECK(quiet());
        }
        // A script's window takes the focus and gives it back to the console when it closes.
        s.Press(SDL_CONTROLLER_BUTTON_START);
        CHECK(s.Open && GImGui->NavWindow && strcmp(GImGui->NavWindow->Name, "a script's window") == 0);
        s.Press(SDL_CONTROLLER_BUTTON_START);
        s.Frames(2);
        CHECK(!s.Open && consoleHasFocus() && quiet());
        s.Press(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        s.Press(SDL_CONTROLLER_BUTTON_A);
        CHECK(quiet() && console.Visible());
        // Every press outside the script's window was the game's.
        CHECK(s.Reached(165, true) == 2 && s.Reached(153, true) == 2 && s.Reached(154, true) == 1);
    }
}

void RunObjcTests()
{
    @autoreleasepool {
        the_runtime_states_classes_as_the_checks_expect_run();
        events_are_described_for_the_input_run();
        the_tap_replaces_the_methods_only_while_they_are_the_checked_code_run();
        a_frame_is_noted_at_the_flip_and_drawn_on_at_its_commit_run();
        the_font_atlas_reaches_the_renderer_through_its_mirrors_run();
        only_a_texture_the_shader_can_sample_is_registered_run();
        a_listener_is_not_let_go_of_while_it_is_being_called_run();
        a_renderer_whose_pass_has_not_run_is_not_destroyed_run();
        the_frame_hooks_go_in_only_on_the_game_that_was_checked_run();
        a_layout_gives_its_characters_as_sdl_takes_them_run();
        the_text_client_takes_the_keys_while_a_text_field_has_the_keyboard_run();
        the_overlay_has_the_cursor_while_it_has_the_pointer_and_gives_the_views_back_run();
        a_controller_is_read_before_the_game_and_a_kept_button_is_not_queued_run();
        a_controller_has_the_instance_id_sdl_would_give_it_while_the_game_has_it_run();
        an_iohid_controllers_axes_are_read_where_the_game_reads_them_run();
        the_controller_hooks_go_in_only_on_the_game_that_was_checked_run();
        a_scripts_window_is_used_with_a_gamecontroller_pad_alone_run();
        a_scripts_window_is_used_with_an_iohid_pad_alone_run();
        a_window_without_a_script_is_reached_with_imguis_own_gamepad_controls_run();
        a_controller_does_not_take_the_keyboard_for_the_console_window_run();
    }
}
