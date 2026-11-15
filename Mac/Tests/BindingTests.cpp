// Tests of the binding layer (Mac/Binding). No game needed: the adapters and
// hooks run against stand-ins that record their registers, the rows are
// checked against the engine table as it is written, and the fail-closed
// paths run on the in-memory test binary.
#include <Binding/CallAdapters.h>
#include <Binding/CodePatches.h>
#include <Binding/GameVersion.h>
#include <Binding/HookBridges.h>
#include <Binding/Layouts.h>
#include <Release/GameAdvice.h>
#include <Binding/NetMessages.h>
#include <Binding/OsirisBinding.h>
#include <Binding/Prototype.h>
#include <Binding/StaticSymbolBinding.h>
#include <Binding/StatsFunctions.h>
#include <Binding/Startup.h>
#include <Binding/StatusVfx.h>
#include <Binding/TemplateBank.h>
#include <Binding/TypeIndexBinding.h>
#include <Binding/VirtualCalls.h>
#include <Engine/EngineResolver.h>
#include <Platform/CodeWriter.h>
#include <Platform/FaultObserver.h>
#include <Engine/EngineTable.h>
#include <Engine/Entries/OsirisNames.h>
#include <Engine/Entries/PatchNames.h>
#include <Layout/GameFiles.h>
#include <Layout/GameMembers.h>
#include <Layout/GameStats.h>
#include <Layout/DeclaredVirtuals.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

#include <detours.h>

#include "FakeBinary.h"
#include "StoryContainersNorbyte.h"

using namespace bg3se::mac;
using namespace bg3se::mac::binding;
using engine::Place;
using engine::Returns;

extern "C" {
uint64_t bt_seen[9];        // x0 to x8 as the stand-in received them
uint64_t bt_calls;
uint64_t bt_return[2];      // what the stand-in returns in x0 and x1
void bt_record();
void bt_record_indirect();
void bt_call(void* target, uint64_t const* registers, uint64_t* results);
int bt_host(int);
int bt_callee(int);
int bt_other_callee(int);
extern char bt_host_call[], bt_host_not_a_call[];
int bt_says_one();
int bt_says_one_too();
}

namespace {

int gFailures = 0;
char const* gTest = "";
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; name(); } static void name()

bool Contains(std::string const& text, char const* part) { return text.find(part) != std::string::npos; }
uint64_t U(void const* p) { return reinterpret_cast<uint64_t>(p); }

struct Called
{
    uint64_t X0{ 0 }, X1{ 0 };
};

// Calls `function` with the registers given (x0 first), after pointing the
// stand-in's results at `returns`.
Called Call(void* function, std::initializer_list<uint64_t> registers, uint64_t ret0 = 0, uint64_t ret1 = 0, uint64_t x8 = 0)
{
    uint64_t regs[9]{};
    size_t i = 0;
    for (auto r : registers) regs[i++] = r;
    regs[8] = x8;
    memset(bt_seen, 0xee, sizeof(bt_seen));
    bt_return[0] = ret0;
    bt_return[1] = ret1;
    uint64_t results[2]{};
    bt_call(function, regs, results);
    return { results[0], results[1] };
}

void* Adapter(char const* entry, void* target)
{
    auto adapter = FindCallAdapter(entry);
    CHECK(adapter != nullptr);
    if (!adapter) abort();
    if (adapter->Target) *adapter->Target = target;
    return adapter->Function;
}

// Gives the adapter of `member` a stand-in for one of the game functions it needs.
void Need(char const* member, char const* entry, void* target)
{
    auto adapter = FindCallAdapter(member);
    CHECK(adapter != nullptr);
    if (!adapter) abort();
    bool found = false;
    for (auto const& need : adapter->Needs) {
        if (strcmp(need.Entry, entry) != 0) continue;
        *need.Target = target;
        found = true;
    }
    CHECK(found);
}

GameHook const& Hook(char const* entry)
{
    for (auto const& hook : GameHooks()) {
        if (strcmp(hook.Entry, entry) == 0) return hook;
    }
    printf("no game hook %s\n", entry);
    abort();
}

// --- prototypes ---------------------------------------------------------------

enum class OneByte : uint8_t { A };

TEST(prototype_places_parameters_as_the_platform_does)
{
    // Eight registers, then the stack at each parameter's own size: the
    // layout of the game's StatsFunctorDealDamage::Execute.
    using Execute = void*(void*, void*, void*, void*, void*, bool, void*, int,
        void*, void*, void*, void*, void*, OneByte, int, bool, long long, void*);
    constexpr auto p = kPrototypeOf<Execute>;
    static_assert(p.Known && p.Count == 18 && p.Result == Returns::X0);
    CHECK(p.Args[5].At == Place::X5 && p.Args[5].Bytes == 1 && !p.Args[5].Address);
    CHECK(p.Args[7].At == Place::X7 && p.Args[7].Bytes == 4);
    CHECK(p.Args[8].At == Place::Stack && p.Args[8].StackOffset == 0x00 && p.Args[8].Address);
    CHECK(p.Args[12].StackOffset == 0x20);
    CHECK(p.Args[13].StackOffset == 0x28 && p.Args[13].Bytes == 1);
    CHECK(p.Args[14].StackOffset == 0x2c && p.Args[14].Bytes == 4);
    CHECK(p.Args[15].StackOffset == 0x30 && p.Args[15].Bytes == 1);
    CHECK(p.Args[16].StackOffset == 0x38 && p.Args[16].Bytes == 8 && !p.Args[16].Address);
    CHECK(p.Args[17].StackOffset == 0x40 && p.Args[17].Address);

    static_assert(kPrototypeOf<void()>.Count == 0 && kPrototypeOf<void()>.Result == Returns::Nothing);
    static_assert(kPrototypeOf<bool(int&)>.Result == Returns::W0 && kPrototypeOf<bool(int&)>.Args[0].Address);
    static_assert(kPrototypeOf<GameStringView(uint32_t)>.Result == Returns::X0W1);
    static_assert(kPrototypeOf<NoesisPtrResult(char const*)>.Result == Returns::IndirectX8);
    // What the rules do not cover is not guessed.
    static_assert(!kPrototypeOf<void(float)>.Known);
    static_assert(!kPrototypeOf<void(GameStringView)>.Known);
    static_assert(!kPrototypeOf<double()>.Known);
    static_assert(SamePassing(kPrototypeOf<void(void*, int)>, kPrototypeOf<void(char const&, unsigned)>));
    static_assert(!SamePassing(kPrototypeOf<void(void*, int)>, kPrototypeOf<void(void*, long)>));
    static_assert(!SamePassing(kPrototypeOf<void(void*)>, kPrototypeOf<int(void*)>));
}

TEST(disagreement_names_what_differs)
{
    using namespace engine;
    auto game = Takes({ Pointer(Place::X0, 0, "object"), Value(Place::X1, 4, 1, "index") }, Returns::W0, "bool");
    CHECK(Disagreement(kPrototypeOf<bool(void*, uint32_t)>, game, Through::Call).empty());
    CHECK(Disagreement(kPrototypeOf<bool(void*, uint32_t)>, game, Through::Hook).empty());
    // A wider or narrower value, a missing argument, one too many.
    CHECK(Contains(Disagreement(kPrototypeOf<bool(void*, uint32_t const&)>, game, Through::Call), "4 byte(s)"));
    CHECK(Contains(Disagreement(kPrototypeOf<bool(void*, uint8_t)>, game, Through::Call), "4 byte(s)"));
    CHECK(Contains(Disagreement(kPrototypeOf<bool(void*)>, game, Through::Call), "does not pass"));
    CHECK(Contains(Disagreement(kPrototypeOf<bool(void*, uint32_t, int)>, game, Through::Call), "does not take"));
    // Eight bytes of a value where the function takes a pointer, and a
    // pointer where it takes eight bytes of a value: the same register and
    // width, and not the same thing. A hook that only hands the word on may
    // be allowed the first; nothing is allowed the second, and no call either.
    auto wide = Takes({ Pointer(Place::X0, 0, "object"), Value(Place::X1, 8, 1, "handle") }, Returns::W0, "bool");
    CHECK(Disagreement(kPrototypeOf<bool(void*, uint64_t)>, wide, Through::Call).empty());
    CHECK(Contains(Disagreement(kPrototypeOf<bool(void*, uint64_t*)>, wide, Through::Call), "takes a value there, this prototype passes a pointer"));
    CHECK(Contains(Disagreement(kPrototypeOf<bool(uint64_t, uint64_t)>, wide, Through::Call), "x0: the game function takes a pointer there"));
    CHECK(Contains(Disagreement(kPrototypeOf<bool(uint64_t, uint64_t)>, wide, Through::Hook), "takes a pointer there, this prototype passes a value"));
    CHECK(Disagreement(kPrototypeOf<bool(uint64_t, uint64_t)>, wide, Through::Hook, kPointersAsWords).empty());
    CHECK(Contains(Disagreement(kPrototypeOf<bool(uint64_t, uint64_t)>, wide, Through::Call, kPointersAsWords), "passes a value"));
    CHECK(Contains(Disagreement(kPrototypeOf<bool(void*, uint64_t*)>, wide, Through::Hook, kPointersAsWords), "passes a pointer"));
    // Parameters after the last one the function takes, for a hook that is
    // allowed them; never for a call, on the stack, or before one it takes.
    auto one = Takes({ Pointer(Place::X0, 0, "object") });
    using FiveParameters = void(void*, void*, void*, void*, void*);
    CHECK(Contains(Disagreement(kPrototypeOf<FiveParameters>, one, Through::Hook), "does not take"));
    CHECK(Disagreement(kPrototypeOf<FiveParameters>, one, Through::Hook, kParametersNotTaken).empty());
    CHECK(Contains(Disagreement(kPrototypeOf<FiveParameters>, one, Through::Call, kParametersNotTaken), "does not take"));
    using TenParameters = void(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);
    CHECK(Contains(Disagreement(kPrototypeOf<TenParameters>, one, Through::Hook, kParametersNotTaken), "[sp+0x0]"));
    auto second = Takes({ Unread(Place::X0, 0, "object"), Pointer(Place::X2, 2, "third") });
    CHECK(Contains(Disagreement(kPrototypeOf<void(void*, void*, void*)>, second, Through::Hook, kParametersNotTaken), "before one it takes"));
    // A result the caller may drop, and a hook may not.
    CHECK(Disagreement(kPrototypeOf<void(void*, uint32_t)>, game, Through::Call).empty());
    CHECK(Contains(Disagreement(kPrototypeOf<void(void*, uint32_t)>, game, Through::Hook), "would drop"));
    CHECK(Contains(Disagreement(kPrototypeOf<void*(void*, uint32_t)>, game, Through::Call), "expects its result"));

    // Norbyte's parameters in other places than the game takes them.
    auto moved = Takes({ Pointer(Place::X0, 1, "second"), Pointer(Place::X1, 0, "first") });
    CHECK(Contains(Disagreement(kPrototypeOf<void(void*, void*)>, moved, Through::Call), "Norbyte's parameter 2"));
    CHECK(Disagreement(kPrototypeOf<void(void*, void*)>, moved, Through::Call, kOwnPrototype).empty());
    auto extra = Takes({ Pointer(Place::X0, 0, "object"), Pointer(Place::X1, -1, "something more") });
    CHECK(Contains(Disagreement(kPrototypeOf<void(void*, void*)>, extra, Through::Hook), "no parameter for"));

    // No result where Norbyte declares one: only for a hook, and only when allowed.
    auto nothing = Takes({ Pointer(Place::X0, 0, "object") });
    CHECK(Contains(Disagreement(kPrototypeOf<bool(void*)>, nothing, Through::Hook), "produces none"));
    CHECK(Disagreement(kPrototypeOf<bool(void*)>, nothing, Through::Hook, kResultNotProduced).empty());
    CHECK(Contains(Disagreement(kPrototypeOf<bool(void*)>, nothing, Through::Call, kResultNotProduced), "produces none"));

    // A result in memory whose address is the first argument is Norbyte's first parameter.
    auto indirect = Takes({ Pointer(Place::X1, 1, "list") }, Returns::IndirectX0, "result");
    CHECK(Disagreement(kPrototypeOf<void(void*, void*)>, indirect, Through::Hook).empty());
    CHECK(Contains(Disagreement(kPrototypeOf<void(void*)>, indirect, Through::Hook), "x1"));
    // A result through x8 needs a prototype that returns that way.
    auto x8 = Takes({ Pointer(Place::X0, 0, "path") }, Returns::IndirectX8, "Ptr");
    CHECK(Disagreement(kPrototypeOf<NoesisPtrResult(char const*)>, x8, Through::Call).empty());
    CHECK(!Disagreement(kPrototypeOf<void(char const*)>, x8, Through::Call).empty());
    CHECK(!Disagreement(kPrototypeOf<void*(char const*)>, x8, Through::Call).empty());

    // What a function is passed and never reads may be passed or not.
    auto unread = Takes({ Pointer(Place::X0, 0, "object"), Unread(Place::X1, 1, "never read") });
    CHECK(Disagreement(kPrototypeOf<void(void*)>, unread, Through::Call).empty());
    CHECK(Disagreement(kPrototypeOf<void(void*, bool)>, unread, Through::Call).empty());

    // The stack block, at each argument's own offset.
    auto stack = Takes({ Pointer(Place::X0, 0, ""), Pointer(Place::X1, 1, ""), Pointer(Place::X2, 2, ""), Pointer(Place::X3, 3, ""),
        Pointer(Place::X4, 4, ""), Pointer(Place::X5, 5, ""), Pointer(Place::X6, 6, ""), Pointer(Place::X7, 7, ""),
        StackValue(0, 1, 8, "byte"), StackValue(4, 4, 9, "int") });
    using OnStack = void(void*, void*, void*, void*, void*, void*, void*, void*, bool, int);
    using InSlots = void(void*, void*, void*, void*, void*, void*, void*, void*, long, long);
    CHECK(Disagreement(kPrototypeOf<OnStack>, stack, Through::Call).empty());
    CHECK(Contains(Disagreement(kPrototypeOf<InSlots>, stack, Through::Call), "[sp+0x0]"));
}

// --- the rows -----------------------------------------------------------------

TEST(rows_hold_together_with_the_engine_table)
{
    auto findings = CheckRows(engine::EngineTable());
    if (!findings.empty()) printf("%s", findings.c_str());
    CHECK(findings.empty());

    // Every row's member once, every adapter and bridge a function of its own.
    std::set<std::string> members;
    for (auto const& row : StaticSymbolRows()) CHECK(members.insert(row.Member).second);
    std::set<void*> functions;
    for (auto const& a : CallAdapters()) CHECK(functions.insert(a.Function).second);
    for (auto const& b : HookBridges()) CHECK(functions.insert(b.Bridge).second);
    for (auto const& h : GameHooks()) CHECK(functions.insert(h.Detour).second);
    CHECK(StaticSymbolRows().size() * sizeof(void*) < StaticSymbolsSize());
}

// --- call adapters -------------------------------------------------------------

TEST(adapter_fixed_string_get_string)
{
    auto f = Adapter("ls__FixedString__GetString", reinterpret_cast<void*>(&bt_record));
    uint32_t index = 0x00345678;
    GameStringView out{ nullptr, 0 };
    char const* text = "Cast";
    bt_calls = 0;
    auto r = Call(f, { U(&index), U(&out) }, U(text), 0xaaaaaaaa00000004ull);
    CHECK(bt_calls == 1);
    CHECK(static_cast<uint32_t>(bt_seen[0]) == index);      // the index, by value
    CHECK(out.Text == text && out.Length == 4);             // the length is w1: the upper half of x1 is not part of it
    CHECK(r.X0 == U(&out));
}

TEST(adapter_main_table_create_from_string)
{
    auto f = Adapter("ls__GlobalStringTable__MainTable__CreateFromString", reinterpret_cast<void*>(&bt_record));
    GameStringView view{ "Boosts", 6 };
    uint32_t out = 0;
    int table = 0;
    bt_calls = 0;
    auto r = Call(f, { U(&table), U(&out), U(&view) }, 0xbbbbbbbb00001234ull);
    CHECK(bt_calls == 1 && bt_seen[0] == U(&view));         // the view is the one argument
    CHECK(out == 0x1234 && r.X0 == U(&out));
    // No text: the game function is not called, as the game's callers do not call it.
    GameStringView empty{ "", 0 };
    out = 0;
    Call(f, { U(&table), U(&out), U(&empty) }, 7);
    CHECK(bt_calls == 1 && out == 0xffffffffu);
    out = 0;
    Call(f, { U(&table), U(&out), 0 }, 7);
    CHECK(bt_calls == 1 && out == 0xffffffffu);
}

TEST(adapter_surface_action_factory_create_action)
{
    auto f = Adapter("esv__SurfaceActionFactory__DoCreateAction", reinterpret_cast<void*>(&bt_record));
    int factory = 0, action = 0;
    bt_calls = 0;
    auto r = Call(f, { U(&factory), 3, 0xffc0000000000000ull }, U(&action));
    CHECK(bt_calls == 1);
    CHECK(bt_seen[0] == U(&factory) && static_cast<uint32_t>(bt_seen[1]) == 3);
    CHECK(static_cast<uint32_t>(bt_seen[2]) == 0 && bt_seen[3] == 0);      // no story action, no class descriptions
    CHECK(bt_seen[4] == 0xffc0000000000000ull);
    CHECK(r.X0 == U(&action));
}

TEST(adapter_ai_grid_find_path_immediate)
{
    auto f = Adapter("eoc__AiGrid__FindPathImmediate", reinterpret_cast<void*>(&bt_record));
    // A grid with four buckets and two paths, ids 6 and -3, laid out as the game's.
    struct Node { Node* Next; int32_t Id; int32_t Pad; void* Path; };
    char path[0x280]{};
    Node second{ nullptr, 6, 0, path };
    Node first{ &second, 2, 0, nullptr };       // same bucket as 6; a node without a path
    Node negative{ nullptr, -3, 0, path };
    void* buckets[4]{};
    buckets[6 % 4] = &first;
    buckets[static_cast<uint64_t>(static_cast<int64_t>(-3)) % 4] = &negative;
    char grid[0x100]{};
    uint32_t count = 4;
    memcpy(grid + kAiGridPathBuckets, &count, sizeof(count));
    void* table = buckets;
    memcpy(grid + kAiGridPathTable, &table, sizeof(table));

    bt_calls = 0;
    auto r = Call(f, { U(grid), 6 }, 0);
    CHECK(bt_calls == 1 && bt_seen[0] == U(grid) && static_cast<int32_t>(bt_seen[1]) == 6);
    CHECK((r.X0 & 0xff) == 1);
    r = Call(f, { U(grid), static_cast<uint32_t>(-3) }, 0);
    CHECK(bt_calls == 2 && static_cast<int32_t>(bt_seen[1]) == -3 && (r.X0 & 0xff) == 1);
    // An id the game function would fault on is not passed to it.
    r = Call(f, { U(grid), 7 }, 0);
    CHECK(bt_calls == 2 && (r.X0 & 0xff) == 0);
    r = Call(f, { U(grid), 2 }, 0);                         // found, without a path
    CHECK(bt_calls == 2 && (r.X0 & 0xff) == 0);
    r = Call(f, { U(grid), static_cast<uint32_t>(kAiGridNoPath) }, 0);
    CHECK(bt_calls == 2 && (r.X0 & 0xff) == 0);
    count = 0;
    memcpy(grid + kAiGridPathBuckets, &count, sizeof(count));
    r = Call(f, { U(grid), 6 }, 0);
    CHECK(bt_calls == 2 && (r.X0 & 0xff) == 0);
}

TEST(adapter_applied_material_textures)
{
    int material = 0;
    uint32_t parameter = 0x00777777, guid = 0x00123456;
    int resource = 0;

    auto load = Adapter("ls__AppliedMaterial__LoadVirtualTexture", reinterpret_cast<void*>(&bt_record));
    bt_calls = 0;
    auto r = Call(load, { U(&material), U(&guid) }, U(&resource));
    CHECK(bt_calls == 1 && bt_seen[0] == U(&guid));         // the GUID alone
    CHECK(r.X0 == U(&resource));

    auto set = Adapter("ls__AppliedMaterial__TryOverrideTexture2DParameter", reinterpret_cast<void*>(&bt_record));
    r = Call(set, { U(&material), U(&parameter), U(&guid) }, 1);
    CHECK(bt_calls == 2 && bt_seen[0] == U(&material));
    CHECK(static_cast<uint32_t>(bt_seen[1]) == parameter);  // the parameter's name by value
    CHECK(bt_seen[2] == U(&guid));
    CHECK((r.X0 & 0xff) == 1);
    r = Call(set, { U(&material), U(&parameter), U(&guid) }, 0);
    CHECK((r.X0 & 0xff) == 0);
}

TEST(adapter_texture_manager_unload_texture)
{
    auto adapter = FindCallAdapter("ls__TextureManager__UnloadTexture");
    CHECK(adapter && adapter->Target && adapter->GlobalTarget && adapter->GlobalOffset == kTextureManagerInResourceManager);
    // A resource manager with its texture manager where the game has it.
    char resourceManager[0x100]{};
    int textureManager = 0, passed = 0;
    void* manager = &textureManager;
    memcpy(resourceManager + kTextureManagerInResourceManager, &manager, sizeof(manager));
    void* variable = resourceManager;       // ls::ResourceManager::m_ptr
    *adapter->GlobalTarget = &variable;
    *adapter->Target = reinterpret_cast<void*>(&bt_record);
    uint32_t guid = 0x00123456;
    bt_calls = 0;
    // The manager is taken from the resource manager, not from what Norbyte's caller passes.
    auto r = Call(adapter->Function, { U(&passed), U(&guid) }, 1);
    CHECK(bt_calls == 1 && bt_seen[0] == U(&textureManager) && bt_seen[1] == U(&guid) && (r.X0 & 0xff) == 1);
    // No resource manager yet, or none of its texture manager: nothing is called.
    manager = nullptr;
    memcpy(resourceManager + kTextureManagerInResourceManager, &manager, sizeof(manager));
    r = Call(adapter->Function, { U(&passed), U(&guid) }, 1);
    CHECK(bt_calls == 1 && (r.X0 & 0xff) == 0);
    variable = nullptr;
    r = Call(adapter->Function, { U(&passed), U(&guid) }, 1);
    CHECK(bt_calls == 1 && (r.X0 & 0xff) == 0);
    *adapter->GlobalTarget = nullptr;
    r = Call(adapter->Function, { U(&passed), U(&guid) }, 1);
    CHECK(bt_calls == 1 && (r.X0 & 0xff) == 0);
}

// --- the stats functions the game has only inline ---------------------------

// Stand-ins for the game functions eoc::ParseStaticBoosts is inlined around.
struct BoostsSeen
{
    char const* Text;
    uint32_t Length;
    void* Function;
    char Object[kFunctionSize];     // the function object as ParseBoosts was handed it
    int Called;
    void* Filter;
    void* Boosts;
    int Ended;
    void* EndedWith[3];
    bool Empty;                     // ParseBoosts leaves the object holding nothing
};
BoostsSeen gBoosts;

void FakeBoostCall(void* methods, void const*, void const*, void const*, void const*)
{
    // As the game's: the captures follow the three methods.
    gBoosts.Called++;
    memcpy(&gBoosts.Filter, static_cast<char*>(methods) + (kFunctionCaptures - kFunctionCall), sizeof(void*));
    memcpy(&gBoosts.Boosts, static_cast<char*>(methods) + (kFunctionCaptures - kFunctionCall) + sizeof(void*), sizeof(void*));
}

void* FakeBoostCopy(void*, void const*, void* to) { return to; }

void* FakeBoostMoveDestroy(void* methods, void* from, void* to)
{
    gBoosts.Ended++;
    gBoosts.EndedWith[0] = methods;
    gBoosts.EndedWith[1] = from;
    gBoosts.EndedWith[2] = to;
    return to;
}

bool FakeParseBoosts(char const* text, uint32_t length, void* function)
{
    gBoosts.Text = text;
    gBoosts.Length = length;
    gBoosts.Function = function;
    memcpy(gBoosts.Object, function, sizeof(gBoosts.Object));
    // The game's call of the object: through its methods pointer, with the methods as the first argument.
    void* methods;
    memcpy(&methods, function, sizeof(methods));
    adapter::BoostCall* call;
    memcpy(&call, methods, sizeof(call));
    call(methods, nullptr, nullptr, nullptr, nullptr);
    if (gBoosts.Empty) memset(function, 0, sizeof(void*));
    return length != 0;
}

TEST(parse_static_boosts_hands_parse_boosts_the_function_object_the_game_builds)
{
    constexpr char kMember[] = "eoc__ParseStaticBoosts";
    auto adapter = FindCallAdapter(kMember);
    CHECK(adapter && !adapter->Target && adapter->Needs.size() == 4);
    Need(kMember, "(anonymous namespace)::ParseBoosts", reinterpret_cast<void*>(&FakeParseBoosts));
    Need(kMember, "eoc::ParseStaticBoosts: the boost function's Call", reinterpret_cast<void*>(&FakeBoostCall));
    Need(kMember, "eoc::ParseStaticBoosts: the boost function's Copy", reinterpret_cast<void*>(&FakeBoostCopy));
    Need(kMember, "eoc::ParseStaticBoosts: the boost function's MoveDestroy", reinterpret_cast<void*>(&FakeBoostMoveDestroy));

    char const text[] = "AC(2);Advantage(AttackRoll)";
    GameStringView view{ text, sizeof(text) - 1 };
    uint64_t boosts[2]{}, filter[16]{};
    gBoosts = {};
    CHECK(adapter::ParseStaticBoosts(view, boosts, filter));

    // The text and its length, apart.
    CHECK(gBoosts.Text == text && gBoosts.Length == sizeof(text) - 1);
    // The object: its methods pointer is the address of its own methods, the
    // methods are the lambda's three, the captures the filter and the array.
    void* words[kFunctionSize / sizeof(void*)];
    memcpy(words, gBoosts.Object, sizeof(words));
    CHECK(words[kFunctionMethods / 8] == static_cast<char*>(gBoosts.Function) + kFunctionCall);
    CHECK(words[kFunctionCall / 8] == reinterpret_cast<void*>(&FakeBoostCall));
    CHECK(words[kFunctionCopy / 8] == reinterpret_cast<void*>(&FakeBoostCopy));
    CHECK(words[kFunctionMoveDestroy / 8] == reinterpret_cast<void*>(&FakeBoostMoveDestroy));
    CHECK(words[kFunctionCaptures / 8] == filter && words[kFunctionCaptures / 8 + 1] == boosts);
    CHECK(words[kFunctionCaptures / 8 + 2] == nullptr && words[kFunctionCaptures / 8 + 3] == nullptr);
    // The game's Call finds both captures where it reads them.
    CHECK(gBoosts.Called == 1 && gBoosts.Filter == filter && gBoosts.Boosts == boosts);
    // The object is ended as its owner ends it: its MoveDestroy, with the methods twice and no destination.
    void* methods = static_cast<char*>(gBoosts.Function) + kFunctionCall;
    CHECK(gBoosts.Ended == 1 && gBoosts.EndedWith[0] == methods && gBoosts.EndedWith[1] == methods && gBoosts.EndedWith[2] == nullptr);

    // No filter: the object captures one that holds nothing, never a null pointer.
    gBoosts = {};
    CHECK(adapter::ParseStaticBoosts(view, boosts, nullptr));
    CHECK(gBoosts.Filter != nullptr && gBoosts.Boosts == boosts);
    void* held = &held;
    memcpy(&held, gBoosts.Filter, sizeof(held));
    CHECK(held == nullptr);

    // What ParseBoosts answers is the answer; an object it left holding nothing is not ended.
    GameStringView empty{ text, 0 };
    gBoosts = {};
    gBoosts.Empty = true;
    CHECK(!adapter::ParseStaticBoosts(empty, boosts, filter));
    CHECK(gBoosts.Length == 0 && gBoosts.Ended == 0);

    for (auto const& need : adapter->Needs) *need.Target = nullptr;
}

