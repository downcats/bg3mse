#pragma once

#include <cstdint>
#include <span>
#include <string>

#include <Layout/GameLocks.h>
#include <Layout/GameShapes.h>
#include <Platform/Symbols.h>

// Taking the game's locks. A lock inside a game structure is shared with the
// game's own threads, so it is taken the way the game takes it: through the
// game's own functions where the game has them as functions, and by the same
// steps on the same fields where the game's code has them inline.
//
//   critical section     Lock, Unlock and the destructor are game functions.
//                        Creating one and trying to enter one are inline in
//                        the game: a free one is kFreeCriticalSection, and a
//                        try is the first step of Lock().
//   kernel lock          ReadLock, ReadUnlock, WriteLock and WriteUnlock are
//                        game functions. A free one is all zero; a try is
//                        the first step of ReadLock() or WriteLock().
//   recursive spin lock  WriteLock is a game function.
//   Noesis locks         OSSpinLockLock and OSSpinLockUnlock, which the
//                        game's Noesis imports for them.
//
// The gate. ConfirmGameLocks() resolves the game's functions by their exact
// symbols and requires every lock shape of Layout/GameShapes.h to be
// confirmed. Until it has succeeded, nothing here touches a lock. Creating a
// lock is the exception: it only writes the fields of a lock nobody else can
// see yet, and it runs before the gate (a global of Norbyte's is constructed
// with one).
//
// A lock call cannot report failure: Windows gives EnterCriticalSection no
// way to fail, and Norbyte's code goes on to use what the lock protects. So
// a lock call made while the gate is not open stops the process, with one
// line in the log saying which call and why. Returning without the lock
// would let the caller write to structures the game's threads are using.
// This cannot happen to a correct start-up: the entry point must ask
// GameLocksReady() first and not start the Script Extender when the answer
// is no. The stop is what stands behind that rule.

namespace bg3se::mac {

struct GameLockFunctions
{
    void (*CriticalSectionLock)(GameCriticalSection*);
    void (*CriticalSectionUnlock)(GameCriticalSection*);
    void (*CriticalSectionDestroy)(GameCriticalSection*);
    void (*KernelReadLock)(GameSRWKernelLock*);
    void (*KernelReadUnlock)(GameSRWKernelLock*);
    void (*KernelWriteLock)(GameSRWKernelLock*);
    void (*KernelWriteUnlock)(GameSRWKernelLock*);
    void (*SpinWriteLock)(GameRecursiveSpinLock*);
};

// What the gate needs of an image: the lock shapes confirmed and the eight
// functions found. Runs on the loaded game and on a binary mapped from disk.
struct GameLockCheck
{
    static constexpr int Count = 8;
    char const* Names[Count]{};     // readable names, in the order of GameLockFunctions
    uint64_t VAs[Count]{};          // link-time addresses; 0 when not found
    std::string Problem;            // empty when the gate would open

    bool Ok() const { return Problem.empty(); }
};
GameLockCheck CheckGameLocks(GameImage const& image);

// Opens the gate for this process from the running game. Returns false, with
// the reason logged once, when the game's image cannot be read, a shape is
// not confirmed or a function is missing; the gate then stays shut.
bool ConfirmGameLocks();
bool GameLocksReady();

// For tests: opens the gate on functions that implement the game's protocol.
// Passing nullptr shuts it again.
void SetGameLockFunctionsForTest(GameLockFunctions const* functions);

// The id the game writes into the owner words of its locks: the calling
// thread's Mach port. This is GetCurrentThreadId() on Mac.
uint32_t CurrentThreadPort();

// The shapes a lock call relies on: ConfirmGameLocks shuts the gate, and the
// extender does not start, while one of them is not confirmed.
std::span<GameShape const> GameLockShapes();

// The operations behind the Win32 lock names (Mac/Shim/Win32Locks.cpp). They
// are not named as the Win32 functions are: the lock types live in this
// namespace, so a call of a Win32 name would find a function of that name
// here as well.
void CriticalSectionInitialize(GameCriticalSection* section);
void CriticalSectionDelete(GameCriticalSection* section);
void CriticalSectionEnter(GameCriticalSection* section);
bool CriticalSectionTryEnter(GameCriticalSection* section);
void CriticalSectionLeave(GameCriticalSection* section);

void InitializeKernelLock(GameSRWKernelLock* lock);
void KernelReadLock(GameSRWKernelLock* lock);
bool KernelTryReadLock(GameSRWKernelLock* lock);
void KernelReadUnlock(GameSRWKernelLock* lock);
void KernelWriteLock(GameSRWKernelLock* lock);
bool KernelTryWriteLock(GameSRWKernelLock* lock);
void KernelWriteUnlock(GameSRWKernelLock* lock);

void InitializeNoesisLock(GameNoesisSpinLock* lock);
void NoesisLock(GameNoesisSpinLock* lock);
void NoesisUnlock(GameNoesisSpinLock* lock);

}
