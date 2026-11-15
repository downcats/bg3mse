// Tests of the overlay's input and of the checks of what the overlay depends
// on (Mac/Overlay). No game needed: events are plain values, the class data
// reader runs on the in-memory test binary, and the ImGui context is the
// test's own. The parts that need Objective-C objects are in OverlayTestsObjc.mm.

#include <Overlay/ConsoleWindow.h>
#include <Overlay/ColourSpace.h>
#include <Overlay/Controller.h>
#include <Overlay/Dependencies.h>
#include <Overlay/FontAtlas.h>
#include <Overlay/FrameHandover.h>
#include <Overlay/InputEvents.h>
#include <Overlay/KeyCodes.h>
#include <Overlay/MetalOverlay.h>
#include <Overlay/OverlayState.h>
#include <Overlay/Pointer.h>
#include <Overlay/StateGate.h>
#include <Overlay/TextInput.h>
#include <Engine/Entries/OverlayNames.h>
#include <Platform/CodeShape.h>
#include <Platform/Log.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "FakeBinary.h"
#include "OverlayTests.h"

using namespace bg3se::mac;
using namespace bg3se::mac::overlay;

int gFailures = 0;
char const* gTest = "";

namespace {

bool Contains(std::string const& text, char const* part) { return text.find(part) != std::string::npos; }

// --- controllers ------------------------------------------------------------

GameInputChange ControllerChange(uint32_t player, uint16_t inputType, bool pressed)
{
    GameInputChange change{};
    change.Input = (kFirstControllerDevice + player) << 16 | inputType;
    change.Value[0] = change.Value[1] = pressed ? 1.0f : 0.0f;
    change.Pressed = pressed;
    return change;
}

TEST(a_change_the_game_queues_becomes_the_sdl_button_it_is)
{
    // Player 2's A going down: device 5, of the controller with instance id 9.
    SDL_Event event;
    CHECK(SdlButtonEventOf(ControllerChange(2, kGameButtonA, true), nullptr, 9, 77, event));
    CHECK(event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.which == 9 && event.cbutton.button == SDL_CONTROLLER_BUTTON_A
        && event.cbutton.state == SDL_PRESSED && event.cbutton.timestamp == 77);
    CHECK(SdlButtonEventOf(ControllerChange(0, kGameButtonDpadRight, false), nullptr, 0, 0, event));
    CHECK(event.type == SDL_CONTROLLERBUTTONUP && event.cbutton.which == 0 && event.cbutton.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT
        && event.cbutton.state == SDL_RELEASED);
    CHECK(SdlButtonEventOf(ControllerChange(7, kGameButtonStart, true), nullptr, 3, 0, event) && event.cbutton.which == 3
        && event.cbutton.button == SDL_CONTROLLER_BUTTON_START);
    // Every button the game queues is told apart, and nothing else is a button.
    for (uint16_t input = kGameButtonA; input <= kGameButtonDpadRight; input++) {
        auto button = SdlButtonOfGameInput(input);
        CHECK(button != SDL_CONTROLLER_BUTTON_INVALID && GameQueuesButton(button) == (input != kGameButtonGuide));
    }
    CHECK(SdlButtonOfGameInput(152) == SDL_CONTROLLER_BUTTON_INVALID && SdlButtonOfGameInput(168) == SDL_CONTROLLER_BUTTON_INVALID);
    CHECK(!GameQueuesButton(SDL_CONTROLLER_BUTTON_GUIDE) && !GameQueuesButton(SDL_CONTROLLER_BUTTON_MISC1)
        && !GameQueuesButton(SDL_CONTROLLER_BUTTON_PADDLE1) && !GameQueuesButton(SDL_CONTROLLER_BUTTON_PADDLE4));
    // Not a controller: the keyboard (device 0), the mouse, a ninth player; a half axis of a stick.
    CHECK(!SdlButtonEventOf(ControllerChange(-3, kGameButtonA, true), nullptr, 0, 0, event));
    CHECK(!SdlButtonEventOf(ControllerChange(-1, kGameButtonA, true), nullptr, 0, 0, event));
    CHECK(!SdlButtonEventOf(ControllerChange(8, kGameButtonA, true), nullptr, 0, 0, event));
    CHECK(!SdlButtonEventOf(ControllerChange(0, 168, true), nullptr, 0, 0, event));
    CHECK(DeviceOf(0x50099) == 5 && InputTypeOf(0x50099) == 0x99 && IsControllerDevice(3) && IsControllerDevice(10)
        && !IsControllerDevice(2) && !IsControllerDevice(11));
    CHECK(PlayerOfDevice(3) == 0 && PlayerOfDevice(10) == 7 && PlayerOfDevice(2) == -1 && PlayerOfDevice(11) == -1);
}

TEST(the_first_back_of_a_read_whose_touchpad_changed_is_the_touchpad)
{
    SDL_Event event;
    // The touchpad went down in this read, the options button too: the game
    // queues the touchpad's "back" first.
    ControllerRead read{ 1, true, true };
    CHECK(SdlButtonEventOf(ControllerChange(1, kGameButtonBack, true), &read, 0, 0, event) && event.cbutton.button == SDL_CONTROLLER_BUTTON_TOUCHPAD);
    CHECK(!read.TouchpadChanged);
    CHECK(SdlButtonEventOf(ControllerChange(1, kGameButtonBack, true), &read, 0, 0, event) && event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK);
    // Only the touchpad's own change: the other way, another player, no change.
    read = { 1, true, true };
    CHECK(SdlButtonEventOf(ControllerChange(1, kGameButtonBack, false), &read, 0, 0, event) && event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK);
    CHECK(read.TouchpadChanged);
    CHECK(SdlButtonEventOf(ControllerChange(0, kGameButtonBack, true), &read, 0, 0, event) && event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK);
    read = { 1, false, true };
    CHECK(SdlButtonEventOf(ControllerChange(1, kGameButtonBack, true), &read, 0, 0, event) && event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK);
    // Outside a read (the menu handler, an IOHID device): the options button.
    CHECK(SdlButtonEventOf(ControllerChange(1, kGameButtonBack, true), nullptr, 0, 0, event) && event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK);
}

TEST(sticks_and_triggers_have_sdls_values)
{
    CHECK(SdlStickValue(0) == 0 && SdlStickValue(1) == 32767 && SdlStickValue(-1) == -32767);
    CHECK(SdlStickValue(0.5f) == 16383 && SdlStickValue(-0.5f) == -16383);
    CHECK(SdlStickValue(2) == 32767 && SdlStickValue(-2) == -32767);
    CHECK(SdlTriggerValue(0) == 0 && SdlTriggerValue(1) == 32767 && SdlTriggerValue(0.5f) == 16383);
    CHECK(SdlTriggerValue(-1) == 0 && SdlTriggerValue(3) == 32767);

    GamepadReading reading;
    reading.LeftX = 1;
    reading.LeftY = 1;          // up
    reading.RightX = -1;
    reading.RightY = -0.5f;     // down
    reading.RightTrigger = 1;
    reading.Buttons[SDL_CONTROLLER_BUTTON_GUIDE] = true;
    auto state = SdlStateOf(reading);
    CHECK(state.Axes[SDL_CONTROLLER_AXIS_LEFTX] == 32767 && state.Axes[SDL_CONTROLLER_AXIS_LEFTY] == -32767);
    CHECK(state.Axes[SDL_CONTROLLER_AXIS_RIGHTX] == -32767 && state.Axes[SDL_CONTROLLER_AXIS_RIGHTY] == 16383);
    CHECK(state.Axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] == 0 && state.Axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] == 32767);
    CHECK(state.Buttons[SDL_CONTROLLER_BUTTON_GUIDE] && !state.Buttons[SDL_CONTROLLER_BUTTON_A]);
}

TEST(a_read_posts_the_axes_and_buttons_the_game_does_not_queue)
{
    GamepadState before, now;
    std::vector<SDL_Event> events;
    SdlEventsOfReading(before, now, 0, 0, events);
    CHECK(events.empty());

    now.Axes[SDL_CONTROLLER_AXIS_LEFTY] = -32767;
    now.Axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 100;
    now.Buttons[SDL_CONTROLLER_BUTTON_A] = true;            // the game queues it
    now.Buttons[SDL_CONTROLLER_BUTTON_TOUCHPAD] = true;     // ... and this one, as "back"
    now.Buttons[SDL_CONTROLLER_BUTTON_GUIDE] = true;
    now.Buttons[SDL_CONTROLLER_BUTTON_PADDLE2] = true;
    SdlEventsOfReading(before, now, 3, 42, events);
    CHECK(events.size() == 4);
    if (events.size() == 4) {
        CHECK(events[0].type == SDL_CONTROLLERAXISMOTION && events[0].caxis.axis == SDL_CONTROLLER_AXIS_LEFTY
            && events[0].caxis.value == -32767 && events[0].caxis.which == 3 && events[0].caxis.timestamp == 42);
        CHECK(events[1].type == SDL_CONTROLLERAXISMOTION && events[1].caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT
            && events[1].caxis.value == 100);
        CHECK(events[2].type == SDL_CONTROLLERBUTTONDOWN && events[2].cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE
            && events[2].cbutton.state == SDL_PRESSED && events[2].cbutton.which == 3);
        CHECK(events[3].type == SDL_CONTROLLERBUTTONDOWN && events[3].cbutton.button == SDL_CONTROLLER_BUTTON_PADDLE2);
    }
    // Back: the axis returns to zero, the guide button is let go.
    events.clear();
    before = now;
    now.Axes[SDL_CONTROLLER_AXIS_LEFTY] = 0;
    now.Buttons[SDL_CONTROLLER_BUTTON_GUIDE] = false;
    SdlEventsOfReading(before, now, 3, 0, events);
    CHECK(events.size() == 2 && events[0].caxis.value == 0 && events[1].type == SDL_CONTROLLERBUTTONUP
        && events[1].cbutton.state == SDL_RELEASED);
}

TEST(the_games_controller_buttons_are_compared_before_use)
{
    std::vector<uint16_t> table(0x46 / 2, 0);
    std::string problem;
    CHECK(CheckControllerButtons(table, problem) == GameTable::NotYet);
    uint16_t const expected[][2] = { { 1, 164 }, { 2, 165 }, { 4, 166 }, { 8, 167 }, { 16, 159 }, { 17, 157 }, { 18, 160 }, { 19, 161 },
        { 20, 162 }, { 21, 163 }, { 22, 153 }, { 23, 154 }, { 24, 155 }, { 25, 156 }, { 34, 157 } };
    for (auto const& entry : expected) table[entry[0]] = entry[1];
    table[3] = 999;     // a place the game's reader does not read
    CHECK(CheckControllerButtons(table, problem) == GameTable::AsExpected);
    table[22] = 154;
    CHECK(CheckControllerButtons(table, problem) == GameTable::Different && Contains(problem, "entry 22 is 154, expected 153"));
    table[22] = 153;
    // A table too short for the places read.
    CHECK(CheckControllerButtons(std::span(table).first(30), problem) == GameTable::Different && Contains(problem, "entry 34"));
}

TEST(controllers_are_numbered_as_sdl_numbers_its_joysticks)
{
    // SDL_GetNextJoystickInstanceID: 0 for the first, one more for each
    // after it, never one again.
    ControllerNumbering ids;
    CHECK(ids.Arrived(0) == 0 && ids.Arrived(1) == 1);
    CHECK(ids.Of(0) == 0 && ids.Of(1) == 1 && !ids.Gone(0));
    // The first goes: what comes of it after has its id, and none is given.
    CHECK(ids.Went(0) == 0 && ids.Gone(0) && ids.Of(0) == 0 && ids.Went(0) == -1);
    // The next to arrive takes its player index and a new id.
    CHECK(ids.Arrived(0) == 2 && !ids.Gone(0) && ids.Of(0) == 2 && ids.Of(1) == 1);
    // All go (CloseAllControllers), and come back.
    CHECK(ids.Went(0) == 2 && ids.Went(1) == 1);
    CHECK(ids.Arrived(1) == 3 && ids.Arrived(0) == 4);
    // Closed and opened again at once: the same id.
    CHECK(ids.Went(1) == 3 && ids.Arrived(1, 3) == 3 && ids.Arrived(2) == 5);
    // One the game had before it was watched is numbered when first seen.
    CHECK(!ids.Gone(5) && ids.Of(5) == 6 && ids.Of(5) == 6 && ids.Arrived(3) == 7);
    // No player index: no id, and none used.
    CHECK(ids.Of(-1) == -1 && ids.Of(8) == -1 && ids.Arrived(8) == -1 && ids.Went(8) == -1 && !ids.Gone(8) && ids.Arrived(4) == 8);
}

TEST(a_controller_that_goes_is_centred_and_let_go)
{
    GamepadState last;
    std::vector<SDL_Event> events;
    SdlEventsOfRemoval(last, 4, 9, events);
    CHECK(events.empty());
    last.Axes[SDL_CONTROLLER_AXIS_RIGHTY] = -300;
    last.Axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = 32767;
    last.Buttons[SDL_CONTROLLER_BUTTON_A] = true;
    last.Buttons[SDL_CONTROLLER_BUTTON_GUIDE] = true;
    SdlEventsOfRemoval(last, 4, 9, events);
    CHECK(events.size() == 4);
    if (events.size() == 4) {
        CHECK(events[0].type == SDL_CONTROLLERAXISMOTION && events[0].caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY && events[0].caxis.value == 0
            && events[0].caxis.which == 4 && events[0].caxis.timestamp == 9);
        CHECK(events[1].caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT && events[1].caxis.value == 0);
        // Every button, those the game queues too: SDL lets go of all.
        CHECK(events[2].type == SDL_CONTROLLERBUTTONUP && events[2].cbutton.button == SDL_CONTROLLER_BUTTON_A
            && events[2].cbutton.state == SDL_RELEASED && events[2].cbutton.which == 4);
        CHECK(events[3].cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE);
    }
}

TEST(an_iohid_controllers_axes_have_sdls_values)
{
    // A stick: calibrated to -32768 .. 32767 (y down, as SDL's), cut toward zero.
    CHECK(SdlHidStickValue(0) == 0 && SdlHidStickValue(-0.5) == 0 && SdlHidStickValue(32767) == 32767 && SdlHidStickValue(-32768) == -32768);
    CHECK(SdlHidStickValue(1234.9) == 1234 && SdlHidStickValue(-1234.9) == -1234);
    // A device's override (GamePadDefinitions.plist: +-36000) goes past SDL's range: held to it.
    CHECK(SdlHidStickValue(36000) == 32767 && SdlHidStickValue(-36000) == -32768 && SdlHidStickValue(NAN) == 0);
    // A trigger: 0 .. 32767.
    CHECK(SdlHidTriggerValue(0) == 0 && SdlHidTriggerValue(32767) == 32767 && SdlHidTriggerValue(36000) == 32767);
    CHECK(SdlHidTriggerValue(-5) == 0 && SdlHidTriggerValue(NAN) == 0);

    // By index, in SDL's order; a device with four axes has no triggers.
    double const values[] = { -32768, 100, 32767, -200.5, 16000, 40000 };
    int16_t axes[SDL_CONTROLLER_AXIS_MAX];
    SdlAxesOfHid(values, axes);
    CHECK(axes[SDL_CONTROLLER_AXIS_LEFTX] == -32768 && axes[SDL_CONTROLLER_AXIS_LEFTY] == 100 && axes[SDL_CONTROLLER_AXIS_RIGHTX] == 32767
        && axes[SDL_CONTROLLER_AXIS_RIGHTY] == -200 && axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] == 16000
        && axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] == 32767);
    SdlAxesOfHid(std::span(values).first(4), axes);
    CHECK(axes[SDL_CONTROLLER_AXIS_RIGHTY] == -200 && axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] == 0 && axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] == 0);
    SdlAxesOfHid({}, axes);
    CHECK(axes[SDL_CONTROLLER_AXIS_LEFTX] == 0 && axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] == 0);
}

