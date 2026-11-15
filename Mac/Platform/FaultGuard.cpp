#include <Platform/FaultCapture.h>
#include <Platform/FaultGuard.h>
#include <Platform/FaultServer.h>
#include <Platform/Log.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mig_errors.h>
#include <pthread.h>
#include <sched.h>
#include <sys/sysctl.h>
#include <unistd.h>

// MIG stubs: the server side of mach_exc.defs and the client side of
// mach_exc.defs and exc.defs (generated at build time from the SDK's
// definitions; libSystem exports only the 32-bit-code client).
extern "C" {
#include <mach/exc.h>
#include <mach_exc.h>
#include <mach_excServer.h>
}

extern "C" {
int bg3mse_guard_run(uint64_t* saved, void (*fn)(void*), void* arg);
void bg3mse_guard_resume(uint64_t* saved);
}

namespace bg3se::mac {

namespace {

// The exceptions a guard takes: the hardware faults. EXC_SOFTWARE, EXC_CRASH,
// EXC_RESOURCE, EXC_GUARD and the rest go on as they would without bg3mse.
constexpr exception_mask_t kGuardMask = EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC | EXC_MASK_BREAKPOINT;

// The exception thread is sent the faulting thread's registers and replies
// with the ones it resumes with; it needs no thread or task port.
constexpr exception_behavior_t kGuardBehavior = EXCEPTION_STATE | MACH_EXCEPTION_CODES;

// Word indices into GuardFrame::Saved (Platform/FaultGuardRun.S).
constexpr size_t kSavedSP = 12;
constexpr size_t kSavedWords = 21;

struct GuardFrame
{
    uint64_t Saved[kSavedWords];
    FaultFilter Filter;
    void* Context;
    FaultRecord* Out;
    GuardFrame* Outer;
    uint32_t Generation;
};

// One per thread that has entered a guard, for the thread's life.
struct ThreadGuards
{
    uint32_t Generation;
    mach_port_t Port;               // receive right and one send right; its context is this
    mach_port_t Thread;
    uint64_t ThreadId;
    uint64_t StackLow, StackHigh;
    std::atomic<GuardFrame*> Top;   // the innermost guard
    // The fault the guards last declined, so that the same fault coming back
    // (the instruction re-executed after signal handlers returned) goes on
    // without asking them again. Cleared on entering and leaving a guard.
    std::atomic<bool> Declined;
    std::atomic<uint64_t> DeclinedPC;
    std::atomic<uint64_t> DeclinedAddress;
    // The thread-level handlers the outermost guard replaced.
    mach_msg_type_number_t SavedCount;
    exception_mask_t SavedMasks[EXC_TYPES_COUNT];
    mach_port_t SavedPorts[EXC_TYPES_COUNT];
    exception_behavior_t SavedBehaviors[EXC_TYPES_COUNT];
    thread_state_flavor_t SavedFlavors[EXC_TYPES_COUNT];
};

// Constant-initialised: a dylib constructor may run before this file's
// dynamic initialisers. The lock is a plain pthread mutex, which has no
// destructor to run at exit while the exception threads still use it.
constinit pthread_mutex_t gStartMutex = PTHREAD_MUTEX_INITIALIZER;
constinit std::atomic<bool> gStarted{ false };
constinit std::atomic<uint32_t> gGeneration{ 1 };     // bumped in a forked child
constinit bool gStartFailedLogged = false;
constinit bool gForkHandlersInstalled = false;
constinit bool gKeyCreated = false;
constinit pthread_key_t gKey = 0;
constinit mach_port_t gPortSet = MACH_PORT_NULL;

constexpr int kServerCount = 2;
constexpr size_t kServerStack = 1024 * 1024;
constinit std::atomic<pthread_t> gServerThreads[kServerCount]{};
// The Mach ports of the exception threads and the watchdog, each noted by the
// thread itself, for IsFaultHandlingThread: asking pthread_mach_thread_np of
// another thread takes the pthread list's lock.
constinit std::atomic<mach_port_t> gFaultThreadPorts[kServerCount + 1]{};

void NoteFaultThreadPort()
{
    mach_port_t self = pthread_mach_thread_np(pthread_self());
    for (auto& port : gFaultThreadPorts) {
        mach_port_t expected = MACH_PORT_NULL;
        if (port.compare_exchange_strong(expected, self)) return;
    }
}

// --- The time limit ---------------------------------------------------------------
//
// The kernel keeps a faulting thread stopped until the exception thread
// answers, however long that takes. A filter that never returns (it waits for
// something the stopped thread holds, or writes to something nobody reads)
// would leave the thread stopped for good: a game that hangs where it should
// have crashed. So an exception thread says what it is serving and since
// when, and a third thread, the watchdog, answers the kernel in its place
// once the limit has passed: the fault goes on as if every filter had
// declined it. If the filter returns after all, its answer is dropped.

constinit std::atomic<uint32_t> gTimeLimitMs{ 30 * 1000 };
constinit mach_timebase_info_data_t gTimebase{};
constexpr mach_msg_id_t kWatchdogMessageId = 0x62676d02;
constexpr size_t kWatchdogStack = 256 * 1024;
constinit std::atomic<mach_port_t> gWatchPort{ MACH_PORT_NULL };

mach_timebase_info_data_t const& Timebase()
{
    // A system call that asks the kernel for two constants: no lock, and the
    // same answer from every thread that happens to ask first.
    if (gTimebase.denom == 0) mach_timebase_info(&gTimebase);
    return gTimebase;
}

// What an exception thread is serving. State is a sequence number, which
// tells one exception from the next, and under it the phase of this one.
// The exception thread goes between deciding and waiting; the watchdog takes
// an exception over only from waiting, and only the one whose sequence it
// read with the deadline.
constexpr uint64_t kPhaseBits = 3;
enum : uint64_t
{
    kIdle = 0,
    // Reading and changing the faulting thread's guards. Short, and never
    // taken over: once the fault is given up the thread goes on, and may
    // leave the frames this is reading.
    kDeciding = 1,
    // Running what may not come back: a filter, or the handler the fault is
    // forwarded to.
    kWaiting = 2,
    // The watchdog is answering for it, and has answered.
    kGivingUp = 3,
    kGivenUp = 4,
    kPhaseMask = (1 << kPhaseBits) - 1,
};

struct Serving
{
    std::atomic<uint64_t> State;
    std::atomic<uint64_t> Deadline;     // mach_absolute_time
    std::atomic<pthread_t> Thread;      // the exception thread, once it runs
    // Written by the exception thread before it enters a phase of a new
    // exception; read by the watchdog while it is giving that exception up.
    mach_port_t ReplyPort;
    mach_msg_bits_t ReplyBits;
    mach_msg_id_t ReplyId;
    ThreadGuards* Guards;
    uint64_t PC, Address;
};
constinit Serving gServing[kServerCount]{};

// Per exception thread: room to capture a record without allocating, and
// what the watchdog needs of the request being served.
struct ServerContext
{
    FaultCapture Capture;
    FaultRecord Record;
    Serving* Slot;
    mach_port_t ReplyPort;
    mach_msg_bits_t ReplyBits;
    mach_msg_id_t ReplyId;
};
thread_local ServerContext* tServer = nullptr;

void KickWatchdog()
{
    mach_port_t port = gWatchPort.load(std::memory_order_acquire);
    if (!MACH_PORT_VALID(port)) return;
    mach_msg_header_t kick{};
    kick.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    kick.msgh_remote_port = port;
    kick.msgh_size = sizeof(kick);
    kick.msgh_id = kWatchdogMessageId;
    // A full queue is full of kicks the watchdog has yet to take: it looks
    // at every exception thread again after each of them.
    if (mach_msg(&kick, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(kick), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL) != MACH_MSG_SUCCESS) {
        mach_msg_destroy(&kick);
    }
}

// The exception thread takes up a new exception, in the deciding phase.
void BeginServing(Serving& serving, ThreadGuards* tg, uint64_t pc, uint64_t address)
{
    // The watchdog may still be answering the exception before this one,
    // which this thread left to it: a moment.
    uint64_t state = serving.State.load(std::memory_order_acquire);
    while ((state & kPhaseMask) == kGivingUp) {
        sched_yield();
        state = serving.State.load(std::memory_order_acquire);
    }
    serving.ReplyPort = tServer->ReplyPort;
    serving.ReplyBits = tServer->ReplyBits;
    serving.ReplyId = tServer->ReplyId;
    serving.Guards = tg;
    serving.PC = pc;
    serving.Address = address;
    serving.Deadline.store(FaultDeadlineFromNow(), std::memory_order_relaxed);
    serving.State.store(((state >> kPhaseBits) + 1) << kPhaseBits | kDeciding, std::memory_order_release);
    KickWatchdog();
}

// From deciding to waiting, before something that may not come back.
void WaitServing(Serving& serving)
{
    uint64_t state = serving.State.load(std::memory_order_relaxed);
    serving.State.store((state & ~kPhaseMask) | kWaiting, std::memory_order_release);
}

// Back to deciding. False when the watchdog has given the exception up
// meanwhile: it is answered, and nothing of it is this thread's any more.
bool DecideServing(Serving& serving)
{
    uint64_t state = serving.State.load(std::memory_order_relaxed);
    if ((state & kPhaseMask) != kWaiting) return false;
    return serving.State.compare_exchange_strong(state, (state & ~kPhaseMask) | kDeciding, std::memory_order_acq_rel);
}

void EndServing(Serving& serving)
{
    uint64_t state = serving.State.load(std::memory_order_relaxed);
    serving.State.store((state & ~kPhaseMask) | kIdle, std::memory_order_release);
}

void ReleasePorts(ThreadGuards* tg)
{
    for (mach_msg_type_number_t i = 0; i < tg->SavedCount; i++) {
        if (MACH_PORT_VALID(tg->SavedPorts[i])) mach_port_deallocate(mach_task_self(), tg->SavedPorts[i]);
    }
    tg->SavedCount = 0;
}

bool InstallPort(ThreadGuards* tg)
{
    tg->SavedCount = EXC_TYPES_COUNT;
    kern_return_t kr = thread_swap_exception_ports(tg->Thread, kGuardMask, tg->Port, kGuardBehavior, ARM_THREAD_STATE64,
        tg->SavedMasks, &tg->SavedCount, tg->SavedPorts, tg->SavedBehaviors, tg->SavedFlavors);
    if (kr != KERN_SUCCESS) {
        tg->SavedCount = 0;
        return false;
    }
    return true;
}

void RestorePort(ThreadGuards* tg)
{
    for (mach_msg_type_number_t i = 0; i < tg->SavedCount; i++) {
        thread_set_exception_ports(tg->Thread, tg->SavedMasks[i], tg->SavedPorts[i], tg->SavedBehaviors[i], tg->SavedFlavors[i]);
    }
    ReleasePorts(tg);
}

void Pop(ThreadGuards* tg, GuardFrame* frame)
{
    // A guard entered before a fork() and left in the child is forgotten
    // there: the ports it saved are names of the parent's, which in the child
    // name nothing, or something else.
    if (frame->Generation != gGeneration.load(std::memory_order_relaxed)) return;
    tg->Top.store(frame->Outer, std::memory_order_relaxed);
    tg->Declined.store(false, std::memory_order_relaxed);
    if (!frame->Outer) RestorePort(tg);
}

void DestroyThreadGuards(void* value)
{
    auto tg = static_cast<ThreadGuards*>(value);
    if (tg->Generation == gGeneration.load(std::memory_order_relaxed)) {
        if (tg->Top.load(std::memory_order_relaxed)) RestorePort(tg);
        mach_port_destruct(mach_task_self(), tg->Port, -1, 0);
    }
    free(tg);
}

bool IsTraced()
{
    kinfo_proc info{};
    size_t size = sizeof(info);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    return sysctl(mib, 4, &info, &size, nullptr, 0) == 0 && (info.kp_proc.p_flag & P_TRACED) != 0;
}

// The exception threads. Each receives from the port set that holds every
// guarded thread's port and the observer port (Platform/FaultServer.h), so a
// fault in a filter running on one is handled by the other.
union ServerRequest
{
    mach_msg_header_t Head;
    uint8_t Room[sizeof(union __RequestUnion__catch_mach_exc_subsystem) + sizeof(mach_msg_max_trailer_t)];
    uint8_t ObserverRoom[kObserverMessageMax];
};

union ServerReply
{
    mach_msg_header_t Head;
    union __ReplyUnion__catch_mach_exc_subsystem Exception;
};

ThreadGuards* CurrentThreadGuards();

// The signal mask of a thread of this layer's own, which otherwise has the
// one of whatever thread happened to start it. A signal sent to the process
// is the game's threads' to take: its handler does not belong on a thread
// that is answering for a stopped one. The fatal signals stay open even when
// the starting thread had them blocked: a fault of this thread must take its
// course and end the process, and a blocked fault would be taken again and
// again instead.
void SetOwnThreadSignalMask()
{
    sigset_t mask;
    sigfillset(&mask);
    for (int signal : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT }) sigdelset(&mask, signal);
    pthread_sigmask(SIG_SETMASK, &mask, nullptr);
}

// What a new exception thread is told.
struct ServerStart
{
    mach_port_t PortSet;
    Serving* Slot;
};
constinit ServerStart gServerStarts[kServerCount]{};

void* ServerMain(void* arg)
{
    pthread_setname_np("bg3mse fault handler");
    SetOwnThreadSignalMask();
    NoteServerThread();
    NoteFaultThreadPort();
    auto start = static_cast<ServerStart const*>(arg);
    mach_port_t portSet = start->PortSet;
    tServer = new ServerContext();
    tServer->Slot = start->Slot;
    tServer->Slot->Thread.store(pthread_self(), std::memory_order_relaxed);
    auto request = new ServerRequest();
    auto reply = new ServerReply();
    // This thread's own guard state, which a thread otherwise gets at its
    // first guarded call: a filter's guarded call must find it there, because
    // making it allocates, and the faulting thread may be stopped holding the
    // allocator's lock. (Waits for the start that created this thread to finish.)
    CurrentThreadGuards();
    for (;;) {
        mach_msg_return_t mr = mach_msg(&request->Head, MACH_RCV_MSG, 0, sizeof(*request), portSet, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (mr == MACH_RCV_INVALID_NAME || mr == MACH_RCV_PORT_DIED) break;
        if (mr != MACH_MSG_SUCCESS) continue;
        if (HandleObserverMessage(request->Head, tServer->Capture, tServer->Record)) continue;
        // Where the answer goes, for the watchdog should it have to give it.
        tServer->ReplyPort = request->Head.msgh_remote_port;
        tServer->ReplyBits = MACH_MSGH_BITS_REMOTE(request->Head.msgh_bits);
        tServer->ReplyId = request->Head.msgh_id + 100;
        if (!mach_exc_server(&request->Head, &reply->Head)) {
            mach_msg_destroy(&request->Head);
            continue;
        }
        // The watchdog has answered this one.
        if (reply->Exception.Reply_mach_exception_raise.RetCode == MIG_NO_REPLY) continue;
        if (!MACH_PORT_VALID(reply->Head.msgh_remote_port)) continue;
        mr = mach_msg(&reply->Head, MACH_SEND_MSG, reply->Head.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        if (mr != MACH_MSG_SUCCESS) mach_msg_destroy(&reply->Head);
    }
    return nullptr;
}

// The watchdog answers the kernel for an exception thread that is past the
// limit: the exception was not handled. It is the watchdog's from the moment
// its phase is giving up; the exception thread's own answer is dropped.
void GiveUp(Serving& serving, uint64_t sequence)
{
    // The fault goes on as one every filter declined, and is not given to
    // the filters again when it comes back. The faulting thread, whose guard
    // state this is, is stopped until the answer below.
    if (ThreadGuards* tg = serving.Guards) {
        tg->DeclinedAddress.store(serving.Address, std::memory_order_relaxed);
        tg->DeclinedPC.store(serving.PC, std::memory_order_relaxed);
        tg->Declined.store(true, std::memory_order_relaxed);
    }
    mig_reply_error_t reply{};
    reply.Head.msgh_bits = MACH_MSGH_BITS(serving.ReplyBits, 0);
    reply.Head.msgh_remote_port = serving.ReplyPort;
    reply.Head.msgh_size = sizeof(reply);
    reply.Head.msgh_id = serving.ReplyId;
    reply.NDR = NDR_record;
    reply.RetCode = KERN_FAILURE;
    if (MACH_PORT_VALID(reply.Head.msgh_remote_port) &&
        mach_msg(&reply.Head, MACH_SEND_MSG, sizeof(reply), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != MACH_MSG_SUCCESS) {
        mach_msg_destroy(&reply.Head);
    }
    serving.State.store(sequence << kPhaseBits | kGivenUp, std::memory_order_release);
    // Whatever the filter was doing for the process's last moments is not
    // waited for any longer.
    VoidFatalFaultHoldOf(serving.Thread.load(std::memory_order_relaxed));
    // Last: the log may be what does not come back.
    LogFromFaultHandler(LogLevel::Error, "fault handling: a guard's filters did not answer within the time limit; the fault goes on without them");
}

void* WatchdogMain(void* arg)
{
    pthread_setname_np("bg3mse fault watchdog");
    SetOwnThreadSignalMask();
    NoteFaultThreadPort();
    auto port = static_cast<mach_port_t>(reinterpret_cast<uintptr_t>(arg));
    for (;;) {
        uint64_t now = mach_absolute_time();
        uint64_t next = UINT64_MAX;
        for (auto& serving : gServing) {
            uint64_t state = serving.State.load(std::memory_order_acquire);
            uint64_t phase = state & kPhaseMask;
            if (phase != kWaiting && phase != kDeciding) continue;
            uint64_t deadline = serving.Deadline.load(std::memory_order_relaxed);
            if (now < deadline) {
                next = std::min(next, deadline);
            } else if (phase == kWaiting) {
                // Only the exception whose deadline that was.
                if (serving.State.compare_exchange_strong(state, (state & ~kPhaseMask) | kGivingUp, std::memory_order_acq_rel)) {
                    GiveUp(serving, state >> kPhaseBits);
                } else {
                    next = now;
                }
            } else {
                // Past the limit and deciding: it answers itself in a moment, or waits again.
                next = now;
            }
        }

        struct
        {
            mach_msg_header_t Head;
            mach_msg_max_trailer_t Trailer;
        } kick;
        mach_msg_option_t options = MACH_RCV_MSG;
        mach_msg_timeout_t timeout = MACH_MSG_TIMEOUT_NONE;
        if (next != UINT64_MAX) {
            options |= MACH_RCV_TIMEOUT;
            timeout = std::max<uint32_t>(next > now ? MillisecondsUntil(next) : 0, 10);
        }
        mach_msg_return_t mr = mach_msg(&kick.Head, options, 0, sizeof(kick), port, timeout, MACH_PORT_NULL);
        if (mr == MACH_RCV_INVALID_NAME || mr == MACH_RCV_PORT_DIED) break;
    }
    return nullptr;
}

void ForkPrepare() { pthread_mutex_lock(&gStartMutex); }
void ForkParent() { pthread_mutex_unlock(&gStartMutex); }

// The child has none of the parent's ports or threads but this one.
void ForkChild()
{
    pthread_mutex_unlock(&gStartMutex);
    gStarted.store(false, std::memory_order_relaxed);
    gGeneration.fetch_add(1, std::memory_order_relaxed);
    gPortSet = MACH_PORT_NULL;
    for (auto& thread : gServerThreads) thread.store(nullptr, std::memory_order_relaxed);
    for (auto& port : gFaultThreadPorts) port.store(MACH_PORT_NULL, std::memory_order_relaxed);
    gWatchPort.store(MACH_PORT_NULL, std::memory_order_relaxed);
    for (auto& serving : gServing) serving.State.store(kIdle, std::memory_order_relaxed);
    ResetObserverAfterFork();
}

struct StartLock
{
    StartLock() { pthread_mutex_lock(&gStartMutex); }
    ~StartLock() { pthread_mutex_unlock(&gStartMutex); }
};

bool Start()
{
    StartLock lock;
    if (gStarted.load(std::memory_order_relaxed)) return true;

    if (!gKeyCreated) {
        if (pthread_key_create(&gKey, DestroyThreadGuards) != 0) return false;
        gKeyCreated = true;
    }
    if (!gForkHandlersInstalled) {
        if (pthread_atfork(ForkPrepare, ForkParent, ForkChild) != 0) return false;
        gForkHandlersInstalled = true;
    }

    Timebase();

    mach_port_t portSet = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &portSet) != KERN_SUCCESS) return false;
    if (!StartObserverPort(portSet)) {
        mach_port_destruct(mach_task_self(), portSet, 0, 0);
        return false;
    }

    // The watchdog first: no exception thread serves without it. Its port
    // going away is what ends it.
    mach_port_t watch = MACH_PORT_NULL;
    bool watching = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &watch) == KERN_SUCCESS;
    bool watchSendRight = watching && mach_port_insert_right(mach_task_self(), watch, watch, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS;
    watching = watchSendRight;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (watching) {
        pthread_attr_setstacksize(&attr, kWatchdogStack);
        pthread_t thread;
        watching = pthread_create(&thread, &attr, WatchdogMain, reinterpret_cast<void*>(static_cast<uintptr_t>(watch))) == 0;
    }

    int started = 0;
    if (watching) {
        // Before any exception thread can have something to tell it.
        gWatchPort.store(watch, std::memory_order_release);
        for (auto& serving : gServing) serving.State.store(kIdle, std::memory_order_relaxed);
        pthread_attr_setstacksize(&attr, kServerStack);
        for (int i = 0; i < kServerCount; i++) {
            gServerStarts[i] = { portSet, &gServing[i] };
            pthread_t thread;
            if (pthread_create(&thread, &attr, ServerMain, &gServerStarts[i]) == 0) started++;
        }
    }
    pthread_attr_destroy(&attr);
    if (started == 0) {
        // Nothing receives from the set: take it and the watchdog's port down.
        gWatchPort.store(MACH_PORT_NULL, std::memory_order_release);
        if (MACH_PORT_VALID(watch)) mach_port_destruct(mach_task_self(), watch, watchSendRight ? -1 : 0, 0);
        mach_port_destruct(mach_task_self(), portSet, 0, 0);
        return false;
    }
    gPortSet = portSet;
    gStarted.store(true, std::memory_order_release);
    return true;
}

bool EnsureStarted()
{
    if (gStarted.load(std::memory_order_acquire)) return true;
    if (Start()) return true;
    StartLock lock;
    if (!gStartFailedLogged) {
        gStartFailedLogged = true;
        Log(LogLevel::Error, "fault handling could not be started; guarded calls run unguarded");
    }
    return false;
}

ThreadGuards* CurrentThreadGuards()
{
    if (!EnsureStarted()) return nullptr;
    uint32_t generation = gGeneration.load(std::memory_order_relaxed);
    auto tg = static_cast<ThreadGuards*>(pthread_getspecific(gKey));
    if (tg && tg->Generation == generation) return tg;

    if (!tg) {
        tg = static_cast<ThreadGuards*>(calloc(1, sizeof(ThreadGuards)));
        if (!tg) return nullptr;
        if (pthread_setspecific(gKey, tg) != 0) {
            free(tg);
            return nullptr;
        }
    }
    // A new thread, or the thread that forked: set up from scratch.
    tg->Generation = generation;
    tg->Top.store(nullptr, std::memory_order_relaxed);
    tg->Declined.store(false, std::memory_order_relaxed);
    tg->SavedCount = 0;
    pthread_t self = pthread_self();
    tg->Thread = pthread_mach_thread_np(self);
    pthread_threadid_np(self, &tg->ThreadId);
    tg->StackHigh = reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(self));
    tg->StackLow = tg->StackHigh - pthread_get_stacksize_np(self);

    mach_port_t port = MACH_PORT_NULL;
    bool sendRight = false;
    kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (kr == KERN_SUCCESS) {
        kr = mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
        sendRight = kr == KERN_SUCCESS;
    }
    if (kr == KERN_SUCCESS) kr = mach_port_set_context(mach_task_self(), port, reinterpret_cast<mach_port_context_t>(tg));
    if (kr == KERN_SUCCESS) kr = mach_port_move_member(mach_task_self(), port, gPortSet);
    if (kr != KERN_SUCCESS) {
        // With the send right, when it was made: left behind it would stay
        // as a dead name, one for every guard that tries again.
        if (MACH_PORT_VALID(port)) mach_port_destruct(mach_task_self(), port, sendRight ? -1 : 0, 0);
        tg->Generation = 0;         // retried at the next guard
        return nullptr;
    }
    tg->Port = port;
    return tg;
}

// The exception goes on as if this guard's port had not been there: to the
// handler the outermost guard replaced, or back to the kernel, which tries
// the task's port and then raises the signal.
kern_return_t Forward(ThreadGuards* tg, exception_type_t exception, mach_exception_data_t code, mach_msg_type_number_t codeCount,
    thread_state_t newState, mach_msg_type_number_t* newStateCount)
{
    mach_msg_type_number_t i = 0;
    while (i < tg->SavedCount && !(tg->SavedMasks[i] & (1u << exception))) i++;
    if (i == tg->SavedCount || !MACH_PORT_VALID(tg->SavedPorts[i])) return KERN_FAILURE;

    mach_port_t port = tg->SavedPorts[i];
    exception_behavior_t behavior = tg->SavedBehaviors[i];
    bool wide = (behavior & MACH_EXCEPTION_CODES) != 0;
    exception_behavior_t base = behavior & ~MACH_EXCEPTION_MASK;
    exception_data_type_t narrow[2] = { (exception_data_type_t)(codeCount > 0 ? code[0] : 0), (exception_data_type_t)(codeCount > 1 ? code[1] : 0) };

    kern_return_t kr = KERN_FAILURE;
    if (base == EXCEPTION_DEFAULT) {
        kr = wide ? mach_exception_raise(port, tg->Thread, mach_task_self(), exception, code, codeCount)
                  : exception_raise(port, tg->Thread, mach_task_self(), exception, narrow, codeCount);
    } else if (base == EXCEPTION_STATE || base == EXCEPTION_STATE_IDENTITY) {
        thread_state_flavor_t flavor = tg->SavedFlavors[i];
        natural_t state[THREAD_STATE_MAX];
        natural_t handled[THREAD_STATE_MAX];
        mach_msg_type_number_t stateCount = THREAD_STATE_MAX;
        mach_msg_type_number_t handledCount = THREAD_STATE_MAX;
        if (thread_get_state(tg->Thread, flavor, state, &stateCount) != KERN_SUCCESS) return KERN_FAILURE;
        if (base == EXCEPTION_STATE) {
            kr = wide ? mach_exception_raise_state(port, exception, code, codeCount, &flavor, state, stateCount, handled, &handledCount)
                      : exception_raise_state(port, exception, narrow, codeCount, &flavor, state, stateCount, handled, &handledCount);
        } else {
            kr = wide ? mach_exception_raise_state_identity(port, tg->Thread, mach_task_self(), exception, code, codeCount, &flavor,
                            state, stateCount, handled, &handledCount)
                      : exception_raise_state_identity(port, tg->Thread, mach_task_self(), exception, narrow, codeCount, &flavor,
                            state, stateCount, handled, &handledCount);
        }
        if (kr == KERN_SUCCESS) thread_set_state(tg->Thread, flavor, handled, handledCount);
    }
    if (kr != KERN_SUCCESS) return kr;

    // Handled: the thread resumes with the registers that handler left, which
    // this reply carries back.
    *newStateCount = ARM_THREAD_STATE64_COUNT;
    return thread_get_state(tg->Thread, ARM_THREAD_STATE64, newState, newStateCount) == KERN_SUCCESS ? KERN_SUCCESS : KERN_FAILURE;
}

kern_return_t HandleException(ThreadGuards* tg, exception_type_t exception, mach_exception_data_t code, mach_msg_type_number_t codeCount,
    arm_thread_state64_t const& state, thread_state_t newState, mach_msg_type_number_t* newStateCount)
{
    uint64_t pc = arm_thread_state64_get_pc(state);
    int64_t codes[2] = { codeCount > 0 ? code[0] : 0, codeCount > 1 ? code[1] : 0 };
    uint64_t address = exception == EXC_BAD_ACCESS ? (uint64_t)codes[1] : pc;

    // From here to the answer the exception is under the time limit: see Serving.
    Serving& serving = *tServer->Slot;
    BeginServing(serving, tg, pc, address);

    GuardFrame* top = tg->Top.load(std::memory_order_relaxed);
    bool again = tg->Declined.load(std::memory_order_relaxed) && tg->DeclinedPC.load(std::memory_order_relaxed) == pc &&
        tg->DeclinedAddress.load(std::memory_order_relaxed) == address;
    bool debugger = exception == EXC_BREAKPOINT && IsTraced();
    if (top && !again && !debugger) {
        FaultSource source{};
        source.MachException = (uint32_t)exception;
        source.Code[0] = codes[0];
        source.Code[1] = codes[1];
        source.State = &state;
        source.ThreadId = tg->ThreadId;
        source.Thread = tg->Thread;
        source.StackLow = tg->StackLow;
        source.StackHigh = tg->StackHigh;
        source.Origin = FaultOrigin::Guarded;
        FaultRecord& record = tServer->Record;
        tServer->Capture.Capture(source, record);

        for (GuardFrame* frame = top; frame; frame = frame->Outer) {
            // What is needed of the frame is read before its filter runs,
            // and the frame is touched again only if the exception is still
            // this thread's when the filter has returned.
            FaultFilter filter = frame->Filter;
            void* context = frame->Context;
            FaultDecision decision = FaultDecision::Recover;
            if (filter) {
                WaitServing(serving);
                decision = filter(record, context);
                if (!DecideServing(serving)) return MIG_NO_REPLY;
            }
            if (decision != FaultDecision::Recover) continue;

            if (frame->Out) memcpy(frame->Out, &record, sizeof(record));
            tg->Top.store(frame, std::memory_order_relaxed);
            arm_thread_state64_t resumed = state;
            arm_thread_state64_set_pc_fptr(resumed, &bg3mse_guard_resume);
            arm_thread_state64_set_sp(resumed, frame->Saved[kSavedSP]);
            resumed.__x[0] = reinterpret_cast<uint64_t>(frame->Saved);
            memcpy(newState, &resumed, sizeof(resumed));
            *newStateCount = ARM_THREAD_STATE64_COUNT;
            EndServing(serving);
            return KERN_SUCCESS;
        }
        tg->DeclinedAddress.store(address, std::memory_order_relaxed);
        tg->DeclinedPC.store(pc, std::memory_order_relaxed);
        tg->Declined.store(true, std::memory_order_relaxed);
    }
    // The handler the fault is forwarded to is somebody else's, and waited for.
    WaitServing(serving);
    kern_return_t kr = Forward(tg, exception, code, codeCount, newState, newStateCount);
    if (!DecideServing(serving)) return MIG_NO_REPLY;
    EndServing(serving);
    return kr;
}

}