// Stand-ins for what stats::Object::SetPropertyString is inlined around: each
// does the least its game function does for the caller, and records its call.
struct PropertyFakes
{
    std::vector<std::string> Calls;
    // The lookups' answers.
    int32_t Handle{ 0 };
    void* Modifier{ nullptr };
    int32_t Position{ 0 };
    // What a text splits into by text key.
    std::vector<std::pair<uint32_t, std::string>> Parts;
    uint32_t const* DefaultKeySeen{ nullptr };
    // Live objects of each kind.
    int Strings{ 0 }, Maps{ 0 }, Arrays{ 0 };
    int Conditions{ 100 };
    // What was stored.
    void* MapSet{ nullptr };
    uint32_t MapKey{ 0 };
    std::vector<std::pair<uint32_t, uint64_t>> Stored;
    std::vector<void*> Lists;
    std::vector<std::pair<void*, void*>> Inserted;
    void* Requirements{ nullptr };
};
PropertyFakes gProperty;

std::string Text(void const* string)
{
    auto flag = static_cast<int8_t>(static_cast<char const*>(string)[kGameStringFlag]);
    if (flag >= 0) return std::string(static_cast<char const*>(string), static_cast<size_t>(flag));
    char const* characters;
    uint32_t length;
    memcpy(&characters, string, sizeof(characters));
    memcpy(&length, static_cast<char const*>(string) + kGameStringLongLength, sizeof(length));
    return std::string(characters, length);
}

// A string as the game lays one out: short in itself, long behind a pointer.
void* MakeString(void* string, char const* text, uint32_t length)
{
    char* bytes = static_cast<char*>(string);
    memset(bytes, 0, kGameStringBytes);
    if (length < kGameStringFlag) {
        memcpy(bytes, text, length);
        bytes[kGameStringFlag] = static_cast<char>(length);
    } else {
        char* characters = static_cast<char*>(malloc(length + 1));
        memcpy(characters, text, length);
        characters[length] = 0;
        memcpy(bytes, &characters, sizeof(characters));
        memcpy(bytes + kGameStringLongLength, &length, sizeof(length));
        bytes[kGameStringFlag] = static_cast<char>(0x80);
    }
    gProperty.Strings++;
    return string;
}

void* FakeStringFromText(void* string, char const* text) { return MakeString(string, text, static_cast<uint32_t>(strlen(text))); }
void* FakeStringFromView(void* string, char const* text, uint32_t length) { return MakeString(string, text, length); }

void* FakeStringEnd(void* string)
{
    if (static_cast<int8_t>(static_cast<char*>(string)[kGameStringFlag]) < 0) {
        void* characters;
        memcpy(&characters, string, sizeof(characters));
        free(characters);
    }
    gProperty.Strings--;
    return string;
}

void FakeStringTrim(void* string)
{
    auto text = Text(string);
    size_t begin = text.find_first_not_of(' '), end = text.find_last_not_of(' ');
    text = begin == std::string::npos ? std::string() : text.substr(begin, end - begin + 1);
    FakeStringEnd(string);
    MakeString(string, text.data(), static_cast<uint32_t>(text.size()));
}

int32_t FakeHandleByName(void const*, uint32_t const* name)
{
    gProperty.Calls.push_back("handle " + std::to_string(*name));
    return gProperty.Handle;
}

void* FakeByHandle(void const*, int32_t handle)
{
    gProperty.Calls.push_back("modifier " + std::to_string(handle));
    return gProperty.Modifier;
}

int32_t FakeGetPosition(int32_t valueList, char const* text)
{
    gProperty.Calls.push_back("position " + std::to_string(valueList) + " '" + text + "'");
    return gProperty.Position;
}

int32_t FakeAddCondition(void*, void const* text)
{
    gProperty.Calls.push_back("condition '" + Text(text) + "'");
    return gProperty.Conditions++;
}

void FakeParseRequirements(void const* text, void* requirements)
{
    gProperty.Calls.push_back("requirements '" + Text(text) + "'");
    gProperty.Requirements = requirements;
}

// The map of text key to text, as far as the caller reads it: the keys, their number, the texts.
void FakeSplitPerTextKey(void* map, void const* text, uint32_t const* defaultKey)
{
    gProperty.Calls.push_back("split '" + Text(text) + "'");
    gProperty.DefaultKeySeen = defaultKey;
    memset(map, 0, kHashMapSize);
    auto count = static_cast<uint32_t>(gProperty.Parts.size());
    auto keys = static_cast<uint32_t*>(calloc(count + 1, sizeof(uint32_t)));
    auto texts = static_cast<char*>(calloc(count + 1, kGameStringBytes));
    for (uint32_t n = 0; n < count; n++) {
        keys[n] = gProperty.Parts[n].first;
        MakeString(texts + n * kGameStringBytes, gProperty.Parts[n].second.data(), static_cast<uint32_t>(gProperty.Parts[n].second.size()));
    }
    memcpy(static_cast<char*>(map) + kHashMapKeys, &keys, sizeof(keys));
    memcpy(static_cast<char*>(map) + kHashMapKeysSize, &count, sizeof(count));
    memcpy(static_cast<char*>(map) + kHashMapValues, &texts, sizeof(texts));
    gProperty.Maps++;
}

void* FakeTextMapEnd(void* map)
{
    uint32_t* keys;
    char* texts;
    uint32_t count;
    memcpy(&keys, static_cast<char*>(map) + kHashMapKeys, sizeof(keys));
    memcpy(&count, static_cast<char*>(map) + kHashMapKeysSize, sizeof(count));
    memcpy(&texts, static_cast<char*>(map) + kHashMapValues, sizeof(texts));
    for (uint32_t n = 0; n < count; n++) FakeStringEnd(texts + n * kGameStringBytes);
    free(keys);
    free(texts);
    gProperty.Maps--;
    return map;
}

// ls::Split<2> with its flag clear, as the game's code behaves: delimiters
// before the first part and after it are skipped; the first part and what
// is left; a text of delimiters alone, or none, gives an empty part at its
// end and no rest.
adapter::GameSplit FakeSplitInTwo(GameStringView const* text, GameStringView const* delimiters, bool)
{
    adapter::GameSplit split{ { nullptr, 0 }, { nullptr, 0 } };
    char const* at = text->Text;
    auto left = static_cast<int32_t>(text->Length);
    auto delimiter = [&](char c) { return memchr(delimiters->Text, c, delimiters->Length) != nullptr; };
    auto skip = [&]() { while (left > 0 && delimiter(*at)) { at++; left--; } };
    if (left <= 0) {
        split.First = { at, 0 };
        return split;
    }
    skip();
    int32_t length = 0;
    while (length < left && !delimiter(at[length])) length++;
    split.First = { at, static_cast<uint32_t>(length) };
    if (length == left) return split;
    at += length;
    left -= length;
    skip();
    split.Rest = { at, static_cast<uint32_t>(left) };
    return split;
}

void FakeInsert(void* list, void* functor) { gProperty.Inserted.push_back({ list, functor }); }

// A functor list: its vtable pointer, whose slots are those of the game's.
struct FakeFunctorList
{
    void** Vtable;
    uint32_t TextKey;
};
void* gFakeFunctorListSlots[8];

void* FakeCreateFunctorList(void* manager, void const*, uint32_t const* attribute, uint32_t const* textKey)
{
    gProperty.Calls.push_back("list " + std::to_string(*attribute) + " " + std::to_string(*textKey));
    gFakeFunctorListSlots[kFunctorListInsertSlot / sizeof(void*)] = reinterpret_cast<void*>(&FakeInsert);
    auto list = new FakeFunctorList{ gFakeFunctorListSlots, *textKey };
    gProperty.Lists.push_back(list);
    (void)manager;
    return list;
}

void* FakeParseFunctor(void const* text, uint64_t index)
{
    auto parsed = Text(text);
    gProperty.Calls.push_back("functor " + std::to_string(index) + " '" + parsed + "'");
    // A text that is no functor gives none; the others are told apart by their number.
    return parsed == "bad" ? nullptr : reinterpret_cast<void*>(0x1000 + index);
}

void FakeAcquire(uint32_t index) { gProperty.Calls.push_back("acquire " + std::to_string(index)); }

void FakeFixedStringCreate(uint32_t* result, char const* text, int32_t length)
{
    gProperty.Calls.push_back(std::string("fixed string '") + text + "' " + std::to_string(length));
    *result = *text ? 7000u + static_cast<uint32_t>(strlen(text)) : 0xffffffffu;
}

uint32_t* FakeFixedStringMoveAssign(uint32_t* to, uint32_t* from)
{
    *to = *from;
    *from = 0xffffffffu;
    return to;
}

void* FakeFixedStringEnd(uint32_t* string)
{
    gProperty.Calls.push_back("fixed string ended " + std::to_string(static_cast<int32_t>(*string)));
    return string;
}

template <size_t ElementSize>
void FakeReallocate(void* array, int32_t capacity)
{
    void* elements;
    memcpy(&elements, array, sizeof(elements));
    if (!elements) gProperty.Arrays++;
    elements = realloc(elements, static_cast<size_t>(capacity) * ElementSize);
    memcpy(array, &elements, sizeof(elements));
    memcpy(static_cast<char*>(array) + kArrayCapacity, &capacity, sizeof(capacity));
}

void* FakeArrayEnd(void* array)
{
    void* elements;
    memcpy(&elements, array, sizeof(elements));
    if (elements) gProperty.Arrays--;
    free(elements);
    return array;
}

// The map's set copies the array: what was in it is kept for the test.
template <size_t ElementSize>
void FakeMapSet(void* map, uint32_t const* key, void const* array)
{
    gProperty.MapSet = map;
    gProperty.MapKey = *key;
    char const* elements;
    uint32_t size;
    memcpy(&elements, array, sizeof(elements));
    memcpy(&size, static_cast<char const*>(array) + kArraySize, sizeof(size));
    gProperty.Stored.clear();
    for (uint32_t n = 0; n < size; n++) {
        uint32_t textKey;
        uint64_t value = 0;
        memcpy(&textKey, elements + n * ElementSize, sizeof(textKey));
        memcpy(&value, elements + n * ElementSize + (ElementSize == kFunctorGroupSize ? kFunctorGroupList : kRollConditionCondition),
            ElementSize == kFunctorGroupSize ? 8 : 4);
        gProperty.Stored.push_back({ textKey, value });
    }
}

// Stats, an object and one attribute, as far as the function reads them.
struct PropertyStats
{
    alignas(8) char Stats[kStatsSize]{};
    alignas(8) char Object[0x100]{};
    alignas(8) char List[kNamedManagerSize]{};
    alignas(8) char Modifier[0x10]{};
    alignas(8) char ValueList[0x20]{};
    void* Lists[3]{};
    void* ValueLists[6]{};
    int32_t Values[4]{ 11, 22, 33, 44 };
    void* FunctorBuckets[7]{};
    alignas(8) char FunctorNode[0x18]{};
    // The fixed strings the game's variables hold.
    uint32_t Names[8]{ 501, 502, 503, 504, 505, 506, 507, 508 };
    uint32_t DefaultKey{ 600 };
    uint32_t* DefaultKeyReference{ &DefaultKey };
    void* StatsVariable{ Stats };

    static constexpr int32_t kListIndex = 2, kValueListIndex = 5, kAttribute = 1;
    static constexpr uint32_t kObjectName = 70, kAttributeName = 80;

    template <class T> void Put(char* at, size_t offset, T value) { memcpy(at + offset, &value, sizeof(value)); }

    PropertyStats()
    {
        Lists[kListIndex] = List;
        ValueLists[kValueListIndex] = ValueList;
        Put<void*>(Stats, kStatsModifierLists + kNamedManagerValues, Lists);
        Put<int32_t>(Stats, kStatsModifierLists + kNamedManagerNextHandle, 3);
        Put<void*>(Stats, kStatsValueLists + kNamedManagerValues, ValueLists);
        Put<int32_t>(Stats, kStatsValueLists + kNamedManagerNextHandle, 6);
        Put<uint32_t>(Stats, kStatsFunctors + kMapBucketCount, 7);
        Put<void*>(Stats, kStatsFunctors + kMapBuckets, FunctorBuckets);
        Put<int32_t>(Object, kStatsObjectModifierList, kListIndex);
        Put<int32_t*>(Object, kStatsObjectProperties, Values);
        Put<uint32_t>(Object, kStatsObjectName, kObjectName);
        Put<uint32_t>(Object, kStatsObjectAIFlags, 0xffffffffu);
        Put<int32_t>(Modifier, kModifierValueList, kValueListIndex);
        Type(999);
    }

    // The name of the attribute's list of values.
    void Type(uint32_t name) { Put<uint32_t>(ValueList, kValueListName, name); }
    int32_t Value() const { return Values[kAttribute]; }
};

void BindPropertyFakes(PropertyStats& stats)
{
    constexpr char kMember[] = "stats__Object__SetPropertyString";
    constexpr char const* kNames[] = {
        "EoCFS::strStatsFunctors", "EoCFS::strConditions", "EoCFS::strTargetConditions", "EoCFS::strUseConditions",
        "EoCFS::strRollConditions", "EoCFS::strAIFlags", "EoCFS::strConstantInt", "EoCFS::strRequirements",
    };
    for (size_t n = 0; n < std::size(kNames); n++) Need(kMember, kNames[n], &stats.Names[n]);
    Need(kMember, "gRPGStats", &stats.StatsVariable);
    Need(kMember, "eoc::strDefault", &stats.DefaultKeyReference);
    Need(kMember, "CRPGStats_Modifier_List", stats.List);
    Need(kMember, "eoc::StatsFunctorList", gFakeFunctorListSlots);
#define FAKE(entry, function) Need(kMember, entry, reinterpret_cast<void*>(function))
    FAKE("CNamedElementManager<CRPGStats_Modifier>::GetHandleByName", &FakeHandleByName);
    FAKE("CNamedElementManager<CRPGStats_Modifier>::GetByHandle", &FakeByHandle);
    FAKE("CRPGStats_Modifier::GetPosition", &FakeGetPosition);
    FAKE("RPGStats::AddCondition", &FakeAddCondition);
    FAKE("ParseRequirements", &FakeParseRequirements);
    FAKE("ls::STDString::STDString(char const*)", &FakeStringFromText);
    FAKE("ls::STDString::STDString(ls::_StringView<char> const&)", &FakeStringFromView);
    FAKE("ls::STDString::TrimWhiteSpaceLeftAndRight", &FakeStringTrim);
    FAKE("ls::STDString::~STDString", &FakeStringEnd);
    FAKE("eoc::TextKeyParsingHelpers::SplitStringPerTextKeyWithMerge", &FakeSplitPerTextKey);
    FAKE("eoc::TextKeyParsingHelpers::SplitStringPerTextKeyNoMerge", &FakeSplitPerTextKey);
    FAKE("ls::HashMap<ls::FixedString, ls::STDString>::~HashMap", &FakeTextMapEnd);
    FAKE("ls::Split<2>", &FakeSplitInTwo);
    FAKE("eoc::StatsFunctorListManager::CreateStatsFunctorList", &FakeCreateFunctorList);
    FAKE("eoc::StatsFunctorBase::ParseStatsFunctor", &FakeParseFunctor);
    FAKE("ls__FixedString__IncRef", &FakeAcquire);
    FAKE("ls::FixedString::Create", &FakeFixedStringCreate);
    FAKE("ls::FixedString::operator=(ls::FixedString&&)", &FakeFixedStringMoveAssign);
    FAKE("ls::FixedString::~FixedString", &FakeFixedStringEnd);
    FAKE("ls::DynamicArray<TextKeyStatsFunctorList>::Reallocate", &FakeReallocate<kFunctorGroupSize>);
    FAKE("ls::DynamicArray<TextKeyStatsFunctorList>::~DynamicArray", &FakeArrayEnd);
    FAKE("ls::HashMap<ls::FixedString, ls::DynamicArray<TextKeyStatsFunctorList>>::SetUniversal", &FakeMapSet<kFunctorGroupSize>);
    FAKE("ls::DynamicArray<TextKeyRollCondition>::Reallocate", &FakeReallocate<kRollConditionSize>);
    FAKE("ls::DynamicArray<TextKeyRollCondition>::~DynamicArray", &FakeArrayEnd);
    FAKE("ls::HashMap<ls::FixedString, ls::DynamicArray<TextKeyRollCondition>>::SetUniversal", &FakeMapSet<kRollConditionSize>);
#undef FAKE
}

// One call with fresh records; the lookups find the attribute unless told otherwise.
void SetProperty(PropertyStats& stats, char const* value)
{
    uint32_t name = PropertyStats::kAttributeName;
    adapter::StatsObjectSetPropertyString(stats.Object, &name, value);
}

void FreshProperty(PropertyStats& stats)
{
    for (auto list : gProperty.Lists) delete static_cast<FakeFunctorList*>(list);
    gProperty = {};
    gProperty.Handle = PropertyStats::kAttribute;
    gProperty.Modifier = stats.Modifier;
}

bool Balanced() { return gProperty.Strings == 0 && gProperty.Maps == 0 && gProperty.Arrays == 0; }

TEST(set_property_string_finds_the_attribute_as_the_game_does_or_sets_nothing)
{
    PropertyStats stats;
    BindPropertyFakes(stats);
    auto name = std::to_string(PropertyStats::kAttributeName);
    auto handle = std::to_string(PropertyStats::kAttribute);

    // Found: the list by the object's index, the handle by the name, the
    // modifier by the handle, the list of values by the modifier's index.
    FreshProperty(stats);
    gProperty.Position = 9;
    SetProperty(stats, "text");
    CHECK(gProperty.Calls == (std::vector<std::string>{ "handle " + name, "modifier " + handle,
        "position " + std::to_string(PropertyStats::kValueListIndex) + " 'text'" }));
    CHECK(stats.Value() == 9 && stats.Values[0] == 11 && stats.Values[2] == 33);

    // Nothing is set, and nothing more is asked, when a step finds nothing.
    auto untouched = [&](std::vector<std::string> const& calls) {
        bool same = gProperty.Calls == calls && stats.Value() == 9;
        CHECK(same);
    };
    FreshProperty(stats);
    stats.Put<int32_t>(stats.Object, kStatsObjectModifierList, -1);
    SetProperty(stats, "text");
    untouched({});
    stats.Put<int32_t>(stats.Object, kStatsObjectModifierList, 3);         // not below the lists' next handle
    SetProperty(stats, "text");
    untouched({});
    stats.Put<int32_t>(stats.Object, kStatsObjectModifierList, 1);         // no list there
    SetProperty(stats, "text");
    untouched({});
    stats.Put<int32_t>(stats.Object, kStatsObjectModifierList, PropertyStats::kListIndex);

    FreshProperty(stats);
    gProperty.Handle = -1;                                                  // the list has no such attribute
    SetProperty(stats, "text");
    untouched({ "handle " + name });
    FreshProperty(stats);
    gProperty.Modifier = nullptr;
    SetProperty(stats, "text");
    untouched({ "handle " + name, "modifier " + handle });

    FreshProperty(stats);
    for (int32_t index : { -1, 6, 4 }) {                                    // below zero, not below the next handle, no list there
        stats.Put<int32_t>(stats.Modifier, kModifierValueList, index);
        gProperty.Calls.clear();
        SetProperty(stats, "text");
        untouched({ "handle " + name, "modifier " + handle });
    }
    stats.Put<int32_t>(stats.Modifier, kModifierValueList, PropertyStats::kValueListIndex);

    // No stats yet, no object, no name, no text.
    FreshProperty(stats);
    void* loaded = stats.StatsVariable;
    stats.StatsVariable = nullptr;
    SetProperty(stats, "text");
    stats.StatsVariable = loaded;
    uint32_t attribute = PropertyStats::kAttributeName;
    adapter::StatsObjectSetPropertyString(nullptr, &attribute, "text");
    adapter::StatsObjectSetPropertyString(stats.Object, nullptr, "text");
    adapter::StatsObjectSetPropertyString(stats.Object, &attribute, nullptr);
    untouched({});
}

TEST(set_property_string_sets_each_type_of_attribute_as_a_data_line_does)
{
    PropertyStats stats;
    BindPropertyFakes(stats);
    auto position = [](char const* text) { return "position " + std::to_string(PropertyStats::kValueListIndex) + " '" + text + "'"; };
    auto after = [](size_t lookups = 2) { return std::vector<std::string>(gProperty.Calls.begin() + static_cast<ptrdiff_t>(lookups), gProperty.Calls.end()); };
    enum { Functors, Conditions, TargetConditions, UseConditions, RollConditions, AIFlags, ConstantInt, Requirements };

    // Any other type: the position, unless there is none; then 0 for an empty text, and for another the value stays.
    FreshProperty(stats);
    gProperty.Position = 5;
    SetProperty(stats, "Label");
    CHECK(after() == std::vector<std::string>{ position("Label") } && stats.Value() == 5);
    gProperty.Position = -1;
    SetProperty(stats, "Unknown");
    CHECK(stats.Value() == 5);
    SetProperty(stats, "");
    CHECK(stats.Value() == 0);

    // ConstantInt: whatever the position is, -1 too.
    FreshProperty(stats);
    stats.Type(stats.Names[ConstantInt]);
    gProperty.Position = -1;
    SetProperty(stats, "-1");
    CHECK(after() == std::vector<std::string>{ position("-1") } && stats.Value() == -1);

    // The three kinds of condition: the text becomes a condition, whose index is the value.
    for (int type : { Conditions, TargetConditions, UseConditions }) {
        FreshProperty(stats);
        stats.Type(stats.Names[type]);
        gProperty.Conditions = 100 + type;
        SetProperty(stats, "HasStatus('PRONE') and not Dead()");
        CHECK(after() == std::vector<std::string>{ "condition 'HasStatus('PRONE') and not Dead()'" });
        CHECK(stats.Value() == 100 + type && Balanced());
    }

    // AIFlags: the object's flags become the fixed string of the text; the value is not touched.
    FreshProperty(stats);
    stats.Type(stats.Names[AIFlags]);
    stats.Values[PropertyStats::kAttribute] = 22;
    SetProperty(stats, "CanNotUse");
    CHECK(after() == (std::vector<std::string>{ "fixed string 'CanNotUse' -1", "fixed string ended -1" }));
    uint32_t flags;
    memcpy(&flags, stats.Object + kStatsObjectAIFlags, sizeof(flags));
    CHECK(flags == 7009 && stats.Value() == 22);

    // Requirements: parsed into the object's requirements.
    FreshProperty(stats);
    stats.Type(stats.Names[Requirements]);
    SetProperty(stats, "Combat;!Immobile");
    CHECK(after() == std::vector<std::string>{ "requirements 'Combat;!Immobile'" });
    CHECK(gProperty.Requirements == stats.Object + kStatsObjectRequirements && stats.Value() == 22 && Balanced());

    // RollConditions: a condition for each text key's text, stored under the attribute's name.
    FreshProperty(stats);
    stats.Type(stats.Names[RollConditions]);
    gProperty.Parts = { { 600, "Advantage()" }, { 0xffffffffu, "a text of more than fifteen characters" }, { 610, "" } };
    SetProperty(stats, "the whole text");
    CHECK(after() == (std::vector<std::string>{ "split 'the whole text'", "condition 'Advantage()'", "acquire 600",
        "condition 'a text of more than fifteen characters'", "condition ''", "acquire 610" }));
    CHECK(gProperty.DefaultKeySeen == &stats.DefaultKey);
    CHECK(gProperty.MapSet == stats.Object + kStatsObjectRollConditions && gProperty.MapKey == PropertyStats::kAttributeName);
    CHECK(gProperty.Stored == (std::vector<std::pair<uint32_t, uint64_t>>{ { 600, 100 }, { 0xffffffffu, 101 }, { 610, 102 } }));
    CHECK(stats.Value() == 22 && Balanced());
    // No text key at all: an empty array is stored.
    FreshProperty(stats);
    SetProperty(stats, "");
    CHECK(after() == std::vector<std::string>{ "split ''" } && gProperty.MapSet == stats.Object + kStatsObjectRollConditions
        && gProperty.Stored.empty() && Balanced());

    // StatsFunctors: a list for each text key, filled from the parts of its
    // text between semicolons. An empty part is not parsed; a part that is
    // no functor is counted and not inserted; the count runs through the keys.
    FreshProperty(stats);
    stats.Type(stats.Names[Functors]);
    gProperty.Parts = { { 600, "DealDamage(1d6,Fire); ApplyStatus(BURNING,100,2) ;;  ; bad" }, { 620, "RemoveStatus(SELF,BURNING)" }, { 630, "" } };
    SetProperty(stats, "the functors");
    auto attribute = std::to_string(PropertyStats::kAttributeName);
    CHECK(after() == (std::vector<std::string>{ "split 'the functors'",
        "list " + attribute + " 600", "functor 0 'DealDamage(1d6,Fire)'", "functor 1 'ApplyStatus(BURNING,100,2)'", "functor 2 'bad'", "acquire 600",
        "list " + attribute + " 620", "functor 3 'RemoveStatus(SELF,BURNING)'", "acquire 620",
        "list " + attribute + " 630", "acquire 630" }));
    CHECK(gProperty.DefaultKeySeen == &stats.DefaultKey && gProperty.Lists.size() == 3);
    if (gProperty.Lists.size() == 3) {
        CHECK(gProperty.Inserted == (std::vector<std::pair<void*, void*>>{ { gProperty.Lists[0], reinterpret_cast<void*>(0x1000) },
            { gProperty.Lists[0], reinterpret_cast<void*>(0x1001) }, { gProperty.Lists[1], reinterpret_cast<void*>(0x1003) } }));
        CHECK(gProperty.Stored == (std::vector<std::pair<uint32_t, uint64_t>>{ { 600, U(gProperty.Lists[0]) }, { 620, U(gProperty.Lists[1]) },
            { 630, U(gProperty.Lists[2]) } }));
    }
    CHECK(gProperty.MapSet == stats.Object + kStatsObjectFunctors && gProperty.MapKey == PropertyStats::kAttributeName);
    CHECK(stats.Value() == 22 && Balanced());

    // A functor list of the stats that has the object's own name: the game
    // leaves every key out, and stores the empty array.
    FreshProperty(stats);
    stats.Put<uint32_t>(stats.FunctorNode, kMapNodeKey, PropertyStats::kObjectName);
    stats.Put<void*>(stats.FunctorNode, kMapNodePointerValue, &stats);
    stats.FunctorBuckets[PropertyStats::kObjectName % 7] = stats.FunctorNode;
    gProperty.Parts = { { 600, "DealDamage(1d6,Fire)" } };
    SetProperty(stats, "the functors");
    CHECK(after() == std::vector<std::string>{ "split 'the functors'" });
    CHECK(gProperty.MapSet == stats.Object + kStatsObjectFunctors && gProperty.Stored.empty() && Balanced());
    // A node of that name without a list is no list.
    stats.Put<void*>(stats.FunctorNode, kMapNodePointerValue, nullptr);
    FreshProperty(stats);
    gProperty.Parts = { { 600, "DealDamage(1d6,Fire)" } };
    SetProperty(stats, "the functors");
    CHECK(gProperty.Stored.size() == 1 && gProperty.Inserted.size() == 1 && Balanced());

    FreshProperty(stats);
    for (auto const& need : FindCallAdapter("stats__Object__SetPropertyString")->Needs) *need.Target = nullptr;
}

