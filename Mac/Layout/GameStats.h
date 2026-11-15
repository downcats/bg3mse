#pragma once

#include <cstddef>
#include <cstdint>

// The Mac game's stats object (RPGStats, behind RPGStats::m_ptr) and what
// hangs off it, as the game's own code shows them (Layout/GameShapes.cpp
// says which code). Norbyte's declarations (GameDefinitions/Stats/Stats.h,
// Common.h, Entries.h) are asserted to give the same offsets
// (Layout/DefinitionChecks.cpp).
//
// Two things differ from the Windows object. The random number generator
// after the four "current" pointers is 0x30 bytes, a word more, so every
// member from the extra data on is 8 bytes further in. And a list of
// modifiers (CRPGStats_Modifier_List) is its manager of attributes as a base
// class, so its name is in the manager's last four bytes, which the manager
// leaves unused.

namespace bg3se::mac {

// CNamedElementManager<T, int, 0, -1>: Norbyte's stats::CNamedElementManager<T>.
constexpr size_t kNamedManagerValues = 0x08;            // the elements: pointer, 32-bit capacity, 32-bit size
constexpr size_t kNamedManagerValuesSize = 0x14;
constexpr size_t kNamedManagerNames = 0x18;             // ls::HashMap<ls::FixedString, int>: name to handle
constexpr size_t kNamedManagerNextHandle = 0x58;        // 32 bits
constexpr size_t kNamedManagerSize = 0x60;

// ls::HashMap<ls::FixedString, int>: the buckets (pointer, 32-bit size), the
// chain of each key (pointer, capacity, size), the keys (the same), the
// values (pointer, size). A key's bucket is its string's hash modulo the
// number of buckets.
constexpr size_t kHashMapBuckets = 0x00, kHashMapBucketCount = 0x08;
constexpr size_t kHashMapNext = 0x10, kHashMapNextSize = 0x1c;
constexpr size_t kHashMapKeys = 0x20, kHashMapKeysSize = 0x2c;
constexpr size_t kHashMapValues = 0x30;
constexpr size_t kHashMapSize = 0x40;

// ls::Map<ls::FixedString, T>: Norbyte's LegacyMap. A key's bucket is the
// fixed string's index modulo the number of buckets.
constexpr size_t kMapBucketCount = 0x00, kMapBuckets = 0x08, kMapItemCount = 0x10;
constexpr size_t kMapSize = 0x18;
// ls::RefMap: Norbyte's LegacyRefMap.
constexpr size_t kRefMapItemCount = 0x00, kRefMapBucketCount = 0x04, kRefMapBuckets = 0x08;
// A node of either: an int follows the key, a pointer is aligned after it.
constexpr size_t kMapNodeNext = 0x00, kMapNodeKey = 0x08, kMapNodeIntValue = 0x0c, kMapNodePointerValue = 0x10;

// RPGStats.
constexpr size_t kStatsSize = 0x420;
constexpr size_t kStatsValueLists = 0x000;              // CRPGStats_Modifier_ValueList_Manager
constexpr size_t kStatsModifierLists = 0x060;           // CRPGStats_Modifier_List_Manager
constexpr size_t kStatsObjects = 0x0c0;                 // CRPGStats_Object_Manager
constexpr size_t kStatsTreasureCategories = 0x120;      // CRPGStats_Treasure_Category_Manager
constexpr size_t kStatsTreasureTables = 0x180;          // CRPGStats_Treasure_Table_Manager
constexpr size_t kStatsItemTypes = 0x1e0;               // CRPGStats_ItemType_Manager: a manager and one word
constexpr size_t kStatsFunctors = 0x248;                // eoc::StatsFunctorListManager: a map
constexpr size_t kStatsCurrentValueList = 0x260;        // four pointers used while parsing
constexpr size_t kStatsCurrentType = 0x268;
constexpr size_t kStatsRandom = 0x280;                  // its numbers' pointer first; 0x30 bytes
constexpr size_t kStatsExtraData = 0x2b0;               // CRPGStats_ExtraData*
constexpr size_t kStatsCategoryMappings = 0x2b8;        // four maps
constexpr size_t kStatsWeaponCounters = 0x2d0;
constexpr size_t kStatsSpellbookCounters = 0x2e8;
constexpr size_t kStatsArmorCounters = 0x300;
constexpr size_t kStatsRefMap = 0x318;                  // Norbyte's field_2F0
constexpr size_t kStatsTreasureRarities = 0x328;        // seven fixed strings
constexpr size_t kStatsFixedStrings = 0x348;            // pointer, 32-bit capacity, 32-bit size
constexpr size_t kStatsInt64s = 0x358;                  // four arrays, 0x10 bytes each
constexpr size_t kStatsGuids = 0x368;
constexpr size_t kStatsFloats = 0x378;
constexpr size_t kStatsTranslatedStrings = 0x388;
constexpr size_t kStatsEquipmentSets = 0x398;           // four pointers to managers
constexpr size_t kStatsSpellSets = 0x3a0;
constexpr size_t kStatsItemProgression = 0x3a8;
constexpr size_t kStatsItemCombinations = 0x3b0;
constexpr size_t kStatsCurrentBuffer = 0x3b8;           // ls::UniquePtr<RPGStats_DataBuffer>
constexpr size_t kStatsCurrentBufferPath = 0x3c0;       // fixed string
constexpr size_t kStatsBufferMap = 0x3c8;               // map from an entry's name to its buffer's index
constexpr size_t kStatsBuffers = 0x3e0;                 // array of pointers to buffers
constexpr size_t kStatsBloodTypes = 0x3f0;              // map
constexpr size_t kStatsLoaded = 0x40a;                  // the third of three bytes from 0x408
constexpr size_t kStatsLock = 0x40c;                    // 32 bits
constexpr size_t kStatsConditions = 0x410;              // array of strings
constexpr size_t kArraySize = 0x0c;                     // an array's 32-bit size, after its pointer and capacity
constexpr size_t kArrayCapacity = 0x08;

// ls::LEGACY_Set<T>: Norbyte's TrackedCompactSet<T>. The buffer is an array
// made by new[]: for an element type with a destructor (a fixed string) the
// block starts 16 bytes before the first element, with the element's size
// and then the number of elements; for one without (an int) the block is
// the elements alone. Either way it comes from malloc and goes to free.
constexpr size_t kSetHeaderBytes = 0x10;
constexpr size_t kSetHeaderElementSize = 0x00, kSetHeaderCount = 0x08;

// RPGStats_DataBuffer: Norbyte's PreParsedDataBuffer.
constexpr size_t kStatsBufferType = 0x20, kStatsBufferNext = 0x28;

// StatsObject: Norbyte's stats::Object.
constexpr size_t kStatsObjectProperties = 0x08;         // begin and end pointers of 32-bit indices
constexpr size_t kStatsObjectName = 0x20;
constexpr size_t kStatsObjectFunctors = 0x28;           // ls::HashMap<ls::FixedString, array>
constexpr size_t kStatsObjectRollConditions = 0x68;     // the same
constexpr size_t kStatsObjectAIFlags = 0xa8;
constexpr size_t kStatsObjectRequirements = 0xb0;       // array
constexpr size_t kStatsObjectComboProperties = 0xc0;    // pointer, capacity, size
constexpr size_t kStatsObjectComboCategories = 0xd0;
constexpr size_t kStatsObjectUsing = 0xe0;              // 32-bit handle of the entry it was declared "using", or -1
constexpr size_t kStatsObjectModifierList = 0xe4;       // 32-bit index of its list of modifiers
constexpr size_t kStatsObjectLevel = 0xe8;              // 32 bits, 1 in a new object

// A treasure category and the items it lists: Norbyte's
// stats::TreasureCategory and stats::TreasureCategoryItem, the game's
// CRPGStats_Treasure_SubTable and CRPGStats_Treasure_Object. The items are a
// vector of this build's standard library, of pointers. One item is listed
// by every category its line of the treasure file names: after its seven
// numbers it has a signed byte that counts them, which Norbyte's Windows
// declaration does not have, and the item is freed with the last of them.
constexpr size_t kTreasureCategorySize = 0x38;
constexpr size_t kTreasureCategoryName = 0x00;
constexpr size_t kTreasureCategoryItems = 0x08;         // begin, end and end of storage
constexpr size_t kTreasureItemSize = 0x24;
constexpr size_t kTreasureItemName = 0x00;
constexpr size_t kTreasureItemNumbers = 0x04;           // seven 32-bit numbers
constexpr size_t kTreasureItemLastNumber = 0x1c;
constexpr size_t kTreasureItemCategories = 0x20;        // how many categories list the item

// A treasure table (the game's CRPGStats_Treasure_Table), a subtable of it
// and a subtable's category: Norbyte's stats::TreasureTable,
// TreasureSubTable and TreasureSubTableCategory, which his Lua readers
// allocate by their sizes.
constexpr size_t kTreasureTableSize = 0x20;
constexpr size_t kTreasureTableSubTables = 0x10;
constexpr size_t kTreasureSubTableSize = 0x58;
constexpr size_t kTreasureSubTableName = 0x54;          // a fixed string, Norbyte's field_54
constexpr size_t kTreasureSubTableCategorySize = 0x28;

// CRPGStats_Modifier_List, CRPGStats_Modifier, CRPGStats_Modifier_ValueList:
// Norbyte's ModifierList, Modifier, RPGEnumeration.
constexpr size_t kModifierListName = 0x5c;
constexpr size_t kModifierValueList = 0x00;             // 32-bit index of its list of values
constexpr size_t kModifierName = 0x0c;
constexpr size_t kValueListName = 0x00, kValueListValues = 0x08;

// TextKeyStatsFunctorList, an element of a stats object's functors: Norbyte's
// FunctorGroup. TextKeyRollCondition, of its roll conditions: his
// Object::RollCondition.
constexpr size_t kFunctorGroupSize = 0x10, kFunctorGroupTextKey = 0x00, kFunctorGroupList = 0x08;
constexpr size_t kRollConditionSize = 0x08, kRollConditionTextKey = 0x00, kRollConditionCondition = 0x04;

// eoc::StatsFunctorList: a manager of functors (CNamedElementManager). The
// slot of its vtable that inserts one.
constexpr size_t kFunctorListInsertSlot = 0x18;

// eoc::StatsFunctorList, in full: its name follows its manager, as a list of
// modifiers' does, and these are the slots of the manager's vtable. A
// functor's own vtable has Clone, and a functor has the context it runs in.
constexpr size_t kFunctorListUniqueName = 0x5c;
constexpr size_t kNamedManagerDestroySlot = 0x10, kNamedManagerInsertSlot = 0x18, kNamedManagerCountSlot = 0x38;
constexpr size_t kNamedManagerEntrySlot = 0x40, kNamedManagerConstEntrySlot = 0x48;
constexpr size_t kFunctorCloneSlot = 0x18;
constexpr size_t kFunctorPropertyContext = 0x38;        // 64 bits

// ls::StaticFixedStringRegistry's entry for one text: the text, its length,
// the next entry, and the fixed string made of it.
constexpr size_t kRegistryEntryString = 0x18;

// The flags of a stats attribute, behind a pointer in the stats' array of
// 64-bit values: a context is their low 44 bits.
constexpr unsigned kContextFlagBits = 44;

// eoc::PassivePrototype: Norbyte's stats::PassivePrototype.
constexpr size_t kPassiveSize = 0x210;
constexpr size_t kPassiveProperties = 0x00;             // 32 bits
constexpr size_t kPassiveName = 0x04;
constexpr size_t kPassiveDescription = 0x08;            // eoc::StatsDescription
constexpr size_t kPassiveEnabledConditions = 0x90;      // 32-bit index of a condition
constexpr size_t kPassiveEnabledContext = 0x98;         // 64 bits
constexpr size_t kPassiveToggleOnEffect = 0xa0, kPassiveToggleOffEffect = 0xa4;
constexpr size_t kPassiveStatsFunctorContext = 0xa8;
constexpr size_t kPassiveConditions = 0xb0;
constexpr size_t kPassiveStatsFunctors = 0xb8;          // three functor lists, 0x60 bytes each
constexpr size_t kPassiveToggleOnFunctors = 0x118, kPassiveToggleOffFunctors = 0x178;
constexpr size_t kPassiveToggleGroup = 0x1d8;
constexpr size_t kPassiveToggleOffContext = 0x1e0, kPassiveBoostContext = 0x1e8;
constexpr size_t kPassiveBoostConditions = 0x1f0;
constexpr size_t kPassiveBoosts = 0x1f8;                // array of GUIDs
constexpr size_t kPassivePriorityOrder = 0x208;
constexpr size_t kPassiveTooltipConditionalDamage = 0x20c;
// The bits of the Properties attribute a passive takes.
constexpr uint32_t kPassivePropertyBits = 0x3ffff;

// eoc::InterruptPrototype: Norbyte's stats::InterruptPrototype.
constexpr size_t kInterruptSize = 0x1f0;
constexpr size_t kInterruptName = 0x00;
constexpr size_t kInterruptDescription = 0x08;
constexpr size_t kInterruptContext = 0x90;              // 16 bits
constexpr size_t kInterruptContextScope = 0x92;         // 8 bits
constexpr size_t kInterruptConditions = 0x94;
constexpr size_t kInterruptContainer = 0x98;
constexpr size_t kInterruptRoll = 0x9c;
constexpr size_t kInterruptCosts = 0xa0;                // array of action resource costs
constexpr size_t kInterruptStack = 0xb0;
constexpr size_t kInterruptPropertiesFunctors = 0xb8, kInterruptSuccessFunctors = 0x118, kInterruptFailureFunctors = 0x178;
constexpr size_t kInterruptDefaultValue = 0x1d8;        // 8 bits
constexpr size_t kInterruptEnableCondition = 0x1dc;
constexpr size_t kInterruptEnableContext = 0x1e0;       // 64 bits
constexpr size_t kInterruptFlags = 0x1e8;               // 8 bits
// The bits of their attributes an interrupt takes, the context its functors
// run in, and the bits of a context an interrupt can be enabled in.
constexpr uint32_t kInterruptContextBits = 0xff, kInterruptContextScopeBits = 0x7, kInterruptFlagBits = 0x1, kInterruptDefaultValueBits = 0x3;
constexpr uint64_t kInterruptFunctorContext = 0x4000000000;
constexpr uint64_t kInterruptEnableContextBits = 0x8 | 0x400 | 0x80000 | 0x800000000;

// eoc::SpellAnimations: Norbyte's stats::SpellPrototypeAnimationData.
// Fixed strings, and at +0x40 a set of three fixed strings each
// (ls::LEGACY_Set<ls::AnimationSimpleNamesSet>, a set made by new[]:
// kSetHeaderBytes before its elements), then the flags.
constexpr size_t kSpellAnimationsSize = 0x78;
constexpr size_t kSpellAnimationsNames = 0x40;
constexpr size_t kSpellAnimationsLastName = 0x70;      // the last fixed string, destroyed first
constexpr size_t kSpellAnimationsFlags = 0x74;          // 8 bits
constexpr size_t kAnimationNamesSize = 0x0c;            // an element of the set
// A set's capacity and size, after its pointer.
constexpr size_t kSetCapacity = 0x08, kSetSize = 0x0c;

// eoc::SpellPrototype: Norbyte's stats::SpellPrototype, through what its
// manager's Terminate destroys.
constexpr size_t kSpellName = 0x08;
constexpr size_t kSpellContainer = 0x48;
constexpr size_t kSpellDescription = 0x60;              // eoc::StatsDescription
constexpr size_t kSpellChildren = 0xf8;
constexpr size_t kSpellUseCosts = 0x108, kSpellRitualCosts = 0x118, kSpellDualWieldingUseCosts = 0x128;
constexpr size_t kSpellHitCostGroups = 0x138;
constexpr size_t kSpellVariableUseCosts = 0x148, kSpellVariableDualWieldingUseCosts = 0x158, kSpellVariableRitualCosts = 0x168;
constexpr size_t kSpellAnimation = 0x180, kSpellDualWieldingAnimation = 0x1f8;
constexpr size_t kSpellPrepareEffect = 0x270, kSpellPrepareSound = 0x274, kSpellPrepareLoopSound = 0x278, kSpellCastSound = 0x27c;
constexpr size_t kSpellAlternativeCastTextEvents = 0x288, kSpellContainerSpells = 0x2a0;
constexpr size_t kSpellTrajectories = 0x2b0;            // an array of arrays of fixed strings
constexpr size_t kSpellPathfindTemplate = 0x2c0;        // a fixed string; Norbyte's 32-bit integer has its size
constexpr size_t kSpellMetaConditions = 0x2d0;
constexpr size_t kSpellItemWall = 0x310, kSpellInterruptPrototype = 0x314, kSpellCombatAIOverrideSpell = 0x318;
constexpr size_t kSpellCombatAIOverrideSpells = 0x320;
// An array's size: a pointer and two 32-bit counts.
constexpr size_t kArrayBytes = 0x10;

// eoc::SpellPrototypeManager: Norbyte's stats::SpellPrototypeManager.
constexpr size_t kSpellManagerSpells = 0x08, kSpellManagerCombatAIOverrideSpells = 0x48;
constexpr size_t kSpellManagerNames = 0x88, kSpellManagerInitialized = 0x98;

// eoc::StatusPrototype and its manager: Norbyte's stats::StatusPrototype and
// stats::StatusPrototypeManager.
constexpr size_t kStatusName = 0x08, kStatusDescription = 0x20;
constexpr size_t kStatusAbsorbSurfaceTypes = 0xb8, kStatusBoosts = 0xc0;
constexpr size_t kStatusSoundStart = 0xd8, kStatusSoundLoop = 0xe8, kStatusSoundStop = 0xf8;
constexpr size_t kStatusManagerStatuses = 0x08, kStatusManagerNames = 0x48, kStatusManagerInitialized = 0x58;

// eoc::Passives: Norbyte's stats::PassivePrototypeManager after its map.
constexpr size_t kPassivesInitialized = 0x18;

// eoc::InterruptPrototypeManager: Norbyte's stats::InterruptPrototypeManager.
// Its map holds the prototypes themselves.
constexpr size_t kInterruptManagerInterrupts = 0x08;

static_assert(kFunctorListUniqueName == kModifierListName && kNamedManagerInsertSlot == kFunctorListInsertSlot);
static_assert(kSpellDualWieldingAnimation - kSpellAnimation == kSpellAnimationsSize);
static_assert(kStatsExtraData - kStatsRandom == 0x30);
static_assert(kModifierListName == kNamedManagerNextHandle + sizeof(int32_t));

}
