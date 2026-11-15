#include <stdafx.h>
#include <GameDefinitions/Symbols.h>
#include <GameDefinitions/Components/All.h>
#include <Extender/ScriptExtender.h>
#include <Lua/Shared/LuaModule.h>

#include <Binding/Layouts.h>
#include <Binding/LuaGateTable.h>
#include <Binding/NetMessages.h>
#include <Binding/StaticSymbolBinding.h>
#include <Binding/VirtualCalls.h>
#include <Platform/Log.h>

#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

#include <lauxlib.h>
#include <lua.h>

// The Lua functions of Norbyte's libraries that read through what the
// binding can leave null, or ask nothing before they call a class whose
// slots may not be confirmed (Binding/LuaGates.h). Each row names what his
// function reads without a test; a function that tests first is not here.

BEGIN_NS(mac::binding)

namespace {

static_assert((int)lua::ModuleRole::Client == 0 && (int)lua::ModuleRole::Server == 1 && (int)lua::ModuleRole::Both == 2);
constexpr int kClient = 0, kServer = 1, kBoth = 2;

// ClientAudio.inl: GetSoundManager logs and gives null, and every function
// but Stop, PauseAllSounds and ResumeAllSounds calls through what it gave.
#define AUDIO(fn) { kClient, "Audio", #fn, { "ls__gGlobalResourceManager" }, "SoundManager" }
// Level.inl: GetAiGrid, GetPhysicsScene and the pathfinding read the level
// manager of the state's side without a test.
#define CLIENT_LEVEL(fn, ...) { kClient, "Level", #fn, { "ecl__LevelManager", __VA_ARGS__ } }
#define SERVER_LEVEL(fn, ...) { kServer, "Level", #fn, { "esv__LevelManager", __VA_ARGS__ } }
// NoesisGUI's reflection and symbol tables, which every Noesis type and
// name his UI code handles goes through (UI.h, Builtins.inl).
#define NOESIS_TABLES "Noesis__Reflection__LockedByThreadId", "Noesis__Reflection__Lock", "Noesis__Reflection__NumLocks", \
    "Noesis__Reflection__Data", "Noesis__SymbolManager__Lock", "Noesis__SymbolManager__Data"

// ecl::PickingHelperManager, which GetPickingHelper reads without a test.
std::string PickingHelperSystem()
{
    if (!gExtender) return "the extender is not set up";
    auto& helpers = gExtender->GetClient().GetEntityHelpers();
    // GetRawSystem indexes the world's systems without a test of its own.
    auto world = helpers.GetEntityWorld();
    if (!world || world->Systems.Systems.empty()) return "the client's entity world has no systems yet";
    if (!helpers.GetSystem<ecl::PickingHelperManager>()) return "the client's ecl::PickingHelperManager system is not known";
    return {};
}

constexpr LuaGate kGates[] = {
    AUDIO(SetSwitch),
    AUDIO(SetState),
    AUDIO(SetRTPC),
    AUDIO(GetRTPC),
    AUDIO(ResetRTPC),
    AUDIO(PostEvent),
    AUDIO(LoadEvent),
    AUDIO(UnloadEvent),
    AUDIO(PlayExternalSound),
    AUDIO(LoadBank),
    AUDIO(UnloadBank),
    AUDIO(PrepareBank),
    AUDIO(UnprepareBank),

    // ClientInput.inl hands out the manager read through the member.
    { kClient, "Input", "GetInputManager", { "ls__gInputManager" } },

    // Stats.inl looks the prototype up in the manager read through the member.
    { kBoth, "Stats", "GetCachedBoost", { "eoc__BoostPrototypeManager" } },
    { kBoth, "Stats", "GetCachedSpell", { "eoc__SpellPrototypeManager" } },
    { kBoth, "Stats", "GetCachedStatus", { "eoc__StatusPrototypeManager" } },
    { kBoth, "Stats", "GetCachedPassive", { "eoc__PassivePrototypeManager" } },
    { kBoth, "Stats", "GetCachedInterrupt", { "eoc__InterruptPrototypeManager" } },

    // ServerTemplate.inl: the global template bank's manager, and the cache
    // manager read through the member before its test.
    { kServer, "Template", "GetAllRootTemplates", { "ls__GlobalTemplateManager" } },
    { kServer, "Template", "GetRootTemplate", { "ls__GlobalTemplateManager" } },
    { kServer, "Template", "GetTemplate", { "ls__GlobalTemplateManager", "esv__CacheTemplateManager" } },
    { kServer, "Template", "GetAllCacheTemplates", { "esv__CacheTemplateManager" } },
    { kServer, "Template", "GetCacheTemplate", { "esv__CacheTemplateManager" } },

    CLIENT_LEVEL(GetEntitiesOnTile),
    CLIENT_LEVEL(GetTileDebugInfo),
    CLIENT_LEVEL(GetHeightsAt),
    CLIENT_LEVEL(BeginPathfinding),
    CLIENT_LEVEL(BeginPathfindingImmediate),
    CLIENT_LEVEL(FindPath, "eoc__AiGrid__FindPathImmediate"),
    CLIENT_LEVEL(ReleasePath),
    CLIENT_LEVEL(GetPathById),
    CLIENT_LEVEL(GetActivePathfindingRequests),
    CLIENT_LEVEL(RaycastClosest),
    CLIENT_LEVEL(RaycastAny),
    CLIENT_LEVEL(RaycastAll),
    CLIENT_LEVEL(SweepSphereClosest),
    CLIENT_LEVEL(SweepCapsuleClosest),
    CLIENT_LEVEL(SweepBoxClosest),
    CLIENT_LEVEL(SweepSphereAll),
    CLIENT_LEVEL(SweepCapsuleAll),
    CLIENT_LEVEL(SweepBoxAll),
    CLIENT_LEVEL(TestBox),
    CLIENT_LEVEL(TestSphere),
    SERVER_LEVEL(GetEntitiesOnTile),
    SERVER_LEVEL(GetTileDebugInfo),
    SERVER_LEVEL(GetHeightsAt),
    SERVER_LEVEL(BeginPathfinding),
    SERVER_LEVEL(BeginPathfindingImmediate),
    SERVER_LEVEL(FindPath, "eoc__AiGrid__FindPathImmediate"),
    SERVER_LEVEL(ReleasePath),
    SERVER_LEVEL(GetPathById),
    SERVER_LEVEL(GetActivePathfindingRequests),
    SERVER_LEVEL(RaycastClosest),
    SERVER_LEVEL(RaycastAny),
    SERVER_LEVEL(RaycastAll),
    SERVER_LEVEL(SweepSphereClosest),
    SERVER_LEVEL(SweepCapsuleClosest),
    SERVER_LEVEL(SweepBoxClosest),
    SERVER_LEVEL(SweepSphereAll),
    SERVER_LEVEL(SweepCapsuleAll),
    SERVER_LEVEL(SweepBoxAll),
    SERVER_LEVEL(TestBox),
    SERVER_LEVEL(TestSphere),
    // CreateSurfaceAction and ExecuteSurfaceAction test the current level.
    SERVER_LEVEL(GetLevelInfo),
    SERVER_LEVEL(AddActivePersistentLevelTemplate),

    // ClientUI/Module.inl: the game UI and its state machine through the
    // resource manager, and NoesisGUI's tables.
    { kClient, "UI", "GetRoot", { "ls__gGlobalResourceManager", NOESIS_TABLES } },
    { kClient, "UI", "GetStateMachine", { "ls__gGlobalResourceManager", NOESIS_TABLES } },
    { kClient, "UI", "RegisterType", { NOESIS_TABLES } },
    { kClient, "UI", "Instantiate", { NOESIS_TABLES } },
    { kClient, "UI", "Box", { NOESIS_TABLES } },
    { kClient, "UI", "GetPickingHelper", {}, nullptr, &PickingHelperSystem },
    { kClient, "UI", "EnableErrorReporting", { "Noesis__gErrorHandler" } },

    // IO.inl's FindFiles calls the game's globbing through the member.
    { kBoth, "IO", "FindFiles", { "ls__FileSystem__CollectFilesWithGlob" } },
};

constexpr size_t kGateCount = std::size(kGates);

std::array<std::atomic<LuaFunction>, kGateCount> gOriginals{};
std::array<std::atomic<bool>, kGateCount> gLogged{};

MemberRow const* RowOf(char const* member)
{
    for (auto const& row : StaticSymbolRows()) {
        if (strcmp(row.Member, member) == 0) return &row;
    }
    return nullptr;
}

void const* ValueOf(MemberRow const& row)
{
    if (!gStaticSymbols) return nullptr;
    void const* value = nullptr;
    memcpy(&value, reinterpret_cast<char const*>(gStaticSymbols) + row.Offset, sizeof(value));
    return value;
}

// Why the member is null: the layout that kept it back, or the table.
std::string WhyNull(char const* member)
{
    if (auto layouts = LayoutsOfTheGame()) {
        for (auto const& held : layouts->Members) {
            if (strcmp(held.Name, member) == 0) return held.Why;
        }
    }
    return "the engine table gave nothing for it on this game";
}

std::string WhyNotConfirmed(char const* cls)
{
    if (auto layouts = LayoutsOfTheGame()) {
        for (auto const& why : layouts->UnconfirmedClasses) {
            if (why.find(std::string(" of ") + cls + " where") != std::string::npos) return why;
        }
    }
    return std::string("the game's vtables are not confirmed to hold the virtual functions of ") + cls + " where this build calls them";
}

template <size_t I>
int Gated(lua_State* L)
{
    auto const& gate = kGates[I];
    auto why = ShutBecause(gate);
    if (!why.empty()) {
        auto name = LuaName(gate);
        if (!gLogged[I].exchange(true)) {
            Log(LogLevel::Warning, "[Unavailable] %s gives an error on this game: %s", name.c_str(), why.c_str());
        }
        return luaL_error(L, "%s is unavailable on this game: %s", name.c_str(), why.c_str());
    }
    auto original = gOriginals[I].load(std::memory_order_acquire);
    if (!original) return luaL_error(L, "%s: the function behind its gate is not registered", LuaName(gate).c_str());
    return original(L);
}

template <size_t... I>
constexpr std::array<LuaFunction, sizeof...(I)> MakeGates(std::index_sequence<I...>)
{
    return { &Gated<I>... };
}

constexpr auto kGated = MakeGates(std::make_index_sequence<kGateCount>{});

std::once_flag gSummaryOnce;

}

