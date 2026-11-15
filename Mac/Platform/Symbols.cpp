#include <Platform/Symbols.h>

#include <algorithm>
#include <cstring>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

namespace bg3se::mac {

GameImage const* GameImage::Current()
{
    static GameImage image;
    static bool ok = macho_symtab_main_image(&image.symtab_) && (image.ReadLoadCommands(), true);
    return ok ? &image : nullptr;
}

bool GameImage::OpenFile(std::span<uint8_t const> file)
{
    if (!macho_symtab_from_file(file.data(), file.size(), &symtab_)) return false;
    fromFile_ = true;
    fileEnd_ = file.data() + file.size();
    ReadLoadCommands();
    return true;
}

bool GameImage::OpenLoaded(void const* header)
{
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        if (static_cast<void const*>(_dyld_get_image_header(i)) != header) continue;
        if (!macho_symtab_from_image(header, _dyld_get_image_vmaddr_slide(i), &symtab_)) return false;
        ReadLoadCommands();
        return true;
    }
    return false;
}

void const* FindLoadedImage(std::string_view leafName)
{
    uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        char const* path = _dyld_get_image_name(i);
        if (!path) continue;
        std::string_view name(path);
        if (auto slash = name.rfind('/'); slash != std::string_view::npos) name.remove_prefix(slash + 1);
        if (name == leafName) return _dyld_get_image_header(i);
    }
    return nullptr;
}

void GameImage::ReadLoadCommands()
{
    // The symbol table reader has already checked the load commands' bounds.
    auto mh = static_cast<mach_header_64 const*>(symtab_.header);
    auto p = reinterpret_cast<uint8_t const*>(mh + 1);
    uint32_t sectionNumber = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        auto lc = reinterpret_cast<load_command const*>(p);
        if (lc->cmd == LC_DYLD_CHAINED_FIXUPS) chainedFixups_ = true;
        if (lc->cmd == LC_SEGMENT_64) {
            auto seg = reinterpret_cast<segment_command_64 const*>(lc);
            segments_.push_back({ seg->vmaddr, seg->vmsize, seg->fileoff, seg->filesize });
            auto sect = reinterpret_cast<section_64 const*>(seg + 1);
            for (uint32_t j = 0; j < seg->nsects && lc->cmdsize >= sizeof(*seg) + (j + 1) * sizeof(*sect); j++) {
                sectionNumber++;
                Section section;
                memcpy(section.Segment, sect[j].segname, sizeof(sect[j].segname));
                memcpy(section.Name, sect[j].sectname, sizeof(sect[j].sectname));
                section.Begin = sect[j].addr;
                section.End = sect[j].addr + sect[j].size;
                uint32_t type = sect[j].flags & SECTION_TYPE;
                section.ZeroFill = type == S_ZEROFILL || type == S_GB_ZEROFILL || type == S_THREAD_LOCAL_ZEROFILL;
                section.Code = (sect[j].flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) != 0;
                sections_.push_back(section);
                if (strncmp(seg->segname, SEG_TEXT, sizeof(seg->segname)) == 0 &&
                    strncmp(sect[j].sectname, SECT_TEXT, sizeof(sect[j].sectname)) == 0) {
                    textVA_ = sect[j].addr;
                    textSize_ = sect[j].size;
                    textSection_ = sectionNumber;
                }
                if ((sect[j].flags & SECTION_TYPE) == S_SYMBOL_STUBS && sect[j].reserved2 &&
                    strncmp(sect[j].sectname, "__stubs", sizeof(sect[j].sectname)) == 0) {
                    stubsVA_ = sect[j].addr;
                    stubsSize_ = sect[j].size;
                    stubsIndirect_ = sect[j].reserved1;
                    stubSize_ = sect[j].reserved2;
                }
            }
        }
        p += lc->cmdsize;
    }
}

void const* GameImage::FindCode(char const* mangledName) const
{
    uint64_t va = FindVA(mangledName);
    return va ? CodeAt(va) : nullptr;
}

namespace {

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

}

