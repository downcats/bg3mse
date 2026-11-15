#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <SDL_events.h>
#include <SDL_gamecontroller.h>
#include <imgui.h>

#include <Overlay/GameInput.h>

// Controllers, as plain functions of plain values (the hooks that use them
// are in Overlay/ControllerHooks.h).
//
// On Windows the game reads its controllers through SDL, and Norbyte's code
// gets every SDL controller event from its wrapper of SDL_PollEvent: Lua's
// ControllerButtonInput, which may keep a button from the game, and
// ControllerAxisInput, which is only told; ImGui's SDL backend polls the
// first controller each frame for the overlay's gamepad navigation.
//
// The Mac game has no SDL. It reads a controller of the GameController
// framework in cocoa::MFIInputDevice::UpdateControls() (another kind of HID
// device in cocoa::HIDInputDevice), and queues a change for each element that
// changed through ls::InputManager::InjectInput: a button as its
// InputRawType, pressed or not; a stick as half axes past the game's own dead
// zone. So the two halves are taken where each is what SDL would have given:
//
//   buttons   from the changes the game queues, which a hook of InjectInput
//             sees first and can keep from the game, as SDL.cpp keeps an
//             event from the game's poll
//   axes      from the controller's raw state, read at the start of the
//             game's read of it, in SDL's values; with the buttons the game
//             does not read (the home and share buttons, paddles)
//
// A controller the game reads through IOHID (cocoa::HIDInputDevice) has no
// GameController state: its axes are the values the game's IOHID value
// callback stored, calibrated by IOHID to the ranges the game's Acquire set
// (a stick -32768 .. 32767, a trigger 0 .. 32767, or a device's wider
// override), read at the start of the game's read of the device.
//
// Each event carries the controller's SDL joystick instance id, as SDL gives
// it (SDL_joystick.c, SDL_GetNextJoystickInstanceID): 0 for the first
// controller that arrives, one more for each after it, a new one for a
// controller that comes back. A controller arrives when the game registers
// its device, and goes when the game unregisters it.
//
// The game's InputRawType values are Norbyte's (GameDefinitions/
// Enumerations/Engine.inl); the game's tables of them are compared with the
// values below before a change is told apart by them.

