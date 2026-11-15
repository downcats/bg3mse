#pragma once

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <Engine/EngineResolver.h>
#include <Overlay/ObjcClasses.h>

// Everything in the game the overlay depends on, checked before any of it is
// used: the functions, the vtable slot and the offsets come from the engine
// table's overlay area (Engine/Entries/Overlay.cpp), each found by name and
// compared with the game's code; the two Objective-C methods the overlay
// replaces are compared with the class data.
//
// The overlay has six parts that fail separately. A part with a check that
// did not pass is off: nothing of it is installed and nothing of the game it
// would have used is touched. The frame and the events are one overlay (a
// window that is drawn and cannot be clicked, or the reverse, is no overlay),
// so the drawing needs both; drawing in high dynamic range, controllers,
// injected keys and textures each stand alone.
//
// The same check runs in the game and on a game binary mapped from disk.

namespace bg3se::mac::overlay {

enum class Part : uint8_t
{
    Frame,          // the drawable and command buffer of a frame, and the moment to draw
    Events,         // the events the game is sent, and its view
    Injection,      // queueing a key for the game
    Textures,       // loading a texture of the game by its GUID
    HighDynamicRange,   // drawing on the game's HDR output, as bright as its own UI
    Controllers,        // controller events for Lua, and the overlay's gamepad navigation
    InjectedModifiers,  // the modifiers an injected key names, queued as their keys (with Injection)
    Pointer,            // the pointer's shape and position as the overlay asks, and its leaving the view (with Events)
    HidControllerAxes,  // the axes of the controllers the game reads through IOHID (with Controllers)
    Count
};

char const* Describe(Part part);

struct DependencyCheck
{
    Part Of;
    std::string What;
    bool Passed;
    std::string Detail;
};

struct Dependencies
{
    // Addresses are of this process when the check ran on the running game,
    // and link-time addresses (for reports) when it ran on a file.
    bool Live{ false };

    // --- the frame
    uint64_t FlipBuffersSlot{ 0 };          // the slot of the vtable for rf::metal::MetalRenderFrame
    uint64_t FlipBuffers{ 0 };              // the function the slot holds
    uint64_t CommitCommandBuffer{ 0 };
    uint64_t CommitCommandBufferBytes{ 0 };
    uint32_t FrameDrawable{ 0 };            // rf::metal::MetalRenderFrame: its id<CAMetalDrawable>
    uint32_t FrameRenderer{ 0 };            // ... and its rf::metal::MetalRenderer*
    uint32_t RendererCommandBuffer{ 0 };    // rf::metal::MetalRenderer: its id<MTLCommandBuffer>

    // --- events
    uint64_t SendEvent{ 0 };                // the code of -[BG3Application sendEvent:]
    uint64_t ViewInit{ 0 };                 // the code of -[LSMTLView initWithFrame:device:display:hdr:vsync:]

    // --- injection
    uint64_t InjectInput{ 0 };
    uint64_t InputManager{ 0 };             // the variable ls::InputManager::m_ptr
    uint64_t KeyboardKeys{ 0 };             // the table of input ids, 16 bits each
    uint32_t KeyCodes{ 0 };                 // how many key codes the table has

    // --- textures
    uint64_t AddTexture{ 0 };
    uint64_t InstantiateTexture{ 0 };
    uint64_t PutTexture{ 0 };
    uint64_t ResourceManager{ 0 };          // the variable ls::ResourceManager::m_ptr
    uint32_t TextureManager{ 0 };           // ls::ResourceManager: its ls::TextureManager*
    uint32_t ResourceBanks{ 0 };            // ... and its two resource banks
    uint32_t ResourceSourceFile{ 0 };       // a texture resource: its source file,
    uint32_t ResourceGuid{ 0 };             // ... GUID,
    uint32_t ResourceState{ 0 };            // ... load state byte,
    uint32_t ResourceStreaming{ 0 };        // ... Streaming,
    uint32_t ResourceSRGB{ 0 };             // ... SRGB
    uint32_t ResourceType{ 0 };             // ... and type
    uint32_t MetalTexture{ 0 };             // rf::Texture: its id<MTLTexture>

