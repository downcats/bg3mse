#include <Binding/Libraries.h>

#include <atomic>
#include <cstring>

#include <Binding/GameVersion.h>
#include <Binding/HookBridges.h>
#include <Binding/Layouts.h>
#include <Binding/NetMessages.h>
#include <Binding/Startup.h>
#include <Binding/StaticSymbolBinding.h>
#include <Binding/StatusVfx.h>
#include <Release/GameAdvice.h>
#include <Binding/TemplateBank.h>
#include <Binding/TypeIndexBinding.h>
#include <Engine/EngineTable.h>
#include <Platform/Log.h>

namespace bg3se::mac::binding {

namespace {

std::atomic<engine::Resolution const*> gResolution{ nullptr };

}

void SetResolutionForLibraries(engine::Resolution const* resolution)
{
    gResolution.store(resolution, std::memory_order_release);
}

engine::Resolution const* ResolutionForLibraries()
{
    return gResolution.load(std::memory_order_acquire);
}

bool GameSupported()
{
    return gResolution.load(std::memory_order_acquire) != nullptr;
}

LibrariesBound BindLibraries(void* staticSymbols)
{
    auto resolution = gResolution.load(std::memory_order_acquire);
    if (!resolution) {
        Log(LogLevel::Error, "StaticSymbols: the engine table was not resolved for this game; nothing is bound");
        return {};
    }

    auto report = BindStaticSymbols(staticSymbols, *resolution);
    BindTemplateBank(*resolution);
    BindNetMessages(*resolution);
    BindStatusVfx(*resolution);
    // A member that leads to a structure this build declares otherwise than
    // the game has it is taken back.
    for (auto const& held : LayoutsFor(*resolution).Members) {
        for (auto& member : report.Members) {
            if (strcmp(member.Row->Member, held.Name) != 0 || !member.Value) continue;
            void* null = nullptr;
            memcpy(static_cast<char*>(staticSymbols) + member.Row->Offset, &null, sizeof(null));
            member.Value = nullptr;
            member.Reason = held.Why;
            report.BoundCount--;
            report.Failed = true;
            report.CriticalFailed = report.CriticalFailed || member.Row->Critical;
        }
    }
    size_t unavailable = 0;
    for (auto const& member : report.Members) {
        auto const& row = *member.Row;
        if (row.How == Bound::Unavailable) {
            unavailable++;
            Log(LogLevel::Debug, "StaticSymbols: %s has no counterpart in this game. %s", row.Member, row.WhenNull);
        } else if (!member.Value && !(row.How == Bound::Handlers && member.Reason.empty())) {
            Log(row.Critical ? LogLevel::Error : LogLevel::Warning, "StaticSymbols: %s is not bound%s: %s. %s", row.Member,
                row.Critical ? " (critical)" : "", member.Reason.c_str(), row.WhenNull);
        }
    }
    Log(LogLevel::Info, "StaticSymbols: %zu of %zu members bound, %zu without a counterpart in this game%s", report.BoundCount,
        report.Members.size(), unavailable, report.CriticalFailed ? "; a critical member is missing" : "");
    return { report.CriticalFailed, report.Failed };
}

TypeIndicesBound BindTypeIndices(void* staticSymbols)
{
    static std::atomic<bool> done{ false };
    auto resolution = gResolution.load(std::memory_order_acquire);
    if (!resolution) {
        Log(LogLevel::Error, "type indices: the engine table was not resolved for this game; no component, system or static data type is known");
        return {};
    }
    if (done.exchange(true)) return {};

    // Never freed: the maps keep the names it holds.
    auto withheld = LayoutsFor(*resolution);
    auto plan = new TypeIndexPlan(PlanTypeIndices(*resolution, BoundTypes(), withheld.Types));
    for (auto const& line : plan->Report) Log(LogLevel::Warning, "type indices: %s", line.c_str());

    auto game = resolution->In.Game.Symbols;
    auto fill = FillStaticSymbolsTypeIndices(staticSymbols, *plan, [game](uint64_t va) -> void* {
        return game && !game->FromFile() ? game->LoadedAt(va) : nullptr;
    });
    for (auto const& line : fill.Report) Log(LogLevel::Error, "type indices: %s", line.c_str());

    size_t unknown = fill.Refused;
    for (auto const& tally : plan->Types) unknown += tally.Unmapped + tally.Ambiguous;
    Log(fill.Contexts == (size_t)TypeContext::Count && !unknown ? LogLevel::Info : LogLevel::Warning,
        "type indices: %zu index variables of %zu contexts bound (%zu of %zu components, %zu of %zu systems, %zu of %zu static data types "
        "by name); %zu left unknown", fill.Types, fill.Contexts,
        plan->Types[(size_t)BoundAs::Component].Mapped, plan->Types[(size_t)BoundAs::Component].Bound,
        plan->Types[(size_t)BoundAs::System].Mapped, plan->Types[(size_t)BoundAs::System].Bound,
        plan->Types[(size_t)BoundAs::StaticData].Mapped, plan->Types[(size_t)BoundAs::StaticData].Bound, unknown);
    return { fill.Contexts, fill.Types, unknown };
}

Preconditions CheckResolution(engine::Resolution const& resolution, LayoutsWithheld const& layouts)
{
    if (auto findings = CheckRows(engine::EngineTable()); !findings.empty()) {
        return { StartRefusal::RowsDisagree, "the binding's rows and the engine table disagree:\n" + findings };
    }
    auto plan = PlanStaticSymbols(resolution, false);
    std::string reason;
    for (auto const& member : plan.Members) {
        auto const& row = *member.Row;
        bool served = member.Value != nullptr || (row.How == Bound::Handlers && member.Reason.empty());
        if (!row.Critical || row.How == Bound::Unavailable) continue;
        if (!served) {
            reason += std::string(row.Member) + ": " + member.Reason + "\n";
            continue;
        }
        // Bound by the table, and taken back by BindLibraries.
        for (auto const& held : layouts.Members) {
            if (strcmp(row.Member, held.Name) == 0) {
                reason += std::string(row.Member) + ": " + held.Why + "\n";
                break;
            }
        }
    }
    if (!reason.empty()) return { StartRefusal::CriticalMember, "critical members of StaticSymbols would stay null:\n" + reason };
    // What his code reads without a test wherever it reaches it.
    for (auto const& why : layouts.Refusing) reason += why + "\n";
    if (!reason.empty()) return { StartRefusal::CriticalMember, "the extender would call virtual functions in slots the game's vtables do not hold them in:\n" + reason };
    return {};
}

std::optional<RefusalNotice> NoticeOf(StartRefusal refusal, std::string const& gameVersion, std::string const& checkedGame, bool selfUpdating)
{
    switch (refusal) {
    case StartRefusal::GameUnreadable:
    case StartRefusal::CriticalMember:
    case StartRefusal::GameLocks: {
        std::string version = gameVersion.empty() ? "" : " (v" + gameVersion + ")";
        bool older = !gameVersion.empty() && !checkedGame.empty() && CompareGameVersions(gameVersion, checkedGame) < 0;
        return RefusalNotice{ "bg3mse",
            "bg3mse doesn't support this game version" + std::string(older ? "" : " yet") + version + ". Starting without it. "
                + UnsupportedGameAdvice(gameVersion, checkedGame, selfUpdating) };
    }
    case StartRefusal::RowsDisagree:
    case StartRefusal::HooksFailed:
        return RefusalNotice{ "bg3mse", "bg3mse failed to start. The game will run without it." };
    default:
        return std::nullopt;
    }
}

char const* Describe(StartRefusal refusal)
{
    switch (refusal) {
    case StartRefusal::None: return "started";
    case StartRefusal::AlreadyStarted: return "the start-up sequence has already run";
    case StartRefusal::RowsDisagree: return "the binding's rows and the engine table disagree";
    case StartRefusal::GameUnreadable: return "the loaded game cannot be read";
    case StartRefusal::CriticalMember: return "a critical member of StaticSymbols would stay null";
    case StartRefusal::GameLocks: return "the game's lock functions are not confirmed";
    case StartRefusal::NotTheGameOrAlreadyRunning: return "not the game's process, or an extender already runs in it";
    case StartRefusal::HooksFailed: return "the hooks could not be put on the game";
    default: return "?";
    }
}

}
