#pragma once

#include <Engine/EngineTable.h>

// The areas of the engine table, one source file each.
//
// Every recorded shape in them is of game build 4.1.1.7631656 (arm64; the
// executable's LC_UUID is 3B6F6492-114E-3EA2-BC64-DFDDBD361434), and says so
// on its line. A shape is a check, never a way to find anything: when a game
// update changes a function, its entry is refused until the function has been
// read again and the entry's statements about it confirmed.

namespace bg3se::mac::engine {

std::span<Entry const> CoreLibraryEntries();
std::span<Entry const> StartupEntries();
std::span<Entry const> StatsEntries();
std::span<Entry const> WorldEntries();
std::span<Entry const> UiEntries();
std::span<Entry const> OsirisEntries();
std::span<Entry const> OverlayEntries();
std::span<Entry const> PatchEntries();

}
