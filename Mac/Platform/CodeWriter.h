#pragma once

#include <cstdint>
#include <mutex>
#include <span>
#include <string>

// Every write of code in the process goes through here.
//
// A code page is never made writable. The writer copies the page, patches the
// copy, makes the copy r-x, and maps it over the original address in one step
// (mach_vm_remap with VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE). Threads executing
// the page keep executing it throughout: before the remap they run the old
// bytes, after it the new ones, and the page never loses execute permission.
// A MAP_JIT page (mapped read-write-execute) is written with this thread's JIT
// write gate open instead, each aligned 8 bytes in one store; other threads
// keep executing it.
//
// The swap changes the bytes at once, not the execution. A thread that has
// already run word N of a sequence fetches the new word N+1. Hence two
// contracts:
//
//   one instruction word    may be written at any time, with any number of
//                           threads executing the function (WriteCodeWord,
//                           WriteCodeWords);
//   several words in a row  only where no thread can be executing them
//                           (WriteCodeBlock), and the caller says why that holds
//                           (BlockContract).
//
// A write that spans two pages is refused. Before a swap the page must be
// mapped readable and executable; the swapped page gets the protection and
// inheritance the original had, and both, and the written bytes, are checked
// afterwards. A refusal leaves the code as it was. A swap that does not pass
// the checks afterwards is reported as an error with the swapped page in
// place: the remap has been made by then.
//
// Errors are returned as text: empty when the write was made.

namespace bg3se::mac {

// Why no thread can be executing a block of several words while it is written.
enum class BlockContract
{
    // Memory nothing branches to yet: an island just reserved, before the
    // branch to it is written.
    Unreachable,
    // Start-up, from a dylib constructor, before the game has started any
    // thread of its own.
    BeforeGameThreads,
};

struct WordPatch
{
    void* At;
    uint32_t Word;
};

// One aligned instruction word.
std::string WriteCodeWord(void* at, uint32_t word);

// Several single words that lie on one page, written in one swap. Each is a
// separate one-word patch: the contract of WriteCodeWord holds for each.
std::string WriteCodeWords(std::span<WordPatch const> patches);

// Several words in a row, within one page.
std::string WriteCodeBlock(void* at, std::span<uint32_t const> words, BlockContract contract);

// Stores a pointer into read-only data (a vtable slot in __DATA_CONST, say):
// the page is made writable (copy-on-write) for the one aligned store, then
// given back the protection it had. Adding write permission takes nothing away
// from readers, so this is safe while other threads read the page. Refuses
// executable memory, unmapped memory and a misaligned address.
std::string StorePointer(void* at, void const* value);

// --- for the island allocator and the hook installer -------------------------------

// The one lock of this copy of the platform layer. Code writes, island
// reservations and hook changes all hold it. Public functions take it; the
// *Locked functions expect the caller to hold it.
std::mutex& CodeLock();

// The page size code is written in.
uintptr_t CodePageSize();

// What the VM says of the page holding `address`.
struct PageInfo
{
    bool Mapped{ false };
    bool Readable{ false };
    bool Writable{ false };
    bool Executable{ false };
};
PageInfo QueryPage(uintptr_t address);

// The page holding [at, at + bytes) is copied, `edit` is called with the
// copy's address of `at` (and `context`), and the edited copy is written as
// one swap (or, on a MAP_JIT page, under the JIT write gate). `edit` may
// refuse with a reason, and the page is then left alone. Contract: as for the
// public function that calls it.
using PageEdit = std::string (*)(uint8_t* copyOfAt, void* context);
std::string EditCodeLocked(void* at, size_t bytes, PageEdit edit, void* context);

// WriteCodeWords for a caller that holds CodeLock().
std::string WriteCodeWordsLocked(std::span<WordPatch const> patches);

}
