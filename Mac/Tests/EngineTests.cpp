// Tests of the engine table's mechanics (Mac/Engine). No game needed: the
// resolver runs on the in-memory test binary, and on this executable and a
// dylib loaded here.

#include <Engine/EngineResolver.h>
#include <Engine/EngineTable.h>
#include <Engine/EntryUse.h>
#include <Engine/TypeIndices.h>
#include <Platform/Symbols.h>

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <iterator>
#include <set>
#include <string>

#include "FakeBinary.h"

using namespace bg3se::mac;
using namespace bg3se::mac::engine;

namespace {

int gFailures = 0;
char const* gTest = "";

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; name(); } static void name()

constexpr uint32_t kRet = 0xd65f03c0;

uint32_t Bl(uint64_t from, uint64_t to) { return 0x94000000 | (uint32_t)(((to - from) / 4) & 0x03ffffff); }

bool Contains(std::string const& text, char const* part) { return text.find(part) != std::string::npos; }

// The test binary with its symbol table open.
struct Fake
{
    FakeBinary Binary;
    GameImage Image;

    explicit Fake(bool chainedFixups = false, std::vector<FakeBinary::Sym> const& extra = {}) : Binary(chainedFixups, extra)
    {
        CHECK(Image.OpenFile(Binary.File));
    }

    Images View() const { return { { &Image, nullptr }, {} }; }

    // The recorded shape of a function: its code as it is now.
    CodeShape Shape(uint64_t va, uint64_t end) const { return ShapeOfRange(Image.CodeAt(va), (uint32_t)((end - va) / 4)); }
    CodeShape FirstShape() const { return Shape(FakeBinary::Text, FakeBinary::Text + 0x40); }
    CodeShape SecondShape() const { return Shape(FakeBinary::Text + 0x40, FakeBinary::Text + 0x100); }

    Resolved One(Entry const& entry) const
    {
        Area area{ "test", { &entry, 1 } };
        auto resolution = Resolve({ &area, 1 }, View());
        CHECK(resolution.Entries.size() == 1);
        return resolution.Entries[0];
    }
};

bool Failed(Resolved const& r, char const* check)
{
    for (auto const& c : r.Checks) {
        if (!c.Passed && strcmp(c.What, check) == 0) return true;
    }
    return false;
}

// --- what a function does with its registers --------------------------------

TEST(entry_scan_reads_arguments)
{
    // mov x19, x0; ldr w8, [x1]; mov w20, w2; mov x3, #0; cbz x19, +8; mov x4, #0; ret
    uint32_t code[] = { 0xaa0003f3, 0xb9400028, 0x2a0203f4, 0xd2800003, 0xb4000053, 0xd2800004, kRet };
    auto use = ScanEntry(code, 0x100001000, {});
    CHECK(use.Registers[0] == RegisterUse::Wide);
    CHECK(use.Registers[1] == RegisterUse::Address);
    CHECK(use.Registers[2] == RegisterUse::Narrow);
    CHECK(use.Registers[3] == RegisterUse::Overwritten);
    CHECK(use.Registers[4] == RegisterUse::OverwrittenOnPath);     // after the branch: not on every path
    CHECK(use.Registers[5] == RegisterUse::NotObserved);
    CHECK(use.End == ScanEnd::Return && use.Scanned == 7);
    CHECK(use.Read(0) && use.Read(1) && use.Read(2) && !use.Read(3) && !use.Read(5));
}

TEST(entry_scan_follows_a_saved_argument)
{
    uint64_t va = 0x100001000;
    // mov x19, x0; mov x20, x1; mov x21, x2; ldr w8, [x19]; cmp w20, #1; mov x21, #0; ldr w9, [x21]; ret
    uint32_t code[] = { 0xaa0003f3, 0xaa0103f4, 0xaa0203f5, 0xb9400268, 0x7100069f, 0xd2800015, 0xb94002a9, kRet };
    auto use = ScanEntry(code, va, {});
    CHECK(use.Registers[0] == RegisterUse::Address);    // the copy is used as an address
    CHECK(use.Registers[1] == RegisterUse::Narrow);     // the copy is used as 32 bits
    CHECK(use.Registers[2] == RegisterUse::Wide);       // the copy was overwritten before any use: only the save is seen
    CHECK(use.End == ScanEnd::Return);

    // A copy into another argument register carries the argument with it: mov x1, x0; ldr w8, [x1]; ret
    uint32_t moved[] = { 0xaa0003e1, 0xb9400028, kRet };
    use = ScanEntry(moved, va, {});
    CHECK(use.Registers[0] == RegisterUse::Address && use.Registers[1] == RegisterUse::Overwritten);
}

TEST(entry_scan_stops_at_calls_and_passes_the_stack_probe)
{
    // adrp x16, slot; ldr x16, [x16, #0x10]; blr x16; str x1, [sp]; bl somewhere; mov x19, x0
    uint32_t code[] = { 0xf0000010, 0xf9400a10, 0xd63f0200, 0xf90003e1, 0x94000010, 0xaa0003f3 };
    uint64_t va = 0x100001000, slot = 0x100004010;
    auto probed = ScanEntry(code, va, { slot, 0 });
    CHECK(probed.Registers[1] == RegisterUse::Wide);            // read after the probe
    CHECK(probed.Registers[0] == RegisterUse::NotObserved);     // the call after it ends the scan
    CHECK(probed.Registers[16] == RegisterUse::Overwritten);
    CHECK(probed.End == ScanEnd::Call && probed.Scanned == 5);

    // The same words when that slot is not the probe's: an ordinary call.
    auto plain = ScanEntry(code, va, {});
    CHECK(plain.End == ScanEnd::Call && plain.Scanned == 3 && plain.Registers[1] == RegisterUse::NotObserved);

    // A call to the probe's stub.
    uint32_t stubbed[] = { Bl(va, va + 0x100), 0xf90003e1, kRet };
    auto viaStub = ScanEntry(stubbed, va, { 0, va + 0x100 });
    CHECK(viaStub.Registers[1] == RegisterUse::Wide && viaStub.End == ScanEnd::Return);

    // An instruction the scan does not decode ends it, with what was seen until then.
    uint32_t odd[] = { 0xaa0003f3, 0x00000000, 0xaa0103f4 };
    auto stopped = ScanEntry(odd, va, {});
    CHECK(stopped.End == ScanEnd::Unrecognised && stopped.Scanned == 1 && stopped.Read(0) && !stopped.Read(1));
}