    // --- controllers
    uint64_t UpdateControls{ 0 };           // cocoa::MFIInputDevice::UpdateControls
    uint64_t UpdateControlsBytes{ 0 };
    uint64_t InjectInputBytes{ 0 };
    uint32_t DeviceEnabled{ 0 };            // cocoa::MFIInputDevice: acquired (a byte),
    uint32_t DeviceController{ 0 };         // ... its GCController*,
    uint32_t DevicePlayer{ 0 };             // ... its player index (a byte)
    uint64_t ControllerButtons{ 0 };        // the game's table of controller buttons, 16 bits each
    uint32_t ControllerButtonCount{ 0 };
    uint64_t RegisterDevice{ 0 };           // ls::InputManager::RegisterDevice: a controller arrives
    uint64_t RegisterDeviceBytes{ 0 };
    uint64_t UnregisterDevice{ 0 };         // ls::InputManager::UnregisterDevice: a controller goes
    uint64_t UnregisterDeviceBytes{ 0 };
    uint32_t RegisteredDeviceId{ 0 };       // ls::InputDevice: its device id (16 bits)
    uint64_t OpenController{ 0 };           // cocoa::CocoaWindowManager::OpenController(GCController*)
    uint64_t OpenControllerBytes{ 0 };

    // --- axes of IOHID controllers (DeviceEnabled and DevicePlayer are the same places in an HIDInputDevice)
    uint64_t HidUpdateControls{ 0 };        // cocoa::HIDInputDevice::UpdateControls
    uint64_t HidUpdateControlsBytes{ 0 };
    uint32_t HidAxes{ 0 };                  // cocoa::HIDInputDevice: its GenData* array,
    uint32_t HidAxisCount{ 0 };             // ... how many it holds (32 bits)
    uint32_t HidAxisValue{ 0 };             // GenData: the axis's calibrated value, a double
    uint64_t ControllerAxes{ 0 };           // the game's table of half axes, 16 bits each
    uint32_t ControllerAxisCount{ 0 };
    uint64_t TriggerCalibrationMin{ 0 };    // the NSNumber Acquire gives a trigger as its CalibrationMin

    // --- high dynamic range
    uint64_t GraphicSettings{ 0 };          // the variable holding the game's rf::GraphicSettings*
    uint32_t PaperWhite{ 0 };               // rf::GraphicSettings: HDRPaperWhite, a float, in nits
    float UiWhite{ NAN };                   // the white of the game's UI on HDR output, in nits, read from the code

    std::vector<DependencyCheck> Checks;

    // Every check of the part passed.
    bool Usable(Part part) const;
    // The overlay can draw and be used: the frame and the events.
    bool Drawable() const { return Usable(Part::Frame) && Usable(Part::Events); }
    // The part's failed checks, in one line; empty when it is usable.
    std::string Problem(Part part) const;
};

// The entries the overlay depends on: the overlay area, and the entries of
// other areas it also uses.
std::span<engine::Area const> DependencyTable();

// `resolution` must be of DependencyTable() or of a table that holds it.
Dependencies CheckDependencies(engine::Resolution const& resolution, ObjcClasses const& classes);

// The check of one method the overlay replaces: the class has a method of its
// own for the selector, with the type encoding the replacement is written
// for, implemented by the code at `code` (the code the engine table checked).
DependencyCheck CheckReplacedMethod(ObjcClasses const& classes, Part part, char const* className, char const* selector, char const* types,
    uint64_t code);

// The running game's, checked once, on the resolution SetGameResolution gave.
// The overlay does not resolve the table itself: a function that carries a
// hook no longer has its recorded shape, so the table is resolved once for
// everyone, before any hook is on the game (Mac/Binding/Startup.h), and the
// overlay's own hooks are among those that come after. Without a resolution
// every part is off. Logs one line for each part that is off.
Dependencies const& GameDependencies();

// The resolution of the engine table on the running game (of the whole table,
// or of DependencyTable()), which must outlive the process's use of the
// overlay. To be called before the first GameDependencies(); what it is given
// later changes nothing.
void SetGameResolution(engine::Resolution const* resolution);

// The type encodings the replacements of the two methods are written for.
inline constexpr char kBG3Application[] = "BG3Application";
inline constexpr char kSendEventSelector[] = "sendEvent:";
inline constexpr char kSendEventTypes[] = "v24@0:8@16";
inline constexpr char kLSMTLView[] = "LSMTLView";
inline constexpr char kViewInitSelector[] = "initWithFrame:device:display:hdr:vsync:";
inline constexpr char kViewInitTypes[] = "@68@0:8{CGRect={CGPoint=dd}{CGSize=dd}}16@48i56B60B64";
inline constexpr char kViewLayerVariable[] = "metalLayer";
inline constexpr char kViewLayerType[] = "@\"CAMetalLayer\"";
// The view's own cursor, which -[LSMTLView setCursor:] keeps and cursorUpdate: sets again,
// and whether the game holds the pointer (-[LSMTLView captureCursor:]).
inline constexpr char kViewCursorVariable[] = "CurrentCursor";
inline constexpr char kViewCursorType[] = "@\"NSCursor\"";
inline constexpr char kViewCaptureVariable[] = "captureCursor";
inline constexpr char kViewCaptureType[] = "B";

}
