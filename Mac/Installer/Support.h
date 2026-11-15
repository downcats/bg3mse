#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <sys/types.h>
#include <vector>

// File and process helpers of bg3mse-install's verbs (Installer.cpp, Setup.cpp).

namespace bg3se::mac::installer {

std::string Errno();
std::string Quoted(std::string const& s);
// Whether anything is at the path (a link is not followed).
bool Exists(std::string const& path);
bool IsDirectory(std::string const& path);
std::string DirectoryOf(std::string const& path);
std::string NameOf(std::string const& path);
// The path from the root with no link in it, also when only its folder is
// there yet; the path as given when neither is.
std::string ResolvedPath(std::string const& path);

// A file mapped read-only.
class Mapped
{
public:
    Mapped() = default;
    Mapped(Mapped const&) = delete;
    Mapped& operator=(Mapped const&) = delete;
    ~Mapped() { Close(); }

    // False with the reason when the file is there and cannot be read; a missing file is no error.
    bool Open(std::string const& path, std::string& problem);
    bool Present() const { return present_; }
    std::span<uint8_t const> Bytes() const { return { data_, size_ }; }

private:
    void Close();

    uint8_t const* data_{ nullptr };
    size_t size_{ 0 };
    bool present_{ false };
};

bool SameBytes(std::span<uint8_t const> a, std::span<uint8_t const> b);
// Whether the two files hold the same bytes (false when either cannot be read).
bool SameFile(std::string const& a, std::string const& b);

bool FullSync(int fd);
void SyncDirectory(std::string const& path);

// `bytes` written to <path><kTemporarySuffix>, synced, read back, and renamed
// over `path`: the file is either the old one or the new one, never a part.
// Only the bytes: no extended attribute (a download's quarantine mark among
// them) is carried.
bool WriteAtomically(std::string const& path, std::span<uint8_t const> bytes, mode_t mode, std::string& problem);

// Removes a directory and everything under it; links are removed, not followed.
bool RemoveTree(std::string const& path);

// One run of the installer at a time on the data directory: an exclusive
// lock on the directory, taken without waiting.
class DataLock
{
public:
    explicit DataLock(std::string const& data);
    DataLock(DataLock const&) = delete;
    DataLock& operator=(DataLock const&) = delete;
    ~DataLock();
    // True also when there is no data directory yet: nothing to share.
    bool Held() const { return held_ || fd_ < 0; }

private:
    int fd_{ -1 };
    bool held_{ false };
};

// Runs a program, its output collected; the exit status, or -1 when it did
// not run or end normally. `input` is written to its standard input.
int RunProgram(std::vector<std::string> const& args, std::string& output, std::string const* input = nullptr,
    std::vector<std::string> const& extraEnvironment = {});

}
