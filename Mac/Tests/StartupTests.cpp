// The part of the Script Extender's start-up that needs no game, run without
// one: Norbyte's code as this build compiles it, linked as libbg3mse.dylib
// links it, in a process that has nothing of the game.
//
// What runs is his own sequence, not a copy of it:
//
//   1. what SetupScriptExtender and ScriptExtender::Initialize do before the
//      game is bound: the extender object and InitStaticSymbols();
//   2. in place of LibraryManager::FindLibraries, which binds the game: the
//      stand-ins below, put where FindLibraries puts the game's functions;
//   3. ScriptExtender::PostStartup(), whole: PostStartupFindLibraries (the
//      global fixed strings, the enumerations), the property maps, the event
//      registry, the type information with every DeferredInitialize and
//      Validate, the Lua module registry, the hooks (every game function is
//      null here, so nothing is hooked), the story layer's static
//      registration, the entity helpers' component mappings and their
//      property maps, and the built-in Lua bundle from the image's resources;
//      (before it, his validator of strings is given strings: it guards
//      every object a script is handed);
//   4. what the client does at the game states Init and LoadMenu, and the
//      server at Init and LoadModule: the entity helpers bound to the world,
//      an extension state, and its Lua state made by LoadExtensionState, with
//      the built-in library scripts loaded and the start-up finished;
//   5. the events of a reset, thrown into both Lua states, a chunk of Lua
//      that reads back what was registered;
//   6. what a game update can keep back: for each shape of
//      Layout/GameShapes.h in turn, whether start-up refuses, or else every
//      Lua function that depends on what it keeps back, called in its state
//      (a gated one must give its error, none may fault);
//   7. the extender's end when the player quits (Mac/Binding/Shutdown.h),
//      what DllMain does for DLL_PROCESS_DETACH, the way the game ends: on a
//      thread of its own that stands for the game thread (App::Terminate),
//      while this one ticks the server as its thread does; both Lua states get
//      the Shutdown event and are closed, the server's on the server's thread.
//
// The stand-ins, each what the game's is for what these steps use of it:
//
//   - the table of fixed strings: an object of Norbyte's GlobalStringTable
//     with the game's index format (Mac/Layout/GameStringTable.h), entries
//     with his header before the text and a reference count in it, and the
//     four functions his fixed strings go through. A string is never freed;
//     a count that goes below zero fails the test. The hash in a header is
//     not the game's (nothing of the start-up reads it);
//   - the allocator: malloc and free, which is what his BG3Alloc and BG3Free
//     use while the game's allocator is not bound. A block is overwritten
//     when it is freed, so that the use of a freed block shows;
//   - the numbering of types: every component, system and static data type
//     his definitions look up by name gets an index of its own in its
//     context, as CoreLibSDM::Init gives each one in the game;
//   - a client and a server that have just been made: zeroed objects of his
//     ecl::EoCClient and esv::EoCServer with an empty entity world and a mod
//     manager without modules. Everything the steps read of them is an empty
//     container or a null pointer.
//
// What fails the test, each with its message: a failed se_assert, Fail(), an
// exception that leaves a step, a fault, a fixed string's count going below
// zero, and every line of the log at the error level (ERR, OsiError and
// LuaError all end there) that is not one of the lines listed under
// kExpectedErrors, which say that there is no game.

#define __STDC_WANT_LIB_EXT1__ 1
#include <stdafx.h>

#include <Extender/ScriptExtender.h>
#include <Extender/Client/ExtensionStateClient.h>
#include <Extender/Server/ExtensionStateServer.h>
#include <Extender/Shared/API.h>
#include <GameDefinitions/Components/All.h>
#include <GameDefinitions/GameState.h>
#include <GameDefinitions/Input.h>
#include <GameDefinitions/Level.h>
#include <GameDefinitions/Symbols.h>
#include <Lua/Shared/LuaTypeValidators.h>
#include <Lua/Debugger/LuaDebugger.h>
#include <Lua/Debugger/LuaDebugMessages.h>

#include <Binding/Layouts.h>
#include <Binding/LuaGateTable.h>
#include <Binding/NetMessages.h>
#include <Binding/HookBridges.h>
#include <Binding/Shutdown.h>
#include <Binding/StaticSymbolBinding.h>
#include <Binding/TypeIndexBinding.h>
#include <Binding/VirtualCalls.h>
#include <GameDefinitions/Character.h>
#include <GameDefinitions/Item.h>
#include <Layout/DeclaredVirtuals.h>
#include <Layout/GameCamera.h>
#include <Layout/GameLockCalls.h>
#include <Layout/VirtualSlots.h>
#include <Layout/GameStringTable.h>
#include <Platform/Log.h>

#include "StartupLuaDebugAdapter.h"
#include "StartupStoryDebugger.h"
#include "StartupStoryQueries.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <dlfcn.h>
#include <clocale>
#include <exception>
#include <filesystem>
#include <fstream>
#include <malloc/malloc.h>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// The build's flavour (BG3MSE_CHANNEL, CMakeLists.txt) is the one Norbyte's
// code here was compiled with: the generated Extender/BuildInfo.h and
// CoreLib/Config.h, not the repository's.
#if !defined(BG3MSE_FLAVOUR_BUILD_INFO) || !defined(BG3MSE_FLAVOUR_CONFIG)
#error "Extender/BuildInfo.h or CoreLib/Config.h is the repository's, not the build's flavour"
#endif
#if defined(SE_IS_DEVELOPER_BUILD) != BG3MSE_EXPECTED_DEVELOPER || defined(SE_RELEASE_ASSERTS) != BG3MSE_EXPECTED_DEVELOPER
#error "the flavour's definitions are not the configured channel's"
#endif
static_assert(std::string_view(SE_BUILD_CHANNEL) == BG3MSE_EXPECTED_CHANNEL);

// The Lua debugger test's loopback client (Mac/Tests/StartupTestsPosix.cpp).
int DebugClientConnect(uint16_t port, int timeoutMs);
bool DebugClientSend(int fd, void const* data, size_t size);
bool DebugClientReceive(int fd, void* data, size_t size);
void DebugClientClose(int fd);
uint16_t DebugClientFreePort();

BEGIN_SE()

// Extender/Shared/dllmain.cpp, as SetupScriptExtender calls it.
void LoadConfig(std::wstring const& configPath, ExtenderConfig& config);

namespace {

// --- what failed -------------------------------------------------------------

// The step that is running: what a fault or an abort names.
char const* volatile gStep = "the image's initializers";
int gFailures = 0;

void Failed(std::string const& what)
{
    fprintf(stderr, "FAIL [%s] %s\n", gStep, what.c_str());
    gFailures++;
}

// --- the log -----------------------------------------------------------------

// Lines the absence of a game produces, from the Mac binding: no other line
// at the error level may appear.
char const* const kExpectedErrors[] = {
#if BG3MSE_EXPECTED_DEVELOPER
    // The client's Menu state on a developer build, which ShowStartupError
    // logs before it shows it (Devel and Nightly only, and only without
    // DeveloperMode); nothing shows it here (no ecl::EoCClient::HandleError).
    "STARTUP ERROR: This is an experimental version of the Script Extender",
#endif
    // PostStartupFindLibraries asks the binding for the game's type indices.
    "type indices: the engine table was not resolved for this game",
    // OsirisWrappers::Initialize asks the binding for the story library, and
    // says so when it has none.
    "Osiris: the engine table was not resolved for this game",
    "OsirisWrappers::Initialize(): story (Osiris) support is off",
};

std::string gLogPartial;
unsigned gWarnings = 0;
bool gVerbose = false;
// While set, a line at the error level is counted and does not fail the
// test: his code logs one where it tests for what a game does not have.
bool gTolerateErrors = false;
unsigned gToleratedErrors = 0;
// Chunks given with --lua, run in each Lua state after the read-back (the
// in-game test mod's offline cases, Mac/Tests/InGame); their lines are shown.
std::vector<std::string> gExtraLua;

// The Lua states' Shutdown events, by the lines their handlers print: the
// client's and the server's.
unsigned gShutdownEvents[2]{};

void LogLine(std::string const& line)
{
    if (line.find("@BG3MSE-SHUTDOWN client") != std::string::npos) gShutdownEvents[0]++;
    if (line.find("@BG3MSE-SHUTDOWN server") != std::string::npos) gShutdownEvents[1]++;
    bool error = line.find("[ERROR]") != std::string::npos;
    if (line.find("[WARN") != std::string::npos) gWarnings++;
    if (gVerbose && !error) fprintf(stderr, "%s\n", line.c_str());
    else if (!gExtraLua.empty() && line.find("@BG3MSE-IGT") != std::string::npos) fprintf(stderr, "%s\n", line.c_str());
    if (!error) return;
    if (gTolerateErrors) {
        gToleratedErrors++;
        return;
    }
    for (auto expected : kExpectedErrors) {
        if (line.find(expected) != std::string::npos) return;
    }
    Failed("error in the log: " + line);
}

int LogWrite(void*, char const* text, int size)
{
    gLogPartial.append(text, (size_t)size);
    size_t end;
    while ((end = gLogPartial.find('\n')) != std::string::npos) {
        LogLine(gLogPartial.substr(0, end));
        gLogPartial.erase(0, end + 1);
    }
    return size;
}

// --- faults, aborts, exceptions ----------------------------------------------

void Write(char const* text)
{
    (void)!write(STDERR_FILENO, text, strlen(text));
}

void OnSignal(int signal)
{
    Write("FAIL [");
    Write(gStep);
    Write("] ");
    switch (signal) {
    case SIGSEGV: Write("fault: SIGSEGV"); break;
    case SIGBUS: Write("fault: SIGBUS"); break;
    case SIGILL: Write("fault: SIGILL"); break;
    case SIGFPE: Write("fault: SIGFPE"); break;
    case SIGTRAP: Write("trap (a failed se_assert stops here: see the lines above)"); break;
    case SIGABRT: Write("abort"); break;
    default: Write("signal"); break;
    }
    Write("\n");
    _exit(3);
}

void OnTerminate()
{
    std::string what = "std::terminate";
    if (auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (std::exception const& e) {
            what += std::string(" with an uncaught exception: ") + e.what();
        } catch (...) {
            what += " with an uncaught exception that is not a std::exception";
        }
    }
    Failed(what);
    _exit(3);
}

template <class Fn>
void Step(char const* name, Fn&& fn)
{
    gStep = name;
    int before = gFailures;
    try {
        fn();
    } catch (std::exception const& e) {
        Failed(std::string("exception: ") + e.what());
    } catch (...) {
        Failed("exception that is not a std::exception");
    }
    printf("%s %s\n", gFailures == before ? "ok  " : "FAIL", name);
    fflush(stdout);
}

// --- the table of fixed strings ----------------------------------------------

#include "StringTableStandIn.h"

// --- the allocator -----------------------------------------------------------

void* Allocate(std::size_t size)
{
    return malloc(size);
}

// What is freed is overwritten first, so that a read of a freed block does not
// find what was there: a pointer read from one faults (the pattern is not an
// address), and the test does not depend on what the allocator leaves behind.
void Free(void* block) noexcept
{
    // memset_s: a plain memset in front of free is removed by the compiler.
    if (block) {
        size_t size = malloc_size(block);
        memset_s(block, size, 0xdd, size);
    }
    free(block);
}

// --- the numbering of types --------------------------------------------------

// One index variable for every type his definitions look up, in the context
// his lookup expects it in, numbered from zero within the context.
void NumberTheTypes(StaticSymbols& symbols)
{
    static int32_t states[(size_t)mac::binding::TypeContext::Count];
    static std::deque<int32_t> variables;
    int32_t next[(size_t)mac::binding::TypeContext::Count]{};

    for (auto const& row : mac::binding::TypeContextRows()) {
        symbols.IndexSymbolToContextMaps.insert(std::make_pair(&states[(size_t)row.Is], row.NorbyteName));
    }
    for (auto const& type : mac::binding::BoundTypes()) {
        auto context = mac::binding::TypeContext::Component;
        switch (type.As) {
        case mac::binding::BoundAs::Component:
            context = type.OneFrame ? mac::binding::TypeContext::OneFrameComponent : mac::binding::TypeContext::Component;
            break;
        case mac::binding::BoundAs::System: context = mac::binding::TypeContext::System; break;
        case mac::binding::BoundAs::StaticData: context = mac::binding::TypeContext::ImmutableData; break;
        default: break;
        }
        variables.push_back(next[(size_t)context]++);
        symbols.IndexSymbolToNameMaps.insert(std::make_pair(&variables.back(), IndexSymbolInfo{ type.EngineClass, &states[(size_t)context] }));
    }
}

// --- a client and a server that have just been made --------------------------

template <class T>
T* Zeroed()
{
    return static_cast<T*>(calloc(1, sizeof(T)));
}

ecl::EoCClient* gClient = nullptr;
esv::EoCServer* gServer = nullptr;

void MakeClientAndServer(StaticSymbols& symbols)
{
    gClient = Zeroed<ecl::EoCClient>();
    gClient->EntityWorld = Zeroed<ecs::EntityWorld>();
    gClient->ModManager = Zeroed<ModManager>();
    symbols.ecl__EoCClient = &gClient;

    gServer = Zeroed<esv::EoCServer>();
    gServer->EntityWorld = Zeroed<ecs::EntityWorld>();
    symbols.esv__EoCServer = &gServer;
}

// --- Lua ---------------------------------------------------------------------

// What a script can read back of the registrations without a game: every
// type's information, every enumeration and bitfield with its labels, and
// the library tables themselves.
char const* const kReadBack = R"lua(
local types = Ext.Types.GetAllTypes()
assert(#types > 1000, "few types: " .. #types)
local kinds = {}
for _, name in ipairs(types) do
    local info = Ext.Types.GetTypeInfo(name)
    assert(info ~= nil, "no type information for " .. name)
    assert(info.TypeName == name, "type " .. name .. " is named " .. tostring(info.TypeName))
    kinds[info.Kind] = (kinds[info.Kind] or 0) + 1
    for member, memberType in pairs(info.Members) do
        assert(memberType.TypeName ~= nil and memberType.TypeName ~= "", name .. "." .. member .. " has an unnamed type")
    end
    if info.Kind == "Array" or info.Kind == "Set" or info.Kind == "Map" then
        assert(info.ElementType ~= nil and info.ElementType.TypeName ~= "", name .. " has no element type")
    end
    if info.Kind == "Map" then
        assert(info.KeyType ~= nil and info.KeyType.TypeName ~= "", name .. " has no key type")
    end
    if info.Kind == "Enumeration" then
        for label, value in pairs(info.EnumValues) do
            assert(type(label) == "string" and math.type(value) == "integer", name .. " has a label that is not text and integer")
        end
    end
end
assert((kinds.Object or 0) > 1000 and (kinds.Enumeration or 0) > 100 and (kinds.Array or 0) > 100, "kinds are missing")

local enums = 0
for name, enum in pairs(Ext.Enums) do
    enums = enums + 1
end
assert(enums > 100, "few enumerations: " .. enums)

local text = Ext.Json.Stringify({ a = 1, b = { true, "x", 2.5 } })
local back = Ext.Json.Parse(text)
assert(back.a == 1 and back.b[1] == true and back.b[2] == "x" and back.b[3] == 2.5, "JSON did not come back")
-- A table that never had a key is written as an array, one whose keys were
-- taken out as an object: Norbyte's rule (a table without a hash part is an
-- array), which is the same in every build.
local emptied = { k = 1 }
emptied.k = nil
local empties = Ext.Json.Stringify({ never = {}, emptied = emptied }, { Beautify = false })
assert(empties == '{"emptied":{},"never":[]}' or empties == '{"never":[],"emptied":{}}', "empty tables were written as " .. empties)
assert(type(Ext.Utils.Version()) == "number")
assert(Ext.IsClient() ~= Ext.IsServer())
return #types
)lua";

