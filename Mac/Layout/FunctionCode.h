#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <Platform/Symbols.h>

// One game function's instructions, for the layout checks that read a
// constant or an offset out of the game's code: which constant a register
// holds at an instruction, decided without guessing. Constants are followed
// forward along every path of the function; a register has a value at an
// instruction only when every path that reaches the instruction leaves the
// same constant in it. A function with an indirect jump, whose paths cannot
// be listed, gives no values at all.

namespace bg3se::mac {

class FunctionCode
{
public:
    // The instructions in [begin, end), link-time addresses in __TEXT,__text.
    FunctionCode(GameImage const& image, uint64_t begin, uint64_t end);

    uint64_t Begin() const { return begin_; }
    uint64_t End() const { return end_; }
    bool Contains(uint64_t va) const { return va >= begin_ && va < end_ && !(va & 3); }
    uint32_t At(uint64_t va) const { return words_[(va - begin_) / 4]; }

    // The constant general register `reg` (0..30) holds when the instruction
    // at `site` runs: set by movz/movn (with any movk on top) or a mov of a
    // bitmask on every path to `site`, and written by nothing else since.
    // For a w register the upper half reads as zero.
    std::optional<uint64_t> ConstantAt(uint64_t site, uint32_t reg) const;

    // Every `bl` to `callee`.
    std::vector<uint64_t> CallsTo(uint64_t callee) const;

private:
    struct Registers
    {
        uint32_t Known{ 0 };            // bit per register
        uint64_t Value[31]{};

        bool operator == (Registers const&) const = default;
    };

    void Propagate();

    uint64_t begin_, end_;
    std::vector<uint32_t> words_;
    // The registers on entry to each instruction; empty when the function has
    // an indirect jump.
    std::vector<Registers> entry_;
};

// Whether `instruction` may write general register `reg` (0..30). Errs
// towards "may": an instruction of a class not listed in the implementation
// counts as writing the register its low five bits name.
bool MayWriteRegister(uint32_t instruction, uint32_t reg);

}