// --- the two prototype initialisers ------------------------------------------

// A functor and a functor list as far as the initialisers use them: the
// vtable slots the game's code calls through, the list's name, a functor's
// context.
struct FakeFunctor
{
    void** Vtable;
    char Unread[kFunctorPropertyContext - sizeof(void*)];
    uint64_t Context;
    int Id;
};
static_assert(offsetof(FakeFunctor, Context) == kFunctorPropertyContext);

struct FakeManager
{
    void** Vtable;
    std::vector<FakeFunctor*>* Functors;
    char Unread[kFunctorListUniqueName - 2 * sizeof(void*)];
    uint32_t UniqueName;
};
static_assert(offsetof(FakeManager, UniqueName) == kFunctorListUniqueName && sizeof(FakeManager) == 0x60);

std::vector<std::string> gPrototypeCalls;
void* gFunctorSlots[4];
void* gManagerSlots[10];

void* FakeClone(void const* functor)
{
    auto original = static_cast<FakeFunctor const*>(functor);
    return new FakeFunctor{ gFunctorSlots, {}, 0, original->Id + 1000 };
}

void FakeManagerDestroy(void* manager)
{
    auto list = static_cast<FakeManager*>(manager);
    gPrototypeCalls.push_back("destroy");
    for (auto functor : *list->Functors) delete functor;
    list->Functors->clear();
}

void FakeManagerInsert(void* manager, void* functor) { static_cast<FakeManager*>(manager)->Functors->push_back(static_cast<FakeFunctor*>(functor)); }
uint64_t FakeManagerCount(void const* manager) { return static_cast<FakeManager const*>(manager)->Functors->size(); }
void* FakeManagerEntry(void* manager, uint64_t index) { return (*static_cast<FakeManager*>(manager)->Functors)[index]; }

void MakeManager(void* at, uint32_t name, std::initializer_list<int> functors)
{
    gFunctorSlots[kFunctorCloneSlot / 8] = reinterpret_cast<void*>(&FakeClone);
    gManagerSlots[kNamedManagerDestroySlot / 8] = reinterpret_cast<void*>(&FakeManagerDestroy);
    gManagerSlots[kNamedManagerInsertSlot / 8] = reinterpret_cast<void*>(&FakeManagerInsert);
    gManagerSlots[kNamedManagerCountSlot / 8] = reinterpret_cast<void*>(&FakeManagerCount);
    gManagerSlots[kNamedManagerEntrySlot / 8] = reinterpret_cast<void*>(&FakeManagerEntry);
    gManagerSlots[kNamedManagerConstEntrySlot / 8] = reinterpret_cast<void*>(&FakeManagerEntry);
    auto list = new (at) FakeManager{ gManagerSlots, new std::vector<FakeFunctor*>, {}, name };
    for (int id : functors) list->Functors->push_back(new FakeFunctor{ gFunctorSlots, {}, 7, id });
}

std::vector<int> Ids(void const* manager)
{
    std::vector<int> ids;
    for (auto functor : *static_cast<FakeManager const*>(manager)->Functors) ids.push_back(functor->Id);
    return ids;
}

std::vector<uint64_t> Contexts(void const* manager)
{
    std::vector<uint64_t> contexts;
    for (auto functor : *static_cast<FakeManager const*>(manager)->Functors) contexts.push_back(functor->Context);
    return contexts;
}

// Stats and one object for the initialisers: a list of modifiers that knows
// attributes by name, the stats' pools, and the object's functors by name.
struct PrototypeStats
{
    static constexpr uint32_t kEmpty = 0xffffffffu, kDefault = 600, kObjectName = 70;

    alignas(8) char Stats[kStatsSize]{};
    alignas(8) char Object[0x100]{};
    alignas(8) char List[kNamedManagerSize]{};
    void* Lists[1]{ List };
    void* StatsVariable{ Stats };
    uint32_t Empty{ kEmpty };
    alignas(8) char DefaultEntry[0x20]{};
    // The pools: fixed strings by position, and pointers to flags.
    uint32_t FixedStrings[8]{ 900, 901, 902, 903, 904, 905, 906, 907 };
    uint64_t Flags[8]{};
    void* FlagPointers[8]{};
    // The object's values, by handle; and each attribute's handle by its name.
    std::vector<int32_t> Values;
    std::vector<std::pair<uint32_t, int32_t>> Handles;
    // The object's functors: a hash map of five buckets.
    int32_t Buckets[5]{ -1, -1, -1, -1, -1 };
    int32_t Next[4]{ -1, -1, -1, -1 };
    uint32_t Keys[4]{};
    struct { void* Elements; uint32_t Capacity; uint32_t Size; } Groups[4]{};
    alignas(8) char GroupElements[4][2 * kFunctorGroupSize]{};
    uint32_t KeyCount{ 0 };
    // Fifty names: the variables that hold a name, and the ones that hold a reference to one.
    uint32_t Names[50];
    uint32_t* References[50];

    template <class T> static void Put(char* at, size_t offset, T value) { memcpy(at + offset, &value, sizeof(value)); }

    PrototypeStats()
    {
        for (uint32_t n = 0; n < 50; n++) {
            Names[n] = 2000 + n;
            References[n] = &Names[n];
        }
        for (size_t n = 0; n < 8; n++) FlagPointers[n] = &Flags[n];
        Put<void*>(Stats, kStatsModifierLists + kNamedManagerValues, Lists);
        Put<int32_t>(Stats, kStatsModifierLists + kNamedManagerNextHandle, 1);
        Put<void*>(Stats, kStatsFixedStrings, FixedStrings);
        Put<void*>(Stats, kStatsInt64s, FlagPointers);
        Put<uint32_t>(Object, kStatsObjectName, kObjectName);
        Put<int32_t>(Object, kStatsObjectModifierList, 0);
        Put<void*>(Object, kStatsObjectFunctors + kHashMapBuckets, Buckets);
        Put<uint32_t>(Object, kStatsObjectFunctors + kHashMapBucketCount, 5);
        Put<void*>(Object, kStatsObjectFunctors + kHashMapNext, Next);
        Put<void*>(Object, kStatsObjectFunctors + kHashMapKeys, Keys);
        Put<void*>(Object, kStatsObjectFunctors + kHashMapValues, Groups);
        Put<uint32_t>(DefaultEntry, kRegistryEntryString, kDefault);
    }

    // An attribute the list has, and the object's value for it.
    void Attribute(uint32_t name, int32_t value)
    {
        Handles.push_back({ name, static_cast<int32_t>(Values.size()) });
        Values.push_back(value);
        Put<int32_t*>(Object, kStatsObjectProperties, Values.data());
        Put<int32_t*>(Object, kStatsObjectProperties + sizeof(void*), Values.data() + Values.size());
    }

    // The object's functors under an attribute: up to two groups, each a text key and a list.
    void Functors(uint32_t attribute, std::initializer_list<std::pair<uint32_t, void*>> groups)
    {
        uint32_t key = KeyCount++;
        Keys[key] = attribute;
        uint32_t bucket = (attribute * 31u) % 5;
        Next[key] = Buckets[bucket];
        Buckets[bucket] = static_cast<int32_t>(key);
        uint32_t n = 0;
        for (auto const& group : groups) {
            Put<uint32_t>(GroupElements[key], n * kFunctorGroupSize + kFunctorGroupTextKey, group.first);
            Put<void*>(GroupElements[key], n * kFunctorGroupSize + kFunctorGroupList, group.second);
            n++;
        }
        Groups[key] = { GroupElements[key], n, n };
    }
};
PrototypeStats* gPrototypeStats;

int32_t FakeAttributeHandle(void const*, uint32_t const* name)
{
    for (auto const& known : gPrototypeStats->Handles) {
        if (known.first == *name) return known.second;
    }
    return -1;
}

void* FakeAttributeModifier(void const* list, int32_t handle)
{
    return handle >= 0 ? const_cast<void*>(list) : nullptr;
}

uint32_t FakeHash(uint32_t string) { return string * 31u; }

uint32_t* FakeFixedStringAssign(uint32_t* to, uint32_t from)
{
    *to = from;
    return to;
}

// The text of a fixed string: its number, written out.
char gFixedStringText[16];
GameStringView FakeFixedStringText(uint32_t string)
{
    snprintf(gFixedStringText, sizeof(gFixedStringText), "#%u", string);
    return { gFixedStringText, static_cast<uint32_t>(strlen(gFixedStringText)) };
}

void FakePassiveClean(void*) { gPrototypeCalls.push_back("clean"); }

void const* gDescribed[2];
void FakeDescription(void* description, void const* object)
{
    gPrototypeCalls.push_back("description");
    gDescribed[0] = description;
    gDescribed[1] = object;
}

void* gCostsParsed;
void FakeParseCosts(GameStringView const* text, void* costs)
{
    gPrototypeCalls.push_back("costs '" + std::string(text->Text, text->Length) + "'");
    gCostsParsed = costs;
}

bool FakeParseBoostsText(char const* text, uint32_t length, void* function)
{
    gPrototypeCalls.push_back("boosts '" + std::string(text, length) + "'");
    return FakeParseBoosts(text, length, function);
}

// The names, in the order the two functions' needs list them after the shared ones.
constexpr char const* kPassiveNameEntries[] = {
    "(anonymous namespace)::strToggleOnEffect", "(anonymous namespace)::strToggleOffEffect", "EoCFS::strProperties",
    "(anonymous namespace)::strEnabledContext", "eoc::strStatsFunctorContext", "(anonymous namespace)::strEnabledConditions",
    "EoCFS::strConditions", "EoCFS::strBoostConditions", "EoCFS::strStatsFunctors", "(anonymous namespace)::strToggleOnFunctors",
    "(anonymous namespace)::strToggleOffFunctors", "EoCFS::strBoostContext", "EoCFS::strBoosts", "(anonymous namespace)::strToggleOffContext",
    "(anonymous namespace)::strToggleGroup", "(anonymous namespace)::strPriorityOrder", "(anonymous namespace)::strTooltipConditionalDamage",
};
constexpr char const* kInterruptNameEntries[] = {
    "(anonymous namespace)::strInterruptContext", "(anonymous namespace)::strInterruptContextScope", "(anonymous namespace)::strInterruptFlags",
    "(anonymous namespace)::strConditions of the interrupts", "(anonymous namespace)::strContainer", "(anonymous namespace)::strRoll",
    "(anonymous namespace)::strProperties", "(anonymous namespace)::strSuccess", "(anonymous namespace)::strFailure",
    "(anonymous namespace)::strCost", "(anonymous namespace)::strStack", "(anonymous namespace)::strInterruptDefaultValue",
    "(anonymous namespace)::strEnableCondition", "(anonymous namespace)::strEnableContext",
};
enum PassiveName { ToggleOnEffect, ToggleOffEffect, Properties, EnabledContext, StatsFunctorContext, EnabledConditions, Conditions,
    BoostConditions, StatsFunctors, ToggleOnFunctors, ToggleOffFunctors, BoostContext, Boosts, ToggleOffContext, ToggleGroup, PriorityOrder,
    TooltipConditionalDamage };
enum InterruptName { InterruptContext, InterruptContextScope, InterruptFlags, InterruptConditions, Container, Roll, InterruptProperties,
    Success, Failure, Cost, Stack, InterruptDefaultValue, EnableCondition, EnableContext };
constexpr size_t kInterruptNames = 20;      // where the interrupt's names start among the stats' fifty

// Gives one of the two functions its game: `names` at `first` of the stats'
// names, a variable that holds the name for an EoCFS entry and one that
// holds a reference to it for any other.
void BindPrototypeFakes(char const* member, PrototypeStats& stats, std::span<char const* const> names, size_t first)
{
    gPrototypeStats = &stats;
    for (size_t n = 0; n < names.size(); n++) {
        bool reference = strncmp(names[n], "EoCFS::", 7) != 0;
        Need(member, names[n], reference ? static_cast<void*>(&stats.References[first + n]) : static_cast<void*>(&stats.Names[first + n]));
    }
    Need(member, "gRPGStats", &stats.StatsVariable);
    Need(member, "CRPGStats_Modifier_List", stats.List);
    Need(member, "eoc::StatsFunctorList", gManagerSlots);
    Need(member, "ls::FixedString::Empty", &stats.Empty);
    Need(member, "ls::StaticFixedStringRegistry: the entry for Default", stats.DefaultEntry);
#define FAKE(entry, function) Need(member, entry, reinterpret_cast<void*>(function))
    FAKE("CNamedElementManager<CRPGStats_Modifier>::GetHandleByName", &FakeAttributeHandle);
    FAKE("CNamedElementManager<CRPGStats_Modifier>::GetByHandle", &FakeAttributeModifier);
    FAKE("ls__FixedString__GetString", &FakeFixedStringText);
    FAKE("eoc::StatsDescription::SetFromStatsObject", &FakeDescription);
    FAKE("ls::FixedString::operator=(ls::FixedString const&)", &FakeFixedStringAssign);
    FAKE("ls::FixedString::GetHash", &FakeHash);
#undef FAKE
}

template <class T>
T Field(void const* object, size_t offset)
{
    T value;
    memcpy(&value, static_cast<char const*>(object) + offset, sizeof(value));
    return value;
}

TEST(a_passive_prototype_is_filled_from_its_stats_object_as_the_stats_job_fills_it)
{
    constexpr char kMember[] = "eoc__PassivePrototype__Init";
    PrototypeStats stats;
    BindPrototypeFakes(kMember, stats, kPassiveNameEntries, 0);
    Need(kMember, "eoc::PassivePrototype::Clean", reinterpret_cast<void*>(&FakePassiveClean));
    Need(kMember, "(anonymous namespace)::ParseBoosts", reinterpret_cast<void*>(&FakeParseBoostsText));
    Need(kMember, "eoc::ParseStaticBoosts: the boost function's Call", reinterpret_cast<void*>(&FakeBoostCall));
    Need(kMember, "eoc::ParseStaticBoosts: the boost function's Copy", reinterpret_cast<void*>(&FakeBoostCopy));
    Need(kMember, "eoc::ParseStaticBoosts: the boost function's MoveDestroy", reinterpret_cast<void*>(&FakeBoostMoveDestroy));
    auto name = [&](PassiveName which) { return stats.Names[which]; };

    // The object: most attributes, one without a value, some the list does not have.
    stats.Attribute(name(ToggleOnEffect), 2);
    stats.Attribute(name(Properties), 0);
    stats.Attribute(name(EnabledContext), 1);
    stats.Attribute(name(StatsFunctorContext), 2);
    stats.Attribute(name(EnabledConditions), 77);
    stats.Attribute(name(Conditions), 88);
    stats.Attribute(name(BoostContext), 3);
    stats.Attribute(name(Boosts), 3);
    stats.Attribute(name(ToggleGroup), 4);
    stats.Attribute(name(PriorityOrder), 5);
    stats.Attribute(name(TooltipConditionalDamage), -1);
    stats.Flags[0] = 0xfffffffffff60003ull;         // Properties: only the bits 0 to 17 count
    stats.Flags[1] = 0xffffffffffffffffull;         // EnabledContext: only the low 44 bits
    stats.Flags[2] = 0x0000080000000050ull;
    stats.Flags[3] = 0x9;

    // Its functors: StatsFunctors with a group of another text key before the
    // one for Default; ToggleOnFunctors; none for ToggleOffFunctors.
    FakeManager other, source, toggleOn;
    MakeManager(&other, 4000, { 90 });
    MakeManager(&source, 4001, { 1, 2, 3 });
    MakeManager(&toggleOn, 4002, { 5 });
    stats.Functors(name(StatsFunctors), { { 650, &other }, { PrototypeStats::kDefault, &source } });
    stats.Functors(name(ToggleOnFunctors), { { PrototypeStats::kDefault, &toggleOn } });

    // The prototype, with what an earlier fill left in it.
    alignas(8) char prototype[kPassiveSize];
    memset(prototype, 0x5a, sizeof(prototype));
    PrototypeStats::Put<uint32_t>(prototype, kPassiveProperties, 0x80000000u);
    MakeManager(prototype + kPassiveStatsFunctors, 1, { 40, 41 });
    MakeManager(prototype + kPassiveToggleOnFunctors, 2, { 42 });
    MakeManager(prototype + kPassiveToggleOffFunctors, 3, { 43 });
    uint64_t boosts[2]{ 0, 0x0000000900000008ull };     // an array of nine with room for eight
    memcpy(prototype + kPassiveBoosts, boosts, sizeof(boosts));

    gPrototypeCalls.clear();
    gBoosts = {};
    adapter::PassivePrototypeInit(prototype, stats.Object);

    // Cleaned first, then described; the boosts last of the calls.
    CHECK(gPrototypeCalls == (std::vector<std::string>{ "clean", "description", "destroy", "destroy", "boosts '#903'" }));
    CHECK(gDescribed[0] == prototype + kPassiveDescription && gDescribed[1] == stats.Object);
    CHECK(Field<uint32_t>(prototype, kPassiveName) == PrototypeStats::kObjectName);
    CHECK(Field<uint32_t>(prototype, kPassiveToggleOnEffect) == 902);
    CHECK(Field<uint32_t>(prototype, kPassiveToggleOffEffect) == PrototypeStats::kEmpty);     // the list has no such attribute
    CHECK(Field<uint32_t>(prototype, kPassiveProperties) == (0x80000000u | 0x20003u));        // added to what it had
    CHECK(Field<uint64_t>(prototype, kPassiveEnabledContext) == 0x00000fffffffffffull);
    CHECK(Field<uint64_t>(prototype, kPassiveStatsFunctorContext) == 0x0000080000000050ull);
    CHECK(Field<int32_t>(prototype, kPassiveEnabledConditions) == 77 && Field<int32_t>(prototype, kPassiveConditions) == 88);
    CHECK(Field<int32_t>(prototype, kPassiveBoostConditions) == -1);
    CHECK(Field<uint64_t>(prototype, kPassiveBoostContext) == 0x9 && Field<uint64_t>(prototype, kPassiveToggleOffContext) == 0);
    CHECK(Field<uint32_t>(prototype, kPassiveToggleGroup) == 904);
    CHECK(Field<int32_t>(prototype, kPassivePriorityOrder) == 5);
    CHECK(Field<uint32_t>(prototype, kPassiveTooltipConditionalDamage) == PrototypeStats::kEmpty);   // an attribute without a value

    // StatsFunctors: clones of the Default group's functors, the list's name,
    // and every functor in the passive's functor context.
    CHECK(Ids(prototype + kPassiveStatsFunctors) == (std::vector<int>{ 1001, 1002, 1003 }));
    CHECK(Field<uint32_t>(prototype, kPassiveStatsFunctors + kFunctorListUniqueName) == 4001);
    CHECK(Contexts(prototype + kPassiveStatsFunctors) == std::vector<uint64_t>(3, 0x0000080000000050ull));
    CHECK(Ids(&source) == (std::vector<int>{ 1, 2, 3 }) && Contexts(&source) == std::vector<uint64_t>(3, 7));
    // ToggleOnFunctors likewise, in no context of the passive's; ToggleOffFunctors as they were.
    CHECK(Ids(prototype + kPassiveToggleOnFunctors) == std::vector<int>{ 1005 });
    CHECK(Field<uint32_t>(prototype, kPassiveToggleOnFunctors + kFunctorListUniqueName) == 4002);
    CHECK(Contexts(prototype + kPassiveToggleOnFunctors) == std::vector<uint64_t>{ 0 });
    CHECK(Ids(prototype + kPassiveToggleOffFunctors) == std::vector<int>{ 43 });
    CHECK(Field<uint32_t>(prototype, kPassiveToggleOffFunctors + kFunctorListUniqueName) == 3);
    // The boosts: emptied, and parsed into the prototype's array with a filter that holds nothing.
    CHECK(Field<uint32_t>(prototype, kPassiveBoosts + kArraySize) == 0 && Field<uint32_t>(prototype, kPassiveBoosts + kArrayCapacity) == 8);
    CHECK(gBoosts.Boosts == prototype + kPassiveBoosts && gBoosts.Filter != nullptr && gBoosts.Ended == 1);

    // An object that has no StatsFunctors keeps the prototype's list, whose
    // functors still get the context; a priority below zero is none; no
    // Boosts attribute is the empty text.
    stats.Keys[0] = 1;
    stats.Values[9] = -3;
    stats.Handles.erase(stats.Handles.begin() + 7);
    gPrototypeCalls.clear();
    adapter::PassivePrototypeInit(prototype, stats.Object);
    CHECK(gPrototypeCalls == (std::vector<std::string>{ "clean", "description", "destroy", "boosts ''" }));
    CHECK(Ids(prototype + kPassiveStatsFunctors) == (std::vector<int>{ 1001, 1002, 1003 }));
    CHECK(Field<int32_t>(prototype, kPassivePriorityOrder) == 0);
    CHECK(Field<uint32_t>(prototype, kPassiveProperties) == (0x80000000u | 0x20003u));

    // An object without a list of modifiers: every attribute is its default, and the functors still come.
    PrototypeStats::Put<int32_t>(stats.Object, kStatsObjectModifierList, -1);
    PrototypeStats::Put<uint32_t>(prototype, kPassiveProperties, 0);
    adapter::PassivePrototypeInit(prototype, stats.Object);
    CHECK(Field<uint32_t>(prototype, kPassiveProperties) == 0 && Field<uint32_t>(prototype, kPassiveToggleOnEffect) == PrototypeStats::kEmpty);
    CHECK(Field<uint64_t>(prototype, kPassiveEnabledContext) == 0 && Field<int32_t>(prototype, kPassiveEnabledConditions) == -1);
    CHECK(Field<int32_t>(prototype, kPassiveConditions) == -1 && Field<uint32_t>(prototype, kPassiveToggleGroup) == PrototypeStats::kEmpty);
    CHECK(Ids(prototype + kPassiveToggleOnFunctors) == std::vector<int>{ 1005 });

    // No stats, no prototype, no object: nothing is called.
    gPrototypeCalls.clear();
    adapter::PassivePrototypeInit(nullptr, stats.Object);
    adapter::PassivePrototypeInit(prototype, nullptr);
    stats.StatsVariable = nullptr;
    adapter::PassivePrototypeInit(prototype, stats.Object);
    CHECK(gPrototypeCalls.empty());

    for (auto const& need : FindCallAdapter(kMember)->Needs) *need.Target = nullptr;
}