template <class TState>
void ReadBack(TState& state, char const* name)
{
    LuaVirtualPin lua(state);
    if (!lua) {
        Failed("no Lua state");
        return;
    }
    auto L = lua->GetState();
    int top = lua_gettop(L);
    auto results = lua->LoadScript(kReadBack, name);
    if (!results) {
        Failed("the chunk that reads the registrations back did not run (the reason is the line above)");
    } else if (*results != 1 || !lua_isinteger(L, -1)) {
        Failed("the chunk that reads the registrations back returned something else than a count");
    } else {
        printf("     %s: %lld types read back\n", name, (long long)lua_tointeger(L, -1));
    }
    lua_settop(L, top);
}

// Members a script written for Windows reads that the macOS game's objects
// do not have (esv::projectile::Projectile's field_8 and field_C,
// ecl::TimelineSceneTrigger's field_A8, esv::Character's Inventory, the
// timeline actor control's IsAdditional and field_23, esv::StatusInvisible's
// InvisiblePosition, rf::Camera's field_0 and field_4; see their headers):
// the names are there, read as 0 (a flag as false, a float as 0.0, a vector
// as three zeros, a handle as the null handle, nil:
// Inventory is read from the character's inventory owner component, and
// there is no entity world here) and take no value. The object is one of the type as its constructor makes
// it, or, for a type that has none (the game makes it), a zeroed one whose
// handles and strings are made null: there is no entity world to look a
// handle up in.
char const* const kMissingMembers = R"lua(
local object, typeName, names, typeNames, handle, serializes = ...
local info = Ext.Types.GetTypeInfo(typeName)
assert(info ~= nil, "no type information for " .. typeName)
local keys = {}
for key in pairs(object) do keys[key] = true end
for i, name in ipairs(names) do
    local isHandle = typeNames[i] == "EntityHandle"
    local isFlag = typeNames[i] == "bool"
    local isFloat = typeNames[i] == "float"
    local isVector = typeNames[i] == "vec3"
    local function Check(what, value)
        if isHandle then
            assert(value == nil, typeName .. "." .. name .. " " .. what .. " " .. tostring(value))
        elseif isFlag then
            assert(value == false, typeName .. "." .. name .. " " .. what .. " " .. tostring(value))
        elseif isFloat then
            assert(value == 0 and math.type(value) == "float", typeName .. "." .. name .. " " .. what .. " " .. tostring(value))
        elseif isVector then
            assert(type(value) == "table" and #value == 3 and value[1] == 0 and value[2] == 0 and value[3] == 0,
                typeName .. "." .. name .. " " .. what .. " " .. tostring(value))
        else
            assert(value == 0 and math.type(value) == "integer", typeName .. "." .. name .. " " .. what .. " " .. tostring(value))
        end
    end
    Check("reads as", object[name])
    assert(keys[name], "iterating " .. typeName .. " does not give " .. name)
    if isHandle then
        assert(handle ~= nil, "no handle to write")
        object[name] = handle
    elseif isFlag then
        object[name] = true
    elseif isFloat then
        object[name] = 1.5
    elseif isVector then
        object[name] = { 1, 2, 3 }
    else
        object[name] = 123
    end
    Check("reads, after it was written, as", object[name])
    local member = info.Members[name]
    assert(member ~= nil and member.TypeName == typeNames[i], typeName .. "." .. name .. " is not a " .. typeNames[i]
        .. " in the type information, but " .. tostring(member and member.TypeName))
end
-- Ext.Types.Serialize writes each as Windows writes the member, with the
-- value it reads; the members the owner accepted as they are it leaves out.
local serialized = Ext.Types.Serialize(object)
for i, name in ipairs(names) do
    if serializes then
        local value = serialized[name]
        assert(value ~= nil or typeNames[i] == "EntityHandle", typeName .. "." .. name .. " is not serialized")
        local isHandle = typeNames[i] == "EntityHandle"
        if isHandle then
            assert(value == nil, typeName .. "." .. name .. " is serialized as " .. tostring(value))
        elseif typeNames[i] == "vec3" then
            assert(type(value) == "table" and value[1] == 0 and value[2] == 0 and value[3] == 0, typeName .. "." .. name .. " is serialized as " .. tostring(value))
        else
            assert(value == object[name], typeName .. "." .. name .. " is serialized as " .. tostring(value))
        end
    else
        assert(serialized[name] == nil, typeName .. "." .. name .. " is serialized")
    end
end
return #names
)lua";

void MakeNull(ecl::TimelineSceneTrigger& t)
{
    t.Entity = EntityHandle{};
    t.TriggerEntity.Handle = EntityHandle{};
    for (auto string : { &t.TriggerType, &t.Level, &t.PreviousLevel }) new (string) FixedString();
}

// esv::StatusInvisible's InvisiblePosition. A status reaches Lua through its
// vtable (the proxy asks the status for its type), so there is none to hand
// over here: the type information names the member as Windows does, and what
// the property reads and writes through gives the zero vector and takes no
// value.
char const* const kMissingPosition = R"lua(
local member = Ext.Types.GetTypeInfo("esv::StatusInvisible").Members.InvisiblePosition
assert(member ~= nil, "esv::StatusInvisible has no InvisiblePosition in the type information")
assert(member.TypeName == "vec3", "InvisiblePosition is a " .. tostring(member.TypeName))
return 1
)lua";

void CheckMissingPosition(esv::ExtensionState& state)
{
    alignas(esv::StatusInvisible) static uint8_t storage[sizeof(esv::StatusInvisible)];
    memset(storage, 0xff, sizeof(storage));
    auto status = reinterpret_cast<esv::StatusInvisible*>(storage);
    status->SetMissingPosition(glm::vec3(1.0f, 2.0f, 3.0f));
    if (status->GetMissingPosition() != glm::vec3(0.0f)) Failed("esv::StatusInvisible's InvisiblePosition does not read as zero");

    LuaVirtualPin lua(state);
    if (!lua) {
        Failed("no Lua state");
        return;
    }
    auto L = lua->GetState();
    int top = lua_gettop(L);
    if (luaL_loadstring(L, kMissingPosition) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
        Failed(std::string("esv::StatusInvisible: ") + lua_tostring(L, -1));
    } else {
        printf("     esv::StatusInvisible: InvisiblePosition read as the zero vector\n");
    }
    lua_settop(L, top);
}

// ls::LevelData's TrackedTemplates: the macOS game keeps an array of
// templates for each name, and a script reads, as on Windows, a map of each
// name to one template (the first), whose entries are the game's templates
// themselves; the arrays are TrackedTemplateLists. A template reaches Lua
// through its vtable, so none is handed over here: the map is read as the
// property reads it, and Lua is given a level data without templates.
char const* const kTrackedTemplates = R"lua(
local level = ...
local map = Ext.Types.GetTypeInfo("LevelData").Members.TrackedTemplates
assert(map ~= nil and map.Kind == "Map", "TrackedTemplates is a " .. tostring(map and map.Kind))
assert(map.KeyType.TypeName == "FixedString" and map.ElementType.TypeName == "GameObjectTemplate",
    "TrackedTemplates maps " .. tostring(map.KeyType.TypeName) .. " to " .. tostring(map.ElementType.TypeName))
local templates = level.TrackedTemplates
assert(type(templates) == "userdata", "TrackedTemplates reads as a " .. type(templates))
local names = 0
for _ in pairs(templates) do names = names + 1 end
assert(names == 0 and templates.Tracked == nil, "a level data without templates has some")
assert(type(level.TrackedTemplateLists) == "userdata", "TrackedTemplateLists reads as a " .. type(level.TrackedTemplateLists))
-- Ext.Types.Serialize writes it, as the member is on Windows.
local serialized = Ext.Types.Serialize(level).TrackedTemplates
assert(type(serialized) == "table" and next(serialized) == nil, "TrackedTemplates is serialized as " .. tostring(serialized))
return 1
)lua";

void CheckTrackedTemplates(esv::ExtensionState& state)
{
    // Kept until the process ends: the proxies outlive this call in the
    // state. A zeroed level data has empty maps; the templates are never
    // read, only told apart.
    alignas(LevelData) static uint8_t storage[2][sizeof(LevelData)];
    static uint8_t templates[2];
    memset(storage, 0, sizeof(storage));
    auto level = reinterpret_cast<LevelData*>(storage[0]);
    auto first = reinterpret_cast<GameObjectTemplate*>(&templates[0]);
    auto second = reinterpret_cast<GameObjectTemplate*>(&templates[1]);
    Array<GameObjectTemplate*> both;
    both.push_back(first);
    both.push_back(second);
    level->TrackedTemplateLists.set(FixedString("Tracked"), both);
    level->TrackedTemplateLists.set(FixedString("Empty"), Array<GameObjectTemplate*>{});
    auto map = level->GetTrackedTemplates();
    auto found = map->try_get(FixedString("Tracked"));
    if (map->size() != 1 || !found || *found != first) Failed("TrackedTemplates does not give the first template of each name that has one");
    auto empty = reinterpret_cast<LevelData*>(storage[1]);
    if (empty->GetTrackedTemplates() == map || map->size() != 1) Failed("two level data share one map of first templates");

    LuaVirtualPin lua(state);
    if (!lua) {
        Failed("no Lua state");
        return;
    }
    auto L = lua->GetState();
    int top = lua_gettop(L);
    if (luaL_loadstring(L, kTrackedTemplates) != LUA_OK) {
        Failed(std::string("the chunk for the level data does not compile: ") + lua_tostring(L, -1));
        lua_settop(L, top);
        return;
    }
    lua::MakeObjectRef(L, empty, lua->GetCurrentLifetime());
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        Failed(std::string("LevelData: ") + lua_tostring(L, -1));
    } else {
        printf("     LevelData: TrackedTemplates read as the first template of each name\n");
    }
    lua_settop(L, top);
}

