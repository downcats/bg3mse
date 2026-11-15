#include <Updater/Stamp.h>

#include <Release/GfxMark.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <libkern/OSByteOrder.h>
#include <unistd.h>
#include <vector>

namespace bg3se::mac::updater {

namespace {

bool ReadAt(int fd, uint64_t offset, void* out, size_t size)
{
    return pread(fd, out, size, (off_t)offset) == (ssize_t)size;
}

}

bool ParseStamp(std::string_view text, Stamp& s)
{
    // In two parts: the whole would be a what(1) marker in this image, and
    // `what -s` reads the first one it finds as the image's stamp.
    constexpr std::string_view marker = "@(", rest = "#)bg3mse ";
    if (!text.starts_with(marker) || !text.substr(marker.size()).starts_with(rest)) return false;
    text.remove_prefix(marker.size() + rest.size());
    text = text.substr(0, text.find('\0'));
    auto next = [&](std::string_view key, std::string& out) {
        size_t space = text.find(' ');
        std::string_view word = text.substr(0, space);
        text = space == std::string_view::npos ? std::string_view() : text.substr(space + 1);
        if (!word.starts_with(key)) return false;
        out = std::string(word.substr(key.size()));
        return !out.empty();
    };
    return next("", s.Version) && next("commit=", s.Commit) && next("extender=", s.Extender) && next("channel=", s.Channel);
}

bool ReadSection(std::string const& path, char const* segment, char const* section, std::optional<std::string>& bytes, std::string& why)
{
    bytes.reset();
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        why = "cannot open " + path + ": " + strerror(errno);
        return false;
    }
    struct Closer { int fd; ~Closer() { close(fd); } } closer{ fd };
    uint64_t base = 0;
    uint32_t magic = 0;
    if (!ReadAt(fd, 0, &magic, 4)) {
        why = path + " is not a Mach-O file";
        return false;
    }
    if (magic == FAT_CIGAM || magic == FAT_MAGIC) {
        fat_header fat;
        ReadAt(fd, 0, &fat, sizeof(fat));
        uint32_t n = OSSwapBigToHostInt32(fat.nfat_arch);
        bool found = false;
        for (uint32_t i = 0; i < n && i < 16; i++) {
            fat_arch arch;
            if (!ReadAt(fd, sizeof(fat) + i * sizeof(arch), &arch, sizeof(arch))) break;
            if ((cpu_type_t)OSSwapBigToHostInt32((uint32_t)arch.cputype) == CPU_TYPE_ARM64) {
                base = OSSwapBigToHostInt32(arch.offset);
                found = true;
                break;
            }
        }
        if (!found) {
            why = path + " has no arm64 code";
            return false;
        }
    }
    mach_header_64 header;
    if (!ReadAt(fd, base, &header, sizeof(header)) || header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64
        || header.sizeofcmds > (16u << 20)) {
        why = path + " is not arm64 Mach-O code";
        return false;
    }
    std::vector<uint8_t> commands(header.sizeofcmds);
    if (!ReadAt(fd, base + sizeof(header), commands.data(), commands.size())) {
        why = path + " is cut short";
        return false;
    }
    size_t at = 0;
    for (uint32_t i = 0; i < header.ncmds && at + sizeof(load_command) <= commands.size(); i++) {
        load_command lc;
        memcpy(&lc, &commands[at], sizeof(lc));
        if (lc.cmdsize < sizeof(lc) || at + lc.cmdsize > commands.size()) {
            why = path + " has a load command that runs past its header";
            return false;
        }
        if (lc.cmd == LC_SEGMENT_64 && lc.cmdsize >= sizeof(segment_command_64)) {
            segment_command_64 seg;
            memcpy(&seg, &commands[at], sizeof(seg));
            for (uint32_t s = 0; s < seg.nsects; s++) {
                size_t sat = at + sizeof(seg) + s * sizeof(section_64);
                if (sat + sizeof(section_64) > at + lc.cmdsize) break;
                section_64 sect;
                memcpy(&sect, &commands[sat], sizeof(sect));
                if (strncmp(sect.segname, segment, 16) || strncmp(sect.sectname, section, 16)) continue;
                if (sect.size > 4096) {
                    why = path + ": its section " + segment + "," + section + " is larger than one of bg3mse's";
                    return false;
                }
                std::string text(sect.size, '\0');
                if (!ReadAt(fd, base + sect.offset, text.data(), text.size())) {
                    why = path + " is cut short";
                    return false;
                }
                bytes = std::move(text);
                return true;
            }
        }
        at += lc.cmdsize;
    }
    return true;
}

bool ReadStamp(std::string const& path, Stamp& stamp, std::string& why)
{
    std::optional<std::string> text;
    if (!ReadSection(path, "__TEXT", "__bg3mse_version", text, why)) return false;
    if (!text) {
        why = path + " carries no bg3mse version stamp";
        return false;
    }
    if (!ParseStamp(*text, stamp)) {
        why = path + ": its version stamp cannot be read";
        return false;
    }
    return true;
}

bool ReadGfxMark(std::string const& path, bool& marked, std::string& why)
{
    marked = false;
    std::optional<std::string> text;
    if (!ReadSection(path, BG3MSE_GFX_SEGMENT, BG3MSE_GFX_SECTION, text, why)) return false;
    if (!text) return true;
    std::string_view mark(*text);
    mark = mark.substr(0, mark.find('\0'));
    if (mark != BG3MSE_GFX_MARK) {
        why = path + ": its graphics module mark is \"" + std::string(mark) + "\", not one this bg3mse-install reads";
        return false;
    }
    marked = true;
    return true;
}

}