TEST(the_games_half_axes_are_compared_before_use)
{
    std::vector<uint16_t> table(0x18 / 2, 0);
    std::string problem;
    CHECK(CheckControllerAxes(table, problem) == GameTable::NotYet);
    table = { 140, 142, 143, 141, 146, 149, 150, 148, 151, 151, 152, 152 };
    CHECK(CheckControllerAxes(table, problem) == GameTable::AsExpected);
    // The y half axes the other way round: the axis would be the other sign.
    std::swap(table[2], table[3]);
    CHECK(CheckControllerAxes(table, problem) == GameTable::Different && Contains(problem, "half axis 2 is 141, expected 143"));
    std::swap(table[2], table[3]);
    CHECK(CheckControllerAxes(std::span(table).first(10), problem) == GameTable::Different && Contains(problem, "half axis 10"));
}

// Under the locale a German Mac gives the game, ImGui still writes and reads
// numbers with a point, as the Windows build does in its "C" locale.
TEST(imgui_writes_and_reads_numbers_in_the_c_locale)
{
    std::string before = setlocale(LC_ALL, nullptr);
    CHECK(setlocale(LC_ALL, "de_DE.UTF-8") != nullptr && strcmp(localeconv()->decimal_point, ",") == 0);
    ImGui::CreateContext();
    char text[64];
    float value = 1.5f;
    ImGui::DataTypeFormatString(text, sizeof(text), ImGuiDataType_Float, &value, "%.3f");
    CHECK(strcmp(text, "1.500") == 0);
    ImFormatString(text, sizeof(text), "%g", 0.25);
    CHECK(strcmp(text, "0.25") == 0);
    value = 0;
    CHECK(ImGui::DataTypeApplyFromText("2.25", ImGuiDataType_Float, &value, "%.3f"));
    CHECK(value == 2.25f);
    double wide = 0;
    CHECK(ImGui::DataTypeApplyFromText("-0.125", ImGuiDataType_Double, &wide, "%.3f"));
    CHECK(wide == -0.125);
    ImGui::DestroyContext();
    setlocale(LC_ALL, before.c_str());
}

TEST(the_first_controller_navigates_the_overlay_as_imguis_sdl_backend_does)
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    auto find = [](ImGuiKey key) -> ImGuiInputEvent const* {
        ImGuiInputEvent const* last = nullptr;
        for (auto const& e : GImGui->InputEventsQueue) {
            if (e.Type == ImGuiInputEventType_Key && e.Key.Key == key) last = &e;
        }
        return last;
    };

    ApplyGamepad(nullptr, io);
    CHECK(!(io.BackendFlags & ImGuiBackendFlags_HasGamepad) && GImGui->InputEventsQueue.Size == 0);

    GamepadState state;
    state.Buttons[SDL_CONTROLLER_BUTTON_A] = true;
    state.Axes[SDL_CONTROLLER_AXIS_LEFTX] = 8000;               // the dead zone's edge: nothing
    state.Axes[SDL_CONTROLLER_AXIS_LEFTY] = -32767;             // all the way up
    state.Axes[SDL_CONTROLLER_AXIS_RIGHTX] = 8000 + 2470;       // just short of a tenth of the way past it: not yet down
    state.Axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 32767;
    ApplyGamepad(&state, io);
    CHECK(io.BackendFlags & ImGuiBackendFlags_HasGamepad);
    auto a = find(ImGuiKey_GamepadFaceDown);
    CHECK(a && a->Key.Down);
    // ImGui queues only what changes a key: the ones still up are not there.
    CHECK(!find(ImGuiKey_GamepadFaceRight) && !find(ImGuiKey_GamepadLStickRight) && !find(ImGuiKey_GamepadLStickDown));
    auto up = find(ImGuiKey_GamepadLStickUp);
    CHECK(up && up->Key.Down && std::fabs(up->Key.AnalogValue - (32767.0f - 8000) / (32768 - 8000)) < 1e-6f);
    auto rright = find(ImGuiKey_GamepadRStickRight);
    CHECK(rright && !rright->Key.Down && rright->Key.AnalogValue > 0.09f && rright->Key.AnalogValue < 0.1f);
    auto r2 = find(ImGuiKey_GamepadR2);
    CHECK(r2 && r2->Key.Down && r2->Key.AnalogValue == 1.0f);
    // The controller goes: no gamepad.
    ApplyGamepad(nullptr, io);
    CHECK(!(io.BackendFlags & ImGuiBackendFlags_HasGamepad));
    ImGui::DestroyContext();
}

// --- key codes --------------------------------------------------------------

TEST(key_codes_name_the_keys_of_both_consumers)
{
    CHECK(ImGuiKeyOf(0x00) == ImGuiKey_A && ScancodeOf(0x00) == SDL_SCANCODE_A);
    CHECK(ImGuiKeyOf(0x24) == ImGuiKey_Enter && ScancodeOf(0x24) == SDL_SCANCODE_RETURN);
    CHECK(ImGuiKeyOf(0x33) == ImGuiKey_Backspace && ScancodeOf(0x33) == SDL_SCANCODE_BACKSPACE);
    CHECK(ImGuiKeyOf(0x6f) == ImGuiKey_F12 && ScancodeOf(0x6f) == SDL_SCANCODE_F12);
    CHECK(ImGuiKeyOf(0x7e) == ImGuiKey_UpArrow && ScancodeOf(0x7e) == SDL_SCANCODE_UP);
    CHECK(ImGuiKeyOf(0x3b) == ImGuiKey_LeftCtrl && ScancodeOf(0x3b) == SDL_SCANCODE_LCTRL);
    // Codes without a key.
    CHECK(ImGuiKeyOf(0x34) == ImGuiKey_None && ScancodeOf(0x34) == SDL_SCANCODE_UNKNOWN);
    CHECK(ImGuiKeyOf(0x80) == ImGuiKey_None && ScancodeOf(0xffff) == SDL_SCANCODE_UNKNOWN);
}

TEST(key_codes_and_scancodes_map_both_ways)
{
    std::set<int> scancodes, keys;
    for (uint16_t code = 0; code <= kLastKeyCode; code++) {
        auto scancode = ScancodeOf(code);
        if (scancode == SDL_SCANCODE_UNKNOWN) continue;
        CHECK(KeyCodeOf(scancode) == code);
        CHECK(scancodes.insert(scancode).second);       // no scancode for two keys
        if (auto key = ImGuiKeyOf(code); key != ImGuiKey_None) CHECK(keys.insert(key).second);
    }
    CHECK(scancodes.size() > 100);
    CHECK(KeyCodeOf(SDL_SCANCODE_UNKNOWN) == -1);
    CHECK(KeyCodeOf(SDL_SCANCODE_SYSREQ) == -1);        // no such key on a Mac
}

TEST(keys_are_named_by_the_layout_where_sdl_names_them_so)
{
    // No character from the layout: the key's place on a US keyboard.
    CHECK(SdlKeycodeOf(SDL_SCANCODE_A, 0) == SDLK_a && ImGuiKeyOf(0x00, 0) == ImGuiKey_A);
    CHECK(SdlKeycodeOf(SDL_SCANCODE_1, 0) == SDLK_1 && SdlKeycodeOf(SDL_SCANCODE_0, 0) == SDLK_0);
    CHECK(SdlKeycodeOf(SDL_SCANCODE_RETURN, 0) == SDLK_RETURN && SdlKeycodeOf(SDL_SCANCODE_DELETE, 0) == SDLK_DELETE);
    // Keys SDL gives no character: their scancode's keycode, whatever the layout says.
    CHECK(SdlKeycodeOf(SDL_SCANCODE_F1, 'x') == SDLK_F1 && SdlKeycodeOf(SDL_SCANCODE_KP_1, '1') == SDLK_KP_1);
    CHECK(SdlKeycodeOf(SDL_SCANCODE_LEFT, 0) == SDLK_LEFT && ImGuiKeyOf(0x7b, 0) == ImGuiKey_LeftArrow);
    // A French layout: the key in the place of A types q, of Q types a, of M types a comma.
    CHECK(SdlKeycodeOf(SDL_SCANCODE_A, 'q') == SDLK_q && ImGuiKeyOf(0x00, 'q') == ImGuiKey_Q);
    CHECK(ImGuiKeyOf(0x0c, 'a') == ImGuiKey_A && ImGuiKeyOf(0x2e, ',') == ImGuiKey_Comma);
    // ... the key in the place of 1 types &: by the scancode, which ImGui's backend has no key for.
    CHECK(SdlKeycodeOf(SDL_SCANCODE_1, '&') == '&' && ImGuiKeyOf(0x12, '&') == ImGuiKey_None);
    // ... the key in the place of M (US ;) types m; that of the US comma types ;.
    CHECK(ImGuiKeyOf(0x29, 'm') == ImGuiKey_M && ImGuiKeyOf(0x2b, ';') == ImGuiKey_Semicolon);
    // A character ImGui's backend names nothing by: the place, for the keys it names by place.
    CHECK(ImGuiKeyOf(0x1b, ')') == ImGuiKey_Minus && ImGuiKeyOf(0x32, '<') == ImGuiKey_GraveAccent);
    CHECK(ImGuiKeyOf(0x0a, '@') == ImGuiKey_Oem102 && ImGuiKeyOf(0x0a, 0) == ImGuiKey_Oem102);
    // A letter key typing a letter that is no ASCII one.
    CHECK(SdlKeycodeOf(SDL_SCANCODE_Q, 0xe9) == 0xe9 && ImGuiKeyOf(0x0c, 0xe9) == ImGuiKey_None);
    // The keys SDL has no character of its own for take the layout's, or none.
    CHECK(SdlKeycodeOf(SDL_SCANCODE_NONUSBACKSLASH, '<') == '<' && SdlKeycodeOf(SDL_SCANCODE_NONUSBACKSLASH, 0) == 0);
    CHECK(SdlKeycodeOf(SDL_SCANCODE_INTERNATIONAL3, 0xa5) == 0xa5);

    // The JIS keys.
    CHECK(ScancodeOf(0x5d) == SDL_SCANCODE_INTERNATIONAL3 && ScancodeOf(0x5e) == SDL_SCANCODE_INTERNATIONAL1);
    CHECK(ScancodeOf(0x5f) == SDL_SCANCODE_KP_COMMA && ScancodeOf(0x66) == SDL_SCANCODE_LANG2 && ScancodeOf(0x68) == SDL_SCANCODE_LANG1);
    CHECK(KeyCodeOf(SDL_SCANCODE_LANG1) == 0x68);

    // An ISO keyboard: the key under escape (0x0a) is the grave key's place, the one beside the left shift (0x32) the other.
    CHECK(PositionalKeyCode(0x0a, true) == 0x32 && PositionalKeyCode(0x32, true) == 0x0a && PositionalKeyCode(0x00, true) == 0x00);
    CHECK(PositionalKeyCode(0x0a, false) == 0x0a && PositionalKeyCode(0x32, false) == 0x32);
    CocoaEvent key;
    key.Kind = CocoaEventKind::KeyDown;
    key.KeyCode = 0x0a;
    key.IsoKeyboard = true;
    key.LayoutCharacter = '@';
    SDL_Event sdl;
    CHECK(SdlEventOf(key, 0, sdl) && sdl.key.keysym.scancode == SDL_SCANCODE_GRAVE && sdl.key.keysym.sym == '@');
    std::vector<OverlayInput> inputs;
    OverlayInputsOf(key, inputs);
    CHECK(inputs.size() >= 5 && inputs[4].Key == ImGuiKey_GraveAccent && inputs[4].KeyCode == 0x0a);
    key.IsoKeyboard = false;
    CHECK(SdlEventOf(key, 0, sdl) && sdl.key.keysym.scancode == SDL_SCANCODE_NONUSBACKSLASH);
    // A letter of a French layout, to Lua: the place's scancode, the layout's keycode.
    key.KeyCode = 0x00;
    key.LayoutCharacter = 'q';
    CHECK(SdlEventOf(key, 0, sdl) && sdl.key.keysym.scancode == SDL_SCANCODE_A && sdl.key.keysym.sym == SDLK_q);

    // An injected key's scancode names the same place.
    uint16_t keys[0xb4];
    for (size_t i = 0; i < std::size(keys); i++) keys[i] = (uint16_t)(100 + i);
    SDL_Event injected{};
    injected.type = SDL_KEYDOWN;
    injected.key.keysym.scancode = SDL_SCANCODE_GRAVE;
    GameInputChange change{};
    CHECK(GameChangeOf(injected, keys, true, change) && change.Input == 100 + 0x0a);
    CHECK(GameChangeOf(injected, keys, false, change) && change.Input == 100 + 0x32);
    CHECK(GameChangeOf(injected, keys, change) && change.Input == 100 + 0x32);
}

TEST(modifier_flags_become_sdl_modifiers)
{
    CHECK(SdlModifiersOf(0) == KMOD_NONE);
    CHECK(SdlModifiersOf(kFlagShift | kDeviceLeftShift) == KMOD_LSHIFT);
    CHECK(SdlModifiersOf(kFlagShift | kDeviceRightShift) == KMOD_RSHIFT);
    CHECK(SdlModifiersOf(kFlagShift | kDeviceLeftShift | kDeviceRightShift) == (KMOD_LSHIFT | KMOD_RSHIFT));
    CHECK(SdlModifiersOf(kFlagControl | kDeviceRightControl) == KMOD_RCTRL);
    CHECK(SdlModifiersOf(kFlagOption | kDeviceLeftOption) == KMOD_LALT);
    CHECK(SdlModifiersOf(kFlagCommand | kDeviceRightCommand) == KMOD_RGUI);
    CHECK(SdlModifiersOf(kFlagCapsLock) == KMOD_CAPS);
    // The modifier without a word about which key: the left one.
    CHECK(SdlModifiersOf(kFlagControl) == KMOD_LCTRL);
    // A device bit without its modifier says nothing.
    CHECK(SdlModifiersOf(kDeviceLeftShift) == KMOD_NONE);

    CHECK(ModifierBitOf(0x38) == kDeviceLeftShift && ModifierBitOf(0x3c) == kDeviceRightShift);
    CHECK(ModifierBitOf(0x37) == kDeviceLeftCommand && ModifierBitOf(0x39) == kFlagCapsLock);
    CHECK(ModifierBitOf(0x00) == 0);
}

// --- positions and text -----------------------------------------------------

