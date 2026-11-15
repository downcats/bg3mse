#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Reads a release's zip (Mac/Release/Zip.cmake: files only, stored or
// deflated, no zip64), for the updater to unpack. Every entry's name,
// sizes and CRC are checked; nothing outside the archive is read or written
// by name of an entry: the caller decides where each one goes.

namespace bg3se::mac::updater {

struct ZipEntry
{
    std::string Name;
    uint16_t Method{ 0 };
    uint32_t Crc{ 0 };
    uint64_t CompressedSize{ 0 };
    uint64_t Size{ 0 };
    uint64_t LocalHeader{ 0 };
    uint32_t Mode{ 0 };           // the Unix mode, when the archive was made on Unix
};

class ZipReader
{
public:
    // The whole archive, in memory (it was just downloaded and hashed).
    bool Open(std::span<uint8_t const> archive, std::string& why);
    std::vector<ZipEntry> const& Entries() const { return entries_; }
    // Writes the entry's contents to `path` (created, never there before) and syncs it.
    bool Extract(ZipEntry const& entry, std::string const& path, std::string& why) const;
    // The entry's contents, in memory.
    bool Read(ZipEntry const& entry, std::string& out, std::string& why) const;

private:
    // Inflates the entry into `sink`, checking its size and CRC.
    template<class Sink> bool Inflate(ZipEntry const& entry, Sink&& sink, std::string& why) const;
    std::span<uint8_t const> data_;
    std::vector<ZipEntry> entries_;
};

}
