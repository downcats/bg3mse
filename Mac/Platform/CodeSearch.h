#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <Platform/CodeShape.h>
#include <Platform/Symbols.h>

// Finding a place inside a game function without an address for it: search the
// function's instructions for the expected sequence, with the fields that
// change when code or data moves (branch displacements, adrp pages, the low
// bits an adrp pair adds, ...) left out of the comparison. The caller states
// how many matches it expects, normally one, so a sequence that is missing or
// has become ambiguous is refused instead of guessed at.

namespace bg3se::mac {

struct MaskedWord
{
    uint32_t Value;
    uint32_t Mask{ 0xffffffff };

    constexpr bool Matches(uint32_t word) const { return (word & Mask) == (Value & Mask); }
};

// Masks for the fields a sequence usually leaves open.
namespace Any {
    constexpr uint32_t Branch = 0xfc000000;         // b, bl: any target
    constexpr uint32_t CondBranch = 0xff00001f;     // b.cond, cbz, cbnz: any target
    constexpr uint32_t Adrp = 0x9f00001f;           // adrp: any page
    constexpr uint32_t Imm12 = 0xffc003ff;          // load/store (unsigned offset) or add: any offset
    constexpr uint32_t Imm7 = 0xffc07fff;           // ldp/stp: any offset
    constexpr uint32_t Imm16 = 0xffe0001f;          // movz/movk: any value in its lane
}

// Every address in [begin, end) where `pattern` starts and fits entirely.
std::vector<uint64_t> FindPattern(GameImage const& image, uint64_t begin, uint64_t end, std::span<MaskedWord const> pattern);

// Every `bl callee` in [begin, end).
std::vector<uint64_t> CallsTo(GameImage const& image, uint64_t begin, uint64_t end, uint64_t callee);

// Every slot in [vtable, vtableEnd) holding `method`. Slots are read from the
// image (PointerAt), so on a file whose pointers are chained fixups there is
// nothing to read and the result is empty: check PointersOnlyAtRunTime().
std::vector<uint64_t> VtableSlotsOf(GameImage const& image, uint64_t vtable, uint64_t vtableEnd, uint64_t method);

// Whether a `bl` at `siteVA` calls the imported function `stubName` (e.g.
// "_longjmp") through the image's stub for it.
bool CallsImport(GameImage const& image, uint64_t siteVA, std::string_view stubName);

}