bool StartFaultHandling()
{
    return EnsureStarted();
}

bool IsFaultHandlingThread(mach_port_t thread)
{
    if (!MACH_PORT_VALID(thread)) return false;
    for (auto const& port : gFaultThreadPorts) {
        if (port.load(std::memory_order_relaxed) == thread) return true;
    }
    return false;
}

uint32_t FaultTimeLimitMs()
{
    return gTimeLimitMs.load(std::memory_order_relaxed);
}

void SetFaultTimeLimitMs(uint32_t milliseconds)
{
    gTimeLimitMs.store(milliseconds, std::memory_order_relaxed);
}

uint64_t FaultDeadlineFromNow()
{
    auto const& timebase = Timebase();
    return mach_absolute_time() + static_cast<uint64_t>(FaultTimeLimitMs()) * 1000000 * timebase.denom / timebase.numer;
}

uint32_t MillisecondsUntil(uint64_t deadline)
{
    uint64_t now = mach_absolute_time();
    if (now >= deadline) return 0;
    auto const& timebase = Timebase();
    uint64_t milliseconds = ((deadline - now) * timebase.numer / timebase.denom + 999999) / 1000000;
    return static_cast<uint32_t>(std::min<uint64_t>(milliseconds, 24 * 60 * 60 * 1000));
}

