#include <Platform/FaultObserver.h>
#include <Platform/FaultServer.h>
#include <Platform/Log.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <mach/mach.h>
#include <poll.h>
#include <pthread.h>

namespace bg3se::mac {

namespace {

constexpr int kSignals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT };
constexpr int kMaxObservers = 4;
constexpr mach_msg_id_t kObserverMessageId = 0x62676d01;

// An observer slot is read by the exception threads without a lock: Version
// is odd while the slot is being written, and a reader that sees it change
// skips the slot.
struct Slot
{
    std::atomic<uint32_t> Version;
    std::atomic<FatalFaultObserver> Observer;
    std::atomic<void*> Context;
};

// Constant-initialised: a dylib constructor may run before this file's
// dynamic initialisers.
constinit pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;     // no destructor to run at exit
constinit Slot gSlots[kMaxObservers]{};
// Per signal: the actions bg3mse's handler has displaced, in the order it
// displaced them. The first is what the signal was for before the handler was
// first installed. Each later one is a handler somebody installed after
// that, in front of bg3mse's, and which bg3mse then put its own in front of
// again (PutFatalFaultObserversInFront). An entry is written before the
// count says it is there and never changed after: handlers read them without
// a lock. Whether the handler is the signal's action is not kept here; the
// kernel is asked.
constexpr int kMaxDisplaced = 8;
constinit struct sigaction gDisplaced[NSIG][kMaxDisplaced]{};
constinit std::atomic<int> gDisplacedCount[NSIG]{};

// The handler's frames that are passing a signal on. A handler that was
// displaced may pass the signal to the one it found when it was installed,
// which is bg3mse's: that call comes from inside a frame listed here, on the
// same thread and with the same context, and must go on to the action
// displaced before, not round again. No thread-local variable: a thread's
// first use of one allocates.
struct Passing
{
    std::atomic<pthread_t> Thread;      // whose entry this is; nullptr when free
    // The thread's own: nobody else reads or writes them.
    int Signal;
    void const* Context;                // what the signal was delivered with
    uintptr_t Frame;                    // an address in the handler's frame
};
constexpr int kMaxPassing = 32;
constinit Passing gPassing[kMaxPassing]{};
constinit std::atomic<mach_port_t> gObserverPort{ MACH_PORT_NULL };

// Holds (see the header): how many are taken, by which thread the last one,
// and whether that thread has been given up on.
constinit std::atomic<int> gHolds{ 0 };
constinit std::atomic<pthread_t> gHoldOwner{ nullptr };
constinit std::atomic<bool> gHoldVoid{ false };

struct ObserverRequest
{
    mach_msg_header_t Head;
    int Signal;
    int SignalCode;
    uint64_t SignalAddress;
    arm_thread_state64_t State;
    arm_exception_state64_t ExceptionState;
    uint64_t ThreadId;
    mach_port_t Thread;
    uint64_t StackLow, StackHigh;
    uint8_t Origin;
};

struct ObserverReply
{
    mach_msg_header_t Head;
    mach_msg_max_trailer_t Trailer;
};

static_assert(sizeof(ObserverRequest) + sizeof(mach_msg_max_trailer_t) <= kObserverMessageMax);

struct Lock
{
    Lock() { pthread_mutex_lock(&gMutex); }
    ~Lock() { pthread_mutex_unlock(&gMutex); }
};

bool HaveObservers()
{
    for (auto const& slot : gSlots) {
        if (slot.Observer.load(std::memory_order_relaxed)) return true;
    }
    return false;
}

// Sends the faulting thread's state to an exception thread and waits for the
// observers to have run. Only system calls: this runs in a signal handler.
void TellObservers(int signal, siginfo_t const* info, ucontext_t const* context)
{
    mach_port_t port = gObserverPort.load(std::memory_order_acquire);
    if (!MACH_PORT_VALID(port) || !context || !context->uc_mcontext) return;

    union
    {
        ObserverRequest Request;
        ObserverReply Reply;
    } message;
    memset(&message, 0, sizeof(message));
    auto& request = message.Request;
    request.Signal = signal;
    request.SignalCode = info ? info->si_code : 0;
    request.SignalAddress = info ? reinterpret_cast<uint64_t>(info->si_addr) : 0;
    request.State = context->uc_mcontext->__ss;
    request.ExceptionState = context->uc_mcontext->__es;
    pthread_t self = pthread_self();
    pthread_threadid_np(self, &request.ThreadId);
    request.Thread = pthread_mach_thread_np(self);
    request.StackHigh = reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(self));
    request.StackLow = request.StackHigh - pthread_get_stacksize_np(self);
    request.Origin = static_cast<uint8_t>(GuardsDeclinedFaultAt(arm_thread_state64_get_pc(request.State))
        ? FaultOrigin::DeclinedByGuards : FaultOrigin::Unguarded);

