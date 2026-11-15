// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from ersh1's BG3 Native Camera Tweaks: https://github.com/ersh1/BG3_NativeCameraTweaks

#include <Camera/camera_tweaks.h>
#include <Platform/Config.h>
#include <Platform/Log.h>
#include <Platform/Process.h>
#include <Release/Version.h>

#include <cstdlib>
#include <exception>
#include <string>
#include <sys/stat.h>

// Entry point of libbg3mse-camera.dylib. libbg3mse.dylib loads it, when it is
// installed next to it, from its own constructor; this then runs before the
// game's main(), as the camera hooks require.

namespace {

using namespace bg3se::mac;

void Initialize()
{
    if (getenv("BG3MSE_DISABLE") || !IsGameProcess()) return;

    std::string support = DataDirectory();
    mkdir(support.c_str(), 0755);
    SetLogDebug(LogDebugWanted());
    OpenLogFile(support + "/logs", "camera");

    Log(LogLevel::Info, "bg3mse camera module %s loaded (commit %.12s)", version::kVersion, version::kCommit);
    camera_tweaks_init();            // NativeCameraTweaks.toml decides the rest
}

// An exception leaving a dylib initialiser aborts the game before it starts.
__attribute__((constructor))
void InitializeGuarded()
{
    try {
        Initialize();
    } catch (std::exception const& e) {
        Log(LogLevel::Error, "start-up failed: %s", e.what());
    } catch (...) {
        Log(LogLevel::Error, "start-up failed");
    }
}

}