std::span<LuaGate const> LuaGates()
{
    return kGates;
}

std::string LuaName(LuaGate const& gate)
{
    return std::string("Ext.") + gate.Table + "." + gate.Function;
}

namespace {

// What the binding keeps back of what the gate's function reads: the members
// and the class, not what is only missing for now.
std::string KeptBackBecause(LuaGate const& gate)
{
    std::string why;
    for (auto member : gate.Members) {
        if (!member) break;
        auto row = RowOf(member);
        if (!row) {
            why += std::string(why.empty() ? "" : "; ") + member + " is not a member the binding knows";
        } else if (!ValueOf(*row)) {
            why += std::string(why.empty() ? "" : "; ") + member + " is not bound (" + WhyNull(member) + ")";
        }
    }
    if (gate.Class && !VirtualsConfirmed(gate.Class)) {
        why += std::string(why.empty() ? "" : "; ") + WhyNotConfirmed(gate.Class);
    }
    return why;
}

}

std::string ShutBecause(LuaGate const& gate)
{
    auto why = KeptBackBecause(gate);
    if (gate.Other) {
        auto other = gate.Other();
        if (!other.empty()) why += std::string(why.empty() ? "" : "; ") + other;
    }
    return why;
}

LuaFunction GateLuaFunction(char const* table, char const* subTable, char const* function, int role, LuaFunction original)
{
    // The first Lua state's libraries are made once StaticSymbols is bound.
    LogWhatIsUnavailable();
    if (subTable || !table || !function) return original;
    for (size_t i = 0; i < kGateCount; i++) {
        auto const& gate = kGates[i];
        if (gate.Role != role || strcmp(gate.Table, table) != 0 || strcmp(gate.Function, function) != 0) continue;
        LuaFunction none = nullptr;
        if (!gOriginals[i].compare_exchange_strong(none, original) && none != original) {
            Log(LogLevel::Error, "[Unavailable] %s was registered twice with different functions; the gate calls the first",
                LuaName(gate).c_str());
        }
        return kGated[i];
    }
    return original;
}

