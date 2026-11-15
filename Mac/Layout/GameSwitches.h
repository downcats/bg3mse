#pragma once

#include <cstddef>
#include <cstdint>

// The Mac game's global switches (eoc::EoCGlobalSwitches, a class derived
// from ls::GlobalSwitches; the object behind ls::GlobalSwitches::m_ptr), as
// the game's own code shows them (Layout/GameShapes.cpp says which code).
// Norbyte's GlobalSwitches (GameDefinitions/Misc.h), which states both
// classes as one structure, is asserted to give the same offsets
// (Layout/DefinitionChecks.cpp).
//
// Four things differ from what his structure gives when it is compiled as
// it is for Windows. Each of the twelve sound settings ends in two words,
// not one, so every member after them is 0x60 bytes further in. The members
// of the derived class begin in the base class's tail padding, 4 bytes
// earlier. The array before the hotbar switches is aligned to 16 bytes,
// which moves what follows it by 8 more and makes the object 0x1440 bytes.
// And the switch of the Twitch overlay is a byte earlier.
//
// A member below is one the game's configuration code reads or writes under
// its name: App::InitAppConfig hands a member's value to the configuration
// as the default of the name, App::CopyValuesToConfig stores a member under
// the name. The names are fixed strings that AppConfigHelper.cpp's static
// initialiser makes from the text.

