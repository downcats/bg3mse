#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Planning an entry hook: what the trampoline holds, or why there must be none.
//
// An entry hook replaces a function's first instruction by a `b` to an island
// near it. The trampoline, which runs the original function, holds that first
// instruction moved to the trampoline's address, followed by a `b` back to the
// function's second instruction. The trampoline is within branch range of the
// function, so moving an instruction is re-encoding it:
//
//   position-independent       copied
//   adrp                       re-encoded for its new address
//   adr                        re-encoded, or adrp + add when out of its range
//   b                          re-encoded
//   b.cond, cbz/cbnz, tbz/tbnz the same condition branching over the `b` back
//                              to a `b` to the taken destination
//   ldr/ldrsw literal into a   adrp into the destination register, then the
//   general register           load through it (with an add first when the
//                              literal is not aligned for the load's offset)
//
// Refused: a call (bl, blr) as the first instruction, since the callee would
// return into the trampoline: for the whole of the call the return address is
// in an island without unwind information, so an exception thrown through it
// ends the process and a stack walk stops there. Also refused: literal loads
// into FP/SIMD registers, prfm literal, brk and udf, a literal or adr result
// into the zero register, anything not positively
// recognised as one of the above or as position-independent (the encoding
// groups of the architecture that hold no PC-relative instruction), a direct
// branch other than bl, anywhere in the function, to the function's own first
// instruction (the entry is a loop head or tail-call target, and the
// replacement would be entered again from inside), a function that may read
// x16 or x17 before writing it (the island's far jump overwrites x16, and the
// replacement, an ordinary function, need not keep either: the procedure call
// standard lets no call pass a value in them, but hand-written stubs do), and
// a relocation that cannot reach from the trampoline.
//
// The planner is pure: it reads only the words it is given, so it runs on a
// game binary mapped from disk as well as on the live image.

namespace bg3se::mac {

enum class HookRefusal : uint8_t
{
    None,
    NoFunction,             // the function's extent is unknown or empty
    Misaligned,             // the function or the trampoline is not word-aligned
    Trap,                   // the first instruction is brk or udf
    Call,                   // the first instruction is a call (bl, blr)
    Unrecognised,           // not recognised as relocatable
    LiteralIntoFpSimd,      // ldr (literal) into an FP/SIMD register
    PrefetchLiteral,        // prfm (literal)
    ZeroRegister,           // a literal or address into the zero register
    BranchesToItsEntry,     // a branch inside the function targets its first instruction
    ReadsScratchRegisters,  // the function may read x16 or x17 before writing it
    OutOfRange,             // a relocated instruction cannot reach from the trampoline
    NotExecutable,          // the function is not in executable memory
    AlreadyHooked,          // the function already carries a bg3mse hook
    NoIsland,               // no island within branch range of the function
    Count
};

char const* Describe(HookRefusal refusal);

// The trampoline's longest form: adrp, add, ldr, b.
constexpr size_t kTrampolineMaxWords = 4;

struct TrampolinePlan
{
    HookRefusal Refusal{ HookRefusal::None };
    uint32_t Words[kTrampolineMaxWords]{};
    size_t Count{ 0 };

    bool Ok() const { return Refusal == HookRefusal::None; }
};

// The trampoline for the function whose words (from its first instruction to
// its end) are `function`, at `address`, when the trampoline is at `trampoline`.
TrampolinePlan PlanTrampoline(std::span<uint32_t const> function, uint64_t address, uint64_t trampoline);

// b / bl from `from` to `to`; 0 when out of range or misaligned.
uint32_t EncodeBranch(uint64_t from, uint64_t to, bool link);

// Where a direct branch (b, bl, b.cond, cbz/cbnz, tbz/tbnz) at `at` goes, or 0
// when `word` is not one.
uint64_t DirectBranchTarget(uint64_t at, uint32_t word);

}
