#include <Platform/CodeShape.h>

namespace bg3se::mac {

namespace {

constexpr uint32_t kRet = 0xd65f03c0;

bool IsBranchImm(uint32_t i) { return (i & 0x7c000000) == 0x14000000; }        // b, bl
bool IsBranchCond(uint32_t i) { return (i & 0xff000010) == 0x54000000; }       // b.cond
bool IsCompareBranch(uint32_t i) { return (i & 0x7e000000) == 0x34000000; }    // cbz, cbnz
bool IsTestBranch(uint32_t i) { return (i & 0x7e000000) == 0x36000000; }       // tbz, tbnz
bool IsAdr(uint32_t i) { return (i & 0x1f000000) == 0x10000000; }              // adr, adrp
bool IsLoadLiteral(uint32_t i) { return (i & 0x3b000000) == 0x18000000; }      // ldr (literal)
bool IsAdrp(uint32_t i) { return (i & 0x9f000000) == 0x90000000; }
bool IsAddImm64(uint32_t i) { return (i & 0xff800000) == 0x91000000; }         // add xd, xn, #imm12
bool IsLoadStoreUnsignedImm(uint32_t i) { return (i & 0x3b000000) == 0x39000000; }

// The instruction with the fields a relocation changes set to zero.
uint32_t Masked(uint32_t i, uint32_t previous)
{
    if (IsBranchImm(i)) return i & 0xfc000000;
    if (IsBranchCond(i) || IsCompareBranch(i) || IsLoadLiteral(i)) return i & 0xff00001f;
    if (IsTestBranch(i)) return i & 0xfff8001f;
    if (IsAdr(i)) return i & 0x9f00001f;
    // The low 12 bits that complete an adrp address: add or load/store whose
    // base register is the adrp's destination.
    if (IsAdrp(previous)) {
        uint32_t page = previous & 0x1f;
        uint32_t base = (i >> 5) & 0x1f;
        if (base == page && (IsAddImm64(i) || IsLoadStoreUnsignedImm(i))) return i & 0xffc003ff;
    }
    return i;
}

}

CodeShape ShapeOfRange(void const* code, uint32_t instructions)
{
    auto insn = static_cast<uint32_t const*>(code);
    uint64_t hash = 0xcbf29ce484222325ull;
    uint32_t previous = 0;
    for (uint32_t n = 0; n < instructions; n++) {
        uint32_t word;
        memcpy(&word, insn + n, sizeof(word));
        uint32_t masked = Masked(word, previous);
        for (int b = 0; b < 4; b++) {
            hash = (hash ^ ((masked >> (8 * b)) & 0xff)) * 0x100000001b3ull;
        }
        previous = word;
    }
    return { instructions, hash };
}

uint64_t CallTarget(uint64_t siteVA, uint32_t instruction)
{
    if ((instruction & 0xfc000000) != 0x94000000) return 0;
    int64_t imm = instruction & 0x03ffffff;
    if (imm & (1 << 25)) imm -= (1 << 26);
    return siteVA + (uint64_t)(imm * 4);
}

uint64_t PageAddress(uint64_t siteVA, uint32_t adrp, uint32_t next)
{
    if (!IsAdrp(adrp)) return 0;
    int64_t imm = (int64_t)(((adrp >> 5) & 0x7ffff) << 2 | ((adrp >> 29) & 3));
    if (imm & (1 << 20)) imm -= (1 << 21);
    uint64_t page = (siteVA & ~0xfffull) + (uint64_t)(imm << 12);
    uint32_t reg = adrp & 0x1f;
    if (((next >> 5) & 0x1f) != reg) return 0;
    uint32_t imm12 = (next >> 10) & 0xfff;
    if (IsAddImm64(next)) return page + imm12;
    if (IsLoadStoreUnsignedImm(next)) return page + ((uint64_t)imm12 << (next >> 30));   // scaled by the access size
    return 0;
}

uint64_t JumpTarget(uint64_t siteVA, uint32_t instruction)
{
    if ((instruction & 0xfc000000) != 0x14000000) return 0;
    int64_t imm = instruction & 0x03ffffff;
    if (imm & (1 << 25)) imm -= (1 << 26);
    return siteVA + (uint64_t)(imm * 4);
}

std::optional<uint32_t> LoadStoreOffset(uint32_t i)
{
    if (!IsLoadStoreUnsignedImm(i)) return {};
    uint32_t scale = i >> 30;
    bool simd = (i >> 26) & 1;
    if (simd && (i >> 23) & 1) {
        if (scale != 0) return {};      // only q registers use opc<1> on the vector side
        scale = 4;
    }
    return ((i >> 10) & 0xfff) << scale;
}

std::optional<int32_t> PairOffset(uint32_t i)
{
    // ldp/stp (post-index, signed offset, pre-index) of w or x registers.
    uint32_t kind = (i >> 23) & 7;
    if ((i & 0x3e000000) != 0x28000000 || (kind != 1 && kind != 2 && kind != 3)) return {};
    uint32_t opc = i >> 30;
    if (opc != 0 && opc != 2) return {};
    int32_t imm = (int32_t)((i >> 15) & 0x7f);
    if (imm & 0x40) imm -= 0x80;
    return imm * (opc == 2 ? 8 : 4);
}

std::optional<uint32_t> AddImmediate(uint32_t i)
{
    if ((i & 0x1f800000) != 0x11000000) return {};    // add/adds/sub/subs (immediate), w or x
    uint32_t imm = (i >> 10) & 0xfff;
    return (i >> 22) & 1 ? imm << 12 : imm;
}

std::optional<uint64_t> MovzValue(uint32_t i)
{
    if ((i & 0x7f800000) != 0x52800000) return {};    // movz, w or x
    uint32_t hw = (i >> 21) & 3;
    if (!(i >> 31) && hw > 1) return {};
    return (uint64_t)((i >> 5) & 0xffff) << (16 * hw);
}

std::optional<uint64_t> MovzMovkValue(uint32_t movz, uint32_t movk)
{
    auto low = MovzValue(movz);
    if (!low || (movk & 0x7f800000) != 0x72800000 || RegD(movk) != RegD(movz) || (movk >> 31) != (movz >> 31)) return {};
    uint32_t hw = (movk >> 21) & 3;
    uint64_t lane = 0xffffull << (16 * hw);
    return (*low & ~lane) | ((uint64_t)((movk >> 5) & 0xffff) << (16 * hw));
}

std::optional<uint64_t> LogicalImmediate(uint32_t i)
{
    if ((i & 0x1f800000) != 0x12000000) return {};    // and/orr/eor/ands (immediate)
    bool is64 = i >> 31;
    uint32_t n = (i >> 22) & 1, immr = (i >> 16) & 0x3f, imms = (i >> 10) & 0x3f;
    if (!is64 && n) return {};
    uint32_t combined = (n << 6) | (~imms & 0x3f);
    if (!combined) return {};
    uint32_t len = 31 - __builtin_clz(combined);
    if (len < 1) return {};
    uint32_t size = 1u << len, levels = size - 1;
    uint32_t s = imms & levels, r = immr & levels;
    if (s == levels) return {};
    uint64_t mask = size == 64 ? ~0ull : (1ull << size) - 1;
    uint64_t element = (s + 1 == 64) ? ~0ull : (1ull << (s + 1)) - 1;
    if (r) element = ((element >> r) | (element << (size - r))) & mask;
    uint64_t value = 0;
    for (uint32_t at = 0; at < 64; at += size) value |= element << at;
    return is64 ? value : value & 0xffffffffull;
}

std::optional<BitfieldMove> BitfieldMoveOf(uint32_t i)
{
    if ((i & 0x1f800000) != 0x13000000) return {};    // sbfm/bfm/ubfm
    if (((i >> 29) & 3) == 3) return {};
    bool is64 = i >> 31;
    if (((i >> 22) & 1) != (uint32_t)is64) return {};
    uint32_t bits = is64 ? 64 : 32;
    uint32_t immr = (i >> 16) & 0x3f, imms = (i >> 10) & 0x3f;
    if (immr >= bits || imms >= bits) return {};
    if (imms < immr) return BitfieldMove{ bits - immr, imms + 1, true };
    return BitfieldMove{ immr, imms - immr + 1, false };
}

std::optional<CodeShape> ShapeToFirstReturn(void const* code, uint32_t maxInstructions)
{
    auto insn = static_cast<uint32_t const*>(code);
    uint64_t hash = 0xcbf29ce484222325ull;  // FNV-1a
    uint32_t previous = 0;
    for (uint32_t n = 0; n < maxInstructions; n++) {
        uint32_t word;
        memcpy(&word, insn + n, sizeof(word));
        uint32_t masked = Masked(word, previous);
        for (int b = 0; b < 4; b++) {
            hash = (hash ^ ((masked >> (8 * b)) & 0xff)) * 0x100000001b3ull;
        }
        previous = word;
        if (word == kRet) return CodeShape{ n + 1, hash };
    }
    return {};
}

}
