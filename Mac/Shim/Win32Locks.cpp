// The Win32 lock functions and GetCurrentThreadId. On Mac CRITICAL_SECTION
// and SRWLOCK are the locks of the Mac game (windows.h), and these functions
// take them as the game does (Layout/GameLockCalls.h, which also says what
// happens to a call the game's functions cannot serve).

#include "windows.h"

#include <Layout/GameLockCalls.h>

using namespace bg3se;

DWORD GetCurrentThreadId()
{
    return mac::CurrentThreadPort();
}

void InitializeCriticalSection(LPCRITICAL_SECTION section) { mac::CriticalSectionInitialize(section); }
void DeleteCriticalSection(LPCRITICAL_SECTION section) { mac::CriticalSectionDelete(section); }
void EnterCriticalSection(LPCRITICAL_SECTION section) { mac::CriticalSectionEnter(section); }
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION section) { return mac::CriticalSectionTryEnter(section) ? TRUE : FALSE; }
void LeaveCriticalSection(LPCRITICAL_SECTION section) { mac::CriticalSectionLeave(section); }

void InitializeSRWLock(PSRWLOCK lock) { mac::InitializeKernelLock(lock); }
void AcquireSRWLockExclusive(PSRWLOCK lock) { mac::KernelWriteLock(lock); }
void AcquireSRWLockShared(PSRWLOCK lock) { mac::KernelReadLock(lock); }
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock) { return mac::KernelTryWriteLock(lock) ? TRUE : FALSE; }
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK lock) { return mac::KernelTryReadLock(lock) ? TRUE : FALSE; }
void ReleaseSRWLockExclusive(PSRWLOCK lock) { mac::KernelWriteUnlock(lock); }
void ReleaseSRWLockShared(PSRWLOCK lock) { mac::KernelReadUnlock(lock); }

// A Noesis lock has one side: readers take it as writers do.
void InitializeSRWLock(PNOESISLOCK lock) { mac::InitializeNoesisLock(lock); }
void AcquireSRWLockExclusive(PNOESISLOCK lock) { mac::NoesisLock(lock); }
void AcquireSRWLockShared(PNOESISLOCK lock) { mac::NoesisLock(lock); }
void ReleaseSRWLockExclusive(PNOESISLOCK lock) { mac::NoesisUnlock(lock); }
void ReleaseSRWLockShared(PNOESISLOCK lock) { mac::NoesisUnlock(lock); }
