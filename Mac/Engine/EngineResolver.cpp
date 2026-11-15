#include <Engine/EngineResolver.h>

#include <Platform/EntryHook.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <unordered_map>

namespace bg3se::mac::engine {

namespace {

std::string Format(char const* format, ...) __attribute__((format(printf, 1, 2)));
std::string Format(char const* format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

std::string Describe(Named const& name)
{
    if (name.How == Find::NamePrefix) return std::string(name.Symbol) + "...";
    return name.Symbol ? name.Symbol : "(no name)";
}

std::string Describe(CodeShape const& shape)
{
    return Format("{ %u, 0x%016llxull }", shape.Instructions, (unsigned long long)shape.Hash);
}

constexpr char kStackProbe[] = "___chkstk_darwin";

// One image and everything looked up in it, each kind of lookup in one pass
// over its symbol table.
class Lookups
{
public:
    GameImage const* Image{ nullptr };
    FunctionTable const* Functions{ nullptr };
    StackProbe Probe{};

    void Want(Named const& name)
    {
        if (name.How == Find::ExactName) exact_.try_emplace(name.Symbol);
        else if (name.How == Find::NamePrefix) prefix_.try_emplace(name.Symbol);
    }
    void WantAbsent(char const* prefix)
    {
        if (prefix) prefix_.try_emplace(prefix);
    }

    void Run()
    {
        if (!Image) return;
        std::vector<char const*> names;
        for (auto const& [name, symbol] : exact_) names.push_back(name.data());
        std::vector<GameImage::Symbol> found(names.size());
        Image->Lookup(names, found);
        for (size_t i = 0; i < names.size(); i++) exact_[names[i]] = found[i];

        std::vector<std::string_view> prefixes;
        for (auto const& [prefix, symbol] : prefix_) prefixes.push_back(prefix);
        found.assign(prefixes.size(), {});
        Image->LookupByPrefix(prefixes, found);
        for (size_t i = 0; i < prefixes.size(); i++) prefix_[prefixes[i]] = found[i];

        std::vector<uint64_t> vas;
        for (auto const& [name, symbol] : exact_) if (symbol.VA) vas.push_back(symbol.VA);
        for (auto const& [name, symbol] : prefix_) if (symbol.VA) vas.push_back(symbol.VA);
        std::vector<uint64_t> ends(vas.size());
        Image->NextSymbolVAs(vas, ends);
        for (size_t i = 0; i < vas.size(); i++) next_[vas[i]] = ends[i];

        Probe.Slot = Image->GotSlotOf(kStackProbe);
        Probe.Stub = Image->StubOf(kStackProbe);
    }

    GameImage::Symbol Get(Named const& name) const
    {
        auto const& map = name.How == Find::ExactName ? exact_ : prefix_;
        auto it = name ? map.find(name.Symbol) : map.end();
        return it == map.end() ? GameImage::Symbol{} : it->second;
    }
    GameImage::Symbol GetPrefix(char const* prefix) const { return Get(Prefix(prefix)); }

    // The symbol after `va`; 0 when there is none.
    uint64_t Next(uint64_t va) const
    {
        auto it = next_.find(va);
        return it == next_.end() ? Image->NextSymbolVA(va) : it->second;
    }

private:
    std::unordered_map<std::string_view, GameImage::Symbol> exact_, prefix_;
    std::unordered_map<uint64_t, uint64_t> next_;
};

struct Function
{
    uint64_t VA{ 0 }, End{ 0 };
    std::string Problem;

    bool Ok() const { return Problem.empty(); }
    uint32_t Instructions() const { return (uint32_t)((End - VA) / 4); }
};

// A symbol's address, or why there is none.
uint64_t Locate(Lookups const& in, Named const& name, std::string& problem)
{
    auto symbol = in.Get(name);
    switch (symbol.Status) {
    case GameImage::SymbolStatus::Found: return symbol.VA;
    case GameImage::SymbolStatus::Ambiguous: problem = Describe(name) + " is defined at more than one address"; return 0;
    default: problem = Describe(name) + " is not in the symbol table"; return 0;
    }
}

// A function ends at the next symbol or at the next function start the image
// lists, whichever comes first: a function start has no symbol when the
// function behind it has none, and the game's function starts leave out the
// static initialisers, which have symbols.
Function LocateFunction(Lookups const& in, Named const& name)
{
    Function f;
    f.VA = Locate(in, name, f.Problem);
    if (!f.VA) return f;
    auto const& image = *in.Image;
    if ((f.VA & 3) || f.VA < image.TextBegin() || f.VA >= image.TextEnd()) {
        f.Problem = Describe(name) + " is not in the image's code";
        return f;
    }
    f.End = in.Next(f.VA);
    if (!f.End || f.End > image.TextEnd()) f.End = image.TextEnd();
    if (in.Functions) {
        uint64_t key = image.FromFile() ? f.VA : (uint64_t)(uintptr_t)image.CodeAt(f.VA);
        auto extent = in.Functions->At(key);
        if (!extent.Ok()) {
            f.Problem = Describe(name) + " is not a function start: " + bg3se::mac::Describe(extent.Refusal);
            return f;
        }
        f.End = std::min(f.End, f.VA + extent.Bytes());
    }
    f.End &= ~3ull;
    if (f.End <= f.VA) f.Problem = Describe(name) + " has no extent";
    return f;
}

CodeShape ShapeOf(Lookups const& in, Function const& f)
{
    return ShapeOfRange(in.Image->CodeAt(f.VA), f.Instructions());
}

struct Checks
{
    std::vector<CheckResult>& List;

    bool Add(char const* what, bool passed, std::string detail)
    {
        List.push_back({ what, passed, std::move(detail) });
        return passed;
    }
};

bool CheckShape(Lookups const& in, Function const& f, CodeShape const& expected, char const* what, Checks checks, CodeShape* found = nullptr)
{
    auto shape = ShapeOf(in, f);
    if (found) *found = shape;
    if (shape == expected) return checks.Add(what, true, Describe(shape));
    return checks.Add(what, false, "the code is " + Describe(shape) + ", recorded " + Describe(expected));
}

// Every call in [begin, end) to `callee`.
std::vector<uint64_t> CallSites(Lookups const& in, uint64_t begin, uint64_t end, Callee const& callee, std::string& problem)
{
    auto const& image = *in.Image;
    if (callee.Import) {
        std::vector<uint64_t> sites;
        for (uint64_t va = begin; va + 4 <= end; va += 4) {
            if (CallsImport(image, va, callee.Import)) sites.push_back(va);
        }
        return sites;
    }
    uint64_t target = Locate(in, callee.Function, problem);
    return target ? CallsTo(image, begin, end, target) : std::vector<uint64_t>{};
}

std::string Describe(Callee const& callee)
{
    return callee.Import ? std::string(callee.Import) : Describe(callee.Function);
}

// The place a derivation finds; 0 with a failed check when it finds none.
uint64_t Derive(Lookups const& in, Derivation const& d, Checks checks)
{
    auto host = LocateFunction(in, d.Host);
    if (!checks.Add("host", host.Ok(), host.Ok() ? Describe(d.Host) : host.Problem)) return 0;
    if (!CheckShape(in, host, d.HostShape, "host shape", checks)) return 0;

    auto const& image = *in.Image;
    uint64_t start = 0;
    if (d.Anchor) {
        std::string problem;
        auto sites = CallSites(in, host.VA, host.End, d.Anchor, problem);
        if (!problem.empty()) return checks.Add("anchor", false, problem), 0;
        if (sites.size() != 1) {
            return checks.Add("anchor", false, Format("%zu calls to %s in the host, expected one", sites.size(), Describe(d.Anchor).c_str())), 0;
        }
        checks.Add("anchor", true, "one call to " + Describe(d.Anchor));
        if (d.Pattern.empty()) return sites[0];
        start = sites[0] - 4ull * d.AnchorAt;
        bool fits = start >= host.VA && start + 4 * d.Pattern.size() <= host.End;
        for (size_t n = 0; fits && n < d.Pattern.size(); n++) fits = d.Pattern[n].Matches(image.Word(start + 4 * n));
        if (!checks.Add("pattern", fits, fits ? "matches at the call" : "the instructions around the call are not the recorded ones")) return 0;
    } else {
        auto matches = FindPattern(image, host.VA, host.End, d.Pattern);
        if (matches.size() != 1) {
            return checks.Add("pattern", false, Format("matches %zu times in the host, expected once", matches.size())), 0;
        }
        checks.Add("pattern", true, "matches once");
        start = matches[0];
    }

    uint64_t at = start + 4ull * d.At;
    if (d.What == Derivation::Take::Site) return at;
    uint64_t address = at + 8 <= host.End ? PageAddress(at, image.Word(at), image.Word(at + 4)) : 0;
    if (!checks.Add("address", address != 0, address ? "formed by an adrp pair" : "the instructions are not an adrp pair")) return 0;
    return address;
}

char const* RegisterName(uint32_t reg)
{
    static char const* const kNames[kTrackedRegisters] = {
        "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17",
    };
    return kNames[reg];
}

// The declared argument passing against what the entry does with its registers.
bool CheckArguments(Signature const& call, EntryUse const& use, bool hooked, bool mayIgnore, char const* what, Checks checks)
{
    std::string wrong, seen;
    auto fail = [&](uint32_t reg, char const* why) {
        if (!wrong.empty()) wrong += "; ";
        wrong += Format("%s %s", RegisterName(reg), why);
    };
    bool declared[8]{};
    for (auto const& arg : call.Arguments()) {
        if (arg.At == Place::Stack) continue;
        uint32_t reg = (uint32_t)arg.At;
        declared[reg] = true;
        auto found = use.Registers[reg];
        if (!seen.empty()) seen += ", ";
        seen += Format("%s %s", RegisterName(reg), Describe(found));
        bool ignored = found == RegisterUse::Overwritten && !mayIgnore;
        switch (arg.How) {
        case Passing::Unread:
            if (use.Read(reg) && !mayIgnore) fail(reg, "is read, but declared unread");
            break;
        case Passing::Pointer:
            if (ignored) fail(reg, "is overwritten before it is read, but declared an argument");
            if (found == RegisterUse::Narrow) fail(reg, "is read as a 32-bit value, but declared a pointer");
            break;
        case Passing::Value:
            if (ignored) fail(reg, "is overwritten before it is read, but declared an argument");
            if (found == RegisterUse::Address && arg.Bytes < 8) fail(reg, "is used as an address, but declared a value");
            break;
        }
    }
    for (uint32_t reg = 0; reg < 8; reg++) {
        if (!declared[reg] && use.Read(reg)) fail(reg, "is read at the entry, but no argument is declared in it");
    }
    if (call.Result == Returns::IndirectX8) {
        if (use.Registers[8] == RegisterUse::Overwritten && !mayIgnore) fail(8, "is overwritten before it is read, but declared the address of the result");
        seen += Format("%sx8 %s", seen.empty() ? "" : ", ", Describe(use.Registers[8]));
    } else if (use.Read(8)) {
        fail(8, "is read at the entry, but the result is not declared to be returned through it");
    }
    if (hooked) {
        for (uint32_t reg : { 16u, 17u }) {
            if (use.Read(reg)) fail(reg, "is read at the entry: an entry hook would destroy it");
        }
    }
    std::string scanned = Format("%s%s%u instructions scanned to %s", seen.c_str(), seen.empty() ? "" : "; ", use.Scanned, Describe(use.End));
    if (!wrong.empty()) return checks.Add(what, false, wrong + " (" + scanned + ")");
    return checks.Add(what, true, scanned);
}

// The declared result against the function's returns. Only a function
// declared to return nothing can be contradicted: by a return that sets x0.
bool CheckReturns(Entry const& entry, ReturnUse const& use, Checks checks)
{
    std::string seen = Format("%u return(s): %u set x0, %u follow a call", use.Returns, use.SetX0, use.AfterCall);
    bool nothing = entry.Call.Result == Returns::Nothing || entry.Call.Result == Returns::IndirectX8 || entry.Call.Result == Returns::V0V1;
    if (nothing && use.SetX0) return checks.Add("result", false, "x0 is set before a return, but no result in x0 is declared (" + seen + ")");
    return checks.Add("result", true, seen);
}

// A store to the stack found before a call: [sp + Offset], Bytes wide.
struct StackStore
{
    uint32_t Offset;
    uint32_t Bytes;
};

// The stores to [sp + offset] on the straight path into the call at `site`,
// the nearest one for each offset first.
std::vector<StackStore> StoresBeforeCall(GameImage const& image, uint64_t begin, uint64_t site)
{
    constexpr uint32_t kSp = 31, kWindow = 96;
    std::vector<StackStore> stores;
    auto add = [&](uint32_t offset, uint32_t bytes) {
        for (auto const& store : stores) {
            if (offset < store.Offset + store.Bytes && store.Offset < offset + bytes) return;     // a later store covers it
        }
        stores.push_back({ offset, bytes });
    };
    uint32_t scanned = 0;
    for (uint64_t va = site; va > begin && scanned < kWindow; scanned++) {
        va -= 4;
        uint32_t i = image.Word(va);
        // A call, jump or return ends the straight path.
        if ((i & 0x7c000000) == 0x14000000 || (i & 0xff9ffc1f) == 0xd61f0000) break;
        if (RegN(i) != kSp) continue;
        bool vector = (i >> 26) & 1;
        if ((i & 0x3b000000) == 0x39000000 && ((i >> 22) & 3) == 0 && !vector) {            // str, strh, strb [sp, #imm]
            add(*LoadStoreOffset(i), 1u << (i >> 30));
        } else if ((i & 0x3b200c00) == 0x38000000 && ((i >> 22) & 3) == 0 && !vector) {     // stur
            int32_t offset = (int32_t)((i >> 12) & 0x1ff);
            if (offset & 0x100) offset -= 0x200;
            if (offset >= 0) add((uint32_t)offset, 1u << (i >> 30));
        } else if ((i & 0x3fc00000) == 0x29000000) {                                        // stp, signed offset
            auto offset = PairOffset(i);
            uint32_t bytes = (i >> 31) ? 8 : 4;
            if (offset && *offset >= 0) {
                add((uint32_t)*offset, bytes);
                add((uint32_t)*offset + bytes, bytes);
            }
        }
    }
    return stores;
}

// The signature's stack block against what a named caller stores before its
// call of the function.
bool CheckStackBlock(Lookups const& in, Entry const& entry, uint64_t function, Checks checks)
{
    auto caller = LocateFunction(in, entry.Caller);
    if (!caller.Ok()) return checks.Add("stack block", false, caller.Problem);
    auto sites = CallsTo(*in.Image, caller.VA, caller.End, function);
    if (sites.empty()) return checks.Add("stack block", false, Describe(entry.Caller) + " does not call the function");
    for (uint64_t site : sites) {
        auto stores = StoresBeforeCall(*in.Image, caller.VA, site);
        for (auto const& arg : entry.Call.Arguments()) {
            if (arg.At != Place::Stack) continue;
            auto it = std::find_if(stores.begin(), stores.end(), [&](StackStore const& s) { return s.Offset == arg.StackOffset; });
            if (it == stores.end()) {
                return checks.Add("stack block", false, Format("the caller stores nothing at [sp, #0x%x] before the call", arg.StackOffset));
            }
            if (it->Bytes != arg.Bytes) {
                return checks.Add("stack block", false,
                    Format("the caller stores %u bytes at [sp, #0x%x], declared %u", it->Bytes, arg.StackOffset, arg.Bytes));
            }
        }
    }
    return checks.Add("stack block", true, Format("%zu call(s) in %s store each stack argument at its declared offset and width", sites.size(),
        Describe(entry.Caller).c_str()));
}

void CheckOperands(Lookups const& in, Entry const& entry, Function const* own, Resolved& r, Checks checks)
{
    auto const& image = *in.Image;
    r.Operands.assign(entry.Operands.size(), 0);
    for (size_t n = 0; n < entry.Operands.size(); n++) {
        auto const& operand = entry.Operands[n];
        Function f;
        if (operand.In) f = LocateFunction(in, operand.In);
        else if (own) f = *own;
        else f.Problem = "the table names no function to read it from";
        if (!f.Ok()) { checks.Add(operand.What, false, f.Problem); continue; }

        if (operand.How == Operand::Read::CallCount) {
            std::string problem;
            Callee callee = operand.Import ? CallsImport(operand.Import) : Calls(operand.Target);
            uint64_t calls = CallSites(in, f.VA, f.End, callee, problem).size();
            if (!problem.empty()) { checks.Add(operand.What, false, problem); continue; }
            r.Operands[n] = calls;
            std::string where = operand.In ? Describe(operand.In) : std::string("the function");
            checks.Add(operand.What, calls == operand.Expected, Format("%llu call(s) to %s in %s%s", (unsigned long long)calls,
                Describe(callee).c_str(), where.c_str(), calls == operand.Expected ? "" : Format(", recorded %llu",
                (unsigned long long)operand.Expected).c_str()));
            continue;
        }
        if (operand.How == Operand::Read::StoreOrder) {
            std::string problem;
            uint64_t target = Locate(in, operand.Target, problem);
            if (!target) { checks.Add(operand.What, false, problem); continue; }
            uint64_t index = 0, found = 0;
            for (uint64_t va = f.VA; va + 8 <= f.End; va += 4) {
                uint32_t store = image.Word(va + 4);
                if ((store & 0xffc00000) != 0xf9000000) continue;       // str xT, [xN, #offset]
                uint64_t address = PageAddress(va, image.Word(va), store);
                if (!address) continue;
                if (index++ == operand.Expected) { found = address; break; }
            }
            r.Operands[n] = found;
            checks.Add(operand.What, found == target, found == target
                ? Format("store %llu of %s is to %s", (unsigned long long)operand.Expected, Describe(operand.In).c_str(), Describe(operand.Target).c_str())
                : Format("store %llu of %s is not to %s", (unsigned long long)operand.Expected, Describe(operand.In).c_str(), Describe(operand.Target).c_str()));
            continue;
        }
        auto matches = FindPattern(image, f.VA, f.End, operand.Pattern);
        if (matches.size() != 1) {
            checks.Add(operand.What, false, Format("its instructions match %zu times, expected once", matches.size()));
            continue;
        }
        uint64_t at = matches[0] + 4ull * operand.At;
        uint32_t word = image.Word(at);
        if (operand.How == Operand::Read::BranchTarget) {
            std::string problem;
            uint64_t target = Locate(in, operand.Target, problem);
            uint64_t found = DirectBranchTarget(at, word);
            r.Operands[n] = found;
            if (!target) checks.Add(operand.What, false, problem);
            else checks.Add(operand.What, found == target, found == target ? "goes to " + Describe(operand.Target) : "does not go to " + Describe(operand.Target));
            continue;
        }
        if (operand.How == Operand::Read::PointerLoad) {
            std::string problem;
            uint64_t target = Locate(in, operand.Target, problem);
            uint32_t load = at + 8 <= f.End ? image.Word(at + 4) : 0;
            bool wide = (load & 0xffc00000) == 0xf9400000;      // ldr xT, [xN, #offset]
            uint64_t found = wide ? PageAddress(at, word, load) : 0;
            r.Operands[n] = found;
            if (!target) checks.Add(operand.What, false, problem);
            else checks.Add(operand.What, found == target, found == target ? "a 64-bit load from " + Describe(operand.Target)
                : "the instructions are not a 64-bit load from " + Describe(operand.Target));
            continue;
        }
        std::optional<uint64_t> value;
        switch (operand.How) {
        case Operand::Read::LoadStoreOffset: if (auto v = LoadStoreOffset(word)) value = *v; break;
        case Operand::Read::AddImmediate: if (auto v = AddImmediate(word)) value = *v; break;
        case Operand::Read::MoveValue: value = MovzValue(word); break;
        default: break;
        }
        if (!value) { checks.Add(operand.What, false, "the instruction does not have the operand"); continue; }
        r.Operands[n] = *value;
        checks.Add(operand.What, *value == operand.Expected,
            *value == operand.Expected ? Format("0x%llx", (unsigned long long)*value)
                                       : Format("0x%llx, recorded 0x%llx", (unsigned long long)*value, (unsigned long long)operand.Expected));
    }
}

std::string DescribeSection(GameImage::Section const& section)
{
    return Format("%s,%s%s", section.Segment, section.Name, section.ZeroFill ? " (zero-fill)" : "");
}

void ResolveFunction(Lookups const& in, Entry const& entry, Resolved& r)
{
    Checks checks{ r.Checks };
    auto f = LocateFunction(in, entry.Where);
    if (!checks.Add("symbol", f.Ok(), f.Ok() ? Describe(entry.Where) : f.Problem)) return;
    r.SeenVA = f.VA;
    r.End = f.End;

    CheckShape(in, f, entry.Shape, "shape", checks, &r.Shape);

    auto const& image = *in.Image;
    std::span<uint32_t const> words(static_cast<uint32_t const*>(image.CodeAt(f.VA)), f.Instructions());
    bool hooked = entry.What != Kind::FunctionToCall;
    r.Use = ScanEntry(words, f.VA, in.Probe);
    r.Scanned = true;
    CheckArguments(entry.Call, r.Use, hooked, false, "arguments", checks);

    r.Returns = ScanReturns(words);
    CheckReturns(entry, r.Returns, checks);

    size_t selfCalls = CallsTo(image, f.VA, f.End, f.VA).size();
    checks.Add("calls itself", (selfCalls != 0) == entry.CallsItself,
        Format("%zu call(s) to itself, the table says %s", selfCalls, entry.CallsItself ? "it calls itself" : "none"));

    if (hooked) {
        r.Hook = CheckEntryHook(image, f.VA, f.End);
        checks.Add("entry hook", r.Hook == HookRefusal::None, r.Hook == HookRefusal::None ? "would go in" : bg3se::mac::Describe(r.Hook));
    }
    if (entry.Caller) CheckStackBlock(in, entry, f.VA, checks);
    CheckOperands(in, entry, &f, r, checks);
}

void ResolveCallSite(Lookups const& in, Entry const& entry, Resolved& r)
{
    Checks checks{ r.Checks };
    if (!entry.Derived) { checks.Add("table", false, "a call site needs a derivation"); return; }
    r.SeenVA = Derive(in, *entry.Derived, checks);
    if (!r.SeenVA) return;
    r.End = r.SeenVA + 4;
    bool isCall = CallTarget(r.SeenVA, in.Image->Word(r.SeenVA)) != 0;
    checks.Add("call", isCall, isCall ? "a bl" : "the place is not a call instruction");
    CheckOperands(in, entry, nullptr, r, checks);
}

void ResolveCodeSite(Lookups const& in, Entry const& entry, Resolved& r)
{
    Checks checks{ r.Checks };
    auto d = entry.Derived;
    if (!d || d->What != Derivation::Take::Site || d->At >= d->Pattern.size()) {
        checks.Add("table", false, "a code site needs a derivation whose pattern holds the site");
        return;
    }
    // The pattern says what the replaced instruction is: every bit of it, or
    // for a call that it is the one call to the anchor's callee.
    auto const& was = d->Pattern[d->At];
    bool isAnchorCall = d->Anchor && d->AnchorAt == d->At && was.Mask == Any::Branch && was.Value == 0x94000000;
    if (was.Mask != 0xffffffff && !isAnchorCall) {
        checks.Add("table", false, "the pattern does not state the instruction that is replaced");
        return;
    }
    if (entry.Becomes == 0) {
        checks.Add("table", false, "a code site needs the instruction it is given");
        return;
    }
    r.SeenVA = Derive(in, *d, checks);
    if (!r.SeenVA) return;
    r.End = r.SeenVA + 4;
    checks.Add("instruction", true, Format("%08x, to become %08x", in.Image->Word(r.SeenVA), entry.Becomes));
    CheckOperands(in, entry, nullptr, r, checks);
}

void ResolveGlobal(Lookups const& in, Entry const& entry, Resolved& r)
{
    Checks checks{ r.Checks };
    auto const& image = *in.Image;
    bool ownSymbol = false;
    if (entry.Derived) {
        r.SeenVA = Derive(in, *entry.Derived, checks);
        if (!r.SeenVA) return;
        if (entry.Agrees) {
            std::string problem;
            uint64_t named = Locate(in, entry.Agrees, problem);
            ownSymbol = named == r.SeenVA;
            checks.Add("agrees", ownSymbol, ownSymbol ? "the address is " + Describe(entry.Agrees)
                : named ? "the address is not " + Describe(entry.Agrees) : problem);
        }
    } else {
        std::string problem;
        r.SeenVA = Locate(in, entry.Where, problem);
        if (!checks.Add("symbol", r.SeenVA != 0, r.SeenVA ? Describe(entry.Where) : problem)) return;
        ownSymbol = true;
    }

    auto section = image.SectionAt(r.SeenVA);
    bool data = section && !section->Code;
    checks.Add("section", data, section ? DescribeSection(*section) : "in no section of the image");

    bool pointer = entry.What == Kind::GlobalPointer;
    if (pointer) checks.Add("alignment", (r.SeenVA & 7) == 0, (r.SeenVA & 7) == 0 ? "aligned for a pointer" : "not aligned for a pointer");
    // Only an address with a symbol of its own has a distance to the next one that says anything.
    if (ownSymbol) {
        uint64_t next = in.Next(r.SeenVA);
        uint64_t room = next > r.SeenVA ? next - r.SeenVA : 0;
        if (section && (!next || next > section->End)) room = section->End - r.SeenVA;
        if (pointer) {
            checks.Add("size", room == 8, Format("%llu bytes to the next symbol; a pointer is 8", (unsigned long long)room));
        } else if (entry.Bytes) {
            checks.Add("size", room >= entry.Bytes, Format("%llu bytes to the next symbol; at least %u needed", (unsigned long long)room, entry.Bytes));
        }
    }
    CheckOperands(in, entry, nullptr, r, checks);
}

void ResolveVtable(Lookups const& in, Entry const& entry, Resolved& r)
{
    constexpr uint64_t kAddressPoint = 0x10;    // after the offset to top and the type information pointer
    Checks checks{ r.Checks };
    auto const& image = *in.Image;
    std::string problem;
    uint64_t symbol = Locate(in, entry.Where, problem);
    if (!checks.Add("symbol", symbol != 0, symbol ? Describe(entry.Where) : problem)) return;
    r.SeenVA = symbol + kAddressPoint;
    auto section = image.SectionAt(symbol);
    checks.Add("section", section && !section->Code && !section->ZeroFill, section ? DescribeSection(*section) : "in no section of the image");
    uint64_t end = in.Next(symbol);
    if (section && (!end || end > section->End)) end = section->End;
    r.End = end;
    CheckOperands(in, entry, nullptr, r, checks);
    if (image.PointersOnlyAtRunTime()) {
        checks.Add("slots", false, "the pointers of this file are fixup chains: the slots can only be read in the running game");
        return;
    }
    for (auto const& method : entry.Methods) {
        SlotResult slot{ &method };
        slot.Function = Locate(in, method.Function, problem);
        if (!slot.Function) {
            checks.Add(method.Name, false, problem);
            r.Slots.push_back(slot);
            continue;
        }
        auto slots = VtableSlotsOf(image, r.SeenVA, end, slot.Function);
        if (slots.size() == 1) slot.Offset = (int32_t)(slots[0] - r.SeenVA);
        r.Slots.push_back(slot);
        if (slots.size() != 1) {
            checks.Add(method.Name, false, Format("%s is in %zu slots of the vtable, expected one", Describe(method.Function).c_str(), slots.size()));
        } else if (slot.Offset != method.Offset) {
            checks.Add(method.Name, false, Format("%s is at +0x%x, recorded +0x%x", Describe(method.Function).c_str(), slot.Offset, method.Offset));
        } else {
            checks.Add(method.Name, true, Format("+0x%x: %s", slot.Offset, Describe(method.Function).c_str()));
        }
        if (!method.Call) continue;
        auto f = LocateFunction(in, method.Function);
        if (!f.Ok()) {
            checks.Add(method.Name, false, f.Problem);
            continue;
        }
        std::span<uint32_t const> words(static_cast<uint32_t const*>(image.CodeAt(f.VA)), f.Instructions());
        CheckArguments(*method.Call, ScanEntry(words, f.VA, in.Probe), false, true, method.Name, checks);
    }
}

void ResolveTypeIndices(Lookups const& in, Entry const& entry, TypeIndices const& types, Resolved& r)
{
    Checks checks{ r.Checks };
    size_t outside = 0;
    for (auto const& index : types.All()) {
        if (index.Context != entry.Context) continue;
        r.TypeCount++;
        auto section = in.Image->SectionAt(index.VA);
        if (!section || section->Code) outside++;
    }
    checks.Add("symbols", r.TypeCount != 0, Format("%zu ls::TypeId<T, %s>::m_TypeIndex", r.TypeCount, entry.Context));
    if (!r.TypeCount) return;
    checks.Add("section", outside == 0, outside ? Format("%zu are not in a data section", outside) : "all in data sections");
    // A type named at two addresses could not be looked up by its name.
    std::vector<TypeIndex const*> sorted;
    for (auto const& index : types.All()) {
        if (index.Context == entry.Context) sorted.push_back(&index);
    }
    std::sort(sorted.begin(), sorted.end(), [](TypeIndex const* a, TypeIndex const* b) { return a->Type < b->Type; });
    size_t twice = 0;
    for (size_t i = 1; i < sorted.size(); i++) twice += sorted[i]->Type == sorted[i - 1]->Type && sorted[i]->VA != sorted[i - 1]->VA;
    checks.Add("names", twice == 0, twice ? Format("%zu type names are given to more than one address", twice) : "each type name at one address");

    // The context's state: where the game says that the indices are given.
    if (entry.Where) {
        std::string problem;
        r.SeenVA = Locate(in, entry.Where, problem);
        if (!checks.Add("state", r.SeenVA != 0, r.SeenVA ? Describe(entry.Where) : problem)) return;
        auto section = in.Image->SectionAt(r.SeenVA);
        bool data = section && !section->Code;
        checks.Add("state section", data, section ? DescribeSection(*section) : "in no section of the image");
        uint64_t next = in.Next(r.SeenVA);
        uint64_t room = next > r.SeenVA ? next - r.SeenVA : 0;
        if (section && (!next || next > section->End)) room = section->End - r.SeenVA;
        checks.Add("state size", room >= sizeof(GameTypeContextState) && (r.SeenVA & 7) == 0,
            Format("%llu bytes to the next symbol; the state is %zu", (unsigned long long)room, sizeof(GameTypeContextState)));
    }
    CheckOperands(in, entry, nullptr, r, checks);
}

void ResolveAbsent(Lookups const& in, Entry const& entry, Resolved& r)
{
    Checks checks{ r.Checks };
    if (entry.Gone.NoSymbol) {
        auto symbol = in.GetPrefix(entry.Gone.NoSymbol);
        bool none = symbol.Status == GameImage::SymbolStatus::Missing;
        checks.Add("no symbol", none, none ? Format("nothing is named %s...", entry.Gone.NoSymbol)
            : Format("a symbol named %s... exists: the function may have code of its own now", entry.Gone.NoSymbol));
    }
    bool shaped = !entry.Gone.HostShapes.empty();
    if (shaped && entry.Gone.HostShapes.size() != entry.Gone.Hosts.size()) {
        checks.Add("table", false, "the entry does not record one shape for each host");
        shaped = false;
    }
    for (size_t n = 0; n < entry.Gone.Hosts.size(); n++) {
        auto const& host = entry.Gone.Hosts[n];
        auto f = LocateFunction(in, host);
        checks.Add("host", f.Ok(), f.Ok() ? Describe(host) : f.Problem);
        if (f.Ok() && shaped && entry.Gone.HostShapes[n] != CodeShape{}) CheckShape(in, f, entry.Gone.HostShapes[n], "host shape", checks);
    }
    CheckOperands(in, entry, nullptr, r, checks);
}

void Collect(Lookups& in, Entry const& entry)
{
    auto wantDerivation = [&](Derivation const& d) {
        in.Want(d.Host);
        in.Want(d.Anchor.Function);
    };
    in.Want(entry.Where);
    if (entry.Derived) wantDerivation(*entry.Derived);
    in.Want(entry.Caller);
    in.Want(entry.Agrees);
    for (auto const& method : entry.Methods) in.Want(method.Function);
    for (auto const& operand : entry.Operands) {
        in.Want(operand.In);
        in.Want(operand.Target);
    }
    in.WantAbsent(entry.Gone.NoSymbol);
    for (auto const& host : entry.Gone.Hosts) in.Want(host);
}

void Finish(Resolved& r)
{
    auto const& entry = *r.Of;
    for (auto const& check : r.Checks) {
        if (check.Passed) continue;
        r.Is = Status::Unresolved;
        r.Reason = std::string(check.What) + ": " + check.Detail;
        return;
    }
    if (entry.What == Kind::NoCallableCode) {
        r.Is = Status::NoCallableCode;
        return;
    }
    r.Is = entry.Caveat ? Status::ResolvedWithCaveat : Status::Resolved;
    r.VA = r.SeenVA;
}

}

Status Resolved::Expected() const
{
    if (Of->Unsettled) return Status::Unresolved;
    if (Of->What == Kind::NoCallableCode) return Status::NoCallableCode;
    return Of->Caveat ? Status::ResolvedWithCaveat : Status::Resolved;
}

Resolution Resolve(std::span<Area const> table, Images const& images)
{
    Resolution result;
    result.In = images;

    Lookups lookups[(size_t)Image::Count];
    bool wantTypes = false;
    for (size_t i = 0; i < (size_t)Image::Count; i++) {
        auto const& view = images.Of((Image)i);
        lookups[i].Image = view.Symbols;
        lookups[i].Functions = view.Functions;
    }
    for (auto const& area : table) {
        for (auto const& entry : area.Entries) {
            Collect(lookups[(size_t)entry.In], entry);
            wantTypes = wantTypes || entry.What == Kind::TypeIndex;
        }
    }
    for (auto& in : lookups) in.Run();
    if (wantTypes && images.Game.Symbols) result.Types = TypeIndices::Collect(*images.Game.Symbols);

    for (auto const& area : table) {
        for (auto const& entry : area.Entries) {
            Resolved r;
            r.Of = &entry;
            r.Area = area.Name;
            auto const& in = lookups[(size_t)entry.In];
            if (entry.Unsettled) {
                r.Reason = entry.Unsettled;
            } else if (!in.Image) {
                r.Reason = std::string("the image is not given: ") + Describe(entry.In);
            } else {
                switch (entry.What) {
                case Kind::FunctionToCall:
                case Kind::FunctionToHook:
                case Kind::FunctionToCallAndHook: ResolveFunction(in, entry, r); break;
                case Kind::CallSite: ResolveCallSite(in, entry, r); break;
                case Kind::CodeSite: ResolveCodeSite(in, entry, r); break;
                case Kind::GlobalObject:
                case Kind::GlobalPointer: ResolveGlobal(in, entry, r); break;
                case Kind::Vtable: ResolveVtable(in, entry, r); break;
                case Kind::TypeIndex: ResolveTypeIndices(in, entry, result.Types, r); break;
                case Kind::NoCallableCode: ResolveAbsent(in, entry, r); break;
                default: break;
                }
                Finish(r);
            }
            result.Entries.push_back(std::move(r));
        }
    }

    // An entry is of no use without the one it needs: refuse it with that one.
    // Repeated until nothing changes, for an entry that needs one that needs another.
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& r : result.Entries) {
            if (!r.Of->Needs || !r.Usable()) continue;
            auto needed = result.Find(r.Of->Needs);
            if (needed && needed->Usable()) continue;
            r.Checks.push_back({ "needs", false, std::string(r.Of->Needs) + (needed ? " is unresolved" : " is not in the table") });
            r.Is = Status::Unresolved;
            r.Reason = "needs: " + r.Checks.back().Detail;
            r.VA = 0;
            changed = true;
        }
    }
    for (auto& r : result.Entries) {
        if (r.Of->Needs && r.Usable()) r.Checks.push_back({ "needs", true, std::string(r.Of->Needs) + " is resolved" });
    }
    return result;
}

