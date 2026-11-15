#include <Platform/FunctionExtents.h>
#include <Platform/macho_symbols.h>

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mutex>

namespace bg3se::mac {

char const* Describe(ExtentRefusal refusal)
{
    switch (refusal) {
    case ExtentRefusal::None: return "a function start";
    case ExtentRefusal::NotInImage: return "no loaded image holds the address";
    case ExtentRefusal::NoTable: return "the image has no table of functions that can be read";
    case ExtentRefusal::NotCode: return "the address is not in a code section of its image";
    case ExtentRefusal::Stub: return "the address is a dyld stub, a jump to a function of another image";
    case ExtentRefusal::NotAStart: return "the address is inside a function, not at its start";
    }
    return "?";
}

char const* Describe(FunctionSource source)
{
    switch (source) {
    case FunctionSource::None: return "none";
    case FunctionSource::FunctionStarts: return "LC_FUNCTION_STARTS";
    case FunctionSource::SymbolTable: return "the symbol table";
    }
    return "?";
}

namespace {

// One ULEB128 value; false when it runs past `end` or does not fit 64 bits.
bool ReadUleb(uint8_t const*& p, uint8_t const* end, uint64_t& value)
{
    value = 0;
    for (unsigned shift = 0; p < end; shift += 7) {
        uint8_t byte = *p++;
        uint64_t bits = byte & 0x7f;
        if (shift >= 64 || (shift > 57 && (bits >> (64 - shift)))) return false;
        value |= bits << shift;
        if (!(byte & 0x80)) return true;
    }
    return false;
}

// The arm64 slice of a file: the file itself when it is thin.
std::span<uint8_t const> Arm64Slice(std::span<uint8_t const> file)
{
    if (file.size() < sizeof(mach_header_64)) return {};
    uint32_t magic = OSReadBigInt32(file.data(), 0);
    if (magic != FAT_MAGIC && magic != FAT_MAGIC_64) return file;
    bool wide = magic == FAT_MAGIC_64;
    uint64_t count = OSReadBigInt32(file.data(), 4);
    size_t entry = wide ? sizeof(fat_arch_64) : sizeof(fat_arch);
    if (sizeof(fat_header) + count * entry > file.size()) return {};
    for (uint64_t i = 0; i < count; i++) {
        uint8_t const* arch = file.data() + sizeof(fat_header) + i * entry;
        if ((cpu_type_t)OSReadBigInt32(arch, 0) != CPU_TYPE_ARM64) continue;
        uint64_t offset = wide ? OSReadBigInt64(arch, 8) : OSReadBigInt32(arch, 8);
        uint64_t size = wide ? OSReadBigInt64(arch, 16) : OSReadBigInt32(arch, 12);
        if (offset > file.size() || size > file.size() - offset || size < sizeof(mach_header_64)) return {};
        return file.subspan(offset, size);
    }
    return {};
}

}

bool FunctionTable::Build(void const* header, uint8_t const* linkedit, uint64_t limit, intptr_t slide, std::string& problem)
{
    *this = FunctionTable{};
    auto mh = static_cast<mach_header_64 const*>(header);
    if (mh->magic != MH_MAGIC_64 || mh->cputype != CPU_TYPE_ARM64) {
        problem = "not an arm64 Mach-O image";
        return false;
    }
    if (limit && sizeof(*mh) + (uint64_t)mh->sizeofcmds > limit) {
        problem = "the load commands run past the end of the file";
        return false;
    }
    auto commands = reinterpret_cast<uint8_t const*>(mh + 1);
    auto commandsEnd = commands + mh->sizeofcmds;
    commands_.assign(commands, commandsEnd);

    segment_command_64 const* text = nullptr;
    segment_command_64 const* linkeditSegment = nullptr;
    linkedit_data_command const* functionStarts = nullptr;
    struct RawSection
    {
        uint64_t Address, Size;
        bool Code, Stubs;
    };
    std::vector<RawSection> raw;    // in load-command order: nlist's n_sect is a 1-based index into it
    uint8_t const* p = commands;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        auto lc = reinterpret_cast<load_command const*>(p);
        if (p + sizeof(*lc) > commandsEnd || lc->cmdsize < sizeof(*lc) || lc->cmdsize > (uint64_t)(commandsEnd - p)) {
            problem = "a load command runs past the others";
            return false;
        }
        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(segment_command_64)) {
            auto seg = reinterpret_cast<segment_command_64 const*>(lc);
            if (lc->cmdsize < sizeof(*seg) + (uint64_t)seg->nsects * sizeof(section_64)) {
                problem = "a segment's sections run past its load command";
                return false;
            }
            if (strncmp(seg->segname, SEG_TEXT, sizeof(seg->segname)) == 0) text = seg;
            if (strncmp(seg->segname, SEG_LINKEDIT, sizeof(seg->segname)) == 0) linkeditSegment = seg;
            auto sect = reinterpret_cast<section_64 const*>(seg + 1);
            for (uint32_t s = 0; s < seg->nsects; s++) {
                bool code = (sect[s].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) != 0;
                bool stubs = (sect[s].flags & SECTION_TYPE) == S_SYMBOL_STUBS;
                raw.push_back({ sect[s].addr, sect[s].size, code, stubs });
            }
        } else if (lc->cmd == LC_FUNCTION_STARTS && lc->cmdsize >= sizeof(linkedit_data_command)) {
            functionStarts = reinterpret_cast<linkedit_data_command const*>(lc);
        }
        p += lc->cmdsize;
    }
    if (!text) {
        problem = "the image has no __TEXT segment";
        return false;
    }
    base_ = text->vmaddr + (uint64_t)slide;

    // Code sections as offsets from __TEXT; a 32-bit offset reaches all of them.
    for (auto const& s : raw) {
        if (!s.Code || !s.Size) continue;
        if (s.Address < text->vmaddr || s.Address - text->vmaddr + s.Size > UINT32_MAX) {
            problem = "a code section lies below __TEXT or more than 4 GB above it";
            return false;
        }
        uint32_t begin = (uint32_t)(s.Address - text->vmaddr);
        sections_.push_back({ begin, (uint32_t)(begin + s.Size), s.Stubs });
    }
    std::sort(sections_.begin(), sections_.end(), [](Section const& a, Section const& b) { return a.Begin < b.Begin; });
    for (size_t i = 1; i < sections_.size(); i++) {
        if (sections_[i].Begin < sections_[i - 1].End) {
            problem = "two code sections overlap";
            return false;
        }
    }
    auto inCode = [&](uint64_t offset) {
        auto it = std::upper_bound(sections_.begin(), sections_.end(), offset, [](uint64_t o, Section const& s) { return o < s.Begin; });
        return it != sections_.begin() && offset < std::prev(it)->End && !std::prev(it)->Stubs;
    };

    // `size` bytes at file offset `offset` of __LINKEDIT, or nullptr.
    auto linkeditBytes = [&](uint64_t offset, uint64_t size) -> uint8_t const* {
        if (limit) return offset <= limit && size <= limit - offset ? linkedit + offset : nullptr;
        if (!linkeditSegment || offset < linkeditSegment->fileoff) return nullptr;
        uint64_t end = linkeditSegment->fileoff + linkeditSegment->filesize;
        return offset <= end && size <= end - offset ? linkedit + offset : nullptr;
    };

    if (functionStarts) {
        uint8_t const* data = linkeditBytes(functionStarts->dataoff, functionStarts->datasize);
        if (!data) {
            problem = "the function starts table lies outside __LINKEDIT";
            return false;
        }
        uint8_t const* end = data + functionStarts->datasize;
        uint64_t offset = 0;
        for (uint8_t const* at = data; at < end;) {
            uint64_t delta;
            if (!ReadUleb(at, end, delta)) {
                problem = "the function starts table is truncated";
                return false;
            }
            if (!delta) break;
            if (delta > UINT32_MAX - offset) {
                problem = "the function starts table runs more than 4 GB past __TEXT";
                return false;
            }
            offset += delta;
            if (inCode(offset)) starts_.push_back((uint32_t)offset);
        }
        source_ = FunctionSource::FunctionStarts;
        return true;
    }

    // No function starts: the defined symbols in code sections.
    MachoSymtab symtab{};
    bool haveSymbols = limit ? macho_symtab_from_file(header, (size_t)limit, &symtab) : macho_symtab_from_image(header, slide, &symtab);
    if (!haveSymbols) {
        problem = "the image has neither a function starts table nor a readable symbol table";
        return false;
    }
    auto symbols = static_cast<struct nlist_64 const*>(symtab.symbols);
    for (uint32_t i = 0; i < symtab.nsyms; i++) {
        auto const& sym = symbols[i];
        if ((sym.n_type & N_STAB) || (sym.n_type & N_TYPE) != N_SECT) continue;
        if (sym.n_sect == NO_SECT || sym.n_sect > raw.size() || !raw[sym.n_sect - 1].Code) continue;
        if (sym.n_value < text->vmaddr || sym.n_value - text->vmaddr > UINT32_MAX) continue;
        uint64_t offset = sym.n_value - text->vmaddr;
        if (inCode(offset)) starts_.push_back((uint32_t)offset);
    }
    std::sort(starts_.begin(), starts_.end());
    starts_.erase(std::unique(starts_.begin(), starts_.end()), starts_.end());
    source_ = FunctionSource::SymbolTable;
    return true;
}

