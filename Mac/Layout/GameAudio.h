#pragma once

#include <cstddef>
#include <cstdint>

// Where the macOS game's ww::WwiseManager keeps the sound objects Ext.Audio
// reads, as its own code shows them (Layout/GameShapes.cpp says which code).
// Norbyte's SoundManager is asserted to give the same offsets
// (Layout/DefinitionChecks.cpp).

namespace bg3se::mac {

// After the vtable, 0x40 bytes the constructor zeroes; then seven arrays of
// four 64-bit sound object ids, one id per player, from +0x48, each 0x20
// bytes, that the constructor starts at -1: the listeners are the second
// (+0x68), the ones the game makes each player's default listener; the
// ambient, HUD and cinematic HUD objects the fifth, sixth and seventh
// (+0xc8, +0xe8, +0x108), the ones ls::SoundManager's StopAll functions stop.
// The music object, one for all players, follows them at +0x128; then a
// byte per player saying whether its listener is active (+0x130). The game
// names each object when it registers it: the first array the players'
// emitters, the third their rumble listeners and the fourth their
// controller speakers' listeners.
constexpr size_t kSoundObjects = 0x48;
constexpr size_t kSoundObjectArray = 0x20;
constexpr size_t kSoundObjectArrays = 7;
constexpr size_t kSoundPlayers = 4;
constexpr size_t kSoundPlayerEmitters = 0x48;
constexpr size_t kSoundListeners = 0x68;
constexpr size_t kSoundRumbleListeners = 0x88;
constexpr size_t kSoundSpeakerListeners = 0xa8;
constexpr size_t kSoundAmbient = 0xc8;
constexpr size_t kSoundHud = 0xe8;
constexpr size_t kSoundCineHud = 0x108;
constexpr size_t kSoundMusic = 0x128;
constexpr size_t kStopSoundsSlot = 29;
constexpr size_t kSoundActiveListeners = 0x130;

// ls::SoundComponent: its active data, a pointer, at +8, its flags at +0x10
// and its effect's name at +0x18. The active data is 0x28 bytes: the sound
// object's id at +0 (-1 for none), the entity at +8 (the null handle), and
// the switches, a ref map, at +0x18 (seven buckets). The game registers the
// object through the manager's slot 53 and stores the id it returns.
constexpr size_t kSoundComponentActiveData = 0x08;
constexpr size_t kSoundComponentFlags = 0x10;
constexpr size_t kSoundComponentEffect = 0x18;
constexpr size_t kActiveDataSize = 0x28;
constexpr size_t kActiveDataObject = 0x00;
constexpr size_t kActiveDataEntity = 0x08;
constexpr size_t kActiveDataSwitches = 0x18;
constexpr uint64_t kNoSoundObject = ~0ull;
constexpr uint64_t kNoEntity = 0xffc0000000000000ull;
constexpr size_t kRegisterSoundObjectSlot = 53;

}