TEST(entry_scan_knows_the_common_instructions)
{
    uint64_t va = 0x100001000;
    auto first = [&](std::initializer_list<uint32_t> words, uint32_t reg) {
        std::vector<uint32_t> code(words);
        code.push_back(kRet);
        return ScanEntry(code, va, {}).Registers[reg];
    };
    CHECK(first({ 0xa9bf7bfd }, 0) == RegisterUse::NotObserved);        // stp x29, x30, [sp, #-16]!
    CHECK(first({ 0xa9000be1 }, 1) == RegisterUse::Wide);               // stp x1, x2, [sp]
    CHECK(first({ 0xa9000be1 }, 2) == RegisterUse::Wide);
    CHECK(first({ 0x29400408 }, 0) == RegisterUse::Address);            // ldp w8, w1, [x0]
    CHECK(first({ 0x29400408 }, 1) == RegisterUse::Overwritten);
    CHECK(first({ 0x7100043f }, 1) == RegisterUse::Narrow);             // cmp w1, #1
    CHECK(first({ 0xeb02003f }, 2) == RegisterUse::Wide);               // cmp x1, x2
    CHECK(first({ 0x72a00021 }, 1) == RegisterUse::Narrow);             // movk w1, #1, lsl #16
    CHECK(first({ 0x53001c28 }, 1) == RegisterUse::Narrow);             // uxtb w8, w1
    CHECK(first({ 0x38616808 }, 1) == RegisterUse::Wide);               // ldrb w8, [x0, x1]
    CHECK(first({ 0x38616808 }, 0) == RegisterUse::Address);
    CHECK(first({ 0x1e220020 }, 1) == RegisterUse::Narrow);             // scvtf s0, w1
    CHECK(first({ 0x9e660001 }, 1) == RegisterUse::Overwritten);        // fmov x1, d0
    CHECK(first({ 0xbd000000 }, 0) == RegisterUse::Address);            // str s0, [x0]
    CHECK(first({ 0x9b227c28 }, 2) == RegisterUse::Narrow);             // smull x8, w1, w2
    CHECK(first({ 0x9a821028 }, 2) == RegisterUse::Wide);               // csel x8, x1, x2, ne
    CHECK(first({ 0x36f80042 }, 2) == RegisterUse::Narrow);             // tbz w2, #31, +8
    CHECK(first({ 0xb8e80008 }, 0) == RegisterUse::Address);            // ldaddal w8, w8, [x0]
    CHECK(first({ 0xb8e80008 }, 8) == RegisterUse::Narrow);
    CHECK(first({ 0x88dffc28 }, 1) == RegisterUse::Address);            // ldar w8, [x1]
    CHECK(first({ 0x88dffc28 }, 8) == RegisterUse::Overwritten);
    CHECK(first({ 0xc89ffc22 }, 2) == RegisterUse::Wide);               // stlr x2, [x1]
    CHECK(first({ 0x88a07c22 }, 0) == RegisterUse::Narrow);             // cas w0, w2, [x1]
}

// --- the resolver -----------------------------------------------------------

Entry FirstFunction(Fake const& fake)
{
    return {
        .Name = "first",
        .What = Kind::FunctionToCall,
        .Where = Exact("_first"),
        .Call = Takes({}, Returns::X0),
        .Shape = fake.FirstShape(),
    };
}

TEST(resolver_resolves_a_function_and_reports_its_checks)
{
    Fake fake;
    auto entry = FirstFunction(fake);
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Resolved && r.AsExpected() && r.Usable());
    CHECK(r.VA == FakeBinary::Text && r.End == FakeBinary::Text + 0x40 && r.SeenVA == r.VA);
    CHECK(r.Shape == entry.Shape && r.Scanned);
    CHECK(r.Checks.size() == 5);        // symbol, shape, arguments, result, calls itself
    for (auto const& check : r.Checks) CHECK(check.Passed);

    entry.Caveat = "mind this";
    r = fake.One(entry);
    CHECK(r.Is == Status::ResolvedWithCaveat && r.AsExpected() && r.Usable() && r.VA == FakeBinary::Text);
}

TEST(resolver_refuses_a_missing_symbol)
{
    Fake fake;
    auto entry = FirstFunction(fake);
    entry.Where = Exact("_third");
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && !r.AsExpected() && !r.Usable());
    CHECK(r.VA == 0 && r.SeenVA == 0);
    CHECK(Failed(r, "symbol") && Contains(r.Reason, "not in the symbol table"));

    entry.Where = Prefix("_thi");
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Contains(r.Reason, "_thi... is not in the symbol table"));
}

TEST(resolver_refuses_an_ambiguous_symbol)
{
    Fake fake;
    auto entry = FirstFunction(fake);
    fake.Binary.Symbol(1)->n_un.n_strx = fake.Binary.Symbol(0)->n_un.n_strx;
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && r.VA == 0);
    CHECK(Failed(r, "symbol") && Contains(r.Reason, "more than one address"));

    Fake plain;
    entry = FirstFunction(plain);
    entry.Where = Prefix("_");
    r = plain.One(entry);
    CHECK(r.Is == Status::Unresolved && Contains(r.Reason, "more than one address"));
    entry.Where = Prefix("_fir");
    CHECK(plain.One(entry).Is == Status::Resolved);
}

TEST(resolver_refuses_a_changed_shape_and_accepts_a_moved_one)
{
    Fake fake;
    auto entry = FirstFunction(fake);
    // The call goes somewhere else: the function moved relative to its callee.
    fake.Binary.Word(FakeBinary::Text + 8, Bl(FakeBinary::Text + 8, FakeBinary::Stubs));
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Resolved && r.VA == FakeBinary::Text);

    // add x0, x8, #0x10 becomes add x0, x8, #0x18: the code changed.
    fake.Binary.Word(FakeBinary::Text + 0x10, 0x91006100);
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && r.VA == 0 && r.SeenVA == FakeBinary::Text);
    CHECK(Failed(r, "shape") && Contains(r.Reason, "shape"));
    CHECK(r.Shape != entry.Shape && r.Shape.Instructions == entry.Shape.Instructions);
}

TEST(resolver_checks_declared_arguments_against_the_entry)
{
    Fake fake;
    // _second: mov x19, x0; ldr w8, [x1]; mov w20, w2; mov x3, #0; ret
    uint64_t at = FakeBinary::Text + 0x40;
    for (uint32_t word : { 0xaa0003f3u, 0xb9400028u, 0x2a0203f4u, 0xd2800003u, kRet }) { fake.Binary.Word(at, word); at += 4; }
    Entry entry{
        .Name = "second",
        .What = Kind::FunctionToCall,
        .Where = Exact("_second"),
        .Call = Takes({ Pointer(Place::X0, 0, "object"), Pointer(Place::X1, 1, "index"), Value(Place::X2, 4, 2, "flags") }),
        .Shape = fake.SecondShape(),
    };
    CHECK(fake.One(entry).Is == Status::Resolved);

    auto refused = [&](Signature const& call, char const* why) {
        entry.Call = call;
        auto r = fake.One(entry);
        CHECK(r.Is == Status::Unresolved && Failed(r, "arguments"));
        CHECK(Contains(r.Reason, why));
    };
    // An argument the function overwrites without reading it.
    refused(Takes({ Pointer(Place::X0, 0, ""), Pointer(Place::X1, 1, ""), Value(Place::X2, 4, 2, ""), Value(Place::X3, 4, 3, "") }),
        "x3 is overwritten before it is read");
    // A register the function reads that the table gives no argument.
    refused(Takes({ Pointer(Place::X0, 0, ""), Pointer(Place::X1, 1, "") }), "x2 is read at the entry");
    // A pointer the function reads as a 32-bit value, and a small value it uses as an address.
    refused(Takes({ Pointer(Place::X0, 0, ""), Pointer(Place::X1, 1, ""), Pointer(Place::X2, 2, "") }), "x2 is read as a 32-bit value");
    refused(Takes({ Pointer(Place::X0, 0, ""), Value(Place::X1, 4, 1, ""), Value(Place::X2, 4, 2, "") }), "x1 is used as an address");
    // An argument declared unread that is read.
    refused(Takes({ Unread(Place::X0, 0, ""), Pointer(Place::X1, 1, ""), Value(Place::X2, 4, 2, "") }), "x0 is read, but declared unread");
    // A function that reads x8 takes the address of its result there: that has to be declared.
    // _second: str x0, [x8]; ldr w9, [x1]; mov w20, w2; ret
    at = FakeBinary::Text + 0x40;
    for (uint32_t word : { 0xf9000100u, 0xb9400029u, 0x2a0203f4u, kRet, 0u }) { fake.Binary.Word(at, word); at += 4; }
    entry.Shape = fake.SecondShape();
    refused(Takes({ Pointer(Place::X0, 0, ""), Pointer(Place::X1, 1, ""), Value(Place::X2, 4, 2, "") }), "x8 is read at the entry");
    entry.Call = Takes({ Pointer(Place::X0, 0, ""), Pointer(Place::X1, 1, ""), Value(Place::X2, 4, 2, "") }, Returns::IndirectX8);
    CHECK(fake.One(entry).Is == Status::Resolved);
}