bool LoadedGame::Open(std::string& problem)
{
    game_ = GameImage::Current();
    if (!game_) {
        problem = "the executable's symbol table cannot be read";
        return false;
    }
    if (!gameFunctions_.OpenLoaded(game_->CodeAt(game_->BaseVA()), problem)) return false;
    if (auto header = FindLoadedImage("libOsiris.dylib")) {
        std::string ignored;
        hasOsiris_ = osiris_.OpenLoaded(header) && osirisFunctions_.OpenLoaded(header, ignored);
    }
    return true;
}

Images LoadedGame::View() const
{
    Images images;
    images.Game = { game_, &gameFunctions_ };
    if (hasOsiris_) images.Osiris = { &osiris_, &osirisFunctions_ };
    return images;
}

Resolved const* Resolution::Find(std::string_view name) const
{
    for (auto const& entry : Entries) {
        if (name == entry.Of->Name) return &entry;
    }
    return nullptr;
}

void* Resolution::Address(Resolved const& entry) const
{
    if (!entry.Usable() || !entry.VA) return nullptr;
    auto image = In.Of(entry.Of->In).Symbols;
    return image && !image->FromFile() ? image->LoadedAt(entry.VA) : nullptr;
}

size_t Resolution::Count(Status status) const
{
    return (size_t)std::count_if(Entries.begin(), Entries.end(), [&](Resolved const& r) { return r.Is == status; });
}

