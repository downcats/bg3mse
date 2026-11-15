#include <Overlay/Controller.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bg3se::mac::overlay {

namespace {

struct GameButton
{
    uint16_t Input;
    SDL_GameControllerButton Button;
};

constexpr GameButton kGameButtons[] = {
    { kGameButtonA, SDL_CONTROLLER_BUTTON_A },
    { kGameButtonB, SDL_CONTROLLER_BUTTON_B },
    { kGameButtonX, SDL_CONTROLLER_BUTTON_X },
    { kGameButtonY, SDL_CONTROLLER_BUTTON_Y },
    { kGameButtonBack, SDL_CONTROLLER_BUTTON_BACK },
    { kGameButtonGuide, SDL_CONTROLLER_BUTTON_GUIDE },
    { kGameButtonStart, SDL_CONTROLLER_BUTTON_START },
    { kGameButtonLeftStick, SDL_CONTROLLER_BUTTON_LEFTSTICK },
    { kGameButtonRightStick, SDL_CONTROLLER_BUTTON_RIGHTSTICK },
    { kGameButtonLeftShoulder, SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
    { kGameButtonRightShoulder, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
    { kGameButtonDpadUp, SDL_CONTROLLER_BUTTON_DPAD_UP },
    { kGameButtonDpadDown, SDL_CONTROLLER_BUTTON_DPAD_DOWN },
    { kGameButtonDpadLeft, SDL_CONTROLLER_BUTTON_DPAD_LEFT },
    { kGameButtonDpadRight, SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
};

// The places of s_ControllerButtons that cocoa::MFIInputDevice reads (its
// UpdateControls, and the handler of the menu button its Acquire installs),
// and what CocoaBindingSDM::Init writes there.
struct TableEntry
{
    uint8_t Index;
    uint16_t Input;
};

constexpr TableEntry kTable[] = {
    { 1, kGameButtonDpadUp }, { 2, kGameButtonDpadDown }, { 4, kGameButtonDpadLeft }, { 8, kGameButtonDpadRight },
    { 16, kGameButtonStart }, { 17, kGameButtonBack }, { 18, kGameButtonLeftStick }, { 19, kGameButtonRightStick },
    { 20, kGameButtonLeftShoulder }, { 21, kGameButtonRightShoulder }, { 22, kGameButtonA }, { 23, kGameButtonB },
    { 24, kGameButtonX }, { 25, kGameButtonY }, { 34, kGameButtonBack },
};

// s_ControllerAxes as CocoaBindingSDM::Init writes it: for each axis of a
// HIDInputDevice, in SDL's order of the axes, its half axis below zero and its
// half axis above (InputRawType). A y axis's "above" is the game's up.
constexpr uint16_t kHalfAxes[kMaxHidAxes * 2] = {
    140, 142,       // leftstick_xneg, leftstick_xpos: SDL_CONTROLLER_AXIS_LEFTX
    143, 141,       // leftstick_yneg, leftstick_ypos: SDL_CONTROLLER_AXIS_LEFTY
    146, 149,       // rightstick_xneg, rightstick_xpos: SDL_CONTROLLER_AXIS_RIGHTX
    150, 148,       // rightstick_yneg, rightstick_ypos: SDL_CONTROLLER_AXIS_RIGHTY
    151, 151,       // lefttrigger: SDL_CONTROLLER_AXIS_TRIGGERLEFT
    152, 152,       // righttrigger: SDL_CONTROLLER_AXIS_TRIGGERRIGHT
};
static_assert(SDL_CONTROLLER_AXIS_LEFTX == 0 && SDL_CONTROLLER_AXIS_LEFTY == 1 && SDL_CONTROLLER_AXIS_RIGHTX == 2
    && SDL_CONTROLLER_AXIS_RIGHTY == 3 && SDL_CONTROLLER_AXIS_TRIGGERLEFT == 4 && SDL_CONTROLLER_AXIS_TRIGGERRIGHT == 5
    && SDL_CONTROLLER_AXIS_MAX == kMaxHidAxes);

float Saturate(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

}

SDL_GameControllerButton SdlButtonOfGameInput(uint16_t inputType)
{
    for (auto const& button : kGameButtons) {
        if (button.Input == inputType) return button.Button;
    }
    return SDL_CONTROLLER_BUTTON_INVALID;
}

bool GameQueuesButton(SDL_GameControllerButton button)
{
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_B: case SDL_CONTROLLER_BUTTON_X: case SDL_CONTROLLER_BUTTON_Y:
    case SDL_CONTROLLER_BUTTON_BACK: case SDL_CONTROLLER_BUTTON_START:
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    case SDL_CONTROLLER_BUTTON_DPAD_UP: case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
    case SDL_CONTROLLER_BUTTON_TOUCHPAD:
        return true;
    default:
        return false;
    }
}

int16_t SdlStickValue(float value)
{
    return (int16_t)(std::clamp(value, -1.0f, 1.0f) * 32767);
}

int16_t SdlTriggerValue(float value)
{
    // The joystick's axis, then the controller mapping's 0 .. 32767 of it.
    int raw = (int16_t)((std::clamp(value, 0.0f, 1.0f) * 65535) - 32768);
    float normalized = (float)(raw - SDL_JOYSTICK_AXIS_MIN) / (SDL_JOYSTICK_AXIS_MAX - SDL_JOYSTICK_AXIS_MIN);
    return (int16_t)(int)(normalized * SDL_JOYSTICK_AXIS_MAX);
}

GamepadState SdlStateOf(GamepadReading const& reading)
{
    GamepadState state;
    state.Axes[SDL_CONTROLLER_AXIS_LEFTX] = SdlStickValue(reading.LeftX);
    state.Axes[SDL_CONTROLLER_AXIS_LEFTY] = SdlStickValue(-reading.LeftY);
    state.Axes[SDL_CONTROLLER_AXIS_RIGHTX] = SdlStickValue(reading.RightX);
    state.Axes[SDL_CONTROLLER_AXIS_RIGHTY] = SdlStickValue(-reading.RightY);
    state.Axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = SdlTriggerValue(reading.LeftTrigger);
    state.Axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = SdlTriggerValue(reading.RightTrigger);
    std::copy(std::begin(reading.Buttons), std::end(reading.Buttons), state.Buttons);
    return state;
}

void SdlEventsOfReading(GamepadState const& before, GamepadState const& now, int32_t which, uint32_t ticks, std::vector<SDL_Event>& out)
{
    for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; axis++) {
        if (before.Axes[axis] == now.Axes[axis]) continue;
        SDL_Event event;
        memset(&event, 0, sizeof(event));
        event.type = SDL_CONTROLLERAXISMOTION;
        event.caxis.timestamp = ticks;
        event.caxis.which = which;
        event.caxis.axis = (Uint8)axis;
        event.caxis.value = now.Axes[axis];
        out.push_back(event);
    }
    for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++) {
        if (GameQueuesButton((SDL_GameControllerButton)button) || before.Buttons[button] == now.Buttons[button]) continue;
        SDL_Event event;
        memset(&event, 0, sizeof(event));
        event.type = now.Buttons[button] ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
        event.cbutton.timestamp = ticks;
        event.cbutton.which = which;
        event.cbutton.button = (Uint8)button;
        event.cbutton.state = now.Buttons[button] ? SDL_PRESSED : SDL_RELEASED;
        out.push_back(event);
    }
}

int32_t ControllerNumbering::Arrived(int player, int32_t keep)
{
    if (player < 0 || player >= kControllerPlayers) return -1;
    ids_[player] = keep >= 0 ? keep : next_++;
    here_[player] = true;
    return ids_[player];
}

int32_t ControllerNumbering::Went(int player)
{
    if (player < 0 || player >= kControllerPlayers || !here_[player]) return -1;
    here_[player] = false;
    return ids_[player];
}

int32_t ControllerNumbering::Of(int player)
{
    if (player < 0 || player >= kControllerPlayers) return -1;
    if (ids_[player] < 0) {
        ids_[player] = next_++;
        here_[player] = true;
    }
    return ids_[player];
}

bool ControllerNumbering::Gone(int player) const
{
    return player >= 0 && player < kControllerPlayers && ids_[player] >= 0 && !here_[player];
}

void SdlEventsOfRemoval(GamepadState const& last, int32_t which, uint32_t ticks, std::vector<SDL_Event>& out)
{
    for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; axis++) {
        if (last.Axes[axis] == 0) continue;
        SDL_Event event;
        memset(&event, 0, sizeof(event));
        event.type = SDL_CONTROLLERAXISMOTION;
        event.caxis.timestamp = ticks;
        event.caxis.which = which;
        event.caxis.axis = (Uint8)axis;
        event.caxis.value = 0;
        out.push_back(event);
    }
    for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++) {
        if (!last.Buttons[button]) continue;
        SDL_Event event;
        memset(&event, 0, sizeof(event));
        event.type = SDL_CONTROLLERBUTTONUP;
        event.cbutton.timestamp = ticks;
        event.cbutton.which = which;
        event.cbutton.button = (Uint8)button;
        event.cbutton.state = SDL_RELEASED;
        out.push_back(event);
    }
}