// The names macho_symtab_resolve takes (defined in a section, not a debugging
// entry, a terminated name in the string table), each at the slot its hash
// gives or the next free one after it: the index of the first symbol of that
// name + 1, with kAmbiguous set once another symbol of the name has another
// address. Made of the table it names, so that an image opened again is
// indexed again.
struct GameImage::NameIndex
{
    static constexpr uint32_t kAmbiguous = 0x80000000u;
    void const* Of{ nullptr };
    uint32_t Count{ 0 };
    std::vector<uint32_t> Slots;
};

std::shared_ptr<GameImage::NameIndex const> GameImage::Names() const
{
    std::lock_guard lock(namesMutex_);
    if (names_ && names_->Of == symtab_.symbols && names_->Count == symtab_.nsyms) return names_;
    auto index = std::make_shared<NameIndex>();
    index->Of = symtab_.symbols;
    index->Count = symtab_.nsyms;
    auto symbols = static_cast<struct nlist_64 const*>(symtab_.symbols);
    size_t cap = 16;
    while (cap < (size_t)symtab_.nsyms * 2) cap <<= 1;
    index->Slots.assign(cap, 0);
    char const* end = symtab_.strings + symtab_.strsize;
    uint32_t count = symbols ? std::min<uint32_t>(symtab_.nsyms, NameIndex::kAmbiguous - 1) : 0;
    for (uint32_t k = 0; k < count; k++) {
        auto const& sym = symbols[k];
        if ((sym.n_type & N_STAB) || (sym.n_type & N_TYPE) != N_SECT) continue;
        uint32_t strx = sym.n_un.n_strx;
        if (strx == 0 || strx >= symtab_.strsize) continue;
        char const* name = symtab_.strings + strx;
        uint64_t h = kFnvOffset;
        char const* c = name;
        for (; c < end && *c; c++) h = (h ^ (uint8_t)*c) * kFnvPrime;
        if (c == end) continue;
        for (size_t s = h & (cap - 1);; s = (s + 1) & (cap - 1)) {
            uint32_t& slot = index->Slots[s];
            if (!slot) {
                slot = k + 1;
                break;
            }
            auto const& first = symbols[(slot & ~NameIndex::kAmbiguous) - 1];
            if (strcmp(symtab_.strings + first.n_un.n_strx, name) != 0) continue;
            if (first.n_value != sym.n_value) slot |= NameIndex::kAmbiguous;
            break;
        }
    }
    names_ = std::move(index);
    return names_;
}

uint64_t GameImage::FindVA(char const* mangledName) const
{
    if (!mangledName || !*mangledName) return 0;
    auto index = Names();
    auto const& slots = index->Slots;
    auto symbols = static_cast<struct nlist_64 const*>(symtab_.symbols);
    uint64_t h = kFnvOffset;
    for (char const* c = mangledName; *c; c++) h = (h ^ (uint8_t)*c) * kFnvPrime;
    size_t mask = slots.size() - 1;
    for (size_t s = h & mask; slots[s]; s = (s + 1) & mask) {
        uint32_t slot = slots[s];
        auto const& sym = symbols[(slot & ~NameIndex::kAmbiguous) - 1];
        if (strcmp(symtab_.strings + sym.n_un.n_strx, mangledName) != 0) continue;
        if (slot & NameIndex::kAmbiguous) return 0;
        return sym.n_value >= symtab_.text_vmaddr ? sym.n_value : 0;
    }
    return 0;
}

namespace {

// Calls fn(name, va) for every symbol defined in a section of the image.
template <class Fn>
void ForEachDefined(MachoSymtab const& st, Fn&& fn)
{
    auto symbols = static_cast<struct nlist_64 const*>(st.symbols);
    for (uint32_t i = 0; i < st.nsyms; i++) {
        auto const& sym = symbols[i];
        if ((sym.n_type & N_STAB) || (sym.n_type & N_TYPE) != N_SECT) continue;
        if (sym.n_un.n_strx >= st.strsize) continue;
        fn(st.strings + sym.n_un.n_strx, sym.n_value);
    }
}

}

uint64_t GameImage::FindVAByPrefix(std::string_view prefix) const
{
    uint64_t found = 0;
    bool ambiguous = false;
    ForEachDefined(symtab_, [&](char const* name, uint64_t va) {
        if (strncmp(name, prefix.data(), prefix.size()) != 0) return;
        if (found && found != va) ambiguous = true;
        found = va;
    });
    return ambiguous ? 0 : found;
}

