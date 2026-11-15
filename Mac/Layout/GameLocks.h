#pragma once

#include <cstddef>
#include <cstdint>

// The locks the Mac game embeds in its structures, as the game lays them out.
// Norbyte's definitions name them by their Windows types (CRITICAL_SECTION,
// SRWLOCK), which Mac/Shim/windows.h defines as these.
//
// Each is a plain aggregate without initialisers, as the Windows types are,
// so that a structure holding one keeps the properties (trivial, copyable) it
// has on Windows; the fields are read and written atomically by the code that
// takes the locks (Layout/GameLockProtocol.h). A field that is stated here is
// stated nowhere else: the checks against the game's code
// (Layout/GameShapes.cpp) compare what the game's functions do with offsetof
// and sizeof of these types.

namespace bg3se::mac {

// ls::CriticalSection: a recursive mutex. A free one is
// { 0, -1, 0, 0, spin count }.
struct GameCriticalSection
{
    uint32_t Semaphore;         // Mach semaphore the waiters sleep on; 0 until a thread first has to wait
    int32_t LockCount;          // -1 when free; each thread that holds or wants the lock adds 1
    int32_t RecursionCount;     // how many times the owner has entered
    uint32_t OwnerThread;       // Mach thread port of the owner; 0 when free
    int32_t SpinCount;          // attempts before a waiter sleeps
};
inline constexpr GameCriticalSection kFreeCriticalSection{ 0, -1, 0, 0, 4000 };
static_assert(sizeof(GameCriticalSection) == 0x14 && alignof(GameCriticalSection) == 4);
static_assert(offsetof(GameCriticalSection, Semaphore) == 0 && offsetof(GameCriticalSection, LockCount) == 4
    && offsetof(GameCriticalSection, RecursionCount) == 8 && offsetof(GameCriticalSection, OwnerThread) == 0xc
    && offsetof(GameCriticalSection, SpinCount) == 0x10);

// ls::SRWKernelLock: a reader-writer lock, not recursive. A free one is all zero.
struct alignas(8) GameSRWKernelLock
{
    uint32_t SharedSemaphore;       // Mach semaphore waiting readers sleep on; 0 until first needed
    uint32_t ExclusiveSemaphore;    // Mach semaphore waiting writers sleep on; 0 until first needed
    uint32_t State;                 // readers in bits 0-15, writers holding or waiting in bits 16-30, bit 31 with a writer
    uint32_t Unused;
};
constexpr uint32_t kKernelReaders = 0xffff;             // State: how many readers hold the lock
constexpr uint32_t kKernelWriter = 0x10000;             // State: one writer holding or waiting
constexpr uint32_t kKernelWriters = 0x7fff0000;         // State: all of them
constexpr uint32_t kKernelExclusive = 0x80000000;       // State: the lock is in exclusive mode
static_assert(sizeof(GameSRWKernelLock) == 0x10 && alignof(GameSRWKernelLock) == 8);
static_assert(offsetof(GameSRWKernelLock, SharedSemaphore) == 0 && offsetof(GameSRWKernelLock, ExclusiveSemaphore) == 4
    && offsetof(GameSRWKernelLock, State) == 8);

// The owner word of the two recursive wrappers when no thread holds the
// write side. A holder is its Mach thread port, zero-extended.
constexpr uint64_t kNoRecursiveOwner = ~0ull;

// ls::RecursiveSRWLock<ls::SRWKernelLock>: the kernel lock, which the thread
// that holds its write side may enter again.
struct GameRecursiveKernelLock
{
    GameSRWKernelLock Lock;
    uint64_t OwnerThread;       // kNoRecursiveOwner, or the writer's thread port
    uint32_t WriteEnterCount;   // how many times the writer has entered
};
static_assert(sizeof(GameRecursiveKernelLock) == 0x20 && alignof(GameRecursiveKernelLock) == 8);
static_assert(offsetof(GameRecursiveKernelLock, OwnerThread) == 0x10 && offsetof(GameRecursiveKernelLock, WriteEnterCount) == 0x18);

// ls::SRWSpinLock: readers in bits 0-19, writers holding or wanting the lock
// in units of kSpinWriter above them.
struct GameSRWSpinLock
{
    uint32_t State;
};
constexpr uint32_t kSpinWriter = 0x100000;
constexpr uint32_t kSpinReaders = kSpinWriter - 1;

// ls::RecursiveSRWLock<ls::SRWSpinLock>.
struct GameRecursiveSpinLock
{
    GameSRWSpinLock Lock;
    uint64_t OwnerThread;       // kNoRecursiveOwner, or the writer's thread port
    uint32_t WriteEnterCount;
};
static_assert(sizeof(GameRecursiveSpinLock) == 0x18 && alignof(GameRecursiveSpinLock) == 8);
static_assert(offsetof(GameRecursiveSpinLock, OwnerThread) == 8 && offsetof(GameRecursiveSpinLock, WriteEnterCount) == 0x10);

// The game's ls::RecursiveSRWLock<ls::SRWSpinLock>::WriteLock() on `lock`
// (Layout/GameLockCalls.h). The other operations of this lock are inline in
// the game's code and in Norbyte's (GameDefinitions/Base/Lock.inl).
void GameSpinWriteLock(GameRecursiveSpinLock* lock);

// The locks of the game's NoesisGUI: an OSSpinLock, 0 when free, taken with
// OSSpinLockLock and OSSpinLockUnlock. Noesis has no reader side.
struct GameNoesisSpinLock
{
    int32_t Value;
};
static_assert(sizeof(GameNoesisSpinLock) == 4 && alignof(GameNoesisSpinLock) == 4);
// The owner word of Noesis's reflection lock when no thread holds it. A
// holder is its Mach thread port.
constexpr uint32_t kNoNoesisOwner = 0xffffffffu;
// Noesis::TypeClass keeps its ancestors in an object of this size: whether
// they are collapsed in the byte at +0, the vector of them at +8, its lock
// at +0x18.
constexpr size_t kNoesisAncestorsSize = 0x20;
constexpr size_t kNoesisAncestorsCollapsed = 0x00;
constexpr size_t kNoesisAncestorsVector = 0x08;
constexpr size_t kNoesisAncestorsLock = 0x18;

}