bool SdlButtonEventOf(GameInputChange const& change, ControllerRead* read, int32_t which, uint32_t ticks, SDL_Event& out)
{
    int player = PlayerOfDevice(DeviceOf(change.Input));
    if (player < 0) return false;
    auto button = SdlButtonOfGameInput(InputTypeOf(change.Input));
    if (button == SDL_CONTROLLER_BUTTON_INVALID) return false;
    bool pressed = change.Pressed != 0;
    // The touchpad is read before the options button; both are "back".
    if (button == SDL_CONTROLLER_BUTTON_BACK && read && read->Player == player && read->TouchpadChanged && read->TouchpadPressed == pressed) {
        button = SDL_CONTROLLER_BUTTON_TOUCHPAD;
        read->TouchpadChanged = false;
    }
    memset(&out, 0, sizeof(out));
    out.type = pressed ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    out.cbutton.timestamp = ticks;
    out.cbutton.which = which;
    out.cbutton.button = (Uint8)button;
    out.cbutton.state = pressed ? SDL_PRESSED : SDL_RELEASED;
    return true;
}

int16_t SdlHidStickValue(double value)
{
    if (std::isnan(value)) return 0;
    return (int16_t)std::clamp(value, (double)SDL_JOYSTICK_AXIS_MIN, (double)SDL_JOYSTICK_AXIS_MAX);
}