bool FunctionTable::OpenLoaded(void const* header, std::string& problem)
{
    *this = FunctionTable{};
    problem.clear();
    auto mh = static_cast<mach_header_64 const*>(header);
    if (!mh || mh->magic != MH_MAGIC_64) {
        problem = "not a Mach-O header";
        return false;
    }
    // The slide is where the header is against where __TEXT was linked.
    uint64_t textVA = 0, linkeditVA = 0, linkeditOffset = 0;
    bool haveText = false;
    auto p = reinterpret_cast<uint8_t const*>(mh + 1);
    auto end = p + mh->sizeofcmds;
    for (uint32_t i = 0; i < mh->ncmds && p + sizeof(load_command) <= end; i++) {
        auto lc = reinterpret_cast<load_command const*>(p);
        if (lc->cmdsize < sizeof(*lc) || lc->cmdsize > (uint64_t)(end - p)) break;
        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(segment_command_64)) {
            auto seg = reinterpret_cast<segment_command_64 const*>(lc);
            if (strncmp(seg->segname, SEG_TEXT, sizeof(seg->segname)) == 0) {
                textVA = seg->vmaddr;
                haveText = true;
            }
            if (strncmp(seg->segname, SEG_LINKEDIT, sizeof(seg->segname)) == 0) {
                linkeditVA = seg->vmaddr;
                linkeditOffset = seg->fileoff;
            }
        }
        p += lc->cmdsize;
    }
    if (!haveText) {
        problem = "the image has no __TEXT segment";
        return false;
    }
    intptr_t slide = (intptr_t)((uintptr_t)header - textVA);
    // __LINKEDIT's file offsets, mapped where dyld put the segment.
    auto linkedit = reinterpret_cast<uint8_t const*>((uintptr_t)(linkeditVA + (uint64_t)slide - linkeditOffset));
    return Build(header, linkedit, 0, slide, problem);
}

