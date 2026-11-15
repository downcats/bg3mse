#pragma once

#include <Binding/HookBridges.h>

// The extender's end when the game exits: what DllMain does for
// DLL_PROCESS_DETACH on Windows (Extender/Shared/dllmain.cpp):
// gExtender->Shutdown(), which resets the server's and the client's extension
// states (their Lua states get the Shutdown event and are closed), ends the
// story layer's debugger and takes its hooks out, and takes the engine hooks
// out; then gExtender.reset(), whose destructors end the rest (the overlay's
// ImGui context, which writes its settings file).
//
// On Windows that runs after ExitProcess has ended every other thread. The
// macOS game ends two ways, and the shutdown runs once, at whichever comes:
//
//   - The player quits (the menus' Quit, in a session or not; Cmd+Q, through
//     -[BG3Application terminate:]; the window's close button, through
//     -[AppDelegate close]): each comes to ecl::EocClient::RequestQuit on the
//     game thread, whose StartTerminatingApp calls App::Terminate(0), which
//     disconnects the peers, flushes the game's logs and calls _Exit with
//     every thread still running and nothing torn down. The hook on its entry
//     (Binding/HookBridges.h, GameExit::Quit) runs this there, on the game
//     thread. The server's part (its extension state reset, so its Lua state
//     gets the Shutdown event and is closed; the story layer ended) is queued
//     to the server's thread, which runs it at the start of its next tick as
//     it runs a Lua reset; a server that has not ticked within a second (in
//     the main menu it may wait without ticking) has it done on the game
//     thread instead, and is held at its tick should it reach one meanwhile.
//     The client's part is done here, on the thread that runs its Lua state,
//     unless the quit came from inside that state (then it only gets the
//     Shutdown event). The engine hooks stay in and the extender object
//     stays: other threads run them.
//   - The run loop of -[AppDelegate launchGame] ends (the client's Exit state,
//     through Eoc::Shutdown): BaseApp::Stop, whose BaseApp::ExitEngine joins
//     the game thread and terminates and destroys every worker thread; the
//     App's destructor; the window manager's; ls::StaticDataManager::
//     Shutdown(), which ends the static data managers and then the core
//     library; and _exit(0). The hook on the entry of ls::StaticDataManager::
//     Shutdown (GameExit::Teardown) runs this there, on the main thread:
//     gExtender->Shutdown() and gExtender.reset(), as DllMain does.
//
// The engine table checks each function and what the moment rests on
// (Engine/Entries/Startup.cpp); when it does not, the hook is not made and
// nothing runs.
//
// Where the macOS game differs from Windows here, other threads still being
// alive: a thread the shutdown waits for that does not end (the story
// debugger's, while a debugger is connected) would keep the game from exiting, so after ten seconds the game is
// made to exit (_exit(0)); and the Lua debugger's thread, which nothing ends,
// uses the extender object, so with the Lua debugger on the object is not
// destroyed.
//
// The other ways out of the game, where this does not run:
//   - BaseApp::Terminate and -[LSMTLView windowDidChangeScreen:] call _Exit;
//     the start-up's failure alert in launchGame calls _exit.
//   - exit() (App::InitCommunity, OpenSSL's configuration, os_exit, and
//     AppKit's NSTerminateNow before the App exists): static destructors run,
//     gExtender's among them, without Shutdown, while other threads may run.
//   - a crash: the crash reporter writes its report; nothing is shut down.
//
// The extender's own logs are not lost on any path: every line is flushed
// when it is written (Platform/Log.cpp, Norbyte's Console::LocalPrint), and a
// fault's line goes out with write(2). The C library's streams (the story log,
// which the game's COsiris writes through fopen) are flushed here.

namespace bg3se::mac::binding {

// The extender's shutdown for the way the game ends (above), once; nothing
// when the extender never started. A fault inside is recovered and logged,
// and so is a C++ exception: the game then exits as it would have, without
// its crash reporter being started on the way out. After ten seconds without
// an end, the process exits (_exit(0)).
void ShutdownScriptExtender(GameExit how);

}
