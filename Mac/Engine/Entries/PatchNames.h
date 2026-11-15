#pragma once

// The names of the patch area's entries (Engine/Entries/Patches.cpp), for the
// code that applies them (Mac/Binding/CodePatches.cpp).

namespace bg3se::mac::engine::patches {

// The area.
inline constexpr char kArea[] = "code patches";

// Entries.
inline constexpr char kRestartThroughSteam[] = "App::InitCommunity: call of SteamAPI_RestartAppIfNecessary";
inline constexpr char kUnlockAchievementGate[] = "esv::AchievementManager::UnlockAchievement: leaving the mod loop at a mod";
inline constexpr char kAchievementCounterGate[] = "esv::AchievementManager::IncreaseAchievementCounter: leaving the mod loop at a mod";
inline constexpr char kStoryAchievementGate[] = "esv::OsirisGameFunctions::UnlockAchievement: leaving the mod loop at a mod";
inline constexpr char kSavedAsModded[] = "esv::GameStateSave::Enter: leaving the mod loop at a mod";
inline constexpr char kLoadedAsModded[] = "esv::SavegameManager::FinishLoadGame: leaving the mod loop at a mod";
inline constexpr char kSaveErrorSaveMods[] = "esv::SavegameManager::ThrowError: leaving the loop over the savegame's modules at a mod";
inline constexpr char kSaveErrorServerMods[] = "esv::SavegameManager::ThrowError: leaving the loop over the server's modules at a mod";
inline constexpr char kSaveScreenHasMods[] = "gui::DCSavegames::Update: leaving the mod loop at a mod";
inline constexpr char kSaveErrorsWithoutMods[] = "esv::SavegameManager::ThrowError: no mods";

}
