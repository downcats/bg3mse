// The Detours transaction API that upstream's code uses, for the macOS build,
// on the entry hooks of the platform layer (Platform/EntryHook.h).
#pragma once

#include <windows.h>

// The error codes the functions below return, as winerror.h spells them.
#ifndef NO_ERROR
#define NO_ERROR 0L
#endif
#ifndef ERROR_INVALID_HANDLE
#define ERROR_INVALID_HANDLE 6L
#endif
#ifndef ERROR_NOT_ENOUGH_MEMORY
#define ERROR_NOT_ENOUGH_MEMORY 8L
#endif
#ifndef ERROR_INVALID_BLOCK
#define ERROR_INVALID_BLOCK 9L
#endif
#ifndef ERROR_INVALID_DATA
#define ERROR_INVALID_DATA 13L
#endif
#ifndef ERROR_INVALID_OPERATION
#define ERROR_INVALID_OPERATION 4317L
#endif

// What each function does here, and where that differs from Detours:
//
// DetourTransactionBegin opens a transaction owned by the calling thread. One
// transaction is open at a time: Begin while another is open, on any thread,
// returns ERROR_INVALID_OPERATION and does not wait. Attach, detach, commit
// and abort from a thread that does not own the open transaction (or with
// none open) return ERROR_INVALID_OPERATION.
//
// DetourAttach / DetourAttachEx queue a hook on the function *ppPointer
// points to, by the time the call returns:
//   - the function must start where *ppPointer points, in an image dyld has
//     loaded: its extent comes from Platform/FunctionExtents.h. Code outside
//     any image, an address inside a function, and a dyld stub are refused.
//     No import jump is followed: *ppRealTarget receives *ppPointer itself and
//     *ppRealDetour receives pDetour.
//   - the trampoline is built at once and *ppRealTrampoline receives it: it
//     can be called from then on and behaves as the original function, whether
//     the transaction is later committed or aborted, and it is never freed.
//     A PDETOUR_TRAMPOLINE here points at that code, as upstream's
//     WrappedFunction uses it; there is no structure behind it.
//   - *ppPointer becomes the trampoline at commit. It is written before the
//     hook goes live, so a detour that calls through *ppPointer never reaches
//     itself, and written back if the hook does not go in.
//   - one hook per function, with no chaining: a function that already carries
//     a committed hook (from this API or any other bg3mse code), or that this
//     transaction already attaches or detaches, is refused.
//   - the hook replaces the function's first instruction by a branch to an
//     island near it, whose far jump to the detour uses x16. A function whose
//     code may read x16 or x17 before writing it (one that takes a value in
//     them at its entry) is refused.
//
// DetourDetach queues the removal of the hook *ppPointer names: *ppPointer
// must be the trampoline a committed attach left there, and pDetour that
// attach's detour. At commit the function's first instruction is written
// back, then *ppPointer is set to the function again. Island and trampoline
// stay (a thread may still be in them), and a later attach on the function,
// with any detour, uses them again.
//
// DetourTransactionCommit / DetourTransactionCommitEx make the queued changes.
// When an attach or detach of the transaction failed, the transaction has
// failed: every later attach or detach in it returns that error and queues
// nothing, and commit makes none of the transaction's changes, closes it and
// returns that error (CommitEx gives the failed call's ppPointer). Otherwise every function's code and island is checked
// first; a function or island changed since its attach or detach fails the
// commit with ERROR_INVALID_DATA and nothing changed. Then the detaches are
// made, then the attaches (those whose functions start on one page with one
// write). If a write fails, what went in is taken out again; the log names
// every function of the transaction and says whether it is left hooked (with
// a working trampoline) or as it was, which also holds should taking a hook
// out fail. A failed write returns ERROR_NOT_ENOUGH_MEMORY (the system
// refused the mapping it needs), a function changed meanwhile
// ERROR_INVALID_DATA. The transaction is closed whatever the outcome.
//
// DetourTransactionAbort drops the queued changes and closes the transaction.
// No code is changed and *ppPointer was never written. Trampolines already
// handed out stay callable (Detours frees them).
//
// DetourUpdateThread does nothing. It returns the error of the caller's
// transaction when an attach or detach of it failed, NO_ERROR otherwise
// (Detours documents only ERROR_NOT_ENOUGH_MEMORY). Detours rewrites several
// instructions and must move threads that are in the middle of them; here a
// hook changes one instruction word, in one atomic page swap, so a thread
// runs either the old word or the new one and none needs moving.
//
// Errors: ERROR_INVALID_HANDLE for a null ppPointer, *ppPointer or pDetour;
// ERROR_INVALID_BLOCK for a function that cannot be hooked (not a function
// start, already hooked, refused by the planner of Platform/HookPlanner.h)
// and for a detach of something that is not a committed attach;
// ERROR_NOT_ENOUGH_MEMORY when no island is in range or memory runs out;
// ERROR_INVALID_DATA when a function changed under the transaction. The
// reason is written to the platform log.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _DETOUR_TRAMPOLINE DETOUR_TRAMPOLINE, *PDETOUR_TRAMPOLINE;

LONG WINAPI DetourTransactionBegin(void);
LONG WINAPI DetourTransactionAbort(void);
LONG WINAPI DetourTransactionCommit(void);
LONG WINAPI DetourTransactionCommitEx(PVOID** pppFailedPointer);

LONG WINAPI DetourUpdateThread(HANDLE hThread);

LONG WINAPI DetourAttach(PVOID* ppPointer, PVOID pDetour);
LONG WINAPI DetourAttachEx(PVOID* ppPointer, PVOID pDetour, PDETOUR_TRAMPOLINE* ppRealTrampoline, PVOID* ppRealTarget, PVOID* ppRealDetour);
LONG WINAPI DetourDetach(PVOID* ppPointer, PVOID pDetour);

#ifdef __cplusplus
}
#endif
