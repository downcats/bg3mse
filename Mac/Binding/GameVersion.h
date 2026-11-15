#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The game's version, for Norbyte's LibraryManager::GetGameVersion and
// GameVersionInfo::IsSupported (GameHooks/DataLibraries.h), outside MSVC.
//
// On Windows the version is the executable's version resource, four 16-bit
// numbers such as 4.69.95.x, and a game is supported from a certain version
// on. The macOS game has no such resource and numbers of its own: its version
// is its bundle's version string (4.1.1.7631656 for the build this was
// written with), whose last part is a build number. What a version number
// promises on Windows, this build checks on the game itself: the game is
// supported when the engine table resolved on it, which the start-up
// sequence requires before Norbyte's code runs at all (Mac/Binding/Startup.h).

namespace bg3se::mac::binding {

struct GameVersion
{
    uint32_t Major{ 0 }, Minor{ 0 }, Revision{ 0 }, Build{ 0 };
};

// A version string of one to four numbers separated by dots; the parts it
// does not have are 0. Nothing for a string that is anything else (an empty
// part, a character that is no digit, a number above 32 bits).
std::optional<GameVersion> ParseGameVersion(std::string_view text);

// The version string of the bundle this process runs from
// (CFBundleShortVersionString, else CFBundleVersion); empty when it has none.
std::string GameVersionString();

// The game's version, or nothing when the bundle has no version that parses.
std::optional<GameVersion> ReadGameVersion();

// The engine table resolved on this game and StaticSymbols is bound from it
// (the start-up sequence got as far as running Norbyte's code).
bool GameSupported();

}