TEST(view_points_become_drawable_pixels)
{
    float x = -1, y = -1;
    // A view of 1000 x 500 points showing a drawable of 2000 x 1000 pixels.
    ViewGeometry geometry{ 1000, 500, 2000, 1000, false };
    CHECK(ToDrawablePixels(geometry, 0, 500, x, y) && x == 0 && y == 0);            // top left
    CHECK(ToDrawablePixels(geometry, 1000, 0, x, y) && x == 2000 && y == 1000);     // bottom right
    CHECK(ToDrawablePixels(geometry, 250, 400, x, y) && x == 500 && y == 200);
    // A drawable smaller than the view, as when the game renders below the screen's size.
    geometry = { 1000, 500, 500, 250, false };
    CHECK(ToDrawablePixels(geometry, 250, 400, x, y) && x == 125 && y == 50);
    // A flipped view counts down already.
    geometry = { 1000, 500, 1000, 500, true };
    CHECK(ToDrawablePixels(geometry, 250, 400, x, y) && x == 250 && y == 400);
    // Nothing without a size.
    CHECK(!ToDrawablePixels({ 0, 500, 1000, 500, false }, 1, 1, x, y));
    CHECK(!ToDrawablePixels({ 1000, 500, 0, 0, false }, 1, 1, x, y));
}

TEST(text_the_input_system_commits_reaches_imgui_whole)
{
    std::vector<OverlayInput> inputs;
    TextInputsOf("\xc3\xa9", inputs);
    CHECK(inputs.size() == 1 && inputs[0].What == OverlayInput::Kind::Text && !strcmp(inputs[0].Text, "\xc3\xa9"));
    // Longer than one input holds: cut at character boundaries, nothing lost.
    inputs.clear();
    std::string long_text;
    for (int i = 0; i < 20; i++) long_text += "\xe6\xbc\xa2";      // 60 bytes of three-byte characters
    TextInputsOf(long_text.c_str(), inputs);
    std::string joined;
    for (auto const& input : inputs) {
        CHECK(strlen(input.Text) < sizeof(input.Text) && strlen(input.Text) % 3 == 0);
        joined += input.Text;
    }
    CHECK(inputs.size() == 2 && joined == long_text);
    // What is not text is left out; nothing, no input.
    inputs.clear();
    TextInputsOf("a\tb\x7f", inputs);
    CHECK(inputs.size() == 1 && !strcmp(inputs[0].Text, "ab"));
    inputs.clear();
    TextInputsOf("\r", inputs);
    TextInputsOf(nullptr, inputs);
    CHECK(inputs.empty());
}

TEST(a_key_of_the_input_system_is_not_typed_twice)
{
    CocoaEvent key;
    key.Kind = CocoaEventKind::KeyDown;
    key.KeyCode = 0x00;
    strcpy(key.Text, "a");
    std::vector<OverlayInput> inputs;
    // The input system makes its text: the key, no characters.
    key.TextByInputSystem = true;
    OverlayInputsOf(key, inputs);
    CHECK(inputs.size() == 5 && inputs[4].Key == ImGuiKey_A);
    // ... and during a composition the key is the input method's: the modifiers only.
    inputs.clear();
    key.Composing = true;
    OverlayInputsOf(key, inputs);
    CHECK(inputs.size() == 4);
    for (auto const& input : inputs) CHECK(input.What == OverlayInput::Kind::Key && input.KeyCode == -1);
    // Its key up is ImGui's as any other.
    inputs.clear();
    key.Kind = CocoaEventKind::KeyUp;
    OverlayInputsOf(key, inputs);
    CHECK(inputs.size() == 5 && inputs[4].Key == ImGuiKey_A && !inputs[4].Down);
}

TEST(the_input_methods_window_goes_below_imguis_text_cursor)
{
    double x = 0, y = 0, width = 0, height = 0;
    // A view of 1000 x 500 points, a drawable of 2000 x 1000 pixels; the cursor at (400, 100), 30 pixels high.
    ViewGeometry geometry{ 1000, 500, 2000, 1000, false };
    CHECK(ImeRectInView(geometry, 400, 100, 30, x, y, width, height));
    CHECK(x == 200 && y == 500 - 65 && width == 1 && height == 15);
    geometry.Flipped = true;
    CHECK(ImeRectInView(geometry, 400, 100, 30, x, y, width, height) && x == 200 && y == 50 && height == 15);
    CHECK(!ImeRectInView(geometry, 400, 100, -1, x, y, width, height));
    CHECK(!ImeRectInView({ 0, 0, 0, 0, false }, 400, 100, 30, x, y, width, height));
    // The client thread's word for it, as the main thread reads it.
    OverlayState state;
    state.SetIme(12.5f, 300, 18);
    float px = 0, py = 0;
    state.ImePoint(px, py);
    CHECK(px == 12.5f && py == 300 && state.ImeLineHeight.load() == 18);
}

TEST(drawable_pixels_become_view_points_again)
{
    double x = -1, y = -1;
    ViewGeometry geometry{ 1000, 500, 2000, 1000, false };
    CHECK(ToViewPoint(geometry, 0, 0, x, y) && x == 0 && y == 500);
    CHECK(ToViewPoint(geometry, 500, 200, x, y) && x == 250 && y == 400);
    geometry = { 1000, 500, 1000, 500, true };
    CHECK(ToViewPoint(geometry, 250, 400, x, y) && x == 250 && y == 400);
    // There and back, on either kind of view.
    for (bool flipped : { false, true }) {
        geometry = { 1512, 982, 3024, 1964, flipped };
        float px = 0, py = 0;
        CHECK(ToViewPoint(geometry, 1234.5f, 77.25f, x, y) && ToDrawablePixels(geometry, x, y, px, py));
        CHECK(std::fabs(px - 1234.5f) < 1e-3f && std::fabs(py - 77.25f) < 1e-3f);
    }
    CHECK(!ToViewPoint({ 0, 500, 1000, 500, false }, 1, 1, x, y));
    CHECK(!ToViewPoint({ 1000, 500, 0, 500, false }, 1, 1, x, y));
}

TEST(the_pointer_leaving_the_view_leaves_imgui_without_a_position)
{
    CocoaEvent exited;
    exited.Kind = CocoaEventKind::MouseExited;
    std::vector<OverlayInput> inputs;
    OverlayInputsOf(exited, inputs);
    CHECK(inputs.size() == 1 && inputs[0].What == OverlayInput::Kind::MousePosition && inputs[0].X == -FLT_MAX && inputs[0].Y == -FLT_MAX);
    // A drag goes on outside the view: the position stays.
    inputs.clear();
    exited.PressedButtons = 1;
    OverlayInputsOf(exited, inputs);
    CHECK(inputs.empty());
    // Lua is told as SDL tells it; the game always has it.
    SDL_Event sdl;
    CHECK(SdlEventOf(exited, 9, sdl) && sdl.type == SDL_WINDOWEVENT && sdl.window.event == SDL_WINDOWEVENT_LEAVE && sdl.window.timestamp == 9);
    CHECK(!OverlayTakes(exited, { true, true }));
}

TEST(the_overlay_sets_the_cursor_only_while_it_has_the_pointer)
{
    CHECK(NextCursorStep(true, ImGuiMouseCursor_TextInput, false) == CursorStep::Show);
    CHECK(NextCursorStep(true, ImGuiMouseCursor_Arrow, true) == CursorStep::Show);
    CHECK(NextCursorStep(true, ImGuiMouseCursor_None, false) == CursorStep::Hide);
    // Let go: the view's own back, once.
    CHECK(NextCursorStep(false, ImGuiMouseCursor_TextInput, true) == CursorStep::GiveBack);
    CHECK(NextCursorStep(false, ImGuiMouseCursor_TextInput, false) == CursorStep::Nothing);
    // ImGuiConfigFlags_NoMouseCursorChange, or no overlay any more.
    CHECK(NextCursorStep(true, kKeepCursor, true) == CursorStep::GiveBack);
    CHECK(NextCursorStep(true, kKeepCursor, false) == CursorStep::Nothing);

    // The client thread's request to move the pointer, as the main thread reads it.
    OverlayState state;
    uint32_t serial = state.WarpSerial.load();
    state.RequestWarp(812.5f, -3.0f);
    float x = 0, y = 0;
    state.WarpPoint(x, y);
    CHECK(state.WarpSerial.load() == serial + 1 && x == 812.5f && y == -3.0f);
    CHECK(state.MouseCursor.load() == kKeepCursor);
}

TEST(key_down_text_leaves_out_what_is_not_text)
{
    char out[32];
    CHECK(TextOfKeyDown("a", 0, out, sizeof(out)) == 1 && !strcmp(out, "a"));
    CHECK(TextOfKeyDown("\xc3\xa9\xe6\xbc\xa2", 0, out, sizeof(out)) == 5 && !strcmp(out, "\xc3\xa9\xe6\xbc\xa2"));
    // The up arrow's "character" (U+F700), return, escape, delete.
    CHECK(TextOfKeyDown("\xef\x9c\x80", 0, out, sizeof(out)) == 0 && !out[0]);
    CHECK(TextOfKeyDown("\r", 0, out, sizeof(out)) == 0);
    CHECK(TextOfKeyDown("\x1b", 0, out, sizeof(out)) == 0);
    CHECK(TextOfKeyDown("\x7f", 0, out, sizeof(out)) == 0);
    CHECK(TextOfKeyDown("a\rb", 0, out, sizeof(out)) == 2 && !strcmp(out, "ab"));
    // Shortcuts are not text; shift and option make text.
    CHECK(TextOfKeyDown("c", kFlagCommand, out, sizeof(out)) == 0);
    CHECK(TextOfKeyDown("c", kFlagControl, out, sizeof(out)) == 0);
    CHECK(TextOfKeyDown("C", kFlagShift, out, sizeof(out)) == 1);
    CHECK(TextOfKeyDown("\xc3\xa7", kFlagOption, out, sizeof(out)) == 2);
    // Cut at a character boundary, and a sequence cut short is dropped.
    char small[4];
    CHECK(TextOfKeyDown("ab\xc3\xa9", 0, small, sizeof(small)) == 2 && !strcmp(small, "ab"));
    CHECK(TextOfKeyDown("a\xe6\xbc", 0, out, sizeof(out)) == 1 && !strcmp(out, "a"));
    CHECK(TextOfKeyDown(nullptr, 0, out, sizeof(out)) == 0);
}

// --- the overlay's inputs ---------------------------------------------------

CocoaEvent Key(CocoaEventKind kind, uint16_t code, uint32_t flags = 0, char const* text = "")
{
    CocoaEvent event;
    event.Kind = kind;
    event.KeyCode = code;
    event.ModifierFlags = flags;
    strlcpy(event.Text, text, sizeof(event.Text));
    return event;
}

CocoaEvent Mouse(CocoaEventKind kind, float x, float y, int button = 0)
{
    CocoaEvent event;
    event.Kind = kind;
    event.Button = button;
    event.Clicks = 1;
    event.HasPosition = true;
    event.X = x;
    event.Y = y;
    return event;
}

TEST(a_key_becomes_modifiers_the_key_and_its_text)
{
    std::vector<OverlayInput> inputs;
    OverlayInputsOf(Key(CocoaEventKind::KeyDown, 0x00, kFlagShift | kDeviceLeftShift, "A"), inputs);
    CHECK(inputs.size() == 6);
    CHECK(inputs[0].What == OverlayInput::Kind::Key && inputs[0].Key == ImGuiMod_Shift && inputs[0].Down && inputs[0].KeyCode == -1);
    CHECK(inputs[1].Key == ImGuiMod_Ctrl && !inputs[1].Down);
    CHECK(inputs[2].Key == ImGuiMod_Alt && !inputs[2].Down);
    CHECK(inputs[3].Key == ImGuiMod_Super && !inputs[3].Down);
    CHECK(inputs[4].What == OverlayInput::Kind::Key && inputs[4].Key == ImGuiKey_A && inputs[4].Down && inputs[4].KeyCode == 0);
    CHECK(inputs[5].What == OverlayInput::Kind::Text && !strcmp(inputs[5].Text, "A"));

    inputs.clear();
    OverlayInputsOf(Key(CocoaEventKind::KeyUp, 0x00), inputs);
    CHECK(inputs.size() == 5 && inputs[4].Key == ImGuiKey_A && !inputs[4].Down);

    // A held key repeats its text; ImGui repeats the key itself.
    inputs.clear();
    auto repeat = Key(CocoaEventKind::KeyDown, 0x00, 0, "a");
    repeat.Repeat = true;
    OverlayInputsOf(repeat, inputs);
    CHECK(inputs.size() == 5 && inputs[4].What == OverlayInput::Kind::Text);

    // An arrow key has no text; a key ImGui has no name for gives modifiers only.
    inputs.clear();
    OverlayInputsOf(Key(CocoaEventKind::KeyDown, 0x7e, 0, "\xef\x9c\x80"), inputs);
    CHECK(inputs.size() == 5 && inputs[4].Key == ImGuiKey_UpArrow);
    inputs.clear();
    OverlayInputsOf(Key(CocoaEventKind::KeyDown, 0x34), inputs);
    CHECK(inputs.size() == 4);
}

TEST(a_modifier_change_becomes_the_key_that_changed)
{
    std::vector<OverlayInput> inputs;
    OverlayInputsOf(Key(CocoaEventKind::FlagsChanged, 0x38, kFlagShift | kDeviceLeftShift), inputs);
    CHECK(inputs.size() == 5 && inputs[0].Key == ImGuiMod_Shift && inputs[0].Down);
    CHECK(inputs[4].Key == ImGuiKey_LeftShift && inputs[4].Down);
    // The right shift still held when the left one goes up.
    inputs.clear();
    OverlayInputsOf(Key(CocoaEventKind::FlagsChanged, 0x38, kFlagShift | kDeviceRightShift), inputs);
    CHECK(inputs.size() == 5 && inputs[0].Down && inputs[4].Key == ImGuiKey_LeftShift && !inputs[4].Down);
    // Caps lock is a state.
    inputs.clear();
    OverlayInputsOf(Key(CocoaEventKind::FlagsChanged, 0x39, kFlagCapsLock), inputs);
    CHECK(inputs.size() == 4);
}

TEST(mouse_events_become_position_button_and_wheel)
{
    std::vector<OverlayInput> inputs;
    OverlayInputsOf(Mouse(CocoaEventKind::MouseDown, 10, 20, 1), inputs);
    CHECK(inputs.size() == 2);
    CHECK(inputs[0].What == OverlayInput::Kind::MousePosition && inputs[0].X == 10 && inputs[0].Y == 20);
    CHECK(inputs[1].What == OverlayInput::Kind::MouseButton && inputs[1].Button == 1 && inputs[1].Down);

    inputs.clear();
    OverlayInputsOf(Mouse(CocoaEventKind::MouseUp, 10, 20, 7), inputs);     // a button ImGui does not count
    CHECK(inputs.size() == 1 && inputs[0].What == OverlayInput::Kind::MousePosition);

    inputs.clear();
    auto moved = Mouse(CocoaEventKind::MouseMoved, 3, 4);
    OverlayInputsOf(moved, inputs);
    CHECK(inputs.size() == 1);
    moved.HasPosition = false;          // the view is not known
    inputs.clear();
    OverlayInputsOf(moved, inputs);
    CHECK(inputs.empty());

    // A wheel counts lines, a trackpad pixels.
    CocoaEvent scroll;
    scroll.Kind = CocoaEventKind::ScrollWheel;
    scroll.ScrollX = 2;
    scroll.ScrollY = -3;
    inputs.clear();
    OverlayInputsOf(scroll, inputs);
    CHECK(inputs.size() == 1 && inputs[0].What == OverlayInput::Kind::MouseWheel && inputs[0].X == 2 && inputs[0].Y == -3);
    scroll.PreciseScroll = true;
    scroll.ScrollY = 50;
    inputs.clear();
    OverlayInputsOf(scroll, inputs);
    CHECK(inputs.size() == 1 && inputs[0].X == 0.02f && inputs[0].Y == 0.5f);
    scroll.ScrollCancelled = true;
    inputs.clear();
    OverlayInputsOf(scroll, inputs);
    CHECK(inputs.empty());

    inputs.clear();
    OverlayInputsOf(CocoaEvent{}, inputs);
    CHECK(inputs.empty());
}