template <class TState, class TObject>
void CheckMissingMembers(TState& state, char const* typeName, std::vector<char const*> const& names,
    std::vector<char const*> const& typeNames, bool serializes = true)
{
    LuaVirtualPin lua(state);
    if (!lua) {
        Failed("no Lua state");
        return;
    }
    auto L = lua->GetState();
    int top = lua_gettop(L);
    // Kept until the process ends: the proxy outlives this call in the state.
    alignas(TObject) static uint8_t storage[sizeof(TObject)];
    memset(storage, 0, sizeof(storage));
    TObject* object;
    if constexpr (std::is_default_constructible_v<TObject>) {
        object = new (storage) TObject();
    } else {
        object = reinterpret_cast<TObject*>(storage);
        MakeNull(*object);
    }

    if (luaL_loadstring(L, kMissingMembers) != LUA_OK) {
        Failed(std::string("the chunk for ") + typeName + " does not compile: " + lua_tostring(L, -1));
        lua_settop(L, top);
        return;
    }
    lua::MakeObjectRef(L, object, lua->GetCurrentLifetime());
    lua_pushstring(L, typeName);
    lua_createtable(L, (int)names.size(), 0);
    for (size_t i = 0; i < names.size(); i++) {
        lua_pushstring(L, names[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_createtable(L, (int)typeNames.size(), 0);
    for (size_t i = 0; i < typeNames.size(); i++) {
        lua_pushstring(L, typeNames[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    // A handle that is not null, to write: what it names is not looked up.
    lua::push(L, EntityHandle{ 1, 1 });
    lua_pushboolean(L, serializes);
    if (lua_pcall(L, 6, 1, 0) != LUA_OK) {
        Failed(std::string(typeName) + ": " + lua_tostring(L, -1));
    } else if (lua_tointeger(L, -1) != (lua_Integer)names.size()) {
        Failed(std::string(typeName) + ": the chunk did not check every name");
    } else {
        std::string list;
        for (size_t i = 0; i < names.size(); i++) list += (i == 0 ? "" : i + 1 == names.size() ? " and " : ", ") + std::string(names[i]);
        printf("     %s: %s read as 0\n", typeName, list.c_str());
    }
    lua_settop(L, top);
}

// The keys of the input manager's maps (GameDefinitions/Input.h), which the
// macOS game hashes sign-extended: a script sees each as the unsigned integer
// of Windows's declaration, a device's id of -1 as 65535 and a player's index
// of -1 as 255, and finds the entry by either; the type information names the
// keys of both as Windows does (a 16-bit and a 32-bit unsigned integer).
char const* const kInputKeys = R"lua(
local manager = ...
local function Keys(map)
    local keys, list = {}, {}
    for key, value in pairs(map) do
        assert(math.type(key) == "integer", "a key is a " .. type(key))
        assert(value ~= nil, "the key " .. key .. " has no value")
        keys[key] = true
        list[#list + 1] = tostring(key)
    end
    table.sort(list)
    return keys, table.concat(list, ", ")
end
local states, stateList = Keys(manager.InputStates)
assert(states[65535] and states[1] and not states[-1], "the devices' ids read as " .. stateList)
local s = manager.InputStates
assert(s[65535] ~= nil and s[-1] ~= nil and s[1] ~= nil and s[2] == nil, "a device's state is not found by its id")
local players, playerList = Keys(manager.HoldRepeatEvents)
assert(players[255] and players[0] and not players[-1], "the players' indices read as " .. playerList)
local r = manager.HoldRepeatEvents
assert(r[255] ~= nil and r[-1] ~= nil and r[0] ~= nil and r[1] == nil, "a player's repeats are not found by the index")
local members = Ext.Types.GetTypeInfo("input::InputManager").Members
assert(members.InputStates.KeyType.TypeName == "uint16", "the states' keys are " .. members.InputStates.KeyType.TypeName)
assert(members.HoldRepeatEvents.KeyType.TypeName == "uint32", "the repeats' keys are " .. members.HoldRepeatEvents.KeyType.TypeName)
return 4
)lua";

void CheckInputKeys(ecl::ExtensionState& state)
{
    LuaVirtualPin lua(state);
    if (!lua) {
        Failed("no Lua state");
        return;
    }
    auto L = lua->GetState();
    int top = lua_gettop(L);
    // Kept until the process ends, as the game's manager: the proxy outlives
    // this call in the state. The maps are empty when zeroed.
    alignas(input::InputManager) static uint8_t storage[sizeof(input::InputManager)];
    static input::InputValueSet values[2];
    memset(storage, 0, sizeof(storage));
    auto manager = reinterpret_cast<input::InputManager*>(storage);
    manager->InputStates.set(input::SignHashedDeviceId(0xffff), &values[0]);
    manager->InputStates.set(input::SignHashedDeviceId(1), &values[1]);
    manager->HoldRepeatEvents.set(input::SignHashedPlayerIndex(0xff), input::HoldRepeatEvents{});
    manager->HoldRepeatEvents.set(input::SignHashedPlayerIndex(0), input::HoldRepeatEvents{});
    // Where the game's GetInputValue looks a device's id up: its bucket is
    // the id sign-extended (Mac/Layout/GameShapes.cpp, kInputValue).
    if (HashMapHash(input::SignHashedDeviceId(0xffff)) != ~0ull || HashMapHash(input::SignHashedPlayerIndex(0xfe)) != ~1ull) {
        Failed("the input manager's keys are not hashed sign-extended");
    }

    if (luaL_loadstring(L, kInputKeys) != LUA_OK) {
        Failed(std::string("the chunk for the input manager does not compile: ") + lua_tostring(L, -1));
        lua_settop(L, top);
        return;
    }
    lua::MakeObjectRef(L, manager, lua->GetCurrentLifetime());
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        Failed(std::string("input::InputManager: ") + lua_tostring(L, -1));
    } else if (lua_tointeger(L, -1) != 4) {
        Failed("input::InputManager: the chunk did not check every key");
    } else {
        printf("     input::InputManager: the devices' ids and the players' indices read as unsigned integers\n");
    }
    lua_settop(L, top);
}

// rf::CameraController, as the macOS game lays it out (Mac/Layout/GameCamera.h):
// each member a script reads is read from where the game's code keeps it,
// the camera's included; the camera's two names the game does not have read
// as 0.
char const* const kCameraController = R"lua(
local c = ...
assert(c.ViewDirty == true and c.ProjectionDirty == false and c.IsOrthographic == true and c.OffCenter == false,
    "the flags read as " .. tostring(c.ViewDirty) .. ", " .. tostring(c.ProjectionDirty) .. ", " .. tostring(c.IsOrthographic)
    .. ", " .. tostring(c.OffCenter))
assert(c.FOV == 1.5 and c.NearPlane == 0.25 and c.FarPlane == 512 and c.AspectRatio == 2,
    "the field of view, the planes and the aspect ratio read as " .. c.FOV .. ", " .. c.NearPlane .. ", " .. c.FarPlane .. ", " .. c.AspectRatio)
local t = c.WorldTransform
assert(t.RotationQuat[4] == 1 and t.Translate[1] == 1 and t.Translate[3] == 3 and t.Scale[1] == 4 and t.Scale[3] == 6,
    "the world transform does not read as written")
assert(c.LookAt[1] == 7 and c.LookAt[3] == 9 and c.field_58[1] == 10 and c.field_58[3] == 12, "the point looked at or the vector after it")
assert(c.OrthoLeft == -8 and c.OrthoRight == 8 and c.OrthoBottom == -4 and c.OrthoTop == 4, "the orthographic bounds")
assert(c.ID == "bg3mse", "the ID reads as " .. tostring(c.ID))
local camera = c.Camera
assert(camera.ViewMatrix[1] == 13 and camera.ViewMatrix[16] == 14 and camera.InvViewMatrix[1] == 15
    and camera.ProjectionMatrix[1] == 16 and camera.InvProjectionMatrix[16] == 17, "the camera's matrices")
assert(camera.CullingPlanes[1][1] == 18 and camera.CullingPlanes[10][4] == 19, "the camera's planes")
assert(camera.field_0 == 0 and camera.field_4[1] == 0 and camera.field_4[3] == 0, "the camera's missing members")
assert(c.FrustumPoints[1][1] == 20 and c.FrustumPoints[8][3] == 21, "the frustum points")
return 1
)lua";

void CheckCameraController(ecl::ExtensionState& state)
{
    using namespace mac;
    LuaVirtualPin lua(state);
    if (!lua) {
        Failed("no Lua state");
        return;
    }
    auto L = lua->GetState();
    int top = lua_gettop(L);
    // Kept until the process ends, as the game's controller: the proxy
    // outlives this call in the state. Written at the game's offsets, not
    // through the declaration.
    alignas(rf::CameraController) static uint8_t storage[kCameraControllerSize];
    memset(storage, 0, sizeof(storage));
    auto put = [](size_t offset, float value) { memcpy(storage + offset, &value, sizeof(value)); };
    storage[kCameraViewDirty] = 1;
    storage[kCameraOrthographic] = 1;
    put(kCameraFov, 1.5f);
    put(kCameraNearPlane, 0.25f);
    put(kCameraFarPlane, 512.0f);
    put(kCameraAspectRatio, 2.0f);
    put(kCameraWorldTransform + 0xc, 1.0f);
    put(kCameraWorldTransform + kTransformTranslate, 1.0f);
    put(kCameraWorldTransform + kTransformTranslate + 8, 3.0f);
    put(kCameraWorldTransform + kTransformScale, 4.0f);
    put(kCameraWorldTransform + kTransformScale + 8, 6.0f);
    put(kCameraLookAt, 7.0f);
    put(kCameraLookAt + 8, 9.0f);
    put(kCameraVector, 10.0f);
    put(kCameraVector + 8, 12.0f);
    put(kCameraOrtho, -8.0f);
    put(kCameraOrtho + 4, 8.0f);
    put(kCameraOrtho + 8, -4.0f);
    put(kCameraOrtho + 0xc, 4.0f);
    new (storage + kCameraId) FixedString("bg3mse");
    put(kCameraCamera + kCameraView, 13.0f);
    put(kCameraCamera + kCameraView + 0x3c, 14.0f);
    put(kCameraCamera + kCameraInvView, 15.0f);
    put(kCameraCamera + kCameraProjection, 16.0f);
    put(kCameraCamera + kCameraInvProjection + 0x3c, 17.0f);
    put(kCameraCamera + kCameraCullingPlanes, 18.0f);
    put(kCameraCamera + kCameraSize - 4, 19.0f);
    put(kCameraFrustumPoints, 20.0f);
    put(kCameraControllerSize - 4, 21.0f);
    auto controller = reinterpret_cast<rf::CameraController*>(storage);

    if (luaL_loadstring(L, kCameraController) != LUA_OK) {
        Failed(std::string("the chunk for the camera controller does not compile: ") + lua_tostring(L, -1));
        lua_settop(L, top);
        return;
    }
    lua::MakeObjectRef(L, controller, lua->GetCurrentLifetime());
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        Failed(std::string("rf::CameraController: ") + lua_tostring(L, -1));
    } else {
        printf("     rf::CameraController: every member read where the game keeps it\n");
    }
    lua_settop(L, top);
}

// ls::CameraComponent's MasterBehaviorType, one byte in the macOS game
// (Mac/Layout/GameCamera.h): a script reads that byte whatever lies between
// it and the controller, and writes no more than it.
char const* const kCameraComponent = R"lua(
local c = ...
assert(c.MasterBehaviorType == 3, "the type of the master behaviour reads as " .. tostring(c.MasterBehaviorType))
assert(c.AcceptsInput == true and c.Active == false, "the flags read as " .. tostring(c.AcceptsInput) .. ", " .. tostring(c.Active))
c.MasterBehaviorType = 9
return 1
)lua";

void CheckCameraComponent(ecl::ExtensionState& state)
{
    using namespace mac;
    LuaVirtualPin lua(state);
    if (!lua) {
        Failed("no Lua state");
        return;
    }
    auto L = lua->GetState();
    int top = lua_gettop(L);
    // Kept until the process ends, as the controller above. The seven bytes
    // the game leaves as they were are all ones.
    alignas(CameraComponent) static uint8_t storage[kCameraComponentSize];
    memset(storage, 0, sizeof(storage));
    memset(storage + kCameraComponentBehaviorType + 1, 0xff, kCameraComponentController - kCameraComponentBehaviorType - 1);
    storage[kCameraComponentBehaviorType] = 3;
    storage[kCameraComponentAcceptsInput] = 1;

    if (luaL_loadstring(L, kCameraComponent) != LUA_OK) {
        Failed(std::string("the chunk for the camera component does not compile: ") + lua_tostring(L, -1));
        lua_settop(L, top);
        return;
    }
    lua::MakeObjectRef(L, reinterpret_cast<CameraComponent*>(storage), lua->GetCurrentLifetime());
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        Failed(std::string("ls::CameraComponent: ") + lua_tostring(L, -1));
    } else if (storage[kCameraComponentBehaviorType] != 9 || storage[kCameraComponentBehaviorType + 1] != 0xff
        || storage[kCameraComponentController - 1] != 0xff) {
        Failed("ls::CameraComponent: writing the type of the master behaviour did not write its one byte and nothing else");
    } else {
        printf("     ls::CameraComponent: the type of the master behaviour read and written as its one byte\n");
    }
    lua_settop(L, top);
}

// Where this test's image is loaded.
void* ThisImageBase()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void const*>(&ThisImageBase), &info)) return nullptr;
    return info.dli_fbase;
}

// What esv::ScriptExtender::PreUpdate does with the tasks queued for the
// server's thread at the start of each tick (a protected member of
// ThreadedExtenderState, reached the way a derived class may).
struct ServerTick : ThreadedExtenderState
{
    static void RunTasks(ThreadedExtenderState& server)
    {
        (server.*(&ServerTick::RunPendingTasks))();
    }
};

// --- the native API (Extender/Shared/APIExports.h) ------------------------------
//
// What a native module does with the functions libbg3mse.dylib exports, done
// here by calling them: the test is linked with the extender, and the
// functions are his (Extender/Shared/API.inl).

}

extern "C" {
HMODULE SEUpdaterGetModule();
void SEUpdaterNotifyWhenLoaded(bg3api::LoadCompletionCallbackProc* callback, void* userData);
bg3api::APIVersion SEGetAPIVersion();
void SERegisterLuaStateChangeCallback(bg3api::LuaStateChangeCallbackProc* callback, void* userData);
void SEScheduleCallbackOnThread(bg3api::APIContext context, bg3api::ThreadedCallbackProc* callback, void* userData);
lua_State* SEEnterLuaVM(bg3api::APIContext context);
void SEExitLuaVM(lua_State* state);
}

