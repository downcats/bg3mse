#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <Platform/CodeShape.h>
#include <Platform/Symbols.h>

// What a proof of a Mac shape says of one instruction of a game function
// (Layout/GameShapes.h, Layout/OsirisShapes.h), and the check of it against
// the instruction the image has there.

namespace bg3se::mac::proof {

// What one instruction of a proven function must say. The instruction is
// named by its index in the function: the function's code is required to be
// the code that was read, so the index always names the same instruction.
enum class Operand : uint8_t
{
    Offset,         // load/store with an unsigned offset: the offset
    ByteOffset,     // ldrb/strb with an unsigned offset: the offset
    Unscaled,       // load/store with a signed 9-bit offset (unscaled, pre- or post-indexed): the offset
    Add,            // add/sub/cmp/cmn with an immediate: the immediate
    Move,           // movz or movn: the value written
    Mask,           // and/orr/eor/tst with a bitmask: the mask
    ShiftLeft,      // add with a shifted register: the shift
    ShiftLeftBy,    // lsl with an immediate, or bfi from bit 0: the shift
    ShiftRight,     // lsr with an immediate: the shift
    PairOffset,     // ldp/stp: the offset
    StoresZero,     // a store of the zero register (the value is not used)
    MoveWide,       // movz and the movk after it into the same register: the value the two form
    TestsBit,       // tbz or tbnz: the bit tested
    AtomicAdd32,    // ldadd (any ordering) of a 32-bit word: the value is not used
    CallsImport,    // bl to the import stub named `Name`
    JumpsToImport,  // b to the import stub named `Name` (a tail call)
    Calls,          // bl to the game function whose symbol is `Name`
    JumpsTo,        // b to the game function whose symbol is `Name`
};

struct Expect
{
    uint16_t Index;
    Operand Kind;
    uint64_t Value{ 0 };
    char const* Name{ nullptr };
};

struct Code
{
    GameImage const& Image;
    uint64_t Begin, End;

    uint32_t Count() const { return (uint32_t)((End - Begin) / 4); }
    uint64_t VA(uint32_t index) const { return Begin + 4ull * index; }
    uint32_t At(uint32_t index) const { return Image.Word(VA(index)); }
};

std::optional<int64_t> UnscaledOffset(uint32_t instruction);
std::optional<uint64_t> MoveValue(uint32_t instruction);
std::optional<uint64_t> ShiftLeftOf(uint32_t instruction);
// The page an adrp at `va` forms.
uint64_t PageOf(uint64_t va, uint32_t adrp);
// The address an adrp and the load after it read: adrp x, page; ldr ?, [x, #offset].
uint64_t LoadedAddress(Code const& f, uint32_t adrpIndex);
std::string Hex(uint64_t value);

// Whether the instruction `e` names says what `e` expects; otherwise the
// difference, in `problem`.
bool CheckOperand(Code const& f, Expect const& e, std::string& problem);

}
