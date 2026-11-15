#pragma once

#include <cstddef>
#include <cstdint>

// ls::InputRawChange as the Mac game's callers of
// ls::InputManager::InjectInput(ls::InputRawChange const&, bool) build it and
// as that function reads it: 16 bytes, copied whole into the manager's queue.
// The engine table's overlay area compares the pressed byte's place with the
// code of -[LSMTLView keyDown:], whose instructions also fix the id at +0 and
// the two floats at +4 (Mac/Overlay/Dependencies.cpp).

namespace bg3se::mac::overlay {

struct GameInputChange
{
    uint32_t Input;         // the input's id: for a key, its entry in cocoa::CocoaInputTranslator::s_KeyboardKeys
    float Value[2];         // 1.0, 1.0 for a key going down; 0, 0 for a key going up
    uint8_t Pressed;
    uint8_t Unused[3];
};

static_assert(sizeof(GameInputChange) == 16 && offsetof(GameInputChange, Value) == 4 && offsetof(GameInputChange, Pressed) == 12);

// The table's entry for a key code the game has no input for.
constexpr uint16_t kNoGameInput = 0xffff;

}
