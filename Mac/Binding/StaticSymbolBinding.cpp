#include <Binding/StaticSymbolBinding.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <Binding/CallAdapters.h>
#include <Binding/HookBridges.h>
#include <Binding/NetMessages.h>
#include <Binding/OsirisBinding.h>
#include <Binding/StatusVfx.h>
#include <Binding/TemplateBank.h>
#include <Binding/TypeIndexBinding.h>
#include <Engine/Entries/OverlayNames.h>
#include <Engine/Entries/PatchNames.h>

namespace bg3se::mac::binding {

namespace {

using engine::Kind;

engine::Entry const* FindEntry(std::span<engine::Area const> table, std::string_view name)
{
    for (auto const& area : table) {
        for (auto const& entry : area.Entries) {
            if (name == entry.Name) return &entry;
        }
    }
    return nullptr;
}

bool IsGlobal(Kind kind) { return kind == Kind::GlobalObject || kind == Kind::GlobalPointer; }
bool IsCalled(Kind kind) { return kind == Kind::FunctionToCall || kind == Kind::FunctionToCallAndHook; }
bool IsHooked(Kind kind) { return kind == Kind::FunctionToHook || kind == Kind::FunctionToCallAndHook; }

bool Among(char const* name, std::span<char const* const> names)
{
    for (auto n : names) {
        if (n && std::string_view(n) == name) return true;
    }
    return false;
}

// What does not depend on a game: whether the row can take its entry at all.
// `member` is the entry of the member's own name, `source` the one the value
// comes from.
std::string RowDisagreement(MemberRow const& row, engine::Entry const* member, engine::Entry const* source,
    std::span<engine::Area const> table)
{
    if (!member) return "the engine table has no entry of the member's name";
    if (!source) return std::string("the engine table has no entry named ") + row.Entry;

    // An entry without callable code names the places that take its place:
    // the row must use exactly those, or say there is nothing.
    std::span<char const* const> replacements = member->What == Kind::NoCallableCode ? member->Gone.Replacements : std::span<char const* const>{};
    auto usesReplacements = [&](std::span<char const* const> used) -> std::string {
        for (auto r : replacements) {
            if (!Among(r, used)) return std::string("the table names ") + r + " as taking its place, and the row does not use it";
        }
        return {};
    };

    switch (row.How) {
    case Bound::Global:
        if (!IsGlobal(source->What)) return "the entry is not a global";
        return {};

    case Bound::Function: {
        bool hooked = row.Use == Through::Hook || row.AlsoHooked;
        bool called = row.Use == Through::Call;
        if (hooked && !IsHooked(source->What)) return "the entry is not a function to hook";
        if (called && !IsCalled(source->What)) return "the entry is not a function to call";
        if (source != member) {
            char const* used[] = { row.Entry };
            if (member->What != Kind::NoCallableCode) return "the member has an entry of its own and the row takes another";
            if (auto problem = usesReplacements(used); !problem.empty()) return problem;
        }
        if (auto problem = Disagreement(row.Norbyte, source->Call, row.Use, row.Allow); !problem.empty()) return problem;
        if (row.AlsoHooked) return Disagreement(row.Norbyte, source->Call, Through::Hook, row.Allow);
        return {};
    }

    case Bound::CallAdapter: {
        auto adapter = FindCallAdapter(row.Member);
        if (!adapter) return "there is no call adapter for it";
        if (!SamePassing(adapter->Norbyte, row.Norbyte)) return "the adapter's prototype does not pass what Norbyte's does";
        std::vector<char const*> needed;
        for (auto const& need : adapter->Needs) {
            needed.push_back(need.Entry);
            auto entry = FindEntry(table, need.Entry);
            if (!entry) return std::string("the adapter needs an entry the table does not have: ") + need.Entry;
            if (!need.Target) return std::string("the adapter has no place for the address of ") + need.Entry;
            if (need.Game.Present) {
                if (!IsCalled(entry->What)) return std::string(need.Entry) + " is not a function to call";
                if (auto problem = Disagreement(need.Game, entry->Call, Through::Call, kOwnPrototype); !problem.empty()) {
                    return std::string(need.Entry) + ": " + problem;
                }
            } else if (!IsGlobal(entry->What) && entry->What != Kind::Vtable) {
                return std::string(need.Entry) + " is not a global or a vtable";
            }
        }
        if (!adapter->Target) {
            if (member->What != Kind::NoCallableCode) return "the adapter calls no game function, and the table has one";
            // What does a missing function's work was written from the code
            // the function is inlined into: the entry must hold that code to its shape.
            if (!adapter->Needs.empty() && !member->Gone.ReadFromHosts()) return "the adapter does the function's work, and the entry records no shape of the code it was read from";
            return usesReplacements(needed);
        }
        if (!IsCalled(source->What)) return "the entry is not a function to call";
        if (adapter->Global) {
            auto global = FindEntry(table, adapter->Global);
            if (!global || !IsGlobal(global->What)) return std::string("the adapter reads a global the table does not have: ") + adapter->Global;
            // The offset the adapter reads there is one the entry checks in the game's code.
            bool stated = false;
            for (auto const& operand : source->Operands) {
                if (operand.How == engine::Operand::Read::LoadStoreOffset && operand.Expected == adapter->GlobalOffset) stated = true;
            }
            if (!stated) return "the offset the adapter reads is not among the entry's operands";
        }
        return Disagreement(adapter->Game, source->Call, Through::Call, kOwnPrototype);
    }

    case Bound::HookBridge: {
        auto bridge = FindHookBridge(row.Member);
        if (!bridge) return "there is no hook bridge for it";
        if (!SamePassing(bridge->Norbyte, row.Norbyte)) return "the bridge's prototype does not pass what Norbyte's does";
        for (auto hook : bridge->Hooks) {
            if (hook && !FindEntry(table, hook)) return std::string("the bridge's game hook has no entry: ") + hook;
        }
        return usesReplacements(bridge->Hooks);
    }

    case Bound::Handlers:
        if (member->What != Kind::NoCallableCode) return "the table has code for it: it needs no handlers of its own";
        return usesReplacements(ClientUpdateHooks());

    case Bound::Unavailable:
        if (member->What != Kind::NoCallableCode) return "the table has it, and the row leaves it null";
        return usesReplacements({});

    default:
        return "the row has no kind";
    }
}

// SetHookedEntry's statements. Few, and set at start-up before anything reads them.
struct Hooked
{
    std::string Entry;
    void* Function;
};
std::vector<Hooked>& HookedEntries()
{
    static std::vector<Hooked> entries;
    return entries;
}

void* HookedEntry(std::string_view entry)
{
    for (auto const& hooked : HookedEntries()) {
        if (hooked.Entry == entry) return hooked.Function;
    }
    return nullptr;
}

std::string HooksRefusal(std::span<char const* const> hooks, engine::Resolution const& resolution, bool attached)
{
    for (auto name : hooks) {
        if (!name) continue;
        if (attached) {
            if (!GameHookAttached(name)) return std::string("the game hook is not attached: ") + name;
            continue;
        }
        for (auto const& hook : GameHooks()) {
            if (std::string_view(hook.Entry) != name) continue;
            if (auto problem = GameHookRefusal(hook, resolution); !problem.empty()) return std::string(name) + ": " + problem;
        }
    }
    return {};
}

}

MemberResult DecideMember(MemberRow const& row, engine::Resolution const& resolution, bool hooksAttached, bool live)
{
    MemberResult result;
    result.Row = &row;
    auto member = resolution.Find(row.Member);
    auto source = resolution.Find(row.Entry);
    if (!member || !source) {
        result.Reason = "the engine table has no entry for it";
        return result;
    }
    if (row.How == Bound::Unavailable) return result;

    for (auto name : row.Requires) {
        if (!name) continue;
        auto required = resolution.Find(name);
        if (!required || !required->Usable()) {
            result.Reason = std::string("it needs ") + name + ", which is not resolved" + (required ? ": " + required->Reason : std::string());
            return result;
        }
    }

    auto address = [&](engine::Resolved const& entry) -> void* {
        if (!live) return reinterpret_cast<void*>(entry.VA);
        return resolution.Address(entry);
    };
    auto usable = [&]() -> bool {
        if (!source->Usable()) {
            result.Reason = std::string(row.Entry) + " is not resolved: " + source->Reason;
            return false;
        }
        if (!address(*source)) {
            result.Reason = std::string(row.Entry) + ": its image is not loaded in this process";
            return false;
        }
        return true;
    };

    switch (row.How) {
    case Bound::Global:
        if (!IsGlobal(source->Of->What)) {
            result.Reason = "the entry is not a global";
        } else if (usable()) {
            result.Value = address(*source);
        }
        break;

    case Bound::Function: {
        if (!usable()) break;
        bool hooked = row.Use == Through::Hook || row.AlsoHooked;
        result.Reason = Disagreement(row.Norbyte, source->Of->Call, row.Use, row.Allow);
        if (result.Reason.empty() && row.AlsoHooked) result.Reason = Disagreement(row.Norbyte, source->Of->Call, Through::Hook, row.Allow);
        // The game function already carries a hook of this image: its
        // function stands for it, for calls and for Norbyte's hook alike.
        if (void* stands = live ? HookedEntry(row.Entry) : nullptr; stands && result.Reason.empty()) {
            result.Value = stands;
            break;
        }
        // Norbyte's Wrap does not look at what the Detours layer answers, and
        // a refused attach fails every hook of his transaction: a function
        // that would not take an entry hook is not handed to him.
        if (result.Reason.empty() && hooked && source->Hook != HookRefusal::None) {
            result.Reason = std::string("an entry hook on ") + row.Entry + " is refused: " + mac::Describe(source->Hook);
        }
        if (result.Reason.empty()) result.Value = address(*source);
        break;
    }

    case Bound::CallAdapter: {
        auto adapter = FindCallAdapter(row.Member);
        // Everything the adapter uses besides its target is there.
        auto needsMet = [&]() -> bool {
            for (auto const& need : adapter->Needs) {
                auto needed = resolution.Find(need.Entry);
                if (!needed || !needed->Usable() || !address(*needed)) {
                    result.Reason = std::string("it needs ") + need.Entry + ", which is not resolved" + (needed ? ": " + needed->Reason : std::string());
                    return false;
                }
                if (need.Game.Present) {
                    if (auto problem = Disagreement(need.Game, needed->Of->Call, Through::Call, kOwnPrototype); !problem.empty()) {
                        result.Reason = std::string(need.Entry) + ": " + problem;
                        return false;
                    }
                }
            }
            return true;
        };
        if (!adapter) {
            result.Reason = "there is no call adapter for it";
        } else if (!adapter->Target) {
            // It calls libSystem, as the game's own code does where Norbyte's
            // function was: bound while the table's statement of that holds.
            if (member->Is != engine::Status::NoCallableCode) {
                result.Reason = std::string("the game is not confirmed to be without ") + row.Member + ": " + member->Reason;
            } else if (source->Is != engine::Status::NoCallableCode) {
                result.Reason = std::string("the game is not confirmed to be without ") + row.Entry + ": " + source->Reason;
            } else if (needsMet()) {
                result.Value = adapter->Function;
            }
        } else if (usable()) {
            result.Reason = Disagreement(adapter->Game, source->Of->Call, Through::Call, kOwnPrototype);
            if (result.Reason.empty() && adapter->Global) {
                auto global = resolution.Find(adapter->Global);
                if (!global || !global->Usable() || !address(*global)) {
                    result.Reason = std::string("it needs ") + adapter->Global + ", which is not resolved";
                }
            }
            if (result.Reason.empty() && needsMet()) result.Value = adapter->Function;
        }
        break;
    }

    case Bound::HookBridge: {
        auto bridge = FindHookBridge(row.Member);
        if (!bridge) {
            result.Reason = "there is no hook bridge for it";
            break;
        }
        result.Reason = HooksRefusal(bridge->Hooks, resolution, hooksAttached);
        if (result.Reason.empty()) result.Value = bridge->Bridge;
        break;
    }

    case Bound::Handlers:
        result.Reason = HooksRefusal(ClientUpdateHooks(), resolution, hooksAttached);
        break;

    default:
        break;
    }
    return result;
}

namespace {

BindReport Plan(engine::Resolution const& resolution, bool hooksAttached, bool live)
{
    BindReport report;
    for (auto const& row : StaticSymbolRows()) {
        auto result = DecideMember(row, resolution, hooksAttached, live);
        bool served = result.Value != nullptr || (row.How == Bound::Handlers && result.Reason.empty());
        if (result.Value) report.BoundCount++;
        if (!served && row.How != Bound::Unavailable) {
            report.Failed = true;
            if (row.Critical) report.CriticalFailed = true;
        }
        report.Members.push_back(std::move(result));
    }
    return report;
}

}

std::string CheckMemberRow(MemberRow const& row, std::span<engine::Area const> table)
{
    std::string findings;
    auto say = [&](std::string const& what) {
        findings += what;
        findings += '\n';
    };
    if (!row.WhenNull || !*row.WhenNull) say("the row does not say what Norbyte's code does with a null");
    if (auto problem = RowDisagreement(row, FindEntry(table, row.Member), FindEntry(table, row.Entry), table); !problem.empty()) say(problem);
    for (auto name : row.Requires) {
        if (name && !FindEntry(table, name)) say(std::string("it requires an entry the table does not have: ") + name);
    }
    return findings;
}

void SetHookedEntry(std::string_view entry, void* function)
{
    auto& entries = HookedEntries();
    std::erase_if(entries, [&](Hooked const& hooked) { return hooked.Entry == entry; });
    if (function) entries.push_back({ std::string(entry), function });
}

std::string CheckRows(std::span<engine::Area const> table)
{
    std::string findings;
    auto say = [&](char const* member, std::string const& what) {
        findings += member;
        findings += ": ";
        findings += what;
        findings += '\n';
    };

    // Every byte of the structure, once.
    size_t size = StaticSymbolsSize();
    std::vector<char const*> owner(size, nullptr);
    auto claim = [&](char const* name, size_t offset, size_t bytes) {
        for (size_t i = offset; i < offset + bytes; i++) {
            if (i >= size) {
                say(name, "lies outside StaticSymbols");
                return;
            }
            if (owner[i]) {
                say(name, std::string("shares its bytes with ") + owner[i]);
                return;
            }
            owner[i] = name;
        }
    };
    for (auto const& other : StaticSymbolOtherMembers()) claim(other.Name, other.Offset, other.Size);
    for (auto const& row : StaticSymbolRows()) claim(row.Member, row.Offset, sizeof(void*));
    for (size_t i = 0; i < size; i++) {
        if (!owner[i]) {
            char text[96];
            snprintf(text, sizeof(text), "the bytes at offset 0x%zx belong to no row: a member has no binding", i);
            say("StaticSymbols", text);
            while (i + 1 < size && !owner[i + 1]) i++;
        }
    }

    for (auto const& row : StaticSymbolRows()) {
        auto problems = CheckMemberRow(row, table);
        for (size_t at = 0; at < problems.size();) {
            size_t end = problems.find('\n', at);
            say(row.Member, problems.substr(at, end - at));
            at = end + 1;
        }
    }

    // Every adapter and bridge belongs to a row, and every game hook agrees
    // with the table.
    // ... of StaticSymbols, or of the story layer's (Binding/OsirisBinding.h).
    auto rowOf = [](std::string_view member, Bound how) {
        for (auto const& row : StaticSymbolRows()) {
            if (member == row.Member) return row.How == how;
        }
        return OsirisHasRow(member, how);
    };
    for (auto const& adapter : CallAdapters()) {
        if (!rowOf(adapter.Entry, Bound::CallAdapter)) say(adapter.Entry, "a call adapter that no row uses");
    }
    for (auto const& bridge : HookBridges()) {
        if (!rowOf(bridge.Member, Bound::HookBridge)) say(bridge.Member, "a hook bridge that no row uses");
    }
    for (auto const& hook : GameHooks()) {
        auto entry = FindEntry(table, hook.Entry);
        auto callee = entry && entry->Needs ? FindEntry(table, entry->Needs) : nullptr;
        if (auto problem = GameHookDisagreement(hook, entry, callee); !problem.empty()) say(hook.Entry, problem);
    }

    // The contexts of the type index maps: each has its entry, with the
    // context's state to read whether the game has numbered its types.
    for (auto const& context : TypeContextRows()) {
        auto entry = FindEntry(table, context.Entry);
        if (!entry) say(context.Entry, "a type context whose entry the table does not have");
        else if (entry->What != Kind::TypeIndex || !entry->Context || !entry->Where) say(context.Entry, "the entry is not a context of type indices with its state");
    }

    // The other direction: every entry of the table is used, or named as waiting.
    auto used = [&](std::string_view name) {
        for (auto const& row : StaticSymbolRows()) {
            if (name == row.Member || name == row.Entry) return true;
            for (auto required : row.Requires) {
                if (required && name == required) return true;
            }
        }
        for (auto const& adapter : CallAdapters()) {
            if (adapter.Global && name == adapter.Global) return true;
            for (auto const& need : adapter.Needs) {
                if (name == need.Entry) return true;
            }
        }
        for (auto const& hook : GameHooks()) {
            if (name == hook.Entry) return true;
            auto entry = FindEntry(table, hook.Entry);
            if (entry && entry->Needs && name == entry->Needs) return true;
        }
        for (auto entry : kTemplateBankEntryNames) {
            if (name == entry) return true;
        }
        for (auto const& context : TypeContextRows()) {
            if (name == context.Entry) return true;
        }
        for (auto entry : kConnectMessageEntryNames) {
            if (name == entry) return true;
        }
        for (size_t cls = 0; cls < static_cast<size_t>(StatusVfxClass::Count); cls++) {
            auto entry = StatusVfxEntryName(static_cast<StatusVfxClass>(cls));
            if (entry && name == entry) return true;
        }
        for (auto entry : kStatusVfxBaseEntryNames) {
            if (name == entry) return true;
        }
        return OsirisUsesEntry(name);
    };
    for (auto const& area : table) {
        // The overlay's area is not Norbyte's StaticSymbols: the overlay
        // resolves and checks its own entries (Mac/Overlay/Dependencies.h).
        if (std::string_view(area.Name) == engine::overlay::kArea) continue;
        // Nor is the patches' area: its entries are instructions to replace,
        // which Binding/CodePatches.h names and checks one by one.
        if (std::string_view(area.Name) == engine::patches::kArea) continue;
        for (auto const& entry : area.Entries) {
            if (!used(entry.Name)) say(entry.Name, "an entry of the table that the binding does not use");
        }
    }
    return findings;
}

BindReport PlanStaticSymbols(engine::Resolution const& resolution, bool hooksAttached)
{
    return Plan(resolution, hooksAttached, false);
}

BindReport BindStaticSymbols(void* symbols, engine::Resolution const& resolution)
{
    auto report = Plan(resolution, true, true);
    for (auto const& member : report.Members) {
        if (!member.Value) continue;
        auto const& row = *member.Row;
        if (row.How == Bound::CallAdapter) {
            auto adapter = FindCallAdapter(row.Member);
            if (adapter->Target) *adapter->Target = resolution.Address(*resolution.Find(row.Entry));
            if (adapter->GlobalTarget) *adapter->GlobalTarget = resolution.Address(*resolution.Find(adapter->Global));
            for (auto const& need : adapter->Needs) *need.Target = resolution.Address(*resolution.Find(need.Entry));
        }
        memcpy(static_cast<char*>(symbols) + row.Offset, &member.Value, sizeof(void*));
    }
    return report;
}

char const* Describe(Bound how)
{
    switch (how) {
    case Bound::Global: return "global";
    case Bound::Function: return "function";
    case Bound::CallAdapter: return "call adapter";
    case Bound::HookBridge: return "hook bridge";
    case Bound::Handlers: return "handlers";
    case Bound::Unavailable: return "unavailable";
    default: return "?";
    }
}

}
