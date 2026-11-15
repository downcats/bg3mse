#pragma once

#include <Platform/FaultCapture.h>
#include <Platform/FaultRecord.h>

#include <cstddef>
#include <cstdint>

// The process's other threads at the time of a crash, for the report: what
// Norbyte's minidump has of every thread beyond the crashing one (on Windows
// MiniDumpWriteDump writes them all). Each thread's name, its dispatch queue's
// label, its registers and its frames.
//
// How. The threads are listed with task_threads, and each one in turn, apart
// from the reporting thread, the crashing thread and bg3mse's fault-handling
// threads (below), is suspended (thread_suspend), its registers read
// (thread_get_state), its frame-pointer chain walked with checked reads
// (Platform/FaultCapture.h, WalkFrames) and the pointer to its dispatch queue
// read, and then it is resumed (thread_resume), before the next is
// suspended. Nothing in between allocates, takes a lock, or can fault: Mach
// calls into memory of the reporter's own and reads that fail instead of
// faulting. So a thread suspended while holding the allocator's or any other
// lock holds nothing the reporter waits for, and every thread suspended is
// resumed, there being no way out between the two calls. A thread that ends
// while it is being looked at fails a call and is listed without what that
// call would have given.
//
// The thread's name and id come from the kernel (thread_info), not from
// pthread_getname_np or pthread_from_mach_thread_np, which take the pthread
// list's lock. Its queue's label is asked of libdispatch
// (dispatch_queue_get_label) after the thread is resumed, in a guarded call,
// since the queue may be gone by then.
//
// Not suspended: the reporting thread (it is the one doing this), the
// crashing thread (stopped already; its registers and frames are the
// report's own, MREG and MBTR), and the exception threads and watchdog of
// each copy of bg3mse's platform layer, which serve the faults of the
// reporter's guarded calls (Platform/FaultGuard.h): this copy's by their
// ports, another copy's by their names. They are listed with their role.
//
// Bounds: kMaxThreads threads (the others are counted), kMaxFrames frames a
// thread. The frames are named by symbol (SymbolizeFrames) after every
// thread is resumed, within a time limit: the faulting thread waits for the
// reporter for at most 30 seconds in all.

namespace bg3se::mac::crash {

enum class ThreadRole : uint8_t
{
    Other,
    Crashed,            // the crashing thread
    Reporter,           // the thread writing the report
    FaultHandling,      // an exception thread or watchdog of bg3mse's
};

char const* ThreadRoleName(ThreadRole role);

struct ThreadEntry
{
    uint64_t Id;                    // as FaultRecord::ThreadId; 0 when not known
    mach_port_t Port;
    ThreadRole Role;
    bool Captured;                  // the thread was suspended, and its registers and frames read
    bool HasName;
    bool HasQueue;
    int RunState;                   // TH_STATE_*, -1 when not known
    int SuspendCount;               // before the reporter's; -1 when not known
    char Name[64];
    uint64_t QueueAddress;          // the dispatch queue; 0 when none
    char Queue[128];
    arm_thread_state64_t Registers;
    uint32_t FirstFrame;            // in ThreadList::Frames
    uint32_t FrameCount;
};

class ThreadList
{
public:
    static constexpr uint32_t kMaxThreads = 256;
    static constexpr uint32_t kMaxFrames = 32;
    static constexpr uint32_t kFrameCapacity = kMaxThreads * kMaxFrames;

    // Lists the process's threads, as above; `crashed` is the crashing
    // thread's port. `images` is read and used to say which image each frame
    // is in. Symbols are looked for until mach_absolute_time() passes
    // `deadline` (0: no limit). False when the threads cannot be listed.
    bool Take(mach_port_t crashed, ImageTable& images, uint64_t deadline);

    uint32_t Total() const { return total_; }
    uint32_t Count() const { return count_; }
    ThreadEntry const& At(uint32_t index) const { return threads_[index]; }
    FaultFrame const& Frame(ThreadEntry const& thread, uint32_t index) const { return frames_[thread.FirstFrame + index]; }
    // The frame's symbol, when one was found for it.
    bool Symbol(ThreadEntry const& thread, uint32_t index, FrameSymbol& out) const;

private:
    void Look(mach_port_t port, mach_port_t crashed, mach_port_t self);

    uint32_t total_{ 0 };
    uint32_t count_{ 0 };
    uint32_t frameCount_{ 0 };
    ThreadEntry threads_[kMaxThreads];
    FaultFrame frames_[kFrameCapacity];
    uint64_t lookup_[kFrameCapacity];
    FrameSymbol symbols_[kFrameCapacity];
    bool named_[kFrameCapacity];
    uint32_t order_[kFrameCapacity];
    void const* best_[kFrameCapacity];
};

}