TEST(an_interrupt_prototype_is_filled_from_its_stats_object_as_the_stats_job_fills_it)
{
    constexpr char kMember[] = "eoc__InterruptPrototype__Init";
    PrototypeStats stats;
    BindPrototypeFakes(kMember, stats, kInterruptNameEntries, kInterruptNames);
    Need(kMember, "eoc::action_resource::ParseActionResourceCostWithError", reinterpret_cast<void*>(&FakeParseCosts));
    auto name = [&](InterruptName which) { return stats.Names[kInterruptNames + which]; };

    stats.Attribute(name(InterruptContext), 0);
    stats.Attribute(name(InterruptContextScope), 1);
    stats.Attribute(name(InterruptFlags), 2);
    stats.Attribute(name(InterruptConditions), 31);
    stats.Attribute(name(Container), 5);
    stats.Attribute(name(Roll), 32);
    stats.Attribute(name(Cost), 6);
    stats.Attribute(name(InterruptDefaultValue), 3);
    stats.Attribute(name(EnableContext), 4);
    stats.Flags[0] = 0xffffff5a;                    // the bits 0 to 7
    stats.Flags[1] = 0xfe;                          // the bits 0 to 2
    stats.Flags[2] = 0xff;                          // the bit 0
    stats.Flags[3] = 0xfe;                          // the bits 0 and 1
    stats.Flags[4] = 0xffffffffffffffffull;         // four bits of a context

    FakeManager properties, failure;
    MakeManager(&properties, 4100, { 1, 2 });
    MakeManager(&failure, 4101, {});
    stats.Functors(name(InterruptProperties), { { PrototypeStats::kDefault, &properties } });
    stats.Functors(name(Success), { { 650, &properties } });         // no group for Default
    stats.Functors(name(Failure), { { PrototypeStats::kDefault, &failure } });

    alignas(8) char prototype[kInterruptSize];
    memset(prototype, 0x5a, sizeof(prototype));
    MakeManager(prototype + kInterruptPropertiesFunctors, 1, { 40 });
    MakeManager(prototype + kInterruptSuccessFunctors, 2, { 41 });
    MakeManager(prototype + kInterruptFailureFunctors, 3, { 42 });

    gPrototypeCalls.clear();
    adapter::InterruptPrototypeInit(prototype, stats.Object);

    CHECK(gPrototypeCalls == (std::vector<std::string>{ "description", "destroy", "destroy", "costs '#906'" }));
    CHECK(gDescribed[0] == prototype + kInterruptDescription && gDescribed[1] == stats.Object && gCostsParsed == prototype + kInterruptCosts);
    CHECK(Field<uint32_t>(prototype, kInterruptName) == PrototypeStats::kObjectName);
    CHECK(Field<uint16_t>(prototype, kInterruptContext) == 0x5a && Field<uint8_t>(prototype, kInterruptContextScope) == 0x6);
    CHECK(Field<uint8_t>(prototype, kInterruptFlags) == 1 && Field<uint8_t>(prototype, kInterruptDefaultValue) == 2);
    CHECK(Field<int32_t>(prototype, kInterruptConditions) == 31 && Field<int32_t>(prototype, kInterruptRoll) == 32);
    CHECK(Field<int32_t>(prototype, kInterruptEnableCondition) == -1);
    CHECK(Field<uint32_t>(prototype, kInterruptContainer) == 905 && Field<uint32_t>(prototype, kInterruptStack) == PrototypeStats::kEmpty);
    CHECK(Field<uint64_t>(prototype, kInterruptEnableContext) == (0x8ull | 0x400ull | 0x80000ull | 0x800000000ull));
    // The byte after the scope is not the prototype's to write.
    CHECK(Field<uint8_t>(prototype, kInterruptContextScope + 1) == 0x5a);

    // Properties: replaced, named, in the interrupt's functor context. Success:
    // no functors for Default, so the list stays, context and all. Failure:
    // replaced by an empty list.
    CHECK(Ids(prototype + kInterruptPropertiesFunctors) == (std::vector<int>{ 1001, 1002 }));
    CHECK(Field<uint32_t>(prototype, kInterruptPropertiesFunctors + kFunctorListUniqueName) == 4100);
    CHECK(Contexts(prototype + kInterruptPropertiesFunctors) == std::vector<uint64_t>(2, kInterruptFunctorContext));
    CHECK(Ids(prototype + kInterruptSuccessFunctors) == std::vector<int>{ 41 } && Contexts(prototype + kInterruptSuccessFunctors) == std::vector<uint64_t>{ 7 });
    CHECK(Field<uint32_t>(prototype, kInterruptSuccessFunctors + kFunctorListUniqueName) == 2);
    CHECK(Ids(prototype + kInterruptFailureFunctors).empty());
    CHECK(Field<uint32_t>(prototype, kInterruptFailureFunctors + kFunctorListUniqueName) == 4101);

    // A condition attribute is read at its handle alone, and the game takes
    // a handle up to the number of values, that number included: one past
    // the object's values. An object without flags, and without a Cost.
    stats.Handles = { { name(EnableCondition), static_cast<int32_t>(stats.Values.size()) } };
    stats.Values.reserve(stats.Values.size() + 1);
    stats.Values.data()[stats.Values.size()] = 55;
    gPrototypeCalls.clear();
    adapter::InterruptPrototypeInit(prototype, stats.Object);
    CHECK(Field<int32_t>(prototype, kInterruptEnableCondition) == 55 && Field<int32_t>(prototype, kInterruptConditions) == -1);
    CHECK(Field<uint16_t>(prototype, kInterruptContext) == 0 && Field<uint8_t>(prototype, kInterruptContextScope) == 0);
    CHECK(Field<uint8_t>(prototype, kInterruptFlags) == 0 && Field<uint8_t>(prototype, kInterruptDefaultValue) == 0);
    CHECK(Field<uint64_t>(prototype, kInterruptEnableContext) == 0 && Field<uint32_t>(prototype, kInterruptContainer) == PrototypeStats::kEmpty);
    CHECK(gPrototypeCalls == (std::vector<std::string>{ "description", "destroy", "destroy", "costs ''" }));
    stats.Handles = { { name(EnableCondition), static_cast<int32_t>(stats.Values.size()) + 1 } };
    adapter::InterruptPrototypeInit(prototype, stats.Object);
    CHECK(Field<int32_t>(prototype, kInterruptEnableCondition) == -1);

    gPrototypeCalls.clear();
    stats.StatsVariable = nullptr;
    adapter::InterruptPrototypeInit(prototype, stats.Object);
    CHECK(gPrototypeCalls.empty());

    for (auto const& need : FindCallAdapter(kMember)->Needs) *need.Target = nullptr;
}

TEST(a_function_the_game_has_inline_is_served_only_with_the_code_it_was_read_from)
{
    // Every adapter that does a missing function's work: its member has no
    // callable code in the table, the entry records the shape of a host, and
    // names as its replacements exactly what the adapter needs.
    size_t standIns = 0;
    for (auto const& a : CallAdapters()) {
        if (a.Target || a.Needs.empty()) continue;
        standIns++;
        engine::Entry const* member = nullptr;
        for (auto const& area : engine::EngineTable()) {
            for (auto const& entry : area.Entries) {
                if (strcmp(entry.Name, a.Entry) == 0) member = &entry;
            }
        }
        CHECK(member != nullptr);
        if (!member) continue;
        CHECK(member->What == engine::Kind::NoCallableCode && member->Gone.ReadFromHosts());
        CHECK(member->Gone.HostShapes.size() == member->Gone.Hosts.size());
        CHECK(member->Gone.Replacements.size() <= a.Needs.size());
        for (auto replacement : member->Gone.Replacements) {
            bool needed = false;
            for (auto const& need : a.Needs) needed = needed || strcmp(need.Entry, replacement) == 0;
            CHECK(needed);
        }
        for (auto const& need : a.Needs) CHECK(need.Target != nullptr);
    }
    CHECK(standIns == 4);

    // On a binary without the hosts the member stays null and says why.
    FakeBinary binary;
    GameImage image;
    CHECK(image.OpenFile(binary.File));
    auto resolution = engine::Resolve(engine::EngineTable(), { { &image, nullptr }, {} });
    for (auto const& row : StaticSymbolRows()) {
        auto adapter = row.How == Bound::CallAdapter ? FindCallAdapter(row.Member) : nullptr;
        if (!adapter || adapter->Target || adapter->Needs.empty()) continue;
        for (bool live : { false, true }) {
            auto decided = DecideMember(row, resolution, false, live);
            CHECK(decided.Value == nullptr && !decided.Reason.empty());
        }
    }
}

TEST(adapter_noesis_load_xaml)
{
    auto f = Adapter("Noesis__GUI__LoadXaml", reinterpret_cast<void*>(&bt_record_indirect));
    void* result = nullptr;
    int component = 0;
    char const* path = "Pages/Test.xaml";
    bt_calls = 0;
    auto r = Call(f, { U(&result), U(path) }, U(&component));
    CHECK(bt_calls == 1);
    CHECK(bt_seen[0] == U(path));           // the path alone
    CHECK(bt_seen[8] == U(&result));        // the result's address in x8
    CHECK(result == &component && r.X0 == U(&result));
}

TEST(adapter_global_allocator_is_the_c_library)
{
    auto get = reinterpret_cast<void*(*)()>(Adapter("ls__GlobalAllocator__Get", nullptr));
    auto alloc = reinterpret_cast<void*(*)(void*, size_t, int, int, uint64_t)>(Adapter("ls__GlobalAllocator__Alloc", nullptr));
    auto release = reinterpret_cast<void(*)(void*, void*)>(Adapter("ls__GlobalAllocator__Free", nullptr));
    CHECK(get() != nullptr && get() == get());
    void* small = alloc(get(), 100, 2, 0, 8);
    CHECK(small != nullptr);
    memset(small, 0x5a, 100);
    free(small);                            // the game frees what the extender allocated
    void* aligned = alloc(get(), 256, 2, 0, 64);
    CHECK(aligned != nullptr && (U(aligned) & 63) == 0);
    release(get(), aligned);
    release(get(), malloc(16));             // the extender frees what the game allocated
}

// --- game hooks and bridges ----------------------------------------------------

std::vector<std::string> gOrder;

void PreHandler(void* context, void* self, void const* time)
{
    gOrder.push_back("pre");
    CHECK(context == &gOrder && self == nullptr && time == &gFailures);
    CHECK(bt_calls == 1);                   // the game's function has run
}
void PostHandler(void* context, void* self, void const* time)
{
    gOrder.push_back("post");
    CHECK(context == &gOrder && self == nullptr && time == &gFailures);
    CHECK(bt_calls == 0);                   // the world's update has not
}

TEST(hooks_pass_the_game_arguments_through_an_unwrapped_bridge)
{
    int a = 0, b = 0;

    auto const& init = Hook("CoreLibSDM::Init");
    SetGameHookForTest(init.Entry, reinterpret_cast<void*>(&bt_record));
    bt_calls = 0;
    Call(init.Detour, { U(&a) });
    CHECK(bt_calls == 1 && bt_seen[0] == U(&a));

    auto const& roots = Hook("ls::Path::UpdateRoots");
    SetGameHookForTest(roots.Entry, reinterpret_cast<void*>(&bt_record));
    bt_calls = 0;
    Call(roots.Detour, {});
    CHECK(bt_calls == 1);

    auto const& settings = Hook("App__LoadGraphicSettings");
    SetGameHookForTest(settings.Entry, reinterpret_cast<void*>(&bt_record));
    bt_calls = 0;
    auto r = Call(settings.Detour, { U(&a), U(&b) }, 1);
    CHECK(bt_calls == 1 && bt_seen[0] == U(&a) && bt_seen[1] == U(&b));     // the second argument is passed on
    CHECK((r.X0 & 0xff) == 1);                                              // and the result returned
    r = Call(settings.Detour, { U(&a), U(&b) }, 0);
    CHECK((r.X0 & 0xff) == 0);

    auto const& start = Hook("RPGStats__Load");
    SetGameHookForTest(start.Entry, reinterpret_cast<void*>(&bt_record));
    bt_calls = 0;
    Call(start.Detour, { U(&a), U(&b) });
    CHECK(bt_calls == 1 && bt_seen[0] == U(&a) && bt_seen[1] == U(&b));

    auto const& parse = Hook("RPGStats::Start: call of RPGStats::ParseDataBuffer");
    SetGameHookForTest(parse.Entry, reinterpret_cast<void*>(&bt_record));
    bt_calls = 0;
    Call(parse.Detour, { U(&a), U(&b), U(&gFailures) });
    CHECK(bt_calls == 1 && bt_seen[0] == U(&a) && bt_seen[1] == U(&b) && bt_seen[2] == U(&gFailures));

    auto const& skeleton = Hook("ls::Visual::NotifySkeletonUpdated");
    SetGameHookForTest(skeleton.Entry, reinterpret_cast<void*>(&bt_record));
    bt_calls = 0;
    Call(skeleton.Detour, { U(&a), 0xffffff01 });
    CHECK(bt_calls == 1 && bt_seen[0] == U(&a) && static_cast<uint32_t>(bt_seen[1]) == 1);
    Call(skeleton.Detour, { U(&a), 0 });
    CHECK(bt_calls == 2 && static_cast<uint32_t>(bt_seen[1]) == 0);

    auto const& ui = Hook("gui::GameUI::UpdateFromUI");
    SetGameHookForTest(ui.Entry, reinterpret_cast<void*>(&bt_record));
    bt_calls = 0;
    Call(ui.Detour, { U(&a), U(&b) });
    CHECK(bt_calls == 1 && bt_seen[0] == U(&a) && bt_seen[1] == U(&b));
}

// The hook on CoreLibSDM::Init is also where bg3mse's fault handler goes back
// in front of the one the game has installed by then (Platform/FaultObserver.h).
void ObserveNothing(FaultRecord const&, void*)
{
}

void GamesFaultHandler(int, siginfo_t*, void*)
{
}

TEST(the_core_library_hook_puts_the_fault_handler_in_front_again)
{
    CHECK(AddFatalFaultObserver(ObserveNothing, nullptr));
    struct sigaction games{}, ours{}, now{};
    games.sa_sigaction = GamesFaultHandler;
    games.sa_flags = SA_SIGINFO;
    CHECK(sigaction(SIGFPE, &games, &ours) == 0);
    CHECK(sigaction(SIGFPE, nullptr, &now) == 0 && now.sa_sigaction == GamesFaultHandler);

    int a = 0;
    auto const& init = Hook("CoreLibSDM::Init");
    SetGameHookForTest(init.Entry, reinterpret_cast<void*>(&bt_record));
    bt_calls = 0;
    Call(init.Detour, { U(&a) });
    CHECK(bt_calls == 1);
    CHECK(sigaction(SIGFPE, nullptr, &now) == 0 && now.sa_sigaction == ours.sa_sigaction && now.sa_sigaction != GamesFaultHandler);
}

// The game's exit: the handler runs, then the game's ls::StaticDataManager::Shutdown
// or App::Terminate.
void ExitHandler(GameExit how)
{
    gOrder.push_back(how == GameExit::Quit ? "quit" : "exit");
    CHECK(bt_calls == 0);                   // before the game's function
}

TEST(the_exit_handler_runs_before_the_static_data_is_shut_down)
{
    auto const& shutdown = Hook("ls::StaticDataManager::Shutdown");
    SetGameHookForTest(shutdown.Entry, reinterpret_cast<void*>(&bt_record));

    // Without a handler only the game's function runs.
    gOrder.clear();
    bt_calls = 0;
    Call(shutdown.Detour, {});
    CHECK(bt_calls == 1 && gOrder.empty());

    SetGameExitHandler(&ExitHandler);
    bt_calls = 0;
    Call(shutdown.Detour, {});
    CHECK(bt_calls == 1 && gOrder.size() == 1 && gOrder[0] == "exit");
    SetGameExitHandler(nullptr);
}

TEST(the_exit_handler_runs_before_the_game_terminates_when_the_player_quits)
{
    auto const& terminate = Hook("App::Terminate");
    SetGameHookForTest(terminate.Entry, reinterpret_cast<void*>(&bt_record));
    int app = 0;

    gOrder.clear();
    bt_calls = 0;
    Call(terminate.Detour, { U(&app), 0 });
    CHECK(bt_calls == 1 && gOrder.empty());

    SetGameExitHandler(&ExitHandler);
    bt_calls = 0;
    Call(terminate.Detour, { U(&app), 0 });
    CHECK(bt_calls == 1 && gOrder.size() == 1 && gOrder[0] == "quit");
    SetGameExitHandler(nullptr);
}

TEST(client_update_handlers_run_at_their_two_places)
{
    int client = 0, world = 0;
    auto const& pre = Hook("ecl::GameClient::PreUpdate");
    auto const& post = Hook("App::Update: call of ecs::EntityWorld::Update");
    SetGameHookForTest(pre.Entry, reinterpret_cast<void*>(&bt_record));
    SetGameHookForTest(post.Entry, reinterpret_cast<void*>(&bt_record));

    // Without handlers both places only run the game's code.
    gOrder.clear();
    bt_calls = 0;
    Call(pre.Detour, { U(&client), U(&gFailures) });
    CHECK(bt_calls == 1 && bt_seen[0] == U(&client) && bt_seen[1] == U(&gFailures));
    Call(post.Detour, { U(&world), U(&gFailures) });
    CHECK(bt_calls == 2 && bt_seen[0] == U(&world) && bt_seen[1] == U(&gFailures) && gOrder.empty());

    SetClientUpdateHandlers(&gOrder, &PreHandler, &PostHandler);
    bt_calls = 0;
    Call(pre.Detour, { U(&client), U(&gFailures) });        // the client's pre-update, then Norbyte's pre-hook
    CHECK(bt_calls == 1 && gOrder.size() == 1 && gOrder[0] == "pre");
    bt_calls = 0;
    Call(post.Detour, { U(&world), U(&gFailures) });        // Norbyte's post-hook, then the world's update
    CHECK(bt_calls == 1 && bt_seen[0] == U(&world) && gOrder.size() == 2 && gOrder[1] == "post");
    SetClientUpdateHandlers(nullptr, nullptr, nullptr);
}

// Norbyte's side of a bridge, as his WrappableFunction makes it: a detour of
// his prototype that runs a handler and calls the original through the
// trampoline the Detours layer gave.
void (*gSettingsOriginal)(void*) = nullptr;
bool gSettingsCallOriginal = true;
void SettingsDetour(void* app)
{
    gOrder.push_back("handler");
    CHECK(app == &gFailures);
    if (gSettingsCallOriginal) gSettingsOriginal(app);
    gOrder.push_back("after");
}

bool (*gParseOriginal)(void*) = nullptr;
int gParseHandlerRuns = 0;
bool ParseDetour(void* stats)
{
    gParseHandlerRuns++;
    CHECK(stats == &gFailures);
    CHECK(bt_calls == 0);                   // before the first buffer is parsed
    return gParseOriginal(stats);
}

// RPGStats::Start as far as the hooks see it: three buffers, the call site
// run for each.
void FakeStatsStart(void* stats, void const*)
{
    auto const& parse = Hook("RPGStats::Start: call of RPGStats::ParseDataBuffer");
    auto site = reinterpret_cast<void(*)(void*, void*, void*)>(parse.Detour);
    int result = 0, buffer = 0;
    for (int i = 0; i < 3; i++) site(&result, stats, &buffer);
}
void FakeStatsStartWithoutBuffers(void*, void const*) {}

TEST(a_wrapped_bridge_runs_norbytes_handler_and_the_game_function)
{
    // Every bridge takes a detour through the Detours layer in this build.
    {
        std::vector<void*> pointers;
        for (auto const& bridge : HookBridges()) pointers.push_back(bridge.Bridge);
        CHECK(DetourTransactionBegin() == NO_ERROR);
        for (auto& p : pointers) CHECK(DetourAttach(&p, reinterpret_cast<void*>(&bt_record)) == NO_ERROR);
        CHECK(DetourTransactionCommit() == NO_ERROR);
        CHECK(DetourTransactionBegin() == NO_ERROR);
        for (auto& p : pointers) CHECK(DetourDetach(&p, reinterpret_cast<void*>(&bt_record)) == NO_ERROR);
        CHECK(DetourTransactionCommit() == NO_ERROR);
    }

    // App__LoadGraphicSettings: the handler sees Norbyte's argument, the game
    // function its two, and the game's caller the game function's result.
    auto settingsBridge = FindHookBridge("App__LoadGraphicSettings");
    CHECK(settingsBridge != nullptr);
    auto const& settings = Hook("App__LoadGraphicSettings");
    SetGameHookForTest(settings.Entry, reinterpret_cast<void*>(&bt_record));
    gSettingsOriginal = reinterpret_cast<void(*)(void*)>(settingsBridge->Bridge);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(reinterpret_cast<void**>(&gSettingsOriginal), reinterpret_cast<void*>(&SettingsDetour)) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);

    int init = 0;
    gOrder.clear();
    bt_calls = 0;
    gSettingsCallOriginal = true;
    auto r = Call(settings.Detour, { U(&gFailures), U(&init) }, 0);
    CHECK(gOrder.size() == 2 && gOrder[0] == "handler" && gOrder[1] == "after");
    CHECK(bt_calls == 1 && bt_seen[0] == U(&gFailures) && bt_seen[1] == U(&init));
    CHECK((r.X0 & 0xff) == 0);
    // A wrapper that does not call the original: the game function does not
    // run, and its caller gets the function's one result in this game.
    gSettingsCallOriginal = false;
    bt_calls = 0;
    r = Call(settings.Detour, { U(&gFailures), U(&init) }, 0);
    CHECK(bt_calls == 0 && (r.X0 & 0xff) == 1);

    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(reinterpret_cast<void**>(&gSettingsOriginal), reinterpret_cast<void*>(&SettingsDetour)) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);

    // RPGStats__ParseDataBuffers: once for each RPGStats::Start, before its first buffer.
    auto parseBridge = FindHookBridge("RPGStats__ParseDataBuffers");
    CHECK(parseBridge != nullptr);
    SetGameHookForTest("RPGStats::Start: call of RPGStats::ParseDataBuffer", reinterpret_cast<void*>(&bt_record));
    SetGameHookForTest("RPGStats__Load", reinterpret_cast<void*>(&FakeStatsStart));
    gParseOriginal = reinterpret_cast<bool(*)(void*)>(parseBridge->Bridge);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(reinterpret_cast<void**>(&gParseOriginal), reinterpret_cast<void*>(&ParseDetour)) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);

    auto const& start = Hook("RPGStats__Load");
    gParseHandlerRuns = 0;
    bt_calls = 0;
    Call(start.Detour, { U(&gFailures), 0 });
    CHECK(gParseHandlerRuns == 1 && bt_calls == 3);
    bt_calls = 0;
    Call(start.Detour, { U(&gFailures), 0 });
    CHECK(gParseHandlerRuns == 2 && bt_calls == 3);
    // A load without buffers runs no handler, and leaves nothing behind for
    // a parse call that is not part of a load.
    SetGameHookForTest("RPGStats__Load", reinterpret_cast<void*>(&FakeStatsStartWithoutBuffers));
    Call(start.Detour, { U(&gFailures), 0 });
    CHECK(gParseHandlerRuns == 2);
    bt_calls = 0;
    int result = 0, buffer = 0;
    Call(Hook("RPGStats::Start: call of RPGStats::ParseDataBuffer").Detour, { U(&result), U(&gFailures), U(&buffer) });
    CHECK(gParseHandlerRuns == 2 && bt_calls == 1);

    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(reinterpret_cast<void**>(&gParseOriginal), reinterpret_cast<void*>(&ParseDetour)) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
}

// --- call sites ----------------------------------------------------------------

int Replacement(int value) { return bt_callee(value) + 1000; }

TEST(redirect_call_changes_one_call_and_refuses_anything_else)
{
    CHECK(bt_host(1) == 102);
    // Not a call; a call of another function.
    CHECK(Contains(RedirectCall(bt_host_not_a_call, reinterpret_cast<void*>(&bt_callee), reinterpret_cast<void*>(&Replacement)), "not a call"));
    CHECK(Contains(RedirectCall(bt_host_call, reinterpret_cast<void*>(&bt_other_callee), reinterpret_cast<void*>(&Replacement)), "does not go to"));
    CHECK(bt_host(1) == 102);
    auto problem = RedirectCall(bt_host_call, reinterpret_cast<void*>(&bt_callee), reinterpret_cast<void*>(&Replacement));
    if (!problem.empty()) printf("%s\n", problem.c_str());
    CHECK(problem.empty());
    CHECK(bt_host(1) == 1102);
    // The call no longer goes to the function: a second redirect is refused.
    CHECK(!RedirectCall(bt_host_call, reinterpret_cast<void*>(&bt_callee), reinterpret_cast<void*>(&Replacement)).empty());
}

// --- the template bank ---------------------------------------------------------

thread_local signed char tBankOfThread = 0;
void* BankOfThread(ThreadLocalDescriptor* descriptor)
{
    // The descriptor is the argument, as for the system's own.
    if (descriptor->Key != 0x1234) abort();
    return &tBankOfThread;
}

TEST(template_bank_index_is_the_global_or_the_threads)
{
    CHECK(CurrentTemplateBankIndex() == -1);            // not bound
    signed char bankType = 0;
    ThreadLocalDescriptor descriptor{ &BankOfThread, 0x1234, 0 };
    SetTemplateBankForTest(&bankType, nullptr);
    CHECK(CurrentTemplateBankIndex() == -1);            // one of the two is not enough
    SetTemplateBankForTest(&bankType, &descriptor);
    CHECK(CurrentTemplateBankIndex() == 0);
    bankType = 1;
    CHECK(CurrentTemplateBankIndex() == 1);
    bankType = kBankTypeOfThread;
    tBankOfThread = 1;
    CHECK(CurrentTemplateBankIndex() == 1);
    tBankOfThread = 0;
    CHECK(CurrentTemplateBankIndex() == 0);
    bankType = -1;
    CHECK(CurrentTemplateBankIndex() == -1);            // the game would index below its banks
    bankType = kBankTypeOfThread;
    tBankOfThread = -128;
    CHECK(CurrentTemplateBankIndex() == -1);
    SetTemplateBankForTest(nullptr, nullptr);

    // A game without the two entries leaves it unbound.
    FakeBinary binary;
    GameImage image;
    CHECK(image.OpenFile(binary.File));
    engine::Images images{ { &image, nullptr }, {} };
    auto resolution = engine::Resolve(engine::EngineTable(), images);
    SetTemplateBankForTest(&bankType, &descriptor);
    BindTemplateBank(resolution);
    CHECK(CurrentTemplateBankIndex() == -1);
}

// --- fail closed ---------------------------------------------------------------

// A resolution as the table expects it of a game, made without a game: every
// entry in its expected state, at an address of its own.
engine::Resolution ExpectedResolution()
{
    engine::Resolution resolution;
    uint64_t va = 0x100000000ull;
    for (auto const& area : engine::EngineTable()) {
        for (auto const& entry : area.Entries) {
            engine::Resolved r;
            r.Of = &entry;
            r.Area = area.Name;
            r.Is = r.Expected();
            if (r.Usable()) r.VA = r.SeenVA = (va += 0x1000);
            resolution.Entries.push_back(r);
        }
    }
    return resolution;
}

engine::Resolved& EntryOf(engine::Resolution& resolution, char const* name)
{
    for (auto& r : resolution.Entries) {
        if (strcmp(r.Of->Name, name) == 0) return r;
    }
    printf("no entry %s\n", name);
    abort();
}

void Refuse(engine::Resolution& resolution, char const* name)
{
    auto& r = EntryOf(resolution, name);
    r.Is = engine::Status::Unresolved;
    r.VA = 0;
    r.Reason = "refused for the test";
}

// The members a plan leaves unserved, apart from those unavailable by their row.
std::set<std::string> Missing(BindReport const& report)
{
    std::set<std::string> missing;
    for (auto const& member : report.Members) {
        bool served = member.Value != nullptr || (member.Row->How == Bound::Handlers && member.Reason.empty());
        if (!served && member.Row->How != Bound::Unavailable) {
            missing.insert(member.Row->Member);
            CHECK(!member.Reason.empty());
        }
    }
    return missing;
}

using Names = std::set<std::string>;

