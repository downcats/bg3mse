// The census of the code in the game that answers whether the game is modded
// (the patch of ls::ModuleSettings::IsModded, Engine/Entries/Patches.cpp),
// on the game's .app given as the argument.
//
// Norbyte's Windows patch keys on IsModded's test of the standard modules;
// on macOS that test is ls::ModuleShortDesc::IsStandardModule, which the game
// calls and does not inline. Every use of it must be in a function the patch
// covers or in one listed here as not concerned: every call and tail call,
// every instruction that takes its address and every pointer to it in the
// game's data. And the module id Norbyte's pattern compares with must be read
// by IsStandardModule and the code that makes the string, and nowhere else,
// so that no copy of the test is inlined.

#include <Binding/CodePatches.h>
#include <Engine/EngineTable.h>
#include <Platform/Log.h>
#include <Platform/Symbols.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace bg3se::mac;

namespace {

int gFailures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf(" (%s, line %d)\n", #cond, __LINE__); gFailures++; } } while (0)

engine::Entry const* TableEntry(std::string_view name)
{
    for (auto const& area : engine::EngineTable()) {
        for (auto const& entry : area.Entries) {
            if (name == entry.Name) return &entry;
        }
    }
    return nullptr;
}

std::span<uint8_t const> Map(std::string const& path)
{
    int fd = open(path.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) {
        fprintf(stderr, "cannot open %s\n", path.c_str());
        if (fd >= 0) close(fd);
        return {};
    }
    void* data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        fprintf(stderr, "cannot map %s\n", path.c_str());
        return {};
    }
    return { static_cast<uint8_t const*>(data), (size_t)st.st_size };
}

// The arm64 image of a file that is one or holds one.
std::span<uint8_t const> Arm64Slice(std::span<uint8_t const> file)
{
    if (file.size() < sizeof(fat_header)) return {};
    fat_header fat;
    memcpy(&fat, file.data(), sizeof(fat));
    if (OSSwapBigToHostInt32(fat.magic) != FAT_MAGIC) return file;
    uint32_t count = OSSwapBigToHostInt32(fat.nfat_arch);
    for (uint32_t i = 0; i < count; i++) {
        size_t at = sizeof(fat_header) + i * sizeof(fat_arch);
        if (at + sizeof(fat_arch) > file.size()) return {};
        fat_arch arch;
        memcpy(&arch, file.data() + at, sizeof(arch));
        if ((cpu_type_t)OSSwapBigToHostInt32(arch.cputype) != CPU_TYPE_ARM64) continue;
        size_t offset = OSSwapBigToHostInt32(arch.offset), size = OSSwapBigToHostInt32(arch.size);
        if (offset > file.size() || size > file.size() - offset) return {};
        return file.subspan(offset, size);
    }
    return {};
}

// The contents of every section of the image that has contents in the file,
// other than its code.
struct DataSection
{
    std::string Name;
    uint64_t VA;
    std::span<uint8_t const> Bytes;
};

std::vector<DataSection> DataSections(std::span<uint8_t const> image)
{
    std::vector<DataSection> found;
    if (image.size() < sizeof(mach_header_64)) return found;
    mach_header_64 mh;
    memcpy(&mh, image.data(), sizeof(mh));
    if (mh.magic != MH_MAGIC_64) return found;
    size_t at = sizeof(mach_header_64);
    for (uint32_t i = 0; i < mh.ncmds; i++) {
        if (at + sizeof(load_command) > image.size()) return {};
        load_command lc;
        memcpy(&lc, image.data() + at, sizeof(lc));
        if (lc.cmdsize < sizeof(load_command) || at + lc.cmdsize > image.size()) return {};
        if (lc.cmd == LC_SEGMENT_64 && lc.cmdsize >= sizeof(segment_command_64)) {
            segment_command_64 seg;
            memcpy(&seg, image.data() + at, sizeof(seg));
            if (sizeof(segment_command_64) + seg.nsects * sizeof(section_64) > lc.cmdsize) return {};
            for (uint32_t s = 0; s < seg.nsects; s++) {
                section_64 sect;
                memcpy(&sect, image.data() + at + sizeof(segment_command_64) + s * sizeof(section_64), sizeof(sect));
                uint32_t type = sect.flags & SECTION_TYPE;
                if (type == S_ZEROFILL || type == S_GB_ZEROFILL || type == S_THREAD_LOCAL_ZEROFILL) continue;
                if (sect.flags & S_ATTR_PURE_INSTRUCTIONS) continue;
                if (sect.offset > image.size() || sect.size > image.size() - sect.offset) return {};
                found.push_back({ std::string(sect.segname, strnlen(sect.segname, 16)) + "," + std::string(sect.sectname, strnlen(sect.sectname, 16)),
                    sect.addr, image.subspan(sect.offset, sect.size) });
            }
        }
        at += lc.cmdsize;
    }
    return found;
}

// The function an address of __TEXT,__text is in: the last symbol at or before it.
struct Functions
{
    std::vector<GameImage::NamedVA> Symbols;

