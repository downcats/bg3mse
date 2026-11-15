#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <Platform/HookPlanner.h>

// Entry hooks: a function's first instruction replaced by `b island`.
//
// The island, near the function (Platform/Islands.h), holds a far jump to the
// replacement (ldr x16, #8; br x16; .quad replacement): x16 and x17 are the
// intra-procedure-call scratch registers, dead at a function's entry as they
// are for any dyld stub. The trampoline that runs the original function sits
// in the same block: the first instruction, moved (Platform/HookPlanner.h),
// then a `b` to the function's second instruction. It needs no scratch register.
//
// The far jump overwrites x16, and the replacement need not keep x16 or x17.
// A function that receives a value in x16 or x17 at its entry must therefore
// not be hooked: the procedure call standard does not allow that for an
// ordinary call, but hand-written stubs do it. The planner refuses a function
// whose code may read either before writing it (Platform/HookPlanner.h).
//
// The only patch on the function is one instruction word, so a hook goes in
// and comes out safely while any number of threads run the function
// (Platform/CodeWriter.h).
//
// Two phases, so that several hooks can go in together:
//   prepare   plans the trampoline, reserves the block and writes island and
//             trampoline in one swap, and publishes the pointer to the
//             original (the trampoline). From then on the trampoline can be
//             called and behaves as the original function.
//   commit    writes the one-word patch. Hooks whose functions start on one
//             page go in with one swap; hooks on different pages one page at
//             a time.
// Remove writes the original first instruction back; island and trampoline
// stay, since a thread may still be running them, and a removed hook can be
// committed again.
//
// One hook per function. Preparing a function whose hook (of this copy of the
// platform layer) is prepared but not committed, or removed, re-targets that
// hook: its island's far jump gets the new replacement, the original is
// published again, and the same hook is returned. So a removed hook can be
// prepared again with another replacement, and an install whose commit failed
// can be retried.
//
// Refused, with the reason and nothing changed: what the planner refuses, a
// misaligned target, a function that is not in executable memory, a function
// that carries a committed bg3mse hook (this copy's, or a first instruction
// that branches to a far jump, taken to be another copy's), and no island in
// range.
//
// A function that carries a hook no longer has the shape a CodeShape check
// recorded for it. A second feature that checks the shape of a function
// another feature hooks therefore fails closed; that is intended.
//
// All of this holds the one code lock (CodeLock()), and the hooks are never
// freed: there is no limit on their number.

namespace bg3se::mac {

class GameImage;

class EntryHook
{
public:
    void* Target() const { return target_; }
    void* Trampoline() const { return trampoline_; }
    void* Island() const { return island_; }
    bool Committed() const { return committed_; }

private:
    friend struct EntryHookAccess;

    EntryHook(void* target, uint32_t first, void* island, void* trampoline)
        : target_(target), first_(first), island_(island), trampoline_(trampoline)
    {}

    void* target_;
    uint32_t first_;        // the function's own first instruction
    void* island_;
    void* trampoline_;
    bool committed_{ false };
    EntryHook* next_{ nullptr };
};

// Prepares a hook on the function at `target`, `functionBytes` long (from
// Platform/FunctionExtents.h, or from the symbol table: up to the next
// symbol). `*original` is set to the trampoline before this returns. nullptr,
// with the reason, when refused; `*refusal`, when given, is then the refusal,
// or HookRefusal::None when the call failed for another reason (missing
// arguments, or a hook to re-target whose function or island changed).
EntryHook* PrepareEntryHook(void* target, size_t functionBytes, void const* replacement, void** original, std::string& problem,
    HookRefusal* refusal = nullptr);

// Commits prepared hooks: the patch goes live. Empty when all went in.
std::string CommitEntryHooks(std::span<EntryHook* const> hooks);
std::string CommitEntryHook(EntryHook* hook);

// Prepare and commit.
EntryHook* InstallEntryHook(void* target, size_t functionBytes, void const* replacement, void** original, std::string& problem);

// Writes the function's first instruction back.
std::string RemoveEntryHook(EntryHook* hook);

// The hook this copy of the platform layer has prepared on `target`, if any.
EntryHook* FindEntryHook(void const* target);

// Whether an entry hook could go on the function at link-time address `va`,
// ending at `end`, in `image` (loaded, or a binary mapped from disk): the
// planner's verdict with the trampoline at either end of the image's header
// padding, where installation takes its islands. A function out of range of
// the padding is refused here (NoIsland), although in the running game a page
// mapped near it might serve.
HookRefusal CheckEntryHook(GameImage const& image, uint64_t va, uint64_t end);

}
