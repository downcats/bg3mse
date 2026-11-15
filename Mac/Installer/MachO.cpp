#include <Installer/MachO.h>

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cstring>
#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach/machine.h>

namespace bg3se::mac::installer {

namespace {

// The code signature's structures (xnu's osfmk/kern/cs_blobs.h), which the
// SDK does not ship. Every field is big-endian.
constexpr uint32_t kMagicEmbeddedSignature = 0xfade0cc0;
constexpr uint32_t kMagicCodeDirectory = 0xfade0c02;
constexpr uint32_t kMagicBlobWrapper = 0xfade0b01;     // the CMS signature
constexpr uint32_t kSlotCodeDirectory = 0;
constexpr uint32_t kSlotInfoPlist = 1;                  // a file of the bundle, not a blob
constexpr uint32_t kSlotResourceDirectory = 3;          // likewise
constexpr uint32_t kSlotAlternateFirst = 0x1000;
constexpr uint32_t kSlotAlternateLast = 0x1004;
constexpr uint32_t kSlotSignature = 0x10000;
constexpr uint32_t kFlagAdHoc = 0x00000002;
constexpr uint32_t kVersionScatter = 0x20100;
constexpr uint32_t kVersionCodeLimit64 = 0x20300;

// Offsets in a code directory.
constexpr size_t kCdVersion = 8, kCdFlags = 12, kCdHashOffset = 16, kCdIdentOffset = 20, kCdSpecialSlots = 24, kCdCodeSlots = 28,
    kCdCodeLimit = 32, kCdHashSize = 36, kCdHashType = 37, kCdPageSize = 39, kCdScatterOffset = 44, kCdCodeLimit64 = 56, kCdMinimum = 44;

uint32_t Be32(uint8_t const* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return OSSwapBigToHostInt32(v);
}

uint64_t Be64(uint8_t const* p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return OSSwapBigToHostInt64(v);
}

template <class T> T Read(std::span<uint8_t const> bytes, uint64_t offset)
{
    T v;
    memcpy(&v, bytes.data() + offset, sizeof(T));
    return v;
}

bool Fits(uint64_t offset, uint64_t size, uint64_t limit)
{
    return offset <= limit && size <= limit - offset;
}

bool IsDylibCommand(uint32_t cmd)
{
    return cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB || cmd == LC_REEXPORT_DYLIB || cmd == LC_LAZY_LOAD_DYLIB ||
        cmd == LC_LOAD_UPWARD_DYLIB;
}

// The commands a linker writes after the library commands.
bool IsLinkerTailCommand(uint32_t cmd)
{
    return cmd == LC_RPATH || cmd == LC_FUNCTION_STARTS || cmd == LC_DATA_IN_CODE || cmd == LC_CODE_SIGNATURE;
}

bool IsZeroFill(uint32_t flags)
{
    uint32_t type = flags & SECTION_TYPE;
    return type == S_ZEROFILL || type == S_GB_ZEROFILL || type == S_THREAD_LOCAL_ZEROFILL;
}

bool ReadSlice(std::span<uint8_t const> file, Slice& slice, std::string& problem)
{
    auto bytes = file.subspan(slice.Offset, slice.Size);
    if (bytes.size() < sizeof(mach_header_64)) {
        problem = "a slice too small for a Mach-O header";
        return false;
    }
    auto header = Read<mach_header_64>(bytes, 0);
    if (header.magic != MH_MAGIC_64) {
        problem = header.magic == MH_MAGIC ? "a 32-bit Mach-O image" : "not a 64-bit Mach-O image";
        return false;
    }
    if (slice.CpuType && (uint32_t)header.cputype != slice.CpuType) {
        problem = "a slice whose header names another processor than the universal header does";
        return false;
    }
    slice.CpuType = (uint32_t)header.cputype;
    slice.CpuSubtype = (uint32_t)header.cpusubtype;
    slice.FileType = header.filetype;
    slice.CommandCount = header.ncmds;
    slice.CommandBytes = header.sizeofcmds;
    uint64_t commandsEnd = sizeof(mach_header_64) + (uint64_t)header.sizeofcmds;
    if (commandsEnd > bytes.size()) {
        problem = "load commands past the end of the slice";
        return false;
    }

    uint64_t paddingEnd = bytes.size();
    bool tail = false;
    uint64_t at = sizeof(mach_header_64);
    for (uint32_t i = 0; i < header.ncmds; i++) {
        if (at + sizeof(load_command) > commandsEnd) {
            problem = "load commands that do not fit in sizeofcmds";
            return false;
        }
        auto lc = Read<load_command>(bytes, at);
        if (lc.cmdsize < sizeof(load_command) || lc.cmdsize % 8 || at + lc.cmdsize > commandsEnd) {
            problem = "a load command of a size that does not fit";
            return false;
        }
        if (lc.cmd == LC_SEGMENT_64) {
            if (lc.cmdsize < sizeof(segment_command_64)) {
                problem = "a short LC_SEGMENT_64";
                return false;
            }
            auto seg = Read<segment_command_64>(bytes, at);
            if (sizeof(segment_command_64) + (uint64_t)seg.nsects * sizeof(section_64) > lc.cmdsize ||
                (seg.filesize && !Fits(seg.fileoff, seg.filesize, bytes.size()))) {
                problem = "a segment that does not fit";
                return false;
            }
            if (seg.filesize && seg.fileoff > 0) paddingEnd = std::min<uint64_t>(paddingEnd, seg.fileoff);
            for (uint32_t s = 0; s < seg.nsects; s++) {
                auto sect = Read<section_64>(bytes, at + sizeof(segment_command_64) + s * sizeof(section_64));
                if (IsZeroFill(sect.flags) || !sect.size || !sect.offset) continue;
                paddingEnd = std::min<uint64_t>(paddingEnd, sect.offset);
            }
        } else if (IsDylibCommand(lc.cmd) || lc.cmd == LC_ID_DYLIB) {
            if (lc.cmdsize < sizeof(dylib_command)) {
                problem = "a short library command";
                return false;
            }
            auto dy = Read<dylib_command>(bytes, at);
            uint32_t name = dy.dylib.name.offset;
            if (name < sizeof(dylib_command) || name >= lc.cmdsize) {
                problem = "a library command whose name is outside it";
                return false;
            }
            auto text = bytes.subspan(at + name, lc.cmdsize - name);
            auto end = std::find(text.begin(), text.end(), uint8_t{ 0 });
            if (end == text.end()) {
                problem = "a library command whose name does not end";
                return false;
            }
            std::string path(text.begin(), end);
            if (lc.cmd == LC_ID_DYLIB) {
                slice.IdName = path;
                slice.IdCurrentVersion = dy.dylib.current_version;
            } else {
                slice.Dylibs.push_back({ lc.cmd, i, lc.cmdsize, name, dy.dylib.timestamp, dy.dylib.current_version,
                    dy.dylib.compatibility_version, std::move(path), tail });
            }
        } else if (lc.cmd == LC_UUID && lc.cmdsize >= sizeof(uuid_command)) {
            std::array<uint8_t, 16> uuid;
            memcpy(uuid.data(), Read<uuid_command>(bytes, at).uuid, 16);
            slice.Uuid = uuid;
        } else if (lc.cmd == LC_CODE_SIGNATURE) {
            if (lc.cmdsize < sizeof(linkedit_data_command)) {
                problem = "a short LC_CODE_SIGNATURE";
                return false;
            }
            auto sig = Read<linkedit_data_command>(bytes, at);
            if (!Fits(sig.dataoff, sig.datasize, bytes.size())) {
                problem = "a code signature past the end of the slice";
                return false;
            }
            slice.SignatureOffset = sig.dataoff;
            slice.SignatureSize = sig.datasize;
        }
        if (IsLinkerTailCommand(lc.cmd)) tail = true;
        slice.LastCommand = lc.cmd;
        slice.LastCommandSize = lc.cmdsize;
        at += lc.cmdsize;
    }
    if (paddingEnd < commandsEnd) {
        problem = "load commands that run into the contents of a section";
        return false;
    }
    slice.PaddingEnd = paddingEnd;
    return true;
}

bool HashOf(uint8_t type, uint8_t const* data, size_t size, uint8_t* out)
{
    // CC_LONG is 32 bits: hash in pieces.
    switch (type) {
    case 1: {
        CC_SHA1_CTX c;
        CC_SHA1_Init(&c);
        for (size_t done = 0; done < size;) {
            CC_LONG piece = (CC_LONG)std::min<size_t>(size - done, 1u << 30);
            CC_SHA1_Update(&c, data + done, piece);
            done += piece;
        }
        CC_SHA1_Final(out, &c);
        return true;
    }
    case 2:
    case 3: {
        CC_SHA256_CTX c;
        CC_SHA256_Init(&c);
        for (size_t done = 0; done < size;) {
            CC_LONG piece = (CC_LONG)std::min<size_t>(size - done, 1u << 30);
            CC_SHA256_Update(&c, data + done, piece);
            done += piece;
        }
        uint8_t full[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(full, &c);
        memcpy(out, full, type == 2 ? 32 : 20);
        return true;
    }
    case 4: {
        CC_SHA512_CTX c;    // SHA-384 runs on the SHA-512 context
        CC_SHA384_Init(&c);
        for (size_t done = 0; done < size;) {
            CC_LONG piece = (CC_LONG)std::min<size_t>(size - done, 1u << 30);
            CC_SHA384_Update(&c, data + done, piece);
            done += piece;
        }
        CC_SHA384_Final(out, &c);
        return true;
    }
    default:
        return false;
    }
}

uint8_t HashSizeOf(uint8_t type)
{
    switch (type) {
    case 1: return 20;
    case 2: return 32;
    case 3: return 20;
    case 4: return 48;
    default: return 0;
    }
}

// A code directory, as found in the signature.
struct CodeDirectory
{
    uint32_t BlobOffset{ 0 };        // in the signature
    uint32_t Length{ 0 };
    uint32_t Version{ 0 };
    uint32_t Flags{ 0 };
    uint32_t HashOffset{ 0 };
    uint32_t SpecialSlots{ 0 };
    uint32_t CodeSlots{ 0 };
    uint64_t CodeLimit{ 0 };
    uint8_t HashSize{ 0 };
    uint8_t HashType{ 0 };
    uint64_t PageSize{ 0 };          // 0: one page, the whole of the code
    std::string Identifier;
};

struct Blob
{
    uint32_t Slot;
    uint32_t Offset;
    uint32_t Length;
    uint32_t Magic;
};

struct Signature
{
    std::vector<Blob> Blobs;
    std::vector<CodeDirectory> Directories;   // the primary one first
    bool EmptyCms{ true };                    // no CMS blob, or an empty one
};

bool ParseSignature(std::span<uint8_t const> slice, Slice const& facts, Signature& out, std::string& problem)
{
    auto sig = slice.subspan(facts.SignatureOffset, facts.SignatureSize);
    if (sig.size() < 12 || Be32(sig.data()) != kMagicEmbeddedSignature) {
        problem = "the code signature is not an embedded signature";
        return false;
    }
    uint32_t length = Be32(sig.data() + 4), count = Be32(sig.data() + 8);
    if (length > sig.size() || length < 12 || count > (length - 12) / 8) {
        problem = "the code signature's index does not fit in it";
        return false;
    }
    bool primary = false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t slot = Be32(sig.data() + 12 + 8 * i), offset = Be32(sig.data() + 16 + 8 * i);
        if (!Fits(offset, 8, length)) {
            problem = "a blob of the code signature outside it";
            return false;
        }
        uint32_t magic = Be32(sig.data() + offset), blobLength = Be32(sig.data() + offset + 4);
        if (blobLength < 8 || !Fits(offset, blobLength, length)) {
            problem = "a blob of the code signature that does not fit in it";
            return false;
        }
        out.Blobs.push_back({ slot, offset, blobLength, magic });
        if (slot == kSlotSignature) {
            out.EmptyCms = magic == kMagicBlobWrapper && blobLength == 8;
            continue;
        }
        bool directory = slot == kSlotCodeDirectory || (slot >= kSlotAlternateFirst && slot <= kSlotAlternateLast);
        if (!directory) continue;
        auto cd = sig.subspan(offset, blobLength);
        if (magic != kMagicCodeDirectory || cd.size() < kCdMinimum) {
            problem = "a code directory that is not one";
            return false;
        }
        CodeDirectory d;
        d.BlobOffset = offset;
        d.Length = blobLength;
        d.Version = Be32(cd.data() + kCdVersion);
        d.Flags = Be32(cd.data() + kCdFlags);
        d.HashOffset = Be32(cd.data() + kCdHashOffset);
        uint32_t ident = Be32(cd.data() + kCdIdentOffset);
        d.SpecialSlots = Be32(cd.data() + kCdSpecialSlots);
        d.CodeSlots = Be32(cd.data() + kCdCodeSlots);
        d.CodeLimit = Be32(cd.data() + kCdCodeLimit);
        d.HashSize = cd[kCdHashSize];
        d.HashType = cd[kCdHashType];
        uint8_t pageShift = cd[kCdPageSize];
        if (d.Version >= kVersionScatter && Be32(cd.data() + kCdScatterOffset)) {
            problem = "a code directory with a scatter vector";
            return false;
        }
        if (d.Version >= kVersionCodeLimit64 && cd.size() >= kCdCodeLimit64 + 8 && Be64(cd.data() + kCdCodeLimit64)) {
            problem = "a code directory with a 64-bit code limit";
            return false;
        }
        if (!d.HashSize || d.HashSize != HashSizeOf(d.HashType)) {
            problem = "a code directory of an unknown hash type";
            return false;
        }
        if (pageShift >= 32) {
            problem = "a code directory of an impossible page size";
            return false;
        }
        d.PageSize = pageShift ? (uint64_t{ 1 } << pageShift) : 0;
        uint64_t special = (uint64_t)d.SpecialSlots * d.HashSize;
        if (d.HashOffset < special || d.HashOffset - special < kCdMinimum ||
            !Fits(d.HashOffset, (uint64_t)d.CodeSlots * d.HashSize, blobLength)) {
            problem = "a code directory whose hashes do not fit in it";
            return false;
        }
        if (ident >= blobLength || !memchr(cd.data() + ident, 0, blobLength - ident)) {
            problem = "a code directory whose identifier does not fit in it";
            return false;
        }
        d.Identifier = reinterpret_cast<char const*>(cd.data() + ident);
        uint64_t pages = d.PageSize ? (d.CodeLimit + d.PageSize - 1) / d.PageSize : (d.CodeLimit ? 1 : 0);
        if (d.CodeSlots != pages || d.CodeLimit > facts.SignatureOffset) {
            problem = "a code directory that does not cover the code up to the signature";
            return false;
        }
        if (slot == kSlotCodeDirectory) {
            out.Directories.insert(out.Directories.begin(), d);
            primary = true;
        } else {
            out.Directories.push_back(d);
        }
    }
    if (!primary) {
        problem = "a code signature without a code directory";
        return false;
    }
    return true;
}

// The page of the code a code slot covers.
std::span<uint8_t const> PageOf(std::span<uint8_t const> slice, CodeDirectory const& d, uint64_t index)
{
    if (!d.PageSize) return slice.subspan(0, d.CodeLimit);
    uint64_t start = index * d.PageSize;
    return slice.subspan(start, std::min(d.CodeLimit, start + d.PageSize) - start);
}

bool CheckDirectory(std::span<uint8_t const> slice, Slice const& facts, Signature const& signature, CodeDirectory const& d,
    std::string& problem)
{
    auto sig = slice.subspan(facts.SignatureOffset, facts.SignatureSize);
    uint8_t const* cd = sig.data() + d.BlobOffset;
    uint8_t hash[64];
    for (uint64_t i = 0; i < d.CodeSlots; i++) {
        auto page = PageOf(slice, d, i);
        HashOf(d.HashType, page.data(), page.size(), hash);
        if (memcmp(hash, cd + d.HashOffset + i * d.HashSize, d.HashSize) != 0) {
            problem = "the hash of the page at " + std::to_string(i * d.PageSize) + " does not match the signature";
            return false;
        }
    }
    for (uint32_t k = 1; k <= d.SpecialSlots; k++) {
        uint8_t const* slot = cd + d.HashOffset - (uint64_t)k * d.HashSize;
        auto blob = std::find_if(signature.Blobs.begin(), signature.Blobs.end(), [&](Blob const& b) { return b.Slot == k; });
        if (blob != signature.Blobs.end()) {
            HashOf(d.HashType, sig.data() + blob->Offset, blob->Length, hash);
            if (memcmp(hash, slot, d.HashSize) != 0) {
                problem = "the hash of the signature's blob " + std::to_string(k) + " does not match";
                return false;
            }
        } else if (k != kSlotInfoPlist && k != kSlotResourceDirectory) {
            // A slot of a blob that is not there must be empty; Info.plist and
            // the resource seal are files of the bundle, not checked here.
            if (std::any_of(slot, slot + d.HashSize, [](uint8_t b) { return b != 0; })) {
                problem = "the signature names a blob " + std::to_string(k) + " that it does not hold";
                return false;
            }
        }
    }
    return true;
}

}

Slice const* MachOFile::Find(uint32_t cpuType) const
{
    for (auto const& slice : Slices) {
        if (slice.CpuType == cpuType) return &slice;
    }
    return nullptr;
}

std::optional<MachOFile> ReadMachO(std::span<uint8_t const> file, std::string& problem)
{
    if (file.size() < 8) {
        problem = "too small for a Mach-O file";
        return std::nullopt;
    }
    MachOFile out;
    uint32_t magic = Read<uint32_t>(file, 0);
    if (magic == FAT_CIGAM_64 || magic == FAT_MAGIC_64) {
        problem = "a universal file with a 64-bit header, which this does not handle";
        return std::nullopt;
    }
    if (magic == FAT_CIGAM || magic == FAT_MAGIC) {
        if (magic == FAT_MAGIC) {
            problem = "a universal file whose header is not big-endian";
            return std::nullopt;
        }
        out.Universal = true;
        uint32_t count = Be32(file.data() + 4);
        if (count == 0 || count > 16 || 8 + (uint64_t)count * sizeof(fat_arch) > file.size()) {
            problem = "a universal header that does not fit";
            return std::nullopt;
        }
        for (uint32_t i = 0; i < count; i++) {
            uint8_t const* arch = file.data() + 8 + i * sizeof(fat_arch);
            Slice slice;
            slice.CpuType = Be32(arch);
            slice.CpuSubtype = Be32(arch + 4);
            slice.Offset = Be32(arch + 8);
            slice.Size = Be32(arch + 12);
            if (!Fits(slice.Offset, slice.Size, file.size()) || slice.Offset < 8 + (uint64_t)count * sizeof(fat_arch)) {
                problem = "a slice outside the file";
                return std::nullopt;
            }
            for (auto const& other : out.Slices) {
                if (slice.Offset < other.Offset + other.Size && other.Offset < slice.Offset + slice.Size) {
                    problem = "two slices that overlap";
                    return std::nullopt;
                }
                if (other.CpuType == slice.CpuType) {
                    problem = "two slices for one processor";
                    return std::nullopt;
                }
            }
            if (!ReadSlice(file, slice, problem)) {
                problem = CpuName(slice.CpuType) + " slice: " + problem;
                return std::nullopt;
            }
            out.Slices.push_back(std::move(slice));
        }
        return out;
    }
    Slice slice;
    slice.Offset = 0;
    slice.Size = file.size();
    if (!ReadSlice(file, slice, problem)) return std::nullopt;
    out.Slices.push_back(std::move(slice));
    return out;
}

std::string CpuName(uint32_t cpuType)
{
    if (cpuType == (uint32_t)CPU_TYPE_ARM64) return "arm64";
    if (cpuType == (uint32_t)CPU_TYPE_X86_64) return "x86_64";
    return "cpu type " + std::to_string(cpuType);
}

std::string UuidText(std::array<uint8_t, 16> const& uuid)
{
    char text[37];
    snprintf(text, sizeof(text), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", uuid[0], uuid[1], uuid[2],
        uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return text;
}

SignatureFacts CheckSignature(std::span<uint8_t const> file, Slice const& slice)
{
    SignatureFacts facts;
    if (!slice.SignatureSize) {
        facts.Problem = "not signed";
        return facts;
    }
    facts.Present = true;
    auto bytes = file.subspan(slice.Offset, slice.Size);
    Signature signature;
    if (!ParseSignature(bytes, slice, signature, facts.Problem)) return facts;
    auto const& primary = signature.Directories.front();
    facts.Flags = primary.Flags;
    facts.Identifier = primary.Identifier;
    facts.AdHoc = signature.EmptyCms && (primary.Flags & kFlagAdHoc);
    for (auto const& d : signature.Directories) {
        if (!CheckDirectory(bytes, slice, signature, d, facts.Problem)) return facts;
    }
    facts.HashesMatch = true;
    return facts;
}

}