    // A kernel call, as the rest: nothing here allocates memory or takes a lock.
    mach_port_t reply = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply) != KERN_SUCCESS) return;
    request.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    request.Head.msgh_remote_port = port;
    request.Head.msgh_local_port = reply;
    request.Head.msgh_size = sizeof(request);
    request.Head.msgh_id = kObserverMessageId;
    // The time limit is for both together: getting the message to an
    // exception thread (they may all be busy) and the observers' answer.
    uint64_t deadline = FaultDeadlineFromNow();
    mach_msg_return_t sent = mach_msg(&request.Head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(request), 0, MACH_PORT_NULL,
        FaultTimeLimitMs(), MACH_PORT_NULL);
    if (sent == MACH_MSG_SUCCESS) {
        mach_msg(&message.Reply.Head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(message), reply, MillisecondsUntil(deadline), MACH_PORT_NULL);
    } else if (sent == MACH_SEND_TIMED_OUT || sent == MACH_SEND_INTERRUPTED || sent == MACH_SEND_INVALID_DEST) {
        // The message came back with the rights it carried.
        mach_msg_destroy(&request.Head);
    }
    mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1);
}

// What a signal is passed on to.
enum class Before { Handler, Ignored, Default };

Before KindOf(struct sigaction const* action)
{
    if (!action || action->sa_handler == SIG_DFL) return Before::Default;
    if (action->sa_handler == SIG_IGN) return Before::Ignored;
    return Before::Handler;
}

// Whether two addresses are on the same stack of this thread: its own, or
// the alternate signal stack it has now.
bool OnTheSameStack(uintptr_t a, uintptr_t b)
{
    pthread_t self = pthread_self();
    auto high = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(self));
    auto low = high - pthread_get_stacksize_np(self);
    bool aOnIt = a >= low && a < high, bOnIt = b >= low && b < high;
    if (aOnIt || bOnIt) return aOnIt && bOnIt;
    stack_t alternate{};
    if (sigaltstack(nullptr, &alternate) != 0 || (alternate.ss_flags & SS_DISABLE)) return false;
    auto altLow = reinterpret_cast<uintptr_t>(alternate.ss_sp), altHigh = altLow + alternate.ss_size;
    return a >= altLow && a < altHigh && b >= altLow && b < altHigh;
}

// Lists this call of the handler, and counts the handler's frames on this
// thread that are passing this very delivery on (the same signal and
// context) and that this call was made from inside of: 0 when the kernel or
// a handler in front called, more when a displaced handler calls back.
// `entry` is the place taken in the list, -1 when none was free.
//
// A frame of this thread's at or below this one on the same stack is gone:
// its handler never returned (a handler it had called jumped out). Its entry
// is taken over or freed. So a new delivery at the same depth, which comes
// with the same context address, is not taken for a call from inside.
int EnterPassing(int signal, void const* context, uintptr_t frame, int& entry)
{
    pthread_t self = pthread_self();
    int depth = 0;
    entry = -1;
    for (int i = 0; i < kMaxPassing; i++) {
        Passing& passing = gPassing[i];
        pthread_t owner = passing.Thread.load(std::memory_order_relaxed);
        if (owner == self) {
            if (passing.Frame <= frame && OnTheSameStack(passing.Frame, frame)) {
                if (entry < 0) {
                    entry = i;
                } else {
                    passing.Thread.store(nullptr, std::memory_order_relaxed);
                }
            } else if (passing.Signal == signal && passing.Context == context && passing.Frame > frame) {
                depth++;
            }
        } else if (owner == nullptr && entry < 0) {
            pthread_t expected = nullptr;
            if (passing.Thread.compare_exchange_strong(expected, self, std::memory_order_relaxed)) entry = i;
        }
    }
    if (entry >= 0) {
        gPassing[entry].Signal = signal;
        gPassing[entry].Context = context;
        gPassing[entry].Frame = frame;
    }
    return depth;
}

