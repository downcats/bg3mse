#include <stdafx.h>
#include <Gfx/GfxPatch.h>
#include <Platform/CodeShape.h>

namespace bg3se::mac::gfx {

namespace {

constexpr uint32_t kLdrX16Literal8 = 0x58000050u;   // ldr x16, #8
constexpr uint32_t kBrX16 = 0xd61f0200u;            // br x16

bool Reaches(uint64_t from, uint64_t to)
{
    int64_t delta = (int64_t)(to - from);
    return !(delta & 3) && delta >= -kBranchRange && delta < kBranchRange;
}

void PutAddress(uint32_t* out, uint64_t address)
{
    memcpy(out, &address, sizeof(address));
}

}

uint32_t EncodeB(uint64_t from, uint64_t to)
{
    if (!Reaches(from, to)) return 0;
    return 0x14000000u | ((uint32_t)((int64_t)(to - from) >> 2) & 0x03ffffffu);
}

uint32_t EncodeBl(uint64_t from, uint64_t to)
{
    if (!Reaches(from, to)) return 0;
    return 0x94000000u | ((uint32_t)((int64_t)(to - from) >> 2) & 0x03ffffffu);
}

void WriteFarJump(uint32_t out[kFarJumpWords], uint64_t target)
{
    out[0] = kLdrX16Literal8;
    out[1] = kBrX16;
    PutAddress(&out[2], target);
}

uint32_t ZeroInsteadOfLoad(uint32_t liveLoad)
{
    return 0xd2800000u | RegD(liveLoad);              // movz xN, #0
}

void ZeroInsteadOfConstant(uint32_t out[2], uint32_t liveMovz)
{
    out[0] = 0x52800000u | RegD(liveMovz);            // movz wN, #0
    out[1] = kNop;
}

void WriteCapIdleIsland(uint32_t out[kCapIdleIslandWords], uint32_t clockReg, uint32_t targetReg, uint64_t hook)
{
    out[0] = MovX(0, clockReg);     // the clock
    out[1] = MovX(1, targetReg);    // the wait target; the hook returns to CapFrame's bl
    WriteFarJump(&out[2], hook);
}

IdleAction CapIdleStep(uint64_t now, uint64_t deadline, bool clockStarted, uint64_t pollTicks, uint64_t tailTicks,
    uint64_t marginTicks, uint64_t* wake)
{
    // No deadline to aim for: just step.
    if (!clockStarted) {
        *wake = now + pollTicks;
        return IdleAction::Sleep;
    }
    if ((int64_t)(deadline - now) > (int64_t)(tailTicks + pollTicks)) {
        *wake = now + pollTicks;
        return IdleAction::Sleep;
    }
    uint64_t target = deadline - marginTicks;
    if ((int64_t)(target - now) <= 0) return IdleAction::Yield;
    *wake = target;
    return IdleAction::Sleep;
}

bool SliceTake(UniformSlice& slice, void const* renderer, uint64_t generation, uint64_t size, uint64_t* offset)
{
    if (slice.Renderer != renderer || slice.Generation != generation) return false;
    uint64_t at = (slice.Next + 15) & ~(uint64_t)15;
    if (at + size > slice.End) return false;
    slice.Next = at + size;
    *offset = at;
    return true;
}

uint64_t SliceReserveBytes(uint64_t sliceBytes, uint64_t size)
{
    uint64_t need = (size + 15) & ~(uint64_t)15;
    return need > sliceBytes ? need : sliceBytes;
}

// The live block (BindMaterialParameters):
//    0 ldr xR, [x1, #..]       the renderer          7 add xO, x9, x8       the aligned offset
//    1 add x20, xR, #lock                            8 add x8, xO, xS       xS: the size
//    2 mov x0, x20             3 bl lock             9 str x8, [xR, #bump]
//    4 ldr x8, [xR, #bump]     5-6 align           10 mov x0, x20         11 bl unlock
//   12 ldr x1, [sp, #..]       BG3's reload        13 b resume
bool WriteBindAlloc(uint32_t out[kBindAllocWords], uint32_t const live[kBindAllocLiveWords], uint64_t site,
    uint64_t allocIsland, uint64_t resume)
{
    uint32_t call = EncodeBl(site + 4 * 4, allocIsland);
    uint32_t back = EncodeB(site + 7 * 4, resume);
    if (!call || !back) return false;
    out[0] = live[0];                       // the renderer, as BG3 loads it
    out[1] = live[1];                       // BG3 keeps this for later
    out[2] = MovX(0, RegD(live[0]));
    out[3] = MovX(1, RegM(live[8]));        // the size
    out[4] = call;
    out[5] = MovX(RegD(live[7]), 0);        // the offset, where BG3 expects it
    out[6] = live[12];                      // BG3's reload after the lock
    out[7] = back;
    return true;
}

// The live block (SetupEngineParameters):
//    0 ldr xH, [x19, #..]      the renderer's holder 7 add xO, x9, x8       the aligned offset
//    1 add x27, xH, #lock                            8 add x8, xO, xS       xS: the size
//    2 mov x0, x27             3 bl lock             9 str x8, [xH, #bump]
//    4 ldr x8, [xH, #bump]     5-6 align           10 mov x23, x19        BG3's own move
//   11 mov x0, x27            12 bl unlock          then resume
bool WriteSetupAlloc(uint32_t out[kSetupAllocWords], uint32_t const live[kSetupAllocLiveWords], uint64_t site,
    uint64_t allocIsland, uint64_t resume, uint32_t rendererInHolder)
{
    uint32_t call = EncodeBl(site + 3 * 4, allocIsland);
    uint32_t back = EncodeB(site + 6 * 4, resume);
    if (!call || !back || rendererInHolder > 0xfff) return false;
    out[0] = WithRegD(live[0], 8);                          // ldr x8, [x19, #..]: the holder
    out[1] = 0x91000000u | (rendererInHolder << 10) | (8u << 5);   // add x0, x8, #..: the renderer inside it
    out[2] = MovX(1, RegM(live[8]));        // the size
    out[3] = call;
    out[4] = MovX(RegD(live[7]), 0);        // the offset, where BG3 expects it
    out[5] = live[10];                      // BG3's own move, which sat inside the lock
    out[6] = back;
    return true;
}

void WriteFrameResetIsland(uint32_t out[kFrameResetIslandWords], uint32_t liveReset, uint64_t generationAddr)
{
    out[0] = liveReset;         // BG3's reset
    out[1] = 0x580000b0u;       // ldr x16, #20            &generation
    out[2] = 0xf9400211u;       // ldr x17, [x16]
    out[3] = 0x91000631u;       // add x17, x17, #1
    out[4] = 0xc89ffe11u;       // stlr x17, [x16]
    out[5] = kRet;
    PutAddress(&out[6], generationAddr);
}

// The live block (unified-memory branch):
//    0 add x0, x19, #8         1 mov w21, #kind      2 mov w1, #size        3 mov w2, #align
//    4 bl AddCommand           5 str x20, [x0, #8]   the ring itself        6-11 the rest of the command
//   12 b ...
bool WriteAoBlurBind(uint32_t out[kAoBlurBindWords], uint32_t const live[kAoBlurBindWords], uint64_t site,
    uint64_t island, uint32_t bufferCommand)
{
    uint32_t call = EncodeBl(site + 5 * 4, island);
    if (!call || bufferCommand > 0xffff) return false;
    memcpy(out, live, kAoBlurBindWords * sizeof(uint32_t));
    out[1] = (live[1] & ~(0xffffu << 5)) | (bufferCommand << 5);   // a command holding a buffer, not a ring
    out[5] = call;                                                  // in place of the store of the ring itself
    return true;
}

void WriteAoBlurIsland(uint32_t out[kAoBlurIslandWords], uint32_t counterLoad, uint32_t counterMask, uint32_t slotLoad,
    uint32_t ringStore)
{
    constexpr uint32_t kScratch = 9;
    out[0] = WithRegD(counterLoad, kScratch);                       // ldr w9, [xR, #counter]   the ring counter
    out[1] = WithRegN(WithRegD(counterMask, kScratch), kScratch);   // and w9, w9, #mask
    out[2] = WithRegM(WithRegD(slotLoad, kScratch), kScratch);      // ldr x9, [xR, w9, uxtw #3] the slot just written
    out[3] = WithRegD(ringStore, kScratch);                         // str x9, [x0, #8]          the command's buffer
    out[4] = kRet;
}

bool WriteJitterIsland(uint32_t out[kJitterIslandWords], uint64_t island, uint64_t stub, uint64_t resume)
{
    uint32_t back = EncodeB(island + 8 * 4, resume);
    if (!back) return false;
    out[0] = 0xa9bf47f0u;       // stp x16, x17, [sp, #-16]!
    out[1] = 0x100000d1u;       // adr x17, #24              the ldp below
    out[2] = 0x58000070u;       // ldr x16, #12              the stub
    out[3] = kBrX16;
    out[4] = kNop;
    PutAddress(&out[5], stub);
    out[7] = 0xa8c147f0u;       // ldp x16, x17, [sp], #16
    out[8] = back;
    return true;
}

}
