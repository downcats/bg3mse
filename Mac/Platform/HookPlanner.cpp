#include <Platform/HookPlanner.h>

#include <vector>

namespace bg3se::mac {

namespace {

constexpr int64_t kBranchRange = 128ll << 20;       // b, bl: +-128 MB
constexpr int64_t kAdrRange = 1ll << 20;            // adr: +-1 MB
constexpr int64_t kPageRange = 4ll << 30;           // adrp: +-4 GB

int64_t SignExtend(uint64_t value, unsigned bits)
{
    uint64_t sign = 1ull << (bits - 1);
    return (int64_t)((value ^ sign) - sign);
}

bool Fits(int64_t delta, int64_t range)
{
    return delta >= -range && delta < range;
}

// --- recognising instructions ---------------------------------------------------------

bool IsB(uint32_t i) { return (i & 0xfc000000) == 0x14000000; }
bool IsBl(uint32_t i) { return (i & 0xfc000000) == 0x94000000; }
bool IsBCond(uint32_t i) { return (i & 0xff000010) == 0x54000000; }         // b.cond (not bc.cond)
bool IsAnyBCond(uint32_t i) { return (i & 0xff000000) == 0x54000000; }      // b.cond or bc.cond
bool IsCompareBranch(uint32_t i) { return (i & 0x7e000000) == 0x34000000; } // cbz, cbnz
bool IsTestBranch(uint32_t i) { return (i & 0x7e000000) == 0x36000000; }    // tbz, tbnz
bool IsAdr(uint32_t i) { return (i & 0x9f000000) == 0x10000000; }
bool IsAdrp(uint32_t i) { return (i & 0x9f000000) == 0x90000000; }
bool IsLoadLiteral(uint32_t i) { return (i & 0x3b000000) == 0x18000000; }   // ldr/ldrsw/prfm (literal), any register file
bool IsBrk(uint32_t i) { return (i & 0xffe0001f) == 0xd4200000; }
bool IsUdf(uint32_t i) { return (i & 0xffff0000) == 0x00000000; }
bool IsBlr(uint32_t i) { return (i & 0xfffffc1f) == 0xd63f0000; }

// Instructions that do not read the PC, by the architecture's top-level
// encoding groups (bits 28:25). Data processing (immediate, other than
// adr/adrp), data processing (register), SIMD and FP, and loads and stores
// (other than the literal loads) have no PC-relative form. Of the branch,
// exception and system group only these are taken: the system instructions
// (hints, barriers, msr/mrs, sys), svc, and br/ret without pointer
// authentication. Everything else (reserved, SME, SVE and unallocated groups,
// blr and the other branch and exception forms) is refused; a call is refused
// before this is asked.
bool IsPositionIndependent(uint32_t i)
{
    uint32_t op0 = (i >> 25) & 0xf;
    if ((op0 & 0b1110) == 0b1000) return !IsAdr(i) && !IsAdrp(i);      // data processing (immediate)
    if ((op0 & 0b0111) == 0b0101) return true;                          // data processing (register)
    if ((op0 & 0b0111) == 0b0111) return true;                          // SIMD and FP
    if ((op0 & 0b0101) == 0b0100) return !IsLoadLiteral(i);             // loads and stores
    if ((op0 & 0b1110) == 0b1010) {                                     // branches, exceptions, system
        if ((i & 0xffc00000) == 0xd5000000) return true;                // system instructions
        if ((i & 0xffe0001f) == 0xd4000001) return true;                // svc
        uint32_t reg = i & 0xfffffc1f;
        return reg == 0xd61f0000 || reg == 0xd65f0000;     // br, ret
    }
    return false;
}

// --- x16 and x17 at the entry ----------------------------------------------------------

// Of x16 (bit 0) and x17 (bit 1): which an instruction may read, and which it
// surely writes. Each group's register fields are taken as the architecture
// defines them for that group, since an immediate may hold 16 or 17 as well. An
// instruction of a group not decoded here is taken to read every field.
struct ScratchUse
{
    uint8_t Reads{ 0 };
    uint8_t Writes{ 0 };
};

uint8_t ScratchBit(uint32_t reg)
{
    return reg == 16 ? 1 : reg == 17 ? 2 : 0;
}

// ldr/str of one register, any indexing: the address registers are read, Rt
// is read by a store and written by a load; an FP/SIMD Rt is neither.
ScratchUse OneRegister(uint32_t i, uint8_t address, uint8_t t)
{
    if ((i >> 26) & 1) return { address, 0 };
    uint32_t size = i >> 30, opc = (i >> 22) & 3;
    if (opc == 0) return { uint8_t(address | t), 0 };              // strb, strh, str
    if (size == 3 && opc == 2) return { address, 0 };              // prfm: Rt is the prefetch operation
    if (size >= 2 && opc == 3) return { uint8_t(address | t), 0 }; // unallocated
    return { address, t };                                         // ldrb, ldrh, ldrs*, ldr
}

ScratchUse ScratchUseOf(uint32_t i)
{
    uint8_t d = ScratchBit(i & 31), n = ScratchBit((i >> 5) & 31), a = ScratchBit((i >> 10) & 31), m = ScratchBit((i >> 16) & 31);
    uint8_t all = d | n | a | m;
    uint32_t op0 = (i >> 25) & 0xf;

    if ((op0 & 0b1110) == 0b1000) {                                     // data processing (immediate)
        switch ((i >> 23) & 7) {
        case 0: case 1: return { 0, d };                                // adr, adrp
        case 2: case 3: case 4: return { n, d };                        // add/sub (with tags), min/max, logical
        case 5: {                                                       // move wide
            uint32_t opc = (i >> 29) & 3;
            if (opc == 3) return { d, d };                              // movk keeps the other bits
            if (opc == 1) return { all, 0 };                            // unallocated
            return { 0, d };                                            // movn, movz
        }
        case 6: {                                                       // bitfield
            uint32_t opc = (i >> 29) & 3;
            if (opc == 1) return { uint8_t(n | d), d };                 // bfm keeps bits of Rd
            if (opc == 3) return { all, 0 };                            // unallocated
            return { n, d };                                            // sbfm, ubfm
        }
        default: return { uint8_t(n | m), d };                          // extr
        }
    }

    if ((op0 & 0b1110) == 0b1010) {                                     // branches, exceptions, system
        if ((i & 0x7c000000) == 0x14000000) return {};                  // b, bl
        if ((i & 0xff000000) == 0x54000000) return {};                  // b.cond, bc.cond
        if ((i & 0x7c000000) == 0x34000000) return { d, 0 };            // cbz, cbnz, tbz, tbnz test Rt
        if ((i & 0xfe000000) == 0xd6000000) return { uint8_t(n | d), 0 };   // br, blr, ret, eret and their authenticated forms: Rn and the modifier
        if ((i & 0xffe0001f) == 0xd4000001) return { 1, 0 };            // svc: the Darwin kernel takes the call's number in x16
        if ((i & 0xff000000) == 0xd4000000) return {};                  // brk, hlt, hvc, smc, dcps
        if ((i & 0xffc00000) == 0xd5000000) {                           // system instructions
            if ((i >> 21) & 1) return { 0, d };                         // mrs, sysl
            return { d, 0 };                                            // msr, sys; hints and barriers name x31
        }
        return { all, 0 };
    }

    if ((op0 & 0b0101) == 0b0100) {                                     // loads and stores
        bool simd = (i >> 26) & 1;
        if ((i & 0x3b000000) == 0x18000000) {                           // ldr, ldrsw, prfm (literal)
            return { 0, uint8_t(simd || (i >> 30) == 3 ? 0 : d) };
        }
        if ((i & 0x3a000000) == 0x28000000) {                           // pairs, every indexing
            uint8_t t = simd ? 0 : uint8_t(d | a);
            if ((i >> 22) & 1) return { n, t };                         // ldp, ldpsw, ldnp
            return { uint8_t(n | t), 0 };                               // stp, stnp
        }
        if ((i & 0x3b000000) == 0x39000000) return OneRegister(i, n, d);    // unsigned offset
        if ((i & 0x3b200000) == 0x38000000) return OneRegister(i, n, d);    // unscaled, pre- and post-index, unprivileged
        if ((i & 0x3b200c00) == 0x38200800) return OneRegister(i, uint8_t(n | m), d);   // register offset
        if ((i & 0xff200400) == 0xf8200400) return { n, d };            // ldraa, ldrab
        if ((i & 0x3f200c00) == 0x38200000) return { uint8_t(n | m), d };   // atomics: Rs in, Rt out
        if ((i & 0xbe000000) == 0x0c000000) {                           // ld1-ld4, st1-st4: FP/SIMD registers
            return { uint8_t(n | (((i >> 23) & 1) ? m : 0)), 0 };       // the post-index register
        }
        if ((i & 0x3f000000) == 0x08000000) {                           // exclusive and ordered, one register
            bool o2 = (i >> 23) & 1, load = (i >> 22) & 1, o1 = (i >> 21) & 1;
            if (load && !o1) return { n, d };                           // ldxr, ldaxr, ldar, ldlar
            if (!load && !o2 && !o1) return { uint8_t(n | d), m };      // stxr, stlxr: Rs is the status written
            if (!load && o2 && !o1) return { uint8_t(n | d), 0 };       // stlr, stllr
        }
        return { all, 0 };                                              // pairs of these, compare and swap, the rest
    }

    if ((op0 & 0b0111) == 0b0101) {                                     // data processing (register)
        if ((i & 0x1fe00000) == 0x1a400000) return { uint8_t(n | (((i >> 11) & 1) ? 0 : m)), 0 };  // ccmp, ccmn: [4:0] are flags
        if ((i & 0x1fe00000) == 0x1a000000 && ((i >> 10) & 0x3f)) return { uint8_t(n | m), 0 };    // rmif, setf: flags only
        if ((i & 0x1f000000) == 0x1b000000) return { uint8_t(n | m | a), d };   // three sources
        if ((i & 0x5fe00000) == 0x5ac00000) return { uint8_t(n | d), d };       // one source; pac*, aut* change Rd in place
        return { uint8_t(n | m), d };                                   // logical, add/sub, adc, csel, two sources
    }

    if ((op0 & 0b0111) == 0b0111) {                                     // SIMD and FP: only these name general registers
        if ((i & 0x5f20fc00) == 0x1e200000) {                           // between FP and general registers
            uint32_t opcode = (i >> 16) & 7;
            if (opcode == 2 || opcode == 3 || opcode == 7) return { n, 0 };  // scvtf, ucvtf, fmov from general
            return { 0, d };                                            // fcvt*, fmov to general, fjcvtzs
        }
        if ((i & 0x5f200000) == 0x1e000000) {                           // between FP and fixed point
            uint32_t opcode = (i >> 16) & 7;
            if (opcode == 2 || opcode == 3) return { n, 0 };            // scvtf, ucvtf
            return { 0, d };                                            // fcvtzs, fcvtzu
        }
        if ((i & 0x9fe08400) == 0x0e000400 && !((i >> 29) & 1)) {      // AdvSIMD copy
            uint32_t imm4 = (i >> 11) & 0xf;
            if (imm4 == 1 || imm4 == 3) return { n, 0 };                // dup, ins (general)
            if (imm4 == 5 || imm4 == 7) return { 0, d };                // smov, umov
        }
        return {};
    }

    return { all, 0 };                                                  // SME, SVE, unallocated
}

// Whether the function may read x16 or x17 before writing it: whether it takes
// a value in one of them at its entry. Every path from the entry is followed
// through the function's own branches. A call ends both entry values (they are
// the callee's scratch registers, and nothing may count on them after it), and
// so do ret and a branch out of the function: a value handed on to another
// function is not followed. An indirect jump (a jump table) may go to any
// instruction of the function.
bool ReadsScratchAtEntry(std::span<uint32_t const> function, uint64_t address)
{
    size_t count = function.size();
    std::vector<uint8_t> in(count, 0);
    std::vector<size_t> work;
    auto flow = [&](size_t to, uint8_t live) {
        if (to >= count || (in[to] | live) == in[to]) return;
        in[to] |= live;
        work.push_back(to);
    };
    auto inside = [&](uint64_t target, size_t& index) {
        if (target < address || target >= address + 4 * count || (target & 3)) return false;
        index = (size_t)((target - address) / 4);
        return true;
    };

    flow(0, 3);
    uint8_t anywhere = 0;
    for (;;) {
        while (!work.empty()) {
            size_t at = work.back();
            work.pop_back();
            uint32_t i = function[at];
            uint8_t live = in[at];
            auto use = ScratchUseOf(i);
            if (use.Reads & live) return true;
            live &= (uint8_t)~use.Writes;

            size_t to = 0;
            if (IsBl(i)) continue;
            if (IsB(i)) {
                if (inside(DirectBranchTarget(address + 4 * at, i), to)) flow(to, live);
                continue;
            }
            if (IsAnyBCond(i) || IsCompareBranch(i) || IsTestBranch(i)) {
                if (inside(DirectBranchTarget(address + 4 * at, i), to)) flow(to, live);
                flow(at + 1, live);
                continue;
            }
            if ((i & 0xfe000000) == 0xd6000000) {                       // branch to a register
                uint32_t opc = (i >> 21) & 0xf;
                if (opc == 0 || opc == 8) anywhere |= live;             // br, braa, braaz
                else if ((opc & 7) != 1 && opc != 2 && opc != 4 && opc != 5) anywhere |= live;  // not a call, ret, eret or drps
                continue;
            }
            if (((i >> 25) & 0b1110) == 0b1010 && (i & 0xff000000) != 0xd4000000 && (i & 0xffc00000) != 0xd5000000) {
                anywhere |= live;                                       // a branch not decoded here
            }
            flow(at + 1, live);
        }
        if (!anywhere) return false;
        uint8_t seed = anywhere;
        anywhere = 0;
        for (size_t k = 0; k < count; k++) flow(k, seed);
        if (work.empty()) return false;
    }
}

// --- encoding --------------------------------------------------------------------------

uint32_t EncodeAdrp(uint32_t rd, uint64_t from, uint64_t to)
{
    int64_t pages = (int64_t)((to & ~0xfffull) - (from & ~0xfffull)) >> 12;
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd;
}

uint32_t EncodeAdr(uint32_t rd, uint64_t from, uint64_t to)
{
    uint32_t imm = (uint32_t)(int64_t)(to - from) & 0x1fffff;
    return 0x10000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd;
}

// add xd, xn, #imm12
uint32_t EncodeAddImm(uint32_t rd, uint32_t rn, uint32_t imm12)
{
    return 0x91000000 | (imm12 << 10) | (rn << 5) | rd;
}

// The same conditional branch (b.cond, cbz/cbnz, tbz/tbnz) with its offset set
// to +8: over the next word.
uint32_t SkipOne(uint32_t i)
{
    if (IsTestBranch(i)) return (i & 0xfff8001f) | (2u << 5);
    return (i & 0xff00001f) | (2u << 5);
}

uint64_t ConditionalTarget(uint64_t at, uint32_t i)
{
    if (IsTestBranch(i)) return at + (uint64_t)(SignExtend((i >> 5) & 0x3fff, 14) * 4);
    return at + (uint64_t)(SignExtend((i >> 5) & 0x7ffff, 19) * 4);
}

uint64_t PageTarget(uint64_t at, uint32_t i)
{
    uint64_t imm = ((i >> 5) & 0x7ffff) << 2 | ((i >> 29) & 3);
    if (IsAdrp(i)) return (at & ~0xfffull) + (uint64_t)(SignExtend(imm, 21) * 4096);
    return at + (uint64_t)SignExtend(imm, 21);
}

struct Builder
{
    TrampolinePlan Plan;

