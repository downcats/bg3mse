#pragma once

#include <Platform/FaultRecord.h>

#include <cstddef>

// Filling a FaultRecord, for the fault-handling code of the platform layer
// (Platform/FaultGuard.cpp). Everything here runs on a thread that is free to
// block while the faulting thread is stopped: memory is read with checked
// reads (a bad pointer on the faulting stack fails the read instead of
// faulting), and nothing allocates or takes a lock.

namespace bg3se::mac {

// Copies `size` bytes at `address` of this process into `out`; false when any
// of them is not readable.
bool ReadMemory(uint64_t address, void* out, size_t size);

// What is known of a fault before its record is filled.
struct FaultSource
{
    uint32_t MachException;         // EXC_*, or 0 when seen as a signal
    int64_t Code[2];                // Mach exception codes
    int Signal;                     // when seen as a signal: its number,
    int SignalCode;                 //   si_code
    uint64_t SignalAddress;         //   and si_addr
    arm_thread_state64_t const* State;
    arm_exception_state64_t const* ExceptionState;      // nullptr when not known
    uint64_t ThreadId;
    mach_port_t Thread;
    uint64_t StackLow, StackHigh;   // the faulting thread's stack [low, high); 0, 0 when not known
    FaultOrigin Origin;
};

// The images loaded in this process and dyld itself, read from dyld's
// all-image-infos (the interface debuggers and crash reporters read) with
// checked reads.
class ImageTable
{
public:
    static constexpr size_t Capacity = 4096;

    struct Image
    {
        uint64_t Base;              // Mach-O header
        uint64_t End;               // end of its __TEXT segment
        char const* Path;
        bool MainExecutable;
    };

    // False when dyld's list could not be read (it was being changed on
    // every attempt); the table is then empty.
    bool Read();
    Image const* Find(uint64_t address) const;
    size_t Count() const { return count_; }
    // In the order dyld lists them, dyld itself last.
    Image const& At(size_t index) const { return images_[index]; }

private:
    Image images_[Capacity];
    size_t count_{ 0 };
};

// A thread's frames from its registers: pc, then the link register when it
// is not the first saved return address (the function had not saved it yet,
// or it is a return address inside the function itself), then the
// frame-pointer chain, which arm64 macOS code always keeps. Each frame record
// is read with a checked read, and the chain ends where a record cannot be
// read, where the next one is not higher on the stack and aligned, where it
// would leave the stack [stackLow, stackHigh) (when known; 0, 0 otherwise),
// or after `max` frames. The frames' images are not filled in. The number
// written.
uint32_t WalkFrames(arm_thread_state64_t const& state, uint64_t stackLow, uint64_t stackHigh, FaultFrame* out, uint32_t max);

// SymbolizeFrame for many frames at once, with the same results: each
// image's symbol table is read once for all its frames, instead of once for
// each frame (the game's has about 770k symbols). found[i] and symbols[i] are
// frames[i]'s. `lookup`, when not null, is the address to look each frame up
// at (a return address minus one, so that a call that does not return, the
// last instruction of its function, is named by that function and not by
// the next); the offset is from the frame's pc all the same. `order` and
// `best` are the caller's room, `count` entries each. When `deadline` (mach_absolute_time) is not 0 and has passed, the
// images not yet read are left unnamed. Allocates nothing and takes no lock.
// The number of frames named.
size_t SymbolizeFrames(FaultFrame const* frames, uint64_t const* lookup, uint32_t count, FrameSymbol* symbols, bool* found,
    uint32_t* order, void const** best, uint64_t deadline);

class FaultCapture
{
public:
    // Fills `out` from `source`: kind, signal, registers, backtrace and the
    // image of each frame.
    void Capture(FaultSource const& source, FaultRecord& out);

private:
    ImageTable images_;
};

}