namespace {

struct NativeEvent
{
    bg3api::APIContext Context;
    bg3api::APILuaStateEvent Event;
    lua_State* L;
};
// What the module's listener was told, in order.
std::vector<NativeEvent> gNativeEvents;
int gNativeUserData = 0;

void OnNativeLuaStateEvent(bg3api::APIContext context, bg3api::APILuaStateEvent event, lua_State* L, void* userData)
{
    if (userData != &gNativeUserData) Failed("the listener of the Lua states' events was not given its own data");
    gNativeEvents.push_back({ context, event, L });
}

// Before the extender object exists, as in a process whose game the start-up
// refused: the functions do nothing, and say so by their results.
void CheckNativeApiWithoutExtender()
{
    if (gExtender) return Failed("the extender object exists already");
    if (SEGetAPIVersion() != bg3api::APIVersion::VerCurrent) Failed("SEGetAPIVersion is not the current version");
    SERegisterLuaStateChangeCallback(&OnNativeLuaStateEvent, &gNativeUserData);
    SEScheduleCallbackOnThread(bg3api::APIContext::Server, [](void*) { Failed("a callback ran without an extender"); }, nullptr);
    if (SEEnterLuaVM(bg3api::APIContext::Client) || SEEnterLuaVM(bg3api::APIContext::Server)) Failed("a Lua state was entered without an extender");
    SEExitLuaVM(nullptr);
}

// The extender's module, as a native module is given it: what dlsym takes.
// This test's own image stands for libbg3mse.dylib here (the dylib's own
// module and exports are tested on the dylib, Tests/DylibLoadTests.cpp).
void CheckNativeModule()
{
    void* module = SEUpdaterGetModule();
    if (!module) return Failed("SEUpdaterGetModule gave no module");
    static void* told = nullptr;
    static int data = 0;
    SEUpdaterNotifyWhenLoaded([](HMODULE module, void* userData) { told = userData == &data ? module : nullptr; }, &data);
    if (told != module) Failed("SEUpdaterNotifyWhenLoaded did not call back at once with the module SEUpdaterGetModule gives");
    Dl_info info{};
    void* found = dlsym(module, "SEGetAPIVersion");
    if (found && (!dladdr(found, &info) || info.dli_fbase != ThisImageBase())) Failed("the module leads dlsym to another image");
    if (dlsym(module, "bg3mse_no_such_function")) Failed("the module gives dlsym a function that does not exist");
    // What the module is not: the address of the image's header, which is
    // what an HMODULE is everywhere else in this build.
    if (module == ThisImageBase()) Failed("the module is the image's header, which dlsym does not take");
}

// With both sides' Lua states made: what the listener was told so far, a
// state entered and left, and a callback run on a side's thread.
void CheckNativeApiWithStates()
{
    using bg3api::APIContext;
    using bg3api::APILuaStateEvent;
    auto described = [] {
        std::string text;
        for (auto const& e : gNativeEvents) {
            text += std::string(e.Context == APIContext::Client ? " c" : " s") + std::to_string((unsigned)e.Event) + (e.L ? "+" : "-");
        }
        return text;
    };
    // Each side: a state is loading (with a state) before anything else, a
    // state that is unloading has been loading, and unloaded follows
    // unloading at once, without a state.
    for (auto side : { APIContext::Client, APIContext::Server }) {
        bool loading = false, unloading = false, any = false, reset = false;
        for (auto const& e : gNativeEvents) {
            if (e.Context != side) continue;
            any = true;
            bool ok = true;
            switch (e.Event) {
            case APILuaStateEvent::Loading: ok = e.L != nullptr && !unloading; loading = true; break;
            case APILuaStateEvent::Loaded: ok = e.L != nullptr && loading && !unloading; break;
            case APILuaStateEvent::Unloading: ok = e.L != nullptr && loading && !unloading; unloading = true; break;
            case APILuaStateEvent::Unloaded: ok = e.L == nullptr && unloading; unloading = false; loading = false; reset = true; break;
            }
            if (!ok) return Failed("the Lua states' events came out of order:" + described());
        }
        if (!any || !loading) return Failed("a side's Lua state was made and its listener was not told:" + described());
        // The client's state was reset by the steps before this one.
        if (side == APIContext::Client && !reset) return Failed("the client's reset was not told:" + described());
    }
    if (!gExtender->GetExternalAPI().WasActivated()) Failed("a listener was added and the API does not count as used");

    // Entering the Lua VM, on the side's own thread: the state is held until
    // it is left. (From another side's thread his function logs an error
    // and gives nothing; an error in the log fails this harness.)
    {
        ContextGuardAnyThread context(ContextType::Server);
        auto& state = gExtender->GetServer().GetExtensionState();
        auto before = state.GetEnterCount();
        lua_State* L = SEEnterLuaVM(APIContext::Server);
        if (!L || L != state.GetLua()->GetState()) return Failed("SEEnterLuaVM did not give the server's Lua state");
        if (state.GetEnterCount() != before + 1) Failed("the Lua state is not held while it is entered");
        lua_pushinteger(L, 41);
        lua_pushinteger(L, 1);
        lua_arith(L, LUA_OPADD);
        if (lua_tointeger(L, -1) != 42) Failed("the Lua state that was entered does not compute");
        lua_pop(L, 1);
        SEExitLuaVM(L);
        if (state.GetEnterCount() != before) Failed("the Lua state is still held after it was left");
        SEExitLuaVM(nullptr);
    }

    // A callback for the server's thread runs when that thread takes its
    // tasks, with the data it was given, once.
    static int ran = 0;
    static int data = 0;
    SEScheduleCallbackOnThread(APIContext::Server, [](void* userData) { ran += userData == &data ? 1 : 100; }, &data);
    if (ran != 0) Failed("a callback for the server's thread ran where it was scheduled");
    {
        ContextGuardAnyThread context(ContextType::Server);
        ServerTick::RunTasks(gExtender->GetServer());
        ServerTick::RunTasks(gExtender->GetServer());
    }
    if (ran != 1) Failed("a callback for the server's thread ran " + std::to_string(ran) + " times");
}

// --- the Lua debugger (--lua-debugger PORT) ------------------------------------

// The port of the Lua debugger test, or 0 for none.
uint16_t gLuaDebuggerPort = 0;
// Whether the story debugger test runs (--story-debugger).
bool gStoryDebugger = false;
// Norbyte's debug adapter, built for macOS, for the test that runs it
// against the Lua debugger (--lua-debug-adapter PROGRAM), or null.
char const* gLuaDebugAdapter = nullptr;

// ScriptExtender::Initialize starts the debugger's server into members of
// its own; the test starts it the same way, into the same members (reached
// as an explicit instantiation may name a private member).
template <class Tag, typename Tag::type Member>
struct PrivateMember
{
    friend typename Tag::type MemberOf(Tag) { return Member; }
};
struct DebugInterfaceMember { using type = std::unique_ptr<LuaDebugInterface> ScriptExtender::*; friend type MemberOf(DebugInterfaceMember); };
struct DebugHandlerMember { using type = std::unique_ptr<lua::dbg::DebugMessageHandler> ScriptExtender::*; friend type MemberOf(DebugHandlerMember); };
struct DebugThreadMember { using type = std::thread* ScriptExtender::*; friend type MemberOf(DebugThreadMember); };
struct DebuggerMember { using type = std::unique_ptr<lua::dbg::Debugger> ScriptExtender::*; friend type MemberOf(DebuggerMember); };
template struct PrivateMember<DebugInterfaceMember, &ScriptExtender::luaDebugInterface_>;
template struct PrivateMember<DebugHandlerMember, &ScriptExtender::luaDebugMsgHandler_>;
template struct PrivateMember<DebugThreadMember, &ScriptExtender::luaDebuggerThread_>;
template struct PrivateMember<DebuggerMember, &ScriptExtender::luaDebugger_>;

// ScriptExtender.cpp, Initialize: the server's socket, its message handler,
// the thread that accepts the frontend and reads its messages, the debugger.
// (The debugger before the thread: a frontend that is already waiting on the
// socket is answered as soon as the thread runs, and the answer to its
// connect request goes through the debugger.)
void StartLuaDebugger(uint16_t port)
{
    auto& extender = *gExtender;
    auto& config = extender.GetConfig();
    config.EnableLuaDebugger = true;
    config.LuaDebuggerPort = port;
    auto& intf = extender.*MemberOf(DebugInterfaceMember{});
    intf = std::make_unique<LuaDebugInterface>(port);
    extender.*MemberOf(DebugHandlerMember{}) = std::make_unique<lua::dbg::DebugMessageHandler>(std::ref(*intf));
    auto* raw = intf.get();
    extender.*MemberOf(DebuggerMember{}) = std::make_unique<lua::dbg::Debugger>(std::ref(*(extender.*MemberOf(DebugHandlerMember{}))));
    extender.*MemberOf(DebugThreadMember{}) = new std::thread([raw] { raw->Run(); });
}

// The files a breakpoint can name: the full path of each script the state
// loaded from the game's data (ExtensionStateBase::LuaLoadGameFile).
struct LoadedPaths : ExtensionStateBase
{
    static std::unordered_map<STDString, STDString>& Of(ExtensionStateBase& state)
    {
        return state.*(&LoadedPaths::loadedFileFullPaths_);
    }
};

// The frontend's side of the protocol (LuaDebug.proto, framed as
// Osiris/Debugger/DebugInterface.cpp frames it: a little-endian length that
// counts its own 4 bytes, then the message), as Norbyte's VS Code adapter
// speaks it (LuaDebugger/DAPMessageHandler.cs).
class DebugFrontend
{
public:
    std::vector<std::string> Problems;

