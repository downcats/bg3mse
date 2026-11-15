#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <objc/runtime.h>

#include <Overlay/ControllerHooks.h>
#include <Overlay/InputEvents.h>
#include <Platform/EntryHook.h>
#include <Platform/Log.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <pthread.h>
#include <vector>

#if !__has_feature(objc_arc)
#error "ControllerHooks.mm is written for automatic reference counting"
#endif

namespace bg3se::mac::overlay {

namespace {

using Clock = std::chrono::steady_clock;

// A controller that has not been read for this long is not navigated with:
// the game reads each one every frame it runs.
constexpr auto kStale = std::chrono::milliseconds(500);

bool Pressed(GCControllerButtonInput* button)
{
    return button && button.isPressed;
}

// The framework's state of an extended gamepad, by the SDL button each
// element is (SDL's GameController joystick driver, SDL_mfijoystick.m).
GamepadReading ReadingOf(GCExtendedGamepad* pad)
{
    GamepadReading r;
    r.LeftX = pad.leftThumbstick.xAxis.value;
    r.LeftY = pad.leftThumbstick.yAxis.value;
    r.RightX = pad.rightThumbstick.xAxis.value;
    r.RightY = pad.rightThumbstick.yAxis.value;
    r.LeftTrigger = pad.leftTrigger.value;
    r.RightTrigger = pad.rightTrigger.value;
    r.Buttons[SDL_CONTROLLER_BUTTON_A] = Pressed(pad.buttonA);
    r.Buttons[SDL_CONTROLLER_BUTTON_B] = Pressed(pad.buttonB);
    r.Buttons[SDL_CONTROLLER_BUTTON_X] = Pressed(pad.buttonX);
    r.Buttons[SDL_CONTROLLER_BUTTON_Y] = Pressed(pad.buttonY);
    r.Buttons[SDL_CONTROLLER_BUTTON_BACK] = Pressed(pad.buttonOptions);
    r.Buttons[SDL_CONTROLLER_BUTTON_GUIDE] = Pressed(pad.buttonHome);
    r.Buttons[SDL_CONTROLLER_BUTTON_START] = Pressed(pad.buttonMenu);
    r.Buttons[SDL_CONTROLLER_BUTTON_LEFTSTICK] = Pressed(pad.leftThumbstickButton);
    r.Buttons[SDL_CONTROLLER_BUTTON_RIGHTSTICK] = Pressed(pad.rightThumbstickButton);
    r.Buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = Pressed(pad.leftShoulder);
    r.Buttons[SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = Pressed(pad.rightShoulder);
    r.Buttons[SDL_CONTROLLER_BUTTON_DPAD_UP] = Pressed(pad.dpad.up);
    r.Buttons[SDL_CONTROLLER_BUTTON_DPAD_DOWN] = Pressed(pad.dpad.down);
    r.Buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT] = Pressed(pad.dpad.left);
    r.Buttons[SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = Pressed(pad.dpad.right);
    // By the names of the controller's elements, as SDL takes them: the share
    // button of any controller is SDL's misc1, and SDL's paddles 2 and 3 are
    // the Elite's P3 and P2 (SDL_gamecontroller.h; its Windows driver's
    // mapping of an Elite, SDL_gamecontroller.c, is the same).
    NSDictionary<NSString*, GCControllerButtonInput*>* buttons = pad.controller.physicalInputProfile.buttons;
    r.Buttons[SDL_CONTROLLER_BUTTON_PADDLE1] = Pressed(buttons[GCInputXboxPaddleOne]);
    r.Buttons[SDL_CONTROLLER_BUTTON_PADDLE2] = Pressed(buttons[GCInputXboxPaddleThree]);
    r.Buttons[SDL_CONTROLLER_BUTTON_PADDLE3] = Pressed(buttons[GCInputXboxPaddleTwo]);
    r.Buttons[SDL_CONTROLLER_BUTTON_PADDLE4] = Pressed(buttons[GCInputXboxPaddleFour]);
    if (@available(macOS 12.0, *)) r.Buttons[SDL_CONTROLLER_BUTTON_MISC1] = Pressed(buttons[GCInputButtonShare]);
    if ([pad isKindOfClass:[GCDualShockGamepad class]]) {
        r.Buttons[SDL_CONTROLLER_BUTTON_TOUCHPAD] = Pressed(((GCDualShockGamepad*)pad).touchpadButton);
    } else if (@available(macOS 11.3, *)) {
        if ([pad isKindOfClass:[GCDualSenseGamepad class]]) {
            r.Buttons[SDL_CONTROLLER_BUTTON_TOUCHPAD] = Pressed(((GCDualSenseGamepad*)pad).touchpadButton);
        }
    }
    return r;
}

}

struct ControllerTracker::Impl
{
    // What is known of the controller at one player index.
    struct Slot
    {
        bool Read{ false };             // State is of Device, read at Seen
        bool Adopt{ false };            // State is of the next device read: the same controller, opened again
        void* Device{ nullptr };
        void* Controller{ nullptr };    // its GCController, for an MFi device
        GamepadState State;
        Clock::time_point Seen;
    };

