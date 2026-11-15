#pragma once

#include <cstdint>

#include <SDL_keycode.h>
#include <SDL_scancode.h>
#include <imgui.h>

// macOS virtual key codes (-[NSEvent keyCode]) in the terms of the two
// consumers of a key: Dear ImGui's keys for the overlay, and SDL's scancodes
// and modifier masks for Norbyte's Lua input events and injected keys, which
// are stated in SDL's values on every platform. The codes are the operating
// system's (HIToolbox/Events.h), not the game's.

namespace bg3se::mac::overlay {

// NSEventModifierFlags: the device-independent bits ...
constexpr uint32_t kFlagCapsLock = 1u << 16, kFlagShift = 1u << 17, kFlagControl = 1u << 18, kFlagOption = 1u << 19, kFlagCommand = 1u << 20;
// ... and the bits that say which of the two keys is down (IOKit's NX_DEVICE*KEYMASK).
constexpr uint32_t kDeviceLeftControl = 0x0001, kDeviceLeftShift = 0x0002, kDeviceRightShift = 0x0004, kDeviceLeftCommand = 0x0008,
    kDeviceRightCommand = 0x0010, kDeviceLeftOption = 0x0020, kDeviceRightOption = 0x0040, kDeviceRightControl = 0x2000;

// The key codes above this have no key.
constexpr uint16_t kLastKeyCode = 0x7f;

// ImGuiKey_None and SDL_SCANCODE_UNKNOWN for a code without a key.
ImGuiKey ImGuiKeyOf(uint16_t keyCode);
SDL_Scancode ScancodeOf(uint16_t keyCode);
// The key code of a scancode, or -1.
int KeyCodeOf(SDL_Scancode scancode);

// The key code of the key in the place of `keyCode` on an ANSI keyboard: on
// an ISO keyboard macOS gives the key under escape and the key beside the
// left shift each other's codes, and SDL swaps them back (Cocoa_HandleKeyEvent).
uint16_t PositionalKeyCode(uint16_t keyCode, bool iso);

// SDL's keycode for a key, with the layout's character for it (0 for none,
// Overlay/KeyboardLayout.h): the character for the keys SDL gives one, the
// scancode's keycode for the others.
SDL_Keycode SdlKeycodeOf(SDL_Scancode scancode, uint32_t layoutCharacter);

// The overlay's key as ImGui's SDL backend names it from that keycode and
// the scancode (ImGui_ImplSDL2_KeyEventToImGuiKey): the layout's letter,
// digit, comma, period or semicolon, where the key gives one; the key's
// place otherwise.
ImGuiKey ImGuiKeyOf(uint16_t keyCode, uint32_t layoutCharacter);

// For the key code of a modifier key: the bit of the modifier flags that is
// set while that key is down (for caps lock, while it is on). 0 for any other key.
uint32_t ModifierBitOf(uint16_t keyCode);

// SDL's modifier mask (KMOD_*) for the modifier flags of an event.
uint16_t SdlModifiersOf(uint32_t modifierFlags);

}