    bool Connect(uint16_t port)
    {
        for (int attempt = 0; attempt < 50 && fd_ < 0; attempt++) {
            fd_ = DebugClientConnect(port, 5000);
            if (fd_ < 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (fd_ < 0) Problems.push_back("no connection to 127.0.0.1:" + std::to_string(port));
        return fd_ >= 0;
    }

    void Close()
    {
        DebugClientClose(fd_);
        fd_ = -1;
    }

    // Sends with the next sequence number (1, 2, ...: DebugMessageHandler
    // drops a frontend that skips one); gives that number.
    uint32_t Send(lua::dbg::DebuggerToBackend& msg)
    {
        msg.set_seq_no(++seq_);
        auto body = msg.SerializeAsString();
        uint32_t length = (uint32_t)body.size() + 4;
        uint8_t header[4] = { (uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16), (uint8_t)(length >> 24) };
        if (!DebugClientSend(fd_, header, 4) || !DebugClientSend(fd_, body.data(), body.size())) {
            Problems.push_back("sending message " + std::to_string(seq_) + " failed");
        }
        return seq_;
    }

    // The next message that is not a log line the backend forwards.
    bool Receive(lua::dbg::BackendToDebugger& msg)
    {
        for (;;) {
            uint8_t header[4];
            if (!DebugClientReceive(fd_, header, 4)) {
                Problems.push_back("no message from the backend within 5 s");
                return false;
            }
            uint32_t length = header[0] | (header[1] << 8) | (header[2] << 16) | ((uint32_t)header[3] << 24);
            if (length < 4 || length > 0x10000) {
                Problems.push_back("a message of " + std::to_string(length) + " bytes");
                return false;
            }
            std::string body(length - 4, '\0');
            if (!DebugClientReceive(fd_, body.data(), body.size()) || !msg.ParseFromString(body)) {
                Problems.push_back("a message that does not read");
                return false;
            }
            if (msg.msg_case() != lua::dbg::BackendToDebugger::kDebugOutput) return true;
        }
    }

    // The next message, which must be of that kind (and answer request, if given).
    bool Expect(lua::dbg::BackendToDebugger& msg, lua::dbg::BackendToDebugger::MsgCase kind, char const* what, uint32_t request = 0)
    {
        if (!Receive(msg)) {
            Problems.push_back(std::string("no ") + what);
            return false;
        }
        if (msg.msg_case() != kind) {
            Problems.push_back(std::string("expected ") + what + ", got message kind " + std::to_string((int)msg.msg_case()));
            return false;
        }
        if (request != 0 && msg.reply_seq_no() != request) {
            Problems.push_back(std::string(what) + " answers " + std::to_string(msg.reply_seq_no()) + ", not " + std::to_string(request));
            return false;
        }
        return true;
    }

private:
    int fd_{ -1 };
    uint32_t seq_{ 0 };
};

// The steps the frontend and the Lua thread hand each other.
struct DebugSession
{
    std::mutex Lock;
    std::condition_variable Changed;
    int Stage{ 0 }; // 1: handshake done, 2: breakpoint set, 3: finished (or given up)
    bool Paused{ false };

    void Set(int stage)
    {
        std::lock_guard _(Lock);
        if (stage > Stage) Stage = stage;
        Changed.notify_all();
    }

    bool WaitFor(int stage)
    {
        std::unique_lock lk(Lock);
        return Changed.wait_for(lk, std::chrono::seconds(10), [&] { return Stage >= stage; });
    }
};

constexpr char kDebugChunkName[] = "DebuggerTest.lua";
constexpr char kDebugChunkPath[] = "/bg3mse/DebuggerTest.lua";
constexpr char kDebugChunk[] =
    "local answer = 42\n"
    "local name = \"bg3mse\"\n"
    "Ext.Utils.Print(\"bg3mse debugger test: \" .. name .. \" \" .. answer)\n"
    "return answer\n";

// The frontend: connect and the handshake; a breakpoint on the chunk's third
// line; when it is hit, the frame's variables, an evaluation, and continue.
void RunDebugFrontend(DebugSession& session, DebugFrontend& front)
{
    using namespace lua::dbg;
    BackendToDebugger in;
    auto GiveUp = [&] {
        bool paused;
        {
            std::lock_guard _(session.Lock);
            paused = session.Paused;
        }
        if (paused) {
            DebuggerToBackend resume;
            resume.mutable_continue_()->set_context(SERVER);
            resume.mutable_continue_()->set_action(DbgContinue_Action_CONTINUE);
            front.Send(resume);
        }
        front.Close();
        session.Set(3);
    };

    if (!front.Connect(gLuaDebuggerPort)) return GiveUp();
    DebuggerToBackend connect;
    connect.mutable_connect()->set_protocol_version(DebugMessageHandler::ProtocolVersion);
    front.Send(connect);
    if (!front.Expect(in, BackendToDebugger::kConnectResponse, "the connect response")) return GiveUp();
    if (in.connectresponse().protocol_version() != DebugMessageHandler::ProtocolVersion) {
        front.Problems.push_back("the backend speaks protocol " + std::to_string(in.connectresponse().protocol_version()));
        return GiveUp();
    }
    // modInfoResponse, then a contextUpdated for the server and the client.
    for (auto kind : { BackendToDebugger::kModInfoResponse, BackendToDebugger::kContextUpdated, BackendToDebugger::kContextUpdated }) {
        if (!front.Expect(in, kind, "the handshake's mod and context messages")) return GiveUp();
    }
    if (!front.Expect(in, BackendToDebugger::kDebuggerReady, "debuggerReady")) return GiveUp();
    session.Set(1);

    DebuggerToBackend breakpoints;
    auto bp = breakpoints.mutable_setbreakpoints()->add_breakpoint();
    bp->set_path(kDebugChunkPath);
    bp->set_line(3);
    auto request = front.Send(breakpoints);
    if (!front.Expect(in, BackendToDebugger::kResults, "the result of setBreakpoints", request)) return GiveUp();
    if (in.results().status_code() != SUCCESS) {
        front.Problems.push_back("setBreakpoints gave status " + std::to_string((int)in.results().status_code()));
        return GiveUp();
    }
    session.Set(2);

    if (!front.Expect(in, BackendToDebugger::kBreakpointTriggered, "the breakpoint")) return GiveUp();
    {
        std::lock_guard _(session.Lock);
        session.Paused = true;
    }
    auto const& hit = in.breakpointtriggered();
    if (hit.reason() != BkBreakpointTriggered_Reason_BREAKPOINT || hit.context() != SERVER || hit.stack_size() < 1
        || hit.stack(0).source() != kDebugChunkName || hit.stack(0).line() != 3) {
        front.Problems.push_back("the break is not the breakpoint's: reason " + std::to_string((int)hit.reason()) + ", context "
            + std::to_string((int)hit.context()) + ", " + std::to_string(hit.stack_size()) + " frame(s)"
            + (hit.stack_size() > 0 ? ", " + std::string(hit.stack(0).source()) + ":" + std::to_string(hit.stack(0).line()) : ""));
    }

    // The frame's locals, as the adapter asks for them (Evaluator.cs).
    DebuggerToBackend variables;
    variables.mutable_getvariables()->set_context(SERVER);
    variables.mutable_getvariables()->set_variableref(-1);
    variables.mutable_getvariables()->set_frame(0);
    variables.mutable_getvariables()->set_local(-1);
    request = front.Send(variables);
    if (front.Expect(in, BackendToDebugger::kGetVariablesResponse, "the frame's variables", request)) {
        auto const& response = in.getvariablesresponse();
        bool answer = false, name = false;
        std::string seen;
        for (auto const& v : response.result()) {
            seen += std::string(v.name()) + " ";
            if (v.name() == "answer" && v.value().type_id() == INTEGER && v.value().intval() == 42) answer = true;
            if (v.name() == "name" && v.value().type_id() == STRING && v.value().stringval() == "bg3mse") name = true;
        }
        if (!answer || !name) {
            front.Problems.push_back("the frame's variables lack answer = 42 or name = \"bg3mse\": " + seen
                + std::string(response.error_message()));
        }
    }

    DebuggerToBackend evaluate;
    evaluate.mutable_evaluate()->set_context(SERVER);
    evaluate.mutable_evaluate()->set_expression("answer + 1");
    evaluate.mutable_evaluate()->set_frame(0);
    request = front.Send(evaluate);
    if (front.Expect(in, BackendToDebugger::kEvaluateResponse, "the evaluation", request)) {
        auto const& value = in.evaluateresponse().result();
        if (value.type_id() != INTEGER || value.intval() != 43) {
            front.Problems.push_back("answer + 1 evaluated to type " + std::to_string((int)value.type_id()) + " "
                + std::to_string(value.intval()) + " " + std::string(in.evaluateresponse().error_message()));
        }
    }

    DebuggerToBackend resume;
    resume.mutable_continue_()->set_context(SERVER);
    resume.mutable_continue_()->set_action(DbgContinue_Action_CONTINUE);
    request = front.Send(resume);
    {
        std::lock_guard _(session.Lock);
        session.Paused = false;
    }
    if (front.Expect(in, BackendToDebugger::kResults, "the result of continue", request) && in.results().status_code() != SUCCESS) {
        front.Problems.push_back("continue gave status " + std::to_string((int)in.results().status_code()));
    }
    front.Close();
    session.Set(3);
}

// The Lua thread's side: the debugger's hooks go on at its tick (the game
// calls ServerTick from esv::ScriptExtender::PreUpdate), and the chunk runs
// as a script the state loaded from the game's data.
void RunDebuggerTest()
{
    ContextGuardAnyThread context(ContextType::Server);
    auto& server = gExtender->GetServer();
    auto* debugger = gExtender->GetLuaDebugger();
    if (!server.HasExtensionState() || !debugger) {
        Failed("no server state or no debugger");
        return;
    }
    auto& state = server.GetExtensionState();
    LoadedPaths::Of(state)[kDebugChunkName] = kDebugChunkPath;

    DebugSession session;
    DebugFrontend front;
    std::thread frontend([&] { RunDebugFrontend(session, front); });

    bool ran = false;
    if (session.WaitFor(1)) {
        debugger->ServerTick();
        if (session.WaitFor(2) && session.Stage < 3) {
            debugger->ServerTick();
            LuaVirtualPin lua(state);
            if (lua) {
                auto L = lua->GetState();
                int top = lua_gettop(L);
                auto result = lua->LoadScript(kDebugChunk, kDebugChunkName);
                ran = result.has_value();
                lua_settop(L, top);
            }
        }
    }
    session.WaitFor(3);
    frontend.join();
    // The frontend has gone: the backend turns debugging off at the next tick.
    debugger->ServerTick();
    LoadedPaths::Of(state).erase(kDebugChunkName);

    for (auto const& p : front.Problems) Failed("Lua debugger: " + p);
    if (!ran) Failed("Lua debugger: the chunk did not run");
    if (front.Problems.empty() && ran) {
        printf("     handshake (protocol %u), a breakpoint at %s:3 hit on the server, its locals read, answer + 1 evaluated to 43, continued\n",
            lua::dbg::DebugMessageHandler::ProtocolVersion, kDebugChunkName);
    }
}

// Which way the game ends at the end of the run (--exit).
enum class ExitWay { Quit, QuitIdleServer, Teardown };
ExitWay gExit = ExitWay::Quit;

// A handler of the state's Shutdown event that says it ran.
template <class TState>
void SubscribeToShutdown(TState& state, char const* name)
{
    LuaVirtualPin lua(state);
    if (!lua) {
        Failed("no Lua state");
        return;
    }
    auto L = lua->GetState();
    int top = lua_gettop(L);
    STDString chunk = "Ext.Events.Shutdown:Subscribe(function () Ext.Utils.Print(\"@BG3MSE-SHUTDOWN ";
    chunk += name;
    chunk += "\") end)";
    if (!lua->LoadScript(chunk, "shutdown handler")) Failed(std::string(name) + ": the Shutdown handler could not be subscribed");
    lua_settop(L, top);
}

// Each --lua file in turn, as a chunk of the state; the last one returns the
// number of failures it counted.
template <class TState>
void RunExtraLua(TState& state, char const* name)
{
    LuaVirtualPin lua(state);
    if (!lua) {
        Failed("no Lua state");
        return;
    }
    auto L = lua->GetState();
    for (size_t i = 0; i < gExtraLua.size(); i++) {
        auto const& path = gExtraLua[i];
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            Failed("cannot read " + path);
            return;
        }
        STDString text;
        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, n);
        fclose(f);

        int top = lua_gettop(L);
        auto results = lua->LoadScript(text, STDString(path.c_str()));
        if (!results) {
            Failed(path + " did not run (the reason is the line above)");
        } else if (i + 1 == gExtraLua.size()) {
            if (*results != 1 || !lua_isinteger(L, -1)) {
                Failed(path + " returned something else than a count of failures");
            } else if (lua_tointeger(L, -1) != 0) {
                Failed(std::string(name) + ": " + std::to_string(lua_tointeger(L, -1)) + " failure(s) in " + path);
            }
        }
        lua_settop(L, top);
    }
}

// --- what a game update can take away ----------------------------------------
//
// After a game update a proof of a layout (Layout/GameShapes.h) or of a
// virtual slot can fail, and the binding then keeps back what leads to the
// structure (Binding/Layouts.h). For each shape in turn, the real path decides
// what is kept back with that one shape unconfirmed, and then either start-up
// must refuse (a critical member, a class his code reaches from everywhere,
// a lock shape), or every Lua function that depends on what is kept back is
// called in the state it belongs to: a gated one must give a Lua error that
// says it is unavailable, and the others must not fault.
//
// In this harness every member that points into the game is null, as each
// is when it is kept back; the iteration decides what VirtualsConfirmed and
// the network session answer, and which functions are called.

// Members his code tests itself that a script reaches without a gate: a
// chunk run under pcall in the state of `Role` (0 client, 1 server, 2 both),
// which must not fault.
struct WithoutMember
{
    char const* Member;
    int Role;
    char const* Lua;
};

constexpr WithoutMember kWithoutMembers[] = {
    { "ecl__gCursorControl", 0, "assert(Ext.UI.GetCursorControl() == nil)" },
    { "ls__gDragDropManager", 0, "assert(Ext.UI.GetDragDrop(0) == nil)" },
    { "dlg__DialogManager__PerformDialogLoad", 2, "assert(Ext.StaticData.LoadResource('b5d2ec35-9852-8b1b-aba8-c404cc93ab32', 'Dialog') == nil "
        "and Ext.StaticData.ReleaseResource('b5d2ec35-9852-8b1b-aba8-c404cc93ab32', 'Dialog') == false)" },
    { "ls__gTranslatedStringKeyManager", 2, "assert(Ext.Loca.GetTranslatedStringKey('bg3mse') == nil)" },
    { "eoc__gGuidResourceManager", 2, "Ext.StaticData.GetAll('Background')" },
    { "ls__gTextureAtlasMap", 2, "assert(Ext.StaticData.GetTextureAtlasManager() == nil and Ext.StaticData.GetIconUVs('bg3mse') == nil)" },
};

// Classes his code asks VirtualsConfirmed about where it takes them.
constexpr char const* kAskedClasses[][2] = {
    { "SoundManager", "Ext.Audio's gates (Binding/LuaGateTable.h)" },
    { "phx::PhysicsSceneBase", "GetPhysicsScene asks before it calls (Level.inl)" },
};

struct LuaOutcome
{
    bool Ok{ false };
    std::string Error;
};

template <class TState>
LuaOutcome RunGuarded(TState& state, std::string const& code)
{
    LuaOutcome outcome;
    LuaVirtualPin lua(state);
    if (!lua) {
        outcome.Error = "no Lua state";
        return outcome;
    }
    auto L = lua->GetState();
    int top = lua_gettop(L);
    if (luaL_loadstring(L, code.c_str()) != LUA_OK) {
        outcome.Error = std::string("does not compile: ") + lua_tostring(L, -1);
    } else if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        outcome.Error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(an error that is not text)";
    } else {
        outcome.Ok = true;
    }
    lua_settop(L, top);
    return outcome;
}

