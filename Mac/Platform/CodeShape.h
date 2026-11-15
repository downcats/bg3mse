#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

// A hook that depends on what a game function does (which fields it writes, at
// which offsets) checks the function's shape before it trusts it. The shape is
// a hash of the function's instructions with every position-dependent field
// masked out, so it is the same wherever a patch moves the function or the
// data it refers to, and differs as soon as the code itself changes. Nothing
// here compares a game version.

namespace bg3se::mac {

struct CodeShape
{
    uint32_t Instructions{ 0 };
    uint64_t Hash{ 0 };

    bool operator == (CodeShape const&) const = default;
};

// The shape of the function at `code`, from its first instruction through its
// first `ret`. Empty when no `ret` is found within `maxInstructions`.
std::optional<CodeShape> ShapeToFirstReturn(void const* code, uint32_t maxInstructions = 512);

// The shape of `instructions` instructions at `code`: a whole function, when
// its end is known from the symbol table.
CodeShape ShapeOfRange(void const* code, uint32_t instructions);

// The address a `bl` at `siteVA` calls, or 0 when `instruction` is not a bl.
uint64_t CallTarget(uint64_t siteVA, uint32_t instruction);

// The address an adrp at `siteVA` and the load or add after it form
// (adrp x, page; ldr/ldrb/add ..., [x, #offset]), or 0 when they are not that pair.
uint64_t PageAddress(uint64_t siteVA, uint32_t adrp, uint32_t next);

// The address a `b` (not bl) at `siteVA` jumps to, or 0 when `instruction` is not a b.
uint64_t JumpTarget(uint64_t siteVA, uint32_t instruction);

// Fields read back from single instructions, for the offsets and constants a
// hook takes from the game's own code rather than from a table.

// The byte offset of a load or store with an unsigned immediate offset
// (ldr/str/ldrb/strh/..., general, FP and SIMD registers, q included): the
// immediate scaled by the access size.
std::optional<uint32_t> LoadStoreOffset(uint32_t instruction);

// The byte offset of ldp/stp of general registers (signed offset, pre- or post-index).
std::optional<int32_t> PairOffset(uint32_t instruction);

// The immediate of add/sub (immediate), shift applied.
std::optional<uint32_t> AddImmediate(uint32_t instruction);

// The value movz writes (w or x register).
std::optional<uint64_t> MovzValue(uint32_t instruction);

// The value a movz followed by a movk of the same register writes.
std::optional<uint64_t> MovzMovkValue(uint32_t movz, uint32_t movk);

// The bit pattern a logical immediate (and/orr/eor/ands, w or x) encodes in its
// N:immr:imms field, e.g. 0x3fffff for `and w10, w20, #0x3fffff`.
std::optional<uint64_t> LogicalImmediate(uint32_t instruction);

// A bitfield move (sbfm/bfm/ubfm, w or x, and their aliases). Insert (bfi,
// ubfiz, sbfiz, lsl): the low `Width` bits of the source go to bit `Lsb`.
// Extract (bfxil, ubfx, sbfx, lsr, asr): `Width` bits from source bit `Lsb` go
// to bit 0.
struct BitfieldMove
{
    uint32_t Lsb;
    uint32_t Width;
    bool Insert;

    bool operator == (BitfieldMove const&) const = default;
};
std::optional<BitfieldMove> BitfieldMoveOf(uint32_t instruction);

// Register fields: Rd/Rt (bits 0-4), Rn (5-9), Rm (16-20).
constexpr uint32_t RegD(uint32_t instruction) { return instruction & 0x1f; }
constexpr uint32_t RegN(uint32_t instruction) { return (instruction >> 5) & 0x1f; }
constexpr uint32_t RegM(uint32_t instruction) { return (instruction >> 16) & 0x1f; }
constexpr uint32_t WithRegD(uint32_t instruction, uint32_t reg) { return (instruction & ~0x1fu) | (reg & 0x1f); }
constexpr uint32_t WithRegN(uint32_t instruction, uint32_t reg) { return (instruction & ~(0x1fu << 5)) | ((reg & 0x1f) << 5); }
constexpr uint32_t WithRegM(uint32_t instruction, uint32_t reg) { return (instruction & ~(0x1fu << 16)) | ((reg & 0x1f) << 16); }

}
