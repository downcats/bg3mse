#include <Layout/GameFields.h>

#include <Layout/FunctionCode.h>
#include <Platform/CodeShape.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <tuple>

namespace bg3se::mac {

namespace {

constexpr std::string_view kFieldPtrPrefix = "__ZN2ls9FieldMetaIM";
constexpr std::string_view kFieldPtrSuffix = "8FieldPtrE";
constexpr std::string_view kSerializerPrefix = "__ZNK3ecs4sync19ComponentSerializerI";
// The form that writes whole components (the other takes the dirty members' bits).
constexpr std::string_view kSerializerName = "18SerializeComponentERNS";
constexpr std::string_view kSerializerArguments =
    ">::SerializeComponent(ecs::sync::NetworkWriter&, ecs::EntityWorld const&, ls::Span<ls::ID<ecs::EntityHandleTraits> const> const&, "
    "ls::Span<int const> const&) const";
constexpr std::string_view kGetComponentPrefix = "__ZN3ecs11EntityWorld12GetComponentI";
constexpr std::string_view kDeserializerPrefix = "__ZN3ecs4sync21ComponentDeserializerI";
constexpr std::string_view kDeserializerName = "20DeserializeComponentERNS";
constexpr std::string_view kDeserializerArguments =
    ">::DeserializeComponent(ecs::sync::NetworkReader&, ecs::sync::ReplicationWorld&, ls::Span<ls::ID<ecs::EntityHandleTraits> const> const&, "
    "ls::Span<int const> const&)";

constexpr std::string_view kSavePrefix = "__ZN2ls8savegame9Framework3OpsI";
constexpr std::string_view kSaveName = "4SaveERNS_5model6Writer";
constexpr std::string_view kSaveArguments = ">::Save(ls::model::Writer&, ls::Span<ls::ID<ecs::EntityHandleTraits> const> const&)";
constexpr std::string_view kLoadName = "4LoadERKNS_5model6Reader";
constexpr std::string_view kLoadArguments =
    ">::Load(ls::model::Reader const&, ls::Span<ls::ID<ecs::EntityHandleTraits> const> const&, ls::Span<int const> const&, "
    "ls::model::AnyRefArray const&)";
// What a save game's loader makes each component it loads in.
constexpr char kAllocate[] = "__ZN3ecs29ComponentFrameStorageAllocRawERNS_21ComponentFrameStorageEiRNS_26ComponentFrameStorageIndexE";

// What an entity's storage calls to destroy a component of a type with a
// destructor: the lambda of ecs::_private::GetComponentDestructor<T>, handed the component.
constexpr std::string_view kDestructorPrefix = "__ZZN3ecs8_private22GetComponentDestructorI";
constexpr std::string_view kDestructorName = "8__invoke";
constexpr std::string_view kDestructorArguments = ">())(void*)::'lambda'(void*)::__invoke(void*)";

// The functions of a component that touch its members, and how each comes
// to the component: from EntityWorld::GetComponent<T>, from the frame
// storage a new one is made in, or as its first argument.
struct Source
{
    std::string_view Prefix, Name;      // of the mangled symbol
    std::string_view Open, Arguments;   // the demangled name around the component's
    bool FirstOfTwo;                    // the component is the first of the template's two arguments
    bool Allocates;
    bool Handed;                        // the component is in x0 on entry
    char const* Label;
};
constexpr Source kSources[] = {
    { kSerializerPrefix, kSerializerName, "ecs::sync::ComponentSerializer<", kSerializerArguments, false, false, false, "serializer" },
    { kDeserializerPrefix, kDeserializerName, "ecs::sync::ComponentDeserializer<", kDeserializerArguments, false, false, false, "deserializer" },
    // ls::savegame::Framework::Ops<T, Loader>
    { kSavePrefix, kSaveName, "ls::savegame::Framework::Ops<", kSaveArguments, true, false, false, "save" },
    { kSavePrefix, kLoadName, "ls::savegame::Framework::Ops<", kLoadArguments, true, true, false, "load" },
    { kDestructorPrefix, kDestructorName, "void (*ecs::_private::GetComponentDestructor<", kDestructorArguments, false, false, true, "destructor" },
};
constexpr char kWrite[] = "__ZN2ls13ScratchBuffer5WriteERKNS_7MemViewIKhEE";

std::string Demangle(std::string_view symbol)
{
    std::string mangled(symbol.substr(1));      // Mach-O spelling: one underscore more
    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    if (!demangled) return {};
    std::string text(demangled);
    free(demangled);
    return text;
}

// The end of the template argument that begins at `at`: the ',' or '>' at depth 0.
size_t ArgumentEnd(std::string_view text, size_t at)
{
    int depth = 0;
    for (size_t end = at; end < text.size(); end++) {
        char c = text[end];
        if (c == '<' || c == '(') depth++;
        else if ((c == '>' || c == ')') && depth > 0) depth--;
        else if ((c == '>' || c == ',') && depth == 0) return end;
    }
    return std::string_view::npos;
}

uint32_t FundamentalSize(std::string_view type)
{
    static constexpr std::pair<std::string_view, uint32_t> kSizes[] = {
        { "bool", 1 }, { "char", 1 }, { "signed char", 1 }, { "unsigned char", 1 },
        { "short", 2 }, { "unsigned short", 2 }, { "int", 4 }, { "unsigned int", 4 }, { "float", 4 },
        { "long", 8 }, { "unsigned long", 8 }, { "long long", 8 }, { "unsigned long long", 8 }, { "double", 8 },
    };
    for (auto const& [name, size] : kSizes) {
        if (type == name) return size;
    }
    return 0;
}

// "ls::FieldMeta<int eoc::HealthComponent::*, &eoc::HealthComponent::m_HitPoints>::FieldPtr"
bool ParseFieldPtr(std::string const& text, NamedField& field)
{
    constexpr std::string_view open = "ls::FieldMeta<";
    if (!text.starts_with(open)) return false;
    size_t first = ArgumentEnd(text, open.size());
    if (first == std::string::npos || text[first] != ',') return false;
    std::string_view pointer(text.data() + open.size(), first - open.size());       // "int eoc::HealthComponent::*"
    size_t second = ArgumentEnd(text, first + 1);
    if (second == std::string::npos || text[second] != '>') return false;
    std::string_view member(text.data() + first + 1, second - first - 1);           // " &eoc::HealthComponent::m_HitPoints"
    while (!member.empty() && (member.front() == ' ' || member.front() == '&')) member.remove_prefix(1);
    size_t name = member.rfind("::");
    if (name == std::string_view::npos || !pointer.ends_with("::*")) return false;
    std::string_view cls = member.substr(0, name);
    pointer.remove_suffix(3);
    if (!pointer.ends_with(cls) || pointer.size() <= cls.size() + 1 || pointer[pointer.size() - cls.size() - 1] != ' ') return false;
    field.Class = cls;
    field.Member = member.substr(name + 2);
    field.Type = pointer.substr(0, pointer.size() - cls.size() - 1);
    field.Size = FundamentalSize(field.Type);
    return true;
}

bool IsCall(uint32_t i) { return (i & 0xfc000000) == 0x94000000; }                    // bl
bool IsCallRegister(uint32_t i) { return (i & 0xfffffc1f) == 0xd63f0000; }              // blr
bool IsJump(uint32_t i) { return (i & 0xfc000000) == 0x14000000; }                      // b
bool IsReturn(uint32_t i) { return (i & 0xfffffc1f) == 0xd65f0000; }                    // ret
bool IsIndirectJump(uint32_t i) { return (i & 0xfffffc1f) == 0xd61f0000; }              // br
bool IsTrap(uint32_t i) { return (i & 0xffe0001f) == 0xd4200000 || i == 0; }            // brk, udf
bool IsMoveRegister(uint32_t i) { return (i & 0xffe0ffe0) == 0xaa0003e0; }             // mov xd, xm
bool IsAddImmediate64(uint32_t i) { return (i & 0xff800000) == 0x91000000; }            // add xd, xn, #imm
bool IsSubImmediate64(uint32_t i) { return (i & 0xff800000) == 0xd1000000; }            // sub xd, xn, #imm
// A direct branch inside the function, and where it goes.
std::optional<uint64_t> LocalTarget(uint64_t va, uint32_t i)
{
    if (IsJump(i)) return JumpTarget(va, i);                                            // b
    if ((i & 0xff000010) == 0x54000000 || (i & 0x7e000000) == 0x34000000) {             // b.cond, cbz, cbnz
        int64_t offset = ((int64_t)(int32_t)(i << 8) >> 13) * 4;
        return va + (uint64_t)offset;
    }
    if ((i & 0x7e000000) == 0x36000000) {                                               // tbz, tbnz
        int64_t offset = ((int64_t)(int32_t)(i << 13) >> 18) * 4;
        return va + (uint64_t)offset;
    }
    return {};
}

// The instructions of one function, link-time addresses in __TEXT,__text.
class Words
{
public:
    Words(GameImage const& image, uint64_t begin, uint64_t end) : begin_(begin), end_(end < begin ? begin : end)
    {
        words_.resize((end_ - begin_) / 4);
        for (size_t n = 0; n < words_.size(); n++) words_[n] = image.Word(begin_ + 4 * n);
    }
    uint64_t Begin() const { return begin_; }
    size_t Count() const { return words_.size(); }
    bool Contains(uint64_t va) const { return va >= begin_ && va < end_ && !(va & 3); }
    uint32_t At(uint64_t va) const { return words_[(va - begin_) / 4]; }

private:
    uint64_t begin_, end_;
    std::vector<uint32_t> words_;
};

// Where a jump through a table of offsets goes, in the form clang gives a
// switch:
//     adrp xT, table@page; add xT, xT, table@pageoff     (before, often in the prologue)
//     cmp  wI, #last; b.hi default
//     adr  xA, base; ldrb/ldrh wE, [xT, xI(, lsl #1)]; add xA, xA, xE(, uxt? or lsl) #2; br xA
// Every target must be in the function; anything else gives nothing.
std::optional<std::vector<uint64_t>> JumpTableTargets(GameImage const& image, Words const& code, uint64_t br)
{
    uint32_t target = RegN(code.At(br));
    auto before = [&](uint64_t from, auto match) -> std::optional<uint64_t> {
        for (uint64_t va = from; va >= code.Begin() && va + 32 > from; va -= 4) {
            if (match(code.At(va))) return va;
            if (va == code.Begin()) break;
        }
        return {};
    };
    if (br < code.Begin() + 4) return {};
    // add xA, xA, xE, lsl #2 (shifted register), or with uxtb/uxth/uxtw #2 (extended register)
    auto add = before(br - 4, [&](uint32_t i) {
        bool shifted = (i & 0xffe0fc00) == 0x8b000800;
        bool extended = (i & 0xffe01c00) == 0x8b200800 && ((i >> 13) & 7) <= 2;
        return (shifted || extended) && RegD(i) == target && RegN(i) == target;
    });
    if (!add || *add < code.Begin() + 4) return {};
    uint32_t element = RegM(code.At(*add));
    // ldrb/ldrh wE, [xT, xI] (register offset)
    auto load = before(*add - 4, [&](uint32_t i) {
        return ((i & 0xffe0fc00) == 0x38606800 || (i & 0xffe0ec00) == 0x78606800) && RegD(i) == element;
    });
    if (!load || *load < code.Begin() + 4) return {};
    uint32_t ldr = code.At(*load);
    uint32_t bytes = (ldr >> 30) == 1 ? 2 : 1, table = RegN(ldr), index = RegM(ldr);
    if (bytes == 2 && !((ldr >> 12) & 1)) return {};        // the index must be scaled by the element
    // adr xA, base
    auto adr = before(*load, [&](uint32_t i) { return (i & 0x9f000000) == 0x10000000 && RegD(i) == target; });
    if (!adr) return {};
    uint32_t a = code.At(*adr);
    int64_t imm = (int64_t)((((a >> 5) & 0x7ffff) << 2) | ((a >> 29) & 3));
    if (imm & (1 << 20)) imm -= (1 << 21);
    uint64_t base = *adr + (uint64_t)imm;
    // cmp wI, #last; b.hi
    std::optional<uint64_t> bound;
    for (uint64_t va = *adr; va >= code.Begin() + 4 && va + 64 > *adr; va -= 4) {
        uint32_t i = code.At(va - 4), next = code.At(va);
        if ((i & 0x7f80001f) == 0x7100001f && RegN(i) == index && !((i >> 22) & 1) && (next & 0xff00001f) == 0x54000008) {
            bound = (i >> 10) & 0xfff;
            break;
        }
    }
    if (!bound) return {};
    // The table: the closest adrp and add of xT before the load.
    uint64_t tableVA = 0;
    for (uint64_t va = *load - 4; va >= code.Begin() + 4; va -= 4) {
        uint32_t i = code.At(va);
        if (IsAddImmediate64(i) && RegD(i) == table && RegN(i) == table) {
            tableVA = PageAddress(va - 4, code.At(va - 4), i);
            break;
        }
    }
    if (!tableVA) return {};
    auto entries = static_cast<uint8_t const*>(image.DataAt(tableVA, (*bound + 1) * bytes));
    if (!entries) return {};
    std::vector<uint64_t> targets;
    for (uint64_t n = 0; n <= *bound; n++) {
        uint64_t offset = bytes == 1 ? entries[n] : (uint64_t)(entries[2 * n] | entries[2 * n + 1] << 8);
        uint64_t to = base + 4 * offset;
        if (!code.Contains(to)) return {};
        targets.push_back(to);
    }
    return targets;
}

// A load or store at a constant offset from a base register, without
// writeback: the base, the offset, the bytes and whether it loads. A pair
// gives two such, one per register.
struct Access
{
    uint32_t Base;
    int64_t Offset;
    uint32_t Bytes;
    bool Load;
    uint32_t Rt;
};

int Accesses(uint32_t i, Access out[2])
{
    bool simd = (i >> 26) & 1;
    uint32_t size = i >> 30, opc = (i >> 22) & 3;
    auto bytes = [&]() -> uint32_t { return simd && (opc & 2) && size == 0 ? 16 : 1u << size; };
    auto load = [&]() { return simd ? (opc & 1) != 0 : opc != 0; };
    // Unsigned offset (ldr/str and their byte, half and signed forms).
    if ((i & 0x3b000000) == 0x39000000) {
        if (!simd && size == 3 && opc == 2) return 0;                                   // prfm
        uint32_t width = bytes();
        out[0] = { RegN(i), (int64_t)((i >> 10) & 0xfff) * width, width, load(), RegD(i) };
        return 1;
    }
    // Unscaled offset (ldur/stur).
    if ((i & 0x3b200c00) == 0x38000000) {
        if (!simd && size == 3 && opc == 2) return 0;                                   // prfum
        int64_t imm = ((int64_t)(int32_t)(i << 11)) >> 23;
        out[0] = { RegN(i), imm, bytes(), load(), RegD(i) };
        return 1;
    }
    // Pair, signed offset (ldp/stp, ldpsw).
    if ((i & 0x3b800000) == 0x29000000) {
        uint32_t pairOpc = i >> 30;
        uint32_t width = simd ? 4u << pairOpc : (pairOpc & 2) ? 8 : 4;
        if (!simd && pairOpc == 3) return 0;
        int64_t imm = (((int64_t)(int32_t)(i << 10)) >> 25) * width;
        bool isLoad = (i >> 22) & 1;
        out[0] = { RegN(i), imm, width, isLoad, RegD(i) };
        out[1] = { RegN(i), imm + width, width, isLoad, (i >> 10) & 0x1f };
        return 2;
    }
    return 0;
}


// What a register holds at an instruction: nothing known, the component's
// address plus a constant, the stack pointer plus a constant, or a constant.
struct Held
{
    enum Kind : uint8_t { None, Component, Stack, Constant } Is{ None };
    int64_t Plus{ 0 };
    bool operator == (Held const&) const = default;
};

// A stack slot: what was stored at sp + At from a register.
struct Slot
{
    int64_t At;
    Held Value;
    bool operator == (Slot const&) const = default;
};

struct Track
{
    Held Registers[31]{};
    // The 8-byte stores to the stack that are still there, at most kSlots,
    // the oldest dropped first: a view is stored just before its call.
    static constexpr size_t kSlots = 8;
    Slot Slots[kSlots]{};
    uint8_t SlotCount{ 0 };

