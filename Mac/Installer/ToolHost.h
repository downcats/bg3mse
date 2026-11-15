#pragma once

#include <Installer/Installer.h>

// What the installer's verbs are given on a player's Mac (Installer.h, Host),
// by bg3mse-install and by the installer app alike: this Mac, bg3mse's data
// directory, where Steam puts the game, the running games, whether bg3mse
// supports a game, and the game's own Metal functions. Neither the clipboard
// nor a terminal: each tool gives its own.

namespace bg3se::mac::installer {

Host ToolHost();

// Whether bg3mse supports the game: what `bg3mse-engine check` says of it.
// Every entry of the engine table is as the table expects on the game's
// arm64 executable and libOsiris.dylib, both read from disk.
bool SupportsGame(std::span<uint8_t const> executable, std::span<uint8_t const> osiris, std::string& why);

}