TEST(one_refused_entry_takes_its_members_and_no_others)
{
    for (auto const& hook : GameHooks()) SetGameHookForTest(hook.Entry, nullptr);

    // As the table expects: everything that has a source is served.
    {
        auto resolution = ExpectedResolution();
        auto plan = PlanStaticSymbols(resolution, false);
        CHECK(Missing(plan).empty() && !plan.Failed && !plan.CriticalFailed);
        size_t unavailable = 0, handlers = 0;
        for (auto const& row : StaticSymbolRows()) {
            unavailable += row.How == Bound::Unavailable;
            handlers += row.How == Bound::Handlers;
        }
        CHECK(plan.BoundCount == StaticSymbolRows().size() - unavailable - handlers);
        CHECK(CheckResolution(resolution, {}).Ok());
        // With the hooks not on the game, the bridged members and the handlers are not served.
        auto unhooked = PlanStaticSymbols(resolution, true);
        Names bridged{ "ecl__GameStateMachine__Update" };
        for (auto const& row : StaticSymbolRows()) {
            if (row.How == Bound::HookBridge) bridged.insert(row.Member);
        }
        CHECK(Missing(unhooked) == bridged && unhooked.CriticalFailed);
    }
    // An optional global: that member, and no critical failure.
    {
        auto resolution = ExpectedResolution();
        Refuse(resolution, "ecl__gCursorControl");
        auto plan = PlanStaticSymbols(resolution, false);
        CHECK(Missing(plan) == Names{ "ecl__gCursorControl" } && plan.Failed && !plan.CriticalFailed);
        CHECK(CheckResolution(resolution, {}).Ok());
    }
    // A critical one: start-up refuses, naming it.
    {
        auto resolution = ExpectedResolution();
        Refuse(resolution, "ecl__EoCClient");
        auto plan = PlanStaticSymbols(resolution, false);
        CHECK(Missing(plan) == Names{ "ecl__EoCClient" } && plan.CriticalFailed);
        auto check = CheckResolution(resolution, {});
        CHECK(check.Refusal == StartRefusal::CriticalMember && Contains(check.Reason, "ecl__EoCClient"));
    }
    // A critical one the table gives and a layout keeps back: start-up refuses as well, with the layout's reason.
    {
        auto resolution = ExpectedResolution();
        LayoutsWithheld layouts;
        layouts.Members.push_back({ "ls__gGlobalStringTable", "the game's fixed-string table is not confirmed" });
        layouts.Members.push_back({ "ls__gGlobalResourceManager", "the game's resource manager is not confirmed" });
        auto check = CheckResolution(resolution, layouts);
        CHECK(check.Refusal == StartRefusal::CriticalMember && Contains(check.Reason, "ls__gGlobalStringTable"));
        CHECK(Contains(check.Reason, "fixed-string table is not confirmed") && !Contains(check.Reason, "ls__gGlobalResourceManager"));
        // An optional member kept back refuses nothing.
        layouts.Members.erase(layouts.Members.begin());
        CHECK(CheckResolution(resolution, layouts).Ok());
        // The stats object and the client object are critical too.
        layouts.Members.push_back({ "gRPGStats", "the game's stats object is not confirmed" });
        check = CheckResolution(resolution, layouts);
        CHECK(check.Refusal == StartRefusal::CriticalMember && Contains(check.Reason, "gRPGStats")
            && Contains(check.Reason, "stats object is not confirmed"));
        layouts.Members.back() = { "ecl__EoCClient", "the game's client object is not confirmed" };
        check = CheckResolution(resolution, layouts);
        CHECK(check.Refusal == StartRefusal::CriticalMember && Contains(check.Reason, "client object is not confirmed"));
        layouts.Members.pop_back();
        CHECK(CheckResolution(resolution, layouts).Ok());
        // A class whose virtual functions his code reaches everywhere is not where this build calls them.
        layouts.Refusing.push_back("the game's vtables are not confirmed to hold the virtual functions of esv::Status where this build calls them");
        check = CheckResolution(resolution, layouts);
        CHECK(check.Refusal == StartRefusal::CriticalMember && Contains(check.Reason, "esv::Status"));
        layouts.Refusing.clear();
        // No image to prove a layout on: what this build ties to a layout is kept back, the string table with it.
        check = CheckResolution(resolution, LayoutsWithheldOn(resolution));
        CHECK(check.Refusal == StartRefusal::CriticalMember && Contains(check.Reason, "ls__GlobalStringTable__MainTable__DecRef"));
    }
    // A function Norbyte detours that would not take an entry hook is not handed to him.
    {
        auto resolution = ExpectedResolution();
        EntryOf(resolution, "ecs__EntityWorld__FlushECBs").Hook = HookRefusal::BranchesToItsEntry;
        EntryOf(resolution, "ls__FileReader__ctor").Hook = HookRefusal::Call;     // called and detoured
        EntryOf(resolution, "ls__FileReader__dtor").Hook = HookRefusal::Call;     // only called: no matter
        auto plan = PlanStaticSymbols(resolution, false);
        CHECK(Missing(plan) == (Names{ "ecs__EntityWorld__FlushECBs", "ls__FileReader__ctor" }) && plan.CriticalFailed);
    }
    // The listener vtable a manager's row requires.
    {
        auto resolution = ExpectedResolution();
        Refuse(resolution, "ecl::GameStateEventListener");
        auto plan = PlanStaticSymbols(resolution, false);
        CHECK(Missing(plan) == Names{ "ecl__gGameStateEventManager" } && plan.CriticalFailed);
    }
    // The allocator trio stands or falls with the entry that says where the game's allocation code is.
    {
        auto resolution = ExpectedResolution();
        Refuse(resolution, "ls__GlobalAllocator__Alloc");
        auto plan = PlanStaticSymbols(resolution, false);
        CHECK(Missing(plan) == (Names{ "ls__GlobalAllocator__Get", "ls__GlobalAllocator__Alloc", "ls__GlobalAllocator__Free" }));
    }
    // A bridge's game hook.
    {
        auto resolution = ExpectedResolution();
        Refuse(resolution, "gui::GameUI::UpdateFromUI");
        CHECK(Missing(PlanStaticSymbols(resolution, false)) == Names{ "ui__deferred__PredicatesManager__UpdateFromUI" });
        resolution = ExpectedResolution();
        EntryOf(resolution, "CoreLibSDM::Init").Hook = HookRefusal::Unrecognised;
        auto plan = PlanStaticSymbols(resolution, false);
        CHECK(Missing(plan) == Names{ "App__Ctor" } && plan.CriticalFailed);
        // The call site alone: the load itself is still bridged.
        resolution = ExpectedResolution();
        Refuse(resolution, "RPGStats::Start: call of RPGStats::ParseDataBuffer");
        CHECK(Missing(PlanStaticSymbols(resolution, false)) == Names{ "RPGStats__ParseDataBuffers" });
        // The function a call site calls: the client's handlers, and the member that is that function.
        resolution = ExpectedResolution();
        Refuse(resolution, "ecs__EntityWorld__Update");
        plan = PlanStaticSymbols(resolution, false);
        CHECK(Missing(plan) == (Names{ "ecs__EntityWorld__Update", "ecl__GameStateMachine__Update" }) && plan.CriticalFailed);
        resolution = ExpectedResolution();
        Refuse(resolution, "ecl::GameClient::PreUpdate");
        CHECK(Missing(PlanStaticSymbols(resolution, false)) == Names{ "ecl__GameStateMachine__Update" });
    }
    // The global an adapter reads.
    {
        auto resolution = ExpectedResolution();
        Refuse(resolution, "ls__gGlobalResourceManager");
        CHECK(Missing(PlanStaticSymbols(resolution, false)) == (Names{ "ls__gGlobalResourceManager", "ls__TextureManager__UnloadTexture" }));
    }
    // The stand-in a member is taken from.
    {
        auto resolution = ExpectedResolution();
        Refuse(resolution, "ServerWorker::DoWork");
        auto plan = PlanStaticSymbols(resolution, false);
        CHECK(Missing(plan) == Names{ "esv__GameStateThreaded__GameStateWorker__DoWork" } && plan.CriticalFailed);
    }
}

// --- the story library ---------------------------------------------------------

namespace names = engine::osiris;

TEST(story_rows_hold_together_with_the_engine_table)
{
    auto findings = CheckOsirisRows(engine::EngineTable());
    if (!findings.empty()) printf("%s", findings.c_str());
    CHECK(findings.empty());

    // One row for each pointer of the two structures, each member once.
    CHECK(OsirisGlobalRows().size() * sizeof(void*) == OsirisGlobalsSize());
    CHECK(OsirisHookRows().size() * sizeof(void*) == sizeof(OsirisHookTargets));
    std::set<std::string> members;
    for (auto const& row : OsirisGlobalRows()) CHECK(members.insert(row.Member).second);
    for (auto const& row : OsirisHookRows()) CHECK(members.insert(row.Member).second);
    // The story's rows are not StaticSymbols' rows.
    for (auto const& row : StaticSymbolRows()) CHECK(!members.count(row.Member));

    // Every slot of a node's vtable is where a call through Norbyte's class
    // Node looks for it, and the nine classes are the nine node types.
    CHECK(OsirisNodeSlots().size() == 8 && OsirisNodeClasses().size() == kNodeTypes - 1);
    for (auto const& slot : OsirisNodeSlots()) CHECK(slot.InNode >= 0 && static_cast<uint32_t>(slot.InNode) == slot.InNodeVMT && slot.InNodeVMT == slot.Expected);
    size_t queries = 0;
    for (auto const& row : OsirisNodeClasses()) queries += row.Query;
    CHECK(queries == 3);

    // The only functions handed over with an allowance are the two whose
    // difference is stated in the table.
    for (auto const& row : OsirisHookRows()) {
        bool allowed = row.Allow != kExactly;
        bool named = strcmp(row.Member, "Osiris.RegisterDivFunctions") == 0 || strcmp(row.Member, "Osiris.RuleActionCall") == 0;
        CHECK(allowed == named);
    }
}

TEST(norbytes_story_containers_read_and_build_the_librarys)
{
    // GameDefinitions/Osiris.h, compiled as Norbyte's sources are, on libc++'s
    // own list and map (Mac/Tests/StoryContainersNorbyte.cpp).
    auto findings = test::CheckStoryContainers();
    if (!findings.empty()) printf("%s", findings.c_str());
    CHECK(findings.empty());
    // A list built and walked through his types alone, as his sources are compiled.
    findings = test::CheckStoryListsBuilt();
    if (!findings.empty()) printf("%s", findings.c_str());
    CHECK(findings.empty());
}

TEST(adapter_osiris_add_string)
{
    // COsiStringTable::AddStr(char const*, bool) takes the table, the text and
    // the flag, and returns the handle; Norbyte's function writes it through
    // its second argument and returns that address.
    auto f = Adapter("Osiris.Globals.AddString", reinterpret_cast<void*>(&bt_record));
    int table = 0;
    uint64_t handle = 0;
    char const* text = "S_Player_Karlach";
    bt_calls = 0;
    auto r = Call(f, { U(&table), U(&handle), U(text), 1 }, 0x20000000012345ull);
    CHECK(bt_calls == 1 && bt_seen[0] == U(&table) && bt_seen[1] == U(text) && (bt_seen[2] & 0xff) == 1);
    CHECK(handle == 0x20000000012345ull && r.X0 == U(&handle));
    r = Call(f, { U(&table), U(&handle), U(text), 0 }, 7);
    CHECK((bt_seen[2] & 0xff) == 0 && handle == 7);
}

// What a test's detours of the two file bridges saw.
struct StoryFileCalls
{
    std::vector<std::wstring> Created;
    std::vector<uint32_t> Access;
    std::vector<void*> Handles, Closed;
};
StoryFileCalls gStoryFiles;
void* (*gCreateFileOriginal)(wchar_t const*, uint32_t, uint32_t, void*, uint32_t, uint32_t, void*);
int (*gCloseHandleOriginal)(void*);

void* CreateFileDetour(wchar_t const* path, uint32_t access, uint32_t share, void* security, uint32_t creation, uint32_t flags, void* templateFile)
{
    CHECK(bt_calls == 1);       // the game has written and closed the file
    gStoryFiles.Created.push_back(path);
    gStoryFiles.Access.push_back(access);
    CHECK(share == 0 && security == nullptr && creation == CREATE_ALWAYS && flags == FILE_ATTRIBUTE_NORMAL && templateFile == nullptr);
    void* handle = gCreateFileOriginal(path, access, share, security, creation, flags, templateFile);
    gStoryFiles.Handles.push_back(handle);
    return handle;
}
int CloseHandleDetour(void* handle)
{
    gStoryFiles.Closed.push_back(handle);
    return gCloseHandleOriginal(handle);
}

// An ls::Path as the game has it: sixteen bytes that hold the text, or a
// pointer to it and a negative last byte.
struct FakePath
{
    char Bytes[16]{};
    void Inline(char const* text) { memset(Bytes, 0, sizeof(Bytes)); strcpy(Bytes, text); }
    void Long(char const* text) { memset(Bytes, 0, sizeof(Bytes)); memcpy(Bytes, &text, sizeof(text)); Bytes[kGamePathTextFlag] = static_cast<char>(0x80); }
};

TEST(story_hooks_and_bridges)
{
    // --- COsiris::OpenLogFile: three arguments in, the game function's answer out.
    auto const& open = Hook("Osiris.OpenLogFile");
    SetGameHookForTest(open.Entry, reinterpret_cast<void*>(&bt_record));
    int osiris = 0;
    wchar_t const* log = L"/tmp/osiris.log";
    bt_calls = 0;
    auto r = Call(open.Detour, { U(&osiris), U(log), 0x11 }, 1);
    CHECK(bt_calls == 1 && bt_seen[0] == U(&osiris) && bt_seen[1] == U(log) && (bt_seen[2] & 0xffffffff) == 0x11);
    CHECK((r.X0 & 0xff) == 1);
    r = Call(open.Detour, { U(&osiris), U(log), 0x11 }, 0);
    CHECK((r.X0 & 0xff) == 0);
    // Norbyte's own call of the original reaches the game function too.
    auto openBridge = FindHookBridge("Osiris.OpenLogFile");
    CHECK(openBridge != nullptr);
    bt_calls = 0;
    Call(openBridge->Bridge, { U(&osiris), U(log), 0x01 }, 1);
    CHECK(bt_calls == 1 && bt_seen[1] == U(log) && (bt_seen[2] & 0xffffffff) == 0x01);
    // A wrapper that does not call the original: the file is not open.
    void* wrapped = openBridge->Bridge;
    auto silent = reinterpret_cast<void*>(+[](void*, wchar_t const*, uint32_t) {});
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&wrapped, silent) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    bt_calls = 0;
    r = Call(open.Detour, { U(&osiris), U(log), 0x11 }, 1);
    CHECK(bt_calls == 0 && (r.X0 & 0xff) == 0);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(&wrapped, silent) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);

    // --- the text of a path.
    FakePath path;
    path.Inline("Story/a.div");
    CHECK(GamePathText(&path) == path.Bytes);
    char const* headerText = "/Users/someone/Documents/Larian Studios/Baldur's Gate 3/Story/story_header.div";
    path.Long(headerText);
    CHECK(GamePathText(&path) == headerText);

    // --- the story files. Unwrapped, the hooks pass the game's call on and
    // the bridges do nothing.
    auto const& header = Hook(names::kWriteHeader);
    auto const& close = Hook(names::kCloseStory);
    SetGameHookForTest(header.Entry, reinterpret_cast<void*>(&bt_record));
    SetGameHookForTest(close.Entry, reinterpret_cast<void*>(&bt_record));
    int manager = 0;
    bt_calls = 0;
    r = Call(header.Detour, { U(&manager), U(&path) }, 1);
    CHECK(bt_calls == 1 && bt_seen[0] == U(&manager) && bt_seen[1] == U(&path) && (r.X0 & 0xff) == 1);

    // Wrapped as Norbyte wraps CreateFileW and CloseHandle.
    auto createBridge = FindHookBridge("Osiris.CreateFileW");
    auto closeBridge = FindHookBridge("Osiris.CloseHandle");
    CHECK(createBridge && closeBridge);
    gCreateFileOriginal = reinterpret_cast<decltype(gCreateFileOriginal)>(createBridge->Bridge);
    gCloseHandleOriginal = reinterpret_cast<decltype(gCloseHandleOriginal)>(closeBridge->Bridge);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(reinterpret_cast<void**>(&gCreateFileOriginal), reinterpret_cast<void*>(&CreateFileDetour)) == NO_ERROR);
    CHECK(DetourAttach(reinterpret_cast<void**>(&gCloseHandleOriginal), reinterpret_cast<void*>(&CloseHandleDetour)) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);

    // The header: after the game has written it, one create for writing with
    // its path, and one close of the handle that create returned.
    gStoryFiles = {};
    bt_calls = 0;
    r = Call(header.Detour, { U(&manager), U(&path) }, 1);
    CHECK(bt_calls == 1 && (r.X0 & 0xff) == 1);
    CHECK(gStoryFiles.Created.size() == 1 && gStoryFiles.Closed.size() == 1);
    if (gStoryFiles.Created.size() == 1 && gStoryFiles.Closed.size() == 1) {
        CHECK(gStoryFiles.Created[0] == L"/Users/someone/Documents/Larian Studios/Baldur's Gate 3/Story/story_header.div");
        CHECK((gStoryFiles.Access[0] & GENERIC_WRITE) != 0);
        CHECK(gStoryFiles.Handles[0] != nullptr && gStoryFiles.Handles[0] != INVALID_HANDLE_VALUE && gStoryFiles.Closed[0] == gStoryFiles.Handles[0]);
    }
    // A header the game could not open is not reported.
    gStoryFiles = {};
    bt_calls = 0;
    r = Call(header.Detour, { U(&manager), U(&path) }, 0);
    CHECK(bt_calls == 1 && (r.X0 & 0xff) == 0 && gStoryFiles.Created.empty() && gStoryFiles.Closed.empty());
    // Text beyond ASCII arrives as the characters it is; text that is not
    // UTF-8 is not reported.
    path.Long("/Users/J\xc3\xb6rg/Story/\xf0\x9f\x8e\xb2/story_header.div");
    gStoryFiles = {};
    bt_calls = 0;
    Call(header.Detour, { U(&manager), U(&path) }, 1);
    CHECK(gStoryFiles.Created.size() == 1 && gStoryFiles.Created[0] == L"/Users/Jörg/Story/\U0001f3b2/story_header.div");
    for (char const* broken : { "/Story/\xff.div", "/Story/\xc3", "/Story/\xed\xa0\x80.div", "/Story/\xc0\xaf.div" }) {
        path.Long(broken);
        gStoryFiles = {};
        bt_calls = 0;
        Call(header.Detour, { U(&manager), U(&path) }, 1);
        CHECK(bt_calls == 1 && gStoryFiles.Created.empty());
    }

    // The generated story: the writer is closed, then its path, which lies
    // at the offset the binding was given from the writer, is reported.
    struct { char Writer[0x68]; FakePath Path; } frame{};
    char const* storyText = "/Users/someone/Documents/Larian Studios/Baldur's Gate 3/Story/story_generated.div";
    frame.Path.Long(storyText);
    SetStoryPathFromWriter(0);
    gStoryFiles = {};
    bt_calls = 0;
    Call(close.Detour, { U(&frame.Writer) });
    CHECK(bt_calls == 1 && bt_seen[0] == U(&frame.Writer) && gStoryFiles.Created.empty());     // not known: nothing is read
    SetStoryPathFromWriter(reinterpret_cast<char*>(&frame.Path) - frame.Writer);
    bt_calls = 0;
    Call(close.Detour, { U(&frame.Writer) });
    CHECK(bt_calls == 1 && bt_seen[0] == U(&frame.Writer));
    CHECK(gStoryFiles.Created.size() == 1 && gStoryFiles.Closed.size() == 1);
    if (gStoryFiles.Created.size() == 1 && gStoryFiles.Closed.size() == 1) {
        CHECK(gStoryFiles.Created[0] == L"/Users/someone/Documents/Larian Studios/Baldur's Gate 3/Story/story_generated.div");
        CHECK(gStoryFiles.Closed[0] == gStoryFiles.Handles[0] && gStoryFiles.Handles[0] != nullptr);
    }
    SetStoryPathFromWriter(0);

    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(reinterpret_cast<void**>(&gCreateFileOriginal), reinterpret_cast<void*>(&CreateFileDetour)) == NO_ERROR);
    CHECK(DetourDetach(reinterpret_cast<void**>(&gCloseHandleOriginal), reinterpret_cast<void*>(&CloseHandleDetour)) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
}

int FirstSlot() { return 1; }
int SecondSlot() { return 2; }
int ThirdSlot() { return 3; }
int FirstWrapper() { return 10; }
int SecondWrapper() { return 20; }

