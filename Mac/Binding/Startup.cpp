#include <stdafx.h>

#include <Binding/GameVersion.h>
#include <Binding/HookBridges.h>
#include <Binding/Shutdown.h>
#include <Binding/Startup.h>
#include <Crash/CrashReporter.h>
#include <Engine/EngineTable.h>
#include <Layout/GameLockCalls.h>
#include <Overlay/ConsoleWindow.h>
#include <Overlay/Dependencies.h>
#include <Platform/Config.h>
#include <Platform/DebugLog.h>
#include <Platform/FileReaderHook.h>
#include <Platform/Log.h>
#include <Release/GameAdvice.h>

#include <mutex>

#include <atomic>
#include <dlfcn.h>
#include <thread>

// The one file of the binding that calls Norbyte's code: the two functions of
// Extender/Shared/dllmain.cpp, which are in namespace bg3se outside MSVC.

namespace bg3se {

void SetupScriptExtender(HMODULE hModule);
bool ShouldInitializeExtender();

}

BEGIN_NS(mac::binding)

namespace {

std::atomic<bool> gStarted{ false };

// What the sequence resolved, kept for as long as the process runs:
// StaticSymbols and the hooks hold addresses and names from it.
struct Game
{
    engine::LoadedGame Loaded;
    engine::Resolution Resolution;
    // What depends on a layout the game was not found to have: found once,
    // here, before anybody's hook is on the game.
    LayoutsWithheld Layouts;
};

// Set by PrepareScriptExtender, once.
std::mutex gPrepareMutex;
bool gPrepared = false;
StartRefusal gPrepareRefusal = StartRefusal::None;
Game* gGame = nullptr;

StartRefusal Refuse(StartRefusal refusal, std::string const& reason)
{
    Log(LogLevel::Error, "Script Extender not started: %s%s%s", Describe(refusal), reason.empty() ? "" : ": ", reason.c_str());
    return refusal;
}

// What CoreLib's own Debug and DebugLocal do with a message (CoreLib/Utils.cpp):
// the console object's, which also writes it to the log (Platform/DebugLog.h).
bool ToExtenderConsole(int type, char const* text, bool local)
{
    auto console = gCoreLibPlatformInterface.GlobalConsole;
    if (!console) return false;
    if (local) {
        console->LocalPrint(static_cast<DebugMessageType>(type), text);
    } else {
        console->Print(static_cast<DebugMessageType>(type), text);
    }
    return true;
}

// The image this code is in: what DllMain receives as its module.
HMODULE ThisImage()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void const*>(&ThisImage), &info)) return NULL;
    return reinterpret_cast<HMODULE>(info.dli_fbase);
}

}

StartRefusal PrepareScriptExtender()
{
    std::lock_guard lock(gPrepareMutex);
    if (gPrepared) return gPrepareRefusal;
    gPrepared = true;
    // Until the end is reached: an exception on the way leaves a refusal behind.
    gPrepareRefusal = StartRefusal::GameUnreadable;

    // Never freed: see Game.
    auto game = new Game();
    std::string problem;
    if (!game->Loaded.Open(problem)) return gPrepareRefusal = Refuse(StartRefusal::GameUnreadable, problem);

    // Before any hook of anybody's is on the game.
    game->Resolution = engine::Resolve(engine::EngineTable(), game->Loaded.View());
    game->Layouts = LayoutsWithheldOn(game->Resolution);
    for (auto const& why : game->Layouts.UnconfirmedClasses) Log(LogLevel::Warning, "[Layout] %s", why.c_str());
    if (!game->Layouts.MembersUnchecked.empty()) {
        Log(LogLevel::Warning, "[Layout] the members of component types are not compared with the game's: %s", game->Layouts.MembersUnchecked.c_str());
    }
    if (!game->Layouts.MembersPartlyUnchecked.empty()) {
        Log(LogLevel::Warning, "[Layout] the members of component types are compared without some of the game's functions: %s",
            game->Layouts.MembersPartlyUnchecked.c_str());
    }
    auto check = CheckResolution(game->Resolution, game->Layouts);
    if (!check.Ok()) return gPrepareRefusal = Refuse(check.Refusal, check.Reason);
    KeepLayoutsOfTheGame(&game->Layouts);

    gGame = game;
    return gPrepareRefusal = StartRefusal::None;
}