TEST(resolver_checks_self_calls_and_hooks)
{
    Fake fake;
    // _second: bl _setjmp; bl _second; ret
    fake.Binary.Word(FakeBinary::Text + 0x44, Bl(FakeBinary::Text + 0x44, FakeBinary::Text + 0x40));
    Entry entry{
        .Name = "second",
        .What = Kind::FunctionToCall,
        .Where = Exact("_second"),
        .Shape = fake.SecondShape(),
    };
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "calls itself"));
    entry.CallsItself = true;
    CHECK(fake.One(entry).Is == Status::Resolved);

    // A function to hook must be one the planner accepts: this one starts with a call.
    entry.What = Kind::FunctionToHook;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "entry hook") && r.Hook == HookRefusal::Call);
}

TEST(resolver_compares_the_stack_block_with_a_caller)
{
    Fake fake;
    // _second: str x8, [sp]; strb w9, [sp, #8]; str w10, [sp, #0xc]; bl _first; ret
    uint64_t at = FakeBinary::Text + 0x40;
    for (uint32_t word : { 0xf90003e8u, 0x390023e9u, 0xb9000feau, Bl(FakeBinary::Text + 0x4c, FakeBinary::Text), kRet }) { fake.Binary.Word(at, word); at += 4; }
    Entry entry = FirstFunction(fake);
    entry.Caller = Exact("_second");
    entry.Call = Takes({ StackPointer(0, 0, "object"), StackValue(8, 1, 1, "kind"), StackValue(0xc, 4, 2, "count") }, Returns::X0);
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Resolved);

    entry.Call = Takes({ StackPointer(0, 0, ""), StackValue(8, 4, 1, ""), StackValue(0xc, 4, 2, "") }, Returns::X0);
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "stack block") && Contains(r.Reason, "stores 1 bytes at [sp, #0x8], declared 4"));

    entry.Call = Takes({ StackPointer(0, 0, ""), StackValue(8, 1, 1, ""), StackValue(0xc, 4, 2, ""), StackPointer(0x10, 3, "") }, Returns::X0);
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Contains(r.Reason, "stores nothing at [sp, #0x10]"));

    entry.Caller = Exact("_first");     // does not call it
    entry.Call = Takes({ StackPointer(0, 0, "") }, Returns::X0);
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Contains(r.Reason, "does not call the function"));
}

constexpr MaskedWord kAnyCall[] = { { 0x94000000, Any::Branch } };
constexpr MaskedWord kCallThenReturn[] = { { 0x94000000, Any::Branch }, { kRet } };
constexpr MaskedWord kNoSuchCode[] = { { 0xd503201f } };

TEST(resolver_derives_a_call_site_from_a_pattern_matching_once)
{
    Fake fake;
    Derivation derivation{ .Host = Exact("_second"), .HostShape = fake.SecondShape(), .Pattern = kCallThenReturn };
    Entry entry{ .Name = "site", .What = Kind::CallSite, .Derived = &derivation };
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Resolved && r.VA == FakeBinary::Text + 0x44 && r.End == r.VA + 4);

    // Twice: _second holds two calls.
    derivation.Pattern = kAnyCall;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && r.VA == 0 && Failed(r, "pattern") && Contains(r.Reason, "matches 2 times"));

    // Not at all.
    derivation.Pattern = kNoSuchCode;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "pattern") && Contains(r.Reason, "matches 0 times"));

    // The host changed: nothing is taken from it.
    derivation.Pattern = kCallThenReturn;
    derivation.HostShape.Hash ^= 1;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "host shape"));

    derivation.Host = Exact("_no_host");
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "host"));
}

TEST(resolver_finds_a_code_site_whose_pattern_states_the_instruction)
{
    constexpr uint32_t kNop = 0xd503201f;
    Fake fake;
    // The return after _second's two calls.
    Derivation derivation{ .Host = Exact("_second"), .HostShape = fake.SecondShape(), .Pattern = kCallThenReturn, .At = 1 };
    Entry entry{ .Name = "site", .What = Kind::CodeSite, .Derived = &derivation, .Becomes = kNop };
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Resolved && r.VA == FakeBinary::Text + 0x48 && r.End == r.VA + 4);

    // A call: the pattern leaves its target open, and the anchor on it says which call it is.
    static constexpr MaskedWord kTwoCalls[] = { { 0x94000000, Any::Branch }, { 0x94000000, Any::Branch }, { kRet } };
    Derivation atCall{ .Host = Exact("_second"), .HostShape = fake.SecondShape(), .Anchor = Calls(Exact("_first")), .Pattern = kTwoCalls,
        .AnchorAt = 1, .At = 1 };
    entry.Derived = &atCall;
    r = fake.One(entry);
    CHECK(r.Is == Status::Resolved && r.VA == FakeBinary::Text + 0x44);

    // The other call of the pattern is any call: it is not stated, and may not be replaced.
    atCall.At = 0;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && r.VA == 0 && Failed(r, "table") && Contains(r.Reason, "does not state the instruction"));
    atCall.At = 1;

    // What it becomes must be said, and the site must be in the pattern.
    entry.Becomes = 0;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "table"));
    entry.Becomes = kNop;
    atCall.At = 3;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "table"));
    Derivation bare{ .Host = Exact("_second"), .HostShape = fake.SecondShape(), .Anchor = Calls(Exact("_first")) };
    entry.Derived = &bare;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "table"));

    // The host changed: the site is not taken from it.
    derivation.HostShape.Hash ^= 1;
    entry.Derived = &derivation;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && r.VA == 0 && Failed(r, "host shape"));
}

TEST(resolver_derives_from_the_one_call_to_an_anchor)
{
    Fake fake;
    Derivation derivation{ .Host = Exact("_second"), .HostShape = fake.SecondShape(), .Anchor = Calls(Exact("_first")) };
    Entry entry{ .Name = "site", .What = Kind::CallSite, .Derived = &derivation };
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Resolved && r.VA == FakeBinary::Text + 0x44);

    // An imported callee, through its stub; and a pattern around the call.
    derivation.Anchor = CallsImport("_setjmp");
    r = fake.One(entry);
    CHECK(r.Is == Status::Resolved && r.VA == FakeBinary::Text + 0x40);
    static constexpr MaskedWord kAfterSetjmp[] = { { 0x94000000, Any::Branch }, { 0x94000000, Any::Branch }, { kRet } };
    derivation.Pattern = kAfterSetjmp;
    derivation.At = 1;
    r = fake.One(entry);
    CHECK(r.Is == Status::Resolved && r.VA == FakeBinary::Text + 0x44);
    derivation.AnchorAt = 1;        // the pattern would then start before the host
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "pattern"));
    derivation.Pattern = {};
    derivation.AnchorAt = derivation.At = 0;

    // No call to the anchor, and two.
    derivation.Anchor = CallsImport("_longjmp");
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "anchor") && Contains(r.Reason, "0 calls to _longjmp"));
    fake.Binary.Word(FakeBinary::Text + 0x48, Bl(FakeBinary::Text + 0x48, FakeBinary::Text));
    fake.Binary.Word(FakeBinary::Text + 0x4c, kRet);
    derivation.HostShape = fake.SecondShape();
    derivation.Anchor = Calls(Exact("_first"));
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "anchor") && Contains(r.Reason, "2 calls to _first"));

    derivation.Anchor = Calls(Exact("_nobody"));
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "anchor") && Contains(r.Reason, "not in the symbol table"));
}