void LeavePassing(int entry)
{
    if (entry >= 0) gPassing[entry].Thread.store(nullptr, std::memory_order_relaxed);
}

// What a call of the handler passes the signal on to: the action displaced
// last, or, when the call comes from inside `depth` of the handler's own
// frames, the one displaced that many times earlier. nullptr when there is
// none left, which is the default action.
struct sigaction const* DisplacedBelow(int signal, int depth)
{
    int index = gDisplacedCount[signal].load(std::memory_order_acquire) - 1 - depth;
    return index >= 0 ? &gDisplaced[signal][index] : nullptr;
}

// Calls a handler bg3mse's displaced the way the kernel would have, had it
// still been the signal's action: with the signals of its sa_mask blocked as
// well, and with the signal itself blocked unless it asked for SA_NODEFER.
// The fatal signals bg3mse's own action blocks stay blocked meanwhile. It
// gets the signal's information and context as they came.
//
// SA_RESETHAND is the one thing of the old action that cannot be honoured:
// the kernel does not say that an action had it. sigaction returns the old
// action's flags without that bit (measured on macOS 27.2: installed with
// 0x44, returned as 0x40). A handler that asked to be called once is called
// each time a signal passes through here.
void CallDisplaced(struct sigaction const& displaced, int signal, siginfo_t* info, void* context)
{
    sigset_t before, itself;
    pthread_sigmask(SIG_BLOCK, &displaced.sa_mask, &before);
    sigemptyset(&itself);
    sigaddset(&itself, signal);
    pthread_sigmask((displaced.sa_flags & SA_NODEFER) ? SIG_UNBLOCK : SIG_BLOCK, &itself, nullptr);

    if (displaced.sa_flags & SA_SIGINFO) {
        displaced.sa_sigaction(signal, info, context);
    } else {
        displaced.sa_handler(signal);
    }
    pthread_sigmask(SIG_SETMASK, &before, nullptr);
}

// The signal goes on to the action bg3mse's handler displaced.
void PassOn(int signal, struct sigaction const* displaced, siginfo_t* info, void* context)
{
    switch (KindOf(displaced)) {
    case Before::Handler:
        CallDisplaced(*displaced, signal, info, context);
        return;
    case Before::Ignored:
        // Nothing would have happened, and nothing is changed: the signal's
        // handler, bg3mse's or one installed after it, stays.
        return;
    case Before::Default:
        break;
    }
    // The default action, which ends the process. The signal gets it back in
    // place of the handler it has now, which is bg3mse's own or one that was
    // installed later and called it. Then the signal is sent to this thread
    // again: it stays pending while a handler runs and ends the process when
    // the handler returns, before the thread runs another instruction.
    //
    // It is sent again whatever it was. A fault would also happen again by
    // itself, its instruction being re-executed, but a signal that kill,
    // raise or pthread_kill sent would not, and nothing the handler is given
    // tells the two apart on this system: si_code is the same for both
    // (SEGV_ACCERR, BUS_ADRALN, ILL_ILLTRP by the signal's number alone),
    // si_addr is 0 for a sent SIGSEGV as for a fault at address 0, and the
    // exception state in the context is stale when the signal was delivered
    // from an interrupt (a data abort's syndrome on a sent signal). A wrong
    // guess would leave the process running with the signal lost. The thread
    // dies with the registers it had at the fault, which the return from the
    // handler restores, and the system's crash report still names the fault's
    // exception, codes and address (the kernel keeps them with the thread);
    // only that report's far and esr are those of the last system call.
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(signal, &action, nullptr);
    pthread_kill(pthread_self(), signal);
}

