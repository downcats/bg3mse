#include <Binding/OsirisBinding.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#include <Binding/CallAdapters.h>
#include <Binding/HookBridges.h>
#include <Binding/Startup.h>
#include <Engine/EngineTable.h>
#include <Engine/Entries/OsirisNames.h>
#include <Platform/CodeWriter.h>
#include <Platform/Log.h>

namespace bg3se::mac::binding {

namespace {

namespace names = engine::osiris;

engine::Entry const* FindEntry(std::span<engine::Area const> table, std::string_view name)
{
    for (auto const& area : table) {
        for (auto const& entry : area.Entries) {
            if (name == entry.Name) return &entry;
        }
    }
    return nullptr;
}

engine::Operand const* FindOperand(engine::Entry const& entry, std::string_view what)
{
    for (auto const& operand : entry.Operands) {
        if (what == operand.What) return &operand;
    }
    return nullptr;
}

// What the resolver read for the operand on this game: its checks have
// passed when the entry is usable, so the value is the recorded one.
std::optional<uint64_t> OperandRead(engine::Resolved const& entry, std::string_view what)
{
    auto operands = entry.Of->Operands;
    for (size_t n = 0; n < operands.size() && n < entry.Operands.size(); n++) {
        if (what == operands[n].What) return entry.Operands[n];
    }
    return {};
}

engine::VtableMethod const* FindMethod(engine::Entry const& entry, std::string_view name)
{
    for (auto const& method : entry.Methods) {
        if (name == method.Name) return &method;
    }
    return nullptr;
}

std::string Hex(uint64_t value)
{
    char text[32];
    snprintf(text, sizeof(text), "%#llx", (unsigned long long)value);
    return text;
}

// The offset of the generated story's path from its writer, when the entry
// of the call that closes the writer is resolved: the writer constructed and
// the writer closed are the same local, as are the path it is constructed
// from and the path compiled. The hook that uses the offset reads the path's
// text as the header's function is proven to read one, so that proof must
// hold as well; otherwise nothing is read at the call.
ptrdiff_t StoryPathFromWriter(engine::Resolution const& resolution)
{
    auto header = resolution.Find(names::kWriteHeader);
    if (!header || !header->Usable()) return 0;
    auto flag = OperandRead(*header, names::kPathTextFlag);
    if (!flag || *flag != kGamePathTextFlag) return 0;

    auto site = resolution.Find(names::kCloseStory);
    if (!site || !site->Usable()) return 0;
    auto constructed = OperandRead(*site, names::kWriterConstructed);
    auto from = OperandRead(*site, names::kWriterConstructedFrom);
    auto closed = OperandRead(*site, names::kWriterClosed);
    auto compiled = OperandRead(*site, names::kPathCompiled);
    if (!constructed || !from || !closed || !compiled || *constructed != *closed || *from != *compiled) return 0;
    return static_cast<ptrdiff_t>(*from) - static_cast<ptrdiff_t>(*constructed);
}

NodeClassResult DecideClass(NodeClassRow const& row, engine::Resolution const& resolution, bool live)
{
    NodeClassResult result;
    result.Row = &row;
    auto entry = resolution.Find(row.Entry);
    if (!entry) {
        result.Reason = "the engine table has no entry for it";
        return result;
    }
    if (!entry->Usable()) {
        result.Reason = "not resolved: " + entry->Reason;
        return result;
    }
    for (auto const& slot : OsirisNodeSlots()) {
        if (slot.QueryOnly && !row.Query) continue;
        engine::SlotResult const* found = nullptr;
        for (auto const& candidate : entry->Slots) {
            if (candidate.Method && std::string_view(candidate.Method->Name) == slot.Name) found = &candidate;
        }
        if (!found || found->Offset < 0) {
            result.Reason = std::string(slot.Name) + " was not found in the vtable";
            return result;
        }
        if (static_cast<uint32_t>(found->Offset) != slot.InNodeVMT || (slot.InNode >= 0 && found->Offset != slot.InNode)) {
            result.Reason = std::string(slot.Name) + " is at +" + Hex(static_cast<uint64_t>(found->Offset)) + " of the vtable, where NodeVMT has it at +"
                + Hex(slot.InNodeVMT);
            return result;
        }
    }
    result.Vtable = live ? resolution.Address(*entry) : reinterpret_cast<void*>(entry->VA);
    if (!result.Vtable) result.Reason = "its image is not loaded in this process";
    return result;
}

OsirisPlan Plan(engine::Resolution const& resolution, bool hooksAttached, bool live)
{
    OsirisPlan plan;
    std::string missing;

    if (auto findings = CheckOsirisRows(engine::EngineTable()); !findings.empty()) {
        missing += "the story rows and the engine table disagree:\n" + findings;
    }

    auto decide = [&](std::span<MemberRow const> rows, std::vector<MemberResult>& results) {
        for (auto const& row : rows) {
            auto result = DecideMember(row, resolution, hooksAttached, live);
            if (!result.Value && row.Critical) missing += std::string(row.Member) + ": " + result.Reason + "\n";
            results.push_back(std::move(result));
        }
    };
    decide(OsirisGlobalRows(), plan.Globals);
    decide(OsirisHookRows(), plan.Hooks);

    if (auto library = resolution.In.Osiris.Symbols) {
        plan.Shapes = osiris::CheckOsirisShapes(*library);
        if (!plan.Shapes.AllConfirmed()) missing += "a shape of the story library is not confirmed: " + plan.Shapes.FirstProblem() + "\n";
    } else {
        missing += "libOsiris.dylib is not loaded\n";
    }

    plan.NodeHooks = true;
    for (auto const& row : OsirisNodeClasses()) {
        auto result = DecideClass(row, resolution, live);
        if (!result.Vtable) plan.NodeHooks = false;
        plan.Classes.push_back(std::move(result));
    }

    plan.StoryPathFromWriter = StoryPathFromWriter(resolution);
    plan.Available = missing.empty();
    plan.Reason = std::move(missing);
    return plan;
}

template <class Rows>
bool Covers(Rows rows, size_t size, char const* structure, std::string& findings)
{
    std::vector<char const*> owner(size, nullptr);
    bool whole = true;
    for (auto const& row : rows) {
        for (size_t i = row.Offset; i < row.Offset + sizeof(void*); i++) {
            if (i >= size || owner[i]) {
                findings += std::string(row.Member) + (i >= size ? ": lies outside " : ": shares its bytes with another row of ") + structure + "\n";
                whole = false;
                break;
            }
            owner[i] = row.Member;
        }
    }
    for (size_t i = 0; i < size; i++) {
        if (owner[i]) continue;
        findings += std::string(structure) + ": the bytes at offset " + Hex(i) + " belong to no row: a member has no binding\n";
        whole = false;
        while (i + 1 < size && !owner[i + 1]) i++;
    }
    return whole;
}

}

std::string CheckOsirisRows(std::span<engine::Area const> table)
{
    std::string findings;
    auto say = [&](std::string_view what, std::string const& problem) {
        findings += what;
        findings += ": ";
        findings += problem;
        findings += '\n';
    };

    Covers(OsirisGlobalRows(), OsirisGlobalsSize(), "OsirisStaticGlobals", findings);
    Covers(OsirisHookRows(), sizeof(OsirisHookTargets), "OsirisHookTargets", findings);

    auto check = [&](std::span<MemberRow const> rows) {
        for (auto const& row : rows) {
            auto problems = CheckMemberRow(row, table);
            for (size_t at = 0; at < problems.size();) {
                size_t end = problems.find('\n', at);
                say(row.Member, problems.substr(at, end - at));
                at = end + 1;
            }
        }
    };
    check(OsirisGlobalRows());
    check(OsirisHookRows());

    // The node classes: each type once, and every slot where NodeVMT, the
    // class Node of this build and the table have it.
    bool typed[kNodeTypes]{};
    for (auto const& row : OsirisNodeClasses()) {
        if (row.Type == 0 || row.Type >= kNodeTypes || typed[row.Type]) {
            say(row.Entry, "its node type is not one of the nine, or has a row already");
            continue;
        }
        typed[row.Type] = true;
        auto entry = FindEntry(table, row.Entry);
        if (!entry || entry->What != engine::Kind::Vtable) {
            say(row.Entry, "the engine table has no vtable of that name");
            continue;
        }
        for (auto const& slot : OsirisNodeSlots()) {
            auto method = FindMethod(*entry, slot.Name);
            if (slot.QueryOnly && !row.Query) {
                if (method) say(row.Entry, std::string("the table gives the class ") + slot.Name + ", which only a query node has here");
                continue;
            }
            if (!method) {
                say(row.Entry, std::string("the table does not say where the class has ") + slot.Name);
                continue;
            }
            if (method->Offset != slot.Expected || slot.InNodeVMT != slot.Expected) {
                say(row.Entry, std::string(slot.Name) + ": the table has it at +" + Hex(method->Offset) + ", NodeVMT at +" + Hex(slot.InNodeVMT)
                    + ", Layout/OsirisLayout.h at +" + Hex(slot.Expected));
            }
            if (slot.InNode >= 0 && static_cast<uint32_t>(slot.InNode) != slot.Expected) {
                say(row.Entry, std::string(slot.Name) + ": a call through Norbyte's class Node goes to +" + Hex(static_cast<uint64_t>(slot.InNode))
                    + " of the vtable, the library has the function at +" + Hex(slot.Expected));
            }
        }
        for (auto const& method : entry->Methods) {
            bool known = false;
            for (auto const& slot : OsirisNodeSlots()) known = known || std::string_view(slot.Name) == method.Name;
            if (!known) say(row.Entry, std::string("the table has a slot the binding does not compare with NodeVMT: ") + method.Name);
        }
    }
    for (size_t type = 1; type < kNodeTypes; type++) {
        if (!typed[type]) say("node classes", "node type " + std::to_string(type) + " has no row");
    }

    // What the story file hooks read: the table checks the same offsets in
    // the game's code.
    if (auto entry = FindEntry(table, names::kWriteHeader)) {
        auto flag = FindOperand(*entry, names::kPathTextFlag);
        if (!flag || flag->Expected != kGamePathTextFlag) {
            say(names::kWriteHeader, "the table does not check that a path's text is told by the byte at +" + Hex(kGamePathTextFlag));
        }
    }
    if (auto entry = FindEntry(table, names::kCloseStory)) {
        auto constructed = FindOperand(*entry, names::kWriterConstructed);
        auto from = FindOperand(*entry, names::kWriterConstructedFrom);
        auto closed = FindOperand(*entry, names::kWriterClosed);
        auto compiled = FindOperand(*entry, names::kPathCompiled);
        if (!constructed || !from || !closed || !compiled) {
            say(names::kCloseStory, "the table does not read where the story's writer and its path are");
        } else if (constructed->Expected != closed->Expected || from->Expected != compiled->Expected || from->Expected == constructed->Expected) {
            say(names::kCloseStory, "the table's offsets of the story's writer and its path do not agree with each other");
        }
    }
    return findings;
}

bool OsirisUsesEntry(std::string_view entry)
{
    auto among = [&](std::span<MemberRow const> rows) {
        for (auto const& row : rows) {
            if (entry == row.Member || entry == row.Entry) return true;
            for (auto required : row.Requires) {
                if (required && entry == required) return true;
            }
        }
        return false;
    };
    if (among(OsirisGlobalRows()) || among(OsirisHookRows())) return true;
    for (auto const& row : OsirisNodeClasses()) {
        if (entry == row.Entry) return true;
    }
    return false;
}

bool OsirisHasRow(std::string_view member, Bound how)
{
    for (auto rows : { OsirisGlobalRows(), OsirisHookRows() }) {
        for (auto const& row : rows) {
            if (member == row.Member) return row.How == how;
        }
    }
    return false;
}

namespace {

// The function each wrapped slot held, for as long as the slot holds a
// replacement. A slot that could not be given its function back stays here:
// whoever wraps it again is handed the game's function, not the replacement
// that is still in the slot.
struct WrappedSlot
{
    void** Slot;
    void* Original;
};
std::mutex gWrappedSlotsMutex;
std::vector<WrappedSlot> gWrappedSlots;

void* WrappedOriginal(void** slot)
{
    for (auto const& wrapped : gWrappedSlots) {
        if (wrapped.Slot == slot) return wrapped.Original;
    }
    return nullptr;
}

void ForgetWrapped(void** slot)
{
    std::erase_if(gWrappedSlots, [&](WrappedSlot const& wrapped) { return wrapped.Slot == slot; });
}

}

std::string WrapVtableSlots(std::span<VtableSlotWrap const> slots)
{
    std::lock_guard lock(gWrappedSlotsMutex);
    for (size_t n = 0; n < slots.size(); n++) {
        auto const& slot = slots[n];
        void* held;
        memcpy(&held, slot.Slot, sizeof(held));
        if (held == slot.Replacement) {
            // Still wrapped from before (it could not be unwrapped then): the
            // function it held is the one that was kept, never the replacement.
            void* original = WrappedOriginal(slot.Slot);
            if (original) {
                *slot.Original = original;
                continue;
            }
            std::string problem = "a slot already holds its replacement, and the function it held is not known";
            for (size_t back = n; back-- > 0;) {
                memcpy(&held, slots[back].Slot, sizeof(held));
                if (held != slots[back].Replacement) continue;
                if (auto failed = StorePointer(slots[back].Slot, *slots[back].Original); failed.empty()) {
                    ForgetWrapped(slots[back].Slot);
                } else {
                    problem += "; and a slot that was replaced could not be given its function back, so it stays wrapped: " + failed;
                }
            }
            return problem;
        }
        // Kept before the store: once the slot holds the replacement, another
        // thread may be in it and ask for the original.
        *slot.Original = held;
        gWrappedSlots.push_back({ slot.Slot, held });
        auto problem = StorePointer(slot.Slot, slot.Replacement);
        if (problem.empty()) continue;
        // This slot too, should the refusal have come after its store.
        for (size_t back = n + 1; back-- > 0;) {
            memcpy(&held, slots[back].Slot, sizeof(held));
            if (held != slots[back].Replacement) {
                ForgetWrapped(slots[back].Slot);
                continue;
            }
            if (auto failed = StorePointer(slots[back].Slot, *slots[back].Original); failed.empty()) {
                ForgetWrapped(slots[back].Slot);
            } else {
                problem += "; and a slot that was replaced could not be given its function back, so it stays wrapped: " + failed;
            }
        }
        return problem;
    }
    return {};
}

std::string UnwrapVtableSlots(std::span<VtableSlotWrap const> slots)
{
    std::lock_guard lock(gWrappedSlotsMutex);
    std::string problem;
    for (auto const& slot : slots) {
        void* held;
        memcpy(&held, slot.Slot, sizeof(held));
        if (held != slot.Replacement) {
            ForgetWrapped(slot.Slot);
            continue;
        }
        if (*slot.Original == nullptr) continue;
        auto failed = StorePointer(slot.Slot, *slot.Original);
        // A refusal may come after the store was made: what the slot holds decides.
        memcpy(&held, slot.Slot, sizeof(held));
        if (held != slot.Replacement) {
            ForgetWrapped(slot.Slot);
        } else if (problem.empty()) {
            problem = failed.empty() ? "a slot still holds its replacement after the store" : failed;
        }
    }
    return problem;
}

OsirisPlan PlanOsiris(engine::Resolution const& resolution, bool hooksAttached)
{
    return Plan(resolution, hooksAttached, false);
}

OsirisBound BindOsiris(void* globals)
{
    auto resolution = ResolutionForLibraries();
    if (!resolution) {
        Log(LogLevel::Error, "Osiris: the engine table was not resolved for this game; the story layer stays off");
        return {};
    }

    auto plan = Plan(*resolution, true, true);
    if (!plan.Available) {
        Log(LogLevel::Error, "Osiris: the story layer stays off. None of its hooks goes on libOsiris.dylib and none of the library's "
            "structures is read:\n%s", plan.Reason.c_str());
        return {};
    }

    OsirisBound bound;
    for (auto const& member : plan.Globals) {
        auto const& row = *member.Row;
        if (row.How == Bound::CallAdapter) {
            auto adapter = FindCallAdapter(row.Member);
            if (adapter->Target) *adapter->Target = resolution->Address(*resolution->Find(row.Entry));
        }
        memcpy(static_cast<char*>(globals) + row.Offset, &member.Value, sizeof(void*));
    }
    size_t wrapped = 0;
    std::string off;
    for (auto const& member : plan.Hooks) {
        auto const& row = *member.Row;
        if (!member.Value) {
            Log(LogLevel::Warning, "Osiris: %s is not hooked: %s. %s", row.Member, member.Reason.c_str(), row.WhenNull);
            off += off.empty() ? "; off: " : ", ";
            off += row.Member;
            continue;
        }
        wrapped++;
        memcpy(reinterpret_cast<char*>(&bound.Hooks) + row.Offset, &member.Value, sizeof(void*));
    }

    if (plan.NodeHooks) {
        for (auto const& cls : plan.Classes) bound.Vtables[cls.Row->Type] = cls.Vtable;
    } else {
        for (auto const& cls : plan.Classes) {
            if (!cls.Vtable) Log(LogLevel::Warning, "Osiris: the vtable of %s is not handed over: %s", cls.Row->Entry, cls.Reason.c_str());
        }
        Log(LogLevel::Warning, "Osiris: the node hooks stay off (no node vtable is changed): story events do not reach Lua listeners "
            "and the story debugger does not start");
    }

    SetStoryPathFromWriter(plan.StoryPathFromWriter);
    bound.Available = true;
    Log(off.empty() ? LogLevel::Info : LogLevel::Warning, "Osiris: %zu globals bound, %zu of %zu functions to hook, node hooks %s%s",
        plan.Globals.size(), wrapped, plan.Hooks.size(), plan.NodeHooks ? "available" : "off", off.c_str());
    return bound;
}

}