bool EnsureFaultServer()
{
    return EnsureStarted();
}

void NoteServerThread()
{
    pthread_t self = pthread_self();
    for (auto& thread : gServerThreads) {
        pthread_t expected = nullptr;
        if (thread.compare_exchange_strong(expected, self)) return;
    }
}

bool IsServerThread()
{
    pthread_t self = pthread_self();
    for (auto const& thread : gServerThreads) {
        if (thread.load(std::memory_order_relaxed) == self) return true;
    }
    return false;
}

bool GuardsDeclinedFaultAt(uint64_t pc)
{
    if (!gKeyCreated) return false;
    auto tg = static_cast<ThreadGuards*>(pthread_getspecific(gKey));
    if (!tg || tg->Generation != gGeneration.load(std::memory_order_relaxed) || !tg->Top.load(std::memory_order_relaxed)) return false;
    return tg->Declined.load(std::memory_order_relaxed) && tg->DeclinedPC.load(std::memory_order_relaxed) == pc;
}

bool GuardedInvoke(void (*fn)(void*), void* arg, FaultFilter filter, void* context, FaultRecord* fault)
{
    ThreadGuards* tg = CurrentThreadGuards();
    if (!tg) {
        fn(arg);
        return true;
    }

    GuardFrame frame;
    frame.Filter = filter;
    frame.Context = context;
    frame.Out = fault;
    frame.Generation = tg->Generation;
    frame.Outer = tg->Top.load(std::memory_order_relaxed);
    if (!frame.Outer && !InstallPort(tg)) {
        fn(arg);
        return true;
    }
    tg->Declined.store(false, std::memory_order_relaxed);
    tg->Top.store(&frame, std::memory_order_relaxed);
    // The exception thread reads the list while this thread is stopped at a
    // fault; the stores above must be done before the callable runs.
    std::atomic_signal_fence(std::memory_order_seq_cst);

    struct Leave
    {
        ThreadGuards* Guards;
        GuardFrame* Frame;
        ~Leave() { Pop(Guards, Frame); }
    } leave{ tg, &frame };
    return bg3mse_guard_run(frame.Saved, fn, arg) == 0;
}

}