int16_t SdlHidTriggerValue(double value)
{
    if (std::isnan(value)) return 0;
    return (int16_t)std::clamp(value, 0.0, (double)SDL_JOYSTICK_AXIS_MAX);
}

void SdlAxesOfHid(std::span<double const> values, int16_t (&axes)[SDL_CONTROLLER_AXIS_MAX])
{
    for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; axis++) {
        bool trigger = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        double value = (size_t)axis < values.size() ? values[axis] : 0.0;
        axes[axis] = trigger ? SdlHidTriggerValue(value) : SdlHidStickValue(value);
    }
}

GameTable CheckControllerAxes(std::span<uint16_t const> table, std::string& problem)
{
    if (std::all_of(table.begin(), table.end(), [](uint16_t value) { return value == 0; })) return GameTable::NotYet;
    for (size_t n = 0; n < std::size(kHalfAxes); n++) {
        uint16_t found = n < table.size() ? table[n] : 0;
        if (found == kHalfAxes[n]) continue;
        char text[128];
        snprintf(text, sizeof(text), "half axis %zu is %u, expected %u", n, found, kHalfAxes[n]);
        problem = text;
        return GameTable::Different;
    }
    return GameTable::AsExpected;
}

GameTable CheckControllerButtons(std::span<uint16_t const> table, std::string& problem)
{
    if (std::all_of(table.begin(), table.end(), [](uint16_t value) { return value == 0; })) return GameTable::NotYet;
    for (auto const& entry : kTable) {
        uint16_t found = entry.Index < table.size() ? table[entry.Index] : 0;
        if (found == entry.Input) continue;
        char text[128];
        snprintf(text, sizeof(text), "entry %u is %u, expected %u", entry.Index, found, entry.Input);
        problem = text;
        return GameTable::Different;
    }
    return GameTable::AsExpected;
}