namespace bg3se::mac {

constexpr size_t kSwitchesSize = 0x1440;

// ls::GlobalSwitches: the language's text, the base class's last members.
constexpr size_t kSwitchesLanguage = 0xc0;              // ls::STDString, 16 bytes
constexpr size_t kSwitchesBaseLastInt = 0x180;          // saveSystemECBCheckNumberOfFramesToWait
constexpr size_t kSwitchesBaseLast = 0x188;             // a float; the derived class's members begin at 0x18c

// The sound settings: twelve of 0x68 bytes.
constexpr size_t kSwitchesSoundSettings = 0x1a8;
constexpr size_t kSoundSettingCount = 12;
constexpr size_t kSoundSettingSize = 0x68;
constexpr size_t kSoundSettingValue = 0x08;             // the third of three floats: the value the configuration gets
constexpr size_t kSoundSettingKey = 0x0c;               // ls::FixedString: the name in the configuration
constexpr size_t kSoundSettingFlag = 0x40;
constexpr size_t kSoundSettingText = 0x44;              // ls::TranslatedString
constexpr size_t kSoundSettingProc = 0x58;              // a function's address, and a word after it

// The four cameras' switches, the control switches, and the array that is
// aligned to 16 bytes.
constexpr size_t kSwitchesCameras = 0x7c4;
constexpr size_t kSwitchesControls = 0xe14;
constexpr size_t kSwitchesAlignedArray = 0xeb0;

// The members the configuration names.
constexpr size_t kSwitchesAutoListenEnabled = 0x100;
constexpr size_t kSwitchesCameraShakeEnabled = 0x117;
constexpr size_t kSwitchesTwitchEnableOverlay = 0x11b;
constexpr size_t kSwitchesLongRestScriptWaitingTime = 0x160;
constexpr size_t kSwitchesUseLevelCache = 0x18c;
constexpr size_t kSwitchesLoadAllEffectPools = 0x18d;
constexpr size_t kSwitchesScriptLog = 0x18e;
constexpr size_t kSwitchesCheckRequirements = 0x193;
constexpr size_t kSwitchesUseEndTurnFallback = 0x194;
constexpr size_t kSwitchesShowOriginIntroInCC = 0x196;
constexpr size_t kSwitchesAlwaysShowSplitter = 0x198;
constexpr size_t kSwitchesNorthFacingMinimap = 0x19b;
constexpr size_t kSwitchesHoldToEndTurn = 0x19c;
constexpr size_t kSwitchesSoundVolumeDynamicRange = 0x688;
constexpr size_t kSwitchesSoundOcclusionQuality = 0x68c;
constexpr size_t kSwitchesMuteSoundWhenNotFocused = 0x690;
constexpr size_t kSwitchesCentreDialogue = 0x691;
constexpr size_t kSwitchesForceMono = 0x692;
constexpr size_t kSwitchesSoundPartyLosingThreshold = 0x694;
constexpr size_t kSwitchesSoundPartyWinningThreshold = 0x698;
constexpr size_t kSwitchesGameCameraSpeedMultiplier = 0x6a0;
constexpr size_t kSwitchesGameCameraRotationLocked = 0x6a4;
constexpr size_t kSwitchesGameCameraEnableCloseUpDialog = 0x6a5;
constexpr size_t kSwitchesGameCameraEnableDynamicCombatCamera = 0x6a6;
constexpr size_t kSwitchesGameCameraEnableAttackCamera = 0x6a8;
constexpr size_t kSwitchesGameCameraEnableAttackCameraOtherPlayers = 0x6a9;
constexpr size_t kSwitchesGameCameraEnableFlyFollow = 0x6aa;
constexpr size_t kSwitchesGameCameraAutoRotateSpeed = 0x6b0;
constexpr size_t kSwitchesNrOfQuickSaves = 0x6c8;
constexpr size_t kSwitchesNrOfAutoSaves = 0x6cc;
constexpr size_t kSwitchesCanAutoSave = 0x6d8;
constexpr size_t kSwitchesCanCrossSave = 0x6d9;
constexpr size_t kSwitchesCanSaveDuringTimeline = 0x6da;
constexpr size_t kSwitchesCanLoadDuringTimeline = 0x6db;
constexpr size_t kSwitchesFirstTimeWithCrossSave = 0x6dc;
constexpr size_t kSwitchesHasSeenEAMsg = 0x6dd;
constexpr size_t kSwitchesForceSplitscreen = 0x6de;
constexpr size_t kSwitchesMaxAmountDialogsInLog = 0x6e0;
constexpr size_t kSwitchesControllerCharacterWalkThreshold = 0x6e8;
constexpr size_t kSwitchesControllerCharacterRunThreshold = 0x6ec;
constexpr size_t kSwitchesControllerRumbleStrength = 0x6fc;
constexpr size_t kSwitchesEnableVoiceLogging = 0x730;
constexpr size_t kSwitchesEnableLongPressAndHold = 0x731;
constexpr size_t kSwitchesMouseLock = 0x732;
constexpr size_t kSwitchesDisableEdgePanning = 0x733;
constexpr size_t kSwitchesEscClosesAllUI = 0x734;
constexpr size_t kSwitchesShowSubtitles = 0x735;
constexpr size_t kSwitchesDisplaySpeaker = 0x736;
constexpr size_t kSwitchesShowTextBackground = 0x737;
constexpr size_t kSwitchesTextBackgroundOpacity = 0x738;
constexpr size_t kSwitchesDialogueFontSizeModifier = 0x73c;
constexpr size_t kSwitchesOverheadFontSizeModifier = 0x740;
constexpr size_t kSwitchesUseRadialContextMenu = 0x749;
constexpr size_t kSwitchesPeaceCharacterHighlightMode = 0x7b0;
constexpr size_t kSwitchesCombatCharacterHighlightMode = 0x7b1;
constexpr size_t kSwitchesTacticalCharacterHighlightMode = 0x7b2;
constexpr size_t kSwitchesUnitSystem = 0x7b3;
constexpr size_t kSwitchesWeightedRolls = 0x7b4;
constexpr size_t kSwitchesPostCombatAssignation = 0x7b7;
constexpr size_t kSwitchesAutoFillHotbarCategories = 0x1332;
constexpr size_t kSwitchesPointAndClickSoundFrequency = 0x13f0;
constexpr size_t kSwitchesStartYear = 0x1418;
constexpr size_t kSwitchesStartDay = 0x141c;

}