void FatalSignalHandler(int signal, siginfo_t* info, void* context)
{
    int savedErrno = errno;
    int entry = -1;
    int depth = EnterPassing(signal, context, reinterpret_cast<uintptr_t>(&entry), entry);
    struct sigaction const* displaced = DisplacedBelow(signal, depth);
    // Observers are told once per delivery, by the call that came from
    // outside. A signal that is passed on to "ignore" would not have been
    // delivered at all: it is nobody's fatal fault.
    if (depth == 0 && KindOf(displaced) != Before::Ignored) {
        if (HaveObservers() && !IsServerThread()) TellObservers(signal, info, static_cast<ucontext_t const*>(context));
        // What comes next may end the process.
        WaitForFatalFaultHold();
    }
    PassOn(signal, displaced, info, context);
    LeavePassing(entry);
    errno = savedErrno;
}

bool IsThisHandler(struct sigaction const& action)
{
    return (action.sa_flags & SA_SIGINFO) != 0 && action.sa_sigaction == &FatalSignalHandler;
}

// Makes the handler the action of every signal that has another, saving the
// action it displaces as what the signal is passed on to. A signal that
// already has the handler is left alone: installing it again would save the
// handler as the one behind itself. False when a signal could not be given
// it (logged); the others have it, and the next call tries again. The caller
// holds gMutex.
bool InstallInFront()
{
    struct sigaction action{};
    action.sa_sigaction = FatalSignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int signal : kSignals) sigaddset(&action.sa_mask, signal);
    bool all = true;
    for (int signal : kSignals) {
        struct sigaction current{};
        if (sigaction(signal, nullptr, &current) == 0 && IsThisHandler(current)) continue;
        int count = gDisplacedCount[signal].load(std::memory_order_relaxed);
        if (count == kMaxDisplaced) {
            Log(LogLevel::Error, "fatal-fault observers: signal %d was given another handler %d times; bg3mse's stays behind it", signal, count);
            all = false;
            continue;
        }
        struct sigaction& displaced = gDisplaced[signal][count];
        if (sigaction(signal, &action, &displaced) != 0) {
            Log(LogLevel::Error, "fatal-fault observers: cannot install a handler for signal %d: %s", signal, strerror(errno));
            all = false;
            continue;
        }
        // Somebody may have put the handler back between the two calls: then
        // nothing was displaced.
        if (!IsThisHandler(displaced)) gDisplacedCount[signal].store(count + 1, std::memory_order_release);
    }
    return all;
}

}

bool StartObserverPort(mach_port_t portSet)
{
    mach_port_t port = MACH_PORT_NULL;
    bool sendRight = false;
    kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    if (kr == KERN_SUCCESS) {
        kr = mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
        sendRight = kr == KERN_SUCCESS;
    }
    if (kr == KERN_SUCCESS) kr = mach_port_move_member(mach_task_self(), port, portSet);
    if (kr != KERN_SUCCESS) {
        if (MACH_PORT_VALID(port)) mach_port_destruct(mach_task_self(), port, sendRight ? -1 : 0, 0);
        return false;
    }
    gObserverPort.store(port, std::memory_order_release);
    return true;
}

void ResetObserverAfterFork()
{
    gObserverPort.store(MACH_PORT_NULL, std::memory_order_relaxed);
}

