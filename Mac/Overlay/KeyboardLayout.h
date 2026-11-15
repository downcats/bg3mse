#pragma once

#include <cstdint>

// The keyboard layout's side of a key, as SDL's Cocoa keyboard code takes it
// (SDL_cocoakeyboard.m, UpdateKeymap and Cocoa_HandleKeyEvent): the character
// the key gives in the layout with no modifier held and no dead key, and
// whether the keyboard is an ISO one, on which macOS gives the key under
// escape and the key beside the left shift each other's key codes.
//
// On Windows an SDL key event's keycode is the layout's character for every
// key whose SDL default is a character (letters, digits, punctuation), and
// ImGui's SDL backend takes the overlay's letters, digits, comma, period and
// semicolon from it (ImGui_ImplSDL2_KeyEventToImGuiKey): on a French layout
// the key in the place of A is ImGuiKey_Q. These give the Mac the same.
//
// Main thread: the layout is the one the text input system has on it.

namespace bg3se::mac::overlay {

// The character of `keyCode` in a 'uchr' layout (the data of
// kTISPropertyUnicodeKeyLayoutData), for `keyboardType` (LMGetKbdType()), as
// SDL's UpdateKeymap takes it: the first UTF-16 unit of what the key types
// down with no modifier and no dead key, 0 for none (and for 0x10, which a
// layout gives for a key that types no character of its own).
uint32_t LayoutCharacterOf(void const* uchrData, uint32_t keyboardType, uint16_t keyCode);

class KeyboardLayout
{
public:
    // The current layout's character for the key; 0 when the layout has no
    // Unicode data (the overlay then names keys by their US places, and says
    // so in the log once).
    uint32_t CharacterOf(uint16_t keyCode);
    // Whether the keyboard last used is an ISO keyboard.
    bool Iso();

    static KeyboardLayout& Current();

private:
    void Update();

    void const* source_{ nullptr };     // TISInputSourceRef, retained
    void const* data_{ nullptr };       // its 'uchr' data, owned by the source
    bool said_{ false };
};

// The 'uchr' data of an installed layout, by its input source id (as
// "com.apple.keylayout.French"), for the tests; retains the source for good.
void const* LayoutDataByID(char const* inputSourceID);

}
