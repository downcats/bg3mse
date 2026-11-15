#include <Engine/Entries/Areas.h>
#include <Engine/Entries/OverlayNames.h>
#include <Layout/GameMembers.h>

// The overlay: what the Mac rendering backend and input of Norbyte's ImGui
// overlay (Mac/Overlay) use in the game, in place of the Direct3D, Vulkan and
// SDL functions Norbyte hooks on Windows. The Mac game has none of those: it
// renders with Metal and takes its input from Cocoa.
//
// The frame. rf::metal::MetalRenderFrame::FlipBuffers(bool) is the one place
// that schedules the present of a frame, on the main renderer's command
// buffer; rf::metal::MetalRenderer::CommitCommandBuffer(bool) commits that
// buffer, and commits it without ending an encoder, so at its entry the
// buffer has none open. The overlay learns a frame's drawable and command
// buffer in the first and draws at the entry of the second.
//
// Input. Every NSEvent the game handles passes through -[BG3Application
// sendEvent:], and every input the game acts on is queued through
// ls::InputManager::InjectInput(ls::InputRawChange const&, bool).
//
// Objective-C methods have symbols like any function, so their code is found
// and compared here; that a class's method is that code is checked against
// the class data (Mac/Overlay/ObjcClasses.h).

namespace bg3se::mac::engine {

using namespace overlay;

namespace {

constexpr char kFlipBuffersSymbol[] = "__ZN2rf5metal16MetalRenderFrame11FlipBuffersEb";
constexpr char kGetDrawableSymbol[] = "__ZN2rf5metal16MetalRenderFrame11GetDrawableEv";
constexpr char kCommitSymbol[] = "__ZN2rf5metal13MetalRenderer19CommitCommandBufferEb";
constexpr char kCreateTextureSymbol[] =
    "__ZN2rf5metal9GPUDevice13CreateTextureERKNS_14GraphicsBufferENS_17TextureDescriptorERKN2ls4SpanIKjEERKNS_14TextureBarrierEPNS_13TextureMemoryE";
constexpr char kKeyDownSymbol[] = "-[LSMTLView keyDown:]";
constexpr char kFlagsChangedSymbol[] = "-[LSMTLView flagsChanged:]";
constexpr char kInjectInputSymbol[] = "__ZN2ls12InputManager11InjectInputERKNS_14InputRawChangeEb";
constexpr char kKeyboardKeysSymbol[] = "__ZN5cocoa20CocoaInputTranslator14s_KeyboardKeysE";
constexpr char kAddTextureSymbol[] = "__ZN2ls15ResourceManager10AddTextureERKNS_4PathERKNS_11FixedStringE";
constexpr char kInstantiateTextureSymbol[] = "__ZN2ls15ResourceManager18InstantiateTextureERKNS_11FixedStringEbbb";
constexpr char kInstantiateInternalPrefix[] = "__ZN2ls14TextureManager26InstantiateTextureInternalERKNS_11FixedStringEbbbNS_9SharedPtr";
constexpr char kOverrideTextureSymbol[] = "__ZN2ls15AppliedMaterial29TryOverrideTexture2DParameterERKNS_11FixedStringES3_b";
constexpr char kResourceManagerSymbol[] = "__ZN2ls15ResourceManager5m_ptrE";
constexpr char kGraphicSettingsSymbol[] = "_GraphicSettings";
constexpr char kUpdateControlsSymbol[] = "__ZN5cocoa14MFIInputDevice14UpdateControlsEv";
constexpr char kOnStickChangedSymbol[] = "__ZN5cocoa14MFIInputDevice14OnStickChangedEif";
constexpr char kControllerButtonsSymbol[] = "__ZN5cocoa20CocoaInputTranslator19s_ControllerButtonsE";
constexpr char kControllerAxesSymbol[] = "__ZN5cocoa20CocoaInputTranslator16s_ControllerAxesE";
constexpr char kRegisterDeviceSymbol[] = "__ZN2ls12InputManager14RegisterDeviceERKNS_11InputDeviceE";
constexpr char kUnregisterDeviceSymbol[] = "__ZN2ls12InputManager16UnregisterDeviceENS_14EInputDeviceIDE";
constexpr char kOpenControllerSymbol[] = "__ZN5cocoa18CocoaWindowManager14OpenControllerEP12GCController";
constexpr char kOpenHidControllerSymbol[] = "__ZN5cocoa18CocoaWindowManager14OpenControllerEP13__IOHIDDevice";
constexpr char kCloseControllerSymbol[] = "__ZN5cocoa18CocoaWindowManager15CloseControllerEP13__IOHIDDevice";
constexpr char kCloseAllControllersSymbol[] = "__ZN5cocoa18CocoaWindowManager19CloseAllControllersEv";
constexpr char kHidUpdateControlsSymbol[] = "__ZN5cocoa14HIDInputDevice14UpdateControlsEv";
constexpr char kHidAcquireSymbol[] = "__ZN5cocoa14HIDInputDevice7AcquireEv";
constexpr char kHidValueCallbackSymbol[] = "__ZN5cocoa14HIDInputDevice25Handle_IOHIDValueCallbackEPviS1_P12__IOHIDValue";
constexpr char kCombineUiSymbol[] = "__ZN2ls20CombineUIRenderStage15ExecuteWTKernelEv";

constexpr CodeShape kKeyDownShape{ 247, 0xe0536e65b3710832ull };       // 4.1.1.7631656
constexpr CodeShape kFlagsChangedShape{ 83, 0xd147e795645a4c66ull };       // 4.1.1.7631656

// --- the frame --------------------------------------------------------------

// rf::metal::MetalRenderFrame::FlipBuffers(bool), whole:
//     cbz w1, done ; ldr x2, [x0, #drawable] ; cbz x2, done ; (frame pacing on?) ...
//     ldr x8, [x0, #renderer] ; ldr x0, [x8, #buffer] ; fdiv s0, s0, s1 ; fcvt d0, s0 ; b presentDrawable:afterMinimumDuration:
//   done:
//     ret
//     ldr x8, [x0, #renderer] ; ldr x0, [x8, #buffer] ; b presentDrawable:
constexpr MaskedWord kTakesDrawable[] = {
    { 0x34000001, Any::CondBranch },
    { 0xf9400002, Any::Imm12 },
    { 0xb4000002, Any::CondBranch },
};
constexpr MaskedWord kPresentsPaced[] = {
    { 0xf9400008, Any::Imm12 },
    { 0xf9400100, Any::Imm12 },
    { 0x1e211800 },
    { 0x1e22c000 },
    { 0x14000000, Any::Branch },
};
constexpr MaskedWord kPresents[] = {
    { 0xd65f03c0 },
    { 0xf9400008, Any::Imm12 },
    { 0xf9400100, Any::Imm12 },
    { 0x14000000, Any::Branch },
};
constexpr Operand kFlipBuffersOperands[] = {
    { .What = kFrameDrawable, .Pattern = kTakesDrawable, .At = 1, .Expected = 0x43d8 },
    { .What = kFrameRenderer, .Pattern = kPresents, .At = 1, .Expected = 0x43a8 },
    { .What = kRendererBuffer, .Pattern = kPresents, .At = 2, .Expected = 0x28 },
    { .What = "presents the drawable", .Pattern = kPresents, .At = 3, .How = Operand::Read::BranchTarget,
      .Target = Exact("_objc_msgSend$presentDrawable:") },
    { .What = kFrameRendererPaced, .Pattern = kPresentsPaced, .At = 0, .Expected = 0x43a8 },
    { .What = kRendererBufferPaced, .Pattern = kPresentsPaced, .At = 1, .Expected = 0x28 },
    { .What = "presents the drawable (paced)", .Pattern = kPresentsPaced, .At = 4, .How = Operand::Read::BranchTarget,
      .Target = Exact("_objc_msgSend$presentDrawable:afterMinimumDuration:") },
};

constexpr Signature kFlipBuffersCall = Takes({
    Pointer(Place::X0, -1, "rf::metal::MetalRenderFrame"),
    Value(Place::X1, 1, -1, "bool: present the frame"),
});

constexpr VtableMethod kRenderFrameSlots[] = {
    { kFlipBuffersSlot, Exact(kFlipBuffersSymbol), 0x58, &kFlipBuffersCall },
};

// rf::metal::MetalRenderer::CommitCommandBuffer(bool) commits the buffer it
// was given the renderer of (x20 is the entry's x0, w21 its w1):
//     ldr x0, [x20, #buffer] ; bl commit ; cbz w21, ...
constexpr MaskedWord kCommits[] = {
    { 0xf9400280, Any::Imm12 },
    { 0x94000000, Any::Branch },
    { 0x34000015, Any::CondBranch },
};
constexpr Operand kCommitOperands[] = {
    { .What = kCommittedBuffer, .Pattern = kCommits, .At = 0, .Expected = 0x28 },
    { .What = "commits it", .Pattern = kCommits, .At = 1, .How = Operand::Read::BranchTarget, .Target = Exact("_objc_msgSend$commit") },
    // The other commit is of the buffer it makes for its own blit, and the
    // one encoder it ends is that buffer's: it ends none of the renderer's.
    { .What = "commits", .How = Operand::Read::CallCount, .Expected = 2, .Target = Exact("_objc_msgSend$commit") },
    { .What = "ends encoders", .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact("_objc_msgSend$endEncoding") },
    { .What = "makes buffers", .How = Operand::Read::CallCount, .Expected = 1,
      .Target = Exact("_objc_msgSend$commandBufferWithUnretainedReferences") },
};

// --- input ------------------------------------------------------------------

// -[LSMTLView keyDown:] builds the change on its stack and queues it:
//     str w8, [sp] ; fmov.2s v0, #1.0 ; stur d0, [sp, #4] ; mov w8, #1 ; strb w8, [sp, #pressed]
//     mov x23, sp ; mov x1, sp ; mov w2, #0 ; bl ls::InputManager::InjectInput
constexpr MaskedWord kKeyDownInjects[] = {
    { 0xb90003e8 },
    { 0x0f03f600 },
    { 0xfc0043e0 },
    { 0x52800028 },
    { 0x390003e8, Any::Imm12 },
    { 0x910003f7 },
    { 0x910003e1 },
    { 0x52800002 },
    { 0x94000000, Any::Branch },
};
// ... after taking the input id from the table of key codes:
//     cmp w21, #last ; b.hi none ; adrp x8, page ; add x8, x8, #offset ; ldrh w8, [x8, w21, uxtw #1]
constexpr MaskedWord kKeyDownLooksUp[] = {
    { 0x710002bf, Any::Imm12 },
    { 0x54000008, Any::CondBranch },
    { 0x90000008, Any::Adrp },
    { 0x91000108, Any::Imm12 },
    { 0x78755908 },
};
constexpr Operand kInjectInputOperands[] = {
    { .What = "one lock", .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact("__ZN2ls15CriticalSection4LockEv") },
    { .What = kChangePressed, .In = Exact(kKeyDownSymbol), .Pattern = kKeyDownInjects, .At = 4, .Expected = kInputChangeState },
    { .What = "the key view queues through it", .In = Exact(kKeyDownSymbol), .Pattern = kKeyDownInjects, .At = 8,
      .How = Operand::Read::BranchTarget, .Target = Exact(kInjectInputSymbol) },
};
constexpr Derivation kKeyboardKeysDerivation{
    .Host = Exact(kKeyDownSymbol),
    .HostShape = kKeyDownShape,
    .Pattern = kKeyDownLooksUp,
    .What = Derivation::Take::PageAddress,
    .At = 2,
};
constexpr Operand kKeyboardKeysOperands[] = {
    { .What = kLastKeyCode, .In = Exact(kKeyDownSymbol), .Pattern = kKeyDownLooksUp, .At = 0, .How = Operand::Read::AddImmediate, .Expected = 0xb3 },
};

// -[LSMTLView flagsChanged:] queues a modifier key as any other key: for each
// of the eight modifier key codes (0x36 .. 0x3e but caps lock), the input id
// from the same table, pressed while the event's flag of its kind is set. The
// table is taken in the case of the shift keys:
//     adrp x9, page ; add x9, x9, #offset ; ldrh w9, [x9, w19, uxtw #1] ; ands x10, x0, #NSEventModifierFlagShift
constexpr MaskedWord kFlagsChangedLooksUp[] = {
    { 0x90000009, Any::Adrp },
    { 0x91000129, Any::Imm12 },
    { 0x78735929 },
    { 0xf26f000a },
};
// ... and every case queues the change it built:
//     stp s0, s0, [sp, #4] ; strb w9, [sp, #pressed] ; mov x1, sp ; mov x0, x8 ; mov w2, #0 ; bl ls::InputManager::InjectInput
constexpr MaskedWord kFlagsChangedInjects[] = {
    { 0x2d0083e0 },
    { 0x390003e9, Any::Imm12 },
    { 0x910003e1 },
    { 0xaa0803e0 },
    { 0x52800002 },
    { 0x94000000, Any::Branch },
};
constexpr Derivation kModifierKeysDerivation{
    .Host = Exact(kFlagsChangedSymbol),
    .HostShape = kFlagsChangedShape,
    .Pattern = kFlagsChangedLooksUp,
    .What = Derivation::Take::PageAddress,
    .At = 0,
};
constexpr Operand kModifierKeysOperands[] = {
    { .What = "queues one change", .In = Exact(kFlagsChangedSymbol), .How = Operand::Read::CallCount, .Expected = 1,
      .Target = Exact(kInjectInputSymbol) },
    { .What = kModifierPressed, .In = Exact(kFlagsChangedSymbol), .Pattern = kFlagsChangedInjects, .At = 1, .Expected = kInputChangeState },
    { .What = "the modifier keys are queued through it", .In = Exact(kFlagsChangedSymbol), .Pattern = kFlagsChangedInjects, .At = 5,
      .How = Operand::Read::BranchTarget, .Target = Exact(kInjectInputSymbol) },
};

// --- controllers ------------------------------------------------------------

// cocoa::MFIInputDevice::UpdateControls() reads, for an acquired device, the
// state of its GCController's extended gamepad, and queues a change for each
// element that changed since its last read:
//     ldrb w8, [x0, #acquired] ; cbz w8, done ; mov x19, x0 ; ldr x0, [x0, #controller] ; bl extendedGamepad
// The input of a change is the element's InputRawType from
// cocoa::CocoaInputTranslator::s_ControllerButtons in the low half, and the
// device in the high: the player index plus the first controller's id, as
// OnStickChanged(int, float) forms it:  ldrb w9, [x19, #player] ; add w24, w9, #3
constexpr MaskedWord kReadsGamepad[] = {
    { 0x39400008, Any::Imm12 },
    { 0x34000008, Any::CondBranch },
    { 0xaa0003f3 },
    { 0xf9400000, Any::Imm12 },
    { 0x94000000, Any::Branch },
};
constexpr MaskedWord kFormsDevice[] = {
    { 0x39400269, Any::Imm12 },
    { 0x11000d38, 0xffc003ff },
};
constexpr Operand kUpdateControlsOperands[] = {
    { .What = kDeviceEnabled, .Pattern = kReadsGamepad, .At = 0, .Expected = 0x170 },
    { .What = kDeviceController, .Pattern = kReadsGamepad, .At = 3, .Expected = 0x178 },
    { .What = "reads its extended gamepad", .Pattern = kReadsGamepad, .At = 4, .How = Operand::Read::BranchTarget,
      .Target = Exact("_objc_msgSend$extendedGamepad") },
    // Fourteen buttons; the four axes of the sticks and the two triggers go
    // through OnStickChanged, the last of them by a tail call.
    { .What = "queues its buttons", .How = Operand::Read::CallCount, .Expected = 14, .Target = Exact(kInjectInputSymbol) },
    { .What = "hands on its axes", .How = Operand::Read::CallCount, .Expected = 5, .Target = Exact(kOnStickChangedSymbol) },
    { .What = kDevicePlayer, .In = Exact(kOnStickChangedSymbol), .Pattern = kFormsDevice, .At = 0, .Expected = 0x171 },
    { .What = kControllerDeviceBase, .In = Exact(kOnStickChangedSymbol), .Pattern = kFormsDevice, .At = 1, .How = Operand::Read::AddImmediate,
      .Expected = 3 },
};

// A controller comes and goes. cocoa::CocoaWindowManager::OpenController, for
// a GCController (-[AppDelegate gameControllerDidConnect:] and
// openConnectedMFiDevices) and for an IOHID device (the HID manager's
// matching callback), makes the device, acquires it, puts it in the first
// free slot and registers it with ls::InputManager::RegisterDevice under the
// device id slot + 3, then gives it the slot as its player index; no other
// function of the game's registers a controller. CloseController (a device
// removed, a GCController disconnected, one opened again) and
// CloseAllControllers unregister it with ls::InputManager::UnregisterDevice
// under the same id, and free it. So a registration of device 3 + n is a new
// controller at player index n, until the id is unregistered.
//   OpenController(GCController*), w23 the slot:
//     add w22, w23, #3 ; ... ; strh w22, [sp, #DeviceId] ; adrp x8, ... ; ldr q0, [x8, ...] ; stur q0, [sp, #0x14]
//     strb wzr, [sp, #0x24] ; mov x1, sp ; mov x0, x21 ; bl RegisterDevice ; strb w23, [x19, #player]
//   OpenController(__IOHIDDevice*), w24 the slot: the same with w20 the id, x19 the manager, x21 the device.
constexpr MaskedWord kRegistersSlot[] = {
    { 0x11000ef6, Any::Imm12 },
};
constexpr MaskedWord kRegistersDevice[] = {
    { 0x790003f6, Any::Imm12 },
    { 0x90000008, Any::Adrp },
    { 0x3dc00100, Any::Imm12 },
    { 0x3c8143e0 },
    { 0x390093ff },
    { 0x910003e1 },
    { 0xaa1503e0 },
    { 0x94000000, Any::Branch },
    { 0x39000277, Any::Imm12 },
};
constexpr MaskedWord kRegistersHidSlot[] = {
    { 0x11000f14, Any::Imm12 },
};
constexpr MaskedWord kRegistersHidDevice[] = {
    { 0x790003f4, Any::Imm12 },
    { 0x90000008, Any::Adrp },
    { 0x3dc00100, Any::Imm12 },
    { 0x3c8143e0 },
    { 0x390093ff },
    { 0x910003e1 },
    { 0xaa1303e0 },
    { 0x94000000, Any::Branch },
    { 0x390002b8, Any::Imm12 },
};
constexpr Operand kRegisterDeviceOperands[] = {
    { .What = kRegisteredBase, .In = Exact(kOpenControllerSymbol), .Pattern = kRegistersSlot, .At = 0, .How = Operand::Read::AddImmediate,
      .Expected = 3 },
    { .What = kRegisteredDeviceId, .In = Exact(kOpenControllerSymbol), .Pattern = kRegistersDevice, .At = 0, .Expected = 0x10 },
    { .What = "OpenController(GCController*) registers through it", .In = Exact(kOpenControllerSymbol), .Pattern = kRegistersDevice,
      .At = 7, .How = Operand::Read::BranchTarget, .Target = Exact(kRegisterDeviceSymbol) },
    { .What = kRegisteredPlayer, .In = Exact(kOpenControllerSymbol), .Pattern = kRegistersDevice, .At = 8, .Expected = 0x171 },
    { .What = "OpenController(GCController*) registers once", .In = Exact(kOpenControllerSymbol), .How = Operand::Read::CallCount,
      .Expected = 1, .Target = Exact(kRegisterDeviceSymbol) },
    { .What = kRegisteredBaseHid, .In = Exact(kOpenHidControllerSymbol), .Pattern = kRegistersHidSlot, .At = 0,
      .How = Operand::Read::AddImmediate, .Expected = 3 },
    { .What = kRegisteredDeviceIdHid, .In = Exact(kOpenHidControllerSymbol), .Pattern = kRegistersHidDevice, .At = 0, .Expected = 0x10 },
    { .What = "OpenController(__IOHIDDevice*) registers through it", .In = Exact(kOpenHidControllerSymbol), .Pattern = kRegistersHidDevice,
      .At = 7, .How = Operand::Read::BranchTarget, .Target = Exact(kRegisterDeviceSymbol) },
    { .What = kRegisteredPlayerHid, .In = Exact(kOpenHidControllerSymbol), .Pattern = kRegistersHidDevice, .At = 8, .Expected = 0x171 },
    { .What = "OpenController(__IOHIDDevice*) registers once", .In = Exact(kOpenHidControllerSymbol), .How = Operand::Read::CallCount,
      .Expected = 1, .Target = Exact(kRegisterDeviceSymbol) },
};
//   CloseController(__IOHIDDevice*), x22 the slot the device's key maps to:
//     ldr x0, [x8, #m_ptr] ; add w8, w22, #3 ; sxth w1, w8 ; bl UnregisterDevice
//   CloseAllControllers(), x23 each slot in turn:
//     add w8, w23, #3 ; sxth w1, w8 ; mov x0, x20 ; bl UnregisterDevice
constexpr MaskedWord kUnregistersDevice[] = {
    { 0xf9400100, Any::Imm12 },
    { 0x11000ec8, Any::Imm12 },
    { 0x13003d01 },
    { 0x94000000, Any::Branch },
};
constexpr MaskedWord kUnregistersEachDevice[] = {
    { 0x11000ee8, Any::Imm12 },
    { 0x13003d01 },
    { 0xaa1403e0 },
    { 0x94000000, Any::Branch },
};
//   OpenController(GCController*), x0 the window manager, x1 the controller:
//     ldrb w8, [x0, #0xa4] ; cbnz w8, done ; mov x20, x1 ; mov x21, x0 ; (the controller in the manager's map?)
//     ... mov x0, x21 ; mov x1, x20 ; bl CloseController
// so the one it unregisters while it runs is the controller it was given.
constexpr MaskedWord kReopensController[] = {
    { 0x54000001, Any::CondBranch },
    { 0xaa1503e0 },
    { 0xaa1403e1 },
    { 0x94000000, Any::Branch },
};
constexpr Operand kOpenControllerOperands[] = {
    { .What = "closes the controller it was given, when it has it open", .Pattern = kReopensController, .At = 3,
      .How = Operand::Read::BranchTarget, .Target = Exact(kCloseControllerSymbol) },
    { .What = "closes nothing else", .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kCloseControllerSymbol) },
    { .What = "closes none at all", .How = Operand::Read::CallCount, .Expected = 0, .Target = Exact(kCloseAllControllersSymbol) },
    { .What = "unregisters through nothing else", .How = Operand::Read::CallCount, .Expected = 0, .Target = Exact(kUnregisterDeviceSymbol) },
};
constexpr Operand kUnregisterDeviceOperands[] = {
    { .What = kUnregisteredBase, .In = Exact(kCloseControllerSymbol), .Pattern = kUnregistersDevice, .At = 1, .How = Operand::Read::AddImmediate,
      .Expected = 3 },
    { .What = "CloseController unregisters through it", .In = Exact(kCloseControllerSymbol), .Pattern = kUnregistersDevice, .At = 3,
      .How = Operand::Read::BranchTarget, .Target = Exact(kUnregisterDeviceSymbol) },
    { .What = "CloseController unregisters once", .In = Exact(kCloseControllerSymbol), .How = Operand::Read::CallCount, .Expected = 1,
      .Target = Exact(kUnregisterDeviceSymbol) },
    { .What = kUnregisteredBaseAll, .In = Exact(kCloseAllControllersSymbol), .Pattern = kUnregistersEachDevice, .At = 0,
      .How = Operand::Read::AddImmediate, .Expected = 3 },
    { .What = "CloseAllControllers unregisters through it", .In = Exact(kCloseAllControllersSymbol), .Pattern = kUnregistersEachDevice,
      .At = 3, .How = Operand::Read::BranchTarget, .Target = Exact(kUnregisterDeviceSymbol) },
    { .What = "CloseAllControllers unregisters once", .In = Exact(kCloseAllControllersSymbol), .How = Operand::Read::CallCount,
      .Expected = 1, .Target = Exact(kUnregisterDeviceSymbol) },
};

// cocoa::HIDInputDevice::UpdateControls() reads, for an acquired device with
// new values, the buttons its value callback marked and queues them as the
// MFi device does (InputRawType from s_ControllerButtons in the low half,
// player index + 3 in the high):
//     ldrb w8, [x0, #acquired] ; cbz w8, done ; mov x19, x0 ; ldrb w8, [x0, #fresh] ; cbz w8, done
//     ldrh w8, [x23, x21, lsl #1] ; ldrb w9, [x19, #player] ; bfi w8, w9, #16, #8 ; add w8, w8, #0x30, lsl #12
// then its axes: up to six GenData*, the first four always and the next two
// when it has more than four, in the order s_ControllerAxes gives two half
// axes each (left x, left y, right x, right y, left trigger, right trigger),
// stopping at the first it does not have:
//     ldr x8, [x19, #axes] ; ldr x21, [x8] ; str x21, [sp, #0x28] ; ldur q0, [x8, #8] ; stur q0, [sp, #0x30]
//     ldr x9, [x8, #0x18] ; stp x9, xzr, [sp, #0x40] ; stp xzr, xzr, [sp, #0x50] ; ldr w9, [x19, #count] ; cmp w9, #4
//     b.ls ... ; ldp x9, x8, [x8, #0x20]
// An axis's value is a double in its GenData, which the callback has written:
//     ldrb w8, [x21, #4] ; cbz w8, next ; ldr d1, [x21, #value]
// The game negates the value of a y axis for itself and takes its own dead
// zone; the GenData keeps the value as the callback wrote it.
constexpr MaskedWord kHidReadsDevice[] = {
    { 0x39400008, Any::Imm12 },
    { 0x34000008, Any::CondBranch },
    { 0xaa0003f3 },
    { 0x39400008, Any::Imm12 },
    { 0x34000008, Any::CondBranch },
};
constexpr MaskedWord kHidFormsButton[] = {
    { 0x78757ae8 },
    { 0x39400269, Any::Imm12 },
    { 0x33101d28 },
    { 0x1140c108, Any::Imm12 },
};
constexpr MaskedWord kHidTakesAxes[] = {
    { 0xf9400268, Any::Imm12 },
    { 0xf9400115 },
    { 0xf90017f5 },
    { 0x3cc08100 },
    { 0x3c8303e0 },
    { 0xf9400d09 },
    { 0xa9047fe9 },
    { 0xa9057fff },
    { 0xb9400269, Any::Imm12 },
    { 0x7100113f },
    { 0x54000009, Any::CondBranch },
    { 0xa9422109 },
};
constexpr MaskedWord kHidReadsAxis[] = {
    { 0x394012a8 },
    { 0x34000008, Any::CondBranch },
    { 0xfd400ea1, Any::Imm12 },
};
// cocoa::HIDInputDevice::Handle_IOHIDValueCallback(void*, int, void*, __IOHIDValue*)
// writes an axis's value calibrated (kIOHIDValueScaleTypeCalibrated, 0) into
// the GenData its element names (x19 the device, w8 the element's XIKValue):
//     neg w8, w8 ; ldr x9, [x19, #axes] ; ldr x21, [x9, w8, uxtw #3] ; mov x0, x20 ; mov w1, #0
//     bl IOHIDValueGetScaledValue ; str d0, [x21, #value]
constexpr MaskedWord kHidStoresAxis[] = {
    { 0x4b0803e8 },
    { 0xf9400269, Any::Imm12 },
    { 0xf8685935 },
    { 0xaa1403e0 },
    { 0x52800001, Any::Imm16 },
    { 0x94000000, Any::Branch },
    { 0xfd000ea0, Any::Imm12 },
};
constexpr Operand kHidUpdateControlsOperands[] = {
    { .What = kHidAcquired, .Pattern = kHidReadsDevice, .At = 0, .Expected = 0x170 },
    { .What = kHidPlayer, .Pattern = kHidFormsButton, .At = 1, .Expected = 0x171 },
    { .What = kHidDeviceBase, .Pattern = kHidFormsButton, .At = 3, .How = Operand::Read::AddImmediate, .Expected = 0x30000 },
    { .What = kHidAxes, .Pattern = kHidTakesAxes, .At = 0, .Expected = 0x10 },
    { .What = kHidAxisCount, .Pattern = kHidTakesAxes, .At = 8, .Expected = 0x20 },
    { .What = kHidAxisValue, .Pattern = kHidReadsAxis, .At = 2, .Expected = 0x18 },
    // The buttons, and the half axes: one change, then up to three releases.
    { .What = "queues its changes", .How = Operand::Read::CallCount, .Expected = 5, .Target = Exact(kInjectInputSymbol) },
    { .What = kHidCallbackAxes, .In = Exact(kHidValueCallbackSymbol), .Pattern = kHidStoresAxis, .At = 1, .Expected = 0x10 },
    { .What = kHidCallbackScale, .In = Exact(kHidValueCallbackSymbol), .Pattern = kHidStoresAxis, .At = 4, .How = Operand::Read::MoveValue,
      .Expected = 0 },
    { .What = kHidCallbackValue, .In = Exact(kHidValueCallbackSymbol), .Pattern = kHidStoresAxis, .At = 6, .Expected = 0x18 },
    { .What = "the value callback scales an axis and a button", .In = Exact(kHidValueCallbackSymbol), .How = Operand::Read::CallCount,
      .Expected = 2, .Import = "_IOHIDValueGetScaledValue" },
};
// The table of half axes, as the axis loop takes the one for a value's sign:
//     and x8, x22, #0x1fe ; fcmp d0, #0.0 ; cset w9, pl ; orr x8, x8, x9 ; adrp x9, page ; add x9, x9, #offset
//     ldrh w23, [x9, x8, lsl #1]
constexpr MaskedWord kHidLooksUpAxis[] = {
    { 0x927f1ec8 },
    { 0x1e602008 },
    { 0x1a9f47e9 },
    { 0xaa090108 },
    { 0x90000009, Any::Adrp },
    { 0x91000129, Any::Imm12 },
    { 0x78687937 },
};
constexpr CodeShape kHidUpdateControlsShape{ 327, 0x9ff259390432905eull };    // 4.1.1.7631656
constexpr Derivation kControllerAxesDerivation{
    .Host = Exact(kHidUpdateControlsSymbol),
    .HostShape = kHidUpdateControlsShape,
    .Pattern = kHidLooksUpAxis,
    .What = Derivation::Take::PageAddress,
    .At = 4,
};
// cocoa::HIDInputDevice::Acquire() calibrates each axis element the bundle's
// GamePadDefinitions.plist names for the device: a stick (axis 0 to 3) to
// CalibrationMin = OverrideStickMin or -32768.0 and a trigger to the NSNumber
// literal taken here, both to CalibrationMax = OverrideStickMax or 32767.0,
// granularity 1; a stick also gets a dead zone around its centre, from the
// game's dead zone setting. Its GenData at the axis's index starts with the
// element's calibrated value. The trigger's minimum, after the stick's case:
//     b set ; mov x0, x22 ; adrp x1, @"CalibrationMin" ; add x1, ... ; adrp x2, minimum ; add x2, ... ; bl IOHIDElementSetProperty
constexpr MaskedWord kCalibratesTrigger[] = {
    { 0x14000006 },
    { 0xaa1603e0 },
    { 0x90000001, Any::Adrp },
    { 0x91000021, Any::Imm12 },
    { 0x90000002, Any::Adrp },
    { 0x91000042, Any::Imm12 },
    { 0x94000000, Any::Branch },
};
constexpr Derivation kTriggerCalibrationMinDerivation{
    .Host = Exact(kHidAcquireSymbol),
    .HostShape = { 732, 0x9b4a9b5b5d25ae44ull },     // 4.1.1.7631656
    .Pattern = kCalibratesTrigger,
    .What = Derivation::Take::PageAddress,
    .At = 4,
};

// --- textures ---------------------------------------------------------------

// The texture loading of a material, as it is inlined into
// ls::AppliedMaterial::TryOverrideTexture2DParameter (x21 is the texture
// resource, x23 the resource manager): a resource with a source file is
// registered with the texture manager, its load state is written, and the
// texture is instantiated.
//     ldrb w8, [x21, #Type] ; cmp w8, #8 ; b.hs ... ; add x1, x21, #SourceFile ; add x2, x21, #Guid
//     ldr x0, [x23, #TextureManager] ; bl ls::ResourceManager::AddTexture ; mov w8, #2
constexpr MaskedWord kRegistersTexture[] = {
    { 0x394002a8, Any::Imm12 },
    { 0x7100211f },
    { 0x54000002, Any::CondBranch },
    { 0x910002a1, Any::Imm12 },
    { 0x910002a2, Any::Imm12 },
    { 0xf94002e0, Any::Imm12 },
    { 0x94000000, Any::Branch },
    { 0x52800048 },
};
//     strb w8, [x21, #state] ; add x1, x21, #Guid ; ldrb w2, [x21, #Streaming] ; ldrb w3, [x21, #SRGB]
//     ldr x0, [x23, #TextureManager] ; mov w4, #1 ; bl ls::ResourceManager::InstantiateTexture
constexpr MaskedWord kInstantiatesTexture[] = {
    { 0x390002a8, Any::Imm12 },
    { 0x910002a1, Any::Imm12 },
    { 0x394002a2, Any::Imm12 },
    { 0x394002a3, Any::Imm12 },
    { 0xf94002e0, Any::Imm12 },
    { 0x52800024 },
    { 0x94000000, Any::Branch },
};
// The resource bank it looks the resource up in (x0 is the thread's bank
// index, a signed byte):  ldrsb x8, [x0] ; add x8, x23, x8, lsl #3 ; ldr x0, [x8, #ResourceBanks]
constexpr MaskedWord kTakesResourceBank[] = { { 0x39800008 }, { 0x8b080ee8 }, { 0xf9400100, Any::Imm12 } };
// The resource manager it takes the texture manager from: adrp x22, page ; ldr x23, [x22, #offset]
constexpr MaskedWord kLoadsResourceManager[] = { { 0x90000016, Any::Adrp }, { 0xf94002d7, Any::Imm12 } };
// rf::metal::MetalRenderFrame::GetDrawable() puts the drawable's texture into
// the back buffer's rf::Texture:  bl texture ; str x0, [x19, #MTLTexture]
constexpr MaskedWord kStoresDrawableTexture[] = { { 0x94000000, Any::Branch }, { 0xf9000260, Any::Imm12 } };
// rf::metal::GPUDevice::CreateTexture stores the texture it makes:
//     mov x2, x23 ; bl newTextureWithDescriptor: ; str x0, [x20, #MTLTexture]
constexpr MaskedWord kStoresNewTexture[] = { { 0xaa1703e2 }, { 0x94000000, Any::Branch }, { 0xf9000280, Any::Imm12 } };

constexpr Operand kAddTextureOperands[] = {
    { .What = kResourceType, .In = Exact(kOverrideTextureSymbol), .Pattern = kRegistersTexture, .At = 0, .Expected = kTextureResourceType },
    { .What = kResourceSourceFile, .In = Exact(kOverrideTextureSymbol), .Pattern = kRegistersTexture, .At = 3, .How = Operand::Read::AddImmediate,
      .Expected = kGameResourceSourceFile },
    { .What = kResourceGuidAdded, .In = Exact(kOverrideTextureSymbol), .Pattern = kRegistersTexture, .At = 4, .How = Operand::Read::AddImmediate,
      .Expected = kGameResourceGuid },
    { .What = kTextureManagerAdded, .In = Exact(kOverrideTextureSymbol), .Pattern = kRegistersTexture, .At = 5, .Expected = kResourceManagerTextureManager },
    { .What = "the material registers through it", .In = Exact(kOverrideTextureSymbol), .Pattern = kRegistersTexture, .At = 6,
      .How = Operand::Read::BranchTarget, .Target = Exact(kAddTextureSymbol) },
};
constexpr Operand kInstantiateTextureOperands[] = {
    { .What = kResourceState, .In = Exact(kOverrideTextureSymbol), .Pattern = kInstantiatesTexture, .At = 0, .Expected = kLoadableResourceState },
    { .What = kResourceGuid, .In = Exact(kOverrideTextureSymbol), .Pattern = kInstantiatesTexture, .At = 1, .How = Operand::Read::AddImmediate,
      .Expected = kGameResourceGuid },
    { .What = kResourceStreaming, .In = Exact(kOverrideTextureSymbol), .Pattern = kInstantiatesTexture, .At = 2, .Expected = kTextureResourceStreaming },
    { .What = kResourceSRGB, .In = Exact(kOverrideTextureSymbol), .Pattern = kInstantiatesTexture, .At = 3, .Expected = kTextureResourceSRGB },
    { .What = kTextureManager, .In = Exact(kOverrideTextureSymbol), .Pattern = kInstantiatesTexture, .At = 4, .Expected = kResourceManagerTextureManager },
    { .What = "the material instantiates through it", .In = Exact(kOverrideTextureSymbol), .Pattern = kInstantiatesTexture, .At = 6,
      .How = Operand::Read::BranchTarget, .Target = Exact(kInstantiateTextureSymbol) },
    { .What = kResourceBanks, .In = Exact(kOverrideTextureSymbol), .Pattern = kTakesResourceBank, .At = 2, .Expected = kResourceManagerBanks },
    { .What = "the manager is the resource manager's", .In = Exact(kOverrideTextureSymbol), .Pattern = kLoadsResourceManager,
      .How = Operand::Read::PointerLoad, .Target = Exact(kResourceManagerSymbol) },
    { .What = "one lookup and load", .How = Operand::Read::CallCount, .Expected = 1, .Target = Prefix(kInstantiateInternalPrefix) },
    { .What = kMetalTexture, .In = Exact(kGetDrawableSymbol), .Pattern = kStoresDrawableTexture, .At = 1, .Expected = 0x38 },
    { .What = "the drawable's texture", .In = Exact(kGetDrawableSymbol), .Pattern = kStoresDrawableTexture, .At = 0,
      .How = Operand::Read::BranchTarget, .Target = Exact("_objc_msgSend$texture") },
    { .What = kMetalTextureCreated, .In = Exact(kCreateTextureSymbol), .Pattern = kStoresNewTexture, .At = 2, .Expected = 0x38 },
    { .What = "a texture the device makes", .In = Exact(kCreateTextureSymbol), .Pattern = kStoresNewTexture, .At = 1,
      .How = Operand::Read::BranchTarget, .Target = Exact("_objc_msgSend$newTextureWithDescriptor:") },
};

// --- high dynamic range ----------------------------------------------------

// In HDR output -[LSMTLView updateHDRState:] gives the game's layer the
// colour space kCGColorSpaceITUR_2020_PQ_EOTF, and the game puts its UI over
// the frame with the fragment shader CombineUI_HDR.
// ls::CombineUIRenderStage::ExecuteWTKernel() gives that shader four floats,
// the last of them the paper white of the graphic settings, in nits (the
// field BaseApp::LoadGraphicSettingsConfig fills from the config's
// "HDRPaperWhite"):
//     adrp x8, page ; ldr x8, [x8, #GraphicSettings] ; cbnz w21, ... ; ldr s0, [x8, #0x2c] ; fmov s1, #1.0
//     fdiv s0, s1, s0 ; str s0, [x22, #4] ; ldr s0, [x8, #0x30] ; str s0, [x22, #8] ; ldr s0, [x8, #paper white]
//     str s0, [x22, #0xc]
// ... and hands them to the shader:  mov x2, x22 ; mov w3, #0x10 ; mov x4, #0 ; bl setFragmentBytes:length:atIndex:
//
// The first of the four, the shader's UIPaperWhite, is a constant the stage
// picks by the byte at +0x3d of the frame it reads (where
// rf::metal::MetalRenderFrame::BeginFrame() keeps what
// -[LSMTLView updateHDRState:] returns), the byte that also picks the pipeline
// at +0x2170, which Load() makes from CombineUI_HDR.bshd, over the one at
// +0x2168, made from CombineUI.bshd. It is the only one CombineUI_HDR reads: the UI's colour, sRGB-decoded and taken from
// BT.709 to BT.2020 primaries, is multiplied by it and PQ-encoded over 10000
// nits, so it is the luminance of the game's UI white. HDRPaperWhite, the
// fourth, is not read by that shader. Just before the sequence above:
//     cmp w28, #0 ; mov w8, #white ; fmov s1, w8 ; fmov s0, #1.0 ; fcsel s1, s0, s1, eq ; str s1, [x22]
constexpr MaskedWord kGivesUiWhite[] = {
    { 0x7100039f },
    { 0x52a00008, Any::Imm16 },
    { 0x1e270101 },
    { 0x1e2e1000 },
    { 0x1e210c01 },
    { 0xbd0002c1 },
    { 0x90000008, Any::Adrp },
    { 0xf9400108, Any::Imm12 },
    { 0x35000015, Any::CondBranch },
};
// 300.0f: what CombineUI_HDR is given as the UI's white on this build.
constexpr uint64_t kGameUiWhiteBits = 0x43960000;
constexpr MaskedWord kCombinesUi[] = {
    { 0x90000008, Any::Adrp },
    { 0xf9400108, Any::Imm12 },
    { 0x35000015, Any::CondBranch },
    { 0xbd400100, Any::Imm12 },
    { 0x1e2e1001 },
    { 0x1e201820 },
    { 0xbd0006c0 },
    { 0xbd400100, Any::Imm12 },
    { 0xbd000ac0 },
    { 0xbd400100, Any::Imm12 },
    { 0xbd000ec0 },
};
constexpr MaskedWord kGivesUiConstants[] = {
    { 0xaa1603e2 },
    { 0x52800203 },
    { 0xd2800004 },
    { 0x94000000, Any::Branch },
};
constexpr Operand kGraphicSettingsOperands[] = {
    { .What = kGraphicSettingsLoaded, .In = Exact(kCombineUiSymbol), .Pattern = kCombinesUi, .At = 0, .How = Operand::Read::PointerLoad,
      .Target = Exact(kGraphicSettingsSymbol) },
    { .What = kPaperWhite, .In = Exact(kCombineUiSymbol), .Pattern = kCombinesUi, .At = 9, .Expected = 0x80 },
    { .What = kUiWhite, .In = Exact(kCombineUiSymbol), .Pattern = kGivesUiWhite, .At = 1, .How = Operand::Read::MoveValue,
      .Expected = kGameUiWhiteBits },
    { .What = "the UI shader is given them", .In = Exact(kCombineUiSymbol), .Pattern = kGivesUiConstants, .At = 3,
      .How = Operand::Read::BranchTarget, .Target = Exact("_objc_msgSend$setFragmentBytes:length:atIndex:") },
};

constexpr Entry kEntries[] = {
    {
        .Name = kRenderFrameVtable,
        .What = Kind::Vtable,
        .Mac = "vtable for rf::metal::MetalRenderFrame",
        .Use = "Mac/Overlay: the slot of FlipBuffers(bool) is replaced, where Extender/Client/IMGUI/DX11.inl and Vulkan.inl hook the "
               "creation of the swap chain: the frame's drawable and the command buffer its present is scheduled on are read there",
        .Where = Exact("__ZTVN2rf5metal16MetalRenderFrameE"),
        .Methods = kRenderFrameSlots,
        .Needs = kFlipBuffers,
    },
    {
        .Name = kFlipBuffers,
        .What = Kind::FunctionToCall,
        .Mac = "rf::metal::MetalRenderFrame::FlipBuffers(bool): sends presentDrawable: (or presentDrawable:afterMinimumDuration:) to "
               "the main renderer's command buffer; the only sender of either in the game",
        .Use = "Mac/Overlay: called from the function that replaces it in the vtable; the offsets of the drawable, the renderer and "
               "the renderer's command buffer are the ones its code reads",
        .Where = Exact(kFlipBuffersSymbol),
        .Call = kFlipBuffersCall,
        .Shape = { 26, 0x69931019d2af6895ull },     // 4.1.1.7631656
        .Operands = kFlipBuffersOperands,
        .Caveat = "it starts with a branch (cbz w1) and nothing calls it directly: replace the vtable slot, do not hook its entry. It "
                  "can run while the renderer still has an encoder open on the buffer: nothing may be encoded from it",
    },
    {
        .Name = kCommitCommandBuffer,
        .What = Kind::FunctionToHook,
        .Mac = "rf::metal::MetalRenderer::CommitCommandBuffer(bool): commits the renderer's command buffer and, when asked, waits for it",
        .Use = "Mac/Overlay: a hook at its entry draws the overlay in a render pass of its own on the buffer about to be committed, "
               "where DX11.inl hooks IDXGISwapChain::Present and Vulkan.inl vkQueuePresentKHR",
        .Where = Exact(kCommitSymbol),
        .Call = Takes({
            Pointer(Place::X0, -1, "rf::metal::MetalRenderer"),
            Value(Place::X1, 1, -1, "bool: wait until the buffer has completed"),
        }),
        .Shape = { 165, 0xd99de3f92558caa4ull },     // 4.1.1.7631656
        .Operands = kCommitOperands,
        .Needs = kFlipBuffers,
        .Caveat = "every renderer commits through it, worker threads' too: the hook must tell the buffer of the frame being presented "
                  "from any other, by the buffer FlipBuffers was given",
    },
    {
        .Name = kViewInit,
        .What = Kind::FunctionToCall,
        .Mac = "-[LSMTLView initWithFrame:device:display:hdr:vsync:]: makes the game's view and its CAMetalLayer",
        .Use = "Mac/Overlay: called from the method that replaces it, which notes the view it returns (mouse positions take their "
               "scale from the view), where Extender/Client/SDL.cpp hooks SDL_CreateWindow",
        .Where = Exact(kViewInit),
        .Call = Takes({
            Pointer(Place::X0, -1, "LSMTLView: self"),
            Unread(Place::X1, -1, "SEL"),
            Pointer(Place::X2, -1, "id<MTLDevice>"),
            Value(Place::X3, 4, -1, "int: the display"),
            Value(Place::X4, 1, -1, "BOOL: hdr"),
            Value(Place::X5, 1, -1, "BOOL: vsync"),
        }, Returns::X0, "LSMTLView*"),
        .Shape = { 146, 0x039ed10e71eab994ull },     // 4.1.1.7631656
        .Caveat = "the frame, a CGRect, is passed in d0 to d3, which the statement above cannot name",
    },
    {
        .Name = kSendEvent,
        .What = Kind::FunctionToCall,
        .Mac = "-[BG3Application sendEvent:]: every event the game's main loop takes from the queue is sent through it",
        .Use = "Mac/Overlay: called from the method that replaces it, for every event the overlay and Lua leave to the game, where "
               "Extender/Client/SDL.cpp wraps SDL_PollEvent",
        .Where = Exact(kSendEvent),
        .Call = Takes({
            Pointer(Place::X0, -1, "BG3Application: self"),
            Unread(Place::X1, -1, "SEL"),
            Pointer(Place::X2, -1, "NSEvent"),
        }),
        .Shape = { 32, 0xd3cffa0a63cfcf9full },     // 4.1.1.7631656
    },
    {
        .Name = kInjectInput,
        .What = Kind::FunctionToCallAndHook,
        .Mac = "ls::InputManager::InjectInput(ls::InputRawChange const&, bool): appends the change to the manager's queue under its "
               "lock; the game thread takes the queue",
        .Use = "Mac/Overlay: Ext.Input.InjectKeyPress, InjectKeyDown and InjectKeyUp (Lua/Libs/ClientInput.inl) queue a key through "
               "it, where Extender/Client/SDL.cpp hands the game an SDL event of its own. A hook at its entry sees each button of a "
               "controller before the game does: Lua's ControllerButtonInput, which may keep it from the game, where SDL.cpp wraps "
               "SDL_PollEvent",
        .Where = Exact(kInjectInputSymbol),
        .Call = Takes({
            Pointer(Place::X0, -1, "ls::InputManager"),
            Pointer(Place::X1, -1, "ls::InputRawChange const: the input's id (32 bits), two floats at +4, the pressed byte at +0xc"),
            Value(Place::X2, 1, -1, "bool: zero from the key and button methods of the view, one from its scroll wheel"),
        }),
        .Shape = { 240, 0x7c233f7413b04932ull },     // 4.1.1.7631656
        .Operands = kInjectInputOperands,
        .Needs = kInputManager,
    },
    {
        .Name = kKeyboardKeys,
        .What = Kind::GlobalObject,
        .Mac = "cocoa::CocoaInputTranslator::s_KeyboardKeys: the input id of each macOS virtual key code, 16 bits each, 0xffff for none",
        .Use = "Mac/Overlay: the input id of an injected key, as -[LSMTLView keyDown:] takes it",
        .Derived = &kKeyboardKeysDerivation,
        .Bytes = 0x168,
        .Agrees = Exact(kKeyboardKeysSymbol),
        .Operands = kKeyboardKeysOperands,
        .Caveat = "zero-filled until the game has set up its input: an id read before then is 0",
    },
    {
        .Name = kModifierKeys,
        .What = Kind::GlobalObject,
        .Mac = "cocoa::CocoaInputTranslator::s_KeyboardKeys as -[LSMTLView flagsChanged:] reads it: the game has no modifier state of "
               "its own, each modifier key is queued as a key, pressed while its kind's flag is set",
        .Use = "Mac/Overlay: the modifiers an injected key names (Ext.Input.InjectKeyPress and the others) are queued as their keys "
               "around it, as the view would have queued them",
        .Derived = &kModifierKeysDerivation,
        .Bytes = 0x168,
        .Agrees = Exact(kKeyboardKeysSymbol),
        .Operands = kModifierKeysOperands,
        .Caveat = "zero-filled until the game has set up its input",
    },
    {
        .Name = kAddTexture,
        .What = Kind::FunctionToCall,
        .Mac = "ls::ResourceManager::AddTexture(ls::Path const&, ls::FixedString const&): gives the texture manager an entry for the "
               "GUID with the file's path, unless it has one",
        .Use = "Mac/Overlay: the first half of what ls__AppliedMaterial__LoadTexture does on Windows (Extender/Client/IMGUI/IMGUI.cpp)",
        .Where = Exact(kAddTextureSymbol),
        .Call = Takes({
            Pointer(Place::X0, -1, "ls::TextureManager: the pointer at +0x80 of the resource manager"),
            Pointer(Place::X1, -1, "ls::Path const: the texture resource's source file"),
            Pointer(Place::X2, -1, "ls::FixedString const: the texture's GUID"),
        }),
        .Shape = { 194, 0x735b2aac7b7e4abdull },     // 4.1.1.7631656
        .Operands = kAddTextureOperands,
        .Needs = kResourceManager,
    },
    {
        .Name = kInstantiateTexture,
        .What = Kind::FunctionToCall,
        .Mac = "ls::ResourceManager::InstantiateTexture(ls::FixedString const&, bool, bool, bool): takes a reference to the texture "
               "the manager has an entry for and loads it before returning",
        .Use = "Mac/Overlay: the second half of what ls__AppliedMaterial__LoadTexture does on Windows",
        .Where = Exact(kInstantiateTextureSymbol),
        .Call = Takes({
            Pointer(Place::X0, -1, "ls::TextureManager: the pointer at +0x80 of the resource manager"),
            Pointer(Place::X1, -1, "ls::FixedString const: the texture's GUID"),
            Value(Place::X2, 1, -1, "bool: the resource's Streaming"),
            Value(Place::X3, 1, -1, "bool: the resource's SRGB"),
            Value(Place::X4, 1, -1, "bool: one from every caller read"),
        }, Returns::X0, "rf::Texture*: null when the manager has no entry for the GUID or the load gave no texture"),
        .Shape = { 68, 0xa84938c1d45c0a69ull },     // 4.1.1.7631656
        .Operands = kInstantiateTextureOperands,
        .Needs = kAddTexture,
        .Caveat = "the result is the game's rf::Texture, not Norbyte's TextureDescriptor; its id<MTLTexture> is at the offset "
                  "rf::metal::MetalRenderFrame::GetDrawable and rf::metal::GPUDevice::CreateTexture store one at. A null result has "
                  "given its reference back already: it must not be released again",
    },
    {
        .Name = kUpdateControls,
        .What = Kind::FunctionToHook,
        .Mac = "cocoa::MFIInputDevice::UpdateControls(): reads the state of a GCController and queues the changes of its elements; "
               "cocoa::CocoaWindowManager::Update calls it for every controller the game has opened through the GameController "
               "framework",
        .Use = "Mac/Overlay: a hook at its entry reads the same controller's raw state first, for Lua's ControllerAxisInput and the "
               "overlay's gamepad navigation, where Extender/Client/SDL.cpp gets SDL's controller events and ImGui's SDL backend "
               "polls SDL's game controllers",
        .Where = Exact(kUpdateControlsSymbol),
        .Call = Takes({ Pointer(Place::X0, -1, "cocoa::MFIInputDevice") }),
        .Shape = { 440, 0xa2cbf585316de510ull },     // 4.1.1.7631656
        .Operands = kUpdateControlsOperands,
        .Needs = kInjectInput,
        .Caveat = "the device's controller can be disconnected while the device lives: its extended gamepad is then nil",
    },
    {
        .Name = kControllerButtons,
        .What = Kind::GlobalObject,
        .Mac = "cocoa::CocoaInputTranslator::s_ControllerButtons: the InputRawType of each controller button, 16 bits each, written "
               "by CocoaBindingSDM::Init",
        .Use = "Mac/Overlay: compared, once the game has written it, with the InputRawType values the controller's buttons are told "
               "apart by",
        .Where = Exact(kControllerButtonsSymbol),
        .Bytes = 0x46,
        .Caveat = "zero-filled until the game has set up its input",
    },
    {
        .Name = kRegisterDevice,
        .What = Kind::FunctionToHook,
        .Mac = "ls::InputManager::RegisterDevice(ls::InputDevice const&): records a device the game has opened and queues its arrival; "
               "cocoa::CocoaWindowManager::OpenController calls it for each controller it opens, with the device id player index + 3",
        .Use = "Mac/Overlay: a hook at its entry gives a controller that arrives the next SDL joystick instance id, which Lua's "
               "ControllerAxisInput and ControllerButtonInput carry as DeviceId (SDL numbers each joystick it opens, from 0, and "
               "never reuses a number)",
        .Where = Exact(kRegisterDeviceSymbol),
        .Call = Takes({
            Pointer(Place::X0, -1, "ls::InputManager"),
            Pointer(Place::X1, -1, "ls::InputDevice const (GameDefinitions/Input.h): DeviceId, 16 bits, at +0x10"),
        }),
        .Shape = { 293, 0x9434c943bd1163e3ull },     // 4.1.1.7631656
        .Operands = kRegisterDeviceOperands,
        .Caveat = "the window manager's Init registers the keyboard, the mouse and the touch bar through it too: only the device ids "
                  "of controllers are a controller's",
    },
    {
        .Name = kUnregisterDevice,
        .What = Kind::FunctionToHook,
        .Mac = "ls::InputManager::UnregisterDevice(ls::EInputDeviceID): forgets a device the game has closed; "
               "cocoa::CocoaWindowManager::CloseController and CloseAllControllers call it for each controller they close",
        .Use = "Mac/Overlay: a hook at its entry forgets the SDL joystick instance id of the controller that went, so that one that "
               "comes back gets a new one, as with SDL",
        .Where = Exact(kUnregisterDeviceSymbol),
        .Call = Takes({
            Pointer(Place::X0, -1, "ls::InputManager"),
            Value(Place::X1, 2, -1, "ls::EInputDeviceID, 16 bits, sign-extended by the callers"),
        }),
        .Shape = { 168, 0xf2c14f9cb4ce59faull },     // 4.1.1.7631656
        .Operands = kUnregisterDeviceOperands,
        .Needs = kRegisterDevice,
    },
    {
        .Name = kOpenController,
        .What = Kind::FunctionToHook,
        .Mac = "cocoa::CocoaWindowManager::OpenController(GCController*): opens a GCController the game is told of; one it has open "
               "already it closes first (CloseController, which unregisters it) and opens again",
        .Use = "Mac/Overlay: a hook around it keeps the SDL instance id of a controller opened again while open, as SDL ignores a "
               "GameController it already has (SDL_mfijoystick.m, IOS_AddJoystickDevice)",
        .Where = Exact(kOpenControllerSymbol),
        .Call = Takes({
            Pointer(Place::X0, -1, "cocoa::CocoaWindowManager"),
            Pointer(Place::X1, -1, "GCController"),
        }),
        .Shape = { 292, 0x4932f682837ffc76ull },     // 4.1.1.7631656
        .Operands = kOpenControllerOperands,
        .Needs = kUnregisterDevice,
    },
    {
        .Name = kHidUpdateControls,
        .What = Kind::FunctionToHook,
        .Mac = "cocoa::HIDInputDevice::UpdateControls(): queues the changes of a controller the game reads through IOHID, from the "
               "values its IOHID value callback stored; cocoa::CocoaWindowManager::Update calls it for every such controller",
        .Use = "Mac/Overlay: a hook at its entry reads the axis values the callback stored (calibrated by IOHID to the ranges the "
               "game's Acquire set), for Lua's ControllerAxisInput and the overlay's gamepad navigation, where Extender/Client/SDL.cpp "
               "gets SDL's controller events",
        .Where = Exact(kHidUpdateControlsSymbol),
        .Call = Takes({ Pointer(Place::X0, -1, "cocoa::HIDInputDevice") }),
        .Shape = kHidUpdateControlsShape,
        .Operands = kHidUpdateControlsOperands,
        .Needs = kInjectInput,
        .Caveat = "the value callback writes the values on the HID manager's thread while this reads them: each is one aligned double",
    },
    {
        .Name = kControllerAxes,
        .What = Kind::GlobalObject,
        .Mac = "cocoa::CocoaInputTranslator::s_ControllerAxes: the InputRawType of each half axis, two for each axis a HIDInputDevice "
               "has, 16 bits each, written by CocoaBindingSDM::Init",
        .Use = "Mac/Overlay: compared, once the game has written it, with the half axes that make a HIDInputDevice's axis index the "
               "SDL axis it is",
        .Derived = &kControllerAxesDerivation,
        .Bytes = 0x18,
        .Agrees = Exact(kControllerAxesSymbol),
        .Caveat = "zero-filled until the game has set up its input",
    },
    {
        .Name = kTriggerCalibrationMin,
        .What = Kind::GlobalObject,
        .Mac = "the NSNumber literal cocoa::HIDInputDevice::Acquire() sets as the CalibrationMin of a trigger's element",
        .Use = "Mac/Overlay: its value, read in the running game, must be 0: a trigger's calibrated value is then SDL's range of a "
               "trigger, 0 to 32767",
        .Derived = &kTriggerCalibrationMinDerivation,
        .Needs = kHidUpdateControls,
    },
    {
        .Name = kGraphicSettings,
        .What = Kind::GlobalPointer,
        .Mac = "GraphicSettings: the game's rf::GraphicSettings, which ls::CombineUIRenderStage::ExecuteWTKernel() reads beside the "
               "constant it gives CombineUI_HDR as the white of the game's UI",
        .Use = "Mac/Overlay: in HDR output the overlay's white is drawn at the white of the game's UI, the constant read from that "
               "function's code. Norbyte's backends draw onto an HDR swap chain without a conversion",
        .Where = Exact(kGraphicSettingsSymbol),
        .Operands = kGraphicSettingsOperands,
        .Caveat = "what CombineUI_HDR does with the constant was read from the shader in Core.metallib, not checked at run time",
    },
};

}

std::span<Entry const> OverlayEntries()
{
    return kEntries;
}

}