// Norbyte's readers of a treasure category and of a treasure table
// (Lua/LuaSerializers.cpp), through Ext.Stats, against a stats object that
// has no entry yet. Update of a category reads each item into one it
// allocates (the vector's reader hands it a null pointer for each), and
// Update of a table whose name is new allocates the table: both were written
// through null. On macOS a new item is listed by one category, which is what
// the game's count in the item says when the category is destroyed.
char const* const kTreasureUpdates = R"lua(
local item = { Name = "Bg3mseItem", Priority = 2, MinAmount = 1, MaxAmount = 3, ActPart = 0, Unique = 0, MinLevel = 4, MaxLevel = 9 }
assert(Ext.Stats.TreasureCategory.GetLegacy("Bg3mseCategory") == nil, "the category is there before it is made")
Ext.Stats.TreasureCategory.Update("Bg3mseCategory", { Category = "Bg3mseCategory", Items = { item } })
local category = Ext.Stats.TreasureCategory.GetLegacy("Bg3mseCategory")
assert(category.Category == "Bg3mseCategory" and #category.Items == 1, "the category made by Update has " .. #category.Items .. " items")
for k, v in pairs(item) do
    assert(category.Items[1][k] == v, "the item's " .. k .. " reads " .. tostring(category.Items[1][k]))
end
-- Written back as it was read, with one item more.
category.Items[2] = { Name = "Bg3mseSecond", ActPart = 1 }
Ext.Stats.TreasureCategory.Update("Bg3mseCategory", category)
local again = Ext.Stats.TreasureCategory.GetLegacy("Bg3mseCategory")
assert(#again.Items == 2 and again.Items[1].Name == "Bg3mseItem" and again.Items[1].MaxLevel == 9, "the first item after the category was written back")
assert(again.Items[2].Name == "Bg3mseSecond" and again.Items[2].ActPart == 1 and again.Items[2].Priority == 1 and again.Items[2].MaxAmount == 1,
    "the second item, and the values an item has when the table gives none")

assert(Ext.Stats.TreasureTable.GetLegacy("Bg3mseTable") == nil, "the table is there before it is made")
Ext.Stats.TreasureTable.Update({ Name = "Bg3mseTable", MinLevel = 1, SubTables = {
    { TotalCount = 1, Categories = { { TreasureCategory = "Bg3mseCategory", Frequency = 3 } }, DropCounts = { { Chance = 1, Amount = 2 } } } } })
local made = Ext.Stats.TreasureTable.GetLegacy("Bg3mseTable")
assert(made ~= nil and made.Name == "Bg3mseTable" and made.MinLevel == 1 and #made.SubTables == 1, "the table made by Update")
local sub = made.SubTables[1]
assert(#sub.Categories == 1 and sub.Categories[1].TreasureCategory == "Bg3mseCategory" and sub.Categories[1].Frequency == 3,
    "the new table's category")
assert(sub.TotalCount == 2 and #sub.DropCounts == 1 and sub.DropCounts[1].Amount == 2, "the new table's drop counts")
-- And once more, now that it exists.
made.MaxLevel = 7
Ext.Stats.TreasureTable.Update(made)
assert(Ext.Stats.TreasureTable.GetLegacy("Bg3mseTable").MaxLevel == 7, "the table written back")
)lua";

void CheckTreasureUpdates(esv::ExtensionState& state)
{
    // Kept until the process ends. Zeroed, a stats object has empty
    // managers; the two the readers use are made as his own class makes
    // them (their functions are virtual), the rarities named as the game's.
    alignas(stats::RPGStats) static uint8_t storage[sizeof(stats::RPGStats)];
    static stats::RPGStats* holder = nullptr;
    memset(storage, 0, sizeof(storage));
    auto made = reinterpret_cast<stats::RPGStats*>(storage);
    new (&made->TreasureCategories) stats::CNamedElementManager<stats::TreasureCategory>();
    new (&made->TreasureTables) stats::CNamedElementManager<stats::TreasureTable>();
    char const* const rarities[] = { "Common", "Unique", "Uncommon", "Rare", "Epic", "Legendary", "Divine" };
    for (size_t i = 0; i < std::size(made->TreasureRarities); i++) new (&made->TreasureRarities[i]) FixedString(rarities[i]);
    holder = made;

    auto& symbols = GetStaticSymbols();
    auto before = symbols.gRPGStats;
    symbols.gRPGStats = &holder;
    auto outcome = RunGuarded(state, kTreasureUpdates);
    if (!outcome.Ok) {
        Failed("treasure updates: " + outcome.Error);
    } else {
        auto category = made->TreasureCategories.GetByName(FixedString("Bg3mseCategory"));
        auto table = made->TreasureTables.GetByName(FixedString("Bg3mseTable"));
        if (!category || category->Items.size() != 2 || !category->Items[0] || !category->Items[1]) {
            Failed("the stats object does not hold the category with its two items");
        } else if (category->Items[0]->Categories != 1 || category->Items[1]->Categories != 1) {
            Failed("an item the reader made is not counted as listed by one category");
        } else if (!table || table->SubTables.size() != 1 || !table->SubTables[0] || table->SubTables[0]->Categories.size() != 1) {
            Failed("the stats object does not hold the table with its subtable");
        } else {
            printf("     a category of two items and a table of one subtable, made and written back through Ext.Stats\n");
        }
    }
    symbols.gRPGStats = before;
}

// The client's ScriptExtender::ResetLuaState while something holds the Lua
// state: the reset is put off until the state is let go
// (ExtensionStateBase::RequestLuaReset), and the callback it left runs then,
// when ResetLuaState has long returned. What the callback keeps must
// therefore be the extension state's pointer itself, not a reference to
// ResetLuaState's local: the stack that held it is written over here first.
__attribute__((noinline)) void AskForClientReset()
{
    gExtender->GetClient().ResetLuaState();
}

__attribute__((noinline)) void WriteOverTheStack()
{
    volatile uint8_t gone[8192];
    for (size_t i = 0; i < sizeof(gone); i++) gone[i] = 0x10;
}

void CheckDeferredClientReset(ecl::ExtensionState& state)
{
    auto before = state.GetLua();
    if (!before) {
        Failed("no Lua state to reset");
        return;
    }
    auto told = RunGuarded(state, "Bg3mseBeforeReset = true");
    if (!told.Ok) Failed("the state before the reset: " + told.Error);
    {
        LuaVirtualPin held(state);
        AskForClientReset();
        WriteOverTheStack();
        // Held, the state is neither reset nor handed out again.
        if (state.GetLua() != before) Failed("the Lua state was reset while it was held");
        if (RunGuarded(state, "return").Ok) Failed("a Lua state that waits for its reset was handed out");
    }
    auto after = RunGuarded(state, "assert(Bg3mseBeforeReset == nil, 'the Lua state was not reset when it was let go')");
    if (!after.Ok) {
        Failed("a reset put off until the state is let go: " + after.Error);
    } else {
        printf("     the reset waited for the state to be let go, and its callback ran in the new state\n");
    }
}

// Ext.Template's root template functions while the game has no template
// manager yet (the pointer to it is bound and null): GetGlobalTemplateBank
// gives null then, and the functions give nil.
template <class TState>
void CheckRootTemplatesWithoutBank(TState& state, char const* name)
{
    static GlobalTemplateManager* none = nullptr;
    auto& symbols = GetStaticSymbols();
    auto before = symbols.ls__GlobalTemplateManager;
    symbols.ls__GlobalTemplateManager = &none;
    auto outcome = RunGuarded(state, R"lua(
assert(Ext.Template.GetRootTemplate("7b5f7b9e-0a3f-4a57-9cb5-0d0d6f2c9a11") == nil, "a root template without a template bank")
assert(Ext.Template.GetAllRootTemplates() == nil, "the root templates without a template bank")
)lua");
    if (!outcome.Ok) {
        Failed(std::string(name) + ": root templates without a template bank: " + outcome.Error);
    } else {
        printf("     %s: GetRootTemplate and GetAllRootTemplates give nil\n", name);
    }
    symbols.ls__GlobalTemplateManager = before;
}

// Runs `code` in the client's state, the server's or both, as `role` says.
template <class Fn>
void InStatesOf(int role, Fn&& fn)
{
    if (role == 0 || role == 2) {
        ContextGuardAnyThread context(ContextType::Client);
        auto& client = gExtender->GetClient();
        if (client.HasExtensionState()) fn(client.GetExtensionState(), "client");
    }
    if (role == 1 || role == 2) {
        ContextGuardAnyThread context(ContextType::Server);
        auto& server = gExtender->GetServer();
        if (server.HasExtensionState()) fn(server.GetExtensionState(), "server");
    }
}

// Calls the function behind a gate: it must give the gate's error.
void CallGate(mac::binding::LuaGate const& gate)
{
    auto name = mac::binding::LuaName(gate);
    InStatesOf(gate.Role, [&](auto& state, char const* side) {
        auto outcome = RunGuarded(state, "return " + name + "()");
        if (outcome.Ok) {
            Failed(name + " in the " + side + "'s state ran although what it reads is missing");
        } else if (outcome.Error.find(" is unavailable on this game: ") == std::string::npos) {
            Failed(name + " in the " + side + "'s state did not give the gate's error: " + outcome.Error);
        }
    });
}


// What his code does without what a shape keeps back, by the C++ paths no
// script reaches through a gate.
void CallWithoutResourceManager()
{
    // Loading a module's virtual textures, which the game's module load does.
    gExtender->GetVirtualTextureHelpers().Load();
}

void CallWithoutTemplateCache()
{
    ContextGuardAnyThread context(ContextType::Server);
    auto character = Zeroed<esv::Character>();
    auto item = Zeroed<esv::Item>();
    if (character->CreateCacheTemplate() != nullptr || item->CreateCacheTemplate() != nullptr) {
        Failed("CreateCacheTemplate made a template without the template cache");
    }
}

void CallWithoutExpressionManager()
{
    ContextGuardAnyThread context(ContextType::Server);
    auto& server = gExtender->GetServer();
    if (!server.HasExtensionState()) return;
    LuaVirtualPin lua(server.GetExtensionState());
    if (!lua) return;
    auto L = lua->GetState();
    int top = lua_gettop(L);
    lua_pushcfunction(L, [](lua_State* L) -> int {
        auto expression = lua::get<StatsExpressionRef>(L, 1);
        (void)expression;
        return 0;
    });
    lua_pushstring(L, "1d4");
    if (lua_pcall(L, 1, 0, 0) == LUA_OK) {
        Failed("a stats expression was made without the expression manager");
    } else if (!strstr(lua_tostring(L, -1), "unavailable on this game")) {
        Failed(std::string("a stats expression without the expression manager: ") + lua_tostring(L, -1));
    }
    lua_settop(L, top);
}

WithoutMember const* WithoutOf(char const* member)
{
    for (auto const& without : kWithoutMembers) {
        if (strcmp(without.Member, member) == 0) return &without;
    }
    return nullptr;
}

// What the binding's rows say his code does when the member is null, for a
// member that is only detoured: nothing is wrapped.
bool OnlyWrapped(mac::binding::MemberRow const& row)
{
    return row.Use == mac::binding::Through::Hook || row.How == mac::binding::Bound::HookBridge || row.How == mac::binding::Bound::Handlers;
}

// What the tables here and the gates name is a member of the rows, and a
// detoured member's row says that nothing is wrapped without it.
void CheckTheTables()
{
    using namespace mac::binding;
    for (auto const& row : StaticSymbolRows()) {
        if (row.How == mac::binding::Bound::Unavailable || row.Critical) continue;
        if (OnlyWrapped(row)) {
            if (!row.WhenNull || strncmp(row.WhenNull, "not wrapped", 11) != 0) {
                Failed(std::string(row.Member) + " is detoured, and its row does not say that nothing is wrapped without it");
            }
        }
    }
    for (auto const& without : kWithoutMembers) {
        bool known = false;
        for (auto const& row : StaticSymbolRows()) known = known || strcmp(row.Member, without.Member) == 0;
        if (!known) Failed(std::string("kWithoutMembers names ") + without.Member + ", which is no member of the rows");
    }
    for (auto const& gate : LuaGates()) {
        for (auto name : gate.Members) {
            if (!name) break;
            auto known = std::any_of(StaticSymbolRows().begin(), StaticSymbolRows().end(),
                [&](MemberRow const& row) { return strcmp(row.Member, name) == 0; });
            if (!known) Failed(LuaName(gate) + "'s gate names " + name + ", which is no member of the rows");
        }
    }
    for (auto const& dependent : VirtualDependents()) {
        if (!dependent.Asked) continue;
        bool listed = false;
        for (auto const& asked : kAskedClasses) listed = listed || strcmp(asked[0], dependent.Class) == 0;
        if (!listed) Failed(std::string("his code asks about ") + dependent.Class + ", and nothing here says where");
    }
}

// What the binding keeps back with `shape` alone unconfirmed, by the path the
// start-up sequence takes (LayoutsWithheldOn), on proofs that all hold but
// those of `shape`.
mac::binding::LayoutsWithheld KeptBackWithout(mac::GameShapes const& all, mac::GameShape shape)
{
    using namespace mac;
    using namespace mac::binding;
    auto shapes = all;
    for (auto& proof : shapes.Proofs) proof.Problem = proof.Shape == shape ? "kept unconfirmed by the test" : "";
    auto withheld = WithheldFor(shapes, LayoutDependents());
    std::vector<VirtualSlotResult> slots;
    for (auto const& declared : DeclaredVirtuals()) slots.push_back({ &declared, (int)declared.Slot, {} });
    RequireResultProofs(slots, shapes);
    RequireMemberShapes(slots, shapes, ClassMemberShapes());
    WithholdFor(withheld, slots, VirtualDependents());
    return withheld;
}

void WithEachShapeKeptBack()
{
    using namespace mac;
    using namespace mac::binding;
    CheckTheTables();

    // The proofs as they are named and grouped, from an image that has none of them.
    GameImage nothing;
    auto all = CheckGameShapes(nothing);
    for (uint8_t n = 0; n < (uint8_t)GameShape::Count; n++) {
        if (std::none_of(all.Proofs.begin(), all.Proofs.end(), [n](FunctionProof const& p) { return p.Shape == (GameShape)n; })) {
            Failed(std::string("the game's ") + Describe((GameShape)n) + " has no proof: it could never be confirmed");
        }
    }

    // With every shape confirmed nothing is kept back.
    auto none = KeptBackWithout(all, GameShape::Count);
    if (!none.Members.empty() || !none.Types.empty() || !none.UnconfirmedClasses.empty()) {
        Failed("something is kept back on a game whose every shape is confirmed: " +
            (none.Members.empty() ? (none.Types.empty() ? none.UnconfirmedClasses[0] : none.Types[0].Why) : none.Members[0].Why));
    }

    auto locks = GameLockShapes();
    size_t refused = 0, called = 0;
    // What start-up does with `withheld` kept back, and, when it starts, every
    // call of what depends on it. `lockShape`: a shape the locks rely on.
    auto callWhatDependsOn = [&](char const* what, LayoutsWithheld const& withheld, bool lockShape, bool network) {
        std::string refusal;
        if (lockShape) refusal = "the game's locks are not confirmed";
        for (auto const& held : withheld.Members) {
            for (auto const& row : StaticSymbolRows()) {
                if (row.Critical && strcmp(row.Member, held.Name) == 0) refusal = std::string("critical member ") + held.Name;
            }
        }
        if (!withheld.Refusing.empty()) refusal = "a class his code reaches from everywhere";
        if (!refusal.empty()) {
            printf("     %s: start-up refuses (%s)\n", what, refusal.c_str());
            refused++;
            return;
        }

        KeepLayoutsOfTheGame(&withheld);
        SetNetworkSessionConfirmedForTest(network);
        size_t gates = 0, probes = 0;
        for (auto const& gate : LuaGates()) {
            bool depends = false;
            for (auto name : gate.Members) {
                if (!name) break;
                for (auto const& held : withheld.Members) depends = depends || strcmp(held.Name, name) == 0;
            }
            if (gate.Class && !VirtualsConfirmed(gate.Class)) {
                for (auto const& why : withheld.UnconfirmedClasses) depends = depends || why.find(gate.Class) != std::string::npos;
            }
            if (!depends) continue;
            CallGate(gate);
            gates++;
        }
        for (auto const& held : withheld.Members) {
            auto without = WithoutOf(held.Name);
            if (!without) continue;
            InStatesOf(without->Role, [&](auto& state, char const*) { RunGuarded(state, without->Lua); });
            probes++;
        }
        for (auto const& held : withheld.Members) {
            if (strcmp(held.Name, "ls__gGlobalResourceManager") == 0) CallWithoutResourceManager(), probes++;
            if (strcmp(held.Name, "esv__CacheTemplateManager") == 0) CallWithoutTemplateCache(), probes++;
        }
        if (!network) {
            InStatesOf(0, [&](auto& state, char const*) { RunGuarded(state, "Ext.Net.PostMessageToServer('bg3mse', '{}')"); });
            InStatesOf(1, [&](auto& state, char const*) { RunGuarded(state, "Ext.Net.BroadcastMessage('bg3mse', '{}')"); });
            probes += 2;
        }
        printf("     %s: %zu member(s), %zu type(s), %zu class(es) kept back; %zu gated function(s) gave their error, %zu other call(s)%s\n",
            what, withheld.Members.size(), withheld.Types.size(), withheld.UnconfirmedClasses.size(), gates, probes,
            gates + probes ? "" : " (nothing for a script to call: what depends on it is a hook, a type his code finds missing, or a test of his own)");
        called += gates + probes;
    };

    for (uint8_t n = 0; n < (uint8_t)GameShape::Count; n++) {
        auto shape = (GameShape)n;
        callWhatDependsOn(Describe(shape), KeptBackWithout(all, shape),
            std::find(locks.begin(), locks.end(), shape) != locks.end(), shape != GameShape::NetworkSession);
    }

    // The virtual slot check: each class whose slots the binding asks about,
    // with its slots alone not where this build calls them.
    std::vector<std::string> classes;
    for (auto const& dependent : VirtualDependents()) {
        if (std::find(classes.begin(), classes.end(), dependent.Class) == classes.end()) classes.push_back(dependent.Class);
    }
    for (auto const& cls : classes) {
        LayoutsWithheld withheld;
        std::vector<VirtualSlotResult> slots;
        for (auto const& declared : DeclaredVirtuals()) {
            slots.push_back({ &declared, (int)declared.Slot, cls == declared.Class ? "kept out of its slot by the test" : "" });
        }
        WithholdFor(withheld, slots, VirtualDependents());
        callWhatDependsOn(("the slots of " + cls).c_str(), withheld, false, true);
    }
    KeepLayoutsOfTheGame(nullptr);
    SetNetworkSessionConfirmedForTest(false);
    printf("     %zu shape(s) and class(es) refuse start-up, %zu calls made without what the others keep back\n", refused, called);

    // What is not kept back on any game, but is null here: the members only
    // the engine table can leave null.
    for (auto const& gate : LuaGates()) CallGate(gate);
    CallWithoutExpressionManager();

    // An open gate calls his function: the input manager bound, and null.
    static void* noInputManager = nullptr;
    auto& symbols = GetStaticSymbols();
    symbols.ls__gInputManager = reinterpret_cast<decltype(symbols.ls__gInputManager)>(&noInputManager);
    InStatesOf(0, [](auto& state, char const*) {
        auto outcome = RunGuarded(state, "assert(Ext.Input.GetInputManager() == nil)");
        if (!outcome.Ok) Failed("an open gate did not call his function: " + outcome.Error);
    });
    symbols.ls__gInputManager = nullptr;

    // The summary, as a game with nothing bound would log it.
    LogWhatIsUnavailable(true);
}

void* ThisImage()
{
    return ThisImageBase();
}

}

int RunStartupTests(bool verbose)
{
    gVerbose = verbose;
    for (int signal : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT }) {
        struct sigaction action{};
        action.sa_handler = &OnSignal;
        sigaction(signal, &action, nullptr);
    }
    std::set_terminate(&OnTerminate);

    FILE* log = funopen(nullptr, nullptr, &LogWrite, nullptr, nullptr);
    setvbuf(log, nullptr, _IOLBF, 0);
    mac::SetLogStream(log);

    Step("the native API without an extender object, as on a game the start-up refused", [] {
        CheckNativeApiWithoutExtender();
    });

    Step("the extender object, as SetupScriptExtender makes it", [] {
        gCoreLibPlatformInterface.ThisModule = reinterpret_cast<HMODULE>(ThisImage());
        gExtender = std::make_unique<ScriptExtender>();
        auto& config = gExtender->GetConfig();
        // What would reach for the game or the machine: the crash reporter's
        // handlers, the achievements patch of the game's code, a console
        // window, the debuggers' sockets.
        config.SendCrashReports = false;
        config.EnableAchievements = false;
        config.CreateConsole = false;
        config.EnableDebugger = false;
        config.EnableLuaDebugger = false;
        // The built-in scripts then also load the test library and the
        // development helpers.
        config.DeveloperMode = true;
        // As SetupScriptExtender leaves it.
        config.DebugFlags = DF_DebugTrace | DF_SuppressInitLog;
        InitStaticSymbols();
    });

    Step("a native module: the extender's module, and a listener of the Lua states' events", [] {
        CheckNativeModule();
        if (gExtender->GetExternalAPI().WasActivated()) Failed("the API counts as used before a module used it");
        SERegisterLuaStateChangeCallback(&OnNativeLuaStateEvent, &gNativeUserData);
        if (!gNativeEvents.empty()) Failed("the listener was told of a Lua state before one was made");
    });

    // The directory a developer points the built-in scripts at
    // (LuaBuiltinResourceDirectory), read from a settings file as
    // SetupScriptExtender reads ScriptExtenderSettings.json, given by a path
    // that is not ASCII. Empty here: every script still comes from the bundle.
    auto builtins = std::filesystem::temp_directory_path() / ("bg3mse_startup_builtins_" + std::to_string(getpid())) / u8"Lua sc\u00e9ripts";
    Step("LuaBuiltinResourceDirectory from the settings file", [&] {
        std::filesystem::create_directories(builtins);
        auto settings = builtins.parent_path() / "ScriptExtenderSettings.json";
        std::ofstream(settings) << "{\"LuaBuiltinResourceDirectory\": \"" << builtins.string() << "\"}";
        auto& config = gExtender->GetConfig();
        LoadConfig(FromStdUTF8(settings.string()), config);
        if (ToStdUTF8(config.LuaBuiltinResourceDirectory) != builtins.string()) {
            Failed("LuaBuiltinResourceDirectory is not the settings file's: " + ToStdUTF8(config.LuaBuiltinResourceDirectory));
        }
    });

    Step("the stand-ins, where FindLibraries puts the game's", [] {
        gStrings.Make();
        auto& sym = GetStaticSymbols();
        sym.ls__GlobalStringTable__MainTable__CreateFromString = &StringCreate;
        sym.ls__FixedString__GetString = &StringGet;
        sym.ls__FixedString__IncRef = &StringIncRef;
        sym.ls__GlobalStringTable__MainTable__DecRef = &StringDecRef;
        sym.ls__gGlobalStringTable = &gStrings.Table;
        NumberTheTypes(sym);

        // The end of LibraryManager::FindLibraries.
        gCoreLibPlatformInterface.Alloc = &Allocate;
        gCoreLibPlatformInterface.Free = &Free;
        gCoreLibPlatformInterface.ls__GlobalStringTable__MainTable__CreateFromString = sym.ls__GlobalStringTable__MainTable__CreateFromString;
        gCoreLibPlatformInterface.ls__FixedString__GetString = sym.ls__FixedString__GetString;
        gCoreLibPlatformInterface.ls__FixedString__IncRef = sym.ls__FixedString__IncRef;
        gCoreLibPlatformInterface.ls__GlobalStringTable__MainTable__DecRef = sym.ls__GlobalStringTable__MainTable__DecRef;
        gCoreLibPlatformInterface.ls__gGlobalStringTable = sym.ls__gGlobalStringTable;

        FixedString first("Cast"), again("Cast");
        if (first.Index != again.Index || strcmp(first.GetString(), "Cast") != 0 || first.GetLength() != 4 || !first.IsValid()
            || first.GetMetadata()->RefCount != 2) {
            Failed("the table of fixed strings does not behave as a table");
        }
    });

    Step("the validator of strings, on strings as they are here", [] {
        // What follows the string is what the validator must not read: it
        // took a size and a capacity from there while it read the string as
        // the MSVC library's.
        struct Holder
        {
            STDString Text;
            uint32_t After[2]{ 0xffffffffu, 0 };
        };
        for (size_t length : { 0, 1, 14, 15, 16, 22, 23, 24, 100, 5000 }) {
            Holder holder;
            holder.Text.assign(length, 'x');
            if (!lua::Validate(&holder.Text, Overload<STDString>{})) {
                Failed("a valid string of " + std::to_string(length) + " characters does not pass lua::Validate");
            }
        }
    });

    Step("ScriptExtender::PostStartup", [] {
        gExtender->PostStartup();
        if (!gExtender->WasInitialized()) Failed("PostStartup did not finish");
        if (gExtender->GetLibraryManager().CriticalInitializationFailed()) Failed("the library manager reports a critical failure");
        if (!GFS.strLevel || strcmp(GFS.strLevel.GetString(), "Level") != 0) Failed("the global fixed strings are not initialized");
        if (TypeInformationRepository::GetInstance().GetAllTypes().size() < 1000) Failed("few types are registered");
        if (!gExtender->GetLuaBuiltinBundle().GetResource("Startup.lua")) Failed("the built-in bundle has no Startup.lua");
    });

    Step("a built-in script from LuaBuiltinResourceDirectory", [&] {
        // A file of the directory is read in place of the bundle's, each
        // time it is asked for; without it, the bundle's is read again.
        auto& bundle = gExtender->GetLuaBuiltinBundle();
        auto packed = bundle.GetResource("Startup.lua");
        auto changed = STDString(packed ? *packed : STDString()) + "\n-- from LuaBuiltinResourceDirectory\n";
        std::ofstream(builtins / "Startup.lua", std::ios::binary) << changed;
        auto read = bundle.GetResource("Startup.lua");
        if (!read || *read != changed) Failed("Startup.lua was not read from LuaBuiltinResourceDirectory");
        std::filesystem::remove(builtins / "Startup.lua");
        read = bundle.GetResource("Startup.lua");
        if (!read || !packed || *read != *packed) Failed("Startup.lua is not the bundle's once the directory has none");
        std::error_code ignored;
        std::filesystem::remove_all(builtins.parent_path(), ignored);
    });

    if (gLuaDebuggerPort != 0 || gLuaDebugAdapter) {
        Step("the Lua debugger's server, as ScriptExtender::Initialize starts it (EnableLuaDebugger)", [] {
            if (gLuaDebuggerPort == 0) gLuaDebuggerPort = DebugClientFreePort();
            if (gLuaDebuggerPort == 0) {
                Failed("no free port");
                return;
            }
            StartLuaDebugger(gLuaDebuggerPort);
        });
    }

    Step("the game makes its client and server", [] {
        MakeClientAndServer(GetStaticSymbols());
    });

    if (gStoryDebugger) {
        // Before the server has a Lua state: the state's story layer makes
        // new wrappers of the node classes when it first hears of a story
        // (OsirisCallbackManager::HookOsiris), and those do not know the
        // debugger that was made for the story just before.
        Step("the story debugger: its server, a story loaded, a frontend's breakpoints, pauses, database and evaluation (EnableDebugger)", [] {
            ContextGuardAnyThread context(ContextType::Server);
            std::string summary;
            for (auto const& problem : mac::test::RunStoryDebuggerTest(summary)) Failed(problem);
            if (!summary.empty()) printf("     %s\n", summary.c_str());
        });
    }

    Step("client: Init and LoadMenu (entity helpers, extension state, Lua state, built-in scripts)", [] {
        ContextGuardAnyThread context(ContextType::Client);
        auto& client = gExtender->GetClient();
        client.GetEntityHelpers().Bind();
        client.ResetExtensionState();
        client.LoadExtensionState(ExtensionStateContext::Game);
        if (!client.HasExtensionState() || !client.GetExtensionState().GetLua()) {
            Failed("the client has no Lua state");
            return;
        }
        ReadBack(client.GetExtensionState(), "client");
    });

    Step("client: names of members the game's objects lack", [] {
        ContextGuardAnyThread context(ContextType::Client);
        auto& client = gExtender->GetClient();
        if (!client.HasExtensionState()) return;
        CheckMissingMembers<ecl::ExtensionState, ecl::TimelineSceneTrigger>(client.GetExtensionState(), "ecl::TimelineSceneTrigger",
            { "field_A8" }, { "uint64" }, false);
        CheckMissingMembers<ecl::ExtensionState, SyncedTimelineActorControlComponent>(client.GetExtensionState(),
            "SyncedTimelineActorControlComponent", { "IsAdditional", "field_23" }, { "bool", "uint8" });
        CheckMissingMembers<ecl::ExtensionState, rf::Camera>(client.GetExtensionState(), "rf::Camera", { "field_0", "field_4" },
            { "float", "vec3" });
    });

    Step("client: a camera controller as the game lays it out", [] {
        ContextGuardAnyThread context(ContextType::Client);
        auto& client = gExtender->GetClient();
        if (!client.HasExtensionState()) return;
        CheckCameraController(client.GetExtensionState());
        CheckCameraComponent(client.GetExtensionState());
    });

    Step("client: the keys of the input manager's maps, as Windows has them", [] {
        ContextGuardAnyThread context(ContextType::Client);
        auto& client = gExtender->GetClient();
        if (!client.HasExtensionState()) return;
        CheckInputKeys(client.GetExtensionState());
    });

    if (!gExtraLua.empty()) {
        Step("client: the --lua chunks", [] {
            ContextGuardAnyThread context(ContextType::Client);
            auto& client = gExtender->GetClient();
            if (client.HasExtensionState()) RunExtraLua(client.GetExtensionState(), "client");
        });
    }

    Step("client: the events of a reset", [] {
        ContextGuardAnyThread context(ContextType::Client);
        auto& client = gExtender->GetClient();
        if (!client.HasExtensionState()) return;
        auto& state = client.GetExtensionState();
        state.OnModuleResume();
        state.OnGameSessionLoading();
        state.OnGameSessionLoaded();
        state.OnResetCompleted();
    });

    Step("client: a Lua reset asked for while the Lua state is held, made when the state is let go", [] {
        ContextGuardAnyThread context(ContextType::Client);
        auto& client = gExtender->GetClient();
        if (!client.HasExtensionState()) return;
        CheckDeferredClientReset(client.GetExtensionState());
    });

    Step("client: Menu, as players have it (the notice of a developer build)", [] {
        ContextGuardAnyThread context(ContextType::Client);
        auto& config = gExtender->GetConfig();
        config.DeveloperMode = false;
        gExtender->GetClient().OnGameStateChanged(ecl::GameState::LoadMenu, ecl::GameState::Menu);
        config.DeveloperMode = true;
    });

    Step("server: Init and LoadModule (entity helpers, extension state, Lua state, built-in scripts)", [] {
        ContextGuardAnyThread context(ContextType::Server);
        auto& server = gExtender->GetServer();
        server.GetEntityHelpers().Bind();
        server.ResetExtensionState();
        server.LoadExtensionState(ExtensionStateContext::Game);
        if (!server.HasExtensionState() || !server.GetExtensionState().GetLua()) {
            Failed("the server has no Lua state");
            return;
        }
        ReadBack(server.GetExtensionState(), "server");
    });

    if (gLuaDebuggerPort != 0 && !gLuaDebugAdapter) {
        Step("server: the Lua debugger: a frontend's handshake, a breakpoint, the frame's variables, continue", [] {
            RunDebuggerTest();
        });
    }
    if (gLuaDebuggerPort != 0 && gLuaDebugAdapter) {
        Step("server: the Lua debugger and its debug adapter: an editor's session from initialize to disconnect", [] {
            ContextGuardAnyThread context(ContextType::Server);
            std::string summary;
            // The session ends one run of the script in an error, which the
            // server's state logs.
            unsigned tolerated = gToleratedErrors;
            gTolerateErrors = true;
            auto problems = mac::test::RunLuaDebugAdapterTest(gLuaDebugAdapter, gLuaDebuggerPort, summary);
            gTolerateErrors = false;
            for (auto const& problem : problems) Failed(problem);
            if (!summary.empty() && gToleratedErrors - tolerated != 1) {
                Failed(std::to_string(gToleratedErrors - tolerated) + " line(s) at the error level during the session, not the script's one");
            }
            if (!summary.empty()) printf("     %s\n", summary.c_str());
        });
    }

    Step("server: a story's user queries called from Lua: one a rule calls, one none calls", [] {
        ContextGuardAnyThread context(ContextType::Server);
        auto& server = gExtender->GetServer();
        if (!server.HasExtensionState()) return;
        esv::LuaServerPin lua(server.GetExtensionState());
        if (!lua) {
            Failed("the server has no Lua state");
            return;
        }
        auto problems = mac::test::RunStoryQueryTest(lua->Osiris(), [&](char const* chunk) {
            auto outcome = RunGuarded(server.GetExtensionState(), chunk);
            return outcome.Ok ? std::string() : outcome.Error;
        });
        for (auto const& problem : problems) Failed(problem);
    });

    Step("server: names of members the game's objects lack", [] {
        ContextGuardAnyThread context(ContextType::Server);
        auto& server = gExtender->GetServer();
        if (!server.HasExtensionState()) return;
        CheckMissingMembers<esv::ExtensionState, esv::projectile::Projectile>(server.GetExtensionState(), "esv::projectile::Projectile",
            { "field_8", "field_C" }, { "uint32", "uint32" }, false);
        CheckMissingPosition(server.GetExtensionState());
        CheckTrackedTemplates(server.GetExtensionState());
    });

    Step("server: a treasure category with items and a new treasure table made by Update; root templates without a template bank", [] {
        ContextGuardAnyThread context(ContextType::Server);
        auto& server = gExtender->GetServer();
        if (!server.HasExtensionState()) return;
        CheckTreasureUpdates(server.GetExtensionState());
        CheckRootTemplatesWithoutBank(server.GetExtensionState(), "server");
    });

    Step("client: root templates without a template bank", [] {
        ContextGuardAnyThread context(ContextType::Client);
        auto& client = gExtender->GetClient();
        if (!client.HasExtensionState()) return;
        CheckRootTemplatesWithoutBank(client.GetExtensionState(), "client");
    });

    Step("server: esv::Character's Inventory, which the game's character lacks, without an entity world", [] {
        ContextGuardAnyThread context(ContextType::Server);
        auto& server = gExtender->GetServer();
        if (!server.HasExtensionState()) return;
        CheckMissingMembers<esv::ExtensionState, esv::Character>(server.GetExtensionState(), "esv::Character",
            { "Inventory" }, { "EntityHandle" });
    });

    Step("both: numbers and JSON in the locale the game sets from the player's language", [] {
        // The game calls setlocale(LC_ALL, "") as it loads its language; a
        // German system gives a decimal comma. Norbyte's code on Windows
        // runs in the "C" locale whatever the system's.
        std::string before = setlocale(LC_ALL, nullptr);
        if (!setlocale(LC_ALL, "de_DE.UTF-8") || strcmp(localeconv()->decimal_point, ",") != 0) {
            Failed("de_DE.UTF-8 gives no decimal comma here: the case shows nothing");
        }
        InStatesOf(2, [](auto& state, char const* side) {
            auto outcome = RunGuarded(state, R"(
                local function same(value, wanted, what)
                    if value ~= wanted then error(what .. " gave " .. tostring(value)) end
                end
                same(tostring(1.5), "1.5", "tostring")
                same(string.format("%.2f", 1.5), "1.50", "string.format")
                same(tonumber("1.5"), 1.5, "tonumber")
                same(1.5 .. "", "1.5", "concatenation")
                local text = Ext.Json.Stringify({ x = 1.5 })
                same(text:find("1.5", 1, true) ~= nil and text:find("1,5", 1, true) == nil, true, "Ext.Json.Stringify " .. text)
                same(Ext.Json.Parse('{"x": 2.25}').x, 2.25, "Ext.Json.Parse")
                -- A number key is written with "%lf" (Json.inl), not by rapidjson.
                local keyed = Ext.Json.Stringify({ [1.5] = 1 })
                same(keyed:find("1.500000", 1, true) ~= nil, true, "Ext.Json.Stringify of a number key " .. keyed)
            )");
            if (!outcome.Ok) Failed(std::string("the ") + side + "'s state under de_DE.UTF-8: " + outcome.Error);
        });
        setlocale(LC_ALL, before.c_str());
    });

    if (!gExtraLua.empty()) {
        Step("server: the --lua chunks", [] {
            ContextGuardAnyThread context(ContextType::Server);
            auto& server = gExtender->GetServer();
            if (server.HasExtensionState()) RunExtraLua(server.GetExtensionState(), "server");
        });
    }

    Step("server: the events of a reset", [] {
        ContextGuardAnyThread context(ContextType::Server);
        auto& server = gExtender->GetServer();
        if (!server.HasExtensionState()) return;
        auto& state = server.GetExtensionState();
        state.OnModuleResume();
        state.OnGameSessionLoading();
        state.OnGameSessionLoaded();
        state.OnResetCompleted();
    });

    Step("a native module: the events of both Lua states, a state entered and left, a callback on the server's thread", [] {
        CheckNativeApiWithStates();
    });

    Step("what a game update can keep back: each shape in turn, and the Lua functions that depend on it", [] {
        gTolerateErrors = true;
        WithEachShapeKeptBack();
        gTolerateErrors = false;
        printf("     %u line(s) at the error level from his tests for what is missing\n", gToleratedErrors);
    });

    Step("handlers of the Lua states' Shutdown event", [] {
        {
            ContextGuardAnyThread context(ContextType::Client);
            auto& client = gExtender->GetClient();
            if (client.HasExtensionState()) SubscribeToShutdown(client.GetExtensionState(), "client");
        }
        {
            ContextGuardAnyThread context(ContextType::Server);
            auto& server = gExtender->GetServer();
            if (server.HasExtensionState()) SubscribeToShutdown(server.GetExtensionState(), "server");
        }
    });

    if (gExit == ExitWay::Teardown) {
        Step("the run loop ends (ls::StaticDataManager::Shutdown; DllMain, DLL_PROCESS_DETACH)", [] {
            mac::binding::ShutdownScriptExtender(mac::binding::GameExit::Teardown);
            if (gExtender) Failed("the extender object is still there");
            if (gShutdownEvents[0] != 1 || gShutdownEvents[1] != 1) {
                Failed("the Shutdown event reached the client " + std::to_string(gShutdownEvents[0]) + " time(s) and the server "
                    + std::to_string(gShutdownEvents[1]) + " time(s), not once each");
            }
            // Once only.
            mac::binding::ShutdownScriptExtender(mac::binding::GameExit::Teardown);
        });
    } else {
        Step(gExit == ExitWay::Quit ? "the player quits while the server ticks (App::Terminate on the game thread; DllMain, DLL_PROCESS_DETACH)"
                                    : "the player quits while the server waits without ticking (App::Terminate on the game thread)", [] {
            // The game thread quits; the server's thread goes on ticking (or
            // waits, as in the main menu), and each tick runs the tasks
            // queued for it, as esv::ScriptExtender::PreUpdate does.
            std::atomic<bool> quit{ false };
            std::thread game([&quit] {
                mac::binding::TakeGameThreadForTest();
                mac::binding::ShutdownScriptExtender(mac::binding::GameExit::Quit);
                quit.store(true, std::memory_order_release);
            });
            unsigned ticks = 0;
            while (!quit.load(std::memory_order_acquire)) {
                if (gExit == ExitWay::Quit) {
                    ContextGuardAnyThread context(ContextType::Server);
                    ServerTick::RunTasks(gExtender->GetServer());
                    ticks++;
                }
                usleep(1000);
            }
            game.join();
            if (gExit == ExitWay::QuitIdleServer) {
                // The server reaches its tick after all: the task finds its part done.
                ContextGuardAnyThread context(ContextType::Server);
                ServerTick::RunTasks(gExtender->GetServer());
            }
            if (!gExtender) {
                Failed("the extender object is gone: other threads of the game still run");
                return;
            }
            auto& client = gExtender->GetClient();
            auto& server = gExtender->GetServer();
            if (client.HasExtensionState() && client.GetExtensionState().GetLua()) Failed("the client's Lua state is still open");
            if (server.HasExtensionState() && server.GetExtensionState().GetLua()) Failed("the server's Lua state is still open");
            if (gShutdownEvents[0] != 1 || gShutdownEvents[1] != 1) {
                Failed("the Shutdown event reached the client " + std::to_string(gShutdownEvents[0]) + " time(s) and the server "
                    + std::to_string(gShutdownEvents[1]) + " time(s), not once each");
            }
            if (gExit == ExitWay::Quit) printf("     the server's thread ticked %u time(s) before the quit was done\n", ticks);
            // Once only, whichever way the game goes on to end.
            mac::binding::ShutdownScriptExtender(mac::binding::GameExit::Teardown);
            if (gShutdownEvents[0] != 1 || gShutdownEvents[1] != 1) Failed("a second exit shut down again");
        });
    }

    gStep = "the end";
    fflush(log);

    printf("%u warning line(s) in the log%s\n", gWarnings, verbose ? "" : " (run with -v to see the log)");
    if (gFailures) {
        printf("%d failure(s)\n", gFailures);
    } else {
        printf("the game-independent start-up ran without a failure\n");
    }
    fflush(stdout);
    // As the game's exit ends: no atexit handler, no static destructor.
    _exit(gFailures ? 1 : 0);
}

END_SE()

// Fail() (CoreLib/Utils.cpp) shows its reason in a message box before it ends
// the process. Here the reason is the test's failure, and nobody is asked
// anything: these two take the place of the shim's alert.
int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT)
{
    fprintf(stderr, "FAIL [%s] message box \"%s\": %s\n", bg3se::gStep, caption ? caption : "", text ? text : "");
    bg3se::gFailures++;
    return IDOK;
}