// adrp x8, page; add x8, x8, #offset
constexpr MaskedWord kAddressPair[] = { { 0x90000008, Any::Adrp }, { 0x91000108, Any::Imm12 } };

TEST(resolver_derives_a_global_from_an_address_pair)
{
    Fake fake;
    // _second: adrp x8, _after@PAGE; add x8, x8, _after@PAGEOFF; ret
    fake.Binary.Word(FakeBinary::Text + 0x40, 0xf0000008);
    fake.Binary.Word(FakeBinary::Text + 0x44, 0x9100c108);
    fake.Binary.Word(FakeBinary::Text + 0x48, kRet);
    Derivation derivation{ .Host = Exact("_second"), .HostShape = fake.SecondShape(), .Pattern = kAddressPair,
        .What = Derivation::Take::PageAddress };
    Entry entry{ .Name = "after", .What = Kind::GlobalObject, .Derived = &derivation, .Agrees = Exact("_after") };
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Resolved && r.VA == FakeBinary::Data + 0x30);

    entry.Agrees = Exact("_vtable");
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "agrees"));

    // An address in code is no global.
    fake.Binary.Word(FakeBinary::Text + 0x40, 0x90000008);     // adrp x8, the text page
    derivation.HostShape = fake.SecondShape();
    entry.Agrees = {};
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "section"));
}

TEST(resolver_checks_a_global_by_its_section_and_size)
{
    Fake fake(false, { { "_pointer", FakeBinary::Data + 0x38, N_SECT, 3 }, { "_last", FakeBinary::Data + 0x40, N_SECT, 3 } });
    Entry pointer{ .Name = "pointer", .What = Kind::GlobalPointer, .Where = Exact("_pointer") };
    auto r = fake.One(pointer);
    CHECK(r.Is == Status::Resolved && r.VA == FakeBinary::Data + 0x38);

    // 0x30 bytes to the next symbol is no single pointer.
    pointer.Where = Exact("_vtable");
    r = fake.One(pointer);
    CHECK(r.Is == Status::Unresolved && Failed(r, "size"));

    Entry object{ .Name = "object", .What = Kind::GlobalObject, .Where = Exact("_vtable"), .Bytes = 0x30 };
    CHECK(fake.One(object).Is == Status::Resolved);
    object.Bytes = 0x38;
    r = fake.One(object);
    CHECK(r.Is == Status::Unresolved && Failed(r, "size"));

    // The last symbol has the rest of its section.
    object.Where = Exact("_last");
    object.Bytes = 0x7c0;
    CHECK(fake.One(object).Is == Status::Resolved);

    // A function is no global.
    object.Where = Exact("_first");
    object.Bytes = 0;
    r = fake.One(object);
    CHECK(r.Is == Status::Unresolved && Failed(r, "section"));
}

TEST(resolver_finds_vtable_slots_by_method)
{
    Fake fake;
    static constexpr VtableMethod kMethods[] = { { "Second", Exact("_second"), 8 } };
    Entry entry{ .Name = "vtable", .What = Kind::Vtable, .Where = Exact("_vtable"), .Methods = kMethods };
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Resolved && r.VA == FakeBinary::Data + 0x10);
    CHECK(r.Slots.size() == 1 && r.Slots[0].Offset == 8 && r.Slots[0].Function == FakeBinary::Text + 0x40);

    // The method is somewhere else than recorded.
    static constexpr VtableMethod kMoved[] = { { "Second", Exact("_second"), 0x10 } };
    entry.Methods = kMoved;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && r.VA == 0 && Failed(r, "Second") && Contains(r.Reason, "recorded +0x10"));

    // A function the vtable does not hold, and one it holds in two slots.
    static constexpr VtableMethod kNotThere[] = { { "After", Exact("_after"), 0 } };
    entry.Methods = kNotThere;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "After") && Contains(r.Reason, "in 0 slots"));
    CHECK(r.Slots.size() == 1 && r.Slots[0].Offset == -1);
    static constexpr VtableMethod kTwice[] = { { "First", Exact("_first"), 0 } };
    entry.Methods = kTwice;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "First") && Contains(r.Reason, "in 2 slots"));

    // A slot's argument passing against the function the vtable holds: _second is bl _setjmp; bl _first; ret,
    // which reads nothing, as an override that ignores its arguments may.
    static constexpr Signature kSlotCall = Takes({ Pointer(Place::X0, 0, "object"), Value(Place::X1, 4, 1, "count") });
    static constexpr VtableMethod kWithCall[] = { { "Second", Exact("_second"), 8, &kSlotCall } };
    entry.Methods = kWithCall;
    CHECK(fake.One(entry).Is == Status::Resolved);
    {
        // mov x19, x0; ldr w8, [x1]; mov w20, w2; ret: it uses x1 as an address and reads x2, which the slot does not pass.
        Fake reading;
        uint64_t at = FakeBinary::Text + 0x40;
        for (uint32_t word : { 0xaa0003f3u, 0xb9400028u, 0x2a0203f4u, kRet }) { reading.Binary.Word(at, word); at += 4; }
        r = reading.One(entry);
        CHECK(r.Is == Status::Unresolved && Failed(r, "Second") && Contains(r.Reason, "x1 is used as an address"));
        CHECK(Contains(r.Reason, "x2 is read at the entry"));
        // Overwriting an argument unread is no contradiction for a slot.
        at = FakeBinary::Text + 0x40;
        for (uint32_t word : { 0xd2800001u, kRet }) { reading.Binary.Word(at, word); at += 4; }      // mov x1, #0; ret
        CHECK(reading.One(entry).Is == Status::Resolved);
    }

    // A method without a symbol.
    static constexpr VtableMethod kUnnamed[] = { { "Gone", Exact("_gone"), 0 } };
    entry.Methods = kUnnamed;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "Gone"));

    // A file whose pointers are fixup chains has no slot to read.
    Fake chained(true);
    entry.Methods = kMethods;
    r = chained.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "slots"));
}