TEST(the_queue_replays_into_imgui_in_order)
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(200, 100);
    // As with the overlay's renderer: ImGui's textures are taken as they come, none is built first.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    OverlayInputQueue queue;
    std::vector<OverlayInput> inputs;
    OverlayInputsOf(Mouse(CocoaEventKind::MouseDown, 30, 40, 0), inputs);
    OverlayInputsOf(Key(CocoaEventKind::KeyDown, 0x00, 0, "a"), inputs);
    queue.Push(inputs);
    CHECK(GImGui->InputEventsQueue.Size == 0);      // nothing reaches ImGui where the events are taken
    queue.Replay(io);
    CHECK(GImGui->InputEventsQueue.Size > 0);
    ImGui::NewFrame();
    CHECK(io.MousePos.x == 30 && io.MousePos.y == 40);
    CHECK(ImGui::IsMouseDown(ImGuiMouseButton_Left));
    ImGui::EndFrame();
    // ImGui takes a key after a mouse button in the frame after.
    ImGui::NewFrame();
    CHECK(ImGui::IsKeyDown(ImGuiKey_A));
    ImGui::EndFrame();

    // Replayed once.
    queue.Replay(io);
    CHECK(GImGui->InputEventsQueue.Size == 0);
    inputs.clear();
    OverlayInputsOf(Key(CocoaEventKind::KeyUp, 0x00), inputs);
    queue.Push(inputs);
    queue.Clear();
    queue.Replay(io);
    ImGui::NewFrame();
    CHECK(ImGui::IsKeyDown(ImGuiKey_A));            // the cleared key-up never arrived
    ImGui::EndFrame();
    ImGui::DestroyContext();

    // With nothing taking the inputs out, the queue keeps the newest.
    OverlayInputQueue unread;
    for (int i = 0; i < 3000; i++) {
        inputs.clear();
        OverlayInputsOf(Mouse(CocoaEventKind::MouseMoved, (float)i, 0), inputs);
        unread.Push(inputs);
    }
    CHECK(unread.Size() == OverlayInputQueue::kMost);
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().DisplaySize = ImVec2(4000, 100);
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    unread.Replay(ImGui::GetIO());
    CHECK(unread.Size() == 0);
    ImGui::NewFrame();
    CHECK(ImGui::GetIO().MousePos.x == 2999);
    ImGui::EndFrame();
    ImGui::DestroyContext();
}

// --- what the overlay takes from the game -----------------------------------

TEST(the_overlay_takes_keys_for_text_and_the_mouse_it_has)
{
    OverlayWants nothing, text{ true, false }, mouse{ false, true }, both{ true, true };
    auto keyDown = Key(CocoaEventKind::KeyDown, 0x00), keyUp = Key(CocoaEventKind::KeyUp, 0x00);
    auto flags = Key(CocoaEventKind::FlagsChanged, 0x38, kFlagShift | kDeviceLeftShift);
    auto down = Mouse(CocoaEventKind::MouseDown, 1, 1), up = Mouse(CocoaEventKind::MouseUp, 1, 1), moved = Mouse(CocoaEventKind::MouseMoved, 1, 1);
    CocoaEvent scroll;
    scroll.Kind = CocoaEventKind::ScrollWheel;

    for (auto const& event : { keyDown, keyUp, flags, down, up, moved, scroll }) CHECK(!OverlayTakes(event, nothing));
    // Keys while a text field has the keyboard, not while the overlay merely has the mouse.
    CHECK(OverlayTakes(keyDown, text) && OverlayTakes(keyUp, text));
    CHECK(!OverlayTakes(keyDown, mouse) && !OverlayTakes(keyUp, mouse));
    CHECK(!OverlayTakes(down, text) && !OverlayTakes(moved, text) && !OverlayTakes(scroll, text));
    for (auto const& event : { down, up, moved, scroll }) CHECK(OverlayTakes(event, mouse) && OverlayTakes(event, both));
    // A modifier's change always reaches the game; so does a key with Command.
    CHECK(!OverlayTakes(flags, both));
    CHECK(!OverlayTakes(Key(CocoaEventKind::KeyDown, 0x00, kFlagCommand | kDeviceLeftCommand), both));
    CHECK(!OverlayTakes(Key(CocoaEventKind::KeyUp, 0x00, kFlagCommand | kDeviceLeftCommand), both));
    CHECK(OverlayTakes(Key(CocoaEventKind::KeyDown, 0x00, kFlagShift | kDeviceLeftShift), text));
    CHECK(!OverlayTakes(CocoaEvent{}, both));
}

// --- Lua's state, between the thread that replaces it and the main thread ---

TEST(an_event_is_not_handled_on_a_state_that_is_being_replaced)
{
    StateGate gate;

    // An event is being handled on the state: its replacement waits for the handler.
    {
        auto use = gate.Use();
        CHECK(use.owns_lock());
        std::atomic<bool> replaced{ false };
        std::thread client([&] {
            auto replacing = gate.Replace();
            replaced = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(!replaced.load());
        use.unlock();
        client.join();
        CHECK(replaced.load());
    }

    // The state is being replaced: an event does not wait, and has no state to be handled on.
    {
        std::atomic<bool> replacing{ false }, done{ false };
        std::thread client([&] {
            auto hold = gate.Replace();
            replacing = true;
            while (!done) std::this_thread::yield();
        });
        while (!replacing) std::this_thread::yield();
        auto before = std::chrono::steady_clock::now();
        CHECK(!gate.Use().owns_lock());
        CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
        done = true;
        client.join();
        CHECK(gate.Use().owns_lock());
    }

    // A handler that ends the game replaces the state on its own thread: it is not stopped by itself.
    {
        auto use = gate.Use();
        auto replacing = gate.Replace();
        CHECK(use.owns_lock() && replacing.owns_lock());
    }
    CHECK(&ClientStateGate() == &ClientStateGate());
}

// --- what Norbyte's code is given -------------------------------------------

TEST(events_become_the_sdl_events_of_norbytes_code)
{
    SDL_Event sdl;
    auto key = Key(CocoaEventKind::KeyDown, 0x6f, kFlagControl | kDeviceLeftControl);      // Ctrl+F12: the console key
    CHECK(SdlEventOf(key, 1234, sdl));
    CHECK(sdl.type == SDL_KEYDOWN && sdl.key.timestamp == 1234 && sdl.key.state == SDL_PRESSED && sdl.key.repeat == 0);
    CHECK(sdl.key.keysym.scancode == SDL_SCANCODE_F12 && sdl.key.keysym.mod == KMOD_LCTRL);
    CHECK(sdl.key.keysym.sym == SDL_SCANCODE_TO_KEYCODE(SDL_SCANCODE_F12));
    key.Repeat = true;
    CHECK(SdlEventOf(key, 0, sdl) && sdl.key.repeat == 1);
    CHECK(SdlEventOf(Key(CocoaEventKind::KeyUp, 0x00), 0, sdl) && sdl.type == SDL_KEYUP && sdl.key.state == SDL_RELEASED
        && sdl.key.keysym.scancode == SDL_SCANCODE_A);
    CHECK(!SdlEventOf(Key(CocoaEventKind::KeyDown, 0x34), 0, sdl));

    // SDL makes key events of modifier keys.
    CHECK(SdlEventOf(Key(CocoaEventKind::FlagsChanged, 0x3e, kFlagControl | kDeviceRightControl), 0, sdl));
    CHECK(sdl.type == SDL_KEYDOWN && sdl.key.keysym.scancode == SDL_SCANCODE_RCTRL && sdl.key.keysym.mod == KMOD_RCTRL);
    CHECK(SdlEventOf(Key(CocoaEventKind::FlagsChanged, 0x3e, 0), 0, sdl) && sdl.type == SDL_KEYUP);
    CHECK(!SdlEventOf(Key(CocoaEventKind::FlagsChanged, 0x00, 0), 0, sdl));

    auto button = Mouse(CocoaEventKind::MouseDown, 10.4f, 20.6f, 1);
    button.Clicks = 2;
    CHECK(SdlEventOf(button, 7, sdl));
    CHECK(sdl.type == SDL_MOUSEBUTTONDOWN && sdl.button.button == SDL_BUTTON_RIGHT && sdl.button.state == SDL_PRESSED && sdl.button.clicks == 2);
    CHECK(sdl.button.x == 10 && sdl.button.y == 21 && sdl.button.timestamp == 7);
    CHECK(SdlEventOf(Mouse(CocoaEventKind::MouseUp, 0, 0, 0), 0, sdl) && sdl.type == SDL_MOUSEBUTTONUP && sdl.button.button == SDL_BUTTON_LEFT);
    CHECK(SdlEventOf(Mouse(CocoaEventKind::MouseDown, 0, 0, 2), 0, sdl) && sdl.button.button == SDL_BUTTON_MIDDLE);
    CHECK(SdlEventOf(Mouse(CocoaEventKind::MouseDown, 0, 0, 3), 0, sdl) && sdl.button.button == SDL_BUTTON_X1);

    // Moving the mouse is nothing Norbyte's code has a use for.
    CHECK(!SdlEventOf(Mouse(CocoaEventKind::MouseMoved, 1, 1), 0, sdl));

    CocoaEvent scroll;
    scroll.Kind = CocoaEventKind::ScrollWheel;
    scroll.ScrollX = 1;
    scroll.ScrollY = -2;
    CHECK(SdlEventOf(scroll, 0, sdl) && sdl.type == SDL_MOUSEWHEEL);
    CHECK(sdl.wheel.preciseX == -1 && sdl.wheel.preciseY == -2 && sdl.wheel.x == -1 && sdl.wheel.y == -2);
    scroll.ScrollX = scroll.ScrollY = 0;
    CHECK(!SdlEventOf(scroll, 0, sdl));
    scroll.ScrollY = 5;
    scroll.ScrollCancelled = true;
    CHECK(!SdlEventOf(scroll, 0, sdl));

    auto resized = SdlViewportResized(1920, 1080, 9);
    CHECK(resized.type == SDL_WINDOWEVENT && resized.window.event == SDL_WINDOWEVENT_RESIZED && resized.window.data1 == 1920
        && resized.window.data2 == 1080 && resized.window.timestamp == 9);

    uint32_t earlier = InputTicks();
    CHECK(InputTicks() >= earlier);
}

// --- what the game is given -------------------------------------------------

TEST(an_injected_key_becomes_the_change_the_game_queues)
{
    uint16_t keys[0xb4];
    for (size_t i = 0; i < std::size(keys); i++) keys[i] = (uint16_t)(100 + i);
    keys[0x7a] = kNoGameInput;      // F1: say the game has no input for it

    SDL_Event sdl{};
    sdl.type = SDL_KEYDOWN;
    sdl.key.keysym.scancode = SDL_SCANCODE_A;
    GameInputChange change{};
    CHECK(GameChangeOf(sdl, keys, change));
    CHECK(change.Input == 100 && change.Value[0] == 1.0f && change.Value[1] == 1.0f && change.Pressed == 1);
    sdl.type = SDL_KEYUP;
    sdl.key.keysym.scancode = SDL_SCANCODE_F12;
    CHECK(GameChangeOf(sdl, keys, change));
    CHECK(change.Input == 100 + 0x6f && change.Value[0] == 0.0f && change.Value[1] == 0.0f && change.Pressed == 0);
    CHECK(change.Unused[0] == 0 && change.Unused[1] == 0 && change.Unused[2] == 0);

    sdl.key.keysym.scancode = SDL_SCANCODE_F1;
    CHECK(!GameChangeOf(sdl, keys, change));        // no input in the game
    sdl.key.keysym.scancode = SDL_SCANCODE_SYSREQ;
    CHECK(!GameChangeOf(sdl, keys, change));        // no key on a Mac
    sdl.key.keysym.scancode = SDL_SCANCODE_F12;
    CHECK(!GameChangeOf(sdl, { keys, 0x60 }, change));     // a table that ends before the key
    sdl.type = SDL_MOUSEBUTTONDOWN;
    CHECK(!GameChangeOf(sdl, keys, change));        // not a key

    // A table the game has not filled yet says nothing.
    uint16_t empty[0xb4]{};
    sdl.type = SDL_KEYDOWN;
    sdl.key.keysym.scancode = SDL_SCANCODE_A;
    CHECK(!GameChangeOf(sdl, empty, change));
}

TEST(an_injected_keys_modifiers_are_held_around_it)
{
    uint16_t keys[0xb4];
    for (size_t i = 0; i < std::size(keys); i++) keys[i] = (uint16_t)(100 + i);
    SDL_Event sdl{};
    sdl.type = SDL_KEYDOWN;
    sdl.key.keysym.scancode = SDL_SCANCODE_A;
    sdl.key.keysym.mod = KMOD_LSHIFT | KMOD_RCTRL | KMOD_CAPS | KMOD_NUM;
    std::vector<GameInputChange> changes;

    // Left shift (0x38) and right control (0x3e) down, the key, then up, last first.
    GameChangesOfInjected(sdl, keys, KMOD_NONE, true, false, changes);
    CHECK(changes.size() == 5);
    if (changes.size() == 5) {
        CHECK(changes[0].Input == 100 + 0x38 && changes[0].Pressed == 1 && changes[0].Value[0] == 1.0f);
        CHECK(changes[1].Input == 100 + 0x3e && changes[1].Pressed == 1);
        CHECK(changes[2].Input == 100 && changes[2].Pressed == 1);
        CHECK(changes[3].Input == 100 + 0x3e && changes[3].Pressed == 0 && changes[3].Value[0] == 0.0f && changes[3].Value[1] == 0.0f);
        CHECK(changes[4].Input == 100 + 0x38 && changes[4].Pressed == 0);
    }
    // A key going up with its modifiers: the same around it.
    changes.clear();
    sdl.type = SDL_KEYUP;
    GameChangesOfInjected(sdl, keys, KMOD_NONE, true, false, changes);
    CHECK(changes.size() == 5 && changes[2].Input == 100 && changes[2].Pressed == 0 && changes[0].Pressed == 1 && changes[4].Pressed == 0);

    // A modifier the game has been sent as held is not pressed again (nor let go).
    changes.clear();
    sdl.type = SDL_KEYDOWN;
    GameChangesOfInjected(sdl, keys, KMOD_LSHIFT, true, false, changes);
    CHECK(changes.size() == 3 && changes[0].Input == 100 + 0x3e && changes[1].Input == 100 && changes[2].Input == 100 + 0x3e);
    // A modifier key injected with its own modifier is that key alone.
    changes.clear();
    sdl.key.keysym.scancode = SDL_SCANCODE_LSHIFT;
    sdl.key.keysym.mod = KMOD_LSHIFT;
    GameChangesOfInjected(sdl, keys, KMOD_NONE, true, false, changes);
    CHECK(changes.size() == 1 && changes[0].Input == 100 + 0x38);
    // Without the check of the game's modifier keys, the key alone.
    changes.clear();
    sdl.key.keysym.scancode = SDL_SCANCODE_A;
    sdl.key.keysym.mod = KMOD_LGUI;
    GameChangesOfInjected(sdl, keys, KMOD_NONE, false, false, changes);
    CHECK(changes.size() == 1 && changes[0].Input == 100);
    // A modifier the game has no input for is left out; a key it has none for, nothing.
    changes.clear();
    keys[0x37] = kNoGameInput;
    GameChangesOfInjected(sdl, keys, KMOD_NONE, true, false, changes);
    CHECK(changes.size() == 1 && changes[0].Input == 100);
    changes.clear();
    sdl.key.keysym.scancode = SDL_SCANCODE_SYSREQ;
    GameChangesOfInjected(sdl, keys, KMOD_NONE, true, false, changes);
    CHECK(changes.empty());
}

// --- the kinds of target ----------------------------------------------------

TEST(the_overlay_draws_on_the_targets_the_game_presents_in)
{
    constexpr uint64_t kBGRA8Unorm = 80, kBGR10A2Unorm = 94, kRGBA16Float = 115, kRGBA8Unorm = 70;
    // What -[LSMTLView updateHDRState:] gives the layer: Display P3 without
    // extended range, on either of its formats; BT.2020 PQ with it.
    CHECK(ClassifyTarget(kBGRA8Unorm, ColourSpace::DisplayP3, false) == TargetKind::Standard);
    CHECK(ClassifyTarget(kBGR10A2Unorm, ColourSpace::DisplayP3, false) == TargetKind::Standard);
    CHECK(ClassifyTarget(kBGRA8Unorm, ColourSpace::SRGB, false) == TargetKind::Standard);
    CHECK(ClassifyTarget(kBGR10A2Unorm, ColourSpace::Bt2020Pq, true) == TargetKind::HighDynamicRange);
    CHECK(ClassifyTarget(kBGRA8Unorm, ColourSpace::Bt2020Pq, true) == TargetKind::HighDynamicRange);
    // Anything else is refused: an unknown space, a range that disagrees with the space, another format.
    CHECK(ClassifyTarget(kBGRA8Unorm, ColourSpace::Unknown, false) == TargetKind::Unsupported);
    CHECK(ClassifyTarget(kBGR10A2Unorm, ColourSpace::Unknown, true) == TargetKind::Unsupported);
    CHECK(ClassifyTarget(kBGR10A2Unorm, ColourSpace::Bt2020Pq, false) == TargetKind::Unsupported);
    CHECK(ClassifyTarget(kBGRA8Unorm, ColourSpace::DisplayP3, true) == TargetKind::Unsupported);
    CHECK(ClassifyTarget(kRGBA16Float, ColourSpace::DisplayP3, false) == TargetKind::Unsupported);
    CHECK(ClassifyTarget(kRGBA8Unorm, ColourSpace::SRGB, false) == TargetKind::Unsupported);
    TargetFormat format{ kBGR10A2Unorm, ColourSpace::Bt2020Pq, true, TargetKind::HighDynamicRange };
    CHECK(Describe(format) == "MTLPixelFormat 94, BT.2020 PQ, extended range");
}

// --- colours on HDR output ----------------------------------------------------

bool Near(double a, double b, double within) { return std::fabs(a - b) <= within; }

TEST(pq_has_the_values_of_st_2084)
{
    CHECK(PqEncode(0.0) < 1e-6);
    CHECK(Near(PqEncode(1.0), 1.0, 1e-12));
    // 100, 203 (BT.2408's reference white) and 1000 nits.
    CHECK(Near(PqEncode(100.0 / kPqPeakNits), 0.508078, 1e-5));
    CHECK(Near(PqEncode(203.0 / kPqPeakNits), 0.580690, 1e-5));
    CHECK(Near(PqEncode(1000.0 / kPqPeakNits), 0.751827, 1e-5));
    // Out of range is clamped; the curve inverts.
    CHECK(PqEncode(-1.0) < 1e-6 && Near(PqEncode(2.0), 1.0, 1e-12));
    for (double nits : { 0.01, 1.0, 80.0, 203.0, 500.0, 4000.0, 10000.0 }) {
        CHECK(Near(PqDecode(PqEncode(nits / kPqPeakNits)) * kPqPeakNits, nits, nits * 1e-9));
    }
}

TEST(srgb_is_extended_beyond_its_range)
{
    CHECK(Near(SrgbToLinear(1.0), 1.0, 1e-12) && SrgbToLinear(0.0) == 0.0);
    CHECK(Near(SrgbToLinear(0.5), 0.214041, 1e-6));
    for (double v : { -1.5, -0.02, 0.01, 0.04045, 0.3, 0.9, 1.0, 2.5 }) {
        CHECK(Near(LinearToSrgb(SrgbToLinear(v)), v, 1e-7));
    }
    // Mirrored below zero, continued above one.
    CHECK(Near(SrgbToLinear(-0.5), -SrgbToLinear(0.5), 1e-15) && SrgbToLinear(2.0) > 4.0);
}

TEST(the_primaries_are_bt_709_and_bt_2020)
{
    // The matrices invert each other, and white stays white.
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            double product = 0;
            for (int k = 0; k < 3; k++) product += kBt2020ToBt709[i][k] * kBt709ToBt2020[k][j];
            CHECK(Near(product, i == j ? 1.0 : 0.0, 1e-12));
        }
    }
    Rgb white = Multiply(kBt709ToBt2020, { 1, 1, 1 });
    CHECK(Near(white.R, 1, 1e-9) && Near(white.G, 1, 1e-9) && Near(white.B, 1, 1e-9));
    // BT.2087's rounded values.
    Rgb red = Multiply(kBt709ToBt2020, { 1, 0, 0 });
    CHECK(Near(red.R, 0.6274, 1e-4) && Near(red.G, 0.0691, 1e-4) && Near(red.B, 0.0164, 1e-4));
}

