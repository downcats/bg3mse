#include <Engine/Entries/Areas.h>
#include <Engine/Entries/PatchNames.h>

// The code Norbyte's extender patches (the <Patch> elements of
// GameHooks/BinaryMappings.xml, applied by LibraryManager::ApplyCodePatch):
// on Windows a run of x86 bytes found by a pattern, here one instruction of a
// named function each. A site is found from its host's code, the host's whole
// shape is compared, and the pattern states the instruction that is replaced:
// a site is written only where the code is the code that was read.
//
// Read from build 4.1.1.7631656 (arm64).

namespace bg3se::mac::engine {

namespace {

using namespace patches;

constexpr uint32_t kNop = 0xd503201f;

// --- KillLauncher ------------------------------------------------------------
//
// Windows: `mov ecx, 1086940 ; call SteamAPI_RestartAppIfNecessary ; test al, al ;
// jz + ; xor ecx, ecx ; call exit`, and the patch removes everything from the
// call to the exit, so that the game never ends itself to be started again
// through Steam. Here, in App::InitCommunity():
//
//     mov  w0, #0x95dc ; movk w0, #0x10, lsl #16      the application's id, 1086940
//     bl   _SteamAPI_RestartAppIfNecessary            the one call to that import
//     cbnz w0, <mov w0, #0 ; bl _exit>
//
// The call becomes `mov w0, #0`: the answer is "no restart", and the branch to
// the exit is never taken. (Removing the call alone would leave the id in w0.)
constexpr MaskedWord kRestartCall[] = {
    { 0x5292bb80 },                     // mov w0, #0x95dc
    { 0x72a00200 },                     // movk w0, #0x10, lsl #16
    { 0x94000000, Any::Branch },        // bl _SteamAPI_RestartAppIfNecessary
    { 0x35000000, Any::CondBranch },    // cbnz w0, exit
};
constexpr Derivation kRestartSite{
    .Host = Exact("__ZN3App13InitCommunityEv"),
    .HostShape = { 7116, 0x46205241920965bfull },     // 4.1.1.7631656
    .Anchor = CallsImport("_SteamAPI_RestartAppIfNecessary"),
    .Pattern = kRestartCall,
    .AnchorAt = 2,
    .At = 2,
};

// --- ls::ModuleSettings::IsModded --------------------------------------------
//
// Windows: one function, which says whether a module of the settings is not
// one of the game's own; the patch makes its answer "no" for every caller.
// Here that function has no code of its own. Its loop is inline in every
// caller, around a call of ls::ModuleShortDesc::IsStandardModule() (the
// comparison with the game's own module ids, which other code uses to tell
// mods apart and which is left alone):
//
//     mov  index, #1 ; mov modded, #1
//     ldr  x8, [the settings' modules] ; add x0, x8, offset
//     bl   IsStandardModule(module)
//     tbz  w0, #0, done            a module that is not the game's: leave, "modded" as it stands (1)
//     cmp  index, count
//     cset modded, lo              the last one was the game's too: 0
//     ...  next module
//
// With the tbz gone the loop runs to its end and leaves "modded" 0. The patch
// is that branch in each of the eight functions that hold the loop, so that
// here too every caller is told "no":
//
//   esv::AchievementManager::UnlockAchievement,
//   esv::AchievementManager::IncreaseAchievementCounter and
//   esv::OsirisGameFunctions::UnlockAchievement, over the server's modules:
//       each returns without doing anything when the answer is "modded";
//   esv::GameStateSave::Enter, over the server's modules: the answer is the
//       byte at +0xf8 of the savegame's description, with which it is written;
//   esv::SavegameManager::FinishLoadGame, over the modules of the savegame
//       being loaded: the answer (or that byte, when it is set) is kept in the
//       manager, at +0x87;
//   esv::SavegameManager::ThrowError, twice: over the modules of the savegame,
//       unless that byte is set, and over the server's (see below);
//   gui::DCSavegames::Update, over the modules the savegame screen holds (at
//       +0x1a0 of the object at its +0x9d8): the answer is the screen's
//       SettingsHaveMods property.
//
// IsStandardModule has twelve more callers. None of them holds this loop:
// each does something for every module that is, or is not, the game's own
// (lists them, counts them, loads or removes them), which the Windows patch
// does not change either.
constexpr Named kIsStandardModule = Exact("__ZNK2ls15ModuleShortDesc16IsStandardModuleEv");
constexpr Callee kCallsIsStandardModule = Calls(kIsStandardModule);

// The loop where "modded" is used right after it: the branch leaves to the
// instruction after the loop.
constexpr MaskedWord kModLoopExit[] = {
    { 0x94000000, Any::Branch },        // bl ls::ModuleShortDesc::IsStandardModule
    { 0x360000e0 },                     // tbz w0, #0, +0x1c
    { 0xeb00001f, 0xffe0fc1f },         // cmp xN, xM
    { 0x1a9f27e0, 0xffffffe0 },         // cset wN, lo
};
// The loop that is followed by `b + ; mov modded, #0` for settings without
// modules: the branch leaves past those two.
constexpr MaskedWord kModLoopExitPastNoModules[] = {
    { 0x94000000, Any::Branch },        // bl ls::ModuleShortDesc::IsStandardModule
    { 0x36000120 },                     // tbz w0, #0, +0x24
    { 0xeb00001f, 0xffe0fc1f },         // cmp xN, xM
    { 0x1a9f27e0, 0xffffffe0 },         // cset wN, lo
};
constexpr Derivation kUnlockAchievementSite{
    .Host = Exact("__ZN3esv18AchievementManager17UnlockAchievementERKN2ls11_StringViewIcEEPKNS_9CharacterE"),
    .HostShape = { 256, 0x739c1bd77e45e161ull },     // 4.1.1.7631656
    .Anchor = kCallsIsStandardModule,
    .Pattern = kModLoopExit,
    .AnchorAt = 0,
    .At = 1,
};
constexpr Derivation kAchievementCounterSite{
    .Host = Exact("__ZN3esv18AchievementManager26IncreaseAchievementCounterEPKNS_9CharacterERKN2ls11FixedStringEtb"),
    .HostShape = { 241, 0x570d0a78d95c06ecull },     // 4.1.1.7631656
    .Anchor = kCallsIsStandardModule,
    .Pattern = kModLoopExit,
    .AnchorAt = 0,
    .At = 1,
};
constexpr Derivation kStoryAchievementSite{
    .Host = Exact("__ZN3esv19OsirisGameFunctions17UnlockAchievementEPN3osi18ExecutionParameterE"),
    .HostShape = { 55, 0x08b6feaa97f95ad0ull },     // 4.1.1.7631656
    .Anchor = kCallsIsStandardModule,
    .Pattern = kModLoopExit,
    .AnchorAt = 0,
    .At = 1,
};
constexpr Derivation kSavedAsModdedSite{
    .Host = Exact("__ZN3esv13GameStateSave5EnterEv"),
    .HostShape = { 2691, 0xeb1e28f36470d1d9ull },     // 4.1.1.7631656
    .Anchor = kCallsIsStandardModule,
    .Pattern = kModLoopExitPastNoModules,
    .AnchorAt = 0,
    .At = 1,
};
constexpr Derivation kLoadedAsModdedSite{
    .Host = Exact("__ZN3esv15SavegameManager14FinishLoadGameEv"),
    .HostShape = { 452, 0xd6b5b234cb22b32eull },     // 4.1.1.7631656
    .Anchor = kCallsIsStandardModule,
    .Pattern = kModLoopExit,
    .AnchorAt = 0,
    .At = 1,
};
constexpr Derivation kSaveScreenSite{
    .Host = Exact("__ZN3gui11DCSavegames6UpdateERKN2ls7AnyViewEf"),
    .HostShape = { 933, 0x7846238ebfc0b141ull },     // 4.1.1.7631656
    .Anchor = kCallsIsStandardModule,
    .Pattern = kModLoopExitPastNoModules,
    .AnchorAt = 0,
    .At = 1,
};

// esv::SavegameManager::ThrowError holds the loop twice, so the call does not
// say which: each is found by the load of the modules it goes over, and the
// call in what is found is checked to be the one of IsStandardModule.
//
//     ldr  x8, [x21, #0x28]        the modules of the savegame's description (x21)
//     ldr  x8, [sp, #0x10]         the modules of ls::Module::ToSettings() of the server's module
constexpr Named kThrowError = Exact("__ZN3esv15SavegameManager10ThrowErrorEN3eoc14ESaveGameErrorEb");
constexpr CodeShape kThrowErrorShape{ 163, 0xf2ba76bbe09e4c08ull };     // 4.1.1.7631656
constexpr MaskedWord kSaveModLoopExit[] = {
    { 0xf94016a8 },                     // ldr x8, [x21, #0x28]
    { 0x8b000100, 0xffe0ffff },         // add x0, x8, xN
    { 0x94000000, Any::Branch },        // bl ls::ModuleShortDesc::IsStandardModule
    { 0x360000e0 },                     // tbz w0, #0, +0x1c
    { 0xeb00001f, 0xffe0fc1f },         // cmp xN, xM
    { 0x1a9f27e0, 0xffffffe0 },         // cset wN, lo
};
constexpr MaskedWord kServerModLoopExit[] = {
    { 0xf9400be8 },                     // ldr x8, [sp, #0x10]
    { 0x8b000100, 0xffe0ffff },         // add x0, x8, xN
    { 0x94000000, Any::Branch },        // bl ls::ModuleShortDesc::IsStandardModule
    { 0x360000e0 },                     // tbz w0, #0, +0x1c
    { 0xeb00001f, 0xffe0fc1f },         // cmp xN, xM
    { 0x1a9f27e0, 0xffffffe0 },         // cset wN, lo
};
constexpr Derivation kSaveErrorSaveModsSite{
    .Host = kThrowError,
    .HostShape = kThrowErrorShape,
    .Pattern = kSaveModLoopExit,
    .At = 3,
};
constexpr Derivation kSaveErrorServerModsSite{
    .Host = kThrowError,
    .HostShape = kThrowErrorShape,
    .Pattern = kServerModLoopExit,
    .At = 3,
};
constexpr Operand kSaveModLoopCalls[] = {
    { .What = "the loop's call", .In = kThrowError, .Pattern = kSaveModLoopExit, .At = 2, .How = Operand::Read::BranchTarget,
      .Target = kIsStandardModule },
    { .What = "mod loops", .In = kThrowError, .How = Operand::Read::CallCount, .Expected = 2, .Target = kIsStandardModule },
};
constexpr Operand kServerModLoopCalls[] = {
    { .What = "the loop's call", .In = kThrowError, .Pattern = kServerModLoopExit, .At = 2, .How = Operand::Read::BranchTarget,
      .Target = kIsStandardModule },
    { .What = "mod loops", .In = kThrowError, .How = Operand::Read::CallCount, .Expected = 2, .Target = kIsStandardModule },
};

// --- esv::SavegameManager::ThrowError ----------------------------------------
//
// Windows: `test bl, bl ; jnz + ; call ls::ModuleSettings::HasCustomMods ;
// test al, al ; jz` over one instruction, and the patch removes the jz, so
// that the instruction runs whether or not mods are loaded: a problem in a
// savegame stays a warning. ("Needed to ensure that savegame loader doesn't
// convert warnings to errors in 'unmodded' saves.") Here HasCustomMods is
// inline (it is the second of the two loops above: on Windows the function
// the patch of IsModded changes, which is why this patch is needed), and "no
// mods" is reached from three places (no modules at all, twice, and the end
// of the loop over them), all of which go to
//
//     mov  w21, #1                 no mods: an error not on the list is passed on as an error
//     ldr  x22, [sp, #0x10]
//     cbz  x22, ...
//
// whose counterpart for "mods" is the same three instructions with #0 just
// before it. The site is that `mov w21, #1`, which becomes `mov w21, #0`: one
// instruction covers the three ways there.
constexpr MaskedWord kNoModsArm[] = {
    { 0x52800015 },                     // mov w21, #0          (mods)
    { 0xf9400bf6 },                     // ldr x22, [sp, #0x10]
    { 0xb5000016, Any::CondBranch },    // cbnz x22, ...
    { 0x14000000, Any::Branch },        // b ...
    { 0x52800035 },                     // mov w21, #1          (no mods)
    { 0xf9400bf6 },                     // ldr x22, [sp, #0x10]
    { 0xb4000016, Any::CondBranch },    // cbz x22, ...
};
constexpr Derivation kSaveErrorSite{
    .Host = kThrowError,
    .HostShape = kThrowErrorShape,
    .Pattern = kNoModsArm,
    .At = 4,
};

constexpr Entry kEntries[] = {
    {
        .Name = kRestartThroughSteam,
        .What = Kind::CodeSite,
        .Mac = "the one call of SteamAPI_RestartAppIfNecessary(1086940) in App::InitCommunity(), whose answer decides whether the game exits",
        .Use = "GameHooks/BinaryMappings.xml, the patch of KillLauncher: applied when the DisableLauncher setting is on (LibraryManager::FindLibraries)",
        .Derived = &kRestartSite,
        .Becomes = 0x52800000,      // mov w0, #0
    },
    {
        .Name = kUnlockAchievementGate,
        .What = Kind::CodeSite,
        .Mac = "in esv::AchievementManager::UnlockAchievement(ls::_StringView<char> const&, esv::Character const*): the branch out of the loop "
               "over the server's modules at the first that is not the game's own",
        .Use = "GameHooks/BinaryMappings.xml, the patch of ls::ModuleSettings::IsModded: applied with the EnableAchievements setting "
               "(LibraryManager::ApplyCodePatches)",
        .Derived = &kUnlockAchievementSite,
        .Becomes = kNop,
    },
    {
        .Name = kAchievementCounterGate,
        .What = Kind::CodeSite,
        .Mac = "in esv::AchievementManager::IncreaseAchievementCounter(esv::Character const*, ls::FixedString const&, unsigned short, bool): "
               "the same branch",
        .Use = "GameHooks/BinaryMappings.xml, the patch of ls::ModuleSettings::IsModded",
        .Derived = &kAchievementCounterSite,
        .Becomes = kNop,
    },
    {
        .Name = kStoryAchievementGate,
        .What = Kind::CodeSite,
        .Mac = "in esv::OsirisGameFunctions::UnlockAchievement(osi::ExecutionParameter*): the same branch",
        .Use = "GameHooks/BinaryMappings.xml, the patch of ls::ModuleSettings::IsModded",
        .Derived = &kStoryAchievementSite,
        .Becomes = kNop,
    },
    {
        .Name = kSavedAsModded,
        .What = Kind::CodeSite,
        .Mac = "in esv::GameStateSave::Enter(): the same branch, in the loop over the server's modules whose answer is written with the "
               "savegame (the byte at +0xf8 of its description)",
        .Use = "GameHooks/BinaryMappings.xml, the patch of ls::ModuleSettings::IsModded",
        .Derived = &kSavedAsModdedSite,
        .Becomes = kNop,
    },
    {
        .Name = kLoadedAsModded,
        .What = Kind::CodeSite,
        .Mac = "in esv::SavegameManager::FinishLoadGame(): the same branch, in the loop over the modules of the savegame that was loaded, "
               "whose answer the manager keeps (at +0x87) unless the savegame was written as modded",
        .Use = "GameHooks/BinaryMappings.xml, the patch of ls::ModuleSettings::IsModded",
        .Derived = &kLoadedAsModdedSite,
        .Becomes = kNop,
    },
    {
        .Name = kSaveErrorSaveMods,
        .What = Kind::CodeSite,
        .Mac = "in esv::SavegameManager::ThrowError(eoc::ESaveGameError, bool): the same branch, in the loop over the modules of the savegame",
        .Use = "GameHooks/BinaryMappings.xml, the patch of ls::ModuleSettings::IsModded",
        .Derived = &kSaveErrorSaveModsSite,
        .Operands = kSaveModLoopCalls,
        .Becomes = kNop,
    },
    {
        .Name = kSaveErrorServerMods,
        .What = Kind::CodeSite,
        .Mac = "in esv::SavegameManager::ThrowError(eoc::ESaveGameError, bool): the same branch, in the loop over the server's modules "
               "(the call the Windows function makes to ls::ModuleSettings::HasCustomMods, as BinaryMappings.xml names it)",
        .Use = "GameHooks/BinaryMappings.xml, the patch of ls::ModuleSettings::IsModded",
        .Derived = &kSaveErrorServerModsSite,
        .Operands = kServerModLoopCalls,
        .Becomes = kNop,
    },
    {
        .Name = kSaveScreenHasMods,
        .What = Kind::CodeSite,
        .Mac = "in gui::DCSavegames::Update(ls::AnyView const&, float): the same branch, in the loop whose answer is the savegame screen's "
               "SettingsHaveMods property",
        .Use = "GameHooks/BinaryMappings.xml, the patch of ls::ModuleSettings::IsModded",
        .Derived = &kSaveScreenSite,
        .Becomes = kNop,
    },
    {
        .Name = kSaveErrorsWithoutMods,
        .What = Kind::CodeSite,
        .Mac = "in esv::SavegameManager::ThrowError(eoc::ESaveGameError, bool): the instruction that says no mods are loaded",
        .Use = "GameHooks/BinaryMappings.xml, the patch of esv::SavegameManager::ThrowError: applied with the patch of IsModded",
        .Derived = &kSaveErrorSite,
        .Becomes = 0x52800015,      // mov w21, #0
    },
};

}

std::span<Entry const> PatchEntries()
{
    return kEntries;
}

}