TEST(resolver_reads_operands_back)
{
    Fake fake;
    static constexpr MaskedWord kSecondAdd[] = { { 0x91000100, Any::Imm12 }, { kRet } };            // add x0, x8, #?; ret
    static constexpr MaskedWord kLoad[] = { { 0xf9400268, Any::Imm12 }, { 0x9107c100 } };           // ldr x8, [x19, #?]; add x0, x8, #0x1f0
    static constexpr MaskedWord kCall[] = { { 0x9107c100 }, { 0x94000000, Any::Branch } };
    static constexpr Operand kOperands[] = {
        { .What = "the offset", .Pattern = kSecondAdd, .How = Operand::Read::AddImmediate, .Expected = 0x10 },
        { .What = "the field", .Pattern = kLoad, .How = Operand::Read::LoadStoreOffset, .Expected = 0x2000 },
        { .What = "the call", .In = Exact("_second"), .Pattern = kCallThenReturn, .How = Operand::Read::BranchTarget, .Target = Exact("_first") },
    };
    Entry entry = FirstFunction(fake);
    entry.Operands = kOperands;
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Resolved);
    CHECK(r.Operands.size() == 3 && r.Operands[0] == 0x10 && r.Operands[1] == 0x2000 && r.Operands[2] == FakeBinary::Text);

    static constexpr Operand kWrongValue[] = {
        { .What = "the offset", .Pattern = kSecondAdd, .How = Operand::Read::AddImmediate, .Expected = 0x18 },
    };
    entry.Operands = kWrongValue;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "the offset") && Contains(r.Reason, "0x10, recorded 0x18"));

    static constexpr Operand kWrongTarget[] = {
        { .What = "the call", .Pattern = kCall, .At = 1, .How = Operand::Read::BranchTarget, .Target = Exact("_second") },
    };
    entry.Operands = kWrongTarget;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "the call"));

    static constexpr MaskedWord kLoadAlone[] = { { 0xf9400268, Any::Imm12 } };      // twice in _first
    static constexpr Operand kTwice[] = {
        { .What = "the field", .Pattern = kLoadAlone, .How = Operand::Read::LoadStoreOffset, .Expected = 0x2000 },
    };
    entry.Operands = kTwice;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Contains(r.Reason, "match 2 times"));

    // How often one function calls another.
    static constexpr Operand kCalledOnce[] = {
        { .What = "called once", .In = Exact("_second"), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact("_first") },
    };
    entry.Operands = kCalledOnce;
    CHECK(fake.One(entry).Is == Status::Resolved);
    static constexpr Operand kCalledTwice[] = {
        { .What = "called twice", .In = Exact("_second"), .How = Operand::Read::CallCount, .Expected = 2, .Target = Exact("_first") },
    };
    entry.Operands = kCalledTwice;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "called twice"));
}

TEST(resolver_reads_a_pointer_load_back)
{
    Fake fake;
    // _second: adrp x8, _after@PAGE; ldr x8, [x8, _after@PAGEOFF]; ret
    fake.Binary.Word(FakeBinary::Text + 0x40, 0xf0000008);
    fake.Binary.Word(FakeBinary::Text + 0x44, 0xf9401908);
    fake.Binary.Word(FakeBinary::Text + 0x48, kRet);
    static constexpr MaskedWord kLoadPair[] = { { 0x90000008, Any::Adrp }, { 0xf9400108, Any::Imm12 } };
    static constexpr Operand kPointer[] = {
        { .What = "read as a pointer", .In = Exact("_second"), .Pattern = kLoadPair, .How = Operand::Read::PointerLoad, .Target = Exact("_after") },
    };
    Entry entry{ .Name = "after", .What = Kind::GlobalObject, .Where = Exact("_after"), .Operands = kPointer };
    CHECK(fake.One(entry).Is == Status::Resolved);

    fake.Binary.Word(FakeBinary::Text + 0x44, 0xb9403108);     // ldr w8, [x8, #0x30]: 32 bits
    static constexpr MaskedWord kNarrowPair[] = { { 0x90000008, Any::Adrp }, { 0xb9400108, Any::Imm12 } };
    static constexpr Operand kNarrow[] = {
        { .What = "read as a pointer", .In = Exact("_second"), .Pattern = kNarrowPair, .How = Operand::Read::PointerLoad, .Target = Exact("_after") },
    };
    entry.Operands = kNarrow;
    auto r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "read as a pointer"));
}

TEST(resolver_confirms_what_has_no_code)
{
    Fake fake;
    static constexpr Named kHosts[] = { Exact("_first") };
    Entry entry{ .Name = "gone", .What = Kind::NoCallableCode, .Gone = { .Fate = "inlined into first", .NoSymbol = "_thi", .Hosts = kHosts } };
    auto r = fake.One(entry);
    CHECK(r.Is == Status::NoCallableCode && r.AsExpected() && !r.Usable() && r.VA == 0);

    // A symbol of that name exists after all.
    entry.Gone.NoSymbol = "_sec";
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && !r.AsExpected() && Failed(r, "no symbol"));

    // The function the code went to is gone.
    static constexpr Named kNoHosts[] = { Exact("_third") };
    entry.Gone = { .Fate = "inlined into third", .NoSymbol = "_thi", .Hosts = kNoHosts };
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "host"));

    // Something was written from the hosts' code: each host that was read
    // must still have the shape recorded for it.
    static constexpr Named kTwoHosts[] = { Exact("_first"), Exact("_second") };
    CodeShape shapes[] = { fake.FirstShape(), fake.SecondShape() };
    entry.Gone = { .Fate = "inlined into first and second", .NoSymbol = "_thi", .Hosts = kTwoHosts, .HostShapes = shapes };
    CHECK(entry.Gone.ReadFromHosts());
    r = fake.One(entry);
    CHECK(r.Is == Status::NoCallableCode && r.AsExpected());
    shapes[1].Hash ^= 1;
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "host shape"));
    // A host that was not read is not compared, and must still exist.
    shapes[1] = {};
    r = fake.One(entry);
    CHECK(r.Is == Status::NoCallableCode && entry.Gone.ReadFromHosts());
    shapes[0] = {};
    CHECK(!entry.Gone.ReadFromHosts());
    // One shape for each host, or the entry is not understood.
    entry.Gone.HostShapes = std::span<CodeShape const>(shapes, 1);
    r = fake.One(entry);
    CHECK(r.Is == Status::Unresolved && Failed(r, "table"));
}

TEST(resolver_keeps_unsettled_and_imageless_entries_unresolved)
{
    Fake fake;
    Entry unsettled = FirstFunction(fake);
    unsettled.Unsettled = "not read yet";
    auto r = fake.One(unsettled);
    CHECK(r.Is == Status::Unresolved && r.AsExpected() && r.VA == 0 && r.Reason == "not read yet" && r.Checks.empty());

    Entry elsewhere = FirstFunction(fake);
    elsewhere.In = Image::Osiris;
    r = fake.One(elsewhere);
    CHECK(r.Is == Status::Unresolved && !r.AsExpected() && Contains(r.Reason, "not given"));
}

TEST(resolver_refuses_an_entry_with_the_one_it_needs)
{
    Fake fake;
    Derivation derivation{ .Host = Exact("_second"), .HostShape = fake.SecondShape(), .Anchor = Calls(Exact("_first")) };
    Entry entries[] = {
        { .Name = "site", .What = Kind::CallSite, .Derived = &derivation, .Needs = "first" },
        FirstFunction(fake),
        { .Name = "chained", .What = Kind::GlobalObject, .Where = Exact("_after"), .Needs = "site" },
        { .Name = "lost", .What = Kind::GlobalObject, .Where = Exact("_after"), .Needs = "no such entry" },
    };
    Area area{ "test", entries };
    auto resolution = Resolve({ &area, 1 }, fake.View());
    CHECK(resolution.Find("site")->Is == Status::Resolved && resolution.Find("chained")->Is == Status::Resolved);
    CHECK(resolution.Find("lost")->Is == Status::Unresolved && Failed(*resolution.Find("lost"), "needs"));

    // The function the site calls has changed: the site, found as before, is refused with it, and what needs the site too.
    entries[1].Shape.Hash ^= 1;
    resolution = Resolve({ &area, 1 }, fake.View());
    auto site = resolution.Find("site");
    CHECK(resolution.Find("first")->Is == Status::Unresolved);
    CHECK(site->Is == Status::Unresolved && site->VA == 0 && site->SeenVA == FakeBinary::Text + 0x44);
    CHECK(Failed(*site, "needs") && Contains(site->Reason, "first is unresolved"));
    CHECK(resolution.Find("chained")->Is == Status::Unresolved && resolution.Find("chained")->VA == 0);
}

