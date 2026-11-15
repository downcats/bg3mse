#include <Updater/Zip.h>
#include <Updater/Files.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace bg3se::mac::updater {

namespace {

uint16_t U16(uint8_t const* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t U32(uint8_t const* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

constexpr uint32_t kEnd = 0x06054b50, kCentral = 0x02014b50, kLocal = 0x04034b50;

bool WriteAll(int fd, uint8_t const* p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

}

bool ZipReader::Open(std::span<uint8_t const> a, std::string& why)
{
    data_ = a;
    entries_.clear();
    if (a.size() < 22) {
        why = "the zip is too short";
        return false;
    }
    // The end record, after which there may be a comment of up to 64 KiB.
    size_t end = SIZE_MAX;
    size_t lowest = a.size() > 22 + 65535 ? a.size() - 22 - 65535 : 0;
    for (size_t at = a.size() - 22 + 1; at-- > lowest;) {
        if (U32(&a[at]) == kEnd && at + 22 + U16(&a[at + 20]) == a.size()) {
            end = at;
            break;
        }
    }
    if (end == SIZE_MAX) {
        why = "the zip has no end record";
        return false;
    }
    uint8_t const* e = &a[end];
    if (U16(e + 4) != 0 || U16(e + 6) != 0 || U16(e + 8) != U16(e + 10)) {
        why = "the zip spans several disks";
        return false;
    }
    uint32_t count = U16(e + 10), size = U32(e + 12), offset = U32(e + 16);
    if (count == 0xffff || size == 0xffffffff || offset == 0xffffffff) {
        why = "the zip is a zip64 archive";
        return false;
    }
    if ((uint64_t)offset + size > end) {
        why = "the zip's directory is outside it";
        return false;
    }
    size_t at = offset, stop = (size_t)offset + size;
    for (uint32_t i = 0; i < count; i++) {
        if (at + 46 > stop || U32(&a[at]) != kCentral) {
            why = "the zip's directory is damaged";
            return false;
        }
        uint8_t const* c = &a[at];
        ZipEntry entry;
        uint16_t madeBy = U16(c + 4), flags = U16(c + 8);
        entry.Method = U16(c + 10);
        entry.Crc = U32(c + 16);
        entry.CompressedSize = U32(c + 20);
        entry.Size = U32(c + 24);
        uint16_t nameLength = U16(c + 28), extra = U16(c + 30), comment = U16(c + 32);
        uint32_t external = U32(c + 38);
        entry.LocalHeader = U32(c + 42);
        if (at + 46 + nameLength + extra + comment > stop) {
            why = "the zip's directory is damaged";
            return false;
        }
        entry.Name.assign(reinterpret_cast<char const*>(c + 46), nameLength);
        if (flags & 1) {
            why = "the zip entry " + entry.Name + " is encrypted";
            return false;
        }
        if (entry.Method != 0 && entry.Method != 8) {
            why = "the zip entry " + entry.Name + " is compressed in a way that is not read";
            return false;
        }
        if ((madeBy >> 8) == 3) entry.Mode = external >> 16;
        if (entry.Name.empty() || entry.Name.find('\0') != std::string::npos) {
            why = "a zip entry has no valid name";
            return false;
        }
        if (entry.LocalHeader + 30 > offset) {
            why = "the zip entry " + entry.Name + " is outside the archive";
            return false;
        }
        uint8_t const* l = &a[entry.LocalHeader];
        if (U32(l) != kLocal) {
            why = "the zip entry " + entry.Name + " has no local header";
            return false;
        }
        uint64_t dataAt = entry.LocalHeader + 30 + U16(l + 26) + U16(l + 28);
        if (dataAt + entry.CompressedSize > offset) {
            why = "the zip entry " + entry.Name + " runs past its data";
            return false;
        }
        entries_.push_back(std::move(entry));
        at += 46 + nameLength + extra + comment;
    }
    return true;
}

template<class Sink>
bool ZipReader::Inflate(ZipEntry const& entry, Sink&& sink, std::string& why) const
{
    uint8_t const* l = &data_[entry.LocalHeader];
    uint8_t const* in = l + 30 + U16(l + 26) + U16(l + 28);
    uLong crc = crc32(0, nullptr, 0);
    uint64_t produced = 0;
    if (entry.Method == 0) {
        if (entry.CompressedSize != entry.Size) {
            why = "the zip entry " + entry.Name + " has two sizes";
            return false;
        }
        crc = crc32_z(crc, in, (z_size_t)entry.Size);
        if (!sink(in, (size_t)entry.Size, why)) return false;
        produced = entry.Size;
    } else {
        z_stream z{};
        if (inflateInit2(&z, -MAX_WBITS) != Z_OK) {
            why = "zlib cannot start";
            return false;
        }
        z.next_in = const_cast<Bytef*>(in);
        z.avail_in = (uInt)entry.CompressedSize;
        std::vector<uint8_t> out(1 << 20);
        int rc = Z_OK;
        while (rc != Z_STREAM_END) {
            z.next_out = out.data();
            z.avail_out = (uInt)out.size();
            rc = inflate(&z, Z_NO_FLUSH);
            size_t n = out.size() - z.avail_out;
            if ((rc != Z_OK && rc != Z_STREAM_END) || (rc == Z_OK && n == 0 && z.avail_in == 0)) {
                inflateEnd(&z);
                why = "the zip entry " + entry.Name + " does not inflate";
                return false;
            }
            produced += n;
            if (produced > entry.Size) {
                inflateEnd(&z);
                why = "the zip entry " + entry.Name + " is larger than it says";
                return false;
            }
            crc = crc32_z(crc, out.data(), n);
            if (!sink(out.data(), n, why)) {
                inflateEnd(&z);
                return false;
            }
        }
        inflateEnd(&z);
    }
    if (produced != entry.Size || crc != entry.Crc) {
        why = "the zip entry " + entry.Name + " is damaged (size or CRC)";
        return false;
    }
    return true;
}

bool ZipReader::Extract(ZipEntry const& entry, std::string const& path, std::string& why) const
{
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd < 0) {
        why = "cannot create " + path + ": " + Errno();
        return false;
    }
    bool ok = Inflate(entry, [&](uint8_t const* p, size_t n, std::string& w) {
        if (WriteAll(fd, p, n)) return true;
        w = "cannot write " + path + ": " + Errno();
        return false;
    }, why);
    if (ok && fcntl(fd, F_FULLFSYNC) != 0 && fsync(fd) != 0) {
        why = "cannot sync " + path + ": " + Errno();
        ok = false;
    }
    if (close(fd) != 0 && ok) {
        why = "cannot write " + path + ": " + Errno();
        ok = false;
    }
    if (!ok) unlink(path.c_str());
    return ok;
}

bool ZipReader::Read(ZipEntry const& entry, std::string& out, std::string& why) const
{
    out.clear();
    if (entry.Size > (64u << 20)) {
        why = "the zip entry " + entry.Name + " is too large to read into memory";
        return false;
    }
    out.reserve((size_t)entry.Size);
    return Inflate(entry, [&](uint8_t const* p, size_t n, std::string&) {
        out.append(reinterpret_cast<char const*>(p), n);
        return true;
    }, why);
}

}
