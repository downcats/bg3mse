#pragma once

#include <cstddef>
#include <cstdint>

// Where the Mac game keeps the members of the objects behind globals that
// Norbyte's code reads, as its own code shows them (Layout/GameShapes.cpp
// says which code). His declarations are asserted to give the same offsets
// (Layout/DefinitionChecks.cpp).

namespace bg3se::mac {

// ls::LevelManager, the base of the server's and the client's: after its
// vtable and 0x80 bytes, the local template manager at +0x88, the current
// level at +0x90 and the map of levels by name at +0x98 (an ls::Map of fixed
// strings: a node's level after its key, at +0x10).
constexpr size_t kLevelManagerTemplates = 0x88;
constexpr size_t kLevelManagerCurrent = 0x90;
constexpr size_t kLevelManagerLevels = 0x98;
// The local template manager's level data manager at +0x40, whose map of
// level data by name is at +8 (a hash map of fixed strings, hashed by their
// hash in 32 bits). A level keeps the local template manager at +0x20; the
// server's level is 0xf0 bytes and keeps its template cache at +0xe8, a
// cache template manager of level templates (5) with the level's name at
// +0x114, 0x118 bytes in all.
constexpr size_t kTemplatesLevelData = 0x40;
constexpr size_t kLevelDataLevels = 0x08;
// A level's data keeps the templates it tracks at +0x18, a hash map of fixed
// strings whose values are arrays of templates (16 bytes each).
constexpr size_t kLevelDataTrackedTemplates = 0x18;
constexpr size_t kLevelDataTrackedValueSize = 0x10;
constexpr size_t kLevelTemplates = 0x20;
constexpr size_t kServerLevelSize = 0xf0;
constexpr size_t kServerLevelTemplateCache = 0xe8;
constexpr size_t kLevelTemplateCacheSize = 0x118;
constexpr size_t kLevelTemplateCacheName = 0x114;
constexpr uint32_t kLevelTemplateCacheType = 5;

// ls::TranslatedStringRepository: nine text pools at +8, one for each pair of
// identities (three by three, the first pair's at +8); the fallback pool at
// +0x50, keyed by the handle alone, and the versioned one at +0x58, keyed by
// handle and version; a recursive spin lock at +0xf0, its owning thread at
// +0xf8. A pool's map of texts is at +0x10 in it, keyed by the handle alone
// (a version is compared afterwards, when one is asked for), each text a
// pointer and a length (16 bytes). A handle is a fixed string with its
// version at +4.
constexpr size_t kTranslatedPools = 0x08;
constexpr size_t kTranslatedPoolCount = 9;
constexpr size_t kTranslatedPoolRow = 3 * sizeof(void*);
constexpr size_t kTranslatedFallbackPool = 0x50;
constexpr size_t kTranslatedVersionedFallbackPool = 0x58;
constexpr size_t kTranslatedLock = 0xf0;
constexpr size_t kTranslatedLockOwner = 0xf8;
constexpr size_t kTextPoolTexts = 0x10;
constexpr size_t kTextSize = 0x10;
constexpr size_t kTextHandleVersion = 4;

// ls::TranslatedStringKeyManager: its map from a key to a translated string
// is at +0x18, and a node's string follows its key, at +0xc. A translated
// string is two handles, the argument string at +8.
constexpr size_t kTranslatedKeys = 0x18;
constexpr size_t kTranslatedKeyString = 0x0c;
constexpr size_t kTranslatedStringArgument = 0x08;

// ls::InputManager, at the members the game's code below shows: the input
// states by device at +0x80 (a hash map of 16-bit device ids, hashed
// sign-extended), the input definitions at +0xc0, the input scheme at +0xd0,
// the device listeners at +0x2f0, each player's device at +0x360 (32 bits)
// and device id at +0x370 (16 bits), the players' colours at +0x3c8 (16
// bytes each) and the twelve devices at +0x408, 0x28 bytes each, a device's
// colour at +0x14 and whether it has one at +0x24.
constexpr size_t kInputStates = 0x80;
constexpr size_t kInputDefinitions = 0xc0;
constexpr size_t kInputScheme = 0xd0;
constexpr size_t kInputDeviceListeners = 0x2f0;
constexpr size_t kInputPlayerDevices = 0x360;
constexpr size_t kInputPlayerDeviceIds = 0x370;
constexpr size_t kInputPlayerColors = 0x3c8;
constexpr size_t kInputDevices = 0x408;
constexpr size_t kInputDeviceSize = 0x28;
constexpr size_t kInputDeviceCount = 12;
constexpr size_t kInputDeviceColor = 0x14;
constexpr size_t kInputDeviceHasColor = 0x24;

// The rest of ls::InputManager, as the game makes it and its injections and
// events use it: the first critical section at +0, the one the injected
// inputs are taken under at +0x14, zero from +0x28 (the signal, unread by
// any code shown here) on; the repeats at +0x40, a hash map keyed by a
// one-byte player index hashed sign-extended, each value 0x60 bytes; the
// pressed modifiers in the byte at +0x2a8, the time of the last update at
// +0x2b0, two flags at +0x2b8; three listener groups of 0x30 bytes at
// +0x2c0 (a count of traversals at +0, the listeners at +8, whether to
// compact them after a traversal at +0x28); the raw inputs at +0x350; the
// remap being made at +0x378, its binding at +0x380 (modifiers at +4, the
// player at +6); the events at +0x388 (0x38 bytes each, eight made), the
// injected inputs at +0x3a8 (0x14 bytes each, whether to inject the raw
// input at +0x10), the device events at +0x3b8 (eight bytes each, the device
// at +4); the keyboard layout translator at +0x5e8 and eight more bytes:
// 0x5f8 bytes in all.
constexpr size_t kInputLock = 0x00;
constexpr size_t kInputInjectLock = 0x14;
constexpr size_t kInputAfterLocks = 0x28;
constexpr size_t kInputRepeats = 0x40;
constexpr size_t kInputRepeatSize = 0x60;
constexpr size_t kInputModifiers = 0x2a8;
constexpr size_t kInputUpdateTime = 0x2b0;
constexpr size_t kInputFlags = 0x2b8;
constexpr size_t kInputListeners = 0x2c0;
constexpr size_t kInputNotifyUpListeners = 0x320;
constexpr size_t kListenerGroupListeners = 0x08;
constexpr size_t kListenerGroupCompact = 0x28;
constexpr size_t kListenerGroupSize = 0x30;
constexpr size_t kInputRawInputs = 0x350;
constexpr size_t kInputRemap = 0x378;
constexpr size_t kInputRemapBinding = 0x380;
constexpr size_t kBindingModifiers = 4, kBindingPlayer = 6;
constexpr size_t kInputEvents = 0x388;
constexpr size_t kInputEventSize = 0x38;
constexpr size_t kInputEventsMade = 8;
constexpr size_t kInputInjects = 0x3a8;
constexpr size_t kInjectSize = 0x14;
constexpr size_t kInjectRaw = 0x10;
constexpr size_t kInputDeviceEvents = 0x3b8;
constexpr size_t kDeviceEventSize = 8;
constexpr size_t kDeviceEventDevice = 4;
constexpr size_t kInputTranslator = 0x5e8;
constexpr size_t kInputSize = 0x5f8;

// eoc::BoostPrototypeManager: the condition manager at +8, the map from a
// boost's GUID to its prototype at +0x10 (hash maps of 0x40 bytes; a GUID
// key is 16 bytes and its bucket the two halves' exclusive or modulo the
// bucket count), the array of condition handles at +0x50. A prototype is
// 0x28 bytes: its description (an ls::STDString) at +0, the type's name at
// +0x10, the parsed data at +0x18, the boost's type in the byte at +0x20 and
// the condition at +0x24.
constexpr size_t kBoostManagerConditions = 0x08;
constexpr size_t kBoostManagerBoosts = 0x10;
constexpr size_t kBoostManagerHandles = 0x50;
constexpr size_t kBoostSize = 0x28;
constexpr size_t kBoostTypeName = 0x10;
constexpr size_t kBoostData = 0x18;
constexpr size_t kBoostType = 0x20;
constexpr size_t kBoostCondition = 0x24;

// ls::TextureAtlasManager's map of atlases by path, an ls::RefMap of
// ls::Path: a node is 0x20 bytes, its path at +8 an ls::STDString of 16
// bytes (the bytes at +0 and their 32-bit count at +8 when long, the flag in
// the sign of byte 15), the atlas after it at +0x18. A path's bucket is
// MurmurHash3 x64-128 of its bytes with the seed 0, the first 64 bits,
// modulo the bucket count.
constexpr size_t kAtlasNodePath = 0x08;
constexpr size_t kAtlasNodeAtlas = 0x18;
constexpr size_t kAtlasNodeSize = 0x20;
constexpr size_t kPathSize = 0x10;
constexpr uint64_t kPathHashSeed = 0;

// ecl::CursorControl: after its two vtables whether the cursor shows in the
// byte at +0x10, the visible timer at +0x14 and the fade timer at +0x18;
// the cursor and overlay asked for at +0x24 and +0x28 (32 bits each); the
// array of forced cursors at +0x30, 32 bits each.
constexpr size_t kCursorShow = 0x10;
constexpr size_t kCursorVisibleTimer = 0x14;
constexpr size_t kCursorFadeTimer = 0x18;
constexpr size_t kCursorRequested = 0x24;
constexpr size_t kCursorRequestedOverlay = 0x28;
constexpr size_t kCursorOverrides = 0x30;
constexpr size_t kCursorTypeSize = 4;
// Its name at +0x1c (a fixed string) and the overlay shown at +0x20, made
// none and released by its destructor; 0x40 bytes in all. A new control has
// no overlay (0) and the default cursor (1).
constexpr size_t kCursorName = 0x1c;
constexpr size_t kCursorOverlay = 0x20;
constexpr size_t kCursorSize = 0x40;
constexpr uint32_t kCursorNoOverlay = 0, kCursorDefault = 1;

// ls::DragDropManager: the map from a player's id to that player's drag
// data is at +0xd0 (a ref map: its item count first). A node is 0x48 bytes:
// the 16-bit id at +8 and the data at +0x10, its entities first, then the
// position (+0x10), three flags (+0x18), the active controller (+0x20) and
// the array of controllers (+0x28). A controller keeps its player's id at
// +8. The id is hashed as a signed 16-bit number.
constexpr size_t kDragPlayers = 0xd0;
constexpr size_t kDragNodeData = 0x10;
constexpr size_t kDragNodeSize = 0x48;
constexpr size_t kDragDataPosition = 0x10;
constexpr size_t kDragDataFlags = 0x18;
constexpr size_t kDragDataActiveController = 0x20;
constexpr size_t kDragDataControllers = 0x28;
constexpr size_t kDragControllerPlayer = 0x08;

// ls::ImmutableDataHeadmaster, the GUID resource manager: it starts with the
// hash map from a type's index to that type's manager, keyed by the 32-bit
// index, sign-extended and taken modulo the bucket count.
constexpr size_t kStaticDataManagers = 0;
constexpr size_t kStaticDataIndexSize = 4;

// ls::CacheTemplateManagerBase: after its vtable the type of handle at +8;
// the templates by name at +0x10, by handle at +0x50 and the counts of
// references by handle at +0x90 (hash maps of 0x40 bytes); a recursive
// kernel lock at +0xd0, its owning thread at +0xe0 and the count of its
// write entries at +0xe8; the arrays of added and removed templates at +0xf0
// and +0x100; whether templates are synchronised in the byte at +0x110.
constexpr size_t kTemplateCacheType = 0x08;
constexpr size_t kTemplateCacheByName = 0x10;
constexpr size_t kTemplateCacheByHandle = 0x50;
constexpr size_t kTemplateCacheRefCounts = 0x90;
constexpr size_t kTemplateCacheLock = 0xd0;
constexpr size_t kTemplateCacheLockOwner = 0xe0;
constexpr size_t kTemplateCacheLockWrites = 0xe8;
constexpr size_t kTemplateCacheAdded = 0xf0;
constexpr size_t kTemplateCacheRemoved = 0x100;
constexpr size_t kTemplateCacheSync = 0x110;

}