int MessageBoxW(HWND, LPCWSTR text, LPCWSTR caption, UINT)
{
    fprintf(stderr, "FAIL [%s] message box \"%ls\": %ls\n", bg3se::gStep, caption ? caption : L"", text ? text : L"");
    bg3se::gFailures++;
    return IDOK;
}

// bg3mse_startup_tests [-v] [--exit quit|quit-idle|teardown] [--lua FILE]... [--lua-debugger PORT] [--lua-debug-adapter PROGRAM] [--story-debugger]
int main(int argc, char** argv)
{
    bool verbose = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--lua-debugger") == 0 && i + 1 < argc) {
            bg3se::gLuaDebuggerPort = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--lua-debug-adapter") == 0 && i + 1 < argc) {
            bg3se::gLuaDebugAdapter = argv[++i];
        } else if (strcmp(argv[i], "--story-debugger") == 0) {
            bg3se::gStoryDebugger = true;
        } else if (strcmp(argv[i], "--lua") == 0 && i + 1 < argc) {
            bg3se::gExtraLua.push_back(argv[++i]);
        } else if (strcmp(argv[i], "--exit") == 0 && i + 1 < argc && strcmp(argv[i + 1], "quit") == 0) {
            bg3se::gExit = bg3se::ExitWay::Quit;
            i++;
        } else if (strcmp(argv[i], "--exit") == 0 && i + 1 < argc && strcmp(argv[i + 1], "quit-idle") == 0) {
            bg3se::gExit = bg3se::ExitWay::QuitIdleServer;
            i++;
        } else if (strcmp(argv[i], "--exit") == 0 && i + 1 < argc && strcmp(argv[i + 1], "teardown") == 0) {
            bg3se::gExit = bg3se::ExitWay::Teardown;
            i++;
        } else {
            fprintf(stderr, "usage: %s [-v] [--exit quit|quit-idle|teardown] [--lua FILE]... [--lua-debugger PORT] [--lua-debug-adapter PROGRAM] [--story-debugger]\n", argv[0]);
            return 2;
        }
    }
    return bg3se::RunStartupTests(verbose);
}