void ApplyGamepad(GamepadState const* state, ImGuiIO& io)
{
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    if (!state) return;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

    auto button = [&](ImGuiKey key, SDL_GameControllerButton which) { io.AddKeyEvent(key, state->Buttons[which]); };
    auto analog = [&](ImGuiKey key, SDL_GameControllerAxis axis, float v0, float v1) {
        float value = Saturate((float)(state->Axes[axis] - v0) / (float)(v1 - v0));
        io.AddKeyAnalogEvent(key, value > 0.1f, value);
    };
    // ImGui_ImplSDL2_UpdateGamepads, in its order and with its dead zone.
    constexpr int kThumbDeadZone = 8000;
    button(ImGuiKey_GamepadStart, SDL_CONTROLLER_BUTTON_START);
    button(ImGuiKey_GamepadBack, SDL_CONTROLLER_BUTTON_BACK);
    button(ImGuiKey_GamepadFaceLeft, SDL_CONTROLLER_BUTTON_X);
    button(ImGuiKey_GamepadFaceRight, SDL_CONTROLLER_BUTTON_B);
    button(ImGuiKey_GamepadFaceUp, SDL_CONTROLLER_BUTTON_Y);
    button(ImGuiKey_GamepadFaceDown, SDL_CONTROLLER_BUTTON_A);
    button(ImGuiKey_GamepadDpadLeft, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    button(ImGuiKey_GamepadDpadRight, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    button(ImGuiKey_GamepadDpadUp, SDL_CONTROLLER_BUTTON_DPAD_UP);
    button(ImGuiKey_GamepadDpadDown, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    button(ImGuiKey_GamepadL1, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    button(ImGuiKey_GamepadR1, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    analog(ImGuiKey_GamepadL2, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 0.0f, 32767);
    analog(ImGuiKey_GamepadR2, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 0.0f, 32767);
    button(ImGuiKey_GamepadL3, SDL_CONTROLLER_BUTTON_LEFTSTICK);
    button(ImGuiKey_GamepadR3, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    analog(ImGuiKey_GamepadLStickLeft, SDL_CONTROLLER_AXIS_LEFTX, -kThumbDeadZone, -32768);
    analog(ImGuiKey_GamepadLStickRight, SDL_CONTROLLER_AXIS_LEFTX, +kThumbDeadZone, +32767);
    analog(ImGuiKey_GamepadLStickUp, SDL_CONTROLLER_AXIS_LEFTY, -kThumbDeadZone, -32768);
    analog(ImGuiKey_GamepadLStickDown, SDL_CONTROLLER_AXIS_LEFTY, +kThumbDeadZone, +32767);
    analog(ImGuiKey_GamepadRStickLeft, SDL_CONTROLLER_AXIS_RIGHTX, -kThumbDeadZone, -32768);
    analog(ImGuiKey_GamepadRStickRight, SDL_CONTROLLER_AXIS_RIGHTX, +kThumbDeadZone, +32767);
    analog(ImGuiKey_GamepadRStickUp, SDL_CONTROLLER_AXIS_RIGHTY, -kThumbDeadZone, -32768);
    analog(ImGuiKey_GamepadRStickDown, SDL_CONTROLLER_AXIS_RIGHTY, +kThumbDeadZone, +32767);
}

}
