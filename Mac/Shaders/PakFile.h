#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Reads a Larian package (LSPK v18) from disk. Mod paks are untrusted input:
// every size and offset is checked against the file before it is used.

namespace bg3se::mac {

class PakFile
{
public:
    struct Entry
    {
        std::string Name;
        uint64_t Offset{ 0 };
        uint32_t DiskSize{ 0 };
        uint32_t Size{ 0 };
        uint8_t Compression{ 0 };
        uint8_t Part{ 0 };
    };

    // nullptr when the file is not a readable v18 package; `error` says why.
    static std::unique_ptr<PakFile> Open(std::string const& path, std::string* error = nullptr);
    ~PakFile();

    PakFile(PakFile const&) = delete;
    PakFile& operator = (PakFile const&) = delete;

    std::string const& Path() const { return path_; }
    std::string const& FileName() const { return fileName_; }
    uint8_t Priority() const { return priority_; }
    std::vector<Entry> const& Entries() const { return entries_; }

    Entry const* Find(std::string_view name) const;

    // The entry's contents, or nothing when it cannot be read exactly.
    std::optional<std::vector<uint8_t>> Read(Entry const& entry) const;

private:
    PakFile() = default;

    std::string path_;
    std::string fileName_;
    int fd_{ -1 };
    uint64_t fileSize_{ 0 };
    uint8_t priority_{ 0 };
    std::vector<Entry> entries_;
    std::unordered_map<std::string_view, uint32_t> index_;
};

}
