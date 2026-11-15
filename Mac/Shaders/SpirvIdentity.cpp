#include <stdafx.h>
#include <Shaders/SpirvIdentity.h>

extern "C" {
#include <Shaders/identity.h>
}

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace bg3se::mac {

namespace {

struct OperandPattern
{
    uint16_t Opcode;
    char const* Pattern;
};

constexpr OperandPattern kPatterns[] = {
#include <spirv_operands.inc>
};

enum : uint16_t
{
    OpSourceContinued = 2, OpSource = 3, OpSourceExtension = 4, OpName = 5, OpMemberName = 6, OpString = 7, OpLine = 8,
    OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16,
    OpCapability = 17, OpTypeInt = 21, OpFunction = 54, OpVariable = 59, OpFunctionEnd = 56, OpDecorate = 71, OpMemberDecorate = 72,
    OpSwitch = 251, OpNoLine = 317, OpModuleProcessed = 330, OpDecorateString = 5632, OpMemberDecorateString = 5633,
};

constexpr uint32_t kMagic = 0x07230203;
// Declarations and calls nest a few levels deep in a real shader.
constexpr uint32_t kMaxDepth = 256;
// Volatile, Aligned, Nontemporal: the memory access bits whose parameters are literals.
constexpr uint32_t kKnownMemoryAccess = 0x7;

using NodeHash = std::array<uint8_t, 16>;

// What a hash is taken of: tagged words, so that two different shapes cannot
// serialise to the same bytes.
class Digest
{
public:
    Digest() { CC_SHA256_Init(&ctx_); }

    void Tag(char tag) { CC_SHA256_Update(&ctx_, &tag, 1); }
    void Word(uint32_t word) { CC_SHA256_Update(&ctx_, &word, sizeof(word)); }
    void Bytes(void const* data, size_t size)
    {
        Word((uint32_t)size);
        CC_SHA256_Update(&ctx_, data, (CC_LONG)size);
    }
    void Node(NodeHash const& hash) { CC_SHA256_Update(&ctx_, hash.data(), (CC_LONG)hash.size()); }

    SpirvHash Full()
    {
        SpirvHash out;
        CC_SHA256_Final(out.data(), &ctx_);
        return out;
    }
    NodeHash Short()
    {
        auto full = Full();
        NodeHash out;
        memcpy(out.data(), full.data(), out.size());
        return out;
    }

private:
    CC_SHA256_CTX ctx_;
};

enum class OperandKind : uint8_t { Literal, Id, ResultType, Result };

struct Operand
{
    OperandKind Kind;
    uint32_t Value;
};

struct Instruction
{
    uint16_t Opcode{ 0 };
    uint32_t const* Words{ nullptr };     // after the first word
    uint32_t Count{ 0 };
};

char const* PatternOf(uint16_t opcode)
{
    auto it = std::lower_bound(std::begin(kPatterns), std::end(kPatterns), opcode,
        [](OperandPattern const& p, uint16_t op) { return p.Opcode < op; });
    return it != std::end(kPatterns) && it->Opcode == opcode ? it->Pattern : nullptr;
}

// The operands of an instruction by its pattern. False when it does not fit.
bool Decode(Instruction const& inst, std::vector<Operand>& out)
{
    out.clear();
    char const* pattern = PatternOf(inst.Opcode);
    if (!pattern) return false;

    uint32_t at = 0;
    auto push = [&](OperandKind kind) { out.push_back({ kind, inst.Words[at++] }); };
    auto rest = [&](OperandKind kind) { while (at < inst.Count) push(kind); };

    for (char const* p = pattern; *p;) {
        char letter = *p++;
        char quantifier = (*p == '?' || *p == '*') ? *p++ : 0;
        uint32_t repeat = quantifier == '*' ? UINT32_MAX : 1;
        for (uint32_t n = 0; n < repeat; n++) {
            if (at >= inst.Count) {
                if (quantifier) break;
                return false;
            }
            switch (letter) {
            case 'T': push(OperandKind::ResultType); break;
            case 'R': push(OperandKind::Result); break;
            case 'i': push(OperandKind::Id); break;
            case 'l': push(OperandKind::Literal); break;
            case 's': {
                // Words up to and including the one that holds the terminator.
                bool terminated = false;
                while (at < inst.Count && !terminated) {
                    uint32_t word = inst.Words[at];
                    terminated = !(word & 0xff) || !(word & 0xff00) || !(word & 0xff0000) || !(word & 0xff000000);
                    push(OperandKind::Literal);
                }
                if (!terminated) return false;
                break;
            }
            case 'c': case 'D': case 'E': case 'L':
                rest(OperandKind::Literal);
                break;
            case 'M':
                if (inst.Words[at] & ~kKnownMemoryAccess) return false;
                rest(OperandKind::Literal);
                break;
            case 'I':
                push(OperandKind::Literal);
                rest(OperandKind::Id);
                break;
            case 'P':
                if (inst.Count - at < 2) return false;
                push(OperandKind::Literal);
                push(OperandKind::Id);
                break;
            default:
                return false;
            }
        }
    }
    return at == inst.Count;
}

bool IsDebug(uint16_t opcode)
{
    switch (opcode) {
    case OpSourceContinued: case OpSource: case OpSourceExtension: case OpName: case OpMemberName: case OpString:
    case OpLine: case OpNoLine: case OpModuleProcessed:
        return true;
    default:
        return false;
    }
}

class Module
{
public:
    bool Hash(std::span<uint8_t const> bytes, SpirvHash& hash, std::string& why)
    {
        if (!Read(bytes) || !Index()) {
            why = why_;
            return false;
        }

        // The entry points, in an order that does not depend on the module's.
        std::vector<NodeHash> entries;
        for (uint32_t index : entryPoints_) {
            auto entry = HashEntryPoint(insts_[index]);
            if (!entry) {
                why = why_;
                return false;
            }
            entries.push_back(*entry);
        }
        if (entries.empty()) {
            why = "the module has no entry point";
            return false;
        }
        std::sort(entries.begin(), entries.end());

        // A type or a constant is what it is made of; a variable is a place.
        // Two variables nothing tells apart (same type, storage class and
        // decorations) cannot be told apart in the functions that use them.
        std::sort(variables_.begin(), variables_.end());
        for (size_t i = 1; i < variables_.size(); i++) {
            if (variables_[i].first == variables_[i - 1].first) {
                why = "two global variables that nothing tells apart";
                return false;
            }
        }

        Digest d;
        d.Tag('M');
        d.Bytes(memoryModel_.data(), memoryModel_.size() * sizeof(uint32_t));
        d.Word((uint32_t)entries.size());
        for (auto const& e : entries) d.Node(e);
        hash = d.Full();
        return true;
    }

private:
    bool Fail(std::string why)
    {
        if (why_.empty()) why_ = std::move(why);
        return false;
    }

    bool Read(std::span<uint8_t const> bytes)
    {
        if (bytes.size() < 20 || bytes.size() % 4) return Fail("not a whole number of words");
        words_.resize(bytes.size() / 4);
        memcpy(words_.data(), bytes.data(), bytes.size());
        if (words_[0] != kMagic) return Fail("not a SPIR-V module");
        bound_ = words_[3];
        // Every id is the result of an instruction, so there are fewer ids than words.
        if (!bound_ || bound_ > words_.size()) return Fail("the id bound is out of range");

        for (size_t at = 5; at < words_.size();) {
            uint32_t count = words_[at] >> 16;
            if (!count || at + count > words_.size()) return Fail("an instruction runs past the module");
            insts_.push_back({ (uint16_t)(words_[at] & 0xffff), &words_[at + 1], count - 1 });
            at += count;
        }
        return true;
    }

    // Where every id is defined, what decorates it, where the functions are.
    bool Index()
    {
        definition_.assign(bound_, UINT32_MAX);
        function_.assign(bound_, UINT32_MAX);
        local_.assign(bound_, UINT32_MAX);
        decorations_.resize(bound_);
        globalHash_.resize(bound_);
        functionHash_.resize(bound_);
        state_.assign(bound_, 0);

        uint32_t inFunction = UINT32_MAX;
        std::vector<Operand> ops;
        for (uint32_t i = 0; i < insts_.size(); i++) {
            auto const& inst = insts_[i];
            if (IsDebug(inst.Opcode) || inst.Opcode == OpCapability || inst.Opcode == OpExtension) continue;
            if (!Decode(inst, ops)) return Fail("instruction " + std::to_string(inst.Opcode) + " is not one this code reads");

            for (auto const& op : ops) {
                if (op.Kind != OperandKind::Literal && op.Value >= bound_) return Fail("an id is past the bound");
            }
            for (auto const& op : ops) {
                if (op.Kind != OperandKind::Result) continue;
                if (definition_[op.Value] != UINT32_MAX) return Fail("an id is defined twice");
                definition_[op.Value] = i;
            }

            switch (inst.Opcode) {
            case OpFunction:
                if (inFunction != UINT32_MAX) return Fail("a function inside a function");
                inFunction = ops[1].Value;
                function_[inFunction] = i;
                firstFunction_ = std::min(firstFunction_, i);
                break;
            case OpFunctionEnd:
                if (inFunction == UINT32_MAX) return Fail("a function end outside a function");
                inFunction = UINT32_MAX;
                break;
            case OpDecorate: case OpDecorateString:
                // Everything after the target.
                decorations_[ops[0].Value].push_back({ inst.Opcode, inst.Words + 1, inst.Count - 1 });
                break;
            case OpMemberDecorate: case OpMemberDecorateString:
                decorations_[ops[0].Value].push_back({ inst.Opcode, inst.Words + 1, inst.Count - 1 });
                break;
            case OpEntryPoint:
                entryPoints_.push_back(i);
                break;
            case OpExecutionMode:
                executionModes_.push_back(i);
                break;
            case OpMemoryModel:
                memoryModel_.assign(inst.Words, inst.Words + inst.Count);
                break;
            default:
                break;
            }
        }
        if (inFunction != UINT32_MAX) return Fail("a function without an end");
        return true;
    }

    // The decorations of an id, in an order that does not depend on the module's.
    void AddDecorations(Digest& d, uint32_t id)
    {
        auto const& list = decorations_[id];
        if (list.empty()) {
            d.Word(0);
            return;
        }
        std::vector<NodeHash> hashes;
        hashes.reserve(list.size());
        for (auto const& deco : list) {
            Digest one;
            one.Tag('d');
            one.Word(deco.Opcode);
            one.Bytes(deco.Words, deco.Count * sizeof(uint32_t));
            hashes.push_back(one.Short());
        }
        std::sort(hashes.begin(), hashes.end());
        d.Word((uint32_t)hashes.size());
        for (auto const& h : hashes) d.Node(h);
    }

    // A type, constant, global variable or imported instruction set, by what it is made of.
    std::optional<NodeHash> HashGlobal(uint32_t id)
    {
        if (globalHash_[id]) return globalHash_[id];
        uint32_t index = definition_[id];
        if (index == UINT32_MAX) { Fail("an id is used but never defined"); return {}; }
        if (index >= firstFunction_) { Fail("a function's id is used outside it"); return {}; }
        if (state_[id]) { Fail("a declaration refers to itself"); return {}; }
        if (depth_ >= kMaxDepth) { Fail("declarations nested too deep"); return {}; }
        state_[id] = 1;
        depth_++;

        auto const& inst = insts_[index];
        std::vector<Operand> ops;
        Decode(inst, ops);

        Digest d;
        d.Tag('G');
        d.Word(inst.Opcode);
        d.Word((uint32_t)ops.size());
        for (auto const& op : ops) {
            switch (op.Kind) {
            case OperandKind::Literal:
                d.Tag('l');
                d.Word(op.Value);
                break;
            case OperandKind::Result:
                d.Tag('r');
                break;
            default: {
                auto operand = HashGlobal(op.Value);
                if (!operand) return {};
                d.Tag('i');
                d.Node(*operand);
            }
            }
        }
        AddDecorations(d, id);
        state_[id] = 0;
        depth_--;
        globalHash_[id] = d.Short();
        if (inst.Opcode == OpVariable) variables_.emplace_back(*globalHash_[id], id);
        return globalHash_[id];
    }

    // The selector of a switch decides how wide its case literals are; one word is assumed.
    bool SwitchIsOneWord(uint32_t selector)
    {
        uint32_t index = definition_[selector];
        if (index == UINT32_MAX) return false;
        std::vector<Operand> ops;
        Decode(insts_[index], ops);
        for (auto const& op : ops) {
            if (op.Kind != OperandKind::ResultType) continue;
            uint32_t type = definition_[op.Value];
            return type != UINT32_MAX && insts_[type].Opcode == OpTypeInt && insts_[type].Count == 3 && insts_[type].Words[1] <= 32;
        }
        return false;
    }

    std::optional<NodeHash> HashFunction(uint32_t id)
    {
        if (functionHash_[id]) return functionHash_[id];
        uint32_t begin = function_[id];
        if (begin == UINT32_MAX) { Fail("a call to something that is not a function"); return {}; }
        if (state_[id]) { Fail("a function calls itself"); return {}; }
        state_[id] = 1;

        if (depth_ >= kMaxDepth) { Fail("calls nested too deep"); return {}; }
        depth_++;

        // The ids this function defines, numbered in the order it defines them.
        // A function it calls is hashed when the call is met: that numbers the
        // callee's ids, which are not this function's, and leaves these alone.
        uint32_t end = begin;
        std::vector<Operand> ops;
        while (insts_[end].Opcode != OpFunctionEnd) end++;

        std::vector<uint32_t> defined;
        for (uint32_t i = begin; i <= end; i++) {
            if (IsDebug(insts_[i].Opcode)) continue;
            Decode(insts_[i], ops);
            for (auto const& op : ops) {
                if (op.Kind != OperandKind::Result) continue;
                local_[op.Value] = (uint32_t)defined.size();
                defined.push_back(op.Value);
            }
        }

        Digest d;
        d.Tag('F');
        bool ok = true;
        for (uint32_t i = begin; i <= end && ok; i++) {
            auto const& inst = insts_[i];
            if (IsDebug(inst.Opcode)) continue;
            Decode(inst, ops);

            if (inst.Opcode == OpSwitch && !SwitchIsOneWord(ops[0].Value)) {
                ok = Fail("a switch on a selector wider than a word");
                break;
            }
            if (inst.Opcode == OpExtInst) {
                // Its operands are ids only in the instruction set this code knows.
                auto set = definition_[ops[2].Value];
                static constexpr char kGlsl[] = "GLSL.std.450";
                if (set == UINT32_MAX || insts_[set].Opcode != OpExtInstImport
                    || (insts_[set].Count - 1) * 4 < sizeof(kGlsl)
                    || memcmp(insts_[set].Words + 1, kGlsl, sizeof(kGlsl)) != 0) {
                    ok = Fail("an extended instruction set other than GLSL.std.450");
                    break;
                }
            }

            d.Tag('I');
            d.Word(inst.Opcode);
            d.Word((uint32_t)ops.size());
            for (auto const& op : ops) {
                switch (op.Kind) {
                case OperandKind::Literal:
                    d.Tag('l');
                    d.Word(op.Value);
                    break;
                case OperandKind::Result:
                    d.Tag('r');
                    AddDecorations(d, op.Value);
                    break;
                default:
                    if (local_[op.Value] != UINT32_MAX) {
                        d.Tag('L');
                        d.Word(local_[op.Value]);
                    } else if (function_[op.Value] != UINT32_MAX) {
                        if (auto callee = HashFunction(op.Value)) {
                            d.Tag('F');
                            d.Node(*callee);
                        } else {
                            ok = false;
                        }
                    } else if (auto global = HashGlobal(op.Value)) {
                        d.Tag('G');
                        d.Node(*global);
                    } else {
                        ok = false;
                    }
                }
                if (!ok) break;
            }
        }

        for (uint32_t local : defined) local_[local] = UINT32_MAX;
        state_[id] = 0;
        depth_--;
        if (!ok) return {};
        functionHash_[id] = d.Short();
        return functionHash_[id];
    }

    std::optional<NodeHash> HashEntryPoint(Instruction const& inst)
    {
        std::vector<Operand> ops;
        Decode(inst, ops);      // model, function, name words, interface ids
        uint32_t function = ops[1].Value;
        auto body = HashFunction(function);
        if (!body) return {};

        Digest d;
        d.Tag('E');
        d.Word(ops[0].Value);
        d.Node(*body);
        std::vector<NodeHash> interface;
        std::vector<uint32_t> name;
        for (size_t i = 2; i < ops.size(); i++) {
            if (ops[i].Kind == OperandKind::Literal) {
                name.push_back(ops[i].Value);
                continue;
            }
            auto variable = HashGlobal(ops[i].Value);
            if (!variable) return {};
            interface.push_back(*variable);
        }
        d.Bytes(name.data(), name.size() * sizeof(uint32_t));
        std::sort(interface.begin(), interface.end());
        d.Word((uint32_t)interface.size());
        for (auto const& v : interface) d.Node(v);

        std::vector<NodeHash> modes;
        for (uint32_t index : executionModes_) {
            auto const& mode = insts_[index];
            if (mode.Words[0] != function) continue;
            Digest one;
            one.Tag('m');
            one.Bytes(mode.Words + 1, (mode.Count - 1) * sizeof(uint32_t));
            modes.push_back(one.Short());
        }
        std::sort(modes.begin(), modes.end());
        d.Word((uint32_t)modes.size());
        for (auto const& m : modes) d.Node(m);
        return d.Short();
    }

    std::vector<uint32_t> words_;
    std::vector<Instruction> insts_;
    uint32_t bound_{ 0 };
    uint32_t firstFunction_{ UINT32_MAX };
    std::vector<uint32_t> definition_;      // id -> instruction
    std::vector<uint32_t> function_;        // id -> its OpFunction, for function ids
    std::vector<uint32_t> local_;           // id -> its number in the function being hashed
    std::vector<std::vector<Instruction>> decorations_;
    std::vector<std::optional<NodeHash>> globalHash_;
    std::vector<std::optional<NodeHash>> functionHash_;
    std::vector<uint8_t> state_;            // being hashed: a cycle is refused
    std::vector<uint32_t> entryPoints_;
    std::vector<uint32_t> executionModes_;
    std::vector<uint32_t> memoryModel_;
    std::vector<std::pair<NodeHash, uint32_t>> variables_;     // every global variable reached, by hash
    uint32_t depth_{ 0 };
    std::string why_;
};

uint32_t ReadU32(uint8_t const* p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

constexpr size_t kStageHeader = 50;     // "SHD8", three u16, the module's size, a word, its 16-byte hash, 16 bytes

}

bool SpirvModuleHash(std::span<uint8_t const> module, SpirvHash& hash, std::string& why)
{
    Module m;
    return m.Hash(module, hash, why);
}

bool SpirvIdOperands(uint16_t opcode, std::span<uint32_t const> operands, int& result, std::vector<uint32_t>& ids)
{
    result = -1;
    ids.clear();
    std::vector<Operand> decoded;
    if (!Decode({ opcode, operands.data(), (uint32_t)operands.size() }, decoded)) return false;
    for (size_t i = 0; i < decoded.size(); i++) {
        if (decoded[i].Kind == OperandKind::Result) result = (int)i;
        else if (decoded[i].Kind != OperandKind::Literal) ids.push_back((uint32_t)i);
    }
    return true;
}

bool SpirvElementMembersRead(std::span<uint8_t const> module, uint32_t variable, std::vector<uint32_t>& members)
{
    constexpr uint16_t kConstant = 43, kAccessChain = 65, kInBoundsAccessChain = 66;
    members.clear();
    if (module.size() < 20 || module.size() % 4) return false;
    std::vector<uint32_t> words(module.size() / 4);
    memcpy(words.data(), module.data(), module.size());
    if (words[0] != kMagic) return false;

    constexpr uint16_t kLoad = 61, kCompositeExtract = 81;
    std::vector<uint32_t> wholeTypes;
    std::vector<std::pair<uint32_t, uint32_t>> constants;       // a whole number's id and value
    std::vector<uint32_t> elements, loaded;                     // pointers to one element; one element's value
    std::vector<Operand> operands;
    auto has = [](std::vector<uint32_t> const& ids, uint32_t id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
    auto add = [&](uint32_t member) { if (!has(members, member)) members.push_back(member); };
    bool inFunction = false;
    for (size_t at = 5; at < words.size();) {
        uint32_t count = words[at] >> 16;
        uint16_t opcode = (uint16_t)(words[at] & 0xffff);
        if (!count || at + count > words.size()) return false;
        if (opcode == OpTypeInt && count == 4) wholeTypes.push_back(words[at + 1]);
        if (opcode == kConstant && count == 4 && has(wholeTypes, words[at + 1])) constants.push_back({ words[at + 2], words[at + 3] });
        if (opcode == OpFunction) inFunction = true;
        if (inFunction && !IsDebug(opcode)) {
            if (!Decode({ opcode, words.data() + at + 1, count - 1 }, operands)) return false;
            for (size_t i = 0; i < operands.size(); i++) {
                if (operands[i].Kind != OperandKind::Id) continue;
                uint32_t id = operands[i].Value;
                if (id == variable) {
                    // Only as the base of a chain: its type, its result, the variable, then the array (the
                    // block's one member) and the element; and the member, or the whole element is meant.
                    bool chain = (opcode == kAccessChain || opcode == kInBoundsAccessChain) && i == 2 && operands.size() >= 5;
                    if (!chain) return false;
                    if (operands.size() == 5) {
                        elements.push_back(operands[1].Value);
                        continue;
                    }
                    auto member = std::find_if(constants.begin(), constants.end(), [&](auto const& c) { return c.first == operands[5].Value; });
                    if (member == constants.end()) return false;
                    add(member->second);
                } else if (has(elements, id)) {
                    // A whole element is only ever loaded,
                    if (opcode != kLoad || i != 2) return false;
                    loaded.push_back(operands[1].Value);
                } else if (has(loaded, id)) {
                    // and of what was loaded only single members are taken.
                    if (opcode != kCompositeExtract || i != 2 || operands.size() < 4 || operands[3].Kind != OperandKind::Literal) return false;
                    add(operands[3].Value);
                }
            }
        }
        at += count;
    }
    std::sort(members.begin(), members.end());
    return true;
}

bool ParseContainer(std::span<uint8_t const> c, ContainerParts& parts)
{
    if (c.size() < 16 || memcmp(c.data(), "BSHD", 4) != 0) return false;
    parts.Version = ReadU32(c.data() + 4);
    uint64_t padded = ReadU32(c.data() + 8), length = ReadU32(c.data() + 12);
    if (length > padded || 16 + padded + 28 > c.size()) return false;
    parts.Name = c.subspan(16, length);

    uint64_t at = 16 + padded;
    uint32_t sizes[7];
    for (auto& size : sizes) {
        size = ReadU32(c.data() + at);
        at += 4;
    }
    uint64_t total = at;
    for (uint32_t size : sizes) total += size;
    if (total != c.size()) return false;

    parts.Reflection = c.subspan(at, sizes[0]);
    at += sizes[0];
    for (int i = 0; i < 6; i++) {
        parts.Stages[i] = c.subspan(at, sizes[i + 1]);
        at += sizes[i + 1];
    }
    return true;
}

bool SplitVulkanStage(std::span<uint8_t const> stage, VulkanStageHeader& header, std::span<uint8_t const>& module)
{
    if (stage.size() < kStageHeader || memcmp(stage.data(), "SHD8", 4) != 0) return false;
    if (ReadU32(stage.data() + 10) != stage.size() - kStageHeader) return false;
    // Everything but the size (10..13) and the hash (18..33).
    memcpy(header.data(), stage.data() + 4, 6);
    memcpy(header.data() + 6, stage.data() + 14, 4);
    memcpy(header.data() + 10, stage.data() + 34, 16);
    module = stage.subspan(kStageHeader);
    return true;
}

std::vector<std::span<uint8_t const>> VulkanStageModules(std::span<uint8_t const> stage)
{
    // The toolkit's earlier chunk: three words of its own, then one module to the end.
    constexpr size_t kOldHeader = 10;
    if (stage.size() > kOldHeader + 20 && memcmp(stage.data(), "SHD7", 4) == 0) {
        auto module = stage.subspan(kOldHeader);
        if (module.size() % 4 == 0 && ReadU32(module.data()) == kMagic) return { module };
        return {};
    }
    if (stage.size() < kStageHeader || memcmp(stage.data(), "SHD8", 4) != 0) return {};
    uint64_t first = ReadU32(stage.data() + 10), second = ReadU32(stage.data() + 14), rest = stage.size() - kStageHeader;
    if (!first || first + second != rest) return {};
    if (!second) return { stage.subspan(kStageHeader) };
    return { stage.subspan(kStageHeader, first), stage.subspan(kStageHeader + first, second) };
}

SpirvHash ContainerHash(std::span<uint8_t const> container, uint32_t* structural)
{
    if (structural) *structural = 0;
    Digest d;
    ContainerParts parts;
    if (!ParseContainer(container, parts)) {
        d.Tag('R');
        d.Bytes(container.data(), container.size());
        return d.Full();
    }

    d.Tag('C');
    d.Word(parts.Version);
    d.Bytes(parts.Name.data(), parts.Name.size());
    d.Bytes(parts.Reflection.data(), parts.Reflection.size());
    for (auto const& stage : parts.Stages) {
        VulkanStageHeader header;
        std::span<uint8_t const> module;
        SpirvHash hash;
        std::string why;
        if (SplitVulkanStage(stage, header, module) && SpirvModuleHash(module, hash, why)) {
            d.Tag('S');
            d.Bytes(header.data(), header.size());
            d.Bytes(hash.data(), hash.size());
            if (structural) (*structural)++;
        } else {
            d.Tag('B');
            d.Bytes(stage.data(), stage.size());
        }
    }
    return d.Full();
}

}

extern "C" void identity_hash(const void* container, size_t size, uint8_t out[16])
{
    auto hash = bg3se::mac::ContainerHash({ static_cast<uint8_t const*>(container), size });
    memcpy(out, hash.data(), 16);
}
