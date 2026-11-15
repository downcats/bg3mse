#pragma once

#include <cstdint>
#include <span>

// Which registers a function reads before it writes them, read from the code
// at its entry. An argument register a function uses is read before it is
// written; a register the function overwrites first carries no argument.
// That is what can be decoded mechanically of a function's argument passing,
// and it is what tells a parameter that link-time optimisation dropped, or
// turned from a pointer into a value, from the one its mangled name promises.
//
// The scan follows the straight path from the entry: through conditional
// branches (the path that falls through is a path from the entry, so a read on
// it is a read of the incoming value), and stops at the first call, jump,
// return or instruction it does not know. A call to the stack probe a large
// frame starts with (___chkstk_darwin, reached through its pointer or its
// stub) is passed over: the code after it reads the arguments, so the probe
// preserves them.
//
// What the scan does not see it does not judge: a register that is neither
// read nor written before the scan stops is "not observed".

namespace bg3se::mac::engine {

enum class RegisterUse : uint8_t
{
    NotObserved,
    Address,            // first read as the base of a load or store: a pointer
    Wide,               // first read as a 64-bit value
    Narrow,             // first read as a 32-bit (or narrower) value
    Overwritten,        // written before any read, with no branch before the write
    OverwrittenOnPath,  // written before any read on the straight path, after a conditional branch
};

enum class ScanEnd : uint8_t
{
    Call,
    Jump,
    Return,
    Unrecognised,
    End,                // the function's last instruction, or the scan's limit
};

constexpr uint32_t kTrackedRegisters = 18;      // x0-x17

struct EntryUse
{
    RegisterUse Registers[kTrackedRegisters]{};
    uint32_t Scanned{ 0 };          // instructions decoded
    ScanEnd End{ ScanEnd::End };

    bool Read(uint32_t reg) const
    {
        auto use = Registers[reg];
        return use == RegisterUse::Address || use == RegisterUse::Wide || use == RegisterUse::Narrow;
    }
};

// What identifies the stack probe in the image the function is in: the
// address of the pointer slot through which code reaches it, and of its stub
// (either may be 0).
struct StackProbe
{
    uint64_t Slot{ 0 };
    uint64_t Stub{ 0 };
};

// Scans `function` (its instructions from the entry on), which is at link-time
// address `va`.
EntryUse ScanEntry(std::span<uint32_t const> function, uint64_t va, StackProbe const& probe);

// What the function's returns say about a result. A `ret` reached straight
// from an instruction that writes x0 (nothing but plain instructions between)
// returns a value on that path: a function without a result has no reason to
// set x0 there. A `ret` that follows a call directly may be handing on the
// callee's result, or not: that is counted apart.
struct ReturnUse
{
    uint32_t Returns{ 0 };          // ret instructions in the function
    uint32_t SetX0{ 0 };            // of those, reached straight from a write of x0
    uint32_t AfterCall{ 0 };        // of the others, reached straight from a call
};

ReturnUse ScanReturns(std::span<uint32_t const> function);

char const* Describe(RegisterUse use);
char const* Describe(ScanEnd end);

}
