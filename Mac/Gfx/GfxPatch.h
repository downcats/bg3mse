#pragma once

#include <cstdint>

// Pure decisions and instruction words for the graphics fixes. Nothing here
// touches memory it was not handed, so the tests check every word without a
// game. Every sequence that keeps any of the game's own instructions takes
// them as `live` words, read from the running game at the site, never from a
// table: their branch targets and literals belong to one build.
//
// The fixes' research (what to patch, the instruction sequences and layouts)
// comes from bg3mfx, https://github.com/AndrewTriesToCode/bg3mfx
// (MIT, Copyright (c) 2026 AndrewTriesToCode). The code is written for bg3mse.

namespace bg3se::mac::gfx {

constexpr int64_t kBranchRange = 128ll * 1024 * 1024;

// b / bl from `from` to `to`; 0 when out of range or misaligned.
uint32_t EncodeB(uint64_t from, uint64_t to);
uint32_t EncodeBl(uint64_t from, uint64_t to);

// mov xd, xm (orr xd, xzr, xm)
constexpr uint32_t MovX(uint32_t rd, uint32_t rm) { return 0xaa0003e0u | (rm << 16) | rd; }
constexpr uint32_t kRet = 0xd65f03c0u;
constexpr uint32_t kNop = 0xd503201fu;

// Whether `words` instruction words at `at` lie on one page of `pageSize`
// bytes. The code writer writes a sequence of several words only within one
// page, so a fix whose sequence crosses a page is refused before anything of
// it is written.
constexpr bool OnOnePage(uint64_t at, uint64_t words, uint64_t pageSize)
{
    return words && pageSize && at / pageSize == (at + 4 * words - 1) / pageSize;
}

// ldr x16, #8; br x16; .quad target
constexpr int kFarJumpWords = 4;
void WriteFarJump(uint32_t out[kFarJumpWords], uint64_t target);

// --- Thread idling ---------------------------------------------------------------

// `movz xN, #0` for the load `ldr xN, [...]` it replaces (a spin budget of zero).
uint32_t ZeroInsteadOfLoad(uint32_t liveLoad);

// `movz wN, #0; nop` for the pair `movz wN, #lo; movk wN, #hi` it replaces.
void ZeroInsteadOfConstant(uint32_t out[2], uint32_t liveMovz);

// Island for CapFrame's idle yield: passes the clock (x<clockReg>) and its wait
// target (x<targetReg>) to `hook` in place of `bl pthread_yield_np`; the hook
// returns straight to CapFrame.
constexpr int kCapIdleIslandWords = 6;
void WriteCapIdleIsland(uint32_t out[kCapIdleIslandWords], uint32_t clockReg, uint32_t targetReg, uint64_t hook);

enum class IdleAction { Yield, Sleep };

// What CapFrame's idle step does now. While jobs may still be stolen after
// another step, sleep one poll; once the next step would land in CapFrame's own
// final spin (`tailTicks`), sleep up to `marginTicks` before the deadline, so
// that spin only covers the margin.
IdleAction CapIdleStep(uint64_t now, uint64_t deadline, bool clockStarted, uint64_t pollTicks, uint64_t tailTicks,
    uint64_t marginTicks, uint64_t* wake);

// --- Reduced locking ---------------------------------------------------------------

// One thread's slice of the renderer's uniform buffer for one frame.
struct UniformSlice
{
    void const* Renderer{ nullptr };
    uint64_t Generation{ 0 };
    uint64_t Next{ 0 };
    uint64_t End{ 0 };
};

// Take `size` bytes, 16-byte aligned, from the slice. False when the slice is
// for another renderer or frame or does not have the room; it is then left
// alone and the caller must refill it.
bool SliceTake(UniformSlice& slice, void const* renderer, uint64_t generation, uint64_t size, uint64_t* offset);

// How many bytes to reserve for a new slice that must hold `size`.
uint64_t SliceReserveBytes(uint64_t sliceBytes, uint64_t size);

// BindMaterialParameters' inlined lock-and-bump (14 live words, the last a `b`
// to `resume`) replaced by a call to the allocation island.
constexpr int kBindAllocLiveWords = 14;
constexpr int kBindAllocWords = 8;
bool WriteBindAlloc(uint32_t out[kBindAllocWords], uint32_t const live[kBindAllocLiveWords], uint64_t site,
    uint64_t allocIsland, uint64_t resume);

// SetupEngineParameters' inlined lock-and-bump (13 live words, falling through
// to `resume`). The renderer sits `rendererInHolder` bytes into the object the
// first live word loads.
constexpr int kSetupAllocLiveWords = 13;
constexpr int kSetupAllocWords = 7;
bool WriteSetupAlloc(uint32_t out[kSetupAllocWords], uint32_t const live[kSetupAllocLiveWords], uint64_t site,
    uint64_t allocIsland, uint64_t resume, uint32_t rendererInHolder);

// BeginFrame's reset of the bump offset (the live store), which also retires
// every slice by publishing the next generation at `generationAddr`.
constexpr int kFrameResetIslandWords = 8;
void WriteFrameResetIsland(uint32_t out[kFrameResetIslandWords], uint32_t liveReset, uint64_t generationAddr);

// --- AO blur ---------------------------------------------------------------------

// The bind in the unified-memory branch: the live words with the command kind
// set to `bufferCommand` (the kind the other branch records, holding a buffer
// rather than the ring) and the store of the ring replaced by a call to the island.
constexpr int kAoBlurBindWords = 13;
bool WriteAoBlurBind(uint32_t out[kAoBlurBindWords], uint32_t const live[kAoBlurBindWords], uint64_t site,
    uint64_t island, uint32_t bufferCommand);

// The island: the ring slot just written, from the same counter, mask and slot
// load the function's own update uses, stored where the bind stored the ring.
//   counterLoad  ldr wC, [xR, #counter]
//   counterMask  and wC, wC, #mask
//   slotLoad     ldr xS, [xR, wC, uxtw #3]
//   ringStore    str xR, [x0, #8]  (the live bind word it stands in for)
constexpr int kAoBlurIslandWords = 5;
void WriteAoBlurIsland(uint32_t out[kAoBlurIslandWords], uint32_t counterLoad, uint32_t counterMask, uint32_t slotLoad,
    uint32_t ringStore);

// --- Temporal jitter -------------------------------------------------------------------

// Saves x16/x17, calls `stub` with its return point in x17, restores them and
// resumes after the replaced load.
constexpr int kJitterIslandWords = 9;
bool WriteJitterIsland(uint32_t out[kJitterIslandWords], uint64_t island, uint64_t stub, uint64_t resume);

}
