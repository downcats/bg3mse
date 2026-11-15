// Tests of the lock functions without the game: the Win32 lock names, the
// inline parts of the game's lock protocols, and Norbyte's two lock classes,
// against real threads. The game's own lock functions are stood in for by
// functions written here to the protocol read from the game's code
// (Layout/GameShapes.cpp names the functions and what each does to which
// field).

#include <windows.h>

#include <Layout/GameLockCalls.h>
#include <Platform/Log.h>

#include "LockTestsNorbyte.h"

#include <atomic>
#include <cstdio>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace bg3se::mac;

namespace {

int gFailures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            gFailures++; \
        } \
    } while (0)

// --- The game's protocol ----------------------------------------------------------

uint32_t Load(uint32_t const& field) { return __atomic_load_n(&field, __ATOMIC_SEQ_CST); }
int32_t Load(int32_t const& field) { return __atomic_load_n(&field, __ATOMIC_SEQ_CST); }

template <class T>
bool CompareExchange(T& field, T& expected, T desired)
{
    return __atomic_compare_exchange_n(&field, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// The semaphore in `field`, created by whoever needs it first.
semaphore_t Semaphore(uint32_t& field)
{
    semaphore_t existing = Load(field);
    if (existing) return existing;
    semaphore_t made = 0;
    if (semaphore_create(mach_task_self(), &made, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) return 0;
    uint32_t expected = 0;
    if (CompareExchange(field, expected, (uint32_t)made)) return made;
    semaphore_destroy(mach_task_self(), made);
    return expected;
}

void Wait(uint32_t& field)
{
    semaphore_t semaphore = Semaphore(field);
    while (semaphore_wait(semaphore) == KERN_ABORTED) {}
}

// ls::CriticalSection::Lock()
void CriticalSectionLock(GameCriticalSection* cs)
{
    uint32_t thread = CurrentThreadPort();
    auto take = [&] {
        int32_t expected = -1;
        return CompareExchange(cs->LockCount, expected, 0);
    };
    bool mine = take();
    if (!mine) {
        if (cs->OwnerThread == thread) {
            __atomic_fetch_add(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
            cs->RecursionCount++;
            return;
        }
        for (int32_t spins = cs->SpinCount; spins > 0 && !mine; spins--) {
            int32_t count = Load(cs->LockCount);
            if (count >= 1) break;                  // others are already asleep: join them
            if (count == -1) mine = take();
        }
        if (!mine) {
            mine = __atomic_fetch_add(&cs->LockCount, 1, __ATOMIC_SEQ_CST) == -1;
            if (!mine) Wait(cs->Semaphore);         // handed over by Unlock()
        }
    }
    cs->OwnerThread = thread;
    cs->RecursionCount = 1;
}

// ls::CriticalSection::Unlock()
void CriticalSectionUnlock(GameCriticalSection* cs)
{
    if (--cs->RecursionCount != 0) {
        __atomic_fetch_sub(&cs->LockCount, 1, __ATOMIC_SEQ_CST);
        return;
    }
    cs->OwnerThread = 0;
    if (__atomic_fetch_sub(&cs->LockCount, 1, __ATOMIC_SEQ_CST) >= 1) semaphore_signal(Semaphore(cs->Semaphore));
}

// ls::CriticalSection::~CriticalSection()
void CriticalSectionDestroy(GameCriticalSection* cs)
{
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwnerThread = 0;
    if (cs->Semaphore) semaphore_destroy(mach_task_self(), cs->Semaphore);
    cs->Semaphore = 0;
}

// The state after adding `delta`, entering exclusive mode when a writer is
// there and no reader is. Returns the state before.
uint32_t KernelAdd(GameSRWKernelLock* lock, uint32_t delta)
{
    uint32_t state = Load(lock->State);
    for (;;) {
        uint32_t next = state + delta;
        if ((next & kKernelWriters) && !(next & kKernelReaders)) next |= kKernelExclusive;
        if (CompareExchange(lock->State, state, next)) return state;
    }
}

// The state after taking `delta` off, leaving exclusive mode with the last
// writer. Returns the state before.
uint32_t KernelRemove(GameSRWKernelLock* lock, uint32_t delta)
{
    uint32_t state = Load(lock->State);
    for (;;) {
        uint32_t next = state - delta;
        if (!(next & kKernelWriters)) next &= kKernelReaders;
        if (CompareExchange(lock->State, state, next)) return state;
    }
}

// srwlock_leave_exclusive
void KernelWake(GameSRWKernelLock* lock, uint32_t state)
{
    if (state & kKernelWriters) {
        semaphore_signal(Semaphore(lock->ExclusiveSemaphore));
    } else {
        for (uint32_t readers = state & kKernelReaders; readers > 0; readers--) semaphore_signal(Semaphore(lock->SharedSemaphore));
    }
}

// ls::SRWKernelLock::WriteLock()
void KernelWriteLock(GameSRWKernelLock* lock)
{
    if (KernelAdd(lock, kKernelWriter) != 0) Wait(lock->ExclusiveSemaphore);
}

// ls::SRWKernelLock::WriteUnlock()
void KernelWriteUnlock(GameSRWKernelLock* lock)
{
    KernelWake(lock, KernelRemove(lock, kKernelWriter) - kKernelWriter);
}

// ls::SRWKernelLock::ReadLock()
void KernelReadLock(GameSRWKernelLock* lock)
{
    uint32_t state = Load(lock->State);
    bool queue;
    for (;;) {
        // Behind writers that wait while readers hold the lock, a reader
        // queues as a writer would, so that it does not overtake them.
        queue = (state & kKernelWriters) && !(state & kKernelExclusive);
        if (CompareExchange(lock->State, state, state + (queue ? kKernelWriter : 1))) break;
    }
    if (queue) {
        Wait(lock->ExclusiveSemaphore);
        state = KernelRemove(lock, kKernelWriter - 1) - kKernelWriter;
        KernelWake(lock, state);
    }
    if (state & kKernelWriters) Wait(lock->SharedSemaphore);
}

// ls::SRWKernelLock::ReadUnlock()
void KernelReadUnlock(GameSRWKernelLock* lock)
{
    uint32_t before = KernelAdd(lock, (uint32_t)-1);
    uint32_t after = before - 1;
    if ((after & kKernelWriters) && !(after & kKernelReaders)) semaphore_signal(Semaphore(lock->ExclusiveSemaphore));
}

// ls::RecursiveSRWLock<ls::SRWSpinLock>::WriteLock()
void SpinWriteLock(GameRecursiveSpinLock* lock)
{
    uint32_t thread = CurrentThreadPort();
    if (lock->OwnerThread == kNoRecursiveOwner || lock->OwnerThread != thread) {
        for (;;) {
            while (Load(lock->Lock.State) >= kSpinWriter) sched_yield();
            if (__atomic_fetch_add(&lock->Lock.State, kSpinWriter, __ATOMIC_SEQ_CST) < kSpinWriter) break;
            __atomic_fetch_sub(&lock->Lock.State, kSpinWriter, __ATOMIC_SEQ_CST);
        }
        while (Load(lock->Lock.State) & kSpinReaders) sched_yield();
        lock->OwnerThread = thread;
    }
    lock->WriteEnterCount++;
}

constexpr GameLockFunctions kProtocol{
    CriticalSectionLock, CriticalSectionUnlock, CriticalSectionDestroy,
    KernelReadLock, KernelReadUnlock, KernelWriteLock, KernelWriteUnlock,
    SpinWriteLock,
};

// --- Helpers -----------------------------------------------------------------------

constexpr int kThreads = 8;
constexpr int kRounds = 20000;

// Runs `body(thread index)` on kThreads threads at once.
template <class F>
void OnThreads(F body)
{
    std::vector<std::thread> threads;
    std::atomic<bool> go{ false };
    for (int n = 0; n < kThreads; n++) {
        threads.emplace_back([&, n] {
            while (!go.load()) sched_yield();
            body(n);
        });
    }
    go.store(true);
    for (auto& thread : threads) thread.join();
}

// What a child process that runs `body` dies of (0 when it exits normally).
template <class F>
int SignalOf(F body)
{
    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        SetLogStream(nullptr);
        body();
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    return WIFSIGNALED(status) ? WTERMSIG(status) : 0;
}

// --- Tests -------------------------------------------------------------------------

void TestThreadId()
{
    CHECK(GetCurrentThreadId() == pthread_mach_thread_np(pthread_self()));
    CHECK(GetCurrentThreadId() != 0 && GetCurrentThreadId() != 0xffffffffu);
    DWORD other = 0;
    std::thread([&] { other = GetCurrentThreadId(); }).join();
    CHECK(other != 0 && other != GetCurrentThreadId());
}

void TestGate()
{
    CHECK(!GameLocksReady());

    // Creating a lock needs no game function and no gate.
    CRITICAL_SECTION cs;
    memset(&cs, 0xee, sizeof(cs));
    InitializeCriticalSection(&cs);
    CHECK(cs.Semaphore == 0 && cs.LockCount == -1 && cs.RecursionCount == 0 && cs.OwnerThread == 0 && cs.SpinCount == 4000);
    SRWLOCK srw;
    memset(&srw, 0xee, sizeof(srw));
    InitializeSRWLock(&srw);
    CHECK(srw.SharedSemaphore == 0 && srw.ExclusiveSemaphore == 0 && srw.State == 0);
    GameNoesisSpinLock noesis{ 7 };
    InitializeSRWLock(&noesis);
    CHECK(noesis.Value == 0);

    // Taking one while the gate is shut stops the process, and the lock is
    // not touched on the way.
    CHECK(SignalOf([&] { EnterCriticalSection(&cs); }) == SIGABRT);
    CHECK(SignalOf([&] { LeaveCriticalSection(&cs); }) == SIGABRT);
    CHECK(SignalOf([&] { TryEnterCriticalSection(&cs); }) == SIGABRT);
    CHECK(SignalOf([&] { DeleteCriticalSection(&cs); }) == SIGABRT);
    CHECK(SignalOf([&] { AcquireSRWLockShared(&srw); }) == SIGABRT);
    CHECK(SignalOf([&] { AcquireSRWLockExclusive(&srw); }) == SIGABRT);
    CHECK(SignalOf([&] { TryAcquireSRWLockShared(&srw); }) == SIGABRT);
    CHECK(SignalOf([&] { TryAcquireSRWLockExclusive(&srw); }) == SIGABRT);
    CHECK(SignalOf([&] { ReleaseSRWLockShared(&srw); }) == SIGABRT);
    CHECK(SignalOf([&] { ReleaseSRWLockExclusive(&srw); }) == SIGABRT);
    CHECK(SignalOf([&] { AcquireSRWLockExclusive(&noesis); }) == SIGABRT);
    CHECK(SignalOf([&] { ReleaseSRWLockShared(&noesis); }) == SIGABRT);
    CHECK(cs.LockCount == -1 && srw.State == 0 && noesis.Value == 0);
    auto lock = test::MakeSRWSpinLock();
    CHECK(SignalOf([&] { lock.WriteLock(lock.Object); }) == SIGABRT);
    lock.Destroy(lock.Object);

    // A process that is not the game has no game functions to find.
    CHECK(!ConfirmGameLocks() && !GameLocksReady());
}

void TestCriticalSection()
{
    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);

    EnterCriticalSection(&cs);
    CHECK(cs.LockCount == 0 && cs.RecursionCount == 1 && cs.OwnerThread == GetCurrentThreadId());
    EnterCriticalSection(&cs);                  // the owner enters again
    CHECK(cs.LockCount == 1 && cs.RecursionCount == 2);
    CHECK(TryEnterCriticalSection(&cs));
    CHECK(cs.LockCount == 2 && cs.RecursionCount == 3);
    bool entered = true;
    std::thread([&] { entered = TryEnterCriticalSection(&cs); }).join();
    CHECK(!entered && cs.LockCount == 2);       // another thread does not get in, and leaves no trace
    LeaveCriticalSection(&cs);
    LeaveCriticalSection(&cs);
    CHECK(cs.RecursionCount == 1 && cs.OwnerThread == GetCurrentThreadId());
    LeaveCriticalSection(&cs);
    CHECK(cs.LockCount == -1 && cs.RecursionCount == 0 && cs.OwnerThread == 0);

    // A try on a free lock takes it exactly as Lock() would.
    std::thread([&] {
        CHECK(TryEnterCriticalSection(&cs));
        CHECK(cs.LockCount == 0 && cs.RecursionCount == 1 && cs.OwnerThread == GetCurrentThreadId());
        LeaveCriticalSection(&cs);
    }).join();
    CHECK(cs.LockCount == -1);

    // Mutual exclusion: a counter only the lock protects, entered by every
    // way in.
    long counter = 0;
    std::atomic<long> tried{ 0 };
    OnThreads([&](int thread) {
        for (int n = 0; n < kRounds; n++) {
            if ((n + thread) % 3 == 0) {
                if (!TryEnterCriticalSection(&cs)) {
                    tried++;
                    EnterCriticalSection(&cs);
                }
            } else {
                EnterCriticalSection(&cs);
            }
            if (n % 5 == 0) {
                EnterCriticalSection(&cs);
                counter++;
                LeaveCriticalSection(&cs);
            } else {
                counter++;
            }
            LeaveCriticalSection(&cs);
        }
    });
    CHECK(counter == (long)kThreads * kRounds);
    CHECK(cs.LockCount == -1 && cs.RecursionCount == 0 && cs.OwnerThread == 0);

    DeleteCriticalSection(&cs);
    CHECK(cs.Semaphore == 0 && cs.LockCount == -1);
}

void TestKernelLock()
{
    SRWLOCK lock;
    InitializeSRWLock(&lock);

    AcquireSRWLockShared(&lock);
    CHECK(lock.State == 1);
    CHECK(TryAcquireSRWLockShared(&lock));              // readers share
    CHECK(lock.State == 2);
    CHECK(!TryAcquireSRWLockExclusive(&lock));          // a writer does not get in beside them
    CHECK(lock.State == 2);
    ReleaseSRWLockShared(&lock);
    ReleaseSRWLockShared(&lock);
    CHECK(lock.State == 0);

    CHECK(TryAcquireSRWLockExclusive(&lock));
    CHECK(lock.State == (kKernelExclusive | kKernelWriter));
    CHECK(!TryAcquireSRWLockShared(&lock) && !TryAcquireSRWLockExclusive(&lock));
    ReleaseSRWLockExclusive(&lock);
    CHECK(lock.State == 0);
    AcquireSRWLockExclusive(&lock);
    CHECK(lock.State == (kKernelExclusive | kKernelWriter));
    ReleaseSRWLockExclusive(&lock);

    // Two readers hold the lock at the same time.
    std::atomic<int> inside{ 0 };
    std::atomic<bool> both{ false };
    auto reader = [&] {
        AcquireSRWLockShared(&lock);
        inside++;
        for (int n = 0; n < 2000000 && !both.load(); n++) {
            if (inside.load() == 2) both.store(true);
            sched_yield();
        }
        ReleaseSRWLockShared(&lock);
    };
    std::thread first(reader), second(reader);
    first.join();
    second.join();
    CHECK(both.load() && lock.State == 0);

    // Writers exclude each other and the readers; readers see whole updates.
    long a = 0, b = 0;
    std::atomic<long> torn{ 0 };
    OnThreads([&](int thread) {
        for (int n = 0; n < kRounds; n++) {
            int kind = (n + thread) % 4;
            if (kind == 0) {
                AcquireSRWLockExclusive(&lock);
                a++;
                b++;
                ReleaseSRWLockExclusive(&lock);
            } else if (kind == 1 && TryAcquireSRWLockExclusive(&lock)) {
                a++;
                b++;
                ReleaseSRWLockExclusive(&lock);
            } else {
                if (kind != 2 || !TryAcquireSRWLockShared(&lock)) AcquireSRWLockShared(&lock);
                if (a != b) torn++;
                ReleaseSRWLockShared(&lock);
            }
        }
    });
    CHECK(torn.load() == 0 && a == b && a >= (long)kThreads * kRounds / 4);
    CHECK(lock.State == 0);
}

void TestNorbyteLock(test::NorbyteLock lock, bool kernel)
{
    // The class is the game's recursive lock: its owner and count are where
    // the game has them.
    auto owner = [&] {
        return kernel ? static_cast<GameRecursiveKernelLock*>(lock.Object)->OwnerThread
                      : static_cast<GameRecursiveSpinLock*>(lock.Object)->OwnerThread;
    };
    auto entries = [&] {
        return kernel ? static_cast<GameRecursiveKernelLock*>(lock.Object)->WriteEnterCount
                      : static_cast<GameRecursiveSpinLock*>(lock.Object)->WriteEnterCount;
    };
    auto state = [&] {
        return kernel ? static_cast<GameRecursiveKernelLock*>(lock.Object)->Lock.State
                      : static_cast<GameRecursiveSpinLock*>(lock.Object)->Lock.State;
    };
    CHECK(lock.Size == (kernel ? sizeof(GameRecursiveKernelLock) : sizeof(GameRecursiveSpinLock)));
    CHECK(owner() == kNoRecursiveOwner && entries() == 0 && state() == 0);

    lock.WriteLock(lock.Object);
    CHECK(owner() == GetCurrentThreadId() && entries() == 1);
    CHECK(state() == (kernel ? (kKernelExclusive | kKernelWriter) : kSpinWriter));
    lock.WriteLock(lock.Object);                // the writer enters again
    lock.ReadLock(lock.Object);                 // ... and may read
    CHECK(entries() == 2 && state() == (kernel ? (kKernelExclusive | kKernelWriter) : kSpinWriter));
    lock.ReadUnlock(lock.Object);
    lock.WriteUnlock(lock.Object);
    CHECK(owner() == GetCurrentThreadId() && entries() == 1);
    lock.WriteUnlock(lock.Object);
    CHECK(owner() == kNoRecursiveOwner && entries() == 0 && state() == 0);

    lock.ReadLock(lock.Object);
    CHECK(state() == 1 && owner() == kNoRecursiveOwner);
    lock.ReadUnlock(lock.Object);
    CHECK(state() == 0);

    long a = 0, b = 0;
    std::atomic<long> torn{ 0 };
    OnThreads([&](int thread) {
        for (int n = 0; n < kRounds; n++) {
            if ((n + thread) % 4 == 0) {
                lock.WriteLock(lock.Object);
                a++;
                if (n % 8 == 0) {
                    lock.WriteLock(lock.Object);
                    b++;
                    lock.WriteUnlock(lock.Object);
                } else {
                    b++;
                }
                lock.WriteUnlock(lock.Object);
            } else {
                lock.ReadLock(lock.Object);
                if (a != b) torn++;
                lock.ReadUnlock(lock.Object);
            }
        }
    });
    CHECK(torn.load() == 0 && a == b && a == (long)kThreads * kRounds / 4);
    CHECK(owner() == kNoRecursiveOwner && entries() == 0 && state() == 0);
    lock.Destroy(lock.Object);
}

void TestNoesisLock()
{
    GameNoesisSpinLock lock;
    InitializeSRWLock(&lock);
    AcquireSRWLockExclusive(&lock);
    CHECK(lock.Value != 0);
    ReleaseSRWLockExclusive(&lock);
    CHECK(lock.Value == 0);

    // The owner protocol of Noesis's reflection lock, as Norbyte's
    // Reflection::RegisterType runs it.
    std::atomic<uint32_t> owner{ kNoNoesisOwner };
    uint32_t depth = 0;
    long counter = 0;
    auto enter = [&] {
        DWORD thread = GetCurrentThreadId();
        if (thread != owner.load()) {
            AcquireSRWLockExclusive(&lock);
            owner.store(thread);
        }
        return ++depth;
    };
    auto leave = [&](uint32_t locks) {
        --depth;
        if (locks == 1) {
            owner.store(kNoNoesisOwner);
            ReleaseSRWLockExclusive(&lock);
        }
    };
    OnThreads([&](int) {
        for (int n = 0; n < kRounds; n++) {
            uint32_t outer = enter();
            if (n % 4 == 0) {
                uint32_t inner = enter();       // registering a type from inside a registration
                counter++;
                leave(inner);
            } else {
                counter++;
            }
            leave(outer);
        }
    });
    CHECK(counter == (long)kThreads * kRounds && lock.Value == 0 && depth == 0);
}

}

int main()
{
    SetLogStream(nullptr);
    TestThreadId();
    TestGate();

    SetGameLockFunctionsForTest(&kProtocol);
    CHECK(GameLocksReady());
    TestCriticalSection();
    TestKernelLock();
    TestNorbyteLock(test::MakeSRWLock(), true);
    TestNorbyteLock(test::MakeSRWSpinLock(), false);
    TestNoesisLock();
    SetGameLockFunctionsForTest(nullptr);
    CHECK(!GameLocksReady());

    if (gFailures) {
        printf("%d checks failed\n", gFailures);
        return 1;
    }
    printf("lock tests passed\n");
    return 0;
}