TEST(resolution_answers_by_name_and_counts)
{
    Fake fake;
    Entry entries[] = {
        FirstFunction(fake),
        { .Name = "missing", .What = Kind::GlobalPointer, .Where = Exact("_nowhere") },
        { .Name = "gone", .What = Kind::NoCallableCode, .Gone = { .Fate = "never was", .NoSymbol = "_thi" } },
    };
    Area area{ "test", entries };
    auto resolution = Resolve({ &area, 1 }, fake.View());
    CHECK(resolution.Entries.size() == 3);
    CHECK(resolution.Count(Status::Resolved) == 1 && resolution.Count(Status::Unresolved) == 1 && resolution.Count(Status::NoCallableCode) == 1);
    CHECK(!resolution.AllAsExpected());
    auto first = resolution.Find("first");
    CHECK(first && first->Is == Status::Resolved && std::string_view(first->Area) == "test");
    CHECK(resolution.Find("nothing") == nullptr);
    // The image is a file: there is no address in this process.
    CHECK(first && resolution.Address(*first) == nullptr);
    auto missing = resolution.Find("missing");
    CHECK(missing && resolution.Address(*missing) == nullptr);
}

// --- loaded images ----------------------------------------------------------

TEST(resolver_runs_on_loaded_images_and_gives_addresses)
{
    void* lib = dlopen(BG3MSE_TEST_FUNCTIONS, RTLD_NOW | RTLD_LOCAL);
    CHECK(lib != nullptr);
    if (!lib) return;
    char const* path = BG3MSE_TEST_FUNCTIONS;
    void const* header = FindLoadedImage(strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    GameImage library;
    FunctionTable libraryFunctions;
    std::string problem;
    CHECK(header && library.OpenLoaded(header) && libraryFunctions.OpenLoaded(header, problem));
    if (!header) return;

    // This executable stands in for the game, the dylib for libOsiris.dylib.
    LoadedGame self;
    CHECK(self.Open(problem) && !self.HasOsiris());
    Images images = self.View();
    CHECK(images.Game.Symbols == GameImage::Current() && images.Game.Functions && !images.Osiris.Symbols);
    images.Osiris = { &library, &libraryFunctions };

    Entry entries[] = {
        { .Name = "twice", .In = Image::Osiris, .What = Kind::FunctionToCall, .Where = Exact("_lib_twice"),
          .Call = Takes({ Value(Place::X0, 8, 0, "a number") }, Returns::X0) },
        { .Name = "add local", .In = Image::Osiris, .What = Kind::FunctionToHook, .Where = Exact("_lib_add_local"),
          .Call = Takes({ Value(Place::X0, 8, 0, "a number") }, Returns::X0) },
        { .Name = "data", .In = Image::Osiris, .What = Kind::GlobalObject, .Where = Exact("_lib_data"), .Bytes = 8 },
        { .Name = "main", .What = Kind::FunctionToCall, .Where = Exact("_main"), .Call = Takes({}, Returns::W0) },
    };
    Area area{ "test", entries };
    // The shapes are those of the code as it is loaded: resolve once to read them, then again with them recorded.
    auto first = Resolve({ &area, 1 }, images);
    CHECK(first.Entries.size() == 4);
    for (size_t i = 0; i < first.Entries.size(); i++) entries[i].Shape = first.Entries[i].Shape;
    auto resolution = Resolve({ &area, 1 }, images);

    auto twice = resolution.Find("twice");
    CHECK(twice && twice->Is == Status::Resolved && twice->Shape.Instructions == 2);
    auto address = twice ? resolution.Address(*twice) : nullptr;
    CHECK(address && address == dlsym(lib, "lib_twice"));
    if (address) CHECK(reinterpret_cast<long (*)(long)>(address)(21) == 42);

    auto hooked = resolution.Find("add local");
    CHECK(hooked && hooked->Is == Status::Resolved && hooked->Hook == HookRefusal::None);
    auto data = resolution.Find("data");
    CHECK(data && data->Is == Status::Resolved && resolution.Address(*data) == dlsym(lib, "lib_data"));
    auto main = resolution.Find("main");
    CHECK(main && main->Is == Status::Resolved && resolution.Address(*main) != nullptr);

    // Without the library its entries are refused, each alone.
    auto alone = Resolve({ &area, 1 }, self.View());
    CHECK(alone.Count(Status::Unresolved) == 3 && alone.Find("main")->Is == Status::Resolved);
}

// --- type indices -----------------------------------------------------------

TEST(type_index_names_split_at_the_context)
{
    std::string type, context;
    CHECK(SplitTypeIndexName("ls::TypeId<eoc::HealthComponent, ecs::ComponentTypeIdContext>::m_TypeIndex", type, context));
    CHECK(type == "eoc::HealthComponent" && context == "ecs::ComponentTypeIdContext");
    CHECK(SplitTypeIndexName("ls::TypeId<ecs::query::Spec<ls::TypeList<a::B, c::D<int, bool>>, ls::TypeList<>>, ecs::QueryRegistry>::m_TypeIndex",
        type, context));
    CHECK(type == "ecs::query::Spec<ls::TypeList<a::B, c::D<int, bool>>, ls::TypeList<>>" && context == "ecs::QueryRegistry");
    CHECK(SplitTypeIndexName("ls::TypeId<void (*)(int, char), ns::Context>::m_TypeIndex", type, context));
    CHECK(type == "void (*)(int, char)" && context == "ns::Context");
    CHECK(!SplitTypeIndexName("ls::TypeId<OnlyOne>::m_TypeIndex", type, context));
    CHECK(!SplitTypeIndexName("ls::TypeId<a, b>::m_Other", type, context));
    CHECK(!SplitTypeIndexName("ls::Other<a, b>::m_TypeIndex", type, context));
    CHECK(!SplitTypeIndexName("ls::TypeId<a>, b>::m_TypeIndex", type, context));
}

constexpr char kHealthIndex[] = "__ZN2ls6TypeIdIN3eoc15HealthComponentEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE";
constexpr char kArmorIndex[] = "__ZN2ls6TypeIdIN3eoc14ArmorComponentEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE";
constexpr char kHealthReplication[] = "__ZN2ls6TypeIdIN3eoc15HealthComponentEN3ecs4sync21ReplicatedTypeContextEE11m_TypeIndexE";

TEST(type_indices_are_collected_from_the_symbol_table)
{
    Fake fake(false, {
        { kHealthIndex, FakeBinary::Data + 0x40, N_SECT, 3 },
        { kArmorIndex, FakeBinary::Data + 0x44, N_SECT, 3 },
        { kHealthReplication, FakeBinary::Data + 0x48, N_SECT, 3 },
        { "__ZN2ls6TypeIdIN3eoc15HealthComponentEN3ecs22ComponentTypeIdContextEE7m_OtherE", FakeBinary::Data + 0x4c, N_SECT, 3 },
    });
    auto types = TypeIndices::Collect(fake.Image);
    CHECK(types.All().size() == 3);
    CHECK(types.CountIn("ecs::ComponentTypeIdContext") == 2 && types.CountIn("ecs::sync::ReplicatedTypeContext") == 1);
    CHECK(types.Contexts().size() == 2);
    auto health = types.Find("eoc::HealthComponent", "ecs::ComponentTypeIdContext");
    CHECK(health && health->VA == FakeBinary::Data + 0x40 && health->Symbol == kHealthIndex);
    auto replicated = types.Find("eoc::HealthComponent", "ecs::sync::ReplicatedTypeContext");
    CHECK(replicated && replicated->VA == FakeBinary::Data + 0x48);
    CHECK(types.Find("eoc::ArmorComponent", "ecs::sync::ReplicatedTypeContext") == nullptr);
    CHECK(types.Find("eoc::Nothing", "ecs::ComponentTypeIdContext") == nullptr);

    Entry context{ .Name = "components", .What = Kind::TypeIndex, .Context = "ecs::ComponentTypeIdContext" };
    auto r = fake.One(context);
    CHECK(r.Is == Status::Resolved && r.TypeCount == 2);
    context.Context = "ecs::SystemsContext";
    r = fake.One(context);
    CHECK(r.Is == Status::Unresolved && Failed(r, "symbols"));
}

TEST(a_type_context_resolves_to_its_state)
{
    Fake fake(false, {
        { kHealthIndex, FakeBinary::Data + 0x40, N_SECT, 3 },
        { "_state", FakeBinary::Data + 0x50, N_SECT, 3 },
        { "_guard", FakeBinary::Data + 0x60, N_SECT, 3 },
        { "_short_state", FakeBinary::Data + 0x70, N_SECT, 3 },
        { "_too_near", FakeBinary::Data + 0x78, N_SECT, 3 },
    });
    // Without a state the context has no address: only the names are known.
    Entry context{ .Name = "components", .What = Kind::TypeIndex, .Context = "ecs::ComponentTypeIdContext" };
    auto r = fake.One(context);
    CHECK(r.Is == Status::Resolved && r.VA == 0);

    context.Where = Exact("_state");
    r = fake.One(context);
    CHECK(r.Is == Status::Resolved && r.VA == FakeBinary::Data + 0x50 && r.TypeCount == 1);

    context.Where = Exact("_missing_state");
    r = fake.One(context);
    CHECK(r.Is == Status::Unresolved && Failed(r, "state") && r.VA == 0);

    // The state is two pointers: a symbol with less room is something else.
    context.Where = Exact("_short_state");
    r = fake.One(context);
    CHECK(r.Is == Status::Unresolved && Failed(r, "state size"));

    // Code is not a state.
    context.Where = Exact("_first");
    r = fake.One(context);
    CHECK(r.Is == Status::Unresolved && Failed(r, "state section"));

    // The numbering code is part of the entry: an operand that is not there refuses it.
    static constexpr MaskedWord kNotThere[] = { { 0x11000694 }, { 0xf940034a }, { 0xb9000154 } };
    static constexpr Operand kOperands[] = { { .What = "the index is stored", .In = Exact("_first"), .Pattern = kNotThere, .At = 2 } };
    context.Where = Exact("_state");
    context.Operands = kOperands;
    r = fake.One(context);
    CHECK(r.Is == Status::Unresolved && Failed(r, "the index is stored"));
}

TEST(registered_type_names_follow_the_games_rule)
{
    // A name without keywords or spaces is its own registered name.
    CHECK(RegisteredTypeName("eoc::HealthComponent") == "eoc::HealthComponent");
    // The Windows compiler's spelling and this game's give one name.
    CHECK(RegisteredTypeName("eoc::character_creation::_private::DefaultValuesManager<struct eoc::character_creation::_private::DefaultValues<enum EAbility> >")
        == RegisteredTypeName("eoc::character_creation::_private::DefaultValuesManager<eoc::character_creation::_private::DefaultValues<EAbility>>"));
    CHECK(RegisteredTypeName("eoc::character_creation::_private::DefaultValuesManager<struct eoc::character_creation::_private::ReplaceDefaultValues<class eoc::PassivePrototype> >")
        == "eoc::character_creation::_private::DefaultValuesManager<eoc::character_creation::_private::ReplaceDefaultValues<eoc::PassivePrototype>>");
    CHECK(RegisteredTypeName("class ls::TypeList<struct a::B, class c::D<int, bool> >") == "ls::TypeList<a::B,c::D<int,bool>>");
    CHECK(RegisteredTypeName("union U const *") == "U*");
    CHECK(RegisteredTypeName("const struct S") == "S");
    // A keyword is one only where a word begins.
    CHECK(RegisteredTypeName("ns::subclass Foo") == "ns::subclassFoo");
    CHECK(RegisteredTypeName("ns::my_enum E<menum X>") == "ns::my_enumE<menumX>");
    CHECK(RegisteredTypeName("void __cdecl(int)") == "void(int)");
    CHECK(RegisteredTypeName("`anonymous namespace'::T") == "::T");
    CHECK(RegisteredTypeName("") == "");
}

// --- the table itself -------------------------------------------------------

bool IsFunction(Kind kind) { return kind == Kind::FunctionToCall || kind == Kind::FunctionToHook || kind == Kind::FunctionToCallAndHook; }

TEST(table_is_well_formed)
{
    std::set<std::string_view> names;
    for (auto const& area : EngineTable()) {
        for (auto const& entry : area.Entries) names.insert(entry.Name);
    }
    size_t count = 0;
    for (auto const& area : EngineTable()) {
        CHECK(area.Name && *area.Name);
        for (auto const& entry : area.Entries) {
            count++;
            gTest = entry.Name;
            CHECK(entry.Use && *entry.Use);
            if (entry.Unsettled) continue;
            if (IsFunction(entry.What)) {
                CHECK(entry.Where && entry.Mac);
                CHECK(entry.Shape.Instructions != 0);
                bool stack = false;
                for (auto const& arg : entry.Call.Arguments()) {
                    CHECK(arg.Type && *arg.Type);
                    stack = stack || arg.At == Place::Stack;
                }
                CHECK(!stack || entry.Caller);      // a stack block is checked at a caller
            }
            if (entry.What == Kind::CallSite) CHECK(entry.Derived && entry.Derived->Anchor && entry.Derived->HostShape.Instructions != 0);
            if (entry.What == Kind::GlobalObject || entry.What == Kind::GlobalPointer) CHECK(entry.Where || entry.Derived);
            if (entry.Derived) CHECK(entry.Derived->Host && (entry.Derived->Anchor || !entry.Derived->Pattern.empty()));
            if (entry.What == Kind::Vtable) CHECK(entry.Where && !entry.Methods.empty());
            if (entry.What == Kind::TypeIndex) CHECK(entry.Context && *entry.Context);
            if (entry.What == Kind::NoCallableCode) {
                CHECK(entry.Gone.Fate && *entry.Gone.Fate);
                for (auto replacement : entry.Gone.Replacements) CHECK(names.count(replacement) == 1);
            } else {
                CHECK(!entry.Gone.Fate && entry.Gone.Replacements.empty());
            }
            CHECK(!entry.Needs || names.count(entry.Needs) == 1);
            for (auto const& operand : entry.Operands) {
                CHECK(operand.What && *operand.What);
                bool counted = operand.How == Operand::Read::CallCount || operand.How == Operand::Read::StoreOrder;
                // A count is of the calls to a function, or to an import (CallCount only).
                bool target = (bool)operand.Target != (operand.Import != nullptr);
                CHECK(counted ? target && (!operand.Import || operand.How == Operand::Read::CallCount) : !operand.Pattern.empty() && !operand.Import);
            }
        }
    }
    gTest = "table_is_well_formed";
    CHECK(names.size() == count);       // every name once
}

// Every pointer member of StaticSymbols (BG3Extender/GameDefinitions/Symbols.h), in its order.
constexpr char const* kStaticSymbols[] = {
    "ls__FixedString__GetString", "ls__FixedString__IncRef", "ls__GlobalStringTable__MainTable__CreateFromString",
    "ls__GlobalStringTable__MainTable__DecRef", "ls__gGlobalStringTable", "ls__ThreadRegistry__RequestThreadIndex", "ls__FileReader__ctor",
    "ls__FileReader__dtor", "ls__FileSystem__CollectFilesWithGlob", "ls__PathRoots", "App__Ctor", "App__UpdatePaths", "ecl__EoCClient",
    "esv__EoCServer", "ecl__EoCClient__HandleError", "ecl__gCursorControl", "ls__gDragDropManager",
    "dlg__DialogManager__PerformDialogLoad", "ls__gTranslatedStringRepository",
    "ls__gTranslatedStringKeyManager", "ecl__gGameStateEventManager", "esv__gGameStateEventManager",
    "ecl__GameStateThreaded__GameStateWorker__DoWork", "esv__GameStateThreaded__GameStateWorker__DoWork", "ecl__GameStateMachine__Update",
    "esv__GameStateMachine__Update", "App__LoadGraphicSettings", "ls__gInputManager", "ecs__EntityWorld__Update", "ecs__EntityWorld__FlushECBs",
    "eoc__SpellPrototypeManager", "eoc__SpellPrototype__Init", "eoc__StatusPrototypeManager", "eoc__StatusPrototype__Init",
    "eoc__BoostPrototypeManager", "eoc__PassivePrototypeManager", "eoc__PassivePrototype__Init", "eoc__InterruptPrototypeManager",
    "eoc__InterruptPrototype__Init", "eoc__DynamicStatsExpressionManager", "eoc__ParseStaticBoosts", "esv__OsirisVariableHelper__SavegameVisit",
    "stats__DealDamageFunctor__ApplyDamage", "esv__StatsSystem__ThrowDamageEvent", "esv__ExecuteStatsFunctor_AttackTargetContext",
    "esv__ExecuteStatsFunctor_AttackPositionContext", "esv__ExecuteStatsFunctor_MoveContext", "esv__ExecuteStatsFunctor_TargetContext",
    "esv__ExecuteStatsFunctor_NearbyAttackedContext", "esv__ExecuteStatsFunctor_NearbyAttackingContext", "esv__ExecuteStatsFunctor_EquipContext",
    "esv__ExecuteStatsFunctor_SourceContext", "esv__ExecuteStatsFunctor_InterruptContext", "gRPGStats", "RPGStats__Load",
    "RPGStats__ParseDataBuffers", "stats__Object__SetPropertyString", "esv__LevelManager", "ecl__LevelManager", "ls__GlobalTemplateManager",
    "esv__CacheTemplateManager", "esv__gSurfaceActionFactory", "esv__SurfaceActionFactory__DoCreateAction", "esv__TransformSurfaceAction__Init",
    "eoc__AiGrid__FindPath", "eoc__AiGrid__FindPathImmediate", "esv__SavegameManager", "AppInstance", "ls__GlobalAllocator__Get",
    "ls__GlobalAllocator__Alloc", "ls__GlobalAllocator__Free", "eoc__gGuidResourceManager", "ls__gGlobalResourceManager", "ls__gTextureAtlasMap",
    "ls__AppliedMaterial__LoadTexture", "ls__TextureManager__UnloadTexture", "ls__AppliedMaterial__LoadVirtualTexture",
    "ls__AppliedMaterial__TryOverrideTexture2DParameter", "ls__Visual__UpdateBlendshapeWeightsFromSkeleton", "Noesis__Reflection__LockedByThreadId",
    "Noesis__Reflection__Lock", "Noesis__Reflection__NumLocks", "Noesis__Reflection__Data", "Noesis__SymbolManager__Lock",
    "Noesis__SymbolManager__Data", "Noesis__gErrorHandler", "Noesis__GUI__LoadXaml", "Noesis__Visual__AddVisualChild",
    "Noesis__Visual__RemoveVisualChild", "Noesis__Visual__PointFromScreen", "Noesis__Visual__PointToScreen", "Noesis__VisualTreeHelper__HitTest",
    "ui__DataContextProvider__ExecuteCommandQueues", "ui__deferred__PredicatesManager__UpdateFromUI", "ls__GlobalSwitches", "Kernel_FindFirstFileW",
    "Kernel_FindNextFileW", "Kernel_FindClose",
};

TEST(table_has_every_static_symbol)
{
    CHECK(std::size(kStaticSymbols) == 98);
    std::set<std::string_view> names;
    for (auto const& area : EngineTable()) {
        for (auto const& entry : area.Entries) names.insert(entry.Name);
    }
    for (auto member : kStaticSymbols) {
        gTest = member;
        CHECK(names.count(member) == 1);
    }
}

}

