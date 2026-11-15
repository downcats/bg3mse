#pragma once

#include <cstdint>
#include <mach/arm/thread_status.h>
#include <mach/mach_types.h>

// What bg3mse records of a hardware fault: its kind, where it happened, the
// faulting thread's registers and a backtrace whose frames say which image
// they are in. Guarded calls (Platform/FaultGuard.h) hand a record to their
// filter and to their caller; fatal-fault observers get one too.
//
// A record is plain data with a fixed size: it is filled on bg3mse's
// exception thread, which allocates nothing and takes no lock while the
// faulting thread is stopped (that thread may hold the malloc, dyld or stdio
// lock). Everything in it stays valid as long as the images its frames name
// stay loaded.

namespace bg3se::mac {

enum class FaultKind : uint8_t
{
    // EXC_BAD_ACCESS (SIGSEGV or SIGBUS): an unmapped or protected address,
    // a misaligned access, or a jump to memory that is not executable.
    BadAccess,
    // A BadAccess in the guard region below the faulting thread's stack, with
    // the stack pointer at its bottom.
    StackOverflow,
    // EXC_BAD_INSTRUCTION (SIGILL): an undefined or privileged instruction.
    IllegalInstruction,
    // EXC_ARITHMETIC (SIGFPE). Integer division by zero does not trap on
    // arm64 and Apple CPUs do not trap floating-point exceptions, so this
    // kind is handled like the others but has not been seen.
    Arithmetic,
    // EXC_BREAKPOINT (SIGTRAP): a brk instruction, which is what
    // __builtin_trap() and failed hardening checks compile to.
    Breakpoint,
    // SIGABRT: abort(), which std::terminate ends in. Only fatal-fault
    // observers see this kind: it is not a hardware fault and never reaches a guard.
    Abort,
};

char const* FaultKindName(FaultKind kind);

enum class FaultOrigin : uint8_t
{
    Guarded,            // inside a guarded call; given to its filters
    Unguarded,          // on a thread outside any guarded call
    DeclinedByGuards,   // inside a guarded call whose filters all declined it
};

enum FaultFrameFlags : uint8_t
{
    // The frame is the faulting thread's link register, not a return address
    // found on the stack: the faulting function had not saved it yet (a leaf,
    // or a fault in a prologue), or it is a return address inside the
    // faulting function itself.
    FrameFromLinkRegister = 1,
    FrameInMainExecutable = 2,      // the image is the process's executable (the game)
    FrameInThisImage = 4,           // the image is the one this copy of the platform layer is linked into
};

struct FaultFrame
{
    uint64_t PC;                    // frame 0: the faulting pc; later frames: return addresses
    uint64_t ImageBase;             // Mach-O header of the image the address is in; 0 when in none
    char const* ImagePath;          // that image's path as dyld recorded it; nullptr when in none
    uint8_t Flags;                  // FaultFrameFlags
};

struct FaultRecord
{
    static constexpr uint32_t MaxFrames = 64;

    FaultKind Kind;
    FaultOrigin Origin;
    int Signal;                     // the signal the fault is, or becomes when nothing handles it first
    uint32_t MachException;         // EXC_*; 0 when the fault was seen only as a signal
    int64_t Code[2];                // the Mach exception codes, or the signal's si_code and 0
    uint64_t Address;               // the faulting data address for BadAccess / StackOverflow, otherwise PC
    uint64_t PC;
    uint64_t ThreadId;              // pthread_threadid_np of the faulting thread
    mach_port_t Thread;             // the faulting thread's port name in this task
    arm_thread_state64_t Registers; // x0-x28, fp, lr, sp, pc, cpsr at the fault
    bool HasExceptionState;
    arm_exception_state64_t ExceptionState;     // far, esr; only when the fault was seen as a signal
    uint64_t StackLow, StackHigh;   // the faulting thread's stack [low, high); 0, 0 when not known
    uint32_t FrameCount;
    FaultFrame Frames[MaxFrames];
};

// The function a frame's address is in, by the nearest preceding symbol of
// that image's own symbol table (the game ships with all of its symbols; the
// OS's libraries keep their exported ones, and dyld's shared cache reports
// the others as "<redacted>"). Name points into the image's string table.
struct FrameSymbol
{
    char const* Name;
    uint64_t Offset;
};

// False when the frame is in no image or its image has no symbol before the
// address. Reads only the loaded image: it allocates nothing and takes no
// lock, so a filter or observer may call it. It scans the whole table (the
// game's has about 770k symbols, a few milliseconds per frame).
bool SymbolizeFrame(FaultFrame const& frame, FrameSymbol& symbol);

// The Mach-O header of the image this copy of the platform layer is linked
// into (libbg3mse.dylib, libbg3mse-camera.dylib or a test executable).
uint64_t ThisImageBase();

}