    std::mutex SinkLock;
    ControllerSink* Sink{ nullptr };
    std::mutex Lock;
    ControllerNumbering Numbering;      // under Lock
    Slot Slots[kControllerPlayers];     // under Lock
    std::vector<SDL_Event> Events;      // under SinkLock: the storage of one read's events
    std::atomic<int> Table{ 0 };        // 0: not yet compared, 1: as expected, 2: different
    std::atomic<int> Axes{ 0 };         // the same, of the half axes
    std::atomic<bool> HidAxes{ true };
    std::atomic<bool> SaidThread{ false };

    // Keeps the state just read of the device at `player`; returns the one
    // read before (all released and centred the first time a device is
    // read there), and the controller's instance id in `which`. With
    // `keepButtons` the device has no buttons to read: they stay, in both
    // states, as the game's changes left them. `fresh`: the device was not
    // the one last read at `player`.
    GamepadState Record(int player, void* device, void* controller, GamepadState& now, bool keepButtons, int32_t& which, bool& fresh)
    {
        std::lock_guard _(Lock);
        which = Numbering.Of(player);
        auto& slot = Slots[player];
        fresh = !slot.Read || slot.Device != device || slot.Controller != controller;
        GamepadState before;
        if (slot.Read && (slot.Adopt || (slot.Device == device && slot.Controller == controller))) before = slot.State;
        slot.Adopt = false;
        if (keepButtons) {
            std::copy(std::begin(slot.State.Buttons), std::end(slot.State.Buttons), now.Buttons);
            std::copy(std::begin(slot.State.Buttons), std::end(slot.State.Buttons), before.Buttons);
        }
        slot.Read = true;
        slot.Device = device;
        slot.Controller = controller;
        slot.State = now;
        slot.Seen = Clock::now();
        return before;
    }
};

ControllerTracker::ControllerTracker(ControllerPlaces const& places)
    : places_(places), impl_(new Impl)
{}

void ControllerTracker::SetSink(ControllerSink* sink)
{
    std::lock_guard _(impl_->SinkLock);
    impl_->Sink = sink;
}

void ControllerTracker::SetHidAxes(bool on)
{
    impl_->HidAxes.store(on);
}

bool ControllerTracker::TableUsable()
{
    int state = impl_->Table.load();
    if (state != 0) return state == 1;
    std::string problem;
    switch (CheckControllerButtons(places_.Buttons, problem)) {
    case GameTable::NotYet:
        return false;
    case GameTable::AsExpected:
        impl_->Table.store(1);
        return true;
    case GameTable::Different:
        if (impl_->Table.exchange(2) == 0) {
            Log(LogLevel::Error, "overlay: the game's controller buttons are not the InputRawType values the overlay tells them apart by "
                "(%s): controllers give no Lua events and no navigation", problem.c_str());
        }
        return false;
    }
    return false;
}

bool ControllerTracker::AxesUsable()
{
    if (!places_.HidAxes || !impl_->HidAxes.load()) return false;
    int state = impl_->Axes.load();
    if (state != 0) return state == 1;
    std::string problem;
    switch (CheckControllerAxes(places_.HalfAxes, problem)) {
    case GameTable::NotYet:
        return false;
    case GameTable::AsExpected:
        impl_->Axes.store(1);
        return true;
    case GameTable::Different:
        if (impl_->Axes.exchange(2) == 0) {
            Log(LogLevel::Error, "overlay: the game's half axes are not in the order of SDL's axes (%s): the axes of controllers the game "
                "reads through IOHID give no Lua events and no navigation", problem.c_str());
        }
        return false;
    }
    return false;
}

ControllerRead ControllerTracker::Post(void* device, void* controller, int player, GamepadState now, bool keepButtons)
{
    ControllerRead read;
    read.Player = player;
    int32_t which = -1;
    bool fresh = false;
    GamepadState before = impl_->Record(player, device, controller, now, keepButtons, which, fresh);
    // Which of the game's objects a controller is read from, for a crash in
    // the game's code that holds one of them after the game let it go.
    if (fresh) {
        if (controller) {
            Log(LogLevel::Info, "overlay: controller %d (player %d) is read from the game's GameController device %p (GCController %p)",
                which, player, device, controller);
        } else {
            Log(LogLevel::Info, "overlay: controller %d (player %d) is read from the game's IOHID device %p", which, player, device);
        }
    }
    read.TouchpadChanged = before.Buttons[SDL_CONTROLLER_BUTTON_TOUCHPAD] != now.Buttons[SDL_CONTROLLER_BUTTON_TOUCHPAD];
    read.TouchpadPressed = now.Buttons[SDL_CONTROLLER_BUTTON_TOUCHPAD];

    // Told without the lock the overlay's navigation takes: a Lua handler runs here.
    std::lock_guard sink(impl_->SinkLock);
    impl_->Events.clear();
    SdlEventsOfReading(before, now, which, InputTicks(), impl_->Events);
    // Axes are only told; a button the game does not read has nothing to keep from it.
    if (impl_->Sink) {
        for (auto const& event : impl_->Events) impl_->Sink->OnControllerEvent(event);
    }
    return read;
}

ControllerRead ControllerTracker::BeforeRead(void* device)
{
    if (!device || !TableUsable()) return {};
    auto bytes = static_cast<uint8_t const*>(device);
    if (!bytes[places_.Acquired]) return {};      // as the game's function: an unacquired device is not read
    int player = bytes[places_.Player];
    if (player >= kControllerPlayers) return {};
    void* controllerPointer = *reinterpret_cast<void* const*>(bytes + places_.Controller);
    if (!controllerPointer) return {};
    if (!impl_->SaidThread.exchange(true)) {
        Log(LogLevel::Info, "overlay: the game reads its controllers on %s", pthread_main_np() ? "the main thread" : "a thread that is not the main thread");
    }

    GamepadState now;
    @autoreleasepool {
        id object = (__bridge id)controllerPointer;
        if (![object isKindOfClass:[GCController class]]) return {};
        GCExtendedGamepad* pad = ((GCController*)object).extendedGamepad;
        // Disconnected: the game reads nothing either.
        if (!pad) return {};
        now = SdlStateOf(ReadingOf(pad));
    }
    return Post(device, controllerPointer, player, now, false);
}

ControllerRead ControllerTracker::BeforeHidRead(void* device)
{
    if (!device || !TableUsable() || !AxesUsable()) return {};
    auto bytes = static_cast<uint8_t const*>(device);
    if (!bytes[places_.Acquired]) return {};      // as the game's function
    int player = bytes[places_.Player];
    if (player >= kControllerPlayers) return {};

    // The axes as the game's function takes them: the first four, the next
    // two when the device holds more than four, up to the first it has not.
    double values[kMaxHidAxes];
    size_t count = 0;
    auto axes = *reinterpret_cast<uint8_t const* const* const*>(bytes + places_.HidAxes);
    if (axes) {
        uint32_t held = *reinterpret_cast<uint32_t const*>(bytes + places_.HidAxisCount);
        size_t taken = held > 4 ? kMaxHidAxes : 4;
        for (; count < taken && axes[count]; count++) {
            values[count] = *reinterpret_cast<double const*>(axes[count] + places_.HidAxisValue);
        }
    }
    GamepadState now;
    SdlAxesOfHid({ values, count }, now.Axes);
    // Its buttons are what the game queued of them.
    return Post(device, nullptr, player, now, true);
}

bool ControllerTracker::BeforeInject(GameInputChange const& change, ControllerRead* read)
{
    int player = PlayerOfDevice(DeviceOf(change.Input));
    if (player < 0 || !TableUsable()) return true;
    if (SdlButtonOfGameInput(InputTypeOf(change.Input)) == SDL_CONTROLLER_BUTTON_INVALID) return true;     // a half axis
    int32_t which;
    {
        std::lock_guard _(impl_->Lock);
        // The game lets go of a controller's buttons after it went: told when it went.
        if (impl_->Numbering.Gone(player)) return true;
        which = impl_->Numbering.Of(player);
    }
    SDL_Event event;
    if (!SdlButtonEventOf(change, read, which, InputTicks(), event)) return true;
    {
        // The navigation's state of the buttons of a device with no state of
        // its own to read (IOHID); a read of a GameController sets them again.
        std::lock_guard _(impl_->Lock);
        impl_->Slots[player].State.Buttons[event.cbutton.button] = event.cbutton.state == SDL_PRESSED;
    }
    std::lock_guard _(impl_->SinkLock);
    return !impl_->Sink || impl_->Sink->OnControllerEvent(event);
}

void ControllerTracker::BeforeRegister(void const* inputDevice, int32_t keep)
{
    if (!inputDevice) return;
    auto id = *reinterpret_cast<int16_t const*>(static_cast<uint8_t const*>(inputDevice) + places_.DeviceId);
    int player = id < 0 ? -1 : PlayerOfDevice((uint32_t)id);
    if (player < 0) return;
    int32_t which;
    {
        std::lock_guard _(impl_->Lock);
        which = impl_->Numbering.Arrived(player, keep);
        // The same controller opened again keeps the state SDL has of it.
        if (keep >= 0) impl_->Slots[player].Adopt = true;
        else impl_->Slots[player] = {};
    }
    Log(LogLevel::Info, "overlay: the game registered controller %d (player %d)%s", which, player,
        keep >= 0 ? ", the same controller opened again" : "");
}

int32_t ControllerTracker::BeforeUnregister(int32_t deviceId, bool reopening)
{
    auto id = (int16_t)deviceId;
    int player = id < 0 ? -1 : PlayerOfDevice((uint32_t)id);
    if (player < 0) return -1;
    GamepadState last;
    int32_t which;
    {
        std::lock_guard _(impl_->Lock);
        // One the game had before it was watched has its id now, to go with.
        if (!impl_->Numbering.Gone(player)) impl_->Numbering.Of(player);
        which = impl_->Numbering.Went(player);
        last = impl_->Slots[player].State;
        if (!reopening) impl_->Slots[player] = {};
    }
    if (which >= 0) {
        Log(LogLevel::Info, "overlay: the game unregistered controller %d (player %d)%s", which, player,
            reopening ? " to open it again" : "");
    }
    if (which < 0 || reopening || !TableUsable()) return which;
    std::lock_guard sink(impl_->SinkLock);
    impl_->Events.clear();
    SdlEventsOfRemoval(last, which, InputTicks(), impl_->Events);
    if (impl_->Sink) {
        for (auto const& event : impl_->Events) impl_->Sink->OnControllerEvent(event);
    }
    return which;
}

bool ControllerTracker::FirstGamepad(GamepadState& out)
{
    std::lock_guard _(impl_->Lock);
    auto now = Clock::now();
    for (auto const& slot : impl_->Slots) {
        if (!slot.Read || now - slot.Seen > kStale) continue;
        out = slot.State;
        return true;
    }
    return false;
}

// --- the hooks --------------------------------------------------------------

namespace {

using UpdateControlsFn = void (*)(void* device);
using InjectInputFn = void (*)(void* manager, GameInputChange const* change, bool fromWheel);
using RegisterDeviceFn = void (*)(void* manager, void const* device);
using UnregisterDeviceFn = void (*)(void* manager, int32_t deviceId);
using OpenControllerFn = void (*)(void* windowManager, void* controller);

ControllerTracker* gTracker = nullptr;
UpdateControlsFn gUpdateControls = nullptr;
UpdateControlsFn gHidUpdateControls = nullptr;
InjectInputFn gInjectInput = nullptr;
RegisterDeviceFn gRegisterDevice = nullptr;
UnregisterDeviceFn gUnregisterDevice = nullptr;
OpenControllerFn gOpenController = nullptr;
EntryHook* gUpdateHook = nullptr;
EntryHook* gHidUpdateHook = nullptr;
EntryHook* gInjectHook = nullptr;
EntryHook* gRegisterHook = nullptr;
EntryHook* gUnregisterHook = nullptr;
EntryHook* gOpenHook = nullptr;
bool gInstalled = false;
bool gHidInstalled = false;
// The read of a controller this thread's game code is in.
thread_local ControllerRead* tRead = nullptr;
// The opening of a GCController this thread's game code is in: the id of
// the controller it closed first, if it did.
struct Opening
{
    int32_t Kept{ -1 };
};
thread_local Opening* tOpening = nullptr;

void UpdateControls(void* device)
{
    ControllerRead read = gTracker->BeforeRead(device);
    ControllerRead* outer = tRead;
    tRead = &read;
    gUpdateControls(device);
    tRead = outer;
}

void HidUpdateControls(void* device)
{
    ControllerRead read = gTracker->BeforeHidRead(device);
    ControllerRead* outer = tRead;
    tRead = &read;
    gHidUpdateControls(device);
    tRead = outer;
}

void InjectInput(void* manager, GameInputChange const* change, bool fromWheel)
{
    if (change && !gTracker->BeforeInject(*change, tRead)) return;
    gInjectInput(manager, change, fromWheel);
}

void RegisterDevice(void* manager, void const* device)
{
    gTracker->BeforeRegister(device, tOpening ? tOpening->Kept : -1);
    gRegisterDevice(manager, device);
}

void UnregisterDevice(void* manager, int32_t deviceId)
{
    // Inside OpenController(GCController*) the one unregistered is the same
    // GCController, under its own key (the engine table checks the call).
    int32_t which = gTracker->BeforeUnregister(deviceId, tOpening != nullptr);
    if (tOpening && which >= 0) tOpening->Kept = which;
    gUnregisterDevice(manager, deviceId);
}

void OpenController(void* windowManager, void* controller)
{
    Opening opening;
    Opening* outer = tOpening;
    tOpening = &opening;
    gOpenController(windowManager, controller);
    tOpening = outer;
}

// The trigger's CalibrationMin that HIDInputDevice::Acquire sets, an NSNumber
// literal of the game's: 0 makes a trigger's value SDL's.
std::string CheckTriggerMinimum(uint64_t address)
{
    if (!address) return "the game's literal was not found";
    id object = (__bridge id)reinterpret_cast<void*>((uintptr_t)address);
    Class cls = object_getClass(object);
    if (!cls || ![cls isSubclassOfClass:[NSNumber class]]) return "the game's literal is not an NSNumber";
    double value = [(NSNumber*)object doubleValue];
    if (value != 0.0) {
        char text[96];
        snprintf(text, sizeof(text), "the game's literal is %g, expected 0", value);
        return text;
    }
    return {};
}

}

std::string InstallControllerHooks(Dependencies const& d, ControllerSink* sink)
{
    if (gInstalled) return {};
    if (!sink) return "no sink for the events";
    if (!d.Live) return "the dependencies were not checked on the running game";
    if (!d.Usable(Part::Controllers)) return d.Problem(Part::Controllers);

    // The axes of IOHID controllers stand alone: without them the rest still goes in.
    bool hid = d.Usable(Part::HidControllerAxes);
    if (!hid) {
        Log(LogLevel::Error, "overlay: the axes of controllers the game reads through IOHID give no Lua events and no navigation: %s",
            d.Problem(Part::HidControllerAxes).c_str());
    } else if (auto problem = CheckTriggerMinimum(d.TriggerCalibrationMin); !problem.empty()) {
        hid = false;
        Log(LogLevel::Error, "overlay: the axes of controllers the game reads through IOHID give no Lua events and no navigation: a "
            "trigger's CalibrationMin: %s", problem.c_str());
    }

    // The tracker is never freed: a hook that was taken out may still be running.
    if (!gTracker) {
        ControllerPlaces places;
        places.Acquired = d.DeviceEnabled;
        places.Player = d.DevicePlayer;
        places.Controller = d.DeviceController;
        places.DeviceId = d.RegisteredDeviceId;
        places.Buttons = { reinterpret_cast<uint16_t const*>((uintptr_t)d.ControllerButtons), d.ControllerButtonCount };
        if (d.Usable(Part::HidControllerAxes)) {
            places.HidAxes = d.HidAxes;
            places.HidAxisCount = d.HidAxisCount;
            places.HidAxisValue = d.HidAxisValue;
            places.HalfAxes = { reinterpret_cast<uint16_t const*>((uintptr_t)d.ControllerAxes), d.ControllerAxisCount };
        }
        gTracker = new ControllerTracker(places);
    }
    gTracker->SetHidAxes(hid);
    gTracker->SetSink(sink);

    struct Planned
    {
        uint64_t Address, Bytes;
        void const* Replacement;
        void** Original;
        EntryHook** Hook;
        char const* Name;
    };
    Planned planned[] = {
        { d.InjectInput, d.InjectInputBytes, reinterpret_cast<void const*>(&InjectInput), reinterpret_cast<void**>(&gInjectInput), &gInjectHook,
          "InjectInput" },
        { d.RegisterDevice, d.RegisterDeviceBytes, reinterpret_cast<void const*>(&RegisterDevice), reinterpret_cast<void**>(&gRegisterDevice),
          &gRegisterHook, "RegisterDevice" },
        { d.UnregisterDevice, d.UnregisterDeviceBytes, reinterpret_cast<void const*>(&UnregisterDevice),
          reinterpret_cast<void**>(&gUnregisterDevice), &gUnregisterHook, "UnregisterDevice" },
        { d.OpenController, d.OpenControllerBytes, reinterpret_cast<void const*>(&OpenController), reinterpret_cast<void**>(&gOpenController),
          &gOpenHook, "OpenController" },
        { d.UpdateControls, d.UpdateControlsBytes, reinterpret_cast<void const*>(&UpdateControls), reinterpret_cast<void**>(&gUpdateControls),
          &gUpdateHook, "UpdateControls" },
        { d.HidUpdateControls, d.HidUpdateControlsBytes, reinterpret_cast<void const*>(&HidUpdateControls),
          reinterpret_cast<void**>(&gHidUpdateControls), &gHidUpdateHook, "HIDInputDevice::UpdateControls" },
    };
    std::vector<EntryHook*> hooks;
    for (auto& p : planned) {
        if (p.Hook == &gHidUpdateHook && !hid) continue;
        std::string problem;
        *p.Hook = PrepareEntryHook(reinterpret_cast<void*>((uintptr_t)p.Address), (size_t)p.Bytes, p.Replacement, p.Original, problem);
        if (!*p.Hook) return std::string(p.Name) + " cannot be hooked: " + problem;
        hooks.push_back(*p.Hook);
    }
    std::string problem = CommitEntryHooks(hooks);
    if (!problem.empty()) {
        for (auto hook : hooks) {
            if (hook->Committed()) RemoveEntryHook(hook);
        }
        return "the controller hooks cannot go in: " + problem;
    }
    gInstalled = true;
    gHidInstalled = hid;
    return {};
}

void RemoveControllerHooks()
{
    if (!gInstalled) return;
    gInstalled = false;
    gHidInstalled = false;
    for (auto hook : { gHidUpdateHook, gUpdateHook, gOpenHook, gUnregisterHook, gRegisterHook, gInjectHook }) {
        if (hook && hook->Committed()) RemoveEntryHook(hook);
    }
    gTracker->SetSink(nullptr);
}

bool ControllerHooksInstalled()
{
    return gInstalled;
}

bool HidAxesInstalled()
{
    return gHidInstalled;
}

bool FirstGamepad(GamepadState& out)
{
    return gInstalled && gTracker && gTracker->FirstGamepad(out);
}

}