StartRefusal StartScriptExtender()
{
    if (gStarted.exchange(true)) return Refuse(StartRefusal::AlreadyStarted, {});

    // The reason was logged when it was found.
    if (auto refusal = PrepareScriptExtender(); refusal != StartRefusal::None) return refusal;
    auto game = gGame;

    if (!ConfirmGameLocks()) return Refuse(StartRefusal::GameLocks, {});

    // DllMain: the game's process, and one extender in it.
    if (!ShouldInitializeExtender()) return Refuse(StartRefusal::NotTheGameOrAlreadyRunning, {});

    // From here the game is changed.
    std::string report;
    bool attached = AttachGameHooks(game->Resolution, report);
    if (!report.empty()) Log(attached ? LogLevel::Warning : LogLevel::Error, "hooks on the game:\n%s", report.c_str());
    if (!attached) return Refuse(StartRefusal::HooksFailed, "what went on was taken out again, except what the lines above name");

    SetResolutionForLibraries(&game->Resolution);
    // The file reader's constructor may carry the platform's hook by now:
    // that hook's function is then the constructor Norbyte's code calls and hooks.
    SetHookedEntry("ls__FileReader__ctor", FileReaderHookFunction());
    // The overlay checks what it depends on in this same resolution.
    overlay::SetGameResolution(&game->Resolution);
    // Without a terminal, the console Norbyte's code makes is a window of the overlay.
    overlay::EnableConsoleWindow();

    // DllMain, DLL_PROCESS_ATTACH. The DirectWrite proxy it creates has no
    // counterpart here: the extender is not loaded in place of a system library.
    HMODULE module = ThisImage();
    gCoreLibPlatformInterface.ThisModule = module;
    // From here messages go through Norbyte's console object, once it exists.
    SetDebugConsoleSink(&ToExtenderConsole);
    SetupScriptExtender(module);
    // What Windows shows after a crash of the extender, now that the console is there.
    TellOfLastCrash();
    // DllMain, DLL_PROCESS_DETACH: when the game exits (Binding/Shutdown.h).
    SetGameExitHandler(&ShutdownScriptExtender);
    return StartRefusal::None;
}

void TellOfLastCrash()
{
    // The directory ReportDirectory gives the reporter (Crash/ExtenderCrashReporting.cpp).
    auto notice = crash::TakeCrashNotice(DataDirectory() + "/Crashes");
    if (!notice) return;
    // The log, and the console when there is one (CreateConsole), are told,
    // and the user in a dialog as on Windows unless bg3mse.conf says no.
    Log(LogLevel::Warning, "Script Extender: %s", notice->c_str());
    auto setting = ConfValue(kCrashDialogSetting);
    if (setting && !FlagValue(*setting)) return;
    // CrashReporter.exe's title. The system shows the alert
    // (CFUserNotification), so it needs no window of the game's, and from a
    // thread of its own: the game's start-up goes on behind it.
    std::thread([text = std::move(*notice)] {
        if (MessageBoxA(NULL, text.c_str(), "bg3mse", MB_OK | MB_ICONWARNING) == 0) {
            Log(LogLevel::Warning, "Script Extender: the crash notice could not be shown (error %u)", (unsigned)GetLastError());
        }
    }).detach();
}

void ShowRefusalNotice(StartRefusal refusal)
{
    static std::atomic<bool> shown{ false };
    // The release's manifest.json, beside this image in bin/ and in the updater's cache.
    std::string checked;
    bool selfUpdating = true;
    Dl_info self{};
    if (dladdr(reinterpret_cast<void const*>(&ShowRefusalNotice), &self) && self.dli_fname) {
        std::string path = self.dli_fname;
        path = path.substr(0, path.rfind('/') + 1) + "manifest.json";
        if (FILE* f = fopen(path.c_str(), "rb")) {
            char text[64 * 1024];
            size_t n = fread(text, 1, sizeof(text), f);
            fclose(f);
            checked = CheckedGameVersion(std::string_view(text, n));
            selfUpdating = SelfUpdating(std::string_view(text, n));
        }
    }
    if (FILE* f = fopen((DataDirectory() + "/ScriptExtenderUpdaterConfig.json").c_str(), "rb")) {
        char text[64 * 1024];
        size_t n = fread(text, 1, sizeof(text), f);
        fclose(f);
        if (UpdatesDisabled(std::string_view(text, n))) selfUpdating = false;
    }
    auto notice = NoticeOf(refusal, GameVersionString(), checked, selfUpdating);
    if (!notice || shown.exchange(true)) return;
    Log(LogLevel::Info, "Script Extender: telling the player: %s", notice->Text.c_str());
    // MessageBox returns when the player has answered; the game does not wait
    // for that, as it does not on Windows for a start-up error in the menu.
    std::thread([notice = std::move(*notice)] {
        if (MessageBoxA(NULL, notice.Text.c_str(), notice.Caption.c_str(), MB_OK | MB_ICONERROR) == 0) {
            Log(LogLevel::Warning, "Script Extender: the message could not be shown (error %u)", (unsigned)GetLastError());
        }
    }).detach();
}

END_NS()
