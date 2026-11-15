// bg3mse-symbolicate: the backtrace of a crash report (Crash/CrashReport.h)
// with the names, files and lines the release's .dSYM files give.
//
//   bg3mse-symbolicate <crash_<time>.crpt> <x.dSYM or a directory of them>...
//
// A shipped dylib is stripped of its local symbols (Mac/Release/Symbols.cmake),
// so the report names few of its frames. The report says, for each loaded
// image, where it was loaded and its LC_UUID (MIMG), and for each frame its
// address and image: the crashing thread's (MBTR) and every other thread's
// (MTHR). A dSYM whose UUID is an image's is that image's symbols, and atos
// reads it for the image's frames, of all threads at once. Every other frame
// is shown as the report has it. The crashing thread comes first, then the
// other threads, each under a line with its number, name, queue and id.
//
// Exit status: 0 when a dSYM was found for an image of any thread's frames,
// 2 when none was, 1 when the report or a dSYM cannot be read.

#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach/machine.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

extern char** environ;

namespace {

namespace fs = std::filesystem;

constexpr uint32_t Tag(char a, char b, char c, char d)
{
    return (uint32_t)(uint8_t)a << 24 | (uint32_t)(uint8_t)b << 16 | (uint32_t)(uint8_t)c << 8 | (uint32_t)(uint8_t)d;
}

bool ReadAll(std::string const& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream text;
    text << f.rdbuf();
    out = text.str();
    return true;
}

// The sections of a report: 'CRSH', then tag, length, body.
std::optional<std::map<uint32_t, std::string>> ReadReport(std::string const& path)
{
    std::string bytes;
    if (!ReadAll(path, bytes) || bytes.size() < 4) return std::nullopt;
    uint32_t value;
    memcpy(&value, bytes.data(), 4);
    if (value != Tag('C', 'R', 'S', 'H')) return std::nullopt;
    std::map<uint32_t, std::string> sections;
    for (size_t at = 4; at < bytes.size();) {
        uint32_t head[2];
        if (bytes.size() - at < sizeof(head)) return std::nullopt;
        memcpy(head, bytes.data() + at, sizeof(head));
        at += sizeof(head);
        if (bytes.size() - at < head[1]) return std::nullopt;
        sections.emplace(head[0], bytes.substr(at, head[1]));
        at += head[1];
    }
    return sections;
}

std::vector<std::string> Lines(std::string const& text)
{
    std::vector<std::string> lines;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

// " key=value" in a line of several; the last key's value runs to the end
// of the line when `last` is set.
std::string Field(std::string const& line, std::string_view key, bool last = false)
{
    std::string text = " " + line, wanted = " " + std::string(key) + "=";
    size_t at = text.find(wanted);
    if (at == std::string::npos) return {};
    size_t from = at + wanted.size();
    if (last) return text.substr(from);
    return text.substr(from, text.find(' ', from) - from);
}

uint64_t Hex(std::string const& text)
{
    return strtoull(text.c_str(), nullptr, 16);
}

struct Image
{
    uint64_t Base{ 0 };
    uint64_t Slide{ 0 };
    std::string Uuid;
    std::string Path;
};

struct Frame
{
    unsigned Index{ 0 };
    uint64_t PC{ 0 };
    uint64_t ImageBase{ 0 };
    std::string Symbol;
    // MTHR's thread number; -1 for the crashing thread's (MBTR).
    long Thread{ -1 };
};

// A thread of MTHR, by its line "thread=<n> ... name=".
struct Thread
{
    long Number{ 0 };
    std::string Line;
    std::string Name;
    std::string Queue;
};

// The LC_UUID of the arm64 image in a Mach-O file, thin or universal, as
// the report writes it.
std::optional<std::string> UuidOf(fs::path const& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    auto readAt = [&](uint64_t offset, void* out, size_t size) {
        f.clear();
        f.seekg((std::streamoff)offset);
        f.read(static_cast<char*>(out), (std::streamsize)size);
        return (size_t)f.gcount() == size;
    };
    uint32_t magic = 0;
    if (!readAt(0, &magic, 4)) return std::nullopt;
    uint64_t start = 0;
    if (magic == FAT_CIGAM) {
        fat_header header;
        if (!readAt(0, &header, sizeof(header))) return std::nullopt;
        bool found = false;
        for (uint32_t i = 0; i < OSSwapBigToHostInt32(header.nfat_arch) && i < 64; i++) {
            fat_arch arch;
            if (!readAt(sizeof(header) + i * sizeof(arch), &arch, sizeof(arch))) return std::nullopt;
            if ((cpu_type_t)OSSwapBigToHostInt32((uint32_t)arch.cputype) == CPU_TYPE_ARM64) {
                start = OSSwapBigToHostInt32(arch.offset);
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    mach_header_64 header;
    if (!readAt(start, &header, sizeof(header)) || header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64) return std::nullopt;
    if (header.sizeofcmds > (64u << 20)) return std::nullopt;
    std::vector<uint8_t> commands(header.sizeofcmds);
    if (!readAt(start + sizeof(header), commands.data(), commands.size())) return std::nullopt;
    for (size_t at = 0, i = 0; i < header.ncmds && at + sizeof(load_command) <= commands.size(); i++) {
        load_command lc;
        memcpy(&lc, commands.data() + at, sizeof(lc));
        if (lc.cmdsize < sizeof(lc) || at + lc.cmdsize > commands.size()) break;
        if (lc.cmd == LC_UUID && lc.cmdsize >= sizeof(uuid_command)) {
            uuid_command command;
            memcpy(&command, commands.data() + at, sizeof(command));
            auto u = command.uuid;
            char text[37];
            snprintf(text, sizeof(text), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            return std::string(text);
        }
        at += lc.cmdsize;
    }
    return std::nullopt;
}

// The DWARF files of the dSYMs at or under `path`, by UUID.
void FindSymbols(fs::path const& path, std::map<std::string, fs::path>& out)
{
    std::error_code error;
    auto add = [&](fs::path const& dsym) {
        fs::path dwarf = dsym / "Contents" / "Resources" / "DWARF";
        for (auto const& entry : fs::directory_iterator(dwarf, error)) {
            if (!entry.is_regular_file()) continue;
            if (auto uuid = UuidOf(entry.path())) out.emplace(*uuid, entry.path());
        }
    };
    if (path.extension() == ".dSYM") {
        add(path);
        return;
    }
    for (auto it = fs::recursive_directory_iterator(path, error); it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (error) break;
        if (it->is_directory() && it->path().extension() == ".dSYM") {
            add(it->path());
            it.disable_recursion_pending();
        }
    }
}

// What atos makes of `addresses` in the image loaded at `base`: one line an
// address, or nothing when it cannot be run.
std::vector<std::string> Atos(fs::path const& dwarf, uint64_t base, std::vector<uint64_t> const& addresses)
{
    std::vector<std::string> args = { "xcrun", "atos", "-o", dwarf.string(), "-arch", "arm64", "-l" };
    char number[32];
    snprintf(number, sizeof(number), "0x%llx", (unsigned long long)base);
    args.push_back(number);
    for (uint64_t address : addresses) {
        snprintf(number, sizeof(number), "0x%llx", (unsigned long long)address);
        args.push_back(number);
    }
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    int fds[2];
    if (pipe(fds) != 0) return {};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, "xcrun", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return {};
    }
    std::string output;
    char buffer[4096];
    for (ssize_t n; (n = read(fds[0], buffer, sizeof(buffer))) > 0;) output.append(buffer, (size_t)n);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return {};
    auto lines = Lines(output);
    if (lines.size() != addresses.size()) return {};
    return lines;
}

std::string BaseName(std::string const& path)
{
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: bg3mse-symbolicate <report.crpt> <x.dSYM or directory>...\n");
        return 1;
    }
    auto report = ReadReport(argv[1]);
    if (!report) {
        fprintf(stderr, "bg3mse-symbolicate: %s is not a crash report\n", argv[1]);
        return 1;
    }
    auto section = [&](uint32_t tag) -> std::string {
        auto it = report->find(tag);
        return it == report->end() ? std::string() : it->second;
    };

    std::map<std::string, fs::path> symbols;
    for (int i = 2; i < argc; i++) FindSymbols(argv[i], symbols);

    std::map<uint64_t, Image> images;
    for (auto const& line : Lines(section(Tag('M', 'I', 'M', 'G')))) {
        Image image{ Hex(Field(line, "base")), Hex(Field(line, "slide")), Field(line, "uuid"), Field(line, "path", true) };
        if (image.Base) images.emplace(image.Base, image);
    }
    std::vector<Frame> frames;
    for (auto const& line : Lines(section(Tag('M', 'B', 'T', 'R')))) {
        if (Field(line, "frame").empty()) continue;
        frames.push_back({ (unsigned)strtoul(Field(line, "frame").c_str(), nullptr, 10), Hex(Field(line, "pc")),
            Hex(Field(line, "image")), Field(line, "symbol", true) });
    }
    if (frames.empty()) {
        fprintf(stderr, "bg3mse-symbolicate: %s has no backtrace (MBTR)\n", argv[1]);
        return 1;
    }
    size_t crashingFrames = frames.size();
    std::vector<Thread> threads;
    for (auto const& line : Lines(section(Tag('M', 'T', 'H', 'R')))) {
        if (line.rfind("thread=", 0) == 0) {
            threads.push_back({ strtol(Field(line, "thread").c_str(), nullptr, 10), line, Field(line, "name", true), {} });
        } else if (line.rfind("queue ", 0) == 0 && !threads.empty()) {
            threads.back().Queue = Field(line, "label", true);
        } else if (line.rfind("frame ", 0) == 0) {
            frames.push_back({ (unsigned)strtoul(Field(line, "index").c_str(), nullptr, 10), Hex(Field(line, "pc")),
                Hex(Field(line, "image")), Field(line, "symbol", true), strtol(Field(line, "thread").c_str(), nullptr, 10) });
        }
    }

    // The frames of each image with a dSYM, symbolicated together. A return
    // address is the instruction after the call: the call is the one before.
    std::map<size_t, std::string> named;
    std::map<uint64_t, std::vector<size_t>> byImage;
    for (size_t i = 0; i < frames.size(); i++) byImage[frames[i].ImageBase].push_back(i);
    bool anyImage = false;
    for (auto const& [base, indices] : byImage) {
        auto image = images.find(base);
        if (image == images.end()) continue;
        auto dsym = symbols.find(image->second.Uuid);
        if (dsym == symbols.end()) continue;
        anyImage = true;
        std::vector<uint64_t> addresses;
        for (size_t i : indices) addresses.push_back(frames[i].Index == 0 ? frames[i].PC : frames[i].PC - 1);
        auto lines = Atos(dsym->second, base, addresses);
        if (lines.empty()) {
            fprintf(stderr, "bg3mse-symbolicate: atos could not read %s\n", dsym->second.c_str());
            return 1;
        }
        for (size_t k = 0; k < indices.size(); k++) named[indices[k]] = lines[k];
    }

    auto sver = section(Tag('S', 'V', 'E', 'R')), gver = section(Tag('G', 'V', 'E', 'R'));
    printf("%s: Script Extender %s, game %s\n", argv[1], sver.c_str(), gver.c_str());
    for (auto const& line : Lines(section(Tag('M', 'B', 'L', 'D')))) printf("  %s\n", line.c_str());
    auto printFrame = [&](size_t i) {
        auto const& frame = frames[i];
        auto image = images.find(frame.ImageBase);
        std::string name = image == images.end() ? std::string("?") : BaseName(image->second.Path);
        std::string what;
        if (auto it = named.find(i); it != named.end()) {
            what = it->second;
        } else if (!frame.Symbol.empty() && frame.Symbol != "-") {
            what = frame.Symbol;
        } else if (image != images.end()) {
            char text[64];
            snprintf(text, sizeof(text), "0x%llx", (unsigned long long)(frame.PC - image->second.Slide));
            what = name + "!" + text;
        } else {
            what = "?";
        }
        printf("%3u  0x%016llx  %-32s %s\n", frame.Index, (unsigned long long)frame.PC, name.c_str(), what.c_str());
    };
    for (size_t i = 0; i < crashingFrames; i++) printFrame(i);
    // The other threads, in the report's order, with the frames each has.
    for (auto const& thread : threads) {
        printf("\nThread %ld", thread.Number);
        if (!thread.Name.empty()) printf(" \"%s\"", thread.Name.c_str());
        if (!thread.Queue.empty()) printf(", queue \"%s\"", thread.Queue.c_str());
        printf(", id %s", Field(thread.Line, "id").c_str());
        std::string role = Field(thread.Line, "role");
        if (role != "other") printf(" (%s)", role.c_str());
        printf("\n");
        for (size_t i = crashingFrames; i < frames.size(); i++) {
            if (frames[i].Thread == thread.Number) printFrame(i);
        }
    }
    if (!anyImage) {
        fprintf(stderr, "bg3mse-symbolicate: no dSYM given is of an image of this backtrace\n");
        return 2;
    }
    return 0;
}
