#include <Layout/GameLockCalls.h>

#include <Layout/GameShapes.h>
#include <Platform/Log.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <libkern/OSAtomic.h>
#include <mutex>
#include <pthread.h>

namespace bg3se::mac {

namespace {

constexpr char const* kNames[GameLockCheck::Count] = {
    "ls::CriticalSection::Lock()",
    "ls::CriticalSection::Unlock()",
    "ls::CriticalSection::~CriticalSection()",
    "ls::SRWKernelLock::ReadLock()",
    "ls::SRWKernelLock::ReadUnlock()",
    "ls::SRWKernelLock::WriteLock()",
    "ls::SRWKernelLock::WriteUnlock()",
    "ls::RecursiveSRWLock<ls::SRWSpinLock>::WriteLock()",
};
constexpr char const* kSymbols[GameLockCheck::Count] = {
    "__ZN2ls15CriticalSection4LockEv",
    "__ZN2ls15CriticalSection6UnlockEv",
    "__ZN2ls15CriticalSectionD1Ev",
    "__ZN2ls13SRWKernelLock8ReadLockEv",
    "__ZN2ls13SRWKernelLock10ReadUnlockEv",
    "__ZN2ls13SRWKernelLock9WriteLockEv",
    "__ZN2ls13SRWKernelLock11WriteUnlockEv",
    "__ZN2ls16RecursiveSRWLockINS_11SRWSpinLockEE9WriteLockEv",
};
// Every shape a lock call relies on.
constexpr GameShape kShapes[] = {
    GameShape::CriticalSection, GameShape::SRWKernelLock, GameShape::RecursiveKernelLock, GameShape::RecursiveSpinLock,
    GameShape::NoesisLocks,
};
static_assert(sizeof(GameLockFunctions) == GameLockCheck::Count * sizeof(void*));

GameLockFunctions gFunctions{};
std::atomic<bool> gReady{ false };

// A lock call the gate does not allow. Does not return: see the header.
[[noreturn]] void Refuse(char const* call)
{
    Log(LogLevel::Error, "[Locks] %s was called while the game's lock functions are not confirmed; the process stops here, "
        "because going on without the lock would corrupt what it protects", call);
    abort();
}

inline GameLockFunctions const& Functions(char const* call)
{
    if (!gReady.load(std::memory_order_acquire)) Refuse(call);
    return gFunctions;
}

// The fields of a game lock are plain integers that the game's code updates
// with atomic instructions.
template <class T>
bool CompareExchange(T& field, T expected, T desired)
{
    return __atomic_compare_exchange_n(&field, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

}

uint32_t CurrentThreadPort()
{
    return pthread_mach_thread_np(pthread_self());
}

std::span<GameShape const> GameLockShapes()
{
    return kShapes;
}

GameLockCheck CheckGameLocks(GameImage const& image)
{
    GameLockCheck check;
    memcpy(check.Names, kNames, sizeof(kNames));

    auto shapes = CheckGameShapes(image);
    for (auto shape : kShapes) {
        if (shapes.Confirmed(shape)) continue;
        check.Problem = std::string("the game's ") + Describe(shape) + " is not confirmed";
        for (auto const& proof : shapes.Proofs) {
            if (proof.Shape == shape && !proof.Problem.empty()) {
                check.Problem += std::string(" (") + proof.Function + ": " + proof.Problem + ")";
                break;
            }
        }
        return check;
    }

    // The proofs have just required each of these symbols to hold the code
    // that was read.
    image.FindVAs(kSymbols, check.VAs);
    for (int n = 0; n < GameLockCheck::Count; n++) {
        if (!check.VAs[n] || check.VAs[n] < image.TextBegin() || check.VAs[n] >= image.TextEnd()) {
            check.Problem = std::string(kNames[n]) + " is not in the symbol table under the expected signature";
            return check;
        }
    }
    return check;
}

bool ConfirmGameLocks()
{
    static std::once_flag once;
    std::call_once(once, [] {
        auto image = GameImage::Current();
        if (!image) {
            Log(LogLevel::Error, "[Locks] the game's symbol table cannot be read, so its locks cannot be taken");
            return;
        }
        auto check = CheckGameLocks(*image);
        if (!check.Ok()) {
            Log(LogLevel::Error, "[Locks] the game's locks cannot be taken: %s", check.Problem.c_str());
            return;
        }
        void const* code[GameLockCheck::Count];
        for (int n = 0; n < GameLockCheck::Count; n++) code[n] = image->CodeAt(check.VAs[n]);
        memcpy(&gFunctions, code, sizeof(gFunctions));
        gReady.store(true, std::memory_order_release);
    });
    return GameLocksReady();
}

bool GameLocksReady()
{
    return gReady.load(std::memory_order_acquire);
}

void SetGameLockFunctionsForTest(GameLockFunctions const* functions)
{
    gReady.store(false, std::memory_order_release);
    if (!functions) return;
    gFunctions = *functions;
    gReady.store(true, std::memory_order_release);
}

// --- Critical section ---------------------------------------------------------

void CriticalSectionInitialize(GameCriticalSection* section)
{
    *section = kFreeCriticalSection;
}

void CriticalSectionDelete(GameCriticalSection* section)
{
    Functions("DeleteCriticalSection").CriticalSectionDestroy(section);
}

void CriticalSectionEnter(GameCriticalSection* section)
{
    Functions("EnterCriticalSection").CriticalSectionLock(section);
}

bool CriticalSectionTryEnter(GameCriticalSection* section)
{
    Functions("TryEnterCriticalSection");
    // The two ways ls::CriticalSection::Lock() gets the lock without waiting:
    // a free lock is taken by moving its count from -1 to 0, and the owner
    // enters again by adding to the count.
    uint32_t thread = CurrentThreadPort();
    if (CompareExchange(section->LockCount, kFreeCriticalSection.LockCount, 0)) {
        section->OwnerThread = thread;
        section->RecursionCount = 1;
        return true;
    }
    if (__atomic_load_n(&section->OwnerThread, __ATOMIC_RELAXED) == thread) {
        __atomic_fetch_add(&section->LockCount, 1, __ATOMIC_SEQ_CST);
        section->RecursionCount++;
        return true;
    }
    return false;
}

void CriticalSectionLeave(GameCriticalSection* section)
{
    Functions("LeaveCriticalSection").CriticalSectionUnlock(section);
}

// --- Kernel reader-writer lock ---------------------------------------------------

void InitializeKernelLock(GameSRWKernelLock* lock)
{
    *lock = {};
}

void KernelReadLock(GameSRWKernelLock* lock)
{
    Functions("AcquireSRWLockShared").KernelReadLock(lock);
}

bool KernelTryReadLock(GameSRWKernelLock* lock)
{
    Functions("TryAcquireSRWLockShared");
    // The first step of ls::SRWKernelLock::ReadLock(): while no writer holds
    // or waits for the lock, one more reader.
    for (;;) {
        uint32_t state = __atomic_load_n(&lock->State, __ATOMIC_SEQ_CST);
        if (state & kKernelWriters) return false;
        if (CompareExchange(lock->State, state, state + 1)) return true;
    }
}

void KernelReadUnlock(GameSRWKernelLock* lock)
{
    Functions("ReleaseSRWLockShared").KernelReadUnlock(lock);
}

void KernelWriteLock(GameSRWKernelLock* lock)
{
    Functions("AcquireSRWLockExclusive").KernelWriteLock(lock);
}

bool KernelTryWriteLock(GameSRWKernelLock* lock)
{
    Functions("TryAcquireSRWLockExclusive");
    // The first step of ls::SRWKernelLock::WriteLock(): a free lock becomes
    // one writer in exclusive mode.
    return CompareExchange(lock->State, 0u, kKernelExclusive | kKernelWriter);
}

void KernelWriteUnlock(GameSRWKernelLock* lock)
{
    Functions("ReleaseSRWLockExclusive").KernelWriteUnlock(lock);
}

// --- Recursive spin lock ---------------------------------------------------------

void GameSpinWriteLock(GameRecursiveSpinLock* lock)
{
    Functions("SRWSpinLock::WriteLock").SpinWriteLock(lock);
}

// --- Noesis locks ------------------------------------------------------------------

void InitializeNoesisLock(GameNoesisSpinLock* lock)
{
    lock->Value = 0;
}

// OSSpinLock is deprecated in favour of os_unfair_lock, which is a different
// lock: these words are taken by the game's Noesis with OSSpinLockLock and
// OSSpinLockUnlock, and only the same two functions work with it on them.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static_assert(sizeof(OSSpinLock) == sizeof(GameNoesisSpinLock));

void NoesisLock(GameNoesisSpinLock* lock)
{
    Functions("AcquireSRWLock on a Noesis lock");
    OSSpinLockLock(&lock->Value);
}

void NoesisUnlock(GameNoesisSpinLock* lock)
{
    Functions("ReleaseSRWLock on a Noesis lock");
    OSSpinLockUnlock(&lock->Value);
}
#pragma clang diagnostic pop

}