bool HandleObserverMessage(mach_msg_header_t& message, FaultCapture& capture, FaultRecord& record)
{
    if (message.msgh_id != kObserverMessageId || message.msgh_local_port != gObserverPort.load(std::memory_order_relaxed)) return false;
    if (message.msgh_size < sizeof(ObserverRequest)) {
        mach_msg_destroy(&message);
        return true;
    }
    auto const& request = reinterpret_cast<ObserverRequest const&>(message);

    FaultSource source{};
    source.Signal = request.Signal;
    source.SignalCode = request.SignalCode;
    source.SignalAddress = request.SignalAddress;
    source.State = &request.State;
    source.ExceptionState = &request.ExceptionState;
    source.ThreadId = request.ThreadId;
    source.Thread = request.Thread;
    source.StackLow = request.StackLow;
    source.StackHigh = request.StackHigh;
    source.Origin = static_cast<FaultOrigin>(request.Origin);
    capture.Capture(source, record);

    for (auto& slot : gSlots) {
        uint32_t version = slot.Version.load(std::memory_order_acquire);
        if (version & 1) continue;
        FatalFaultObserver observer = slot.Observer.load(std::memory_order_relaxed);
        void* context = slot.Context.load(std::memory_order_relaxed);
        if (!observer || slot.Version.load(std::memory_order_acquire) != version) continue;
        observer(record, context);
    }

    mach_msg_header_t reply{};
    reply.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(message.msgh_bits), 0);
    reply.msgh_remote_port = message.msgh_remote_port;
    reply.msgh_size = sizeof(reply);
    reply.msgh_id = kObserverMessageId + 100;
    if (MACH_PORT_VALID(reply.msgh_remote_port) &&
        mach_msg(&reply, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(reply), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL) != MACH_MSG_SUCCESS) {
        mach_msg_destroy(&reply);
    }
    return true;
}

bool PutFatalFaultObserversInFront()
{
    if (!HaveObservers()) return true;
    Lock lock;
    return InstallInFront();
}

void HoldFatalFaults()
{
    gHoldOwner.store(pthread_self(), std::memory_order_relaxed);
    gHoldVoid.store(false, std::memory_order_relaxed);
    gHolds.fetch_add(1, std::memory_order_release);
}

void ReleaseFatalFaults()
{
    if (gHolds.fetch_sub(1, std::memory_order_release) == 1) gHoldOwner.store(nullptr, std::memory_order_relaxed);
}

void VoidFatalFaultHoldOf(pthread_t thread)
{
    if (gHoldOwner.load(std::memory_order_relaxed) == thread) gHoldVoid.store(true, std::memory_order_release);
}

// Runs in a signal handler: system calls and atomics only. poll() with no
// descriptors is the sleep a handler may take.
void WaitForFatalFaultHold()
{
    if (gHolds.load(std::memory_order_acquire) <= 0) return;
    if (IsServerThread() || gHoldOwner.load(std::memory_order_relaxed) == pthread_self()) return;
    uint64_t deadline = FaultDeadlineFromNow();
    while (gHolds.load(std::memory_order_acquire) > 0 && !gHoldVoid.load(std::memory_order_acquire) && MillisecondsUntil(deadline) > 0) {
        poll(nullptr, 0, 2);
    }
}

bool AddFatalFaultObserver(FatalFaultObserver observer, void* context)
{
    if (!observer || !EnsureFaultServer()) return false;
    Lock lock;
    if (!InstallInFront()) return false;
    for (auto& slot : gSlots) {
        if (slot.Observer.load(std::memory_order_relaxed)) continue;
        uint32_t version = slot.Version.load(std::memory_order_relaxed);
        slot.Version.store(version + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.Context.store(context, std::memory_order_relaxed);
        slot.Observer.store(observer, std::memory_order_relaxed);
        slot.Version.store(version + 2, std::memory_order_release);
        return true;
    }
    Log(LogLevel::Error, "fatal-fault observers: all %d slots are taken", kMaxObservers);
    return false;
}

void RemoveFatalFaultObserver(FatalFaultObserver observer, void* context)
{
    Lock lock;
    for (auto& slot : gSlots) {
        if (slot.Observer.load(std::memory_order_relaxed) != observer || slot.Context.load(std::memory_order_relaxed) != context) continue;
        uint32_t version = slot.Version.load(std::memory_order_relaxed);
        slot.Version.store(version + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.Observer.store(nullptr, std::memory_order_relaxed);
        slot.Context.store(nullptr, std::memory_order_relaxed);
        slot.Version.store(version + 2, std::memory_order_release);
        return;
    }
}

}