TEST(the_overlay_is_put_over_hdr_output_as_over_a_standard_target)
{
    constexpr double kWhite = 250.0;
    double white = PqEncode(kWhite / kPqPeakNits);
    Rgb game{ 0.3, 0.6, 0.45 };
    // Where it covers nothing, the game's pixel is left as it was.
    Rgb kept = CompositeOverPq(game, { 0.9, 0.9, 0.9 }, 0.0, kWhite);
    CHECK(kept.R == game.R && kept.G == game.G && kept.B == game.B);
    // Opaque white is the paper white; opaque grey is grey at the paper white's scale.
    Rgb opaque = CompositeOverPq(game, { 1, 1, 1 }, 1.0, kWhite);
    CHECK(Near(opaque.R, white, 1e-9) && Near(opaque.G, white, 1e-9) && Near(opaque.B, white, 1e-9));
    Rgb grey = CompositeOverPq(game, { 0.5, 0.5, 0.5 }, 1.0, kWhite);
    CHECK(Near(grey.G, PqEncode(SrgbToLinear(0.5) * kWhite / kPqPeakNits), 1e-9));
    // Half covered: the blend of the sRGB values, as a standard target makes
    // it, of the overlay and of the game's pixel seen at the paper white.
    Rgb gameAtWhite{ white, white, white };
    Rgb half = CompositeOverPq(gameAtWhite, { 0.0, 0.0, 0.0 }, 0.5, kWhite);
    CHECK(Near(half.R, PqEncode(SrgbToLinear(0.5) * kWhite / kPqPeakNits), 1e-9));
    // A pixel brighter than the paper white stays brighter under a thin overlay.
    Rgb bright{ PqEncode(0.1), PqEncode(0.1), PqEncode(0.1) };
    Rgb thin = CompositeOverPq(bright, { 0.0, 0.0, 0.0 }, 0.1, kWhite);
    CHECK(thin.R > white && thin.R < bright.R);
    // A blended colour of the game that BT.709 cannot hold comes back as it was.
    Rgb saturated{ 0.0, PqEncode(0.02), 0.0 };
    Rgb under = CompositeOverPq(saturated, { 0.0, 0.0, 0.0 }, 1e-9, kWhite);
    CHECK(Near(under.G, saturated.G, 1e-6) && under.R < 1e-3 && under.B < 1e-3);
}

// CombineUI_HDR (Data/Shaders/Metal/Core.metallib) on an opaque pixel of the
// game's UI, transcribed with the shader's own constants: the sRGB curve,
// BT.709 to BT.2020 to four places, UIPaperWhite, 1e-4, then PQ.
Rgb GameUiOverPq(Rgb ui, double uiPaperWhite)
{
    auto decode = [](double c) { return c <= 0.04045 ? c * 0.0773993805 : std::pow(std::fabs(c + 0.055) * 0.947867334, 2.4); };
    Rgb l{ decode(ui.R), decode(ui.G), decode(ui.B) };
    Rgb wide{ 0.6274 * l.R + 0.3293 * l.G + 0.0433 * l.B, 0.0691 * l.R + 0.9195 * l.G + 0.0114 * l.B,
        0.0164 * l.R + 0.0880 * l.G + 0.8956 * l.B };
    auto encode = [&](double c) {
        double p = std::pow(std::fabs(c * uiPaperWhite * 0.0001), 0.1593017578125);
        return std::pow(std::fabs((p * 18.8515625 + 0.8359375) / (p * 18.6875 + 1.0)), 78.84375);
    };
    return { encode(wide.R), encode(wide.G), encode(wide.B) };
}

TEST(an_opaque_overlay_colour_is_the_games_ui_colour_on_hdr_output)
{
    // The white the game's code gives its UI shader on the build read.
    constexpr double kUiWhite = 300.0;
    constexpr double kHalfCode = 0.5 / 1023.0;
    Rgb game{ 0.2, 0.7, 0.4 };
    for (Rgb colour : { Rgb{ 1, 1, 1 }, Rgb{ 0.5, 0.5, 0.5 }, Rgb{ 0.02, 0.02, 0.02 }, Rgb{ 1, 0, 0 }, Rgb{ 0, 1, 0 }, Rgb{ 0, 0, 1 },
             Rgb{ 0.8, 0.6, 0.1 } }) {
        Rgb overlay = CompositeOverPq(game, colour, 1.0, kUiWhite);
        Rgb ui = GameUiOverPq(colour, kUiWhite);
        CHECK(Near(overlay.R, ui.R, kHalfCode) && Near(overlay.G, ui.G, kHalfCode) && Near(overlay.B, ui.B, kHalfCode));
    }
    // The game's UI white is 300 nits whatever its HDRPaperWhite: at another
    // white the overlay's would not be the UI's.
    Rgb other = CompositeOverPq(game, { 1, 1, 1 }, 1.0, 203.0);
    CHECK(!Near(other.G, GameUiOverPq({ 1, 1, 1 }, kUiWhite).G, kHalfCode));
}

TEST(the_games_ui_white_is_read_from_its_code)
{
    // ls::CombineUIRenderStage::ExecuteWTKernel() on build 4.1.1.7631656, from 0x105d992bc.
    constexpr uint32_t kCode[] = {
        0x7100039f,     // cmp w28, #0
        0x52a872c8,     // mov w8, #0x43960000
        0x1e270101,     // fmov s1, w8
        0x1e2e1000,     // fmov s0, #1.0
        0x1e210c01,     // fcsel s1, s0, s1, eq
        0xbd0002c1,     // str s1, [x22]
        0xf0016ac8,     // adrp x8, ...
        0xf9408d08,     // ldr x8, [x8, #0x118]
        0x35000095,     // cbnz w21, ...
    };
    engine::Operand const* white = nullptr;
    for (auto const& area : DependencyTable()) {
        for (auto const& entry : area.Entries) {
            for (auto const& operand : entry.Operands) {
                if (!strcmp(operand.What, engine::overlay::kUiWhite)) white = &operand;
            }
        }
    }
    CHECK(white != nullptr);
    if (!white) return;
    CHECK(white->How == engine::Operand::Read::MoveValue && white->Pattern.size() == std::size(kCode));
    auto matches = [&](uint32_t const* code) {
        for (size_t i = 0; i < white->Pattern.size(); i++) {
            if (!white->Pattern[i].Matches(code[i])) return false;
        }
        return true;
    };
    CHECK(matches(kCode));
    auto value = MovzValue(kCode[white->At]);
    CHECK(value && *value == white->Expected);
    float nits;
    uint32_t bits = (uint32_t)white->Expected;
    memcpy(&nits, &bits, sizeof(nits));
    CHECK(nits == 300.0f && UsablePaperWhite(nits));

    // Another white is read as it is, and the resolver refuses it against the recorded one.
    uint32_t changed[std::size(kCode)];
    memcpy(changed, kCode, sizeof(kCode));
    changed[1] = 0x52a86948;    // mov w8, #0x434a0000 (202.0)
    CHECK(matches(changed) && MovzValue(changed[white->At]) != white->Expected);
    // Code that does not pick it by the HDR flag, or stores it elsewhere, is not taken for it.
    memcpy(changed, kCode, sizeof(kCode));
    changed[4] = 0x1e211c01;    // fcsel s1, s0, s1, ne
    CHECK(!matches(changed));
    memcpy(changed, kCode, sizeof(kCode));
    changed[5] = 0xbd0006c1;    // str s1, [x22, #4]
    CHECK(!matches(changed));
}

TEST(only_a_paper_white_a_display_can_show_is_used)
{
    CHECK(UsablePaperWhite(80.0f) && UsablePaperWhite(203.0f) && UsablePaperWhite(10000.0f));
    CHECK(!UsablePaperWhite(0.0f) && !UsablePaperWhite(-5.0f) && !UsablePaperWhite(10001.0f));
    CHECK(!UsablePaperWhite(NAN) && !UsablePaperWhite(INFINITY));
}

TEST(the_composite_shader_carries_the_constants)
{
    auto source = CompositeShaderSource();
    CHECK(Contains(source, "composite_vertex") && Contains(source, "composite_fragment") && Contains(source, "[[color(0)]]"));
    CHECK(Contains(source, "kM2 = 78.84375") && Contains(source, "kC3 = 18.6875") && Contains(source, "0.627403896"));

    // The same under the locale a German Mac gives the game.
    std::string before = setlocale(LC_ALL, nullptr);
    CHECK(setlocale(LC_ALL, "de_DE.UTF-8") != nullptr);
    CHECK(CompositeShaderSource() == source);
    setlocale(LC_ALL, before.c_str());
}

// --- the frame handed from the client thread to the main thread -------------

// What a test's "main thread" drew: the slot, or none.
int32_t Drawn(FrameHandover& handover)
{
    int32_t drawn = FrameHandover::kNoFrame;
    handover.Commit([&](int32_t slot) { drawn = slot; });
    return drawn;
}

constexpr auto kMakes = [] { return true; };