TEST(vtable_slots_are_wrapped_one_by_one_all_or_none)
{
    // A vtable as the library has it: read-only data.
    size_t pageSize = static_cast<size_t>(getpagesize());
    auto vtable = static_cast<void**>(mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    CHECK(vtable != MAP_FAILED);
    vtable[0] = reinterpret_cast<void*>(&FirstSlot);
    vtable[1] = reinterpret_cast<void*>(&SecondSlot);
    vtable[2] = reinterpret_cast<void*>(&ThirdSlot);
    CHECK(mprotect(vtable, pageSize, PROT_READ) == 0);

    void* kept[2]{};
    VtableSlotWrap slots[] = {
        { &vtable[0], reinterpret_cast<void*>(&FirstWrapper), &kept[0] },
        { &vtable[2], reinterpret_cast<void*>(&SecondWrapper), &kept[1] },
    };
    CHECK(WrapVtableSlots(slots).empty());
    CHECK(vtable[0] == reinterpret_cast<void*>(&FirstWrapper) && vtable[1] == reinterpret_cast<void*>(&SecondSlot)
        && vtable[2] == reinterpret_cast<void*>(&SecondWrapper));
    CHECK(kept[0] == reinterpret_cast<void*>(&FirstSlot) && kept[1] == reinterpret_cast<void*>(&ThirdSlot));
    // The page is read-only again.
    CHECK(mprotect(vtable, pageSize, PROT_READ) == 0);
    CHECK(UnwrapVtableSlots(slots).empty());
    CHECK(vtable[0] == reinterpret_cast<void*>(&FirstSlot) && vtable[2] == reinterpret_cast<void*>(&ThirdSlot));
    // Unwrapping what is not wrapped changes nothing.
    CHECK(UnwrapVtableSlots(slots).empty() && vtable[0] == reinterpret_cast<void*>(&FirstSlot));

    // Slots that stayed wrapped (their wrapper went away without unwrapping
    // them) and are wrapped again: what is kept is the game's function, never
    // the replacement that is still in the slot.
    CHECK(WrapVtableSlots(slots).empty());
    void* keptAgain[2]{};
    VtableSlotWrap again[] = {
        { &vtable[0], reinterpret_cast<void*>(&FirstWrapper), &keptAgain[0] },
        { &vtable[2], reinterpret_cast<void*>(&SecondWrapper), &keptAgain[1] },
    };
    CHECK(WrapVtableSlots(again).empty());
    CHECK(keptAgain[0] == reinterpret_cast<void*>(&FirstSlot) && keptAgain[1] == reinterpret_cast<void*>(&ThirdSlot));
    CHECK(vtable[0] == reinterpret_cast<void*>(&FirstWrapper) && vtable[2] == reinterpret_cast<void*>(&SecondWrapper));
    CHECK(UnwrapVtableSlots(again).empty());
    CHECK(vtable[0] == reinterpret_cast<void*>(&FirstSlot) && vtable[2] == reinterpret_cast<void*>(&ThirdSlot));
    // And wrapped afresh after that, the slots are read anew.
    keptAgain[0] = keptAgain[1] = nullptr;
    CHECK(WrapVtableSlots(again).empty() && keptAgain[0] == reinterpret_cast<void*>(&FirstSlot));
    CHECK(UnwrapVtableSlots(again).empty() && vtable[0] == reinterpret_cast<void*>(&FirstSlot));

    // A slot that cannot be stored: the one replaced before it gets its
    // function back, and the reason is returned.
    void* elsewhere[2]{};
    void* keptElsewhere = nullptr;
    VtableSlotWrap refused[] = {
        { &vtable[0], reinterpret_cast<void*>(&FirstWrapper), &kept[0] },
        { reinterpret_cast<void**>(reinterpret_cast<char*>(&elsewhere[0]) + 1), reinterpret_cast<void*>(&SecondWrapper), &keptElsewhere },
    };
    kept[0] = nullptr;
    auto problem = WrapVtableSlots(refused);
    CHECK(!problem.empty() && Contains(problem, "aligned"));
    CHECK(vtable[0] == reinterpret_cast<void*>(&FirstSlot) && elsewhere[0] == nullptr && elsewhere[1] == nullptr);
    // Code is not a vtable.
    VtableSlotWrap code[] = { { reinterpret_cast<void**>(reinterpret_cast<void*>(&bt_record)), reinterpret_cast<void*>(&FirstWrapper), &keptElsewhere } };
    CHECK(!WrapVtableSlots(code).empty());
    munmap(vtable, pageSize);
}

// Every vtable entry of the resolution found whole: each method in its slot.
void WithVtableSlots(engine::Resolution& resolution)
{
    for (auto& r : resolution.Entries) {
        if (r.Of->What != engine::Kind::Vtable) continue;
        r.Slots.clear();
        for (auto const& method : r.Of->Methods) r.Slots.push_back({ &method, r.VA + 0x10000 + method.Offset, method.Offset });
    }
}

Names MissingOf(std::vector<MemberResult> const& members)
{
    Names missing;
    for (auto const& member : members) {
        if (!member.Value) {
            missing.insert(member.Row->Member);
            CHECK(!member.Reason.empty());
        }
    }
    return missing;
}

TEST(the_story_layer_goes_without_only_the_hooks_his_code_can_do_without)
{
    for (auto const& hook : GameHooks()) SetGameHookForTest(hook.Entry, nullptr);
    Names const bridged{ "Osiris.OpenLogFile", "Osiris.CreateFileW", "Osiris.CloseHandle" };

    // As the table expects, but with no library whose code could prove the
    // shapes: every row and class is decided, and the gate stays shut.
    {
        auto resolution = ExpectedResolution();
        WithVtableSlots(resolution);
        auto plan = PlanOsiris(resolution, false);
        CHECK(MissingOf(plan.Globals).empty() && MissingOf(plan.Hooks).empty());
        CHECK(plan.Globals.size() == OsirisGlobalRows().size() && plan.Hooks.size() == OsirisHookRows().size());
        CHECK(plan.NodeHooks && plan.Classes.size() == kNodeTypes - 1);
        for (auto const& cls : plan.Classes) CHECK(cls.Vtable != nullptr && cls.Reason.empty());
        CHECK(!plan.Available && Contains(plan.Reason, "libOsiris.dylib is not loaded"));
        CHECK(plan.Reason.find('\n') == plan.Reason.size() - 1);       // and that is the one reason
        // With this layer's hooks not on the game, the bridged functions are
        // not handed over, and none of the three is critical.
        auto unhooked = PlanOsiris(resolution, true);
        CHECK(MissingOf(unhooked.Hooks) == bridged && MissingOf(unhooked.Globals).empty());
        CHECK(!Contains(unhooked.Reason, "Osiris.OpenLogFile") && !Contains(unhooked.Reason, "Osiris.CreateFileW")
            && !Contains(unhooked.Reason, "Osiris.CloseHandle"));
    }
    // A library that is not the one that was read: no shape is confirmed.
    {
        FakeBinary binary;
        GameImage library;
        CHECK(library.OpenFile(binary.File));
        auto shapes = osiris::CheckOsirisShapes(library);
        CHECK(!shapes.Proofs.empty() && !shapes.AllConfirmed() && !shapes.FirstProblem().empty());
        for (uint8_t n = 0; n < (uint8_t)osiris::Shape::Count; n++) CHECK(!shapes.Confirmed((osiris::Shape)n));
        for (auto const& proof : shapes.Proofs) CHECK(!proof.Found && !proof.Problem.empty());

        auto resolution = ExpectedResolution();
        WithVtableSlots(resolution);
        resolution.In.Osiris.Symbols = &library;
        auto plan = PlanOsiris(resolution, false);
        CHECK(!plan.Available && Contains(plan.Reason, "a shape of the story library is not confirmed"));
        CHECK(!Contains(plan.Reason, "is not loaded"));
    }
    // One global, one function, the block of functions the game registers,
    // the game's packing of a function's handle: each is named, and the gate
    // is shut for all.
    for (char const* refused : { "Osiris.Globals.Nodes", "Osiris.RegisterDivFunctions", "Osiris.Globals.AddString", "Osiris.DivFunctions",
             "Osiris.Globals.StringTable", names::kGetHandle }) {
        auto resolution = ExpectedResolution();
        WithVtableSlots(resolution);
        Refuse(resolution, refused);
        auto plan = PlanOsiris(resolution, false);
        CHECK(!plan.Available && Contains(plan.Reason, refused));
    }
    // A hook whose original his code calls only through it: as a missing
    // export on Windows, it is left out alone and the gate is not shut by it.
    for (char const* refused : { "Osiris.InitGame", "Osiris.DeleteAllData", "Osiris.GetFunctionMappings", "Osiris.CloseLogFile", "Osiris.Load",
             "Osiris.Compile", "Osiris.Merge", "Osiris.Event", "Osiris.RuleActionCall" }) {
        auto resolution = ExpectedResolution();
        WithVtableSlots(resolution);
        Refuse(resolution, refused);
        auto plan = PlanOsiris(resolution, false);
        CHECK(MissingOf(plan.Hooks) == Names{ refused } && MissingOf(plan.Globals).empty() && plan.NodeHooks);
        CHECK(!Contains(plan.Reason, refused) && Contains(plan.Reason, "libOsiris.dylib is not loaded"));
        CHECK(plan.Reason.find('\n') == plan.Reason.size() - 1);
    }
    // The one that hands him the story shuts it.
    {
        auto resolution = ExpectedResolution();
        WithVtableSlots(resolution);
        Refuse(resolution, "Osiris.RegisterDivFunctions");
        auto plan = PlanOsiris(resolution, false);
        CHECK(MissingOf(plan.Hooks) == Names{ "Osiris.RegisterDivFunctions" } && Contains(plan.Reason, "Osiris.RegisterDivFunctions"));
    }
    {
        // The string table is what the four string functions need.
        auto resolution = ExpectedResolution();
        Refuse(resolution, "Osiris.Globals.StringTable");
        auto plan = PlanOsiris(resolution, false);
        CHECK(MissingOf(plan.Globals) == (Names{ "Osiris.Globals.StringTable", "Osiris.Globals.GetString", "Osiris.Globals.AddString",
            "Osiris.Globals.AddStringRef", "Osiris.Globals.RemoveString" }));
    }
    // A function that would not take an entry hook is not handed to Norbyte.
    {
        auto resolution = ExpectedResolution();
        EntryOf(resolution, "Osiris.RegisterDivFunctions").Hook = HookRefusal::BranchesToItsEntry;
        EntryOf(resolution, "Osiris.Globals.GetString").Hook = HookRefusal::Call;      // only called: no matter
        auto plan = PlanOsiris(resolution, false);
        CHECK(MissingOf(plan.Hooks) == Names{ "Osiris.RegisterDivFunctions" } && MissingOf(plan.Globals).empty());
        CHECK(Contains(plan.Reason, "Osiris.RegisterDivFunctions"));
    }
    // The places that stand for the Windows file functions: without either,
    // the two bridges are not handed over, and nothing else is lost.
    for (char const* refused : { names::kWriteHeader, names::kCloseStory, names::kWriterClose }) {
        auto resolution = ExpectedResolution();
        Refuse(resolution, refused);
        if (strcmp(refused, names::kWriterClose) == 0) Refuse(resolution, names::kCloseStory);     // the call site needs it
        auto plan = PlanOsiris(resolution, false);
        CHECK(MissingOf(plan.Hooks) == (Names{ "Osiris.CreateFileW", "Osiris.CloseHandle" }));
        CHECK(!Contains(plan.Reason, "Osiris.CreateFileW") && !Contains(plan.Reason, names::kWriteHeader));
    }
    // The story's path is known only from a call site whose operands were
    // read, and read only as the header's hook is proven to read a path.
    {
        auto resolution = ExpectedResolution();
        CHECK(PlanOsiris(resolution, false).StoryPathFromWriter == 0);     // nothing was read: no offset is assumed
        auto& site = EntryOf(resolution, names::kCloseStory);
        for (auto const& operand : site.Of->Operands) site.Operands.push_back(operand.Expected);
        CHECK(PlanOsiris(resolution, false).StoryPathFromWriter == 0);     // how a path's text is read was not
        auto& header = EntryOf(resolution, names::kWriteHeader);
        for (auto const& operand : header.Of->Operands) header.Operands.push_back(operand.Expected);
        CHECK(PlanOsiris(resolution, false).StoryPathFromWriter == 0x68);
        {
            auto refused = resolution;
            Refuse(refused, names::kWriteHeader);
            CHECK(PlanOsiris(refused, false).StoryPathFromWriter == 0);
        }
        // The writer closed is not the writer constructed.
        for (size_t n = 0; n < site.Of->Operands.size(); n++) {
            if (strcmp(site.Of->Operands[n].What, names::kWriterClosed) == 0) site.Operands[n] += 8;
        }
        CHECK(PlanOsiris(resolution, false).StoryPathFromWriter == 0);
    }
}

TEST(a_node_class_that_is_not_proven_takes_the_node_hooks_and_nothing_else)
{
    // Refused, a slot not found, a slot at another offset, a query slot
    // missing from a query class: no vtable is handed over, and the rows are
    // decided as before.
    for (int kind = 0; kind < 4; kind++) {
        auto resolution = ExpectedResolution();
        WithVtableSlots(resolution);
        char const* which = kind == 3 ? "Osiris.NodeVMT.UserQuery" : "Osiris.NodeVMT.RelOp";
        auto& entry = EntryOf(resolution, which);
        if (kind == 0) Refuse(resolution, which);
        if (kind == 1) entry.Slots[2].Offset = -1;
        if (kind == 2) entry.Slots[4].Offset += 8;
        if (kind == 3) entry.Slots.pop_back();
        auto plan = PlanOsiris(resolution, false);
        CHECK(!plan.NodeHooks);
        size_t handed = 0;
        for (auto const& cls : plan.Classes) {
            bool refused = strcmp(cls.Row->Entry, which) == 0;
            CHECK((cls.Vtable == nullptr) == refused && cls.Reason.empty() != refused);
            handed += cls.Vtable != nullptr;
        }
        CHECK(handed == kNodeTypes - 2);
        CHECK(MissingOf(plan.Globals).empty() && MissingOf(plan.Hooks).empty());
        CHECK(!Contains(plan.Reason, "NodeVMT"));       // the node hooks are not what the gate asks for
    }
    // A class that is not a query class is not asked for the query slots.
    {
        auto resolution = ExpectedResolution();
        WithVtableSlots(resolution);
        CHECK(EntryOf(resolution, "Osiris.NodeVMT.And").Slots.size() == 6 && EntryOf(resolution, "Osiris.NodeVMT.DivQuery").Slots.size() == 8);
        CHECK(PlanOsiris(resolution, false).NodeHooks);
    }
}

TEST(without_a_resolved_game_the_story_layer_gets_nothing)
{
    // No start-up sequence has run: nothing is filled.
    SetResolutionForLibraries(nullptr);
    std::vector<char> globals(OsirisGlobalsSize(), 0);
    auto bound = BindOsiris(globals.data());
    CHECK(!bound.Available);
    for (char byte : globals) CHECK(byte == 0);

    // The test binary has none of the game's symbols.
    FakeBinary binary;
    GameImage image;
    CHECK(image.OpenFile(binary.File));
    engine::Images images{ { &image, nullptr }, { &image, nullptr } };
    auto resolution = engine::Resolve(engine::EngineTable(), images);
    for (bool attached : { false, true }) {
        auto plan = PlanOsiris(resolution, attached);
        CHECK(!plan.Available && !plan.NodeHooks && plan.StoryPathFromWriter == 0);
        CHECK(MissingOf(plan.Globals).size() == OsirisGlobalRows().size() && MissingOf(plan.Hooks).size() == OsirisHookRows().size());
        for (auto const& cls : plan.Classes) CHECK(cls.Vtable == nullptr && !cls.Reason.empty());
    }
    SetResolutionForLibraries(&resolution);
    auto adapter = FindCallAdapter("Osiris.Globals.AddString");
    CHECK(adapter && adapter->Target);
    *adapter->Target = nullptr;
    bound = BindOsiris(globals.data());
    CHECK(!bound.Available && bound.Hooks.Event == nullptr && bound.Hooks.CreateFileW == nullptr && *adapter->Target == nullptr);
    for (char byte : globals) CHECK(byte == 0);
    for (auto vtable : bound.Vtables) CHECK(vtable == nullptr);
    SetResolutionForLibraries(nullptr);
}

TEST(a_game_without_the_symbols_gets_nothing)
{
    // The test binary has none of the game's symbols: every entry is refused.
    FakeBinary binary;
    GameImage image;
    CHECK(image.OpenFile(binary.File));
    engine::Images images{ { &image, nullptr }, {} };
    auto resolution = engine::Resolve(engine::EngineTable(), images);

    for (auto const& hook : GameHooks()) {
        SetGameHookForTest(hook.Entry, nullptr);
        CHECK(!GameHookRefusal(hook, resolution).empty());
    }
    std::string report;
    CHECK(AttachGameHooks(resolution, report));             // nothing to attach is no failure of a write
    for (auto const& hook : GameHooks()) {
        CHECK(!GameHookAttached(hook.Entry) && *hook.Next == nullptr);
        CHECK(Contains(report, hook.Entry));
    }

    for (bool attached : { false, true }) {
        auto plan = PlanStaticSymbols(resolution, attached);
        CHECK(plan.Members.size() == StaticSymbolRows().size());
        CHECK(plan.BoundCount == 0 && plan.CriticalFailed && plan.Failed);
        for (auto const& member : plan.Members) {
            CHECK(member.Value == nullptr);
            CHECK(member.Row->How == Bound::Unavailable || !member.Reason.empty());
        }
    }

    std::vector<char> symbols(StaticSymbolsSize(), 0);
    for (auto const& a : CallAdapters()) {
        if (a.Target) *a.Target = nullptr;
        if (a.GlobalTarget) *a.GlobalTarget = nullptr;
        for (auto const& need : a.Needs) *need.Target = nullptr;
    }
    auto bound = BindStaticSymbols(symbols.data(), resolution);
    CHECK(bound.BoundCount == 0 && bound.CriticalFailed);
    for (char byte : symbols) CHECK(byte == 0);
    for (auto const& a : CallAdapters()) {
        CHECK((!a.Target || *a.Target == nullptr) && (!a.GlobalTarget || *a.GlobalTarget == nullptr));
        for (auto const& need : a.Needs) CHECK(*need.Target == nullptr);
    }
}

// --- the type index maps -----------------------------------------------------

constexpr char kHealthIndex[] = "__ZN2ls6TypeIdIN3eoc15HealthComponentEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE";
constexpr char kArmorIndex[] = "__ZN2ls6TypeIdIN3eoc14ArmorComponentEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE";
constexpr char kHealthOneFrame[] = "__ZN2ls6TypeIdIN3eoc15HealthComponentEN3ecs30OneFrameComponentTypeIdContextEE11m_TypeIndexE";
constexpr char kHealthReplication[] = "__ZN2ls6TypeIdIN3eoc15HealthComponentEN3ecs4sync21ReplicatedTypeContextEE11m_TypeIndexE";
constexpr char kDiedIndex[] = "__ZN2ls6TypeIdIN3esv5death26DiedEventOneFrameComponentEN3ecs30OneFrameComponentTypeIdContextEE11m_TypeIndexE";
constexpr char kVisualSystemIndex[] = "__ZN2ls6TypeIdINS_12VisualSystemEN3ecs14SystemsContextEE11m_TypeIndexE";
constexpr char kDefaultValuesIndex[] =
    "__ZN2ls6TypeIdIN3eoc18character_creation8_private20DefaultValuesManagerINS3_13DefaultValuesI8EAbilityEEEENS_23ImmutableDataHeadmasterEE11m_TypeIndexE";
// The static data type above as Norbyte's definitions spell it, and as the game registers it.
constexpr char kDefaultValuesNorbyte[] =
    "eoc::character_creation::_private::DefaultValuesManager<struct eoc::character_creation::_private::DefaultValues<enum EAbility> >";
constexpr char kDefaultValuesRegistered[] =
    "eoc::character_creation::_private::DefaultValuesManager<eoc::character_creation::_private::DefaultValues<EAbility>>";

constexpr uint64_t kHealthVA = FakeBinary::Data + 0x40, kArmorVA = FakeBinary::Data + 0x44, kHealthReplicationVA = FakeBinary::Data + 0x48,
    kDiedVA = FakeBinary::Data + 0x4c, kVisualSystemVA = FakeBinary::Data + 0x50, kDefaultValuesVA = FakeBinary::Data + 0x54;
constexpr uint64_t kReplicationState = FakeBinary::Data + 0x30, kComponentState = FakeBinary::Data + 0x60,
    kOneFrameState = FakeBinary::Data + 0x70, kSystemState = FakeBinary::Data + 0x80, kStaticDataState = FakeBinary::Data + 0x90;

// The five contexts as a table of their own, on the test binary.
struct TypeGame
{
    FakeBinary Binary;
    GameImage Image;
    engine::Entry Entries[5]{
        { .Name = "TypeIdContext::Component", .What = engine::Kind::TypeIndex, .Where = engine::Exact("_component_state"), .Context = "ecs::ComponentTypeIdContext" },
        { .Name = "TypeIdContext::OneFrameComponent", .What = engine::Kind::TypeIndex, .Where = engine::Exact("_one_frame_state"), .Context = "ecs::OneFrameComponentTypeIdContext" },
        { .Name = "TypeIdContext::System", .What = engine::Kind::TypeIndex, .Where = engine::Exact("_system_state"), .Context = "ecs::SystemsContext" },
        { .Name = "TypeIdContext::Replication", .What = engine::Kind::TypeIndex, .Where = engine::Exact("_after"), .Context = "ecs::sync::ReplicatedTypeContext" },
        { .Name = "TypeIdContext::ImmutableData", .What = engine::Kind::TypeIndex, .Where = engine::Exact("_static_data_state"), .Context = "ls::ImmutableDataHeadmaster" },
    };
    engine::Area Table{ "types", Entries };
    engine::Resolution Resolution;

    explicit TypeGame(std::vector<FakeBinary::Sym> const& symbols) : Binary(false, symbols)
    {
        CHECK(Image.OpenFile(Binary.File));
        Resolution = engine::Resolve({ &Table, 1 }, { { &Image, nullptr }, {} });
    }
};

std::vector<FakeBinary::Sym> EveryKindOfType()
{
    return {
        { kHealthIndex, kHealthVA, N_SECT, 3 },
        { kArmorIndex, kArmorVA, N_SECT, 3 },
        { kHealthReplication, kHealthReplicationVA, N_SECT, 3 },
        { kDiedIndex, kDiedVA, N_SECT, 3 },
        { kVisualSystemIndex, kVisualSystemVA, N_SECT, 3 },
        { kDefaultValuesIndex, kDefaultValuesVA, N_SECT, 3 },
        { "_component_state", kComponentState, N_SECT, 3 },
        { "_one_frame_state", kOneFrameState, N_SECT, 3 },
        { "_system_state", kSystemState, N_SECT, 3 },
        { "_static_data_state", kStaticDataState, N_SECT, 3 },
    };
}

TypeVariable const* VariableAt(TypeIndexPlan const& plan, uint64_t va)
{
    for (auto const& variable : plan.Variables) {
        if (variable.VA == va) return &variable;
    }
    return nullptr;
}

bool Says(std::vector<std::string> const& report, char const* a, char const* b)
{
    for (auto const& line : report) {
        if (Contains(line, a) && Contains(line, b)) return true;
    }
    return false;
}

TEST(type_contexts_are_the_tables_and_named_as_norbytes_code_compares_them)
{
    CHECK(TypeContextRows().size() == (size_t)TypeContext::Count);
    for (auto const& row : TypeContextRows()) {
        engine::Entry const* entry = nullptr;
        for (auto const& area : engine::EngineTable()) {
            for (auto const& e : area.Entries) {
                if (strcmp(e.Name, row.Entry) == 0) entry = &e;
            }
        }
        CHECK(entry && entry->What == engine::Kind::TypeIndex && entry->Where && entry->Needs);
        if (!entry) continue;
        // The name his code compares is the game's name of the context,
        // except for the systems.
        if (row.Is == TypeContext::System) {
            CHECK(strcmp(row.NorbyteName, "ecs::EntityWorld::SystemsContext") == 0 && strcmp(entry->Context, "ecs::SystemsContext") == 0);
        } else {
            CHECK(strcmp(row.NorbyteName, entry->Context) == 0);
        }
    }
}

TEST(type_names_reach_their_index_variables_by_the_games_rule)
{
    TypeGame game(EveryKindOfType());
    for (auto const& entry : game.Resolution.Entries) CHECK(entry.Usable() && entry.VA != 0);

    static constexpr BoundType kBound[] = {
        { "eoc::HealthComponent", BoundAs::Component, false },
        { "esv::death::DiedEventOneFrameComponent", BoundAs::Component, true },
        { "eoc::NoSuchComponent", BoundAs::Component, false },
        { "ls::VisualSystem", BoundAs::System, false },
        { "ls::NoSuchSystem", BoundAs::System, false },
        { kDefaultValuesNorbyte, BoundAs::StaticData, false },
    };
    auto plan = PlanTypeIndices(game.Resolution, kBound);
    auto const& components = plan.Types[(size_t)BoundAs::Component];
    auto const& systems = plan.Types[(size_t)BoundAs::System];
    auto const& staticData = plan.Types[(size_t)BoundAs::StaticData];
    CHECK(components.Bound == 3 && components.Mapped == 2 && components.Unmapped == 1 && components.Ambiguous == 0);
    CHECK(systems.Bound == 2 && systems.Mapped == 1 && systems.Unmapped == 1);
    CHECK(staticData.Bound == 1 && staticData.Mapped == 1 && staticData.Unmapped == 0);
    CHECK(plan.ByRule == 1 && plan.Replicated == 1);
    CHECK(!plan.AllMapped());
    CHECK(Says(plan.Report, "eoc::NoSuchComponent", "unmapped") && Says(plan.Report, "ls::NoSuchSystem", "unmapped"));
    CHECK(plan.Report.size() == 2);

    // Every variable of the five contexts is in the plan, under Norbyte's
    // spelling where he binds the type and the game's where he does not.
    CHECK(plan.Variables.size() == 6);
    auto health = VariableAt(plan, kHealthVA);
    CHECK(health && health->Context == TypeContext::Component && health->Bound && health->Name == "eoc::HealthComponent");
    auto armor = VariableAt(plan, kArmorVA);
    CHECK(armor && armor->Context == TypeContext::Component && !armor->Bound && armor->Name == "eoc::ArmorComponent");
    auto replicated = VariableAt(plan, kHealthReplicationVA);
    CHECK(replicated && replicated->Context == TypeContext::Replication && replicated->Bound && replicated->Name == "eoc::HealthComponent");
    auto died = VariableAt(plan, kDiedVA);
    CHECK(died && died->Context == TypeContext::OneFrameComponent && died->Bound);
    auto system = VariableAt(plan, kVisualSystemVA);
    CHECK(system && system->Context == TypeContext::System && system->Name == "ls::VisualSystem");
    auto values = VariableAt(plan, kDefaultValuesVA);
    CHECK(values && values->Context == TypeContext::ImmutableData && values->Bound);
    CHECK(values && values->Name == kDefaultValuesNorbyte && values->Registered == kDefaultValuesRegistered);

    CHECK(plan.Contexts[(size_t)TypeContext::Component].StateVA == kComponentState);
    CHECK(plan.Contexts[(size_t)TypeContext::Component].Variables == 2);
    CHECK(plan.Contexts[(size_t)TypeContext::Replication].StateVA == kReplicationState);

    // A declaration that says one-frame where the game says otherwise is
    // mapped, and said.
    static constexpr BoundType kWrongKind[] = { { "eoc::HealthComponent", BoundAs::Component, true } };
    plan = PlanTypeIndices(game.Resolution, kWrongKind);
    CHECK(plan.Types[(size_t)BoundAs::Component].Mapped == 1 && Says(plan.Report, "eoc::HealthComponent", "a component that persists"));

    // A system's name is not looked for among the components.
    static constexpr BoundType kWrongList[] = { { "eoc::HealthComponent", BoundAs::System, false }, { "ls::VisualSystem", BoundAs::StaticData, false } };
    plan = PlanTypeIndices(game.Resolution, kWrongList);
    CHECK(plan.Types[(size_t)BoundAs::System].Unmapped == 1 && plan.Types[(size_t)BoundAs::StaticData].Unmapped == 1);
    CHECK(VariableAt(plan, kHealthVA) && !VariableAt(plan, kHealthVA)->Bound);
}

TEST(an_ambiguous_type_name_stays_unknown)
{
    // One name in both component contexts: his lookup by name could mean either.
    TypeGame game({
        { kHealthIndex, kHealthVA, N_SECT, 3 },
        { kHealthOneFrame, kDiedVA, N_SECT, 3 },
        { kArmorIndex, kArmorVA, N_SECT, 3 },
        { "_component_state", kComponentState, N_SECT, 3 },
        { "_one_frame_state", kOneFrameState, N_SECT, 3 },
    });
    static constexpr BoundType kBound[] = { { "eoc::HealthComponent", BoundAs::Component, false }, { "eoc::ArmorComponent", BoundAs::Component, false } };
    auto plan = PlanTypeIndices(game.Resolution, kBound);
    auto const& components = plan.Types[(size_t)BoundAs::Component];
    CHECK(components.Mapped == 1 && components.Ambiguous == 1 && components.Unmapped == 0);
    CHECK(Says(plan.Report, "eoc::HealthComponent", "ambiguous"));
    // Neither variable goes into the maps: under the game's name his lookup would find it all the same.
    CHECK(!VariableAt(plan, kHealthVA) && !VariableAt(plan, kDiedVA) && VariableAt(plan, kArmorVA));

    // Two spellings of the definitions for one variable.
    static constexpr BoundType kTwice[] = { { "eoc::ArmorComponent", BoundAs::Component, false }, { "struct eoc::ArmorComponent", BoundAs::Component, false } };
    plan = PlanTypeIndices(game.Resolution, kTwice);
    CHECK(plan.Types[(size_t)BoundAs::Component].Mapped == 0 && plan.Types[(size_t)BoundAs::Component].Ambiguous == 2);
    CHECK(!VariableAt(plan, kArmorVA));
    // The same spelling twice is one name.
    static constexpr BoundType kSame[] = { { "eoc::ArmorComponent", BoundAs::Component, false }, { "eoc::ArmorComponent", BoundAs::Component, false } };
    plan = PlanTypeIndices(game.Resolution, kSame);
    CHECK(plan.Types[(size_t)BoundAs::Component].Mapped == 2 && VariableAt(plan, kArmorVA) && VariableAt(plan, kArmorVA)->Bound);
}

TEST(a_refused_context_leaves_its_types_unknown)
{
    // No state for the one-frame components: the entry is refused, and with
    // it every component name, which could be in that context.
    TypeGame game({
        { kHealthIndex, kHealthVA, N_SECT, 3 },
        { kDiedIndex, kDiedVA, N_SECT, 3 },
        { kVisualSystemIndex, kVisualSystemVA, N_SECT, 3 },
        { "_component_state", kComponentState, N_SECT, 3 },
        { "_system_state", kSystemState, N_SECT, 3 },
    });
    static constexpr BoundType kBound[] = { { "eoc::HealthComponent", BoundAs::Component, false }, { "ls::VisualSystem", BoundAs::System, false } };
    auto plan = PlanTypeIndices(game.Resolution, kBound);
    CHECK(!plan.Contexts[(size_t)TypeContext::OneFrameComponent].StateVA && !plan.Contexts[(size_t)TypeContext::OneFrameComponent].Refused.empty());
    CHECK(plan.Types[(size_t)BoundAs::Component].Unmapped == 1 && plan.Types[(size_t)BoundAs::System].Mapped == 1);
    CHECK(!plan.AllMapped());
    CHECK(Says(plan.Report, "TypeIdContext::OneFrameComponent", "refused"));
    CHECK(!VariableAt(plan, kHealthVA) && !VariableAt(plan, kDiedVA) && VariableAt(plan, kVisualSystemVA));
}

// The game's memory for the fill: the states, the lists of names and the
// index variables, by link-time address.
struct TypeMemory : TypeIndexSink
{
    struct Entry { void* Where; };
    std::vector<std::pair<uint64_t, void*>> Places;
    std::vector<std::pair<int32_t*, std::string>> Contexts;
    struct Added { int32_t* Variable; std::string Name; int32_t* State; };
    std::vector<Added> Types;

    void Put(uint64_t va, void* where) { Places.push_back({ va, where }); }
    void* At(uint64_t va) const
    {
        for (auto const& place : Places) {
            if (place.first == va) return place.second;
        }
        return nullptr;
    }
    void Context(int32_t* state, char const* norbyteName) override { Contexts.push_back({ state, norbyteName }); }
    void Type(int32_t* variable, char const* name, int32_t* state) override { Types.push_back({ variable, name, state }); }
    Added const* Find(int32_t* variable) const
    {
        for (auto const& type : Types) {
            if (type.Variable == variable) return &type;
        }
        return nullptr;
    }
};

engine::GameTypeName NameOf(char const* text) { return { text, (int32_t)strlen(text) }; }

TEST(type_indices_are_taken_only_from_what_the_game_numbered)
{
    TypeGame game(EveryKindOfType());
    static constexpr BoundType kBound[] = {
        { "eoc::HealthComponent", BoundAs::Component, false },
        { "esv::death::DiedEventOneFrameComponent", BoundAs::Component, true },
        { "ls::VisualSystem", BoundAs::System, false },
        { kDefaultValuesNorbyte, BoundAs::StaticData, false },
    };
    auto plan = PlanTypeIndices(game.Resolution, kBound);
    CHECK(plan.AllMapped() && plan.Variables.size() == 6);

    // The components are numbered: Armor 0, Health 1 (by name, as the game sorts them).
    engine::GameTypeName componentNames[] = { NameOf("eoc::ArmorComponent"), NameOf("eoc::HealthComponent") };
    engine::GameTypeNames components{ componentNames, 2, 2 };
    engine::GameTypeContextState componentState{ &components, nullptr };
    // The one-frame components are not numbered yet.
    engine::GameTypeContextState oneFrameState{ nullptr, nullptr };
    // The systems are, and the variable still has what its initialiser left.
    engine::GameTypeName systemNames[] = { NameOf("ls::VisualSystem") };
    engine::GameTypeNames systems{ systemNames, 1, 1 };
    engine::GameTypeContextState systemState{ &systems, nullptr };
    // The replicated types are, and the variable holds another type's index.
    engine::GameTypeName replicationNames[] = { NameOf("eoc::HealthComponent"), NameOf("eoc::OtherComponent") };
    engine::GameTypeNames replication{ replicationNames, 2, 2 };
    engine::GameTypeContextState replicationState{ &replication, nullptr };
    // The static data: the game's list has the registered name, the map gets Norbyte's.
    engine::GameTypeName staticNames[] = { NameOf("a::First"), NameOf(kDefaultValuesRegistered) };
    engine::GameTypeNames staticData{ staticNames, 2, 2 };
    engine::GameTypeContextState staticDataState{ &staticData, nullptr };

    int32_t health = 1, armor = 0, died = 0, visualSystem = 0x7fffffff, healthReplication = 1, defaultValues = 1;
    TypeMemory memory;
    memory.Put(kComponentState, &componentState);
    memory.Put(kOneFrameState, &oneFrameState);
    memory.Put(kSystemState, &systemState);
    memory.Put(kReplicationState, &replicationState);
    memory.Put(kStaticDataState, &staticDataState);
    memory.Put(kHealthVA, &health);
    memory.Put(kArmorVA, &armor);
    memory.Put(kDiedVA, &died);
    memory.Put(kVisualSystemVA, &visualSystem);
    memory.Put(kHealthReplicationVA, &healthReplication);
    memory.Put(kDefaultValuesVA, &defaultValues);
    auto at = [&](uint64_t va) { return memory.At(va); };

    auto fill = FillTypeIndices(plan, at, memory);
    CHECK(fill.Contexts == 4 && memory.Contexts.size() == 4);
    CHECK(fill.Types == 3 && memory.Types.size() == 3 && fill.Refused == 2);
    for (auto const& context : memory.Contexts) {
        CHECK(context.first != reinterpret_cast<int32_t*>(&oneFrameState));
        if (context.first == reinterpret_cast<int32_t*>(&systemState)) CHECK(context.second == "ecs::EntityWorld::SystemsContext");
        if (context.first == reinterpret_cast<int32_t*>(&componentState)) CHECK(context.second == "ecs::ComponentTypeIdContext");
    }
    auto added = memory.Find(&health);
    CHECK(added && added->Name == "eoc::HealthComponent" && added->State == reinterpret_cast<int32_t*>(&componentState));
    added = memory.Find(&armor);
    CHECK(added && added->Name == "eoc::ArmorComponent");
    added = memory.Find(&defaultValues);
    CHECK(added && added->Name == kDefaultValuesNorbyte && added->State == reinterpret_cast<int32_t*>(&staticDataState));
    // Not numbered, not numbered for this variable, numbered as something else.
    CHECK(!memory.Find(&died) && !memory.Find(&visualSystem) && !memory.Find(&healthReplication));
    CHECK(Says(fill.Report, "one-frame components", "has not numbered"));
    CHECK(Says(fill.Report, "ls::VisualSystem", "0x7fffffff"));
    CHECK(Says(fill.Report, "eoc::HealthComponent", "eoc::OtherComponent"));

    // Once the game has numbered everything, everything is taken.
    engine::GameTypeName oneFrameNames[] = { NameOf("esv::death::DiedEventOneFrameComponent") };
    engine::GameTypeNames oneFrame{ oneFrameNames, 1, 1 };
    oneFrameState.Numbered = &oneFrame;
    visualSystem = 0;
    healthReplication = 0;
    TypeMemory after;
    after.Places = memory.Places;
    fill = FillTypeIndices(plan, at, after);
    CHECK(fill.Contexts == 5 && fill.Types == 6 && fill.Refused == 0 && fill.Report.empty());

    // A name that is the start of the right one, or longer, is another type.
    componentNames[1] = NameOf("eoc::HealthComponentX");
    TypeMemory longer;
    fill = FillTypeIndices(plan, at, longer);
    CHECK(fill.Types == 5 && !longer.Find(&health));
    componentNames[1] = { "eoc::HealthComponent", 11 };
    TypeMemory shorter;
    fill = FillTypeIndices(plan, at, shorter);
    CHECK(fill.Types == 5 && !shorter.Find(&health));
    // An index past the game's list, or below it.
    componentNames[1] = NameOf("eoc::HealthComponent");
    health = 2;
    TypeMemory past;
    fill = FillTypeIndices(plan, at, past);
    CHECK(fill.Types == 5 && !past.Find(&health));
    health = -1;
    TypeMemory negative;
    fill = FillTypeIndices(plan, at, negative);
    CHECK(fill.Types == 5 && !negative.Find(&health));

    // A game mapped from disk has no memory to read: nothing is taken.
    TypeMemory none;
    fill = FillTypeIndices(plan, [](uint64_t) -> void* { return nullptr; }, none);
    CHECK(fill.Contexts == 0 && fill.Types == 0 && none.Types.empty() && none.Contexts.empty());
}

TEST(a_status_effect_object_is_classed_by_its_vtable_alone)
{
    // One vtable for each class: seven slots, as the game's are.
    constexpr size_t kClasses = static_cast<size_t>(StatusVfxClass::Count);
    void* slots[kClasses + 2][7]{};
    void const* vtables[kClasses]{};
    for (size_t n = 1; n < kClasses; n++) vtables[n] = slots[n];
    struct Object { void const* Vtable; uint64_t Status; };

    // Not bound: every object is the base class.
    Object beam{ vtables[static_cast<size_t>(StatusVfxClass::Beam)], 0 };
    CHECK(ClassOfStatusVfx(&beam) == StatusVfxClass::Base);

    SetStatusVfxForTest(vtables);
    for (size_t n = 1; n < kClasses; n++) {
        Object object{ vtables[n], 0 };
        CHECK(ClassOfStatusVfx(&object) == static_cast<StatusVfxClass>(n));
    }
    // A class that is not one of them (an aura, a material, one the table
    // does not know), an object without a vtable, and no object.
    Object aura{ slots[kClasses], 0 }, none{ nullptr, 0 };
    CHECK(ClassOfStatusVfx(&aura) == StatusVfxClass::Base);
    CHECK(ClassOfStatusVfx(&none) == StatusVfxClass::Base);
    CHECK(ClassOfStatusVfx(nullptr) == StatusVfxClass::Base);
    // A pointer into a vtable is not that vtable.
    Object inside{ &slots[1][1], 0 };
    CHECK(ClassOfStatusVfx(&inside) == StatusVfxClass::Base);

    SetStatusVfxForTest(nullptr);
    CHECK(ClassOfStatusVfx(&beam) == StatusVfxClass::Base);

    // On a binary without the vtables nothing is bound, and what was is taken back.
    FakeBinary binary;
    GameImage image;
    CHECK(image.OpenFile(binary.File));
    auto resolution = engine::Resolve(engine::EngineTable(), { { &image, nullptr }, {} });
    CHECK(Contains(StatusVfxRefusal(resolution), "is not resolved"));
    SetStatusVfxForTest(vtables);
    BindStatusVfx(resolution);
    CHECK(ClassOfStatusVfx(&beam) == StatusVfxClass::Base);

    // The table has a vtable entry for each class and for the two left as
    // the base class, each with all seven slots stated, the two destructors
    // first: no slot is where Norbyte's five-slot structure has it.
    std::vector<char const*> names;
    for (size_t n = 1; n < kClasses; n++) names.push_back(StatusVfxEntryName(static_cast<StatusVfxClass>(n)));
    for (auto name : kStatusVfxBaseEntryNames) names.push_back(name);
    CHECK(StatusVfxEntryName(StatusVfxClass::Base) == nullptr && StatusVfxEntryName(StatusVfxClass::Count) == nullptr);
    std::set<std::string> symbols;
    for (auto name : names) {
        engine::Entry const* found = nullptr;
        for (auto const& area : engine::EngineTable()) {
            for (auto const& entry : area.Entries) {
                if (strcmp(entry.Name, name) == 0) found = &entry;
            }
        }
        CHECK(found != nullptr);
        if (!found) continue;
        CHECK(found->What == engine::Kind::Vtable && found->Where.How == engine::Find::ExactName);
        CHECK(symbols.insert(found->Where.Symbol).second);
        CHECK(found->Methods.size() == 7);
        for (size_t slot = 0; slot < found->Methods.size(); slot++) CHECK(found->Methods[slot].Offset == 8 * slot);
    }
    CHECK(symbols.size() == 8);
}

TEST(the_connect_message_is_wrapped_only_through_its_own_vtable)
{
    // A vtable as the game's is: two destructors, then the function.
    void* function = reinterpret_cast<void*>(&bt_callee);
    void* destructor = reinterpret_cast<void*>(&bt_other_callee);
    void* vtable[] = { destructor, destructor, function, nullptr };
    void* otherVtable[] = { destructor, destructor, destructor, nullptr };
    struct { void** Vtable; uint64_t Fields[4]; } message{ vtable, {} }, other{ otherVtable, {} };

    CHECK(SerializeOfMessage(&message, vtable, 0x10, function) == function);
    // The slot Norbyte's Windows structure names is the deleting destructor here: never that.
    CHECK(SerializeOfMessage(&message, vtable, 0x08, function) == nullptr);
    // Another class's object, or nothing resolved.
    CHECK(SerializeOfMessage(&other, vtable, 0x10, function) == nullptr);
    CHECK(SerializeOfMessage(&message, nullptr, 0x10, function) == nullptr);
    CHECK(SerializeOfMessage(&message, vtable, 0x10, nullptr) == nullptr);
    CHECK(SerializeOfMessage(nullptr, vtable, 0x10, function) == nullptr);

    SetConnectMessageForTest(vtable, 0x10, function);
    CHECK(ClientConnectMessageSerialize(&message) == function);
    CHECK(ClientConnectMessageSerialize(&other) == nullptr);
    SetConnectMessageForTest(nullptr, 0, nullptr);
    CHECK(ClientConnectMessageSerialize(&message) == nullptr);

    // On a binary without the entries nothing is bound.
    FakeBinary binary;
    GameImage image;
    CHECK(image.OpenFile(binary.File));
    auto resolution = engine::Resolve(engine::EngineTable(), { { &image, nullptr }, {} });
    SetConnectMessageForTest(vtable, 0x10, function);
    BindNetMessages(resolution);
    CHECK(ClientConnectMessageSerialize(&message) == nullptr);

    // The table has both entries, and the vtable entry names the function's slot.
    engine::Entry const* entries[2]{};
    for (auto const& area : engine::EngineTable()) {
        for (auto const& entry : area.Entries) {
            for (size_t n = 0; n < 2; n++) {
                if (strcmp(entry.Name, kConnectMessageEntryNames[n]) == 0) entries[n] = &entry;
            }
        }
    }
    CHECK(entries[0] && entries[1]);
    if (entries[0] && entries[1]) {
        CHECK(entries[0]->What == engine::Kind::FunctionToHook && entries[1]->What == engine::Kind::Vtable);
        bool named = false;
        for (auto const& method : entries[1]->Methods) {
            named = named || (strcmp(method.Function.Symbol, entries[0]->Where.Symbol) == 0 && method.Offset == 0x10);
        }
        CHECK(named);
    }
}

TEST(networking_is_withheld_on_a_game_whose_session_is_not_proven)
{
    // Norbyte's network managers find no peer until a game is bound.
    SetNetworkSessionConfirmedForTest(false);
    CHECK(!NetworkSessionConfirmed());
    SetNetworkSessionConfirmedForTest(true);
    CHECK(NetworkSessionConfirmed());

    // A binary without the network session's functions does not confirm it.
    FakeBinary binary;
    GameImage image;
    CHECK(image.OpenFile(binary.File));
    CHECK(!CheckGameShapes(image).Confirmed(GameShape::NetworkSession));
    BindNetMessages(engine::Resolve(engine::EngineTable(), { { &image, nullptr }, {} }));
    CHECK(!NetworkSessionConfirmed());

    // Nor does a resolution without a game.
    SetNetworkSessionConfirmedForTest(true);
    BindNetMessages(engine::Resolve(engine::EngineTable(), { { nullptr, nullptr }, {} }));
    CHECK(!NetworkSessionConfirmed());
}

TEST(an_unproven_layout_keeps_back_what_leads_to_it)
{
    // What this build ties to a layout: the table of fixed strings with the
    // four functions his fixed strings go through, the resource manager's
    // pointer, the effects manager's system type, the stats object, the
    // client and server objects with their mod managers, and the path roots
    // and the file reader.
    bool resources = false, effects = false;
    std::set<std::string> strings, stats, client, server, modules, files, events, inlined;
    for (auto const& dependent : LayoutDependents()) {
        CHECK((dependent.Member != nullptr) != (dependent.EngineClass != nullptr));
        // His code reads through these without a test: each is a critical
        // member, so start-up refuses when its layout is not proven.
        if (dependent.Shape == GameShape::Stats || dependent.Shape == GameShape::Client || dependent.Shape == GameShape::Server
            || dependent.Shape == GameShape::Modules || dependent.Shape == GameShape::Files || dependent.Shape == GameShape::StateEvents) {
            CHECK(dependent.Member != nullptr);
            if (!dependent.Member) continue;
            // What does the work of a function the game has only inline is
            // kept back too, and is no critical member: his code tests it.
            if (auto adapter = FindCallAdapter(dependent.Member); adapter && !adapter->Target && !adapter->Needs.empty()) {
                if (dependent.Shape == GameShape::Stats) inlined.insert(dependent.Member);
                for (auto const& r : StaticSymbolRows()) CHECK(strcmp(r.Member, dependent.Member) != 0 || (!r.Critical && r.How == Bound::CallAdapter));
                continue;
            }
            (dependent.Shape == GameShape::Stats ? stats : dependent.Shape == GameShape::Client ? client
                : dependent.Shape == GameShape::Server ? server : dependent.Shape == GameShape::Modules ? modules
                : dependent.Shape == GameShape::Files ? files : events).insert(dependent.Member);
            bool critical = false;
            for (auto const& r : StaticSymbolRows()) critical = critical || (strcmp(r.Member, dependent.Member) == 0 && r.Critical);
            CHECK(critical);
        }
        if (dependent.Shape == GameShape::StringTable && dependent.Member) {
            strings.insert(dependent.Member);
            bool critical = false;
            for (auto const& r : StaticSymbolRows()) critical = critical || (strcmp(r.Member, dependent.Member) == 0 && r.Critical);
            CHECK(critical);
        }
        if (dependent.Shape == GameShape::ResourceManager) {
            resources = dependent.Member && strcmp(dependent.Member, "ls__gGlobalResourceManager") == 0;
            bool row = false;
            for (auto const& r : StaticSymbolRows()) row = row || strcmp(r.Member, dependent.Member) == 0;
            CHECK(row);
        }
        if (dependent.Shape == GameShape::EffectsManager) {
            effects = dependent.EngineClass && strcmp(dependent.EngineClass, "ls::EffectsManager") == 0;
            bool bound = false;
            for (auto const& type : BoundTypes()) bound = bound || (type.As == BoundAs::System && strcmp(type.EngineClass, dependent.EngineClass) == 0);
            CHECK(bound);
        }
    }
    CHECK(resources && effects);
    CHECK(stats == std::set<std::string>{ "gRPGStats" });
    // Every such function depends on the stats object's layout.
    std::set<std::string> standIns;
    for (auto const& adapter : CallAdapters()) {
        if (!adapter.Target && !adapter.Needs.empty()) standIns.insert(adapter.Entry);
    }
    CHECK(inlined == standIns);
    CHECK(client == std::set<std::string>{ "ecl__EoCClient" } && server == std::set<std::string>{ "esv__EoCServer" });
    CHECK(modules == (std::set<std::string>{ "ecl__EoCClient", "esv__EoCServer" }));
    CHECK(files == (std::set<std::string>{ "ls__PathRoots", "ls__FileReader__ctor" }));
    CHECK(events == (std::set<std::string>{ "ecl__gGameStateEventManager", "esv__gGameStateEventManager" }));
    CHECK(strings == (std::set<std::string>{ "ls__gGlobalStringTable", "ls__GlobalStringTable__MainTable__CreateFromString",
        "ls__GlobalStringTable__MainTable__DecRef", "ls__FixedString__GetString", "ls__FixedString__IncRef" }));

    static constexpr LayoutDependent kDependents[] = {
        { GameShape::ResourceManager, "a_member", nullptr },
        { GameShape::EffectsManager, nullptr, "ls::VisualSystem" },
    };
    // Every proof holds: nothing is kept back.
    GameShapes shapes;
    shapes.Proofs.push_back({ GameShape::ResourceManager, "a function", true, {}, {} });
    shapes.Proofs.push_back({ GameShape::EffectsManager, "another function", true, {}, {} });
    auto withheld = WithheldFor(shapes, kDependents);
    CHECK(withheld.Members.empty() && withheld.Types.empty());
    // One proof fails: what depends on that shape, and nothing else.
    shapes.Proofs[1].Problem = "its code changed";
    withheld = WithheldFor(shapes, kDependents);
    CHECK(withheld.Members.empty() && withheld.Types.size() == 1);
    CHECK(withheld.Types.size() == 1 && strcmp(withheld.Types[0].EngineClass, "ls::VisualSystem") == 0);
    CHECK(withheld.Types.size() == 1 && Contains(withheld.Types[0].Why, "another function") && Contains(withheld.Types[0].Why, "its code changed"));
    // No proof at all (a game that could not be read): everything.
    withheld = WithheldFor(GameShapes{}, kDependents);
    CHECK(withheld.Members.size() == 1 && withheld.Types.size() == 1);
    CHECK(withheld.Members.size() == 1 && strcmp(withheld.Members[0].Name, "a_member") == 0);

    // A withheld type is unmapped, and its variable is not in the maps under any name.
    TypeGame game(EveryKindOfType());
    static constexpr BoundType kBound[] = { { "eoc::HealthComponent", BoundAs::Component, false }, { "ls::VisualSystem", BoundAs::System, false } };
    auto plan = PlanTypeIndices(game.Resolution, kBound, withheld.Types);
    CHECK(plan.Types[(size_t)BoundAs::System].Unmapped == 1 && plan.Types[(size_t)BoundAs::System].Mapped == 0);
    CHECK(plan.Types[(size_t)BoundAs::Component].Mapped == 1);
    CHECK(!VariableAt(plan, kVisualSystemVA) && VariableAt(plan, kHealthVA));
    CHECK(Says(plan.Report, "ls::VisualSystem", "its code changed") || Says(plan.Report, "ls::VisualSystem", "not confirmed"));
    CHECK(!plan.AllMapped());
}

TEST(a_component_whose_members_the_game_contradicts_is_withheld)
{
    // A component of two ints at +0 and +4, and a structure that is not a component.
    std::vector<BoundClass> classes(2);
    classes[0] = { "TestComponent", "eoc::TestComponent", 8, -1, 0,
        { { "A", 0, 4, -1, 0, -1, false, true }, { "B", 4, 4, -1, 0, -1, false, true } } };
    classes[1] = { "Other", nullptr, 8, -1, 0, { { "C", 0, 8, -1, 0, -1, false, true } } };

    // The game writes both members, and the two as one, and copies bytes
    // across them and past the end: nothing is withheld.
    GameFieldScan game;
    game.Serialized.push_back({ "eoc::TestComponent", "serializer", { { 0, 4 }, { 4, 4 }, { 0, 8 } }, 0 });
    game.Serialized.push_back({ "eoc::TestComponent", "load",
        { { 2, 4, FieldAccess::Stored }, { 0, 16, FieldAccess::Stored }, { 6, 2, FieldAccess::Read } }, 0 });
    // An address inside a member handed to a call that was not followed (a
    // range cleared from +2), and one just past the end: no evidence either.
    game.Serialized.push_back({ "eoc::TestComponent", "destructor",
        { { 2, 0, FieldAccess::PassedOn }, { 8, 0, FieldAccess::PassedOn }, { 8, 0, FieldAccess::Passed } }, 0 });
    game.Named.push_back({ "eoc::TestComponent", "m_B", "int", 4, 4 });
    LayoutsWithheld withheld;
    WithholdContradicted(withheld, game, classes);
    CHECK(withheld.Types.empty());

    // The game writes four bytes at +2 and names a member at +6: the
    // component is withheld once, with the first of the two in the reason.
    game.Serialized[0].Fields.push_back({ 2, 4 });
    game.Named.push_back({ "eoc::TestComponent", "m_C", "int", 6, 4 });
    WithholdContradicted(withheld, game, classes);
    CHECK(withheld.Types.size() == 1);
    CHECK(withheld.Types.size() == 1 && strcmp(withheld.Types[0].EngineClass, "eoc::TestComponent") == 0
        && Contains(withheld.Types[0].Why, "+0x6 (m_C)") && Contains(withheld.Types[0].Why, "(and 1 more)"));

    // What the game says of a class that is not a bound component, and the
    // component's address handed to a function, withhold nothing.
    withheld = {};
    game = {};
    game.Named.push_back({ "eoc::NotBound", "m_X", "int", 2, 4 });
    game.Serialized.push_back({ "eoc::TestComponent", "destructor", { { 0, 0, FieldAccess::Passed } }, 3 });
    WithholdContradicted(withheld, game, classes);
    CHECK(withheld.Types.empty());

    // Without the game's image nothing is compared, and nothing is withheld for it.
    engine::Resolution none;
    auto layouts = LayoutsWithheldOn(none);
    CHECK(!layouts.MembersUnchecked.empty());
}

TEST(every_unproven_shape_keeps_something_back)
{
    // The locks are gated where they are called (Layout/GameLockCalls.h):
    // start-up refuses when they are not confirmed. The network session is
    // gated where Norbyte's network managers find their peer
    // (Binding/NetMessages.h). Every other shape keeps back what leads to it.
    std::set<GameShape> gated{ GameShape::CriticalSection, GameShape::SRWKernelLock, GameShape::RecursiveKernelLock,
        GameShape::RecursiveSpinLock, GameShape::NoesisLocks, GameShape::NetworkSession };
    // The results of virtual functions are gated through the rows of the
    // slot check that name their proofs (DeclaredVirtual::ResultProof).
    gated.insert(GameShape::VirtualResults);
    // A class's members are gated with the class (ClassMemberShapes).
    for (auto const& cls : ClassMemberShapes()) gated.insert(cls.Shape);
    for (auto const& dependent : LayoutDependents()) gated.insert(dependent.Shape);
    for (uint8_t n = 0; n < (uint8_t)GameShape::Count; n++) CHECK(gated.count((GameShape)n) == 1);

    // What is read without a test keeps back a critical member, so start-up
    // refuses; what is reached through a bound type keeps the type back.
    // Every dependent names a member of the rows or a bound type.
    static const std::set<GameShape> kRefusing{ GameShape::EntityWorld, GameShape::EntityCommands, GameShape::EntityQueries,
        GameShape::EntityReplication, GameShape::Queues, GameShape::Variant, GameShape::NetPeer };
    for (auto const& dependent : LayoutDependents()) {
        CHECK(dependent.Member != nullptr || dependent.EngineClass != nullptr);
        if (dependent.Member) {
            bool known = false, critical = false;
            for (auto const& r : StaticSymbolRows()) {
                if (strcmp(r.Member, dependent.Member) != 0) continue;
                known = true;
                critical = r.Critical;
            }
            CHECK(known && (critical || !kRefusing.count(dependent.Shape)));
        }
        if (dependent.EngineClass) {
            bool bound = false;
            for (auto const& type : BoundTypes()) bound = bound || strcmp(type.EngineClass, dependent.EngineClass) == 0;
            CHECK(bound);
        }
    }

    // One proof of the command buffers fails: the world's update is kept
    // back, with the function and the problem in the reason; the components
    // stay.
    GameShapes shapes;
    for (uint8_t n = 0; n < (uint8_t)GameShape::Count; n++) shapes.Proofs.push_back({ (GameShape)n, "a function", true, {}, {} });
    shapes.Proofs.push_back({ GameShape::EntityCommands, "ecs::FrameAllocator::Allocate(unsigned short)", true, {}, "instruction 72 does not call _posix_memalign" });
    auto withheld = WithheldFor(shapes, LayoutDependents());
    CHECK(withheld.Types.empty() && withheld.Members.size() == 1);
    CHECK(withheld.Members.size() == 1 && strcmp(withheld.Members[0].Name, "ecs__EntityWorld__Update") == 0);
    CHECK(withheld.Members.size() == 1 && Contains(withheld.Members[0].Why, "entity command buffers")
        && Contains(withheld.Members[0].Why, "_posix_memalign"));
    // A component member's proof fails: those components, and no member.
    shapes.Proofs.pop_back();
    shapes.Proofs.push_back({ GameShape::Components, "esv::Projectile::Projectile()", true, {}, "its code changed" });
    withheld = WithheldFor(shapes, LayoutDependents());
    CHECK(withheld.Members.empty() && withheld.Types.size() == 9);
    // A proof of the camera controller fails: the camera component, through
    // which scripts reach the controller, and nothing else.
    shapes.Proofs.pop_back();
    shapes.Proofs.push_back({ GameShape::Cameras, "rf::CameraController::UpdateDirtyComponents()", true, {}, "its code changed" });
    withheld = WithheldFor(shapes, LayoutDependents());
    CHECK(withheld.Members.empty() && withheld.Types.size() == 1);
    CHECK(withheld.Types.size() == 1 && strcmp(withheld.Types[0].EngineClass, "ls::CameraComponent") == 0
        && Contains(withheld.Types[0].Why, "UpdateDirtyComponents") && Contains(withheld.Types[0].Why, "its code changed"));
    // A proof of a dialogue's members fails: the function that loads one is
    // not bound, so that Ext.StaticData.LoadResource loads and holds none,
    // and the client's timeline system, which has the client's dialog
    // manager, is unknown.
    shapes.Proofs.pop_back();
    shapes.Proofs.push_back({ GameShape::Dialogs, "LSDialogs::DialogManager::Update(ls::GameTime const&)", true, {}, "its code changed" });
    withheld = WithheldFor(shapes, LayoutDependents());
    CHECK(withheld.Members.size() == 1 && withheld.Types.size() == 1);
    CHECK(withheld.Members.size() == 1 && strcmp(withheld.Members[0].Name, "dlg__DialogManager__PerformDialogLoad") == 0
        && Contains(withheld.Members[0].Why, "its code changed"));
    CHECK(withheld.Types.size() == 1 && strcmp(withheld.Types[0].EngineClass, "ecl::TimelineSystem") == 0);
    bool optional = false;
    for (auto const& r : StaticSymbolRows()) optional = optional || (strcmp(r.Member, "dlg__DialogManager__PerformDialogLoad") == 0 && !r.Critical);
    CHECK(optional);
}

TEST(every_class_of_the_slot_check_keeps_something_back)
{
    // Each class whose virtual functions are checked is reached through a
    // member of StaticSymbols, a bound type, or a place of his code that
    // asks; nothing names a class the check does not cover.
    std::set<std::string> declared, dependent;
    for (auto const& row : DeclaredVirtuals()) declared.insert(row.Class);
    for (auto const& d : VirtualDependents()) {
        dependent.insert(d.Class);
        CHECK(declared.count(d.Class) == 1);
        CHECK((d.Member != nullptr) + (d.EngineClass != nullptr) + d.Asked + d.Refuses == 1);
        if (d.Member) {
            bool row = false;
            for (auto const& r : StaticSymbolRows()) row = row || strcmp(r.Member, d.Member) == 0;
            CHECK(row);
        }
        if (d.EngineClass) {
            bool bound = false;
            for (auto const& type : BoundTypes()) bound = bound || strcmp(type.EngineClass, d.EngineClass) == 0;
            CHECK(bound);
        }
    }
    CHECK(declared == dependent);

    static constexpr DeclaredVirtual kRows[] = {
        { "A", "First", 2, "__ZTV1A", "__ZN1A5FirstEv" },
        { "A", "Second", 3, "__ZTV1A", "__ZN1A6SecondEv" },
        { "B", "Only", 2, "__ZTV1B", "__ZN1B4OnlyEv" },
        { "C", "Only", 5, "__ZTV1C", "__ZN1C4OnlyEv" },
    };
    static constexpr VirtualDependent kDependents[] = {
        { "A", "a_member", nullptr },
        { "A", nullptr, "a::Type" },
        { "B", nullptr, nullptr, true },
        { "C", nullptr, nullptr, false, true },
    };
    // Every slot holds: nothing kept back, both classes confirmed.
    std::vector<VirtualSlotResult> slots{ { &kRows[0], 2, {} }, { &kRows[1], 3, {} }, { &kRows[2], 2, {} }, { &kRows[3], 5, {} } };
    LayoutsWithheld withheld;
    WithholdFor(withheld, slots, kDependents);
    CHECK(withheld.Members.empty() && withheld.Types.empty() && withheld.UnconfirmedClasses.empty() && withheld.Refusing.empty());
    CHECK(withheld.ConfirmedClasses == (std::vector<std::string>{ "A", "B", "C" }));
    // One function of A is elsewhere: A's member and type, with the function
    // and the problem in the reason; B stays confirmed.
    slots[1].Problem = "this build calls it through slot 3, and the game's vtable holds it in slot 4";
    withheld = {};
    WithholdFor(withheld, slots, kDependents);
    CHECK(withheld.Members.size() == 1 && withheld.Types.size() == 1 && withheld.UnconfirmedClasses.size() == 1);
    CHECK(withheld.Members.size() == 1 && strcmp(withheld.Members[0].Name, "a_member") == 0 && Contains(withheld.Members[0].Why, "Second")
        && Contains(withheld.Members[0].Why, "slot 4"));
    CHECK(withheld.Types.size() == 1 && strcmp(withheld.Types[0].EngineClass, "a::Type") == 0);
    CHECK(withheld.ConfirmedClasses == (std::vector<std::string>{ "B", "C" }) && withheld.Refusing.empty());
    // A function of C, which his code reaches everywhere: start-up refuses.
    slots[3].Problem = "the game's vtable does not hold the function";
    withheld = {};
    WithholdFor(withheld, slots, kDependents);
    CHECK(withheld.Refusing.size() == 1 && Contains(withheld.Refusing[0], " C "));
    // No result at all (a game that could not be read): nothing is confirmed.
    withheld = {};
    WithholdFor(withheld, {}, kDependents);
    CHECK(withheld.Members.size() == 1 && withheld.Types.size() == 1 && withheld.ConfirmedClasses.empty()
        && withheld.UnconfirmedClasses.size() == 3 && withheld.Refusing.size() == 1);

    // A row that names how its result comes back holds only with that proof.
    static constexpr DeclaredVirtual kReturning{ "D", "Position", 3, "__ZTV1D", "__ZNK1D8PositionEv", "D::Position()" };
    std::vector<VirtualSlotResult> returning{ { &kReturning, 3, {} } };
    GameShapes shapes;
    shapes.Proofs.push_back({ GameShape::VirtualResults, "D::Position()", true, {}, {} });
    RequireResultProofs(returning, shapes);
    CHECK(returning[0].Problem.empty());
    shapes.Proofs[0].Problem = "it does not hand the position on in s0, s1 and s2";
    RequireResultProofs(returning, shapes);
    CHECK(Contains(returning[0].Problem, "s0, s1 and s2"));
    returning[0].Problem.clear();
    RequireResultProofs(returning, GameShapes{});
    CHECK(Contains(returning[0].Problem, "D::Position()"));
    bool named = true;
    for (auto const& row : DeclaredVirtuals()) named = named && (!row.ResultProof || *row.ResultProof);
    CHECK(named);

    // A class whose members are a shape holds only with that shape.
    static constexpr ClassMembers kMembers[] = { { "D", GameShape::Audio } };
    returning[0].Problem.clear();
    GameShapes members;
    members.Proofs.push_back({ GameShape::Audio, "D::D()", true, {}, {} });
    RequireMemberShapes(returning, members, kMembers);
    CHECK(returning[0].Problem.empty());
    members.Proofs[0].Problem = "the listeners are not at +0x68";
    RequireMemberShapes(returning, members, kMembers);
    CHECK(Contains(returning[0].Problem, "+0x68") && Contains(returning[0].Problem, "sound objects"));
    returning[0].Problem.clear();
    RequireMemberShapes(returning, GameShapes{}, kMembers);
    CHECK(!returning[0].Problem.empty());
    bool known = true;
    for (auto const& cls : ClassMemberShapes()) {
        known = known && std::any_of(DeclaredVirtuals().begin(), DeclaredVirtuals().end(),
            [&](DeclaredVirtual const& row) { return strcmp(row.Class, cls.Class) == 0; });
    }
    CHECK(known);

    // His code is told a class is confirmed only once the start-up sequence
    // has kept what it found.
    CHECK(!VirtualsConfirmed("B"));
    LayoutsWithheld kept;
    kept.ConfirmedClasses = { "B" };
    KeepLayoutsOfTheGame(&kept);
    CHECK(VirtualsConfirmed("B") && !VirtualsConfirmed("A") && !VirtualsConfirmed(nullptr));
    KeepLayoutsOfTheGame(nullptr);
    CHECK(!VirtualsConfirmed("B"));
}

TEST(every_bound_type_has_a_name)
{
    // The lists of this build, as Norbyte's code walks them.
    size_t counts[(size_t)BoundAs::Count]{};
    for (auto const& type : BoundTypes()) {
        CHECK(type.EngineClass && *type.EngineClass);
        CHECK(type.As < BoundAs::Count);
        if (type.As < BoundAs::Count) counts[(size_t)type.As]++;
        if (type.As != BoundAs::Component) CHECK(!type.OneFrame);
    }
    CHECK(counts[(size_t)BoundAs::Component] > 1000 && counts[(size_t)BoundAs::System] > 50 && counts[(size_t)BoundAs::StaticData] > 100);
}

TEST(code_patches_go_in_whole_or_not_at_all_and_come_out)
{
    constexpr uint32_t kSayOne = 0x52800020, kSayZero = 0x52800000, kSayTwo = 0x52800040;
    // Through pointers: the compiler must not know what the functions return.
    int (*volatile first)() = &bt_says_one;
    int (*volatile second)() = &bt_says_one_too;
    void* firstAt = reinterpret_cast<void*>(&bt_says_one);
    void* secondAt = reinterpret_cast<void*>(&bt_says_one_too);
    CodeSiteWrite sites[] = { { firstAt, { kSayOne }, kSayZero }, { secondAt, { kSayOne }, kSayZero } };

    std::vector<uint32_t> originals;
    CHECK(first() == 1 && second() == 1);
    CHECK(WriteCodeSites(sites, originals).empty());
    CHECK(first() == 0 && second() == 0);
    CHECK((originals == std::vector<uint32_t>{ kSayOne, kSayOne }));
    // Written already: the instruction is no longer the one that was read.
    std::vector<uint32_t> again;
    CHECK(Contains(WriteCodeSites(sites, again), "not the one that was read") && again.empty());
    CHECK(RestoreCodeSites(sites, originals).empty());
    CHECK(first() == 1 && second() == 1);

    // One site that does not hold its instruction refuses the whole, before anything is written.
    CodeSiteWrite mixed[] = { { firstAt, { kSayOne }, kSayZero }, { secondAt, { kSayTwo }, kSayZero } };
    originals.clear();
    CHECK(Contains(WriteCodeSites(mixed, originals), "nothing was written") && originals.empty());
    CHECK(first() == 1 && second() == 1);

    // A masked instruction: any call may be there, and the word found is what is put back.
    CodeSiteWrite call[] = { { bt_host_call, { 0x94000000, Any::Branch }, 0xd503201f } };
    uint32_t callWord;
    memcpy(&callWord, bt_host_call, sizeof(callWord));
    CHECK(WriteCodeSites(call, originals).empty() && originals == std::vector<uint32_t>{ callWord });
    CHECK(RestoreCodeSites(call, originals).empty() && memcmp(bt_host_call, &callWord, sizeof(callWord)) == 0);

    // Taking out leaves alone a site that someone has changed since, and puts back the others.
    CHECK(WriteCodeSites(sites, originals).empty());
    CHECK(WriteCodeWord(firstAt, kSayTwo).empty());
    CHECK(Contains(RestoreCodeSites(sites, originals), "no longer the one written"));
    CHECK(first() == 2 && second() == 1);
    CHECK(WriteCodeWord(firstAt, kSayOne).empty());

    // Norbyte's three functions: his names, and nothing without a resolution.
    CHECK(CodePatchMappings().size() == 3);
    for (auto const& mapping : CodePatchMappings()) {
        CHECK(!ApplyCodePatch(mapping.Mapping) && !WasCodePatchApplied(mapping.Mapping) && !UndoCodePatch(mapping.Mapping));
        // Every site is a code site of the table that says what it becomes
        // and states the instruction it replaces.
        CHECK(!mapping.Sites.empty());
        for (auto name : mapping.Sites) {
            engine::Entry const* entry = nullptr;
            for (auto const& area : engine::EngineTable()) {
                for (auto const& candidate : area.Entries) {
                    if (std::string_view(candidate.Name) == name) entry = &candidate;
                }
            }
            CHECK(entry && entry->What == engine::Kind::CodeSite && entry->Becomes != 0 && entry->Derived);
            if (!entry || !entry->Derived) continue;
            auto const& d = *entry->Derived;
            CHECK(d.At < d.Pattern.size() && d.HostShape.Instructions != 0);
            CHECK(d.Pattern[d.At].Mask == 0xffffffff || (d.Anchor && d.AnchorAt == d.At));
            // The replacement is not what is there: a patch changes something.
            CHECK(!d.Pattern[d.At].Matches(entry->Becomes));
        }
        // A game where nothing resolved.
        engine::Resolution none;
        CHECK(!CodePatchRefusal(mapping.Mapping, none).empty());
    }
    CHECK(!ApplyCodePatch("no such mapping") && !WasCodePatchApplied("no such mapping") && !UndoCodePatch("no such mapping"));
    CHECK(Contains(CodePatchRefusal("no such mapping", engine::Resolution{}), "no such mapping"));
}

// The table's entry of that name.
engine::Entry const* TableEntry(std::string_view name)
{
    for (auto const& area : engine::EngineTable()) {
        for (auto const& entry : area.Entries) {
            if (name == entry.Name) return &entry;
        }
    }
    return nullptr;
}

TEST(the_patch_of_is_modded_is_every_function_that_holds_its_loop)
{
    // On Windows the patch changes one function, and every caller gets its
    // answer. In this game the function is inline in eight callers: the
    // mapping is all eight, each the branch that leaves the loop at a mod.
    constexpr std::string_view kIsStandardModule = "__ZNK2ls15ModuleShortDesc16IsStandardModuleEv";
    constexpr char const* kHosts[] = {
        "__ZN3esv18AchievementManager17UnlockAchievementERKN2ls11_StringViewIcEEPKNS_9CharacterE",
        "__ZN3esv18AchievementManager26IncreaseAchievementCounterEPKNS_9CharacterERKN2ls11FixedStringEtb",
        "__ZN3esv19OsirisGameFunctions17UnlockAchievementEPN3osi18ExecutionParameterE",
        "__ZN3esv13GameStateSave5EnterEv",
        "__ZN3esv15SavegameManager14FinishLoadGameEv",
        "__ZN3esv15SavegameManager10ThrowErrorEN3eoc14ESaveGameErrorEb",
        "__ZN3esv15SavegameManager10ThrowErrorEN3eoc14ESaveGameErrorEb",
        "__ZN3gui11DCSavegames6UpdateERKN2ls7AnyViewEf",
    };
    CodePatchMapping const* isModded = nullptr;
    for (auto const& mapping : CodePatchMappings()) {
        if (std::string_view(mapping.Mapping) == "ls::ModuleSettings::IsModded") isModded = &mapping;
    }
    CHECK(isModded && isModded->Sites.size() == std::size(kHosts));
    if (!isModded || isModded->Sites.size() != std::size(kHosts)) return;

    std::vector<engine::Derivation const*> derivations;
    for (size_t n = 0; n < isModded->Sites.size(); n++) {
        auto entry = TableEntry(isModded->Sites[n]);
        CHECK(entry && entry->Derived);
        if (!entry || !entry->Derived) return;
        auto const& d = *entry->Derived;
        derivations.push_back(&d);
        CHECK(std::string_view(d.Host.Symbol) == kHosts[n] && d.Host.How == engine::Find::ExactName);

        // The site is `tbz w0, #0, <out of the loop>`, stated in full, and it becomes a nop.
        CHECK(entry->Becomes == 0xd503201f);
        CHECK(d.At >= 1 && d.At < d.Pattern.size());
        if (d.At < 1 || d.At >= d.Pattern.size()) continue;
        CHECK(d.Pattern[d.At].Mask == 0xffffffff && (d.Pattern[d.At].Value & 0xfff8001f) == 0x36000000);
        // The instruction before it is a call, and the call is the one of IsStandardModule:
        // the host's only one, or the one an operand of the entry checks.
        auto const& call = d.Pattern[d.At - 1];
        CHECK(call.Value == 0x94000000 && call.Mask == Any::Branch);
        bool anchored = d.Anchor && d.Anchor.Function.Symbol && d.Anchor.Function.Symbol == kIsStandardModule && d.AnchorAt == d.At - 1;
        bool checked = false;
        for (auto const& operand : entry->Operands) {
            if (operand.How != engine::Operand::Read::BranchTarget) continue;
            checked = checked || (operand.In.Symbol && std::string_view(operand.In.Symbol) == d.Host.Symbol
                && operand.Pattern.data() == d.Pattern.data() && operand.Pattern.size() == d.Pattern.size() && operand.At == d.At - 1
                && operand.Target.Symbol && operand.Target.Symbol == kIsStandardModule);
        }
        CHECK(anchored || checked);
    }

    // No site twice. Two sites of one host are found by patterns that cannot
    // match the same instructions: an instruction both state in full differs.
    for (size_t a = 0; a < derivations.size(); a++) {
        for (size_t b = a + 1; b < derivations.size(); b++) {
            CHECK(std::string_view(isModded->Sites[a]) != isModded->Sites[b]);
            auto const& first = *derivations[a];
            auto const& second = *derivations[b];
            if (std::string_view(first.Host.Symbol) != second.Host.Symbol) continue;
            CHECK(!first.Anchor && !second.Anchor && first.At == second.At && first.Pattern.size() == second.Pattern.size());
            bool apart = false;
            for (size_t n = 0; n < first.Pattern.size() && n < second.Pattern.size(); n++) {
                apart = apart || (first.Pattern[n].Mask == 0xffffffff && second.Pattern[n].Mask == 0xffffffff
                    && first.Pattern[n].Value != second.Pattern[n].Value);
            }
            CHECK(apart);
        }
    }

    // The patch of ThrowError is in the same function and is another instruction.
    for (auto const& mapping : CodePatchMappings()) {
        if (&mapping == isModded) continue;
        for (auto name : mapping.Sites) {
            for (auto site : isModded->Sites) CHECK(std::string_view(name) != site);
        }
    }

    // Every code site of the table's patch area is a site of one mapping: none is found and then left unwritten.
    for (auto const& area : engine::EngineTable()) {
        if (std::string_view(area.Name) != engine::patches::kArea) continue;
        CHECK(!area.Entries.empty());
        for (auto const& entry : area.Entries) {
            size_t uses = 0;
            for (auto const& mapping : CodePatchMappings()) {
                for (auto name : mapping.Sites) uses += std::string_view(name) == entry.Name;
            }
            CHECK(uses == 1);
        }
    }
}

TEST(game_version_is_the_bundles_and_support_is_the_resolution)
{
    // The macOS game's version string, with its build number of more than 16 bits.
    auto version = ParseGameVersion("4.1.1.7631656");
    CHECK(version && version->Major == 4 && version->Minor == 1 && version->Revision == 1 && version->Build == 7631656);
    auto shorter = ParseGameVersion("4.69");
    CHECK(shorter && shorter->Major == 4 && shorter->Minor == 69 && shorter->Revision == 0 && shorter->Build == 0);
    auto largest = ParseGameVersion("0.0.0.4294967295");
    CHECK(largest && largest->Build == 4294967295u);
    for (char const* text : { "", ".", "4.", ".4", "4..1", "4.1.1.1.1", "v4.1", "4.1 ", "4.1-beta", "4.-1", "0.0.0.4294967296", "99999999999999999999" }) {
        CHECK(!ParseGameVersion(text));
    }
    // This test does not run from a bundle with a version.
    CHECK(GameVersionString().empty() && !ReadGameVersion());

    // Supported: the start-up sequence handed over a resolution of the table.
    CHECK(!GameSupported());
    engine::Resolution resolution;
    SetResolutionForLibraries(&resolution);
    CHECK(GameSupported());
    SetResolutionForLibraries(nullptr);
    CHECK(!GameSupported());
}

TEST(a_refusal_that_concerns_the_player_has_windows_notice)
{
    // A game this build cannot work with: that it starts without bg3mse, and
    // what to do, from the game version the release was checked against.
    std::string const norbyte = "bg3mse doesn't support this game version yet (v4.1.1.7631656). Starting without it. ";
    for (auto refusal : { StartRefusal::GameUnreadable, StartRefusal::CriticalMember, StartRefusal::GameLocks }) {
        auto notice = NoticeOf(refusal, "4.1.1.7631656");
        CHECK(notice && notice->Caption == "bg3mse" && notice->Text == norbyte + "Check for a newer bg3mse.");
        auto unversioned = NoticeOf(refusal, "");
        CHECK(unversioned && unversioned->Text == "bg3mse doesn't support this game version yet. Starting without it. Check for a newer bg3mse.");
        auto older = NoticeOf(refusal, "4.1.1.7631656", "4.1.1.7700000");
        CHECK(older && older->Text == "bg3mse doesn't support this game version (v4.1.1.7631656). Starting without it. Update the game in Steam.");
        auto newer = NoticeOf(refusal, "4.1.1.7631656", "4.1.1.7000000");
        CHECK(newer && newer->Text == norbyte + "It'll update itself once a new version is out.");
        auto downloaded = NoticeOf(refusal, "4.1.1.7631656", "4.1.1.7000000", false);
        CHECK(downloaded && downloaded->Text == norbyte + "Download the new bg3mse once it's out.");
    }
    CHECK(bg3se::mac::CheckedGameVersion("{\"game_build_checked\" : \"4.1.1.7631656\", \"macos_minimum\" : \"11.0\"}") == "4.1.1.7631656");
    CHECK(bg3se::mac::CheckedGameVersion("{}").empty());
    CHECK(bg3se::mac::SelfUpdating("{\"self_update\" : true}") && bg3se::mac::SelfUpdating("{}"));
    CHECK(!bg3se::mac::SelfUpdating("{\"name\" : \"bg3mse-0.1.0\", \"self_update\" : false}"));
    CHECK(bg3se::mac::UpdatesDisabled("{ \"DisableUpdates\": true }") && !bg3se::mac::UpdatesDisabled("{ \"DisableUpdates\": false }")
        && !bg3se::mac::UpdatesDisabled("{ \"ManifestURL\": \"x\" }"));
    // This layer's own failure.
    for (auto refusal : { StartRefusal::RowsDisagree, StartRefusal::HooksFailed }) {
        auto notice = NoticeOf(refusal, "4.1.1.7631656");
        CHECK(notice && notice->Caption == "bg3mse" && notice->Text == "bg3mse failed to start. The game will run without it.");
    }
    // Not the game, already running, or started: nothing to tell.
    for (auto refusal : { StartRefusal::None, StartRefusal::AlreadyStarted, StartRefusal::NotTheGameOrAlreadyRunning }) {
        CHECK(!NoticeOf(refusal, "4.1.1.7631656"));
    }
    // Every refusal is placed.
    static_assert(static_cast<int>(StartRefusal::Count) == 8);
}

}