    Slot const* SlotAt(int64_t at) const
    {
        for (size_t n = 0; n < SlotCount; n++) {
            if (Slots[n].At == at) return &Slots[n];
        }
        return nullptr;
    }
    void Forget(int64_t from, int64_t to)       // the slots that overlap [from, to)
    {
        size_t kept = 0;
        for (size_t n = 0; n < SlotCount; n++) {
            if (Slots[n].At < to && from < Slots[n].At + 8) continue;
            Slots[kept++] = Slots[n];
        }
        SlotCount = (uint8_t)kept;
    }
    void Store(int64_t at, Held value)
    {
        Forget(at, at + 8);
        if (SlotCount == kSlots) {
            std::copy(Slots + 1, Slots + kSlots, Slots);
            SlotCount--;
        }
        Slots[SlotCount++] = { at, value };
    }
    // Where two paths meet: what both agree on.
    bool Meet(Track const& other)
    {
        bool changed = false;
        for (uint32_t r = 0; r < 31; r++) {
            if (Registers[r].Is != Held::None && !(Registers[r] == other.Registers[r])) {
                Registers[r] = {};
                changed = true;
            }
        }
        size_t kept = 0;
        for (size_t n = 0; n < SlotCount; n++) {
            auto theirs = other.SlotAt(Slots[n].At);
            if (theirs && *theirs == Slots[n]) Slots[kept++] = Slots[n];
        }
        changed = changed || kept != SlotCount;
        SlotCount = (uint8_t)kept;
        return changed;
    }
};

// The constant a mov of an immediate sets (movz, movn, a bitmask), and
// whether `i` is a movk of `previous`.
std::optional<int64_t> MovedConstant(uint32_t i, Held const& previous)
{
    bool is64 = i >> 31;
    auto width = [&](uint64_t value) { return (int64_t)(is64 ? value : (value & 0xffffffffull)); };
    if (auto value = MovzValue(i)) return width(*value);
    uint32_t lane = 16 * ((i >> 21) & 3);
    if ((i & 0x7f800000) == 0x12800000 && (is64 || lane < 32)) return width(~((uint64_t)((i >> 5) & 0xffff) << lane));        // movn
    if ((i & 0x7f800000) == 0x72800000 && (is64 || lane < 32) && previous.Is == Held::Constant) {                            // movk
        return width(((uint64_t)previous.Plus & ~(0xffffull << lane)) | ((uint64_t)((i >> 5) & 0xffff) << lane));
    }
    if ((i & 0x7f8003e0) == 0x320003e0) {                                                                                     // mov rd, #bitmask
        if (auto value = LogicalImmediate(i)) return width(*value);
    }
    return {};
}

// What the tracking of one scan shares: the functions, the one that writes
// bytes, and what a function does with a component's address it is handed
// in a register, at a depth (followed to `kCallDepth` calls deep, each
// function, register and depth once, so that what is found does not depend
// on the order the functions are read in).
struct Tracking
{
    GameImage const& Image;
    std::vector<GameImage::NamedVA> const& Functions;   // ascending
    uint64_t Write;
    std::map<std::tuple<uint64_t, uint32_t, int>, std::vector<SerializedField>> Handed;
};

constexpr int kCallDepth = 3;
// Longer functions are not followed into: what they do with an address is
// rarely about the members, and following them is what would cost.
constexpr uint64_t kLongestFollowed = 4096 * 4;

bool TrackComponent(Tracking& tracking, std::function<bool(uint64_t)> const& fetch, std::optional<uint32_t> argument, int depth,
    uint64_t begin, uint64_t end, SerializedComponent& out);

// The members a function touches through the address it is handed in `reg`,
// relative to that address.
std::vector<SerializedField> const& Handed(Tracking& tracking, uint64_t callee, uint32_t reg, int depth)
{
    auto key = std::make_tuple(callee, reg, depth);
    if (auto known = tracking.Handed.find(key); known != tracking.Handed.end()) return known->second;
    auto& fields = tracking.Handed[key];        // empty if it cannot be followed
    auto const& functions = tracking.Functions;
    auto at = std::lower_bound(functions.begin(), functions.end(), callee, [](GameImage::NamedVA const& f, uint64_t va) { return f.VA < va; });
    if (at == functions.end() || at->VA != callee) return fields;
    uint64_t end = at + 1 != functions.end() ? (at + 1)->VA : tracking.Image.TextEnd();
    if (end - callee > kLongestFollowed) return fields;
    SerializedComponent handed;
    if (TrackComponent(tracking, [](uint64_t) { return false; }, reg, depth, callee, end, handed)) {
        auto& kept = tracking.Handed[key];
        kept = std::move(handed.Fields);
        return kept;
    }
    return fields;
}

// One game function that comes to a component and touches its members:
// where the component's address goes, followed forward along every path of
// the function (where two paths meet, a register or a stack slot keeps what
// it holds only when both agree), and every access through it. The
// component is what a call that `fetch` accepts returns in x0 or, with
// `argument`, what that register holds on entry. A member is recorded where
// the function
//   - writes its bytes (ls::ScratchBuffer::Write of a view on the stack whose
//     pointer is into the component and whose size is a constant),
//   - hands its address to another function (and, `kCallDepth` deep, where
//     that function touches it),
//   - loads or stores it at a constant offset.
// A function with an indirect jump that is not a jump table gives nothing.
bool TrackComponent(Tracking& tracking, std::function<bool(uint64_t)> const& fetch, std::optional<uint32_t> argument, int depth,
    uint64_t begin, uint64_t end, SerializedComponent& out)
{
    auto const& image = tracking.Image;
    uint64_t write = tracking.Write;
    Words code(image, begin, end);
    size_t count = code.Count();
    std::map<uint64_t, std::vector<uint64_t>> tables;     // each br, and where it goes
    for (size_t n = 0; n < count; n++) {
        uint64_t va = begin + 4 * n;
        if (!IsIndirectJump(code.At(va))) continue;
        auto targets = JumpTableTargets(image, code, va);
        if (!targets) return false;
        tables[va] = std::move(*targets);
    }
    // The states are kept where paths meet, and only there: at the first
    // instruction and wherever a branch goes (the leaders, numbered in order).
    constexpr uint32_t kNotLeader = ~0u;
    std::vector<uint32_t> leader(count, kNotLeader);
    if (count) leader[0] = 0;
    for (size_t n = 0; n < count; n++) {
        uint64_t va = begin + 4 * n;
        if (auto to = LocalTarget(va, code.At(va)); to && code.Contains(*to)) leader[(*to - begin) / 4] = 0;
    }
    for (auto const& [br, targets] : tables) {
        for (uint64_t to : targets) leader[(to - begin) / 4] = 0;
    }
    uint32_t leaders = 0;
    for (auto& index : leader) {
        if (index != kNotLeader) index = leaders++;
    }
    std::vector<Track> entry(leaders);
    std::vector<bool> reached(leaders, false), queued(leaders, false);
    std::vector<size_t> work;
    auto flow = [&](size_t to, Track const& state) {
        if (to >= count) return;
        uint32_t at = leader[to];
        if (!reached[at]) {
            reached[at] = true;
            entry[at] = state;
        } else if (!entry[at].Meet(state)) {
            return;
        }
        if (!queued[at]) {
            queued[at] = true;
            work.push_back(to);
        }
    };

    // Each access once, however often the paths reach it.
    std::map<std::pair<uint64_t, int>, SerializedField> found;
    std::vector<bool> writeNotRead(count, false);
    bool fetched = argument.has_value();

    Track start;
    if (argument) start.Registers[*argument] = { Held::Component, 0 };
    if (count) flow(0, start);
    while (!work.empty()) {
        size_t n = work.back();
        work.pop_back();
        queued[leader[n]] = false;
        Track state = entry[leader[n]];
        auto& reg = state.Registers;
        for (;;) {
            uint64_t va = begin + 4 * n;
            uint32_t i = code.At(va);
            auto forgetCallerSaved = [&] {
                for (uint32_t r = 0; r <= 18; r++) reg[r] = {};
                reg[30] = {};
            };

            if (IsCall(i) || IsCallRegister(i)) {
                uint64_t callee = IsCall(i) ? CallTarget(va, i) : 0;
                bool stackHanded = false;
                for (uint32_t r = 0; r <= 7; r++) stackHanded = stackHanded || reg[r].Is == Held::Stack;
                if (callee && fetch(callee)) {
                    forgetCallerSaved();
                    reg[0] = { Held::Component, 0 };
                    fetched = true;
                } else {
                    if (callee && callee == write) {
                        auto view = reg[1].Is == Held::Stack ? state.SlotAt(reg[1].Plus) : nullptr;
                        auto size = reg[1].Is == Held::Stack ? state.SlotAt(reg[1].Plus + 8) : nullptr;
                        if (view && view->Value.Is == Held::Component && view->Value.Plus >= 0 && size && size->Value.Is == Held::Constant
                            && size->Value.Plus > 0 && size->Value.Plus <= 0xffff) {
                            found[{ va, 0 }] = { (uint32_t)view->Value.Plus, (uint32_t)size->Value.Plus, FieldAccess::Written };
                            writeNotRead[n] = false;
                        } else if (!found.count({ va, 0 })) {
                            writeNotRead[n] = true;
                        }
                    } else {
                        for (uint32_t r = 0; r <= 7; r++) {
                            if (reg[r].Is != Held::Component || reg[r].Plus < 0) continue;
                            // The address is a member's only where the function
                            // it goes to was read and uses it; otherwise it may
                            // be the start or end of a range copied or cleared,
                            // or a register left over that the call does not take.
                            auto const* inner = callee && depth < kCallDepth ? &Handed(tracking, callee, r, depth + 1) : nullptr;
                            bool used = inner && std::any_of(inner->begin(), inner->end(), [](SerializedField const& f) { return f.How != FieldAccess::PassedOn; });
                            found[{ va, (int)r }] = { (uint32_t)reg[r].Plus, 0, used ? FieldAccess::Passed : FieldAccess::PassedOn };
                            if (!inner) continue;
                            int index = 0;
                            for (auto const& field : *inner) {
                                found[{ va, 16 + 4096 * (int)r + index++ }] = { (uint32_t)reg[r].Plus + field.Offset, field.Size, field.How };
                            }
                        }
                    }
                    forgetCallerSaved();
                }
                // A callee handed a stack address may have written any slot.
                if (stackHanded) state.SlotCount = 0;
            } else if (IsMoveRegister(i) && RegD(i) < 31) {
                uint32_t from = RegM(i);
                reg[RegD(i)] = from < 31 ? reg[from] : Held{ Held::Constant, 0 };
            } else if ((IsAddImmediate64(i) || IsSubImmediate64(i)) && RegD(i) < 31) {
                int64_t imm = (i >> 10) & 0xfff;
                if (i & 0x00400000) imm <<= 12;
                if (IsSubImmediate64(i)) imm = -imm;
                Held from = RegN(i) == 31 ? Held{ Held::Stack, 0 } : reg[RegN(i)];
                reg[RegD(i)] = from.Is == Held::None ? Held{} : Held{ from.Is, from.Plus + imm };
            } else if (auto constant = RegD(i) < 31 ? MovedConstant(i, reg[RegD(i)]) : std::nullopt) {
                reg[RegD(i)] = { Held::Constant, *constant };
            } else {
                Access accesses[2];
                int many = Accesses(i, accesses);
                // Another form of load or store (writeback, a register
                // offset, exclusive, atomic) through the stack, or through a
                // register that holds a stack address: any slot may change.
                uint32_t rn = RegN(i);
                if (!many && (i & 0x0a000000) == 0x08000000 && (rn == 31 || reg[rn].Is == Held::Stack)) state.SlotCount = 0;
                for (int a = 0; a < many; a++) {
                    auto const& access = accesses[a];
                    bool simd = (i >> 26) & 1;
                    Held base = access.Base == 31 ? Held{ Held::Stack, 0 } : reg[access.Base];
                    if (base.Is == Held::Stack) {
                        // A store to the stack, by sp or a register that holds
                        // a stack address (the frame pointer): a slot that
                        // holds a register, or bytes over the slots it overlaps.
                        if (access.Load) continue;
                        int64_t at = base.Plus + access.Offset;
                        if (!simd && access.Bytes == 8) state.Store(at, access.Rt < 31 ? reg[access.Rt] : Held{ Held::Constant, 0 });
                        else state.Forget(at, at + access.Bytes);
                        continue;
                    }
                    if (base.Is == Held::Component && base.Plus + access.Offset >= 0) {
                        found[{ va, 8 + a }] = { (uint32_t)(base.Plus + access.Offset), access.Bytes,
                            access.Load ? FieldAccess::Read : FieldAccess::Stored };
                    }
                }
                // Other than a call, an instruction writes at most the
                // registers its fields name: Rt, Rn (writeback), Rt2, Rs.
                for (uint32_t r : { i & 0x1f, (i >> 5) & 0x1f, (i >> 10) & 0x1f, (i >> 16) & 0x1f }) {
                    if (r < 31 && reg[r].Is != Held::None && MayWriteRegister(i, r)) reg[r] = {};
                }
            }

            if (auto to = LocalTarget(va, i); to && code.Contains(*to)) flow((*to - begin) / 4, state);
            if (IsIndirectJump(i)) {
                for (uint64_t to : tables[va]) flow((to - begin) / 4, state);
                break;
            }
            if (IsJump(i) || IsReturn(i) || IsTrap(i)) break;
            // Straight on, until an instruction another path also reaches.
            if (++n >= count) break;
            if (leader[n] != kNotLeader) {
                flow(n, state);
                break;
            }
        }
    }
    out.WritesNotRead = (uint32_t)std::count(writeNotRead.begin(), writeNotRead.end(), true);
    out.Fields.clear();
    for (auto const& [where, field] : found) out.Fields.push_back(field);
    return fetched;
}

}

GameFieldScan DecodeGameFields(GameImage const& image, std::function<bool(std::string_view)> const& wanted)
{
    GameFieldScan scan;

    for (auto const& symbol : image.SymbolsNamed(kFieldPtrPrefix, kFieldPtrSuffix)) {
        NamedField field;
        auto value = static_cast<uint64_t const*>(image.DataAt(symbol.VA, sizeof(uint64_t)));
        if (!value || *value > 0xffff || !ParseFieldPtr(Demangle(symbol.Name), field)) {
            scan.NamedNotRead++;
            continue;
        }
        field.Offset = (uint32_t)*value;
        scan.Named.push_back(std::move(field));
    }
    std::sort(scan.Named.begin(), scan.Named.end(), [](NamedField const& a, NamedField const& b) {
        return a.Class != b.Class ? a.Class < b.Class : a.Offset != b.Offset ? a.Offset < b.Offset : a.Member < b.Member;
    });
    scan.Named.erase(std::unique(scan.Named.begin(), scan.Named.end(), [](NamedField const& a, NamedField const& b) {
        return a.Class == b.Class && a.Offset == b.Offset && a.Member == b.Member;
    }), scan.Named.end());

    auto symbols = image.TextSymbols();
    std::map<uint64_t, std::string_view> getters;
    uint64_t write = 0, allocate = 0;
    for (auto const& symbol : symbols) {
        if (symbol.Name.starts_with(kGetComponentPrefix)) getters.emplace(symbol.VA, symbol.Name);
        if (symbol.Name == kWrite) write = symbol.VA;
        if (symbol.Name == kAllocate) allocate = symbol.VA;
    }
    // The text symbols give an address its first name only; the lookup by
    // name, a walk of the whole table, only when that name is another.
    if (!write) write = image.FindVA(kWrite);
    if (!write) {
        scan.Problem = "ls::ScratchBuffer::Write is not in the symbol table under the expected signature";
        return scan;
    }
    // The type each EntityWorld::GetComponent<T, ...> a tracked function calls fetches.
    std::map<uint64_t, std::string> fetched;
    auto fetches = [&](uint64_t callee, std::string const& cls) {
        auto known = fetched.find(callee);
        if (known == fetched.end()) {
            std::string type;
            if (auto getter = getters.find(callee); getter != getters.end()) {
                constexpr std::string_view open = "ecs::EntityWorld::GetComponent<";
                // "eoc::HealthComponent const* ecs::EntityWorld::GetComponent<eoc::HealthComponent const, true>(...)"
                std::string text = Demangle(getter->second);
                size_t at = text.find(open);
                size_t close = at != std::string::npos ? ArgumentEnd(text, at + open.size()) : std::string::npos;
                if (close != std::string::npos) type = text.substr(at + open.size(), close - at - open.size());
                if (type.ends_with(" const")) type.resize(type.size() - 6);
            }
            known = fetched.emplace(callee, std::move(type)).first;
        }
        return !known->second.empty() && known->second == cls;
    };

    Tracking tracking{ image, symbols, write, {} };
    // One walk of the symbols; what each source finds is kept apart, so that
    // a class's functions come in the order of the sources.
    std::vector<SerializedComponent> bySource[std::size(kSources)];
    size_t named[std::size(kSources)]{};       // the functions of each source the symbol table has, wanted or not
    for (size_t n = 0; n < symbols.size(); n++) {
        for (size_t s = 0; s < std::size(kSources); s++) {
            auto const& source = kSources[s];
            if (!symbols[n].Name.starts_with(source.Prefix) || symbols[n].Name.find(source.Name) == std::string_view::npos) continue;
            std::string text = Demangle(symbols[n].Name);
            if (!text.ends_with(source.Arguments)) continue;
            size_t close = text.starts_with(source.Open) ? ArgumentEnd(text, source.Open.size()) : std::string::npos;
            SerializedComponent component;
            component.Source = source.Label;
            if (close != std::string::npos) {
                // The second argument, the loader, ends where the arguments begin.
                size_t second = source.FirstOfTwo && text[close] == ',' ? ArgumentEnd(text, close + 1) : close;
                bool whole = second != std::string::npos && text.size() - second == source.Arguments.size();
                if (whole && (source.FirstOfTwo ? text[close] == ',' : true)) component.Class = text.substr(source.Open.size(), close - source.Open.size());
            }
            if (!component.Class.empty()) named[s]++;
            if (wanted && !component.Class.empty() && !wanted(component.Class)) continue;
            uint64_t end = n + 1 < symbols.size() ? symbols[n + 1].VA : image.TextEnd();
            // A loader that makes no component by the frame storage, or more
            // than one, is not read: which of them is T is not known.
            if (source.Allocates && !component.Class.empty()) {
                int made = 0;
                for (uint64_t va = symbols[n].VA; va < end; va += 4) {
                    uint32_t i = image.Word(va);
                    if (IsCall(i) && CallTarget(va, i) == allocate) made++;
                }
                if (made != 1) component.Class.clear();
            }
            auto isFetch = [&](uint64_t callee) { return source.Allocates ? callee == allocate : fetches(callee, component.Class); };
            auto argument = source.Handed ? std::optional<uint32_t>(0) : std::nullopt;
            if (component.Class.empty() || !TrackComponent(tracking, isFetch, argument, 0, symbols[n].VA, end, component)) {
                // Every replicated component's serializers fetch it; a
                // save-game function of a type without members, or one
                // that looks the component up inline, does not; a
                // destructor that is not read calls the object's virtual
                // destructor (br through its vtable).
                if (source.Allocates || source.FirstOfTwo) scan.SavesNotFollowed++;
                else if (source.Handed) scan.DestructorsNotRead++;
                else scan.SerializersNotRead++;
                continue;
            }
            bySource[s].push_back(std::move(component));
        }
    }
    for (auto& found : bySource) std::move(found.begin(), found.end(), std::back_inserter(scan.Serialized));
    // A kind of function the game does not have under the expected name is
    // not read, and the scan says so: the others still are.
    auto notFound = [&](std::string what) { scan.NotFound += (scan.NotFound.empty() ? "" : "; ") + what; };
    for (size_t s = 0; s < std::size(kSources); s++) {
        if (!named[s]) notFound(std::string("no ") + kSources[s].Label + " of a component type");
    }
    if (!allocate) notFound("ecs::ComponentFrameStorageAllocRaw, where the save-game loaders have the component from");
    if (scan.Serialized.empty()) {
        scan.Problem = "no ecs::sync::ComponentSerializer<T>::SerializeComponent in the symbol table";
        return scan;
    }
    std::stable_sort(scan.Serialized.begin(), scan.Serialized.end(), [](SerializedComponent const& a, SerializedComponent const& b) { return a.Class < b.Class; });
    return scan;
}

}