TEST(a_finished_frame_is_handed_to_the_main_thread)
{
    OverlayState shared;
    FrameHandover handover(shared);
    int copies = 0;
    auto copy = [&] { copies++; };

    // Nothing is drawn before the renderer has the device and its objects.
    handover.Publish(1, copy);
    CHECK(copies == 1 && Drawn(handover) == FrameHandover::kNoFrame && !shared.Presenting.load());
    CHECK(!handover.Initialize([] { return false; }) && !handover.Initialized());
    CHECK(handover.Initialize([] { return true; }) && handover.Initialized() && !handover.HasObjects());
    CHECK(Drawn(handover) == FrameHandover::kNoFrame);

    // The renderer's objects are made once.
    int made = 0;
    auto make = [&] { made++; return true; };
    CHECK(handover.MakeObjects(make) && handover.HasObjects());
    CHECK(handover.MakeObjects(make) && made == 1);
    CHECK(Drawn(handover) == 1);

    // The overlay is on screen only on a target it draws on.
    CHECK(!shared.Presenting.load() && !handover.TargetDrawable());
    handover.SetTargetDrawable(true);
    CHECK(shared.Presenting.load() && handover.TargetDrawable());

    // The frame published last is the one drawn, every time the game commits.
    handover.Publish(2, copy);
    CHECK(Drawn(handover) == 2 && Drawn(handover) == 2 && shared.Presenting.load());

    // A frame that was withdrawn is not drawn.
    handover.Withdraw();
    CHECK(Drawn(handover) == FrameHandover::kNoFrame && !shared.Presenting.load());
    handover.Publish(1, copy);

    // After the renderer was shut down nothing is drawn, and it can be initialised again.
    bool shutDown = false;
    handover.ShutDown([&] { shutDown = true; });
    CHECK(shutDown && !handover.Initialized() && !handover.HasObjects());
    CHECK(Drawn(handover) == FrameHandover::kNoFrame && !shared.Presenting.load());
    CHECK(handover.Initialize([] { return true; }) && handover.MakeObjects(make));
    CHECK(made == 2 && Drawn(handover) == FrameHandover::kNoFrame);
    handover.Publish(1, copy);
    CHECK(Drawn(handover) == 1 && shared.Presenting.load());

    // Objects that could not be made: nothing is drawn, and they are not
    // tried again until the renderer is initialised again.
    handover.ShutDown([] {});
    CHECK(handover.Initialize([] { return true; }) && !handover.MakeObjects([] { return false; }));
    handover.Publish(2, copy);
    CHECK(Drawn(handover) == FrameHandover::kNoFrame && !handover.HasObjects());
    CHECK(handover.MakeObjects(make) && made == 2 && Drawn(handover) == FrameHandover::kNoFrame);
    handover.ShutDown([] {});
    CHECK(handover.Initialize([] { return true; }) && handover.MakeObjects(make) && made == 3);
    handover.Publish(0, copy);
    CHECK(Drawn(handover) == 0);
}

TEST(an_overlay_that_is_not_drawn_takes_no_input)
{
    OverlayState shared;
    FrameHandover handover(shared);
    auto copy = [] {};
    CHECK(handover.Initialize([] { return true; }));
    handover.SetTargetDrawable(true);

    // A frame is published, and the renderer has no objects yet.
    handover.Publish(0, copy);
    CHECK(Drawn(handover) == FrameHandover::kNoFrame && !shared.Presenting.load());
    // Its objects could not be made: frames go on being published, none is
    // drawn, and the game keeps its mouse and keys.
    CHECK(!handover.MakeObjects([] { return false; }));
    handover.Publish(1, copy);
    CHECK(Drawn(handover) == FrameHandover::kNoFrame && !shared.Presenting.load());

    // The renderer is initialised again and has its objects: the overlay is on screen.
    handover.ShutDown([] {});
    CHECK(handover.Initialize([] { return true; }) && handover.MakeObjects(kMakes));
    CHECK(!shared.Presenting.load());
    handover.Publish(2, copy);
    CHECK(Drawn(handover) == 2 && shared.Presenting.load());
    // ... but not on a target the overlay does not draw on.
    handover.SetTargetDrawable(false);
    CHECK(!shared.Presenting.load());
}

// A draw on the test's "main thread" that stays in progress until it is let go.
struct HeldDraw
{
    std::atomic<bool> Drawing{ false }, LetGo{ false };
    std::thread Thread;

    explicit HeldDraw(FrameHandover& handover)
        : Thread([&] {
              handover.Commit([&](int32_t) {
                  Drawing = true;
                  while (!LetGo) std::this_thread::yield();
              });
          })
    {
        while (!Drawing) std::this_thread::yield();
    }

    void Finish()
    {
        LetGo = true;
        Thread.join();
    }
};