bool FunctionTable::OpenFile(std::span<uint8_t const> file, std::string& problem)
{
    *this = FunctionTable{};
    problem.clear();
    auto slice = Arm64Slice(file);
    if (slice.empty()) {
        problem = "no arm64 Mach-O image in the file";
        return false;
    }
    return Build(slice.data(), slice.data(), slice.size(), 0, problem);
}

FunctionExtent FunctionTable::At(uint64_t address) const
{
    FunctionExtent extent;
    extent.Source = source_;
    if (source_ == FunctionSource::None) return extent;
    extent.Refusal = ExtentRefusal::NotCode;
    if (address < base_ || address - base_ > UINT32_MAX) return extent;
    auto offset = (uint32_t)(address - base_);
    auto section = std::upper_bound(sections_.begin(), sections_.end(), offset, [](uint32_t o, Section const& s) { return o < s.Begin; });
    if (section == sections_.begin() || offset >= std::prev(section)->End) return extent;
    --section;
    if (section->Stubs) {
        extent.Refusal = ExtentRefusal::Stub;
        return extent;
    }
    auto start = std::lower_bound(starts_.begin(), starts_.end(), offset);
    if (start == starts_.end() || *start != offset) {
        extent.Refusal = ExtentRefusal::NotAStart;
        return extent;
    }
    uint32_t end = section->End;
    if (auto next = std::next(start); next != starts_.end() && *next < end) end = *next;
    extent.Start = address;
    extent.End = base_ + end;
    extent.Refusal = ExtentRefusal::None;
    return extent;
}

namespace {

struct CachedTable
{
    void const* Header;
    FunctionTable Table;
    CachedTable* Next;
};

// Constant-initialised and never freed: a dylib constructor may look up a
// function before this file's dynamic initialisers have run.
constinit std::mutex gTablesLock;
constinit CachedTable* gTables = nullptr;

// Whether the image at `header` still has the load commands `table` was built from.
bool SameImage(void const* header, FunctionTable const& table)
{
    auto mh = static_cast<mach_header_64 const*>(header);
    auto commands = table.LoadCommands();
    return mh->sizeofcmds == commands.size() && memcmp(mh + 1, commands.data(), commands.size()) == 0;
}

}

FunctionExtent FindLoadedFunction(void const* address)
{
    // Before the lock: dladdr takes the dynamic loader's.
    Dl_info info{};
    if (!address || !dladdr(address, &info) || !info.dli_fbase) {
        FunctionExtent none;
        none.Refusal = ExtentRefusal::NotInImage;
        return none;
    }
    void const* header = info.dli_fbase;

    std::lock_guard lock(gTablesLock);
    CachedTable* cached = nullptr;
    for (CachedTable* c = gTables; c; c = c->Next) {
        if (c->Header == header) {
            cached = c;
            break;
        }
    }
    if (!cached) {
        cached = new CachedTable{ header, {}, gTables };
        gTables = cached;
        std::string problem;
        cached->Table.OpenLoaded(header, problem);
    } else if (!SameImage(header, cached->Table)) {
        std::string problem;
        cached->Table.OpenLoaded(header, problem);
    }
    return cached->Table.At(reinterpret_cast<uint64_t>(address));
}

}
