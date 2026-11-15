#pragma once

#include <span>
#include <string>

#include <SDL_events.h>

#include <Overlay/Controller.h>
#include <Overlay/Dependencies.h>

// Where the overlay meets the game's controllers (Overlay/Controller.h says
// what is taken where, and why).
//
// cocoa::MFIInputDevice::UpdateControls() is hooked at its entry: the hook
// reads the device's GCController before the game does, posts the SDL events
// of what changed that the game does not queue itself (the axes, the buttons
// it does not read), keeps the state for the overlay's navigation, and runs
// the game's function. cocoa::HIDInputDevice::UpdateControls() is hooked the
// same way for the axes of a controller the game reads through IOHID.
//
// ls::InputManager::InjectInput(ls::InputRawChange const&, bool) is hooked at
// its entry: a change of a controller button, from that function or from any
// other reader of the game's (its other HID devices, the menu button's
// handler), becomes the SDL button event it is, and is queued only when the
// sink lets the game have it. Every other change goes to the game's function
// untouched.
//
// ls::InputManager::RegisterDevice(ls::InputDevice const&) and
// UnregisterDevice(ls::EInputDeviceID) are hooked at their entries: a
// controller's device registered is a controller that arrived, and gets the
// next SDL instance id; one unregistered went. cocoa::CocoaWindowManager::
// OpenController(GCController*) is hooked around the game's function: a
// GCController it is told of again while it has it open (the connection
// notification of a controller it opened at launch) it closes and opens
// again, which SDL ignores (SDL_mfijoystick.m, IOS_AddJoystickDevice): the
// controller keeps its id.
//
// The reads run on the thread the game reads its controllers on, InjectInput
// also on every thread that queues an input, the registrations on the thread
// the game opens and closes its controllers on.

namespace bg3se::mac::overlay {

class ControllerSink
{
public:
    virtual ~ControllerSink() = default;
    // An SDL controller event, before the game has what it is of. True: the
    // game gets it. Only a button's answer is acted on.
    virtual bool OnControllerEvent(SDL_Event const& event) = 0;
};

// The places the hooks read, as the game's code gave them.
struct ControllerPlaces
{
    // Both kinds of device (cocoa::MFIInputDevice, cocoa::HIDInputDevice):
    uint32_t Acquired{ 0 };         // acquired, a byte
    uint32_t Player{ 0 };           // the player index, a byte
    // cocoa::MFIInputDevice:
    uint32_t Controller{ 0 };       // its GCController*
    // cocoa::HIDInputDevice; HidAxes 0: its axes are not read.
    uint32_t HidAxes{ 0 };          // its GenData* array,
    uint32_t HidAxisCount{ 0 };     // ... how many it holds (32 bits)
    uint32_t HidAxisValue{ 0 };     // GenData: the axis's value, a double
    // ls::InputDevice:
    uint32_t DeviceId{ 0 };         // its device id, 16 bits
    // The game's tables, compared with what the events are told apart by
    // before they are used.
    std::span<uint16_t const> Buttons;      // s_ControllerButtons
    std::span<uint16_t const> HalfAxes;     // s_ControllerAxes (with HidAxes)
};

// What the hooks do before the game's functions run, on the places the
// game's code gave; the hooks call it, the tests call it on objects of their
// own. One for the process.
class ControllerTracker
{
public:
    explicit ControllerTracker(ControllerPlaces const& places);

    void SetSink(ControllerSink* sink);
    // Whether the axes of IOHID devices are read (when the places have them).
    void SetHidAxes(bool on);

    // The entry of MFIInputDevice::UpdateControls(device): the raw state of
    // the device's controller is read and its events posted. Returns the read
    // the game's function is in while it runs, for the changes it queues.
    ControllerRead BeforeRead(void* device);
    // The entry of HIDInputDevice::UpdateControls(device): its axes.
    ControllerRead BeforeHidRead(void* device);
    // The entry of InjectInput: false when the change is kept from the game.
    bool BeforeInject(GameInputChange const& change, ControllerRead* read);
    // The entries of RegisterDevice and UnregisterDevice. A controller that
    // goes is told as SDL tells it: its axes centred, its buttons let go;
    // not one that is `reopening` (closed by OpenController to be opened
    // again at once), which keeps its id: the id it had is returned, for
    // the registration that follows to `keep`.
    void BeforeRegister(void const* inputDevice, int32_t keep = -1);
    int32_t BeforeUnregister(int32_t deviceId, bool reopening = false);

    // The state of the first controller that was read lately (the lowest
    // player index), for the overlay's navigation: false when none was.
    bool FirstGamepad(GamepadState& out);

private:
    bool TableUsable();
    bool AxesUsable();
    ControllerRead Post(void* device, void* controller, int player, GamepadState now, bool keepButtons);

    ControllerPlaces places_;
    struct Impl;
    Impl* impl_;
};

// Installs the hooks: all of them, or (without the axes of IOHID controllers)
// all but HIDInputDevice::UpdateControls. Empty when they are in; otherwise
// why none is. `sink` must outlive the hooks.
std::string InstallControllerHooks(Dependencies const& dependencies, ControllerSink* sink);
void RemoveControllerHooks();
bool ControllerHooksInstalled();
// Whether the axes of IOHID controllers are read (with the rest).
bool HidAxesInstalled();

// The first controller's state for the overlay's navigation, from the
// installed hooks; false when there is none. Any thread.
bool FirstGamepad(GamepadState& out);

}