int main()
{
    game_version_is_the_bundles_and_support_is_the_resolution_run();
    a_refusal_that_concerns_the_player_has_windows_notice_run();
    code_patches_go_in_whole_or_not_at_all_and_come_out_run();
    the_patch_of_is_modded_is_every_function_that_holds_its_loop_run();
    prototype_places_parameters_as_the_platform_does_run();
    disagreement_names_what_differs_run();
    rows_hold_together_with_the_engine_table_run();
    adapter_fixed_string_get_string_run();
    adapter_main_table_create_from_string_run();
    adapter_surface_action_factory_create_action_run();
    adapter_ai_grid_find_path_immediate_run();
    adapter_applied_material_textures_run();
    adapter_texture_manager_unload_texture_run();
    parse_static_boosts_hands_parse_boosts_the_function_object_the_game_builds_run();
    set_property_string_finds_the_attribute_as_the_game_does_or_sets_nothing_run();
    set_property_string_sets_each_type_of_attribute_as_a_data_line_does_run();
    a_passive_prototype_is_filled_from_its_stats_object_as_the_stats_job_fills_it_run();
    an_interrupt_prototype_is_filled_from_its_stats_object_as_the_stats_job_fills_it_run();
    a_function_the_game_has_inline_is_served_only_with_the_code_it_was_read_from_run();
    adapter_noesis_load_xaml_run();
    adapter_global_allocator_is_the_c_library_run();
    hooks_pass_the_game_arguments_through_an_unwrapped_bridge_run();
    the_core_library_hook_puts_the_fault_handler_in_front_again_run();
    the_exit_handler_runs_before_the_static_data_is_shut_down_run();
    the_exit_handler_runs_before_the_game_terminates_when_the_player_quits_run();
    client_update_handlers_run_at_their_two_places_run();
    a_wrapped_bridge_runs_norbytes_handler_and_the_game_function_run();
    redirect_call_changes_one_call_and_refuses_anything_else_run();
    template_bank_index_is_the_global_or_the_threads_run();
    one_refused_entry_takes_its_members_and_no_others_run();
    story_rows_hold_together_with_the_engine_table_run();
    norbytes_story_containers_read_and_build_the_librarys_run();
    adapter_osiris_add_string_run();
    story_hooks_and_bridges_run();
    vtable_slots_are_wrapped_one_by_one_all_or_none_run();
    the_story_layer_goes_without_only_the_hooks_his_code_can_do_without_run();
    a_node_class_that_is_not_proven_takes_the_node_hooks_and_nothing_else_run();
    without_a_resolved_game_the_story_layer_gets_nothing_run();
    a_game_without_the_symbols_gets_nothing_run();
    type_contexts_are_the_tables_and_named_as_norbytes_code_compares_them_run();
    type_names_reach_their_index_variables_by_the_games_rule_run();
    an_ambiguous_type_name_stays_unknown_run();
    a_refused_context_leaves_its_types_unknown_run();
    type_indices_are_taken_only_from_what_the_game_numbered_run();
    a_status_effect_object_is_classed_by_its_vtable_alone_run();
    the_connect_message_is_wrapped_only_through_its_own_vtable_run();
    networking_is_withheld_on_a_game_whose_session_is_not_proven_run();
    an_unproven_layout_keeps_back_what_leads_to_it_run();
    a_component_whose_members_the_game_contradicts_is_withheld_run();
    every_unproven_shape_keeps_something_back_run();
    every_class_of_the_slot_check_keeps_something_back_run();
    every_bound_type_has_a_name_run();
    if (gFailures) {
        printf("%d check(s) failed\n", gFailures);
        return 1;
    }
    printf("binding tests passed\n");
    return 0;
}