void LogWhatIsUnavailable(bool again)
{
    bool first = false;
    std::call_once(gSummaryOnce, [&] { first = true; });
    if (!first && !again) return;

    std::string held;
    size_t count = 0;
    for (auto const& row : StaticSymbolRows()) {
        if (row.How == Bound::Unavailable || row.How == Bound::Handlers || ValueOf(row)) continue;
        held += std::string("\n  ") + row.Member + ": " + WhyNull(row.Member) + ". " + (row.WhenNull ? row.WhenNull : "");
        count++;
    }
    if (auto layouts = LayoutsOfTheGame()) {
        for (auto const& type : layouts->Types) {
            held += "\n  " + std::string(type.EngineClass) + ": unknown to the extender: " + type.Why;
            count++;
        }
        for (auto const& why : layouts->UnconfirmedClasses) {
            held += "\n  " + why;
            count++;
        }
    }
    if (!NetworkSessionConfirmed()) {
        held += "\n  the network session: the extender registers, sends and receives no network message (Ext.Net, the mods' "
            "messages between client and server)";
        count++;
    }

    // The functions, by library.
    std::map<std::string, std::string> libraries;
    size_t shut = 0;
    for (auto const& gate : kGates) {
        // What only the moment lacks (a system the world has not made yet)
        // is said when a script asks.
        if (KeptBackBecause(gate).empty()) continue;
        auto& functions = libraries[std::string("Ext.") + gate.Table + (gate.Role == kClient ? " (client)" : gate.Role == kServer ? " (server)" : "")];
        functions +=(functions.empty() ? "" : ", ") + std::string(gate.Function);
        shut++;
    }
    std::string functions;
    for (auto const& [library, names] : libraries) functions += "\n  " + library + ": " + names;

    if (!count && !shut) {
        Log(LogLevel::Info, "[Unavailable] nothing: every part of the extender this build checks is confirmed on this game");
        return;
    }
    Log(LogLevel::Warning, "[Unavailable] on this game the extender works without %zu thing(s) it checks; what depends on them "
        "does not work:%s", count, held.c_str());
    if (shut) {
        Log(LogLevel::Warning, "[Unavailable] %zu Lua function(s) give an error instead of reading what is missing:%s", shut,
            functions.c_str());
    } else {
        Log(LogLevel::Warning, "[Unavailable] no Lua function of the extender's libraries is affected");
    }
}

END_NS()
