#include <Engine/EntryUse.h>

#include <Platform/CodeShape.h>

namespace bg3se::mac::engine {

namespace {

enum class Flow : uint8_t { Next, Conditional, Call, Jump, Return, Unrecognised };

// What one instruction does to the general registers. Register 31 (the stack
// pointer or the zero register) is never recorded.
struct Effect
{
    Flow Next{ Flow::Next };
    uint8_t Reads[4]{};
    RegisterUse ReadAs[4]{};
    uint8_t ReadCount{ 0 };
    uint8_t Writes[3]{};
    uint8_t WriteCount{ 0 };

    void Read(uint32_t reg, RegisterUse as)
    {
        if (reg < 31) { Reads[ReadCount] = (uint8_t)reg; ReadAs[ReadCount++] = as; }
    }
    void Write(uint32_t reg)
    {
        if (reg < 31) Writes[WriteCount++] = (uint8_t)reg;
    }
};

constexpr Effect Unknown() { Effect e; e.Next = Flow::Unrecognised; return e; }

RegisterUse Width(bool is64) { return is64 ? RegisterUse::Wide : RegisterUse::Narrow; }

uint32_t Rt2(uint32_t i) { return (i >> 10) & 0x1f; }

// Data processing, immediate operand.
Effect Immediate(uint32_t i)
{
    Effect e;
    bool sf = i >> 31;
    switch ((i >> 23) & 7) {
    case 0: case 1:     // adr, adrp
        e.Write(RegD(i));
        return e;
    case 2:             // add, sub (cmp, cmn, mov to/from sp)
        e.Read(RegN(i), Width(sf));
        e.Write(RegD(i));
        return e;
    case 4:             // and, orr, eor, ands (tst, mov of a bitmask)
        e.Read(RegN(i), Width(sf));
        e.Write(RegD(i));
        return e;
    case 5:             // movn, movz, movk
        switch ((i >> 29) & 3) {
        case 0: case 2: e.Write(RegD(i)); return e;
        case 3: e.Read(RegD(i), Width(sf)); e.Write(RegD(i)); return e;
        default: return Unknown();
        }
    case 6:             // sbfm, bfm, ubfm (the shifts, extensions and bitfield moves)
        if (((i >> 29) & 3) == 3) return Unknown();
        e.Read(RegN(i), Width(sf));
        if (((i >> 29) & 3) == 1) e.Read(RegD(i), Width(sf));       // bfm keeps the other bits
        e.Write(RegD(i));
        return e;
    case 7:             // extr (ror)
        e.Read(RegN(i), Width(sf));
        e.Read(RegM(i), Width(sf));
        e.Write(RegD(i));
        return e;
    default:
        return Unknown();       // add/sub with tags
    }
}

Effect BranchOrSystem(uint32_t i)
{
    Effect e;
    if ((i & 0x7c000000) == 0x14000000) {               // b, bl
        e.Next = i >> 31 ? Flow::Call : Flow::Jump;
        return e;
    }
    if ((i & 0x7e000000) == 0x34000000) {               // cbz, cbnz
        e.Read(RegD(i), Width(i >> 31));
        e.Next = Flow::Conditional;
        return e;
    }
    if ((i & 0x7e000000) == 0x36000000) {               // tbz, tbnz
        e.Read(RegD(i), Width(i >> 31));
        e.Next = Flow::Conditional;
        return e;
    }
    if ((i & 0xff000010) == 0x54000000) {               // b.cond
        e.Next = Flow::Conditional;
        return e;
    }
    if ((i & 0xff9ffc1f) == 0xd61f0000) {               // br, blr, ret
        uint32_t op = (i >> 21) & 3;
        if (op == 3) return Unknown();
        e.Read(RegN(i), RegisterUse::Wide);
        e.Next = op == 0 ? Flow::Jump : op == 1 ? Flow::Call : Flow::Return;
        return e;
    }
    if ((i & 0xfffff01f) == 0xd503201f) return e;       // nop and the other hints
    return Unknown();
}

Effect LoadStore(uint32_t i)
{
    Effect e;
    bool vector = (i >> 26) & 1;
    uint32_t rn = RegN(i), rt = RegD(i);

    if ((i & 0x3a000000) == 0x28000000) {               // ldp, stp
        uint32_t opc = i >> 30, mode = (i >> 23) & 3;
        bool load = (i >> 22) & 1;
        if (!vector && (opc == 3 || (opc == 1 && !load))) return Unknown();     // stgp
        if (!vector) {
            if (load) { e.Write(rt); e.Write(Rt2(i)); }
            else { e.Read(rt, Width(opc == 2)); e.Read(Rt2(i), Width(opc == 2)); }
        }
        e.Read(rn, RegisterUse::Address);
        if (mode == 1 || mode == 3) e.Write(rn);        // post- or pre-index
        return e;
    }
    if ((i & 0x3b000000) == 0x18000000) {               // ldr (literal), prfm (literal)
        if (!vector && (i >> 30) != 3) e.Write(rt);
        return e;
    }
    if ((i & 0x3f000000) == 0x08000000) {               // exclusive and ordered loads and stores, compare-and-swap
        bool ordered = (i >> 23) & 1, load = (i >> 22) & 1, pair = (i >> 21) & 1;
        uint32_t status = RegM(i);
        auto width = Width((i >> 30) == 3);
        if (ordered && pair) {                          // cas: compares with and returns in Rs
            if (Rt2(i) != 31) return Unknown();
            e.Read(status, width);
            e.Read(rt, width);
            e.Read(rn, RegisterUse::Address);
            e.Write(status);
            return e;
        }
        if (pair && Rt2(i) == 31) return Unknown();     // casp
        if (load) {
            e.Write(rt);
            if (pair) e.Write(Rt2(i));
        } else {
            e.Read(rt, width);
            if (pair) e.Read(Rt2(i), width);
            if (!ordered) e.Write(status);              // the exclusive store's verdict
        }
        e.Read(rn, RegisterUse::Address);
        return e;
    }
    if ((i & 0x3a000000) != 0x38000000) return Unknown();   // SIMD structures, tags

    uint32_t size = i >> 30, opc = (i >> 22) & 3;
    bool prefetch = !vector && size == 3 && opc == 2;
    auto data = [&] {
        if (vector || prefetch) return;
        if (opc == 0) e.Read(rt, Width(size == 3));     // a store
        else e.Write(rt);
    };
    if ((i >> 24) & 1) {                                // unsigned offset
        if (!vector && size == 3 && opc == 3) return Unknown();
        data();
        e.Read(rn, RegisterUse::Address);
        return e;
    }
    if (!((i >> 21) & 1)) {
        uint32_t mode = (i >> 10) & 3;
        if (mode == 2) return Unknown();                // unprivileged
        data();
        e.Read(rn, RegisterUse::Address);
        if (mode == 1 || mode == 3) e.Write(rn);        // post- or pre-index
        return e;
    }
    if (((i >> 10) & 3) == 2) {                         // register offset
        data();
        e.Read(rn, RegisterUse::Address);
        e.Read(RegM(i), Width((i >> 13) & 1));
        return e;
    }
    if (((i >> 10) & 3) == 0 && !vector) {              // ldadd, ldclr, ldeor, ldset, swp, ...
        e.Read(RegM(i), Width(size == 3));
        e.Read(rn, RegisterUse::Address);
        e.Write(rt);
        return e;
    }
    return Unknown();                                   // pointer authentication
}

Effect Register(uint32_t i)
{
    Effect e;
    bool sf = i >> 31;
    auto three = [&](bool narrowSources = false) {
        e.Read(RegN(i), Width(sf && !narrowSources));
        e.Read(RegM(i), Width(sf && !narrowSources));
        e.Write(RegD(i));
        return e;
    };
    if (!((i >> 28) & 1)) {                             // logical and add/sub, shifted or extended
        bool extended = ((i >> 24) & 1) && ((i >> 21) & 1);
        if (!extended) return three();
        e.Read(RegN(i), Width(sf));
        e.Read(RegM(i), Width(sf && ((i >> 13) & 3) == 3));     // uxtx, sxtx: the whole register
        e.Write(RegD(i));
        return e;
    }
    uint32_t op2 = (i >> 21) & 0xf;
    if (op2 & 8) {                                      // madd, msub, smaddl, umulh, ...
        e.Read((i >> 10) & 0x1f, Width(sf));
        uint32_t op31 = (i >> 21) & 7;
        return three(op31 == 1 || op31 == 5);           // smaddl, umaddl and their kin multiply 32-bit sources
    }
    switch (op2) {
    case 0:                                             // adc, sbc
        if ((i >> 10) & 0x3f) return Unknown();
        return three();
    case 2:                                             // ccmp, ccmn
        e.Read(RegN(i), Width(sf));
        if (!((i >> 11) & 1)) e.Read(RegM(i), Width(sf));
        return e;
    case 4:                                             // csel, csinc, csinv, csneg
        return three();
    case 6: {
        uint32_t opcode = (i >> 10) & 0x3f;
        if ((i >> 30) & 1) {                            // rbit, rev, clz, cls
            if (RegM(i) != 0 || opcode > 5) return Unknown();
            e.Read(RegN(i), Width(sf));
            e.Write(RegD(i));
            return e;
        }
        if ((i >> 29) & 1) return Unknown();
        bool divide = opcode == 2 || opcode == 3;
        bool shift = opcode >= 8 && opcode <= 11;
        bool crc = opcode >= 16 && opcode <= 23;
        if (!divide && !shift && !crc) return Unknown();
        return three();
    }
    default:
        return Unknown();
    }
}

// Scalar floating point and SIMD. A general register takes part only in the
// conversions between integer and floating point and in the moves between a
// general register and a vector element; everything else in this group works
// on the vector registers alone.
Effect FpSimd(uint32_t i)
{
    Effect e;
    if ((i & 0x5f200000) == 0x1e000000) return Unknown();       // fixed-point conversions
    if ((i & 0x5f20fc00) == 0x1e200000) {                       // integer <-> floating point
        uint32_t opcode = (i >> 16) & 7;
        bool fromInteger = opcode == 2 || opcode == 3 || opcode == 7;
        if (fromInteger) e.Read(RegN(i), Width(i >> 31));
        else e.Write(RegD(i));
        return e;
    }
    if ((i & 0x9fe08400) == 0x0e000400 && !((i >> 29) & 1)) {   // dup, ins, smov, umov
        switch ((i >> 11) & 0xf) {
        case 1: case 3: e.Read(RegN(i), RegisterUse::Wide); return e;   // from a general register
        case 5: case 7: e.Write(RegD(i)); return e;                     // to a general register
        default: return e;
        }
    }
    return e;
}

Effect Decode(uint32_t i)
{
    switch ((i >> 25) & 0xf) {
    case 8: case 9: return Immediate(i);
    case 10: case 11: return BranchOrSystem(i);
    case 4: case 6: case 12: case 14: return LoadStore(i);
    case 5: case 13: return Register(i);
    case 7: case 15: return FpSimd(i);
    default: return Unknown();
    }
}

}

// mov xD, xS (orr xD, xzr, xS): a copy that says nothing yet about what the value is.
bool IsCopy(uint32_t i, uint32_t& to, uint32_t& from)
{
    if ((i & 0xffe0ffe0) != 0xaa0003e0) return false;
    to = RegD(i);
    from = RegM(i);
    return to < 31 && from < 31;
}

EntryUse ScanEntry(std::span<uint32_t const> function, uint64_t va, StackProbe const& probe)
{
    constexpr size_t kLimit = 256;
    constexpr int8_t kNone = -1;
    EntryUse use;
    bool branched = false;
    // An argument is often saved in another register before it is used. Until
    // that register is written again it holds the argument, and how it is used
    // (as an address, as 32 bits) is how the argument is used.
    int8_t holds[31];               // the argument register whose incoming value a register holds
    for (int8_t reg = 0; reg < 31; reg++) holds[reg] = reg < (int8_t)kTrackedRegisters ? reg : kNone;
    bool copiedOnly[kTrackedRegisters]{};       // read so far only by being copied
    size_t count = function.size() < kLimit ? function.size() : kLimit;
    for (size_t n = 0; n < count; n++) {
        uint32_t word = function[n];
        Effect e = Decode(word);
        if (e.Next == Flow::Unrecognised) {
            use.End = ScanEnd::Unrecognised;
            return use;
        }
        use.Scanned++;

        bool isProbe = false;
        if (e.Next == Flow::Call) {
            uint64_t at = va + 4 * n;
            if (uint64_t target = CallTarget(at, word)) {
                isProbe = probe.Stub && target == probe.Stub;
            } else if (n >= 2 && probe.Slot) {
                // adrp xN, slot@PAGE; ldr xN, [xN, slot@PAGEOFF]; blr xN
                uint32_t reg = RegN(word);
                isProbe = RegD(function[n - 1]) == reg && RegD(function[n - 2]) == reg &&
                    PageAddress(at - 8, function[n - 2], function[n - 1]) == probe.Slot;
            }
        }

        uint32_t copyTo = 0, copyFrom = 0;
        bool copy = IsCopy(word, copyTo, copyFrom);
        int8_t copied = copy ? holds[copyFrom] : kNone;
        for (uint8_t r = 0; r < e.ReadCount; r++) {
            int8_t argument = holds[e.Reads[r]];
            if (argument == kNone) continue;
            auto& found = use.Registers[argument];
            if (found == RegisterUse::NotObserved) {
                found = e.ReadAs[r];
                copiedOnly[argument] = copy;
            } else if (copiedOnly[argument] && !copy) {
                // The first use that is not a copy says what the value is.
                if (e.ReadAs[r] != RegisterUse::Wide) found = e.ReadAs[r];
                copiedOnly[argument] = false;
            }
        }
        for (uint8_t w = 0; w < e.WriteCount; w++) {
            uint32_t reg = e.Writes[w];
            holds[reg] = kNone;
            if (reg < kTrackedRegisters && use.Registers[reg] == RegisterUse::NotObserved) {
                use.Registers[reg] = branched ? RegisterUse::OverwrittenOnPath : RegisterUse::Overwritten;
            }
        }
        if (copy) holds[copyTo] = copied;

        switch (e.Next) {
        case Flow::Conditional: branched = true; break;
        case Flow::Call:
            if (!isProbe) { use.End = ScanEnd::Call; return use; }
            break;
        case Flow::Jump: use.End = ScanEnd::Jump; return use;
        case Flow::Return: use.End = ScanEnd::Return; return use;
        default: break;
        }
    }
    use.End = ScanEnd::End;
    return use;
}

ReturnUse ScanReturns(std::span<uint32_t const> function)
{
    ReturnUse use;
    for (size_t n = 0; n < function.size(); n++) {
        if ((function[n] & 0xfffffc1f) != 0xd65f0000) continue;     // ret
        use.Returns++;
        for (size_t back = n; back > 0; back--) {
            Effect e = Decode(function[back - 1]);
            if (e.Next == Flow::Call) { use.AfterCall++; break; }
            if (e.Next != Flow::Next) break;
            bool writes = false;
            for (uint8_t w = 0; w < e.WriteCount; w++) writes = writes || e.Writes[w] == 0;
            if (writes) { use.SetX0++; break; }
        }
    }
    return use;
}

char const* Describe(RegisterUse use)
{
    switch (use) {
    case RegisterUse::NotObserved: return "not observed";
    case RegisterUse::Address: return "read as an address";
    case RegisterUse::Wide: return "read as 64 bits";
    case RegisterUse::Narrow: return "read as 32 bits";
    case RegisterUse::Overwritten: return "overwritten";
    case RegisterUse::OverwrittenOnPath: return "overwritten after a branch";
    }
    return "?";
}

char const* Describe(ScanEnd end)
{
    switch (end) {
    case ScanEnd::Call: return "a call";
    case ScanEnd::Jump: return "a jump";
    case ScanEnd::Return: return "the return";
    case ScanEnd::Unrecognised: return "an instruction the scan does not decode";
    case ScanEnd::End: return "the end of the scan";
    }
    return "?";
}

}
