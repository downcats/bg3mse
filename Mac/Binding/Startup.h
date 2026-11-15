#pragma once

#include <optional>
#include <string>

#include <Binding/Layouts.h>
#include <Binding/StaticSymbolBinding.h>
#include <Engine/EngineResolver.h>

// Starting Norbyte's Script Extender in the game: what DllMain and
// SetupScriptExtender (Extender/Shared/dllmain.cpp) do on Windows, after the
// checks the macOS game needs first.
//
// In this order, and stopping at the first that fails, with the reason in the
// log and nothing of the game changed:
//
//   1. the rows of the binding hold together with the engine table;
//   2. the loaded game's symbols and functions can be read;
//   3. the engine table is resolved, before any hook is on the game (a hooked
//      function no longer has its recorded shape);
//   4. every critical member of StaticSymbols would be bound, and every hook
//      of this layer that such a member needs would go on; a member that
//      leads to a structure whose layout is not proven on this game
//      (Binding/Layouts.h) counts as one that would not be;
//   5. the game's lock functions are confirmed (ConfirmGameLocks): Norbyte's
//      code takes the game's locks from its first moments;
//   6. this is the game's process and no extender runs in it yet
//      (ShouldInitializeExtender).
//
// Then the hooks of this layer go on the game (AttachGameHooks), and
// SetupScriptExtender runs: it loads the configuration and runs
// ScriptExtender::Initialize, whose LibraryManager::FindLibraries fills
// StaticSymbols through BindLibraries (Binding/Libraries.h).
//
// The sequence has two calls, because other parts of bg3mse hook the game
// too. Steps 1 to 4 (PrepareScriptExtender) read the game's code as it was
// shipped, and must run before anybody's hook is on it: a hooked function no
// longer has the shape the table recorded, and its entry would be refused.
// The rest (StartScriptExtender) may run after other hooks are on, and takes
// account of them:
//
//   - the platform's hook on the file reader's constructor
//     (Platform/FileReaderHook.h): when it is on, StaticSymbols gets that
//     hook's function for ls__FileReader__ctor, and Norbyte's own hook goes
//     on it instead of on the game's constructor;
//   - the overlay is handed the same resolution (Overlay/Dependencies.h), so
//     that it resolves nothing itself after hooks are on.
//
// One resolution serves everyone: StaticSymbols, the hooks of this layer, the
// code patches and the overlay.

namespace bg3se::mac::binding {

enum class StartRefusal : uint8_t
{
    None,
    AlreadyStarted,
    RowsDisagree,
    GameUnreadable,
    CriticalMember,
    GameLocks,
    NotTheGameOrAlreadyRunning,
    HooksFailed,
    Count
};

struct Preconditions
{
    StartRefusal Refusal{ StartRefusal::None };
    std::string Reason;
    bool Ok() const { return Refusal == StartRefusal::None; }
};

// Steps 1 and 4 for a resolution, which may be one of binaries mapped from
// disk: what start-up would conclude from the table on that game, given what
// the game's layouts keep back of StaticSymbols (LayoutsWithheldOn).
Preconditions CheckResolution(engine::Resolution const& resolution, LayoutsWithheld const& layouts);

// Steps 1 to 4, in the game process, before any hook is on the game. Changes
// nothing of the game. Returns the refusal, or None; a second call returns
// what the first did.
StartRefusal PrepareScriptExtender();

// The rest of the sequence. Returns the refusal, or None when
// SetupScriptExtender has run. Without a PrepareScriptExtender before it, it
// prepares first: right only when nothing has hooked the game yet.
StartRefusal StartScriptExtender();

char const* Describe(StartRefusal refusal);

// --- what the player is told -------------------------------------------------
//
// On Windows a game the extender does not support stops at once: Fail()
// (CoreLib/Utils.cpp) shows "BG3 Script Extender Error" in a message box and
// ends the process. A start-up that failed otherwise is told in the main menu
// (LibraryManager::ShowStartupError), which a refusal here, made before
// Norbyte's code runs, never reaches. So a refusal that means the game is not
// one this build can work with is told in that message box, with the text
// Norbyte's code has for an unsupported version (ScriptExtender::PostStartup),
// and a refusal of this layer's own making with his text for a critical
// failure; then the game goes on without the extender. Nothing is told when
// this is not the game, or the extender is already there.

struct RefusalNotice
{
    std::string Caption;
    std::string Text;
};

// The notice for a refusal, or nothing; `gameVersion` as GameVersionString()
// gives it (empty when the bundle has none). A game this build cannot work
// with is also told what to do (Release/GameAdvice.h), from `checkedGame`,
// the game version the release was checked against (empty when not known),
// and whether the release updates itself.
std::optional<RefusalNotice> NoticeOf(StartRefusal refusal, std::string const& gameVersion, std::string const& checkedGame = {},
    bool selfUpdating = true);

// Shows NoticeOf(refusal) as a message box, from a thread of its own, so that
// the game's start-up does not wait for the player; logs what was shown. At
// most once per process.
void ShowRefusalNotice(StartRefusal refusal);

// After a crash of the extender, Windows starts CrashReporter.exe, whose
// dialog ("Script Extender Crash") says "The game has unexpectedly crashed."
// and shows the backtrace, asking to send the report, which bg3mse never
// does. Here the next start of the game says so: that the game crashed, the
// report's file and its backtrace (crash::TakeCrashNotice), in the log, the
// console when there is one, and a dialog, once for each crash. The dialog is
// a system alert shown from a thread of its own as the extender starts, while
// the dylib is initialised and before the game has a window; the game goes
// on starting behind it. bg3mse.conf turns it off with
//
//   crash_dialog = 0
//
// Called by StartScriptExtender; the game's own crash reporter process (-crash)
// never starts the extender, so never takes the notice.
inline constexpr char kCrashDialogSetting[] = "crash_dialog";
void TellOfLastCrash();

// --- between the sequence and BindLibraries ---------------------------------

// The resolution StaticSymbols is to be filled from: set by the sequence
// once the hooks are on, or by a test. nullptr takes it away.
void SetResolutionForLibraries(engine::Resolution const* resolution);
// What was set, for the other users of the binding (Binding/OsirisBinding.h, Binding/CodePatches.h); nullptr when nothing is.
engine::Resolution const* ResolutionForLibraries();

}