namespace bg3se::mac::overlay {

// The InputRawType of each controller button the game queues.
inline constexpr uint16_t kGameButtonA = 153, kGameButtonB = 154, kGameButtonX = 155, kGameButtonY = 156, kGameButtonBack = 157,
    kGameButtonGuide = 158, kGameButtonStart = 159, kGameButtonLeftStick = 160, kGameButtonRightStick = 161,
    kGameButtonLeftShoulder = 162, kGameButtonRightShoulder = 163, kGameButtonDpadUp = 164, kGameButtonDpadDown = 165,
    kGameButtonDpadLeft = 166, kGameButtonDpadRight = 167;

// An input id: the InputRawType in the low half, the device in the high.
// The controllers are devices 3 (player 0) to 10.
inline constexpr uint32_t kFirstControllerDevice = 3, kLastControllerDevice = 10;
inline constexpr int kControllerPlayers = kLastControllerDevice - kFirstControllerDevice + 1;
inline uint32_t DeviceOf(uint32_t input) { return input >> 16; }
inline uint16_t InputTypeOf(uint32_t input) { return (uint16_t)input; }
inline bool IsControllerDevice(uint32_t device) { return device >= kFirstControllerDevice && device <= kLastControllerDevice; }
// The player index of a controller's device id, -1 for another device.
inline int PlayerOfDevice(uint32_t device) { return IsControllerDevice(device) ? (int)(device - kFirstControllerDevice) : -1; }

// SDL's joystick instance ids of the controllers the game has, by player
// index. Not locked: its owner locks it.
class ControllerNumbering
{
public:
    // The game registered a controller at `player`: the next id is its, or
    // `keep` (the id of the same controller, closed and opened again at once,
    // which SDL never sees go).
    int32_t Arrived(int player, int32_t keep = -1);
    // The game unregistered the one at `player`: its id, which is not given
    // again; -1 when there was none.
    int32_t Went(int player);
    // The id of the controller at `player`; of one that went, the id it had.
    // One the game had before the numbering watched it gets the next id now.
    // -1 for no player index.
    int32_t Of(int player);
    // Whether the controller at `player` went and none has come since.
    bool Gone(int player) const;

private:
    int32_t next_{ 0 };
    int32_t ids_[kControllerPlayers]{ -1, -1, -1, -1, -1, -1, -1, -1 };
    bool here_[kControllerPlayers]{};
};

// The SDL button of a button the game queues, or SDL_CONTROLLER_BUTTON_INVALID.
// The game queues both the options and the touchpad button as "back".
SDL_GameControllerButton SdlButtonOfGameInput(uint16_t inputType);

// Whether the game reads the button and queues its changes; the events of
// the others come from the raw state.
bool GameQueuesButton(SDL_GameControllerButton button);

// The state of a GameController extended gamepad, as the framework gives it.
struct GamepadReading
{
    float LeftX{ 0 }, LeftY{ 0 }, RightX{ 0 }, RightY{ 0 };     // -1 .. 1, up positive
    float LeftTrigger{ 0 }, RightTrigger{ 0 };                  // 0 .. 1
    bool Buttons[SDL_CONTROLLER_BUTTON_MAX]{};                  // by the SDL button each one is
};

// A controller's state in SDL's values: what SDL_GameControllerGetAxis and
// SDL_GameControllerGetButton would say.
struct GamepadState
{
    int16_t Axes[SDL_CONTROLLER_AXIS_MAX]{};
    bool Buttons[SDL_CONTROLLER_BUTTON_MAX]{};
};

// SDL's values of the framework's (SDL's own GameController joystick driver,
// SDL_mfijoystick.m, and its controller mapping, SDL_gamecontroller.c): a
// stick axis times 32767, cut toward zero, y negated (SDL's runs down); a
// trigger as the joystick's full range mapped onto 0 .. 32767.
int16_t SdlStickValue(float value);
int16_t SdlTriggerValue(float value);
GamepadState SdlStateOf(GamepadReading const& reading);

// The SDL events SDL would post for a controller going from `before` to
// `now`: an axis motion for each axis whose value changed, and a button
// event for each changed button the game does not queue itself. `which` is
// the controller's instance id.
void SdlEventsOfReading(GamepadState const& before, GamepadState const& now, int32_t which, uint32_t ticks, std::vector<SDL_Event>& out);
// The SDL events SDL posts for a controller that goes (SDL_joystick.c,
// SDL_PrivateJoystickRemoved and SDL_PrivateJoystickForceRecentering): every
// axis that is not centred back to 0, every button held let go.
void SdlEventsOfRemoval(GamepadState const& last, int32_t which, uint32_t ticks, std::vector<SDL_Event>& out);

// What one read of a controller by the game changed of the two buttons it
// queues as one: the change it queues first for "back" is the touchpad's,
// when the touchpad's state changed in this read.
struct ControllerRead
{
    int32_t Player{ -1 };
    bool TouchpadChanged{ false };
    bool TouchpadPressed{ false };
};

// The SDL button event for a change the game queues, false when the change
// is not of a controller button. `read` is the read of the controller the
// change comes from, when the change is queued while the game reads one, or
// null; it is updated with what the change used of it. `which` is the
// instance id of the controller at the change's player index.
bool SdlButtonEventOf(GameInputChange const& change, ControllerRead* read, int32_t which, uint32_t ticks, SDL_Event& out);

// --- IOHID controllers

// How many axes a cocoa::HIDInputDevice has at most: two half axes of
// s_ControllerAxes each, in SDL's order of the axes.
inline constexpr int kMaxHidAxes = 6;

// SDL's value of an axis value the game's IOHID callback stored: cut toward
// zero, and held to SDL's range (a device's override of the calibration can
// go past it, the game holds it the same way).
int16_t SdlHidStickValue(double value);
int16_t SdlHidTriggerValue(double value);

// SDL's axes of the values of a device's axes, by their index; an axis the
// device does not have (past the end of `values`) is centred, as SDL's are.
void SdlAxesOfHid(std::span<double const> values, int16_t (&axes)[SDL_CONTROLLER_AXIS_MAX]);

// The game's table of controller buttons, cocoa::CocoaInputTranslator::
// s_ControllerButtons, against the InputRawType values above at the places
// cocoa::MFIInputDevice::UpdateControls reads it at.
enum class GameTable : uint8_t
{
    NotYet,         // all zero: the game has not set up its input
    AsExpected,
    Different,      // `problem` says where
};
GameTable CheckControllerButtons(std::span<uint16_t const> table, std::string& problem);
// The game's table of half axes, cocoa::CocoaInputTranslator::s_ControllerAxes,
// against the order of SDL's axes: the half axes of axis n at 2n and 2n + 1.
GameTable CheckControllerAxes(std::span<uint16_t const> table, std::string& problem);

// The overlay's gamepad navigation, as ImGui_ImplSDL2_UpdateGamepads makes
// it for the first controller: ImGui's gamepad keys from `state`, or no
// gamepad when it is null. The caller owns the ImGui context.
void ApplyGamepad(GamepadState const* state, ImGuiIO& io);

}
