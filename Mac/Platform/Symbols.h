#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <Platform/macho_symbols.h>

// Game addresses on macOS come from the game's own symbol table, by exact
// mangled name: the binary ships with all of its symbols. A mangled name
// encodes the parameter types, so a function whose signature changed in a
// patch is not found, instead of being found and called wrongly.

namespace bg3se::mac {

class GameImage
{
public:
    // The main executable of this process, or nullptr when its symbol table cannot be read.
    static GameImage const* Current();

    // A game binary mapped from disk (tools and tests). `file` must outlive the image.
    bool OpenFile(std::span<uint8_t const> file);

    // An image dyld has loaded, by the mach_header_64 it mapped: a dylib of
    // the game such as libOsiris.dylib (FindLoadedImage gives the header).
    bool OpenLoaded(void const* header);

    // Whether the image was opened from a file: its addresses then point into
    // the mapped file, and only code and initialised data can be read.
    bool FromFile() const { return fromFile_; }

    // Address of a function in this image's __TEXT, or nullptr when the symbol
    // is missing or defined at more than one address. For an image opened from
    // a file the address points into the mapped file.
    void const* FindCode(char const* mangledName) const;

    template <class T>
    T* FindFunction(char const* mangledName) const
    {
        return reinterpret_cast<T*>(const_cast<void*>(FindCode(mangledName)));
    }

    // The same lookups as link-time addresses, which also work on a file.
    // 0 when the symbol is missing or ambiguous. The first lookup indexes the
    // table by name (8 MB for the game's); each one after that is a probe of
    // the index, not a pass over the table.
    uint64_t FindVA(char const* mangledName) const;
    // For a symbol whose full name is long and likely to change (a template
    // parameter list): the one defined symbol that starts with `prefix`.
    uint64_t FindVAByPrefix(std::string_view prefix) const;
    // Where the function at `va` ends: the next defined symbol after it.
    uint64_t NextSymbolVA(uint64_t va) const;

    // Many lookups in one pass over the symbol table each (the game's has
    // about 770k symbols): vas[i] = FindVA(names[i]), ends[i] = NextSymbolVA(vas[i]).
    void FindVAs(std::span<char const* const> names, std::span<uint64_t> vas) const;
    void NextSymbolVAs(std::span<uint64_t const> vas, std::span<uint64_t> ends) const;

    // The same lookups for a caller that must tell a symbol that is not there
    // from one that is defined at two addresses. VA is 0 unless Found.
    enum class SymbolStatus : uint8_t { Missing, Found, Ambiguous };
    struct Symbol
    {
        uint64_t VA{ 0 };
        SymbolStatus Status{ SymbolStatus::Missing };
    };
    void Lookup(std::span<char const* const> names, std::span<Symbol> found) const;
    // found[i] is the one address the defined symbols starting with
    // prefixes[i] name; all prefixes in one pass over the table.
    void LookupByPrefix(std::span<std::string_view const> prefixes, std::span<Symbol> found) const;

    struct NamedVA
    {
        uint64_t VA;
        std::string_view Name;
    };
    // Every defined symbol, in any section, whose name starts with `prefix`
    // and ends with `suffix`, in the table's order.
    std::vector<NamedVA> SymbolsNamed(std::string_view prefix, std::string_view suffix) const;

    // The non-lazy pointer (__got) slot through which the image's code reaches
    // the imported or exported symbol `name`; 0 when there is none or more than one.
    uint64_t GotSlotOf(char const* name) const;

    // The section holding a link-time address.
    struct Section
    {
        char Segment[17]{};
        char Name[17]{};
        uint64_t Begin{ 0 }, End{ 0 };
        bool ZeroFill{ false };     // no contents in the file: zero until the game writes it
        bool Code{ false };
    };
    std::optional<Section> SectionAt(uint64_t va) const;

    // Every address in __TEXT,__text that a defined symbol names, ascending,
    // with the first name the table gives it.
    std::vector<NamedVA> TextSymbols() const;
    // __TEXT,__text as link-time addresses [begin, end).
    uint64_t TextBegin() const { return textVA_; }
    uint64_t TextEnd() const { return textVA_ + textSize_; }

    uint64_t BaseVA() const { return symtab_.text_vmaddr; }
    // Code at a link-time address (inside __TEXT).
    void const* CodeAt(uint64_t va) const;
    // Any address of the loaded image; for an image opened from a file only code is reachable.
    void* LoadedAt(uint64_t va) const;

    // The instruction at a link-time address in __TEXT.
    uint32_t Word(uint64_t va) const;

    // `size` bytes at a link-time address in any segment, in the loaded image
    // or in the file; nullptr when they are not all inside one segment's
    // contents (zero-fill included only for a loaded image).
    void const* DataAt(uint64_t va, size_t size) const;

    // The link-time address a pointer stored at `va` holds, as dyld leaves it
    // (in a loaded image the pointer is slid; the slide is taken off). For a
    // file whose pointers are chained fixups there is no plain value on disk:
    // nothing is returned, and such a lookup only works in the running game.
    std::optional<uint64_t> PointerAt(uint64_t va) const;
    bool PointersOnlyAtRunTime() const { return fromFile_ && chainedFixups_; }

    // The imported symbol a __stubs entry at `va` jumps to (e.g. "_longjmp"),
    // or an empty view when `va` is not a stub.
    std::string_view StubName(uint64_t va) const;
    // The __stubs entry for the imported symbol `name`, or 0 when the image has none.
    uint64_t StubOf(std::string_view name) const;

private:
    struct Segment
    {
        uint64_t VMAddr, VMSize, FileOff, FileSize;
    };

    void ReadLoadCommands();

    struct NameIndex;
    std::shared_ptr<NameIndex const> Names() const;

    MachoSymtab symtab_{};
    bool fromFile_{ false };
    uint8_t const* fileEnd_{ nullptr };
    bool chainedFixups_{ false };
    std::vector<Segment> segments_;
    uint64_t stubsVA_{ 0 }, stubsSize_{ 0 };
    uint64_t textVA_{ 0 }, textSize_{ 0 };
    uint32_t textSection_{ 0 };         // 1-based section number, as nlist's n_sect
    uint32_t stubsIndirect_{ 0 }, stubSize_{ 0 };
    std::vector<Section> sections_;
    // Made by the first FindVA after the image is opened, from whichever thread asks first.
    mutable std::mutex namesMutex_;
    mutable std::shared_ptr<NameIndex const> names_;
};

// The mach_header_64 of the loaded image whose file is named `leafName`
// (e.g. "libOsiris.dylib"), or nullptr when no such image is loaded.
void const* FindLoadedImage(std::string_view leafName);

}