    std::string_view At(uint64_t va) const
    {
        auto it = std::upper_bound(Symbols.begin(), Symbols.end(), va, [](uint64_t v, GameImage::NamedVA const& s) { return v < s.VA; });
        if (it == Symbols.begin()) return {};
        return std::prev(it)->Name;
    }
};

constexpr char kIsStandardModule[] = "__ZNK2ls15ModuleShortDesc16IsStandardModuleEv";

// The callers of IsStandardModule that do not ask whether the game is modded:
// each does something for every module that is, or is not, the game's own,
// and none of them is ls::ModuleSettings::IsModded on Windows either.
struct NotConcerned
{
    char const* Prefix;
    char const* What;
};
constexpr NotConcerned kNotConcerned[] = {
    { "__ZN12_GLOBAL__N_129RegisterVirtualTextureTileSetE", "registers a module's virtual textures unless it is the game's" },
    { "__ZN2ls31UnregisterVirtualTextureTileSetE", "and removes them" },
    { "__ZN2ls10ModManager16DisableConflictsE", "disables the mods that conflict, never the game's modules" },
    { "__ZN2ls3mod27BuildLoadOrderedModulesListE", "orders the modules to load" },
    { "__ZN2ls8FunctionIFvvEE18FunctorMethodsVSSOIZN3ecl16ModuleLoadSystem15DoLoadResourcesEvE3$_1E4CallE", "loads each mod's resources" },
    { "__ZN3ecl16ModManagerClient23GetModSettingsMissmatchE", "lists the modules a host and a client do not share" },
    { "__ZN3esv12LoadProtocol12LoadSavegameE", "removes the game's own modules from a savegame's list before comparing it" },
    { "__ZN3esv12LoadProtocol18LoadModuleAndLevelE", "collects the game's own modules of the settings" },
    { "__ZN3gui11DCSavegames16UpdateSavegameVME", "lists a savegame's mods on the savegame screen" },
    { "__ZN3gui11DCSavegames25CheckLoadSelectedSavegameE", "counts a savegame's mods before it is loaded" },
    { "__ZN3gui12DCModBrowser33OnUpdateSelectedModDetailsCommandE", "shows a mod's details in the mod browser" },
    { "__ZN3gui17DCModVerification21UpdateModMismatchInfoE", "shows which mods a savegame needs" },
};

// Who reads GEFS::strSharedUIModUUID, the module id
// d65cf1b6-23a8-f0db-0a56-4b479a559748 that the pattern of Norbyte's Windows
// patch compares with: IsStandardModule's last comparison, and the code that
// makes and releases the string.
constexpr char kSharedUIModId[] = "__ZN4GEFS18strSharedUIModUUIDE";
constexpr char const* kSharedUIModIdReaders[] = {
    kIsStandardModule,
    "__ZN4GEFS4InitEv",
    "__ZN4GEFS8ShutdownEv",
    "__GLOBAL__sub_I_GameEngineFixedStrings.cpp",
};

int64_t SignExtend(uint64_t value, unsigned bits)
{
    uint64_t sign = 1ull << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}

bool IsAdrp(uint32_t w) { return (w & 0x9f000000u) == 0x90000000u; }

// The bytes at [address, address + bytes) that an instruction using `reg` as
// its base reaches from the page an adrp put in it: a load or store with an
// unsigned or unscaled offset, a pair, or an add of an immediate (which takes
// the address itself). False for any other instruction.
bool Reaches(uint32_t w, uint64_t page, uint64_t& address, uint64_t& bytes)
{
    int64_t offset = 0;
    if ((w & 0x3f000000u) == 0x39000000u) {                     // ldr/str (unsigned offset), any size
        bytes = 1ull << (w >> 30);
        offset = static_cast<int64_t>(((w >> 10) & 0xfff) * bytes);
    } else if ((w & 0x3f200c00u) == 0x38000000u) {              // ldur/stur
        bytes = 1ull << (w >> 30);
        offset = SignExtend((w >> 12) & 0x1ff, 9);
    } else if ((w & 0x3fc00000u) == 0x29400000u || (w & 0x3fc00000u) == 0x29000000u) {    // ldp/stp (signed offset)
        uint64_t size = (w >> 31) ? 8 : 4;
        bytes = 2 * size;
        offset = SignExtend((w >> 15) & 0x7f, 7) * static_cast<int64_t>(size);
    } else if ((w & 0x7fc00000u) == 0x11000000u) {              // add (immediate, unshifted)
        bytes = 1;
        offset = (w >> 10) & 0xfff;
    } else {
        return false;
    }
    address = page + static_cast<uint64_t>(offset);
    return true;
}

int CheckGame(std::string const& app)
{
    std::string path = app + "/Contents/MacOS/Baldur's Gate 3";
    auto file = Map(path);
    if (file.empty()) return 1;
    GameImage image;
    if (!image.OpenFile(file)) {
        fprintf(stderr, "%s: no readable arm64 symbol table\n", path.c_str());
        return 1;
    }
    auto sections = DataSections(Arm64Slice(file));
    CHECK(!sections.empty(), "%s: no data section read", path.c_str());
    // The pointers on disk are the link-time addresses only when dyld rebases
    // them with opcodes; with chained fixups the scan of the data below would
    // find nothing whatever is there.
    CHECK(!image.PointersOnlyAtRunTime(), "this game's pointers are chained fixups: the scan of its data does not apply");
    Functions functions{ image.TextSymbols() };

    uint64_t isStandard = image.FindVA(kIsStandardModule);
    uint64_t sharedUIId = image.FindVA(kSharedUIModId);
    CHECK(isStandard != 0 && sharedUIId != 0, "IsStandardModule or the shared UI module's id is not in this game");
    if (!isStandard || !sharedUIId) return 1;

    // The functions the patch covers, from the table.
    std::map<std::string, unsigned> patched;
    for (auto const& mapping : binding::CodePatchMappings()) {
        if (std::string_view(mapping.Mapping) != "ls::ModuleSettings::IsModded") continue;
        for (auto name : mapping.Sites) {
            auto entry = TableEntry(name);
            CHECK(entry && entry->Derived && entry->Derived->Host.Symbol, "%s: no host in the table", name);
            if (entry && entry->Derived && entry->Derived->Host.Symbol) patched[entry->Derived->Host.Symbol]++;
        }
    }
    CHECK(!patched.empty(), "the table has no site of the patch of IsModded");

    std::map<std::string, unsigned> calls;
    std::map<std::string, unsigned> readers;
    unsigned tailCalls = 0, addressTaken = 0;
    for (uint64_t va = image.TextBegin(); va + 4 <= image.TextEnd(); va += 4) {
        uint32_t word = image.Word(va);
        bool bl = (word & 0xfc000000u) == 0x94000000u, b = (word & 0xfc000000u) == 0x14000000u;
        if (bl || b) {
            uint64_t target = va + static_cast<uint64_t>(SignExtend(word & 0x03ffffffu, 26) * 4);
            if (target != isStandard) continue;
            CHECK(bl, "%s ends in a jump to IsStandardModule: its caller's answer is IsStandardModule's", std::string(functions.At(va)).c_str());
            if (b) tailCalls++;
            if (bl) calls[std::string(functions.At(va))]++;
            continue;
        }
        if (!IsAdrp(word)) continue;
        // adrp: the page of the address an instruction after it completes.
        unsigned reg = word & 0x1f;
        uint64_t imm = ((word >> 29) & 3) | (((word >> 5) & 0x7ffff) << 2);
        uint64_t page = (va & ~0xfffull) + static_cast<uint64_t>(SignExtend(imm, 21) << 12);
        bool idPage = page == (sharedUIId & ~0xfffull), functionPage = page == (isStandard & ~0xfffull);
        if (!idPage && !functionPage) continue;
        // Every load, store or add with that register as its base within the
        // next eight instructions, until an adrp gives the register another page.
        for (uint64_t next = va + 4; next < va + 4 * 9 && next + 4 <= image.TextEnd(); next += 4) {
            uint32_t w = image.Word(next);
            if (IsAdrp(w) && (w & 0x1f) == reg) break;
            if (((w >> 5) & 0x1f) != reg) continue;
            uint64_t address = 0, bytes = 0;
            if (!Reaches(w, page, address, bytes)) continue;
            if (idPage && sharedUIId >= address && sharedUIId < address + bytes) readers[std::string(functions.At(next))]++;
            if (functionPage && address == isStandard) {
                CHECK(false, "%s takes the address of IsStandardModule", std::string(functions.At(next)).c_str());
                addressTaken++;
            }
        }
    }

    // A pointer to it in the game's data: a table of functions, a vtable.
    unsigned pointers = 0;
    size_t scanned = 0;
    for (auto const& section : sections) {
        for (size_t at = (8 - section.VA % 8) % 8; at + 8 <= section.Bytes.size(); at += 8) {
            uint64_t value;
            memcpy(&value, section.Bytes.data() + at, 8);
            if (value != isStandard) continue;
            CHECK(false, "%s+0x%zx holds a pointer to IsStandardModule", section.Name.c_str(), at);
            pointers++;
        }
        scanned += section.Bytes.size();
    }
    printf("IsStandardModule: %u tail call(s), %u instruction(s) that take its address, %u pointer(s) to it in %zu bytes of data\n",
        tailCalls, addressTaken, pointers, scanned);

    unsigned total = 0, covered = 0, unconcerned = 0;
    for (auto const& [host, count] : calls) {
        total += count;
        auto p = patched.find(host);
        if (p != patched.end()) {
            CHECK(p->second == count, "%s calls IsStandardModule %u times, the patch has %u sites in it", host.c_str(), count, p->second);
            covered += count;
            continue;
        }
        bool listed = false;
        for (auto const& other : kNotConcerned) listed = listed || std::string_view(host).starts_with(other.Prefix);
        CHECK(listed, "%s calls IsStandardModule and is neither patched nor known not to ask whether the game is modded", host.c_str());
        if (listed) unconcerned += count;
    }
    for (auto const& [host, count] : patched) {
        CHECK(calls.count(host) != 0, "%s: a function the patch covers does not call IsStandardModule", host.c_str());
    }
    printf("%u calls of IsStandardModule: %u in the %zu functions the patch of IsModded covers, %u in the %zu that do not ask whether "
           "the game is modded\n", total, covered, patched.size(), unconcerned, std::size(kNotConcerned));

    for (auto const& [reader, count] : readers) {
        bool known = false;
        for (auto name : kSharedUIModIdReaders) known = known || reader == name;
        CHECK(known, "%s reads the module id Norbyte's Windows patch compares with: an inlined IsStandardModule?", reader.c_str());
    }
    printf("the module id of Norbyte's Windows pattern is read by %zu functions: IsStandardModule and the code that makes it\n",
        readers.size());
    CHECK(readers.count(kIsStandardModule) == 1, "IsStandardModule does not read the module id it is known to compare");

    printf("%s\n", gFailures == 0 ? "every place that answers whether the game is modded is patched or not concerned"
                                  : "THE PATCH OF ISMODDED DOES NOT COVER THIS GAME");
    return gFailures == 0 ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    SetLogStream(stdout);
    if (argc != 2) {
        fprintf(stderr, "usage: %s <Baldur's Gate 3.app>\n", argv[0]);
        return 2;
    }
    return CheckGame(argv[1]);
}