void GameImage::FindVAs(std::span<char const* const> names, std::span<uint64_t> vas) const
{
    if (names.empty() || vas.size() < names.size()) return;
    std::vector<uint8_t> status(names.size());
    macho_symtab_resolve(&symtab_, names.data(), names.size(), vas.data(), status.data());
    for (size_t i = 0; i < names.size(); i++) {
        if (status[i] != MACHO_SYM_FOUND || vas[i] < symtab_.text_vmaddr) vas[i] = 0;
    }
}

void GameImage::Lookup(std::span<char const* const> names, std::span<Symbol> found) const
{
    if (names.empty() || found.size() < names.size()) return;
    std::vector<uint64_t> vas(names.size());
    std::vector<uint8_t> status(names.size());
    macho_symtab_resolve(&symtab_, names.data(), names.size(), vas.data(), status.data());
    for (size_t i = 0; i < names.size(); i++) {
        if (status[i] == MACHO_SYM_AMBIGUOUS) found[i] = { 0, SymbolStatus::Ambiguous };
        else if (status[i] == MACHO_SYM_FOUND && vas[i] >= symtab_.text_vmaddr) found[i] = { vas[i], SymbolStatus::Found };
        else found[i] = {};
    }
}

void GameImage::LookupByPrefix(std::span<std::string_view const> prefixes, std::span<Symbol> found) const
{
    if (found.size() < prefixes.size()) return;
    for (size_t i = 0; i < prefixes.size(); i++) found[i] = {};
    ForEachDefined(symtab_, [&](char const* name, uint64_t va) {
        for (size_t i = 0; i < prefixes.size(); i++) {
            auto const& prefix = prefixes[i];
            if (prefix.empty() || strncmp(name, prefix.data(), prefix.size()) != 0) continue;
            if (found[i].Status == SymbolStatus::Missing) found[i] = { va, SymbolStatus::Found };
            else if (found[i].Status == SymbolStatus::Found && found[i].VA != va) found[i] = { 0, SymbolStatus::Ambiguous };
        }
    });
}

std::vector<GameImage::NamedVA> GameImage::SymbolsNamed(std::string_view prefix, std::string_view suffix) const
{
    std::vector<NamedVA> found;
    char const* end = symtab_.strings + symtab_.strsize;
    ForEachDefined(symtab_, [&](char const* name, uint64_t va) {
        if (strncmp(name, prefix.data(), prefix.size()) != 0) return;
        std::string_view whole(name, strnlen(name, (size_t)(end - name)));
        if (whole.size() >= prefix.size() + suffix.size() && whole.ends_with(suffix)) found.push_back({ va, whole });
    });
    return found;
}

uint64_t GameImage::GotSlotOf(char const* name) const
{
    uint64_t slot = 0;
    uint8_t status = MACHO_SYM_MISSING;
    macho_symtab_resolve_got(&symtab_, &name, 1, &slot, &status);
    return status == MACHO_SYM_FOUND ? slot : 0;
}

uint64_t GameImage::StubOf(std::string_view name) const
{
    for (uint64_t va = stubsVA_; stubSize_ && va + stubSize_ <= stubsVA_ + stubsSize_; va += stubSize_) {
        if (StubName(va) == name) return va;
    }
    return 0;
}

std::optional<GameImage::Section> GameImage::SectionAt(uint64_t va) const
{
    for (auto const& section : sections_) {
        if (va >= section.Begin && va < section.End) return section;
    }
    return {};
}

void GameImage::NextSymbolVAs(std::span<uint64_t const> vas, std::span<uint64_t> ends) const
{
    if (ends.size() < vas.size()) return;
    std::vector<uint64_t> all;
    all.reserve(symtab_.nsyms);
    ForEachDefined(symtab_, [&](char const*, uint64_t va) { all.push_back(va); });
    std::sort(all.begin(), all.end());
    for (size_t i = 0; i < vas.size(); i++) {
        auto it = std::upper_bound(all.begin(), all.end(), vas[i]);
        ends[i] = vas[i] && it != all.end() ? *it : 0;
    }
}

