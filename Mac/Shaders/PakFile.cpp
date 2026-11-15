#include <stdafx.h>
#include <Shaders/PakFile.h>

#include <fcntl.h>
#include <lz4.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace bg3se::mac {

namespace {

constexpr uint32_t kSignature = 0x4b50534c;     // "LSPK"
constexpr uint32_t kVersion = 18;
constexpr size_t kHeaderSize = 40;
constexpr size_t kEntrySize = 272;
constexpr size_t kNameSize = 256;
constexpr uint64_t kDeletedOffset = 0xbeefdeadbeefull;
constexpr uint64_t kMaxFileListSize = 512ull << 20;
constexpr uint32_t kMaxEntrySize = 256u << 20;

enum : uint8_t { kStored = 0, kZlib = 1, kLz4 = 2 };

template <class T>
T ReadLE(uint8_t const* p)
{
    T v;
    memcpy(&v, p, sizeof(v));
    return v;
}

bool ReadAt(int fd, uint64_t offset, void* out, size_t size)
{
    auto p = static_cast<uint8_t*>(out);
    while (size) {
        ssize_t n = pread(fd, p, size, (off_t)offset);
        if (n <= 0) return false;
        p += n;
        offset += (uint64_t)n;
        size -= (size_t)n;
    }
    return true;
}

bool Reject(std::string* error, char const* why)
{
    if (error) *error = why;
    return false;
}

}

PakFile::~PakFile()
{
    if (fd_ >= 0) close(fd_);
}

std::unique_ptr<PakFile> PakFile::Open(std::string const& path, std::string* error)
{
    std::unique_ptr<PakFile> pak(new PakFile());
    pak->path_ = path;
    auto slash = path.rfind('/');
    pak->fileName_ = slash == std::string::npos ? path : path.substr(slash + 1);

    auto load = [&]() -> bool {
        pak->fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (pak->fd_ < 0) return Reject(error, "cannot open the file");
        struct stat st{};
        if (fstat(pak->fd_, &st) != 0 || st.st_size < (off_t)kHeaderSize) return Reject(error, "too small to be a package");
        pak->fileSize_ = (uint64_t)st.st_size;

        uint8_t header[kHeaderSize];
        if (!ReadAt(pak->fd_, 0, header, sizeof(header))) return Reject(error, "cannot read the header");
        if (ReadLE<uint32_t>(header) != kSignature) return Reject(error, "not an LSPK package");
        if (ReadLE<uint32_t>(header + 4) != kVersion) return Reject(error, "not a version 18 package");
        uint64_t listOffset = ReadLE<uint64_t>(header + 8);
        pak->priority_ = header[21];

        // File list: count, compressed size, then LZ4 data of count * 272 bytes.
        if (listOffset > pak->fileSize_ || pak->fileSize_ - listOffset < 8) return Reject(error, "file list lies outside the file");
        uint8_t counts[8];
        if (!ReadAt(pak->fd_, listOffset, counts, sizeof(counts))) return Reject(error, "cannot read the file list");
        uint64_t fileCount = ReadLE<uint32_t>(counts);
        uint64_t compressedSize = ReadLE<uint32_t>(counts + 4);
        uint64_t listSize = fileCount * kEntrySize;
        if (listSize > kMaxFileListSize) return Reject(error, "file list is implausibly large");
        if (compressedSize > pak->fileSize_ - listOffset - 8) return Reject(error, "file list runs past the end of the file");

        std::vector<char> compressed(compressedSize);
        if (compressedSize && !ReadAt(pak->fd_, listOffset + 8, compressed.data(), compressed.size())) return Reject(error, "cannot read the file list");
        std::vector<uint8_t> list(listSize);
        int got = listSize ? LZ4_decompress_safe(compressed.data(), (char*)list.data(), (int)compressed.size(), (int)list.size()) : 0;
        if (got < 0 || (uint64_t)got != listSize) return Reject(error, "file list does not decompress to its stated size");

        pak->entries_.reserve(fileCount);
        for (uint64_t i = 0; i < fileCount; i++) {
            uint8_t const* raw = list.data() + i * kEntrySize;
            Entry e;
            e.Name.assign((char const*)raw, strnlen((char const*)raw, kNameSize));
            e.Offset = ReadLE<uint32_t>(raw + 256) | ((uint64_t)ReadLE<uint16_t>(raw + 260) << 32);
            e.Part = raw[262];
            e.Compression = raw[263] & 0x0f;
            e.DiskSize = ReadLE<uint32_t>(raw + 264);
            e.Size = ReadLE<uint32_t>(raw + 268);
            if (e.Offset == kDeletedOffset) continue;
            pak->entries_.push_back(std::move(e));
        }
        return true;
    };

    if (!load()) return nullptr;

    pak->index_.reserve(pak->entries_.size());
    for (uint32_t i = 0; i < pak->entries_.size(); i++) {
        pak->index_.emplace(pak->entries_[i].Name, i);
    }
    return pak;
}

PakFile::Entry const* PakFile::Find(std::string_view name) const
{
    auto it = index_.find(name);
    return it == index_.end() ? nullptr : &entries_[it->second];
}

std::optional<std::vector<uint8_t>> PakFile::Read(Entry const& e) const
{
    // Entries of a multi-part package live in other files; none of the
    // packages this reader is used on is split.
    if (e.Part != 0) return {};
    if (e.DiskSize > kMaxEntrySize || e.Size > kMaxEntrySize) return {};
    if (e.Offset > fileSize_ || e.DiskSize > fileSize_ - e.Offset) return {};

    std::vector<uint8_t> disk(e.DiskSize);
    if (e.DiskSize && !ReadAt(fd_, e.Offset, disk.data(), disk.size())) return {};

    switch (e.Compression) {
    case kStored:
        // For a stored entry the disk size is the size; the other field is often zero.
        return disk;

    case kZlib: {
        std::vector<uint8_t> out(e.Size);
        uLongf len = out.size();
        if (uncompress(out.data(), &len, disk.data(), disk.size()) != Z_OK || len != e.Size) return {};
        return out;
    }

    case kLz4: {
        std::vector<uint8_t> out(e.Size);
        int got = LZ4_decompress_safe((char const*)disk.data(), (char*)out.data(), (int)disk.size(), (int)out.size());
        if (got < 0 || (uint32_t)got != e.Size) return {};
        return out;
    }

    default:
        return {};
    }
}

}
