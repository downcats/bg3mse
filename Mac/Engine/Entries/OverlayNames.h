#pragma once

// The names of the overlay area's entries (Engine/Entries/Overlay.cpp) and of
// the operands the overlay reads back from them, for the code that uses the
// resolved values (Mac/Overlay/Dependencies.cpp): an operand is asked for by
// what it is, never by its position in a list.

namespace bg3se::mac::engine::overlay {

// The area.
inline constexpr char kArea[] = "overlay";

// Entries.
inline constexpr char kRenderFrameVtable[] = "rf::metal::MetalRenderFrame";
inline constexpr char kFlipBuffers[] = "rf::metal::MetalRenderFrame::FlipBuffers";
inline constexpr char kCommitCommandBuffer[] = "rf::metal::MetalRenderer::CommitCommandBuffer";
inline constexpr char kViewInit[] = "-[LSMTLView initWithFrame:device:display:hdr:vsync:]";
inline constexpr char kSendEvent[] = "-[BG3Application sendEvent:]";
inline constexpr char kInjectInput[] = "ls::InputManager::InjectInput";
inline constexpr char kKeyboardKeys[] = "cocoa::CocoaInputTranslator::s_KeyboardKeys";
inline constexpr char kModifierKeys[] = "-[LSMTLView flagsChanged:]";
inline constexpr char kAddTexture[] = "ls::ResourceManager::AddTexture";
inline constexpr char kInstantiateTexture[] = "ls::ResourceManager::InstantiateTexture";
inline constexpr char kGraphicSettings[] = "GraphicSettings";
inline constexpr char kUpdateControls[] = "cocoa::MFIInputDevice::UpdateControls";
inline constexpr char kControllerButtons[] = "cocoa::CocoaInputTranslator::s_ControllerButtons";
inline constexpr char kRegisterDevice[] = "ls::InputManager::RegisterDevice";
inline constexpr char kUnregisterDevice[] = "ls::InputManager::UnregisterDevice";
inline constexpr char kOpenController[] = "cocoa::CocoaWindowManager::OpenController(GCController*)";
inline constexpr char kHidUpdateControls[] = "cocoa::HIDInputDevice::UpdateControls";
inline constexpr char kControllerAxes[] = "cocoa::CocoaInputTranslator::s_ControllerAxes";
inline constexpr char kTriggerCalibrationMin[] = "cocoa::HIDInputDevice::Acquire: the CalibrationMin of a trigger";
// Entries of other areas the overlay also needs.
inline constexpr char kInputManager[] = "ls__gInputManager";
inline constexpr char kResourceManager[] = "ls__gGlobalResourceManager";
inline constexpr char kPutTexture[] = "ls__TextureManager__UnloadTexture";
// The function whose inlined texture loading the overlay repeats: its recorded
// shape is what makes every instruction of that loading the one that was read.
inline constexpr char kLoadingHost[] = "ls__AppliedMaterial__TryOverrideTexture2DParameter";

// The vtable slot the overlay replaces.
inline constexpr char kFlipBuffersSlot[] = "FlipBuffers";

// Operands of kFlipBuffers.
inline constexpr char kFrameDrawable[] = "the frame's drawable";
inline constexpr char kFrameRenderer[] = "the frame's renderer";
inline constexpr char kFrameRendererPaced[] = "the frame's renderer (paced)";
inline constexpr char kRendererBuffer[] = "the renderer's command buffer";
inline constexpr char kRendererBufferPaced[] = "the renderer's command buffer (paced)";
// Operands of kCommitCommandBuffer.
inline constexpr char kCommittedBuffer[] = "the buffer it commits";
// Operands of kInjectInput.
inline constexpr char kChangePressed[] = "InputRawChange: pressed";
inline constexpr char kLastKeyCode[] = "the last key code";
// Operands of kModifierKeys.
inline constexpr char kModifierPressed[] = "InputRawChange: pressed (modifiers)";
// Operands of kAddTexture.
inline constexpr char kResourceType[] = "TextureResource: Type";
inline constexpr char kResourceSourceFile[] = "Resource: SourceFile";
inline constexpr char kResourceGuidAdded[] = "Resource: Guid (added)";
inline constexpr char kTextureManagerAdded[] = "ResourceManager: TextureManager (added)";
// Operands of kInstantiateTexture.
inline constexpr char kResourceState[] = "Resource: load state";
inline constexpr char kResourceGuid[] = "Resource: Guid";
inline constexpr char kResourceStreaming[] = "TextureResource: Streaming";
inline constexpr char kResourceSRGB[] = "TextureResource: SRGB";
inline constexpr char kTextureManager[] = "ResourceManager: TextureManager";
inline constexpr char kResourceBanks[] = "ResourceManager: ResourceBanks";
inline constexpr char kMetalTexture[] = "rf::Texture: the MTLTexture";
inline constexpr char kMetalTextureCreated[] = "rf::Texture: the MTLTexture (created)";
// Operands of kUpdateControls.
inline constexpr char kDeviceEnabled[] = "MFIInputDevice: acquired";
inline constexpr char kDeviceController[] = "MFIInputDevice: its GCController";
inline constexpr char kDevicePlayer[] = "MFIInputDevice: its player index";
inline constexpr char kControllerDeviceBase[] = "the device id of the first player's controller";
// Operands of kRegisterDevice.
inline constexpr char kRegisteredDeviceId[] = "InputDevice: DeviceId, as OpenController(GCController*) registers it";
inline constexpr char kRegisteredDeviceIdHid[] = "InputDevice: DeviceId, as OpenController(__IOHIDDevice*) registers it";
inline constexpr char kRegisteredPlayer[] = "the player index OpenController(GCController*) gives the device it registered";
inline constexpr char kRegisteredPlayerHid[] = "the player index OpenController(__IOHIDDevice*) gives the device it registered";
inline constexpr char kRegisteredBase[] = "the device id OpenController(GCController*) registers: the player index plus";
inline constexpr char kRegisteredBaseHid[] = "the device id OpenController(__IOHIDDevice*) registers: the player index plus";
// Operands of kUnregisterDevice.
inline constexpr char kUnregisteredBase[] = "the device id CloseController unregisters: the player index plus";
inline constexpr char kUnregisteredBaseAll[] = "the device id CloseAllControllers unregisters: the player index plus";
// Operands of kHidUpdateControls.
inline constexpr char kHidAcquired[] = "HIDInputDevice: acquired";
inline constexpr char kHidPlayer[] = "HIDInputDevice: its player index";
inline constexpr char kHidDeviceBase[] = "the device id of a HIDInputDevice's changes: the player index plus (in the high half)";
inline constexpr char kHidAxes[] = "HIDInputDevice: its axes (GenData*)";
inline constexpr char kHidAxisCount[] = "HIDInputDevice: how many axes it has";
inline constexpr char kHidAxisValue[] = "GenData: the axis's value";
inline constexpr char kHidCallbackAxes[] = "HIDInputDevice: its axes (GenData*), as its value callback writes them";
inline constexpr char kHidCallbackValue[] = "GenData: the axis's value, as the value callback writes it";
inline constexpr char kHidCallbackScale[] = "the value callback's scale type for an axis";

// Operands of kGraphicSettings.
inline constexpr char kGraphicSettingsLoaded[] = "the settings the UI is combined with";
inline constexpr char kPaperWhite[] = "rf::GraphicSettings: HDRPaperWhite";
inline constexpr char kUiWhite[] = "the white of the game's UI in HDR, in nits (UIPaperWhite)";

// Operand of kPutTexture (Engine/Entries/World.cpp).
inline constexpr char kPutTextureManager[] = "the manager's place";

}