int main()
{
    entry_scan_reads_arguments_run();
    entry_scan_follows_a_saved_argument_run();
    entry_scan_stops_at_calls_and_passes_the_stack_probe_run();
    entry_scan_knows_the_common_instructions_run();
    resolver_resolves_a_function_and_reports_its_checks_run();
    resolver_refuses_a_missing_symbol_run();
    resolver_refuses_an_ambiguous_symbol_run();
    resolver_refuses_a_changed_shape_and_accepts_a_moved_one_run();
    resolver_checks_declared_arguments_against_the_entry_run();
    resolver_checks_self_calls_and_hooks_run();
    resolver_compares_the_stack_block_with_a_caller_run();
    resolver_derives_a_call_site_from_a_pattern_matching_once_run();
    resolver_finds_a_code_site_whose_pattern_states_the_instruction_run();
    resolver_derives_from_the_one_call_to_an_anchor_run();
    resolver_derives_a_global_from_an_address_pair_run();
    resolver_checks_a_global_by_its_section_and_size_run();
    resolver_finds_vtable_slots_by_method_run();
    resolver_reads_operands_back_run();
    resolver_reads_a_pointer_load_back_run();
    resolver_confirms_what_has_no_code_run();
    resolver_keeps_unsettled_and_imageless_entries_unresolved_run();
    resolver_refuses_an_entry_with_the_one_it_needs_run();
    resolution_answers_by_name_and_counts_run();
    resolver_runs_on_loaded_images_and_gives_addresses_run();
    type_index_names_split_at_the_context_run();
    type_indices_are_collected_from_the_symbol_table_run();
    a_type_context_resolves_to_its_state_run();
    registered_type_names_follow_the_games_rule_run();
    table_is_well_formed_run();
    table_has_every_static_symbol_run();
    printf(gFailures ? "%d check(s) failed\n" : "all engine tests passed\n", gFailures);
    return gFailures ? 1 : 0;
}
