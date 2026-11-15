// The Detours transaction API on entry hooks (detours.h says what holds).
//
// Built as its own library, bg3mse_detours, on the platform layer: it needs
// the Windows names of Mac/Shim and nothing of the Script Extender, and
// bg3mse_platform, which the camera module also links, stays free of both.

#include <detours.h>

#include <Platform/CodeWriter.h>
#include <Platform/EntryHook.h>
#include <Platform/FunctionExtents.h>
#include <Platform/HookPlanner.h>
#include <Platform/Log.h>

#include <atomic>
#include <cstdarg>
#include <cstring>
#include <new>
#include <pthread.h>
#include <string>
#include <vector>

using namespace bg3se::mac;

namespace {

// A function this API has hooked at some time: its hook, its own first
// instruction, and the detour of its last committed attach (nullptr once
// detached). Never freed: the hook layer keeps the hook, and a later attach
// uses it again.
struct Attached
{
    EntryHook* Hook;
    uint32_t First;
    void* Detour;
    Attached* Next;
};

enum class Kind
{
    Attach,
    Detach
};

struct Operation
{
    Kind What;
    void** Pointer;         // the caller's ppPointer
    void* Target;           // the function
    void* Detour;
    EntryHook* Hook;
    uint32_t First;         // the function's own first instruction
    Attached* Record;       // an existing record, or one made for this attach
    bool FreshRecord;
};

struct Transaction
{
    std::vector<Operation> Operations;
    LONG Error{ NO_ERROR };
    void** ErrorPointer{ nullptr };
};

// Constant-initialised: a dylib constructor may open a transaction before
// this file's dynamic initialisers have run. gOwner is the thread id of the
// transaction's owner, 0 when none is open; the rest is touched only by the
// owner, and handed from one owner to the next by gOwner's release and acquire.
constinit std::atomic<uint64_t> gOwner{ 0 };
constinit Transaction* gTransaction = nullptr;
constinit Attached* gAttached = nullptr;

uint64_t ThisThread()
{
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return id;
}

// The open transaction, when the calling thread owns it.
Transaction* Owned()
{
    return gOwner.load(std::memory_order_acquire) == ThisThread() ? gTransaction : nullptr;
}

void Close()
{
    for (auto const& op : gTransaction->Operations) {
        if (op.FreshRecord) delete op.Record;
    }
    delete gTransaction;
    gTransaction = nullptr;
    gOwner.store(0, std::memory_order_release);
}

// Records the first failure of the transaction, which commit then returns.
LONG Fail(Transaction& tx, LONG error, void** pointer, char const* call, std::string const& why)
{
    Log(LogLevel::Error, "%s: %s", call, why.c_str());
    if (tx.Error == NO_ERROR) {
        tx.Error = error;
        tx.ErrorPointer = pointer;
    }
    return error;
}

std::string Text(char const* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Text(char const* fmt, ...)
{
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    return text;
}

void Publish(void** pointer, void* value)
{
    __atomic_store_n(pointer, value, __ATOMIC_RELEASE);
}

uint32_t LiveWord(void const* at)
{
    return __atomic_load_n(static_cast<uint32_t const*>(at), __ATOMIC_RELAXED);
}

uint32_t PatchOf(EntryHook const* hook)
{
    return EncodeBranch(reinterpret_cast<uintptr_t>(hook->Target()), reinterpret_cast<uintptr_t>(hook->Island()), false);
}

// The replacement the island's far jump goes to (ldr x16, #8; br x16; .quad replacement).
void* IslandDestination(EntryHook const* hook)
{
    uint64_t literal;
    memcpy(&literal, static_cast<uint8_t const*>(hook->Island()) + 8, sizeof(literal));
    return reinterpret_cast<void*>(literal);
}

Attached* FindAttached(EntryHook const* hook)
{
    for (Attached* a = gAttached; a; a = a->Next) {
        if (a->Hook == hook) return a;
    }
    return nullptr;
}

bool Queued(Transaction const& tx, void const* target)
{
    for (auto const& op : tx.Operations) {
        if (op.Target == target) return true;
    }
    return false;
}

// Why the code of an operation is not as the transaction left it, or empty.
std::string Changed(Operation const& op)
{
    uint32_t live = LiveWord(op.Target);
    if (op.What == Kind::Attach) {
        if (op.Hook->Committed()) return Text("%p carries a committed hook", op.Target);
        if (live != op.First) return Text("the first instruction of %p changed since it was attached", op.Target);
        if (IslandDestination(op.Hook) != op.Detour) return Text("the hook on %p was re-targeted since it was attached", op.Target);
    } else {
        if (!op.Hook->Committed() || live != PatchOf(op.Hook)) return Text("%p no longer carries its hook", op.Target);
    }
    return {};
}

// Takes a hook of a failed commit out again: its patch, live, is replaced by
// the original first instruction. Through the hook layer when it counts the
// hook as committed; otherwise its write went live without passing its checks.
void TakeOut(Operation const& op)
{
    if (LiveWord(op.Target) != PatchOf(op.Hook)) return;
    std::string problem = op.Hook->Committed() ? RemoveEntryHook(op.Hook) : WriteCodeWord(op.Target, op.First);
    if (!problem.empty()) Log(LogLevel::Error, "DetourTransactionCommit: taking the hook on %p out again failed: %s", op.Target, problem.c_str());
}

}

extern "C" {

LONG WINAPI DetourTransactionBegin(void)
{
    uint64_t self = ThisThread();
    uint64_t none = 0;
    if (!gOwner.compare_exchange_strong(none, self, std::memory_order_acquire, std::memory_order_relaxed)) {
        Log(LogLevel::Error, "DetourTransactionBegin: a transaction is already open");
        return ERROR_INVALID_OPERATION;
    }
    gTransaction = new (std::nothrow) Transaction;
    if (!gTransaction) {
        gOwner.store(0, std::memory_order_release);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    return NO_ERROR;
}

LONG WINAPI DetourTransactionAbort(void)
{
    if (!Owned()) return ERROR_INVALID_OPERATION;
    Close();
    return NO_ERROR;
}

LONG WINAPI DetourUpdateThread(HANDLE)
{
    // Nothing to do: a hook changes one instruction word (detours.h).
    Transaction* tx = Owned();
    return tx ? tx->Error : NO_ERROR;
}

LONG WINAPI DetourAttachEx(PVOID* ppPointer, PVOID pDetour, PDETOUR_TRAMPOLINE* ppRealTrampoline, PVOID* ppRealTarget, PVOID* ppRealDetour)
{
    constexpr char kCall[] = "DetourAttach";
    Transaction* tx = Owned();
    if (!tx) {
        Log(LogLevel::Error, "%s: the calling thread has no open transaction", kCall);
        return ERROR_INVALID_OPERATION;
    }
    if (tx->Error != NO_ERROR) return tx->Error;
    if (!ppPointer || !*ppPointer || !pDetour) return Fail(*tx, ERROR_INVALID_HANDLE, ppPointer, kCall, "no function or no detour");
    void* target = *ppPointer;
    if (Queued(*tx, target)) {
        return Fail(*tx, ERROR_INVALID_BLOCK, ppPointer, kCall, Text("%p is already attached or detached in this transaction", target));
    }
    auto extent = FindLoadedFunction(target);
    if (!extent.Ok()) return Fail(*tx, ERROR_INVALID_BLOCK, ppPointer, kCall, Text("no hook on %p: %s", target, Describe(extent.Refusal)));
    try {
        tx->Operations.reserve(tx->Operations.size() + 1);
    } catch (std::bad_alloc const&) {
        return Fail(*tx, ERROR_NOT_ENOUGH_MEMORY, ppPointer, kCall, "out of memory");
    }

    void* trampoline = nullptr;
    std::string problem;
    HookRefusal refusal = HookRefusal::None;
    EntryHook* hook = PrepareEntryHook(target, extent.Bytes(), pDetour, &trampoline, problem, &refusal);
    if (!hook) {
        LONG error = refusal == HookRefusal::NoIsland ? ERROR_NOT_ENOUGH_MEMORY : refusal == HookRefusal::None ? ERROR_INVALID_DATA : ERROR_INVALID_BLOCK;
        return Fail(*tx, error, ppPointer, kCall, problem);
    }
    // The hook layer has just checked that this is still the function's own
    // first instruction (a hook that existed is re-targeted only then).
    uint32_t first = LiveWord(target);
    Attached* record = FindAttached(hook);
    bool fresh = !record;
    if (fresh) {
        record = new (std::nothrow) Attached{ hook, first, nullptr, nullptr };
        if (!record) return Fail(*tx, ERROR_NOT_ENOUGH_MEMORY, ppPointer, kCall, "out of memory");
    }
    tx->Operations.push_back({ Kind::Attach, ppPointer, target, pDetour, hook, first, record, fresh });
    if (ppRealTrampoline) *ppRealTrampoline = static_cast<PDETOUR_TRAMPOLINE>(trampoline);
    if (ppRealTarget) *ppRealTarget = target;
    if (ppRealDetour) *ppRealDetour = pDetour;
    return NO_ERROR;
}

LONG WINAPI DetourAttach(PVOID* ppPointer, PVOID pDetour)
{
    return DetourAttachEx(ppPointer, pDetour, nullptr, nullptr, nullptr);
}

LONG WINAPI DetourDetach(PVOID* ppPointer, PVOID pDetour)
{
    constexpr char kCall[] = "DetourDetach";
    Transaction* tx = Owned();
    if (!tx) {
        Log(LogLevel::Error, "%s: the calling thread has no open transaction", kCall);
        return ERROR_INVALID_OPERATION;
    }
    if (tx->Error != NO_ERROR) return tx->Error;
    if (!ppPointer || !*ppPointer || !pDetour) return Fail(*tx, ERROR_INVALID_HANDLE, ppPointer, kCall, "no trampoline or no detour");
    void* trampoline = *ppPointer;
    Attached* record = nullptr;
    for (Attached* a = gAttached; a; a = a->Next) {
        if (a->Hook->Trampoline() == trampoline) record = a;
    }
    if (!record || !record->Detour || !record->Hook->Committed()) {
        return Fail(*tx, ERROR_INVALID_BLOCK, ppPointer, kCall, Text("%p is not the trampoline of a committed attach", trampoline));
    }
    void* target = record->Hook->Target();
    if (record->Detour != pDetour) {
        return Fail(*tx, ERROR_INVALID_BLOCK, ppPointer, kCall, Text("%p was attached with the detour %p, not %p", target, record->Detour, pDetour));
    }
    if (Queued(*tx, target)) {
        return Fail(*tx, ERROR_INVALID_BLOCK, ppPointer, kCall, Text("%p is already attached or detached in this transaction", target));
    }
    try {
        tx->Operations.push_back({ Kind::Detach, ppPointer, target, pDetour, record->Hook, record->First, record, false });
    } catch (std::bad_alloc const&) {
        return Fail(*tx, ERROR_NOT_ENOUGH_MEMORY, ppPointer, kCall, "out of memory");
    }
    return NO_ERROR;
}

LONG WINAPI DetourTransactionCommitEx(PVOID** pppFailedPointer)
{
    if (pppFailedPointer) *pppFailedPointer = nullptr;
    Transaction* tx = Owned();
    if (!tx) {
        Log(LogLevel::Error, "DetourTransactionCommit: the calling thread has no open transaction");
        return ERROR_INVALID_OPERATION;
    }
    auto& ops = tx->Operations;
    auto fail = [&](LONG error, void** pointer) {
        if (pppFailedPointer) *pppFailedPointer = pointer;
        Close();
        return error;
    };
    if (tx->Error != NO_ERROR) {
        Log(LogLevel::Error, "DetourTransactionCommit: nothing committed: an attach or detach of the transaction failed");
        return fail(tx->Error, tx->ErrorPointer);
    }
    for (auto const& op : ops) {
        if (std::string why = Changed(op); !why.empty()) {
            Log(LogLevel::Error, "DetourTransactionCommit: nothing committed: %s", why.c_str());
            return fail(ERROR_INVALID_DATA, op.Pointer);
        }
    }

    std::vector<EntryHook*> attaches;
    std::vector<Operation const*> detached;
    std::string problem;
    void** failedPointer = nullptr;
    try {
        attaches.reserve(ops.size());
        detached.reserve(ops.size());
    } catch (std::bad_alloc const&) {
        Log(LogLevel::Error, "DetourTransactionCommit: nothing committed: out of memory");
        return fail(ERROR_NOT_ENOUGH_MEMORY, nullptr);
    }
    for (auto const& op : ops) {
        if (op.What == Kind::Attach) {
            attaches.push_back(op.Hook);
            continue;
        }
        problem = RemoveEntryHook(op.Hook);
        if (!problem.empty()) {
            failedPointer = op.Pointer;
            break;
        }
        detached.push_back(&op);
    }
    if (problem.empty() && !attaches.empty()) {
        // Before the hooks go live: a detour calling through *ppPointer reaches the trampoline.
        for (auto const& op : ops) {
            if (op.What == Kind::Attach) Publish(op.Pointer, op.Hook->Trampoline());
        }
        problem = CommitEntryHooks(attaches);
        if (!problem.empty()) {
            for (auto const& op : ops) {
                if (op.What == Kind::Attach && LiveWord(op.Target) != PatchOf(op.Hook) && !failedPointer) failedPointer = op.Pointer;
            }
        }
    }

    if (!problem.empty()) {
        // Back to how the transaction found things, as far as that goes.
        for (auto const& op : ops) {
            if (op.What == Kind::Attach) TakeOut(op);
        }
        for (auto const* op : detached) {
            if (std::string why = CommitEntryHook(op->Hook); !why.empty()) {
                Log(LogLevel::Error, "DetourTransactionCommit: putting the hook on %p back failed: %s", op->Target, why.c_str());
            }
        }
        Log(LogLevel::Error, "DetourTransactionCommit: failed: %s", problem.c_str());
        bool changed = false;
        for (auto& op : ops) {
            uint32_t live = LiveWord(op.Target);
            bool hooked = live == PatchOf(op.Hook);
            if (!hooked && live != op.First) {
                // Neither the hook's branch nor the function's own instruction: changed by someone else.
                changed = true;
                if (!failedPointer) failedPointer = op.Pointer;
            }
            if (hooked) {
                // Left in, or put back: as good as committed. *ppPointer is the trampoline.
                op.Record->Detour = op.Detour;
                if (op.FreshRecord) {
                    op.Record->Next = gAttached;
                    gAttached = op.Record;
                    op.FreshRecord = false;
                }
            } else {
                Publish(op.Pointer, op.Target);
                if (op.What == Kind::Detach) op.Record->Detour = nullptr;
            }
            char const* state = hooked ? (op.What == Kind::Attach ? "hooked: the attach went in" : "hooked: the detach did not go in")
                                       : (op.What == Kind::Attach ? "as it was: the attach did not go in" : "unhooked: the detach went in");
            Log(LogLevel::Error, "DetourTransactionCommit: %s %p: %s", op.What == Kind::Attach ? "attach" : "detach", op.Target, state);
        }
        return fail(changed ? ERROR_INVALID_DATA : ERROR_NOT_ENOUGH_MEMORY, failedPointer);
    }

    for (auto& op : ops) {
        if (op.What == Kind::Detach) {
            Publish(op.Pointer, op.Target);
            op.Record->Detour = nullptr;
            continue;
        }
        op.Record->Detour = op.Detour;
        if (op.FreshRecord) {
            op.Record->Next = gAttached;
            gAttached = op.Record;
            op.FreshRecord = false;
        }
    }
    Close();
    return NO_ERROR;
}

LONG WINAPI DetourTransactionCommit(void)
{
    return DetourTransactionCommitEx(nullptr);
}

}
