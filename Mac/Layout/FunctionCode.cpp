#include <Layout/FunctionCode.h>

#include <Platform/CodeShape.h>

namespace bg3se::mac {

namespace {

bool IsCall(uint32_t i) { return (i & 0xfc000000) == 0x94000000 || (i & 0xfffffc1f) == 0xd63f0000; }   // bl, blr
bool IsJump(uint32_t i) { return (i & 0xfc000000) == 0x14000000; }                                      // b
bool IsCondBranch(uint32_t i) { return (i & 0xff000010) == 0x54000000; }                                // b.cond
bool IsCompareBranch(uint32_t i) { return (i & 0x7e000000) == 0x34000000; }                             // cbz, cbnz
bool IsTestBranch(uint32_t i) { return (i & 0x7e000000) == 0x36000000; }                                // tbz, tbnz
bool IsReturn(uint32_t i) { return (i & 0xfffffc1f) == 0xd65f0000; }                                    // ret
bool IsIndirectJump(uint32_t i) { return (i & 0xfffffc1f) == 0xd61f0000; }                              // br
// brk and udf: what the compiler puts after a call that does not return.
bool IsTrap(uint32_t i) { return (i & 0xffe0001f) == 0xd4200000 || (i & 0xffff0000) == 0; }

int64_t SignExtend(uint32_t value, unsigned bits)
{
    int64_t v = value;
    if (v & (1ll << (bits - 1))) v -= (1ll << bits);
    return v;
}

// Where a direct branch at `va` goes, or 0.
uint64_t BranchTarget(uint64_t va, uint32_t i)
{
    if (IsJump(i)) return va + (uint64_t)(SignExtend(i & 0x03ffffff, 26) * 4);
    if (IsCondBranch(i) || IsCompareBranch(i)) return va + (uint64_t)(SignExtend((i >> 5) & 0x7ffff, 19) * 4);
    if (IsTestBranch(i)) return va + (uint64_t)(SignExtend((i >> 5) & 0x3fff, 14) * 4);
    return 0;
}

}

bool MayWriteRegister(uint32_t i, uint32_t reg)
{
    uint32_t rt = i & 0x1f, rn = (i >> 5) & 0x1f, rt2 = (i >> 10) & 0x1f, rs = (i >> 16) & 0x1f;

    // A call leaves the caller-saved registers and the link register undefined.
    if (IsCall(i)) return reg <= 18 || reg == 30;
    if (IsJump(i) || IsCondBranch(i) || IsCompareBranch(i) || IsTestBranch(i) || IsReturn(i) || IsIndirectJump(i)) return false;

    bool simd = (i >> 26) & 1;
    // Load/store pair: no-allocate, post-index, signed offset, pre-index.
    if ((i & 0x3a000000) == 0x28000000) {
        bool load = (i >> 22) & 1;
        uint32_t kind = (i >> 23) & 3;
        if ((kind == 1 || kind == 3) && rn == reg) return true;     // writeback
        return load && !simd && (rt == reg || rt2 == reg);
    }
    // Load/store register, unsigned immediate offset.
    if ((i & 0x3b000000) == 0x39000000) {
        bool load = ((i >> 22) & 3) != 0;
        return load && !simd && rt == reg;
    }
    // Load/store register: unscaled, post-index, unprivileged, pre-index,
    // register offset, and the atomic memory operations (ldadd, swp, ...).
    if ((i & 0x3b000000) == 0x38000000) {
        bool registerForm = (i >> 21) & 1;
        uint32_t kind = (i >> 10) & 3;
        if (registerForm && kind == 0) return !simd && rt == reg;   // atomic: the old value is loaded into Rt
        if (!registerForm && (kind == 1 || kind == 3) && rn == reg) return true;     // writeback
        bool load = ((i >> 22) & 3) != 0;
        return load && !simd && rt == reg;
    }
    // Load literal.
    if ((i & 0x3b000000) == 0x18000000) return !simd && rt == reg;
    // Exclusive loads and stores, compare-and-swap: the status or the old
    // value goes to Rs, a load to Rt (and Rt2 for a pair).
    if ((i & 0x3f000000) == 0x08000000) return rt == reg || rs == reg || rt2 == reg;

    // Everything else is taken to write the register its low five bits name
    // (data processing does; a compare names the zero register there).
    return rt == reg;
}

FunctionCode::FunctionCode(GameImage const& image, uint64_t begin, uint64_t end)
    : begin_(begin), end_(end < begin ? begin : end)
{
    size_t count = (end_ - begin_) / 4;
    words_.resize(count);
    for (size_t n = 0; n < count; n++) words_[n] = image.Word(begin_ + 4 * n);
    Propagate();
}

void FunctionCode::Propagate()
{
    size_t count = words_.size();
    for (uint32_t i : words_) {
        if (IsIndirectJump(i)) return;
    }

    entry_.assign(count, {});
    std::vector<bool> reached(count, false), queued(count, false);
    std::vector<size_t> work;
    auto flow = [&](size_t to, Registers const& state) {
        if (to >= count) return;
        if (!reached[to]) {
            reached[to] = true;
            entry_[to] = state;
        } else {
            // Where two paths meet, a register keeps its value only when both agree.
            Registers met = entry_[to];
            for (uint32_t reg = 0; reg < 31; reg++) {
                uint32_t bit = 1u << reg;
                if ((met.Known & bit) && (!(state.Known & bit) || state.Value[reg] != met.Value[reg])) {
                    met.Known &= ~bit;
                    met.Value[reg] = 0;
                }
            }
            if (met == entry_[to]) return;
            entry_[to] = met;
        }
        if (!queued[to]) {
            queued[to] = true;
            work.push_back(to);
        }
    };

    if (count) flow(0, {});
    while (!work.empty()) {
        size_t n = work.back();
        work.pop_back();
        queued[n] = false;
        uint32_t i = words_[n];
        Registers state = entry_[n];

        uint32_t rd = RegD(i);
        bool is64 = i >> 31;
        auto set = [&](uint64_t value) {
            state.Known |= 1u << rd;
            state.Value[rd] = is64 ? value : (value & 0xffffffffull);
        };
        auto forget = [&](uint32_t reg) {
            state.Known &= ~(1u << reg);
            state.Value[reg] = 0;
        };
        if (rd != 31 && MovzValue(i)) {
            set(*MovzValue(i));
        } else if (rd != 31 && (i & 0x7f800000) == 0x12800000 && (is64 || ((i >> 21) & 3) < 2)) {     // movn
            set(~((uint64_t)((i >> 5) & 0xffff) << (16 * ((i >> 21) & 3))));
        } else if (rd != 31 && (i & 0x7f800000) == 0x72800000 && (is64 || ((i >> 21) & 3) < 2)) {     // movk
            if (state.Known & (1u << rd)) {
                uint32_t lane = 16 * ((i >> 21) & 3);
                set((state.Value[rd] & ~(0xffffull << lane)) | ((uint64_t)((i >> 5) & 0xffff) << lane));
            }
        } else if (rd != 31 && (i & 0x7f8003e0) == 0x320003e0 && LogicalImmediate(i)) {                // mov rd, #bitmask
            set(*LogicalImmediate(i));
        } else {
            for (uint32_t reg = 0; reg < 31; reg++) {
                if (MayWriteRegister(i, reg)) forget(reg);
            }
        }

        uint64_t va = begin_ + 4 * n;
        if (uint64_t target = BranchTarget(va, i); target && Contains(target)) flow((target - begin_) / 4, state);
        if (!IsJump(i) && !IsReturn(i) && !IsTrap(i)) flow(n + 1, state);
    }
    // An instruction no path reaches has no values.
    for (size_t n = 0; n < count; n++) {
        if (!reached[n]) entry_[n] = {};
    }
}

std::optional<uint64_t> FunctionCode::ConstantAt(uint64_t site, uint32_t reg) const
{
    if (!Contains(site) || reg > 30 || entry_.empty()) return {};
    auto const& state = entry_[(site - begin_) / 4];
    if (!(state.Known & (1u << reg))) return {};
    return state.Value[reg];
}

std::vector<uint64_t> FunctionCode::CallsTo(uint64_t callee) const
{
    std::vector<uint64_t> sites;
    for (uint64_t va = begin_; va < end_; va += 4) {
        if (CallTarget(va, At(va)) == callee) sites.push_back(va);
    }
    return sites;
}

}
