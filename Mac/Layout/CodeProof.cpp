#include <Layout/CodeProof.h>

#include <Platform/CodeSearch.h>

#include <cstdio>

namespace bg3se::mac::proof {

std::optional<int64_t> UnscaledOffset(uint32_t i)
{
    // Load/store register with a 9-bit signed offset: unscaled (ldur/stur),
    // post-indexed, pre-indexed.
    if ((i & 0x3b200000) != 0x38000000 || ((i >> 10) & 3) == 2) return {};
    int64_t imm = (i >> 12) & 0x1ff;
    if (imm & 0x100) imm -= 0x200;
    return imm;
}

std::optional<uint64_t> MoveValue(uint32_t i)
{
    if (auto value = MovzValue(i)) return value;
    if ((i & 0x7f800000) != 0x12800000) return {};      // movn
    bool is64 = i >> 31;
    uint32_t hw = (i >> 21) & 3;
    if (!is64 && hw > 1) return {};
    uint64_t value = ~((uint64_t)((i >> 5) & 0xffff) << (16 * hw));
    return is64 ? value : value & 0xffffffffull;
}

std::optional<uint64_t> ShiftLeftOf(uint32_t i)
{
    if ((i & 0x1f200000) != 0x0b000000 || ((i >> 22) & 3) != 0) return {};     // add/sub (shifted register), lsl
    return (i >> 10) & 0x3f;
}

// The page an adrp at `va` forms.
uint64_t PageOf(uint64_t va, uint32_t adrp)
{
    uint32_t reg = RegD(adrp);
    return PageAddress(va, adrp, 0x91000000 | (reg << 5) | reg);
}

// The address an adrp and the load after it read: adrp x, page; ldr ?, [x, #offset].
uint64_t LoadedAddress(Code const& f, uint32_t adrpIndex)
{
    uint32_t adrp = f.At(adrpIndex), load = f.At(adrpIndex + 1);
    uint64_t page = PageOf(f.VA(adrpIndex), adrp);
    auto offset = LoadStoreOffset(load);
    if (!page || !offset || RegN(load) != RegD(adrp)) return 0;
    return page + *offset;
}

std::string Hex(uint64_t value)
{
    char text[32];
    snprintf(text, sizeof(text), "%#llx", (unsigned long long)value);
    return text;
}

bool CheckOperand(Code const& f, Expect const& e, std::string& problem)
{
    if (e.Index >= f.Count()) {
        problem = "instruction " + std::to_string(e.Index) + " is past the end of the function";
        return false;
    }
    uint32_t i = f.At(e.Index);
    std::optional<uint64_t> found;
    switch (e.Kind) {
    case Operand::Offset:
        if (auto v = LoadStoreOffset(i)) found = *v;
        break;
    case Operand::ByteOffset:
        if ((i & 0xff800000) == 0x39000000) found = *LoadStoreOffset(i);
        break;
    case Operand::Unscaled:
        if (auto v = UnscaledOffset(i)) found = (uint64_t)*v;
        break;
    case Operand::Add:
        if (auto v = AddImmediate(i)) found = *v;
        break;
    case Operand::Move:
        found = MoveValue(i);
        break;
    case Operand::Mask:
        found = LogicalImmediate(i);
        break;
    case Operand::ShiftLeft:
        found = ShiftLeftOf(i);
        break;
    case Operand::StoresZero:
        if ((LoadStoreOffset(i) || UnscaledOffset(i) || PairOffset(i)) && RegD(i) == 31) return true;
        problem = "instruction " + std::to_string(e.Index) + " does not store zero";
        return false;
    case Operand::ShiftLeftBy:
        if (auto move = BitfieldMoveOf(i); move && move->Insert) found = move->Lsb;
        break;
    case Operand::PairOffset:
        if (auto v = PairOffset(i)) found = (uint64_t)(int64_t)*v;
        break;
    case Operand::ShiftRight:
        if (auto move = BitfieldMoveOf(i); move && !move->Insert && (i & 0x60000000) == 0x40000000) found = move->Lsb;
        break;
    case Operand::MoveWide:
        if (e.Index + 1u < f.Count()) found = MovzMovkValue(i, f.At(e.Index + 1));
        break;
    case Operand::TestsBit:
        if ((i & 0x7e000000) == 0x36000000) found = ((i >> 31) << 5) | ((i >> 19) & 0x1f);
        break;
    case Operand::AtomicAdd32:
        // ldadd, ldadda, ldaddl, ldaddal with a word: size 0b10.
        if ((i & 0xff20fc00) == 0xb8200000) return true;
        problem = "instruction " + std::to_string(e.Index) + " is not an atomic addition to a 32-bit word";
        return false;
    case Operand::CallsImport:
        if (CallsImport(f.Image, f.VA(e.Index), e.Name)) return true;
        problem = "instruction " + std::to_string(e.Index) + " does not call " + e.Name;
        return false;
    case Operand::JumpsToImport:
        if (uint64_t target = JumpTarget(f.VA(e.Index), i); target && f.Image.StubName(target) == e.Name) return true;
        problem = "instruction " + std::to_string(e.Index) + " does not jump to " + e.Name;
        return false;
    case Operand::Calls:
    case Operand::JumpsTo: {
        uint64_t target = e.Kind == Operand::Calls ? CallTarget(f.VA(e.Index), i) : JumpTarget(f.VA(e.Index), i);
        if (target && target == f.Image.FindVA(e.Name)) return true;
        problem = "instruction " + std::to_string(e.Index) + " does not go to " + e.Name;
        return false;
    }
    }
    if (!found) {
        problem = "instruction " + std::to_string(e.Index) + " is not of the expected kind";
        return false;
    }
    if (*found != e.Value) {
        problem = "instruction " + std::to_string(e.Index) + " has " + Hex(*found) + " where the Mac type has " + Hex(e.Value);
        return false;
    }
    return true;
}

}
