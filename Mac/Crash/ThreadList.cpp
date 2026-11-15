#include <Crash/ThreadList.h>

#include <Platform/FaultGuard.h>

#include <cstring>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <pthread.h>

namespace bg3se::mac::crash {

namespace {

// What pthread_setname_np names the fault-handling threads of every copy of
// the platform layer (Platform/FaultGuard.cpp).
constexpr char kFaultThreadPrefix[] = "bg3mse fault ";

void CopyName(char* out, size_t size, char const* text, size_t length)
{
    size_t i = 0;
    for (; i < length && text[i] && i + 1 < size; i++) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        out[i] = c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c);
    }
    out[i] = '\0';
}

}

char const* ThreadRoleName(ThreadRole role)
{
    switch (role) {
    case ThreadRole::Other: return "other";
    case ThreadRole::Crashed: return "crashed";
    case ThreadRole::Reporter: return "reporter";
    case ThreadRole::FaultHandling: return "fault-handling";
    }
    return "?";
}

void ThreadList::Look(mach_port_t port, mach_port_t crashed, mach_port_t self)
{
    ThreadEntry& thread = threads_[count_++];
    memset(&thread, 0, sizeof(thread));
    thread.Port = port;
    thread.RunState = -1;
    thread.SuspendCount = -1;
    thread.FirstFrame = frameCount_;

    // What the kernel says of the thread, asked without stopping it.
    thread_identifier_info_data_t identifier{};
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    if (thread_info(port, THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&identifier), &count) == KERN_SUCCESS) {
        thread.Id = identifier.thread_id;
    }
    thread_extended_info_data_t extended{};
    count = THREAD_EXTENDED_INFO_COUNT;
    if (thread_info(port, THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&extended), &count) == KERN_SUCCESS) {
        CopyName(thread.Name, sizeof(thread.Name), extended.pth_name, sizeof(extended.pth_name));
        thread.HasName = thread.Name[0] != '\0';
    }
    thread_basic_info_data_t basic{};
    count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&basic), &count) == KERN_SUCCESS) {
        thread.RunState = basic.run_state;
        thread.SuspendCount = basic.suspend_count;
    }

    // A thread that reports on itself (std::terminate) is the crashed one.
    if (port == crashed) {
        thread.Role = ThreadRole::Crashed;
    } else if (port == self) {
        thread.Role = ThreadRole::Reporter;
    } else if (IsFaultHandlingThread(port) || strncmp(thread.Name, kFaultThreadPrefix, sizeof(kFaultThreadPrefix) - 1) == 0) {
        thread.Role = ThreadRole::FaultHandling;
    } else {
        thread.Role = ThreadRole::Other;
    }
    if (thread.Role != ThreadRole::Other) return;

    // From here to thread_resume: Mach calls and checked reads only.
    if (thread_suspend(port) != KERN_SUCCESS) return;
    arm_thread_state64_t state{};
    count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(port, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count) == KERN_SUCCESS) {
        thread.Registers = state;
        thread.Captured = true;
        uint32_t room = kFrameCapacity - frameCount_;
        thread.FrameCount = WalkFrames(state, 0, 0, frames_ + frameCount_, room < kMaxFrames ? room : kMaxFrames);
        frameCount_ += thread.FrameCount;
    }
    // dispatch_qaddr is where the thread keeps the queue it runs for.
    uint64_t queue = 0;
    if (identifier.dispatch_qaddr && ReadMemory(identifier.dispatch_qaddr, &queue, sizeof(queue))) thread.QueueAddress = queue;
    thread_resume(port);
}

bool ThreadList::Take(mach_port_t crashed, ImageTable& images, uint64_t deadline)
{
    total_ = 0;
    count_ = 0;
    frameCount_ = 0;

    // The list and the send rights in it are the kernel's to this task:
    // vm_allocate'd memory and port names, not the allocator's.
    thread_act_array_t list = nullptr;
    mach_msg_type_number_t listed = 0;
    if (task_threads(mach_task_self(), &list, &listed) != KERN_SUCCESS) return false;
    total_ = listed;
    // pthread_mach_thread_np of the calling thread reads its own data and locks nothing.
    mach_port_t self = pthread_mach_thread_np(pthread_self());
    for (mach_msg_type_number_t i = 0; i < listed; i++) {
        if (count_ < kMaxThreads) Look(list[i], crashed, self);
        mach_port_deallocate(mach_task_self(), list[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), listed * sizeof(list[0]));

    // Every thread runs again. What follows reads what was taken, and what
    // the threads may have changed since only in guarded calls.
    images.Read();
    uint64_t me = ThisImageBase();
    for (uint32_t i = 0; i < frameCount_; i++) {
        auto& frame = frames_[i];
        if (auto image = images.Find(frame.PC)) {
            frame.ImageBase = image->Base;
            frame.ImagePath = image->Path;
            if (image->MainExecutable) frame.Flags |= FrameInMainExecutable;
            if (image->Base == me) frame.Flags |= FrameInThisImage;
        }
    }
    for (uint32_t i = 0; i < count_; i++) {
        auto& thread = threads_[i];
        if (!thread.QueueAddress) continue;
        // A queue's label is set when it is made and lives as long as the
        // queue; the queue may have been released since the read above.
        GuardedCall([&] {
            char const* label = dispatch_queue_get_label(reinterpret_cast<dispatch_queue_t>(thread.QueueAddress));
            if (label && label[0]) {
                CopyName(thread.Queue, sizeof(thread.Queue), label, sizeof(thread.Queue));
                thread.HasQueue = true;
            }
        });
    }
    // The images' symbol tables are read in place, as SymbolizeFrame reads them.
    // Every frame after a thread's first is a return address: the call is
    // the instruction before it.
    for (uint32_t i = 0; i < count_; i++) {
        auto const& thread = threads_[i];
        for (uint32_t k = 0; k < thread.FrameCount; k++) {
            uint64_t pc = frames_[thread.FirstFrame + k].PC;
            lookup_[thread.FirstFrame + k] = k > 0 && pc ? pc - 1 : pc;
        }
    }
    memset(named_, 0, sizeof(named_[0]) * frameCount_);
    GuardedCall([&] { SymbolizeFrames(frames_, lookup_, frameCount_, symbols_, named_, order_, best_, deadline); });
    return true;
}

bool ThreadList::Symbol(ThreadEntry const& thread, uint32_t index, FrameSymbol& out) const
{
    uint32_t at = thread.FirstFrame + index;
    if (index >= thread.FrameCount || !named_[at]) return false;
    out = symbols_[at];
    return true;
}

}