// The MIG server's routines. Guard ports are registered for EXCEPTION_STATE
// only; the other two answer that the exception was not handled.

extern "C" kern_return_t catch_mach_exception_raise(mach_port_t, mach_port_t, mach_port_t, exception_type_t, mach_exception_data_t,
    mach_msg_type_number_t)
{
    return KERN_FAILURE;
}

extern "C" kern_return_t catch_mach_exception_raise_state_identity(mach_port_t, mach_port_t, mach_port_t, exception_type_t,
    mach_exception_data_t, mach_msg_type_number_t, int*, thread_state_t, mach_msg_type_number_t, thread_state_t, mach_msg_type_number_t*)
{
    return KERN_FAILURE;
}

extern "C" kern_return_t catch_mach_exception_raise_state(mach_port_t exceptionPort, exception_type_t exception, const mach_exception_data_t code,
    mach_msg_type_number_t codeCount, int* flavor, const thread_state_t oldState, mach_msg_type_number_t oldStateCount,
    thread_state_t newState, mach_msg_type_number_t* newStateCount)
{
    using namespace bg3se::mac;
    mach_port_context_t context = 0;
    if (mach_port_get_context(mach_task_self(), exceptionPort, &context) != KERN_SUCCESS || !context) return KERN_FAILURE;
    if (*flavor != ARM_THREAD_STATE64 || oldStateCount < ARM_THREAD_STATE64_COUNT) return KERN_FAILURE;
    arm_thread_state64_t state;
    memcpy(&state, oldState, sizeof(state));
    return HandleException(reinterpret_cast<ThreadGuards*>(context), exception, code, codeCount, state, newState, newStateCount);
}
