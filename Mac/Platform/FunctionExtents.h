#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Where a function starts and ends, in any loaded Mach-O image (the game, a
// dylib such as libOsiris.dylib, this process's own executable) or in a binary
// mapped from disk.
//
// The source is the image's LC_FUNCTION_STARTS table when it has one, and its
// symbol table only when it does not:
//
//   function starts   written by the static linker: function starts, named
//                     or not (local and stripped functions included). A
//                     function ends where the next listed one starts, or at
//                     the end of its section, so its extent holds its code and
//                     whatever alignment padding or data follows it. Every
//                     address listed starts a function, so an extent never
//                     ends inside its function; but a table may leave
//                     functions out, and those count in the extent of the
//                     function before them. The game's table leaves out all of
//                     its static initialisers (___cxx_global_var_init*,
//                     __GLOBAL__sub_I_*, ___cxx_global_array_dtor): an extent
//                     can be longer than its function, never shorter.
//   symbol table      every defined symbol in a code section. A function with
//                     no symbol (stripped, or local in an image whose local
//                     symbols were removed) is not a start, and is counted in
//                     the extent of the named function before it, so that
//                     extent can run over it. A symbol on a label inside a
//                     function is taken as a start and cuts that function's
//                     extent short. Neither table says which a symbol is.
//
// Code sections are those with instructions (S_ATTR_PURE_INSTRUCTIONS or
// S_ATTR_SOME_INSTRUCTIONS). Refused, with nothing returned but the reason:
// an address no loaded image holds, an image with neither table (or one that
// cannot be read), an address outside the image's code sections, an address in
// a section of dyld stubs, and an address inside a function rather than at its
// start.
//
// FindLoadedFunction caches each image's decoded table (the game's has about
// 200k entries) for the life of the process. The cache is constant-initialised,
// so it may be used from a dylib constructor before static initialisers have
// run. An image unloaded and another loaded at its address is noticed: a cached
// table is used only while the image's load commands are the ones it was built
// from.

namespace bg3se::mac {

enum class FunctionSource : uint8_t
{
    None,
    FunctionStarts,     // LC_FUNCTION_STARTS
    SymbolTable,        // LC_SYMTAB, for an image without the former
};

enum class ExtentRefusal : uint8_t
{
    None,
    NotInImage,         // no loaded image holds the address
    NoTable,            // the image has no table of functions that can be read
    NotCode,            // not in a code section of the image
    Stub,               // in a section of dyld stubs: a jump to another image's function
    NotAStart,          // inside a function, not at its start
};

char const* Describe(ExtentRefusal refusal);
char const* Describe(FunctionSource source);

// A function's extent [Start, End): addresses in the process for a loaded
// image, link-time addresses for a file.
struct FunctionExtent
{
    uint64_t Start{ 0 };
    uint64_t End{ 0 };
    FunctionSource Source{ FunctionSource::None };
    ExtentRefusal Refusal{ ExtentRefusal::NoTable };

    bool Ok() const { return Refusal == ExtentRefusal::None; }
    uint64_t Bytes() const { return End - Start; }
};

// The table of functions of one image.
class FunctionTable
{
public:
    // An image dyld has loaded, by the mach_header_64 it mapped. Addresses
    // given and returned are this process's.
    bool OpenLoaded(void const* header, std::string& problem);

    // A Mach-O file mapped in memory (of a fat file, its arm64 slice). `file`
    // is read only while opening. Addresses given and returned are link-time.
    bool OpenFile(std::span<uint8_t const> file, std::string& problem);

    FunctionExtent At(uint64_t address) const;

    FunctionSource Source() const { return source_; }
    size_t Count() const { return starts_.size(); }
    // The image's load commands as they were when the table was built.
    std::span<uint8_t const> LoadCommands() const { return commands_; }

private:
    struct Section
    {
        uint32_t Begin, End;    // offsets from the image's __TEXT
        bool Stubs;
    };

    // `linkedit` maps a __LINKEDIT file offset to memory; `limit` bounds the
    // offsets of a file (0 for a loaded image).
    bool Build(void const* header, uint8_t const* linkedit, uint64_t limit, intptr_t slide, std::string& problem);

    uint64_t base_{ 0 };                 // the address of __TEXT's start
    FunctionSource source_{ FunctionSource::None };
    std::vector<uint32_t> starts_;       // offsets from base_, ascending
    std::vector<Section> sections_;      // ascending
    std::vector<uint8_t> commands_;
};

// The function that starts at `address`, in whichever loaded image holds it.
FunctionExtent FindLoadedFunction(void const* address);

}