    void Put(uint32_t word) { Plan.Words[Plan.Count++] = word; }
    TrampolinePlan Refuse(HookRefusal why)
    {
        Plan.Refusal = why;
        Plan.Count = 0;
        return Plan;
    }
};

}

char const* Describe(HookRefusal refusal)
{
    switch (refusal) {
    case HookRefusal::None: return "can be hooked";
    case HookRefusal::NoFunction: return "the function's extent is unknown";
    case HookRefusal::Misaligned: return "the function or its trampoline is not word-aligned";
    case HookRefusal::Trap: return "the first instruction is brk or udf";
    case HookRefusal::Call: return "the first instruction is a call (bl, blr): it would return into the trampoline";
    case HookRefusal::Unrecognised: return "the first instruction is not one the relocator recognises";
    case HookRefusal::LiteralIntoFpSimd: return "the first instruction loads a literal into an FP/SIMD register";
    case HookRefusal::PrefetchLiteral: return "the first instruction is prfm (literal)";
    case HookRefusal::ZeroRegister: return "the first instruction puts an address or literal into the zero register";
    case HookRefusal::BranchesToItsEntry: return "a branch inside the function goes to its first instruction";
    case HookRefusal::ReadsScratchRegisters: return "the function may read x16 or x17 before writing it, and neither reaches it through a hook";
    case HookRefusal::OutOfRange: return "the moved first instruction cannot reach from the trampoline";
    case HookRefusal::NotExecutable: return "the function is not in executable memory";
    case HookRefusal::AlreadyHooked: return "the function already carries a bg3mse hook";
    case HookRefusal::NoIsland: return "no island within branch range of the function";
    case HookRefusal::Count: break;
    }
    return "unknown refusal";
}

uint32_t EncodeBranch(uint64_t from, uint64_t to, bool link)
{
    int64_t delta = (int64_t)(to - from);
    if ((delta & 3) || !Fits(delta, kBranchRange)) return 0;
    return (link ? 0x94000000u : 0x14000000u) | ((uint32_t)(delta >> 2) & 0x03ffffff);
}

uint64_t DirectBranchTarget(uint64_t at, uint32_t i)
{
    if (IsB(i) || IsBl(i)) return at + (uint64_t)(SignExtend(i & 0x03ffffff, 26) * 4);
    if (IsAnyBCond(i) || IsCompareBranch(i) || IsTestBranch(i)) return ConditionalTarget(at, i);
    return 0;
}

TrampolinePlan PlanTrampoline(std::span<uint32_t const> function, uint64_t address, uint64_t trampoline)
{
    Builder b;
    if (function.empty()) return b.Refuse(HookRefusal::NoFunction);
    if ((address & 3) || (trampoline & 3)) return b.Refuse(HookRefusal::Misaligned);

    // The entry must not be reached from inside the function, other than by a call.
    for (size_t n = 0; n < function.size(); n++) {
        uint32_t word = function[n];
        if (!IsBl(word) && DirectBranchTarget(address + 4 * n, word) == address) return b.Refuse(HookRefusal::BranchesToItsEntry);
    }
    // The island's far jump overwrites x16, and the replacement need not keep x16 or x17.
    if (ReadsScratchAtEntry(function, address)) return b.Refuse(HookRefusal::ReadsScratchRegisters);

    uint32_t first = function[0];
    uint64_t resume = address + 4;
    // The b back to the function's second instruction, from the trampoline's `index`th word.
    auto putResume = [&](size_t index) {
        uint32_t back = EncodeBranch(trampoline + 4 * index, resume, false);
        if (!back) return false;
        b.Put(back);
        return true;
    };

    if (IsBrk(first) || IsUdf(first)) return b.Refuse(HookRefusal::Trap);
    // The callee would return into the trampoline, which has no unwind information.
    if (IsBl(first) || IsBlr(first)) return b.Refuse(HookRefusal::Call);

    if (IsB(first)) {
        uint32_t moved = EncodeBranch(trampoline, DirectBranchTarget(address, first), false);
        if (!moved) return b.Refuse(HookRefusal::OutOfRange);
        b.Put(moved);
        return b.Plan;
    }

    if (IsBCond(first) || IsCompareBranch(first) || IsTestBranch(first)) {
        b.Put(SkipOne(first));
        uint32_t taken = EncodeBranch(trampoline + 8, ConditionalTarget(address, first), false);
        if (!putResume(1) || !taken) return b.Refuse(HookRefusal::OutOfRange);
        b.Put(taken);
        return b.Plan;
    }

    if (IsAdrp(first)) {
        uint64_t page = PageTarget(address, first);
        if (!Fits((int64_t)((page) - (trampoline & ~0xfffull)), kPageRange)) return b.Refuse(HookRefusal::OutOfRange);
        b.Put(EncodeAdrp(first & 0x1f, trampoline, page));
        if (!putResume(1)) return b.Refuse(HookRefusal::OutOfRange);
        return b.Plan;
    }

    if (IsAdr(first)) {
        uint32_t rd = first & 0x1f;
        uint64_t to = PageTarget(address, first);
        if (Fits((int64_t)(to - trampoline), kAdrRange)) {
            b.Put(EncodeAdr(rd, trampoline, to));
            if (!putResume(1)) return b.Refuse(HookRefusal::OutOfRange);
            return b.Plan;
        }
        // add with register 31 would write sp, not xzr.
        if (rd == 31) return b.Refuse(HookRefusal::ZeroRegister);
        if (!Fits((int64_t)((to & ~0xfffull) - (trampoline & ~0xfffull)), kPageRange)) return b.Refuse(HookRefusal::OutOfRange);
        b.Put(EncodeAdrp(rd, trampoline, to));
        b.Put(EncodeAddImm(rd, rd, (uint32_t)(to & 0xfff)));
        if (!putResume(2)) return b.Refuse(HookRefusal::OutOfRange);
        return b.Plan;
    }

    if (IsLoadLiteral(first)) {
        if (first & (1u << 26)) return b.Refuse(HookRefusal::LiteralIntoFpSimd);
        uint32_t opc = first >> 30;
        if (opc == 3) return b.Refuse(HookRefusal::PrefetchLiteral);
        uint32_t rt = first & 0x1f;
        // The load's base register is the destination: 31 would be sp.
        if (rt == 31) return b.Refuse(HookRefusal::ZeroRegister);
        uint64_t literal = address + (uint64_t)(SignExtend((first >> 5) & 0x7ffff, 19) * 4);
        if (!Fits((int64_t)((literal & ~0xfffull) - (trampoline & ~0xfffull)), kPageRange)) return b.Refuse(HookRefusal::OutOfRange);
        // ldr wt, ldr xt, ldrsw xt (unsigned offset); the offset is set below.
        static constexpr uint32_t kLoad[3] = { 0xb9400000, 0xf9400000, 0xb9800000 };
        uint32_t size = opc == 1 ? 8 : 4;
        uint32_t low = (uint32_t)(literal & 0xfff);
        uint32_t load = kLoad[opc] | (rt << 5) | rt;
        b.Put(EncodeAdrp(rt, trampoline, literal));
        if (low % size == 0) {
            b.Put(load | ((low / size) << 10));
        } else {
            b.Put(EncodeAddImm(rt, rt, low));
            b.Put(load);
        }
        if (!putResume(b.Plan.Count)) return b.Refuse(HookRefusal::OutOfRange);
        return b.Plan;
    }

    if (!IsPositionIndependent(first)) return b.Refuse(HookRefusal::Unrecognised);
    b.Put(first);
    if (!putResume(1)) return b.Refuse(HookRefusal::OutOfRange);
    return b.Plan;
}

}