TEST(a_withdrawn_frame_is_not_being_drawn_any_more)
{
    OverlayState shared;
    FrameHandover handover(shared);
    CHECK(handover.Initialize([] { return true; }) && handover.MakeObjects(kMakes));
    handover.Publish(0, [] {});

    // The main thread is drawing the frame when the client thread withdraws
    // it (its objects are about to be freed): the withdrawal waits.
    HeldDraw draw(handover);
    std::atomic<bool> withdrawn{ false };
    std::thread client([&] {
        handover.Withdraw();
        withdrawn = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!withdrawn.load());
    draw.Finish();
    client.join();
    CHECK(withdrawn.load() && Drawn(handover) == FrameHandover::kNoFrame);
}

TEST(the_main_thread_does_not_wait_for_the_client_threads_slow_work)
{
    OverlayState shared;
    FrameHandover handover(shared);
    std::atomic<bool> working{ false }, letGo{ false };
    auto slow = [&] {
        working = true;
        while (!letGo) std::this_thread::yield();
        return true;
    };
    // While `client` is in its slow work, the main thread commits a frame:
    // it returns at once (within two seconds, here), having drawn nothing.
    auto during = [&](std::thread&& client) {
        while (!working) std::this_thread::yield();
        std::atomic<bool> returned{ false };
        int32_t drawn = 99;
        std::thread main([&] {
            drawn = Drawn(handover);
            returned = true;
        });
        for (int waited = 0; !returned && waited < 2000; waited++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(returned.load());
        CHECK(!shared.Presenting.load());
        letGo = true;
        client.join();
        main.join();
        CHECK(drawn == FrameHandover::kNoFrame);
        working = letGo = false;
    };
    CHECK(handover.Initialize([] { return true; }));
    handover.SetTargetDrawable(true);

    // The renderer's objects are being made.
    during(std::thread([&] { CHECK(handover.MakeObjects([&] { return slow(); })); }));
    handover.Publish(0, [] {});
    CHECK(Drawn(handover) == 0 && shared.Presenting.load());

    // The renderer is being shut down and waits for its buffers.
    during(std::thread([&] { handover.ShutDown([&] { slow(); }); }));
    CHECK(!handover.Initialized());

    // The shutdown itself does not start under a draw in progress.
    CHECK(handover.Initialize([] { return true; }) && handover.MakeObjects(kMakes));
    handover.Publish(2, [] {});
    HeldDraw draw(handover);
    std::atomic<bool> shutDown{ false };
    std::thread client([&] { handover.ShutDown([&] { shutDown = true; }); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!shutDown.load());
    draw.Finish();
    client.join();
    CHECK(shutDown.load());
}

// --- the font atlas ---------------------------------------------------------

// A renderer that does what ImGui asks of its textures and nothing else, and
// notes the largest it was asked to make.
struct TextureRequests
{
    int Widest{ 0 }, Highest{ 0 }, Made{ 0 }, Destroyed{ 0 };

    void Serve()
    {
        for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
            if (texture->Status == ImTextureStatus_WantCreate) {
                Widest = std::max(Widest, texture->Width);
                Highest = std::max(Highest, texture->Height);
                Made++;
                texture->SetTexID((ImTextureID)(intptr_t)(Made));
                texture->SetStatus(ImTextureStatus_OK);
            } else if (texture->Status == ImTextureStatus_WantUpdates) {
                texture->SetStatus(ImTextureStatus_OK);
            } else if (texture->Status == ImTextureStatus_WantDestroy) {
                Destroyed++;
                texture->SetTexID(ImTextureID_Invalid);
                texture->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }
};

// Frames that draw ImGui's own font at ever larger sizes, each kept in use:
// more glyphs than a small atlas holds.
void DrawLargeText(TextureRequests& renderer, int frames)
{
    for (int frame = 0; frame < frames; frame++) {
        ImGui::NewFrame();
        auto list = ImGui::GetForegroundDrawList();
        for (int size = 0; size <= frame; size++) {
            list->AddText(nullptr, 96.0f + 24.0f * (float)size, ImVec2(0, 0), IM_COL32_WHITE,
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz 0123456789");
        }
        ImGui::Render();
        renderer.Serve();
    }
}

TEST(the_font_atlas_is_kept_within_a_metal_texture)
{
    // The bound is the largest power of two within the limit, and only ever
    // makes the atlas smaller than it was allowed to be.
    {
        ImFontAtlas atlas;
        int own = atlas.TexMaxWidth;
        CHECK(own == atlas.TexMaxHeight && own >= 4096);
        CHECK(BoundFontAtlas(atlas, 16384) == own && atlas.TexMaxWidth == own && atlas.TexMaxHeight == own);
        CHECK(BoundFontAtlas(atlas, 8192) == std::min(own, 8192));
        CHECK(BoundFontAtlas(atlas, 3000) == 2048 && atlas.TexMaxWidth == 2048 && atlas.TexMaxHeight == 2048);
        CHECK(BoundFontAtlas(atlas, 4096) == 2048);      // not raised again
        // No limit to speak of: nothing is changed.
        CHECK(BoundFontAtlas(atlas, 0) == 0 && BoundFontAtlas(atlas, kLeastAtlasLimit - 1) == 0 && atlas.TexMaxWidth == 2048);
        // At the least, the atlas's smallest texture is the limit too.
        CHECK(BoundFontAtlas(atlas, kLeastAtlasLimit) == kLeastAtlasLimit);
        CHECK(atlas.TexMinWidth <= kLeastAtlasLimit && atlas.TexMinHeight <= kLeastAtlasLimit);
    }

    // What ImGui asks of a renderer when nothing bounds the atlas but its
    // own limit: textures that grow, each larger one in place of the last.
    int freeWidest = 0, freeHighest = 0;
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 720);
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        TextureRequests renderer;
        DrawLargeText(renderer, 12);
        freeWidest = renderer.Widest;
        freeHighest = renderer.Highest;
        CHECK(renderer.Made >= 2 && renderer.Destroyed >= 1 && renderer.Destroyed < renderer.Made);
        ImGui::DestroyContext();
    }
    CHECK(freeWidest >= 2048 && freeHighest >= 2048);
    if (freeWidest < 2048 || freeHighest < 2048) {
        printf("the large text's atlas reached %d x %d\n", freeWidest, freeHighest);
        return;
    }

    // Bounded to half of that, no texture is asked for beyond the bound,
    // whatever is drawn: ImGui drops what it can and leaves out what has no room.
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 720);
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        int limit = std::min(freeWidest, freeHighest) / 2;
        CHECK(BoundFontAtlas(*io.Fonts, limit + limit / 2) == limit);
        TextureRequests renderer;
        DrawLargeText(renderer, 12);
        CHECK(renderer.Made >= 1 && renderer.Widest <= limit && renderer.Highest <= limit);
        CHECK(renderer.Widest == limit || renderer.Highest == limit);
        ImGui::DestroyContext();
    }
}

// --- the class data of a binary on disk -------------------------------------

// A class in the test binary's constant data: read-only data, a list of two
// methods (relative or absolute entries) and one instance variable.
struct FakeClass
{
    static constexpr uint64_t D = FakeBinary::Data;
    static constexpr uint64_t kClass = D + 0x100, kData = D + 0x140, kMethods = D + 0x1c0, kVariables = D + 0x240;
    static constexpr uint64_t kSelector1 = D + 0x280, kSelector2 = D + 0x288, kOffset = D + 0x2a0;
    static constexpr uint64_t kName = D + 0x300, kFirst = D + 0x310, kSecond = D + 0x320, kTypes1 = D + 0x330, kTypes2 = D + 0x350,
        kVariable = D + 0x390, kVariableType = D + 0x3a0;

    FakeBinary Binary;
    GameImage Image;

    template <class T> void Put(uint64_t va, T value) { memcpy(Binary.File.data() + 0x4000 + (va - D), &value, sizeof(T)); }
    void Text(uint64_t va, char const* text) { memcpy(Binary.File.data() + 0x4000 + (va - D), text, strlen(text) + 1); }
    void Relative(uint64_t va, uint64_t target) { Put<int32_t>(va, (int32_t)(int64_t)(target - va)); }

    explicit FakeClass(bool relative, uint64_t dataFlags = 0)
        : Binary(false, { { "_OBJC_CLASS_$_Fake", kClass, N_SECT | N_EXT, 3 }, { "_OBJC_CLASS_$_NoData", D + 0x700, N_SECT | N_EXT, 3 } })
    {
        Put<uint64_t>(kClass + 0x20, kData | dataFlags);
        Put<uint64_t>(kData + 0x18, kName);
        Put<uint64_t>(kData + 0x20, kMethods);
        Put<uint64_t>(kData + 0x30, kVariables);
        Text(kName, "Fake");
        Text(kFirst, "first:");
        Text(kSecond, "second");
        Text(kTypes1, "v24@0:8@16");
        Text(kTypes2, "@16@0:8");
        Put<uint64_t>(kSelector1, kFirst);
        Put<uint64_t>(kSelector2, kSecond);
        if (relative) {
            Put<uint32_t>(kMethods, 0x80000000 | 12);
            Put<uint32_t>(kMethods + 4, 2);
            Relative(kMethods + 8, kSelector1);
            Relative(kMethods + 12, kTypes1);
            Relative(kMethods + 16, FakeBinary::Text);
            Relative(kMethods + 20, kSelector2);
            Relative(kMethods + 24, kTypes2);
            Relative(kMethods + 28, FakeBinary::Text + 0x40);
        } else {
            Put<uint32_t>(kMethods, 24);
            Put<uint32_t>(kMethods + 4, 2);
            Put<uint64_t>(kMethods + 8, kFirst);
            Put<uint64_t>(kMethods + 16, kTypes1);
            Put<uint64_t>(kMethods + 24, FakeBinary::Text);
            Put<uint64_t>(kMethods + 32, kSecond);
            Put<uint64_t>(kMethods + 40, kTypes2);
            Put<uint64_t>(kMethods + 48, FakeBinary::Text + 0x40);
        }
        Put<uint32_t>(kVariables, 32);
        Put<uint32_t>(kVariables + 4, 1);
        Put<uint64_t>(kVariables + 8, kOffset);
        Put<uint64_t>(kVariables + 16, kVariable);
        Put<uint64_t>(kVariables + 24, kVariableType);
        Put<int32_t>(kOffset, 8);
        Text(kVariable, "metalLayer");
        Text(kVariableType, "@\"CAMetalLayer\"");
        CHECK(Image.OpenFile(Binary.File));
    }
};

TEST(class_data_is_read_from_a_file)
{
    for (bool relative : { true, false }) {
        // The low bits of a class's data pointer are flags.
        FakeClass fake(relative, relative ? 1 : 0);
        ObjcClassesOfFile classes(fake.Image);
        CHECK(classes.HasClass("Fake"));
        CHECK(!classes.HasClass("Missing"));
        CHECK(!classes.HasClass("NoData"));         // a symbol whose data does not name the class

        auto first = classes.InstanceMethod("Fake", "first:");
        CHECK(first.Found && first.Own && first.Types == "v24@0:8@16" && first.Implementation == FakeBinary::Text);
        auto second = classes.InstanceMethod("Fake", "second");
        CHECK(second.Found && second.Types == "@16@0:8" && second.Implementation == FakeBinary::Text + 0x40);
        CHECK(!classes.InstanceMethod("Fake", "first").Found);      // not the same selector
        CHECK(!classes.InstanceMethod("Missing", "first:").Found);

        auto variable = classes.InstanceVariable("Fake", "metalLayer");
        CHECK(variable.Found && variable.Type == "@\"CAMetalLayer\"" && variable.Offset == 8);
        CHECK(!classes.InstanceVariable("Fake", "other").Found);

        CHECK(classes.DerivesFrom("Fake", "Fake") == std::optional<bool>(true));
        CHECK(!classes.DerivesFrom("Fake", "NSObject").has_value());        // bound when the game is loaded
        CHECK(classes.DerivesFrom("Missing", "NSObject") == std::optional<bool>(false));
    }
}

TEST(class_data_that_is_not_as_expected_is_refused)
{
    {   // An entry size no method list has.
        FakeClass fake(true);
        fake.Put<uint32_t>(FakeClass::kMethods, 0x80000000 | 16);
        ObjcClassesOfFile classes(fake.Image);
        CHECK(classes.HasClass("Fake") && !classes.InstanceMethod("Fake", "first:").Found);
    }
    {   // A count that runs out of the image.
        FakeClass fake(false);
        fake.Put<uint32_t>(FakeClass::kMethods + 4, 100000);
        ObjcClassesOfFile classes(fake.Image);
        CHECK(!classes.InstanceMethod("Fake", "first:").Found);
    }
    {   // Pointers out of the image.
        FakeClass fake(false);
        fake.Put<uint64_t>(FakeClass::kData + 0x20, 0x200000000ull);
        fake.Put<uint64_t>(FakeClass::kData + 0x30, 0);
        ObjcClassesOfFile classes(fake.Image);
        CHECK(classes.HasClass("Fake"));
        CHECK(!classes.InstanceMethod("Fake", "first:").Found && !classes.InstanceVariable("Fake", "metalLayer").Found);
    }
    {   // A name that never ends.
        FakeClass fake(true);
        memset(fake.Binary.File.data() + 0x4000 + 0x300, 'x', 0x500);
        ObjcClassesOfFile classes(fake.Image);
        CHECK(!classes.HasClass("Fake"));
    }
}

// --- the checks, failing closed ---------------------------------------------

// Classes as a test states them.
struct StatedClasses : ObjcClasses
{
    bool Has{ true };
    ObjcMethod Method;
    ObjcVariable Variable;
    std::optional<bool> Derives;

    bool HasClass(char const*) const override { return Has; }
    ObjcMethod InstanceMethod(char const*, char const*) const override { return Method; }
    ObjcVariable InstanceVariable(char const*, char const*) const override { return Variable; }
    std::optional<bool> DerivesFrom(char const*, char const*) const override { return Derives; }
};

TEST(a_method_is_replaced_only_when_it_is_the_code_that_was_checked)
{
    StatedClasses classes;
    classes.Method = { true, true, 0x1000, kSendEventTypes };
    auto check = CheckReplacedMethod(classes, Part::Events, kBG3Application, kSendEventSelector, kSendEventTypes, 0x1000);
    CHECK(check.Passed && check.Of == Part::Events && check.What == "-[BG3Application sendEvent:]");

    // Another implementation: something else replaced it, or the symbol is another function.
    CHECK(!CheckReplacedMethod(classes, Part::Events, kBG3Application, kSendEventSelector, kSendEventTypes, 0x2000).Passed);
    CHECK(!CheckReplacedMethod(classes, Part::Events, kBG3Application, kSendEventSelector, kSendEventTypes, 0).Passed);
    // Another type encoding: the replacement would take its arguments wrongly.
    check = CheckReplacedMethod(classes, Part::Events, kBG3Application, kSendEventSelector, "v32@0:8@16@24", 0x1000);
    CHECK(!check.Passed && Contains(check.Detail, "type encoding"));
    // Inherited: there is no method of the class's own to replace.
    classes.Method.Own = false;
    check = CheckReplacedMethod(classes, Part::Events, kBG3Application, kSendEventSelector, kSendEventTypes, 0x1000);
    CHECK(!check.Passed && Contains(check.Detail, "inherits"));
    classes.Method = {};
    CHECK(!CheckReplacedMethod(classes, Part::Events, kBG3Application, kSendEventSelector, kSendEventTypes, 0x1000).Passed);
    classes.Has = false;
    check = CheckReplacedMethod(classes, Part::Events, kBG3Application, kSendEventSelector, kSendEventTypes, 0x1000);
    CHECK(!check.Passed && Contains(check.Detail, "no class"));
}

TEST(the_table_holds_what_the_overlay_names)
{
    auto table = DependencyTable();
    CHECK(table.size() == 2);
    std::set<std::string_view> names;
    for (auto const& area : table) {
        for (auto const& entry : area.Entries) names.insert(entry.Name);
    }
    // The nineteen entries of the overlay area and the four of other areas.
    CHECK(table[0].Entries.size() == 19 && table[1].Entries.size() == 4 && names.size() == 23);
    for (auto name : { "rf::metal::MetalRenderFrame", "rf::metal::MetalRenderFrame::FlipBuffers", "rf::metal::MetalRenderer::CommitCommandBuffer",
            "-[BG3Application sendEvent:]", "ls::InputManager::InjectInput", "ls::ResourceManager::InstantiateTexture", "ls__gInputManager",
            "ls__gGlobalResourceManager", "ls__TextureManager__UnloadTexture", "ls__AppliedMaterial__TryOverrideTexture2DParameter",
            "GraphicSettings", "cocoa::MFIInputDevice::UpdateControls", "cocoa::CocoaInputTranslator::s_ControllerButtons", "-[LSMTLView flagsChanged:]",
            "ls::InputManager::RegisterDevice", "ls::InputManager::UnregisterDevice", "cocoa::HIDInputDevice::UpdateControls",
            "cocoa::CocoaWindowManager::OpenController(GCController*)",
            "cocoa::CocoaInputTranslator::s_ControllerAxes", "cocoa::HIDInputDevice::Acquire: the CalibrationMin of a trigger" }) {
        gTest = name;
        CHECK(names.count(name) == 1);
    }
    gTest = "the_table_holds_what_the_overlay_names";
}

TEST(a_binary_without_the_games_code_turns_every_part_off)
{
    // The test binary has none of the game's symbols, and no classes.
    FakeBinary binary;
    GameImage image;
    CHECK(image.OpenFile(binary.File));
    engine::Images images;
    images.Game = { &image, nullptr };
    auto resolution = engine::Resolve(DependencyTable(), images);
    ObjcClassesOfFile classes(image);
    auto dependencies = CheckDependencies(resolution, classes);

    CHECK(!dependencies.Live);
    for (size_t part = 0; part < (size_t)Part::Count; part++) {
        gTest = Describe((Part)part);
        CHECK(!dependencies.Usable((Part)part));
        CHECK(!dependencies.Problem((Part)part).empty());
    }
    gTest = "a_binary_without_the_games_code_turns_every_part_off";
    CHECK(!dependencies.Drawable());
    // Nothing of an unresolved entry can be used: no address, no offset.
    CHECK(!dependencies.FlipBuffersSlot && !dependencies.FlipBuffers && !dependencies.CommitCommandBuffer && !dependencies.SendEvent
        && !dependencies.InjectInput && !dependencies.KeyboardKeys && !dependencies.InstantiateTexture);
    CHECK(!dependencies.UpdateControls && !dependencies.ControllerButtons && !dependencies.DevicePlayer);
    CHECK(!dependencies.RegisterDevice && !dependencies.UnregisterDevice && !dependencies.RegisteredDeviceId && !dependencies.OpenController
        && !dependencies.HidUpdateControls
        && !dependencies.HidAxes && !dependencies.ControllerAxes && !dependencies.TriggerCalibrationMin);
    // No white for HDR output: the overlay is not drawn there.
    CHECK(!dependencies.GraphicSettings && std::isnan(dependencies.UiWhite) && !UsablePaperWhite(dependencies.UiWhite));
    CHECK(!dependencies.FrameDrawable && !dependencies.RendererCommandBuffer && !dependencies.KeyCodes && !dependencies.MetalTexture);
    CHECK(Contains(dependencies.Problem(Part::Frame), "rf::metal::MetalRenderFrame::FlipBuffers"));

    // A part with no checks at all is not usable either.
    Dependencies none;
    CHECK(!none.Usable(Part::Frame) && !none.Drawable() && none.Problem(Part::Frame) == "nothing was checked");
    // One failed check turns its part off, and only its part.
    Dependencies some;
    some.Checks = { { Part::Frame, "a", true, "" }, { Part::Frame, "b", false, "why" }, { Part::Events, "c", true, "" } };
    CHECK(!some.Usable(Part::Frame) && some.Usable(Part::Events) && !some.Drawable());
    CHECK(some.Problem(Part::Frame) == "b: why" && some.Problem(Part::Events).empty());
    some.Checks[1].Passed = true;
    CHECK(some.Drawable());
}

TEST(the_overlay_resolves_nothing_itself)
{
    // The running game's dependencies come from the resolution the start-up
    // sequence made before any hook was on the game. This process was given
    // none: every part is off, and the overlay did not go and read the game
    // (here: this test) for itself.
    auto const& dependencies = GameDependencies();
    for (size_t part = 0; part < (size_t)Part::Count; part++) {
        CHECK(!dependencies.Usable((Part)part));
        CHECK(Contains(dependencies.Problem((Part)part), "was not resolved for this game"));
    }
    CHECK(!dependencies.Drawable() && !dependencies.CommitCommandBuffer && !dependencies.SendEvent);
    // The answer is made once: a resolution that comes too late changes nothing.
    engine::Resolution late;
    SetGameResolution(&late);
    CHECK(&GameDependencies() == &dependencies && !GameDependencies().Drawable());
    SetGameResolution(nullptr);
}

// --- the console window -----------------------------------------------------

TEST(the_console_key_is_named_as_imgui_names_keys)
{
    std::string problem;
    CHECK(ParseKeyChord(kDefaultConsoleKey, problem) == (KeyChord{ 0x61, 0 }));
    CHECK(ParseKeyChord("ctrl+shift+F7", problem) == (KeyChord{ 0x62, kFlagControl | kFlagShift }));
    CHECK(ParseKeyChord(" Option + GraveAccent ", problem) == (KeyChord{ 0x32, kFlagOption }));
    CHECK(DescribeKeyChord(KeyChord{ 0x62, kFlagControl | kFlagShift }) == "Ctrl+Shift+F7");
    CHECK(DescribeKeyChord(*ParseKeyChord("Alt+Keypad0", problem)) == "Alt+Keypad0");
    // What is no key for it.
    CHECK(!ParseKeyChord("Cmd+F6", problem) && Contains(problem, "Command"));
    CHECK(!ParseKeyChord("LeftShift", problem) && Contains(problem, "modifier key"));
    CHECK(!ParseKeyChord("Hyper+F6", problem) && Contains(problem, "no modifier"));
    CHECK(!ParseKeyChord("Banana", problem) && Contains(problem, "not the name of a key"));
    CHECK(!ParseKeyChord("Ctrl+", problem) && Contains(problem, "no key"));
}

TEST(the_console_key_shows_and_hides_the_window_and_goes_nowhere_else)
{
    bg3se::shim::ConsoleScreen screen;
    ConsoleWindow window(screen, KeyChord{ 0x61, kFlagControl });
    // Without a console the key is the game's.
    CHECK(!window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61, kFlagControl), true));
    CHECK(!window.TakeEvent(Key(CocoaEventKind::KeyUp, 0x61, kFlagControl), true));
    CHECK(!window.Visible());

    // Made, the console's window is shown.
    window.ConsoleOpened();
    CHECK(window.Visible());
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61, kFlagControl), true) && !window.Visible());
    // Held, the key goes on being the window's, and so is its release, with Control let go first or not.
    auto held = Key(CocoaEventKind::KeyDown, 0x61, kFlagControl);
    held.Repeat = true;
    CHECK(window.TakeEvent(held, true) && !window.Visible());
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyUp, 0x61, 0), true));
    CHECK(!window.TakeEvent(Key(CocoaEventKind::KeyUp, 0x61, 0), true));
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61, kFlagControl), false) && window.Visible());
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyUp, 0x61, kFlagControl), false));

    // Another key, or the key with other modifiers, is not the window's.
    CHECK(!window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x62, kFlagControl), true));
    CHECK(!window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61, 0), true));
    CHECK(!window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61, kFlagControl | kFlagShift), true));
    CHECK(!window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61, kFlagControl | kFlagCommand), true));
    // Caps lock is no modifier of a chord.
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61, kFlagControl | kFlagCapsLock), true) && !window.Visible());
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyUp, 0x61, kFlagControl | kFlagCapsLock), true));

    window.ConsoleClosed();
    CHECK(!window.Visible());
    CHECK(!window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61, kFlagControl), true));
}

TEST(the_log_says_when_the_console_window_cannot_be_seen)
{
    auto logged = [](std::function<void()> what) {
        FILE* log = tmpfile();
        SetLogStream(log);
        what();
        SetLogStream(stderr);
        std::string text(4096, '\0');
        rewind(log);
        text.resize(fread(text.data(), 1, text.size(), log));
        fclose(log);
        return text;
    };
    bg3se::shim::ConsoleScreen screen;
    // The overlay is off in this game: said when the console is made.
    ConsoleWindow off(screen, KeyChord{ 0x61, 0 }, [] { return std::string("the view's class is not as expected"); });
    auto text = logged([&] { off.ConsoleOpened(); });
    CHECK(Contains(text, "overlay is off in this game (the view's class is not as expected)") && Contains(text, "cannot be seen"));
    // The overlay can be drawn: the key is named.
    ConsoleWindow on(screen, KeyChord{ 0x61, 0 }, [] { return std::string(); });
    text = logged([&] { on.ConsoleOpened(); });
    CHECK(Contains(text, "shown whenever the overlay is drawn; F6 hides it"));
    // It is not drawn right now: said when the key asks for the window, once.
    on.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61), true);
    on.TakeEvent(Key(CocoaEventKind::KeyUp, 0x61), true);
    text = logged([&] {
        on.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61), false);
        on.TakeEvent(Key(CocoaEventKind::KeyUp, 0x61), false);
    });
    CHECK(on.Visible() && Contains(text, "the overlay is not drawn now"));
}

