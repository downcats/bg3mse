#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

// Islands: short runs of code within branch range (+-128 MB) of the code that
// branches to them, for far jumps, trampolines and replacement sequences.
//
// Where they come from, in this order:
//   1. The header padding of the image that holds the code: the bytes between
//      the end of the load commands (plus a margin) and the first section. They
//      are mapped r-x with __TEXT and unused once dyld has read the load
//      commands. For the game no page can be mapped within range of most of its
//      code, because the image and its __LINKEDIT fill that range.
//   2. A page mapped for islands in a free gap of the address space within
//      range: r-x and written through the code writer, or, where the system
//      refuses executable memory that is not MAP_JIT, a MAP_JIT page written
//      with the JIT write gate.
// When neither has room in range, the reservation is refused.
//
// Every island is written through the code writer (Platform/CodeWriter.h). A
// block is handed out only where every byte is still zero, and the swap that
// hands it out also writes it non-zero (its words, or `brk`), checking the
// zero bytes in the copy it patches. Blocks are 16-byte aligned and never
// cross a page. Nothing reserved is ever freed: code may still be executing
// in it.
//
// libbg3mse.dylib and libbg3mse-camera.dylib each carry their own copy of
// this layer, with their own lock, over the same header padding. The zero rule
// is what keeps them from handing out the same block. It relies on the two
// copies never patching at the same time: the camera module patches only in
// its constructor, which libbg3mse.dylib's constructor runs (dlopen) on its own
// thread, between its own patches, before the game's threads exist. Two copies
// swapping one page at once could each map a copy that lacks the other's
// patch, and one patch would be lost.

namespace bg3se::mac {

constexpr int64_t kIslandBranchRange = 128ll * 1024 * 1024;
constexpr size_t kIslandAlign = 16;
// What a reserved block holds where nothing else was written: brk #0xb3.
constexpr uint32_t kIslandFill = 0xd4201660;

// The header padding of the image whose mach_header_64 is at `header`, as
// mapped (a loaded image, or a file: addresses are then in the mapping). From
// a margin past the load commands, 16-byte aligned, up to the start of the
// first section of __TEXT. Nothing when the image has no such padding.
struct HeaderPadding
{
    uintptr_t Low;
    uintptr_t High;
};
std::optional<HeaderPadding> HeaderPaddingOf(void const* header);

// Whether [at, at + bytes) is within branch range of `from` in both directions.
bool InBranchRange(uintptr_t from, uintptr_t at, size_t bytes);

// Builds an island's words for the address it would be placed at. Returns
// empty when the island can go there, otherwise why not; the next candidate
// address is then tried.
using IslandBuilder = std::string (*)(uintptr_t at, std::span<uint32_t> words, void* context);

// Where an island may come from (the order is fixed). Everything by default;
// tests restrict it to reach one source.
enum IslandSource : unsigned
{
    kIslandFromPadding = 1,
    kIslandFromNearPage = 2,
    kIslandFromNearJitPage = 4,
    kIslandFromAnywhere = 7,
};

// Reserves an island of `wordCount` words within branch range of `near`, its
// words built by `build` for the address chosen and written in the swap that
// reserves it (the rest of the 16-byte block is kIslandFill). `imageHeader` is
// the header of the image whose padding to use, or nullptr for none. The
// caller holds CodeLock(). nullptr, with the reason, when nothing fits.
void* ReserveIslandLocked(uintptr_t near, void const* imageHeader, size_t wordCount, IslandBuilder build, void* context,
    std::string& problem, unsigned sources = kIslandFromAnywhere);

// The header of the loaded image that holds `address`, or nullptr. Takes the
// dynamic loader's lock: call it before CodeLock().
void const* ImageHeaderOf(void const* address);

// An island of `bytes` bytes (rounded up to 16) within branch range of `near`,
// filled with kIslandFill, for the caller to write with WriteCodeBlock
// (BlockContract::Unreachable) before anything branches to it.
void* ReserveIsland(void const* near, size_t bytes, std::string& problem);

// A jump pad within branch range of `near`: ldr x16, #8; br x16; .quad target.
// A b or bl to it reaches `target` anywhere in the address space. x16 is IP0,
// which any call may clobber, so the pad leaves the callee's arguments alone.
void* MakeJumpPad(void const* near, void const* target, std::string& problem);

// ldr x16, #8; br x16; .quad target
constexpr size_t kFarJumpWords = 4;
void EncodeFarJump(std::span<uint32_t, kFarJumpWords> out, uint64_t target);

}
