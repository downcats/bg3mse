#pragma once

// Which process a dylib was loaded into. The bg3mse dylibs start only in the
// game: any other process that loads them (a tool, a test, the game's own
// crash reporter) is left alone. The decision is made here and nowhere else:
// libbg3mse.dylib's start-up (Mac/Main.cpp), the camera module's
// (Mac/Camera/CameraModule.cpp) and Norbyte's ShouldInitializeExtender
// (Extender/Shared/dllmain.cpp) all ask IsGameProcess().
//
// The game's crash reporter is the game's own executable. When the game
// crashes, the callback it gave its crash handler (CrashReportCallback) starts
// that executable again with the one argument "-crash". In the new process
// -[AppDelegate applicationDidFinishLaunching:] compares every argument, the
// program's name included, with "-crash" without regard to case (strcasecmp);
// when one is equal it shows the crash reporter's window (showCrashReporter)
// and the process never runs the game. It looks at no other argument: without
// "-crash" the process shows the launcher, or skips it, and runs launchGame in
// both cases. (As read in the game's code, build 4.1.1.7631656.) Such a
// process loads everything the game loads, the bg3mse dylibs included.

namespace bg3se::mac {

// True when the main executable is the game's ("Baldur's Gate 3") and the
// process was not started as the game's crash reporter.
bool IsGameProcess();

// The game's own test of a command line, as described above: whether a
// process of the game's executable started with it is the crash reporter.
bool IsCrashReporterCommandLine(int argc, char const* const* argv);

// --- the two ways the game loads libbg3mse.dylib ------------------------------
//
// The installer's load command (Mac/Installer/Installer.h) makes the dylib the
// last library the game's executable depends on: dyld initializes it after
// every library the game brings, and before the game's own initializers and
// main(). A launch option that names it in DYLD_INSERT_LIBRARIES leaves the
// game's executable as Steam installed it; dyld then initializes the inserted
// dylib before the game's own libraries (libOsiris.dylib, the Steam and
// PlayFab libraries, and the system frameworks only the game links), which
// it has mapped but not initialized yet.

// Whether `image` (a path dyld gave) is one of the libraries named in
// DYLD_INSERT_LIBRARIES.
bool InsertedByEnvironment(char const* image);

// Has dyld initialize every library loaded with the game now, before the
// caller goes on, as it has when the dylib is the last of the game's
// libraries: what an inserted dylib, or one an inserted image opens from its
// initializer, needs. A library already initialized is not initialized again. `self` is any address in the calling image, which
// is left alone, as is the game's executable. Returns how many were asked.
unsigned InitializeLibrariesLoadedBefore(void const* self);

// Whether the calling image is the copy of its library that starts: the
// first, in dyld's order, of the loaded images with the install name of the
// image that holds `self`. A second copy (the load command and the variable
// naming two different files) is not, and must do nothing.
bool FirstCopyOfThisLibrary(void const* self);

// --- the updater ---------------------------------------------------------------
//
// The updater is libbg3mse-loader.dylib (Mac/Updater/Loader.cpp), the Mac
// form of Norbyte's DWrite.dll: the game loads it, and it opens the build it
// chose with dlopen. Norbyte's extender tells that it was loaded that way by
// its own file name (ScriptExtender::WarnIfOffline): the updater's copy is
// BG3ScriptExtender.dll, an offline package's is DWrite.dll.
inline constexpr char kUpdaterFileName[] = "libbg3mse-loader.dylib";

// Whether the image that holds `self` was loaded by the updater: the updater
// is loaded, and the image is neither one the game's executable names in a
// load command nor one of DYLD_INSERT_LIBRARIES. When the extender is loaded
// either of those ways the updater loads nothing (it never loads a second
// copy) and fetches nothing.
bool LoadedThroughUpdater(void const* self);

// --- native modules ------------------------------------------------------------
//
// Norbyte's native API (Extender/Shared/APIExports.h) hands a native module
// the extender's module: SEUpdaterGetModule, and the callback of
// SEUpdaterNotifyWhenLoaded. On Windows that is an HMODULE, which the module
// gives GetProcAddress to find the API's other functions. Here it is what
// such a module gives dlsym: a handle of dlopen. The updater and the
// extender give the same kind.

// A dlopen handle of the image that holds `self`, loaded as it is: nothing is
// loaded for it. The one reference taken is kept, the image being one that
// is never unloaded. Null when dyld has no path for the image or does not
// open it by that path.
void* NativeModuleHandle(void const* self);

}