std::vector<GameImage::NamedVA> GameImage::TextSymbols() const
{
    std::vector<NamedVA> found;
    if (!textSection_) return found;
    auto symbols = static_cast<struct nlist_64 const*>(symtab_.symbols);
    for (uint32_t i = 0; i < symtab_.nsyms; i++) {
        auto const& sym = symbols[i];
        if ((sym.n_type & N_STAB) || (sym.n_type & N_TYPE) != N_SECT || sym.n_sect != textSection_) continue;
        if (sym.n_un.n_strx >= symtab_.strsize || sym.n_value < TextBegin() || sym.n_value >= TextEnd()) continue;
        char const* name = symtab_.strings + sym.n_un.n_strx;
        found.push_back({ sym.n_value, { name, strnlen(name, symtab_.strsize - sym.n_un.n_strx) } });
    }
    // One per address, the first the table gives.
    std::stable_sort(found.begin(), found.end(), [](NamedVA const& a, NamedVA const& b) { return a.VA < b.VA; });
    found.erase(std::unique(found.begin(), found.end(), [](NamedVA const& a, NamedVA const& b) { return a.VA == b.VA; }), found.end());
    return found;
}

uint64_t GameImage::NextSymbolVA(uint64_t va) const
{
    uint64_t next = 0;
    ForEachDefined(symtab_, [&](char const*, uint64_t other) {
        if (other > va && (!next || other < next)) next = other;
    });
    return next;
}

void const* GameImage::CodeAt(uint64_t va) const
{
    // __TEXT starts at the Mach-O header, both in a loaded image and in a file slice.
    return static_cast<uint8_t const*>(symtab_.header) + (va - symtab_.text_vmaddr);
}

void* GameImage::LoadedAt(uint64_t va) const
{
    return const_cast<uint8_t*>(static_cast<uint8_t const*>(symtab_.header)) + (va - symtab_.text_vmaddr);
}

uint32_t GameImage::Word(uint64_t va) const
{
    uint32_t word;
    memcpy(&word, CodeAt(va), sizeof(word));
    return word;
}

void const* GameImage::DataAt(uint64_t va, size_t size) const
{
    for (auto const& seg : segments_) {
        if (va < seg.VMAddr || va - seg.VMAddr >= seg.VMSize) continue;
        uint64_t at = va - seg.VMAddr;
        if (!fromFile_) {
            if (seg.VMSize - at < size || seg.VMSize == 0) return nullptr;
            return LoadedAt(va);
        }
        if (at >= seg.FileSize || seg.FileSize - at < size) return nullptr;
        auto p = static_cast<uint8_t const*>(symtab_.header) + seg.FileOff + at;
        return p + size <= fileEnd_ ? p : nullptr;
    }
    return nullptr;
}

std::optional<uint64_t> GameImage::PointerAt(uint64_t va) const
{
    if (PointersOnlyAtRunTime()) return {};
    auto p = DataAt(va, sizeof(uint64_t));
    if (!p) return {};
    uint64_t value;
    memcpy(&value, p, sizeof(value));
    if (fromFile_ || !value) return value;
    return value - ((uint64_t)(uintptr_t)symtab_.header - symtab_.text_vmaddr);
}

std::string_view GameImage::StubName(uint64_t va) const
{
    if (!stubSize_ || va < stubsVA_ || va - stubsVA_ >= stubsSize_ || (va - stubsVA_) % stubSize_) return {};
    uint64_t index = stubsIndirect_ + (va - stubsVA_) / stubSize_;
    if (!symtab_.indirect || index >= symtab_.nindirect) return {};
    uint32_t symbol = symtab_.indirect[index];
    if (symbol & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS) || symbol >= symtab_.nsyms) return {};
    uint32_t strx = static_cast<struct nlist_64 const*>(symtab_.symbols)[symbol].n_un.n_strx;
    if (strx == 0 || strx >= symtab_.strsize) return {};
    char const* name = symtab_.strings + strx;
    return { name, strnlen(name, symtab_.strsize - strx) };
}

}
