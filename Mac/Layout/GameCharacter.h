#pragma once

#include <cstddef>
#include <cstdint>

// Where the Mac game keeps the members of the server's character
// (esv::Character) that Norbyte's code reads, as its own code shows them
// (Layout/GameShapes.cpp, the server character's proofs). His declaration
// (GameDefinitions/Character.h) is asserted to give these offsets
// (Layout/DefinitionChecks.cpp).

namespace bg3se::mac {

// esv::Character: its two templates at +0xc8 and +0xd0, the player data at
// +0x150; the character that owns it at +0x158, the one it follows at +0x160
// and its enemy at +0x170 (entity handles); its base visual at +0x180 (a
// fixed string); the user that owns it at +0x184 (what GetOwnerUserID
// gives) and the user it is reserved for at +0x188 (GetReservedForUserID),
// both unassigned (0xffff0000) when it is made; the speed multiplier at
// +0x18c, 1 when it is made.
constexpr size_t kCharacterTemplate = 0xc8;
constexpr size_t kCharacterOriginalTemplate = 0xd0;
constexpr size_t kCharacterPlayerData = 0x150;
constexpr size_t kCharacterOwner = 0x158;
constexpr size_t kCharacterFollow = 0x160;
constexpr size_t kCharacterHandles = 4;         // entity handles from the owner on, the enemy last
constexpr size_t kCharacterEnemy = 0x170;
constexpr size_t kCharacterBaseVisual = 0x180;
constexpr size_t kCharacterOwnerUser = 0x184;
constexpr size_t kCharacterReservedUser = 0x188;
constexpr size_t kCharacterSpeedMultiplier = 0x18c;
constexpr uint32_t kUnassignedUser = 0xffff0000;

// esv::Item (Norbyte's esv::Item, GameDefinitions/Item.h), as the Windows
// one: two vtable pointers, the entity's object handle, 64 bits of flags, the
// entity (handle and world), the level and the item's type (fixed strings),
// then what the game calls the item's cold data: the peers to update, the
// two templates, five pointers of which the last is the stats object, two
// words, a handle, three fixed strings of which the second is the stats id
// and the third the previous level, the treasure level and the amount, and
// a byte of flags.
constexpr size_t kItemObjectHandle = 0x10;
constexpr size_t kItemFlags = 0x18;
constexpr size_t kItemEntity = 0x20;
constexpr size_t kItemLevel = 0x30;
constexpr size_t kItemType = 0x34;
constexpr size_t kItemColdData = 0x38;
constexpr size_t kItemPeers = 0x38;                 // array of 32-bit peer ids
constexpr size_t kItemTemplate = 0x48;
constexpr size_t kItemOriginalTemplate = 0x50;
constexpr size_t kItemStatsObject = 0x78;
constexpr size_t kItemWord = 0x88;                  // a word, then a handle
constexpr size_t kItemNames = 0x98;                 // two fixed strings: the second is the stats id
constexpr size_t kItemStats = 0x9c;
constexpr size_t kItemPreviousLevel = 0xa0;
constexpr size_t kItemTreasureLevel = 0xa4;         // 32 bits, and the amount after it
constexpr size_t kItemFlags2 = 0xac;                // one byte
constexpr uint8_t kItemGlobal = 0x02;               // in it: what IsGlobal gives

// ecl::Character (Norbyte's ecl::Character, GameDefinitions/Character.h), as
// the Windows one: two vtable pointers, the entity's object handle, 64 bits
// of flags, the level, the entity, then the cold data: the surface path
// influences, two 64-bit network ids, the two templates, eleven pointers,
// the player data, three words, the owner and the character using it, an
// array, four handles, the owning and the reserved user (unassigned when it
// is made), an array, two floats, the speed multiplier (1 when it is made),
// 32 bits, 16 bits of physics flags, a byte, and two bytes of flags.
constexpr size_t kClientCharacterObjectHandle = 0x10;
constexpr size_t kClientCharacterFlags = 0x18;
constexpr size_t kClientCharacterLevel = 0x20;
constexpr size_t kClientCharacterEntity = 0x28;
constexpr size_t kClientCharacterColdData = 0x38;
constexpr size_t kClientCharacterPathInfluences = 0x38;
constexpr size_t kClientCharacterRequests = 0x48;   // two 64-bit ids
constexpr size_t kClientCharacterTemplate = 0x58;
constexpr size_t kClientCharacterOriginalTemplate = 0x60;
constexpr size_t kClientCharacterInputController = 0xa0;    // the eighth of the eleven pointers
constexpr size_t kClientCharacterPlayerData = 0xc8;
constexpr size_t kClientCharacterOwner = 0xe8;      // two handles
constexpr size_t kClientCharacterCircles = 0xf8;    // array
constexpr size_t kClientCharacterEffects = 0x108;   // four handles: the second is the followed character
constexpr size_t kClientCharacterOwnerUser = 0x128; // 32 bits, and the reserved user after it
constexpr size_t kClientCharacterCircleColors = 0x130;
constexpr size_t kClientCharacterLastTick = 0x140;  // two floats
constexpr size_t kClientCharacterSpeedMultiplier = 0x148;
constexpr size_t kClientCharacterPhysicsFlags = 0x150;  // 16 bits
constexpr size_t kClientCharacterFeetStatus = 0x152;
constexpr size_t kClientCharacterFlags2 = 0x153;    // two bytes

// ecl::InputController (Norbyte's, GameDefinitions/Controllers/Input.h), the
// controller a client character's input goes through: a byte of flags at
// +0x48, of which the lowest says that the character is played with a game
// controller. The game sets it from its own controller mode
// (ecl::GameInput::OnControllerModeChanged), and a script reads it to tell
// which of the game's two interfaces is shown.
constexpr size_t kClientInputFlags = 0x48;
constexpr uint8_t kClientInputControllerMode = 0x01;

// esv::Status (Norbyte's esv::Status, GameDefinitions/Status.h), as the
// Windows one: the vtable pointer, a GUID, a word that is -1 when the status
// is made, the story action, the action's originator (a GUID, three fixed
// strings and a flag that is set), a double, the status's id, four floats
// of which the second and third are the life time and the current one (-1),
// a float, two cause bytes, four handles of which the third is the status's
// own, the owner, an array, the cause, a GUID, the source, the root cause,
// a handle, a GUID, the conditions (-1) and events of its removal, three
// flag bytes, four bytes, the source spell, two GUIDs around the stack id,
// a priority, a flag, a fixed string, the condition rolls, the tick type
// and two more flag bytes.
constexpr size_t kStatusWord = 0x18;
constexpr size_t kStatusStoryAction = 0x20;
constexpr size_t kStatusOriginator = 0x28;
constexpr size_t kStatusOriginatorStrings = 0x38;   // three fixed strings, then the flag
constexpr size_t kStatusDouble = 0x48;
constexpr size_t kStatusId = 0x50;
constexpr size_t kStatusLifeTime = 0x58;            // two floats
constexpr size_t kStatusTurnTimer = 0x60;           // two floats
constexpr size_t kStatusCauseType = 0x68;           // two bytes
constexpr size_t kStatusHandle = 0x80;
constexpr size_t kStatusSyncEntity = 0x88;
constexpr size_t kStatusOwner = 0x90;
constexpr size_t kStatusOwners = 0x98;              // array
constexpr size_t kStatusCause = 0xa8;
constexpr size_t kStatusCauseGuid = 0xb8;
constexpr size_t kStatusSource = 0xc8;
constexpr size_t kStatusRootCause = 0xd0;           // handle and world, then a handle
constexpr size_t kStatusGuid = 0xe8;
constexpr size_t kStatusRemoveConditions = 0xf8;    // 32 bits, and the events after them
constexpr size_t kStatusFlags = 0x100;              // three bytes
constexpr size_t kStatusFreezeDuration = 0x103;     // four bytes
constexpr size_t kStatusSourceSpell = 0x108;
constexpr size_t kStatusSpellSourceType = 0x110;
constexpr size_t kStatusSpellPrototype = 0x138;
constexpr size_t kStatusSpellCastSource = 0x140;
constexpr size_t kStatusStackId = 0x150;
constexpr size_t kStatusRecoverable = 0x158;
constexpr size_t kStatusDifficultyStatus = 0x15c;
constexpr size_t kStatusStackedSpellCastSource = 0x160;     // a GUID, then the array of condition rolls
constexpr size_t kStatusTickType = 0x180;
constexpr size_t kStatusFlags4 = 0x181;

}
