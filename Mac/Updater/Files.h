#pragma once

#include <cstdint>
#include <span>
#include <string>

// File operations of the updater. Every file it keeps is written to a new
// temporary file, synced and renamed into place, so that a crash, a kill or
// a full disk leaves either the old file or the new one, never a part.

namespace bg3se::mac::updater {

enum class ReadResult
{
    Ok,
    Missing,
    TooLarge,
    Failed,
};

ReadResult ReadFile(std::string const& path, std::string& out, uint64_t limit);
// Writes `bytes` to <path>.tmp-<pid>, syncs it, renames it over `path` and
// syncs the directory. `why` says what failed.
bool WriteFileAtomically(std::string const& path, std::span<uint8_t const> bytes, std::string& why);
bool MakeDirectories(std::string const& path, std::string& why);
// Removes a directory and everything under it; symbolic links are removed, not followed.
bool RemoveTree(std::string const& path);
bool SyncDirectory(std::string const& path);
std::string Errno();

}