bool Resolution::AllAsExpected() const
{
    return std::all_of(Entries.begin(), Entries.end(), [](Resolved const& r) { return r.AsExpected(); });
}

char const* Describe(Status status)
{
    switch (status) {
    case Status::Resolved: return "resolved";
    case Status::ResolvedWithCaveat: return "resolved with a caveat";
    case Status::NoCallableCode: return "no callable code";
    case Status::Unresolved: return "unresolved";
    default: return "?";
    }
}

char const* Describe(Image image)
{
    switch (image) {
    case Image::Game: return "the game executable";
    case Image::Osiris: return "libOsiris.dylib";
    default: return "?";
    }
}

char const* Describe(Kind kind)
{
    switch (kind) {
    case Kind::FunctionToCall: return "function to call";
    case Kind::FunctionToHook: return "function to hook";
    case Kind::FunctionToCallAndHook: return "function to call and hook";
    case Kind::CallSite: return "call site";
    case Kind::CodeSite: return "code site";
    case Kind::GlobalObject: return "global object";
    case Kind::GlobalPointer: return "global pointer to an object";
    case Kind::Vtable: return "vtable";
    case Kind::TypeIndex: return "type indices";
    case Kind::NoCallableCode: return "no callable code";
    default: return "?";
    }
}

char const* Describe(Place place)
{
    static char const* const kNames[] = { "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "stack" };
    return kNames[(size_t)place];
}

char const* Describe(Returns result)
{
    switch (result) {
    case Returns::Nothing: return "nothing";
    case Returns::W0: return "in w0";
    case Returns::X0: return "in x0";
    case Returns::X0W1: return "in x0 and w1";
    case Returns::V0V1: return "in s0 and s1";
    case Returns::IndirectX8: return "through the address in x8";
    case Returns::IndirectX0: return "through the address in x0, the first argument";
    }
    return "?";
}

}