// What the window types into the console, read as Norbyte's console thread
// reads it; nothing when nothing arrives within a second.
std::u32string ReadTyped(bg3se::shim::ConsoleScreen& screen)
{
    std::u32string read;
    std::atomic<bool> done{ false };
    std::thread reader([&] {
        wchar_t got[64];
        size_t taken = 0;
        while (screen.Read(got, 64, taken)) {
            read.append(reinterpret_cast<char32_t*>(got), taken);
            if (read.size() >= 2 && read.substr(read.size() - 2) == U"\r\n") break;
        }
        done = true;
    });
    for (int i = 0; i < 500 && !done; i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (!done) {
        screen.Close();
        reader.join();
        screen.Open();
    } else {
        reader.join();
    }
    return read;
}

TEST(the_console_window_shows_the_screen_and_types_into_it)
{
    bg3se::shim::ConsoleScreen screen;
    screen.Open();
    ConsoleWindow window(screen, KeyChord{ 0x61, 0 });
    auto me = std::this_thread::get_id();
    screen.SetTitle("BG3 Script Extender Debug Console");
    for (int i = 0; i < 500; i++) screen.Write(me, "line " + std::to_string(i) + "\n");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 900);
    // As with the overlay's renderer: ImGui's textures are taken as they come, none is built first.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    // Drawn as the overlay draws it: after NewFrame, from the context's hook.
    struct Hooked { static void Draw(ImGuiContext*, ImGuiContextHook* hook) { static_cast<ConsoleWindow*>(hook->UserData)->Draw(); } };
    ImGuiContextHook hook;
    hook.Type = ImGuiContextHookType_NewFramePost;
    hook.Callback = &Hooked::Draw;
    hook.UserData = &window;
    ImGui::AddContextHook(GImGui, &hook);

    OverlayInputQueue queue;
    std::vector<OverlayInput> inputs;
    auto frame = [&](std::initializer_list<CocoaEvent> events = {}) {
        inputs.clear();
        for (auto const& event : events) OverlayInputsOf(event, inputs);
        queue.Push(inputs);
        queue.Replay(io);
        ImGui::NewFrame();
        ImGui::Render();
    };
    auto consoleWindow = [] { return ImGui::FindWindowByName("BG3 Script Extender Debug Console###bg3mse.console"); };

    // Shown when the console is made, without the keyboard.
    window.ConsoleOpened();
    frame();
    frame();
    CHECK(consoleWindow() && consoleWindow()->Active);
    CHECK(!io.WantTextInput);
    uint16_t columns = 0, rows = 0;
    screen.Size(columns, rows);
    CHECK(columns > 20 && rows > 5);

    // The key shows it with the keyboard.
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61), true));
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyUp, 0x61), true));
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61), true));
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyUp, 0x61), true));
    CHECK(window.Visible());
    // The line is focused in the next frame, and ImGui says so in the one after.
    for (int i = 0; i < 3; i++) frame();
    CHECK(io.WantTextInput);

    // A command and Enter.
    frame({ Key(CocoaEventKind::KeyDown, 0x23, 0, "print(1)") });
    frame({ Key(CocoaEventKind::KeyUp, 0x23) });
    frame({ Key(CocoaEventKind::KeyDown, 0x24) });
    frame({ Key(CocoaEventKind::KeyUp, 0x24) });
    CHECK(ReadTyped(screen) == U"print(1)\r\n");
    // The line keeps the keyboard; Up brings the command back.
    frame();
    CHECK(io.WantTextInput);
    frame({ Key(CocoaEventKind::KeyDown, 0x7e) });
    frame({ Key(CocoaEventKind::KeyUp, 0x7e) });
    frame({ Key(CocoaEventKind::KeyDown, 0x24) });
    frame({ Key(CocoaEventKind::KeyUp, 0x24) });
    CHECK(ReadTyped(screen) == U"print(1)\r\n");
    // Enter on an empty line is a line, as Norbyte's console waits for one.
    // (The line has the keyboard again two frames after a command.)
    frame();
    CHECK(io.WantTextInput);
    frame({ Key(CocoaEventKind::KeyDown, 0x24) });
    frame({ Key(CocoaEventKind::KeyUp, 0x24) });
    CHECK(ReadTyped(screen) == U"\r\n");

    // Hidden, the window gives the keyboard back.
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61), true) && !window.Visible());
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyUp, 0x61), true));
    frame();
    frame();
    CHECK(!io.WantTextInput);
    CHECK(!consoleWindow()->Active);

    // The screen's lines are drawn while the window is, and only those in sight.
    window.ConsoleOpened();
    for (int i = 0; i < 3; i++) frame();
    CHECK(consoleWindow()->Active);
    screen.Clear();
    frame();
    ImGui::DestroyContext();
}


TEST(the_console_window_selects_copies_and_pastes_as_a_console_does)
{
    bg3se::shim::ConsoleScreen screen;
    screen.Open();
    ConsoleWindow window(screen, KeyChord{ 0x61, 0 });
    auto me = std::this_thread::get_id();
    screen.Write(me, "alpha beta\n\x1b[44mgamma\x1b[0m\ndelta\n");

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600, 900);
    // As with the overlay's renderer: ImGui's textures are taken as they come, none is built first.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    struct Hooked { static void Draw(ImGuiContext*, ImGuiContextHook* hook) { static_cast<ConsoleWindow*>(hook->UserData)->Draw(); } };
    ImGuiContextHook hook;
    hook.Type = ImGuiContextHookType_NewFramePost;
    hook.Callback = &Hooked::Draw;
    hook.UserData = &window;
    ImGui::AddContextHook(GImGui, &hook);
    OverlayInputQueue queue;
    std::vector<OverlayInput> inputs;
    auto frame = [&](std::initializer_list<CocoaEvent> events = {}) {
        inputs.clear();
        for (auto const& event : events) OverlayInputsOf(event, inputs);
        queue.Push(inputs);
        queue.Replay(io);
        ImGui::NewFrame();
        ImGui::Render();
    };
    auto press = [&](uint16_t code, uint32_t flags = 0, char const* text = "") {
        frame({ Key(CocoaEventKind::KeyDown, code, flags, text) });
        frame({ Key(CocoaEventKind::KeyUp, code, flags) });
    };
    auto consoleWindow = [] { return ImGui::FindWindowByName("Script Extender Console###bg3mse.console"); };

    window.ConsoleOpened();
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61), true) && window.TakeEvent(Key(CocoaEventKind::KeyUp, 0x61), true));
    CHECK(window.TakeEvent(Key(CocoaEventKind::KeyDown, 0x61), true) && window.TakeEvent(Key(CocoaEventKind::KeyUp, 0x61), true));
    for (int i = 0; i < 3; i++) frame();
    CHECK(io.WantTextInput);
    // A new console's size: 120 columns by 30 rows.
    uint16_t columns = 0, rows = 0;
    screen.Size(columns, rows);
    CHECK(columns == 120 && rows >= 30);
    CHECK(consoleWindow() != nullptr);

    // The mouse as a hand moves it: to a place, then a button; ImGui takes a
    // button's change in the frame after a move.
    io.DeltaTime = 0.1f;
    auto down = [&](float x, float y, int button = 0) {
        frame({ Mouse(CocoaEventKind::MouseMoved, x, y) });
        frame({ Mouse(CocoaEventKind::MouseDown, x, y, button) });
    };
    auto up = [&](float x, float y, int button = 0) {
        frame({ Mouse(CocoaEventKind::MouseMoved, x, y) });
        frame({ Mouse(CocoaEventKind::MouseUp, x, y, button) });
        frame();
    };
    auto origin = window.RowsOrigin();
    float row = window.RowHeight();
    float y0 = origin.y + row * 0.5f;
    auto after = [&](char const* text) { return origin.x + ImGui::CalcTextSize(text).x + 1.0f; };

    // Dragged over "alpha ", the text is selected.
    down(origin.x + 1.0f, y0);
    up(after("alpha "), y0);
    CHECK(window.SelectedText() == "alpha ");
    // Across lines, the line ends are copied too; Command+C copies and ends
    // the selection, and the line has the keyboard again.
    down(after("alpha "), y0);
    up(after("de"), y0 + 2 * row);
    CHECK(window.SelectedText() == "beta\ngamma\nde");
    for (int i = 0; i < 3; i++) frame();
    CHECK(io.WantTextInput);
    press(0x08, kFlagCommand, "c");
    CHECK(window.SelectedText().empty());
    CHECK(ImGui::GetClipboardText() && std::string(ImGui::GetClipboardText()) == "beta\ngamma\nde");
    // Nothing reached the line.
    press(0x24);
    CHECK(ReadTyped(screen) == U"\r\n");
    // Enter copies a selection, and is not the line's.
    down(origin.x + 1.0f, y0 + 2 * row);
    up(after("delta"), y0 + 2 * row);
    for (int i = 0; i < 3; i++) frame();
    press(0x24);
    CHECK(window.SelectedText().empty() && std::string(ImGui::GetClipboardText()) == "delta");
    CHECK(ReadTyped(screen).empty());

    // A double click selects a word; Escape drops the selection.
    frame({ Mouse(CocoaEventKind::MouseMoved, origin.x + 2.0f, y0 + row) });
    frame({ Mouse(CocoaEventKind::MouseDown, origin.x + 2.0f, y0 + row) });
    frame({ Mouse(CocoaEventKind::MouseUp, origin.x + 2.0f, y0 + row) });
    frame({ Mouse(CocoaEventKind::MouseDown, origin.x + 2.0f, y0 + row) });
    up(origin.x + 2.0f, y0 + row);
    CHECK(window.SelectedText() == "gamma");
    press(0x35);
    CHECK(window.SelectedText().empty());
    // A right click copies a selection, and pastes without one.
    down(origin.x + 1.0f, y0);
    up(after("alpha"), y0);
    down(origin.x + 1.0f, y0, 1);
    up(origin.x + 1.0f, y0, 1);
    CHECK(window.SelectedText().empty() && std::string(ImGui::GetClipboardText()) == "alpha");
    down(origin.x + 1.0f, y0, 1);
    up(origin.x + 1.0f, y0, 1);
    for (int i = 0; i < 3; i++) frame();
    press(0x24);
    CHECK(ReadTyped(screen) == U"alpha\r\n");

    // A paste of several lines: each line end is Enter, the line typed so
    // far is the first's start, and what follows the last stays in the line.
    for (int i = 0; i < 3; i++) frame();
    press(0x07, 0, "x");
    ImGui::SetClipboardText("=1\nprint(2)\r\npart");
    press(0x09, kFlagCommand, "v");
    CHECK(ReadTyped(screen) == U"x=1\r\nprint(2)\r\n");
    press(0x24);
    CHECK(ReadTyped(screen) == U"part\r\n");
    // A paste of one line is ImGui's own.
    ImGui::SetClipboardText("one");
    press(0x09, kFlagCommand, "v");
    press(0x24);
    CHECK(ReadTyped(screen) == U"one\r\n");
    // Up goes through each line entered, the pasted ones too.
    press(0x7e);
    press(0x7e);
    press(0x24);
    CHECK(ReadTyped(screen) == U"part\r\n");

    // A tab is typed; Escape empties the line and keeps the keyboard.
    press(0x07, 0, "a");
    press(0x30, 0, "\t");
    press(0x07, 0, "b");
    press(0x24);
    CHECK(ReadTyped(screen) == U"a\tb\r\n");
    press(0x07, 0, "junk");
    press(0x35);
    CHECK(io.WantTextInput);
    press(0x24);
    CHECK(ReadTyped(screen) == U"\r\n");

    // The rows wrap at the window's width: a long line is more than one row.
    std::string longLine(400, 'w');
    screen.Write(me, longLine + "\n");
    for (int i = 0; i < 2; i++) frame();
    CHECK(consoleWindow() && consoleWindow()->Active);
    ImGui::DestroyContext();
}
}

int main()
{
    key_codes_name_the_keys_of_both_consumers_run();
    key_codes_and_scancodes_map_both_ways_run();
    keys_are_named_by_the_layout_where_sdl_names_them_so_run();
    modifier_flags_become_sdl_modifiers_run();
    view_points_become_drawable_pixels_run();
    drawable_pixels_become_view_points_again_run();
    text_the_input_system_commits_reaches_imgui_whole_run();
    a_key_of_the_input_system_is_not_typed_twice_run();
    the_input_methods_window_goes_below_imguis_text_cursor_run();
    the_pointer_leaving_the_view_leaves_imgui_without_a_position_run();
    the_overlay_sets_the_cursor_only_while_it_has_the_pointer_run();
    key_down_text_leaves_out_what_is_not_text_run();
    a_key_becomes_modifiers_the_key_and_its_text_run();
    a_modifier_change_becomes_the_key_that_changed_run();
    mouse_events_become_position_button_and_wheel_run();
    the_queue_replays_into_imgui_in_order_run();
    the_overlay_takes_keys_for_text_and_the_mouse_it_has_run();
    an_event_is_not_handled_on_a_state_that_is_being_replaced_run();
    events_become_the_sdl_events_of_norbytes_code_run();
    an_injected_key_becomes_the_change_the_game_queues_run();
    an_injected_keys_modifiers_are_held_around_it_run();
    the_overlay_draws_on_the_targets_the_game_presents_in_run();
    pq_has_the_values_of_st_2084_run();
    srgb_is_extended_beyond_its_range_run();
    the_primaries_are_bt_709_and_bt_2020_run();
    the_overlay_is_put_over_hdr_output_as_over_a_standard_target_run();
    an_opaque_overlay_colour_is_the_games_ui_colour_on_hdr_output_run();
    the_games_ui_white_is_read_from_its_code_run();
    only_a_paper_white_a_display_can_show_is_used_run();
    the_composite_shader_carries_the_constants_run();
    a_finished_frame_is_handed_to_the_main_thread_run();
    an_overlay_that_is_not_drawn_takes_no_input_run();
    a_withdrawn_frame_is_not_being_drawn_any_more_run();
    the_main_thread_does_not_wait_for_the_client_threads_slow_work_run();
    the_font_atlas_is_kept_within_a_metal_texture_run();
    class_data_is_read_from_a_file_run();
    class_data_that_is_not_as_expected_is_refused_run();
    a_method_is_replaced_only_when_it_is_the_code_that_was_checked_run();
    a_change_the_game_queues_becomes_the_sdl_button_it_is_run();
    the_first_back_of_a_read_whose_touchpad_changed_is_the_touchpad_run();
    sticks_and_triggers_have_sdls_values_run();
    a_read_posts_the_axes_and_buttons_the_game_does_not_queue_run();
    the_games_controller_buttons_are_compared_before_use_run();
    controllers_are_numbered_as_sdl_numbers_its_joysticks_run();
    a_controller_that_goes_is_centred_and_let_go_run();
    an_iohid_controllers_axes_have_sdls_values_run();
    the_games_half_axes_are_compared_before_use_run();
    imgui_writes_and_reads_numbers_in_the_c_locale_run();
    the_first_controller_navigates_the_overlay_as_imguis_sdl_backend_does_run();
    the_table_holds_what_the_overlay_names_run();
    a_binary_without_the_games_code_turns_every_part_off_run();
    the_overlay_resolves_nothing_itself_run();
    the_console_key_is_named_as_imgui_names_keys_run();
    the_console_key_shows_and_hides_the_window_and_goes_nowhere_else_run();
    the_log_says_when_the_console_window_cannot_be_seen_run();
    the_console_window_shows_the_screen_and_types_into_it_run();
    the_console_window_selects_copies_and_pastes_as_a_console_does_run();
    RunObjcTests();
    printf(gFailures ? "%d check(s) failed\n" : "all overlay tests passed\n", gFailures);
    return gFailures ? 1 : 0;
}
