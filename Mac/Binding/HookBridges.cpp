#include <Binding/HookBridges.h>

#include <atomic>
#include <pthread.h>
#include <cstring>
#include <string>
#include <vector>

#include <Engine/Entries/OsirisNames.h>
#include <Platform/CodeWriter.h>
#include <Platform/FaultObserver.h>
#include <Platform/HookPlanner.h>
#include <Platform/Islands.h>
#include <Platform/Log.h>

#include <detours.h>

namespace bg3se::mac::binding {

namespace {

// --- the originals -----------------------------------------------------------

using CoreLibInitProc = void(void const* arguments);
using UpdateRootsProc = void();
using LoadGraphicSettingsConfigProc = bool(void* app, void* init);
using TimedUpdateProc = void(void* self, void const* time);
using StatsStartProc = void(void* stats, void const* paths);
using ParseDataBufferProc = void(void* result, void* stats, void* buffer);
using NotifySkeletonUpdatedProc = void(void* visual, bool flag);

CoreLibInitProc* gCoreLibInitNext{ nullptr };
UpdateRootsProc* gStaticDataShutdownNext{ nullptr };
// What runs at the game's exit (SetGameExitHandler).
std::atomic<void (*)(GameExit)> gGameExitHandler{ nullptr };
// The thread that last ran the client's update: the game thread.
std::atomic<pthread_t> gGameThread{ nullptr };
using AppTerminateProc = void(void* app, int code);
AppTerminateProc* gAppTerminateNext{ nullptr };
UpdateRootsProc* gUpdateRootsNext{ nullptr };
LoadGraphicSettingsConfigProc* gLoadGraphicSettingsNext{ nullptr };
TimedUpdateProc* gClientPreUpdateNext{ nullptr };
TimedUpdateProc* gClientWorldUpdateNext{ nullptr };
StatsStartProc* gStatsStartNext{ nullptr };
ParseDataBufferProc* gParseDataBufferNext{ nullptr };
NotifySkeletonUpdatedProc* gNotifySkeletonUpdatedNext{ nullptr };
TimedUpdateProc* gGameUIUpdateFromUINext{ nullptr };

using OsirisOpenLogFileProc = bool(void* osiris, wchar_t const* path, uint32_t mode);
using WriteHeaderProc = bool(void const* manager, void const* path);
using WriterCloseProc = void(void* writer);

OsirisOpenLogFileProc* gOsirisOpenLogFileNext{ nullptr };
WriteHeaderProc* gWriteHeaderNext{ nullptr };
WriterCloseProc* gStoryWriterCloseNext{ nullptr };
std::atomic<ptrdiff_t> gStoryPathFromWriter{ 0 };

// --- the bridges, as the hooks call them -------------------------------------
//
// Through pointers the compiler cannot see through: a call of a bridge must
// reach the bridge's first instruction, where Norbyte's detour is, and must
// not be dropped or specialised because the body is known to do little.

void* (*volatile gAppCtor)(void*) = &bridge::AppCtor;
void (*volatile gAppUpdatePaths)(void*) = &bridge::AppUpdatePaths;
void (*volatile gAppLoadGraphicSettings)(void*) = &bridge::AppLoadGraphicSettings;
void (*volatile gRPGStatsLoad)(void*, void*) = &bridge::RPGStatsLoad;
bool (*volatile gRPGStatsParseDataBuffers)(void*) = &bridge::RPGStatsParseDataBuffers;
void (*volatile gVisualUpdateBlendshapeWeights)(void*) = &bridge::VisualUpdateBlendshapeWeightsFromSkeleton;
void (*volatile gUiPredicatesManagerUpdate)(void*) = &bridge::UiPredicatesManagerUpdateFromUI;
void (*volatile gOsirisOpenLogFile)(void*, wchar_t const*, uint32_t) = &bridge::OsirisOpenLogFile;
void* (*volatile gStoryCreateFile)(wchar_t const*, uint32_t, uint32_t, void*, uint32_t, uint32_t, void*) = &bridge::StoryCreateFile;
int (*volatile gStoryCloseHandle)(void*) = &bridge::StoryCloseHandle;

// --- frames ------------------------------------------------------------------

struct GraphicSettingsFrame
{
    void* Init;
    // What the game's caller gets when Norbyte's wrapper does not call the
    // original: the function's only result in this game is true.
    bool Result{ true };
};
struct SkeletonFrame
{
    bool Flag;
};
struct GameUIFrame
{
    void* UI;
    void const* Time;
};

struct OpenLogFileFrame
{
    bool Result{ false };
};
// One story file the game has closed: its address is the handle.
struct StoryFileFrame
{
};

thread_local GraphicSettingsFrame* tGraphicSettings{ nullptr };
thread_local OpenLogFileFrame* tOpenLogFile{ nullptr };
thread_local StoryFileFrame* tStoryFile{ nullptr };
thread_local SkeletonFrame* tSkeleton{ nullptr };
thread_local GameUIFrame* tGameUI{ nullptr };
// Set while RPGStats::Start runs on this thread and has not yet reached its
// first RPGStats::ParseDataBuffer call.
thread_local bool tStatsLoadBeforeParse{ false };

template <class Frame>
struct Entered
{
    Entered(Frame*& slot, Frame* frame) : slot_(slot), outer_(slot) { slot = frame; }
    ~Entered() { slot_ = outer_; }
    Frame*& slot_;
    Frame* outer_;
};

// --- the client's update handlers --------------------------------------------

struct ClientUpdateHandlers
{
    void* Context;
    UpdateHandler* Pre;
    UpdateHandler* Post;
};
std::atomic<ClientUpdateHandlers const*> gClientUpdate{ nullptr };

}

namespace {

// UTF-8 to the 32-bit characters wchar_t has here. False for anything that is
// not well-formed UTF-8.
bool WideFromUtf8(char const* text, std::wstring& wide)
{
    static_assert(sizeof(wchar_t) == 4);
    if (!text) return false;
    auto at = reinterpret_cast<unsigned char const*>(text);
    while (*at) {
        uint32_t point = *at++;
        int more = 0;
        if (point >= 0xf0) { more = 3; point &= 0x07; }
        else if (point >= 0xe0) { more = 2; point &= 0x0f; }
        else if (point >= 0xc0) { more = 1; point &= 0x1f; }
        else if (point >= 0x80) return false;
        uint32_t least = more == 3 ? 0x10000 : more == 2 ? 0x800 : more == 1 ? 0x80 : 0;
        for (; more > 0; more--) {
            if ((*at & 0xc0) != 0x80) return false;
            point = (point << 6) | (*at++ & 0x3f);
        }
        if (point < least || point > 0x10ffff || (point >= 0xd800 && point < 0xe000)) return false;
        wide.push_back(static_cast<wchar_t>(point));
    }
    return true;
}

}

// --- bridge bodies -----------------------------------------------------------

#define BG3MSE_BRIDGE __attribute__((noinline))

namespace bridge {

BG3MSE_BRIDGE void* AppCtor(void* app)
{
    return app;
}

BG3MSE_BRIDGE void AppUpdatePaths(void*)
{
    if (gUpdateRootsNext) gUpdateRootsNext();
}

BG3MSE_BRIDGE void AppLoadGraphicSettings(void* app)
{
    auto frame = tGraphicSettings;
    if (frame && gLoadGraphicSettingsNext) frame->Result = gLoadGraphicSettingsNext(app, frame->Init);
}

BG3MSE_BRIDGE void RPGStatsLoad(void* stats, void* paths)
{
    if (gStatsStartNext) gStatsStartNext(stats, paths);
}

BG3MSE_BRIDGE bool RPGStatsParseDataBuffers(void*)
{
    return true;
}

BG3MSE_BRIDGE void VisualUpdateBlendshapeWeightsFromSkeleton(void* visual)
{
    auto frame = tSkeleton;
    if (frame && gNotifySkeletonUpdatedNext) gNotifySkeletonUpdatedNext(visual, frame->Flag);
}

BG3MSE_BRIDGE void UiPredicatesManagerUpdateFromUI(void*)
{
    auto frame = tGameUI;
    if (frame && gGameUIUpdateFromUINext) gGameUIUpdateFromUINext(frame->UI, frame->Time);
}

// Also what Norbyte's own calls of the original reach (he opens the
// extender's log through it): there is no frame then, and no caller that
// reads the answer.
BG3MSE_BRIDGE void OsirisOpenLogFile(void* osiris, wchar_t const* path, uint32_t mode)
{
    if (!gOsirisOpenLogFileNext) return;
    bool opened = gOsirisOpenLogFileNext(osiris, path, mode);
    if (auto frame = tOpenLogFile) frame->Result = opened;
}

BG3MSE_BRIDGE void* StoryCreateFile(wchar_t const*, uint32_t, uint32_t, void*, uint32_t, uint32_t, void*)
{
    return tStoryFile;
}

BG3MSE_BRIDGE int StoryCloseHandle(void*)
{
    return TRUE;
}

}

// --- the hooks on the game ---------------------------------------------------

namespace hook {

// CoreLibSDM::Init(ls::ArgumentString const&). When it returns, the string
// table and the thread registry exist and the App object does not: the moment
// of Norbyte's pre-hook on the App constructor.
//
// It is also the first moment of bg3mse's own after the game installed its
// crash handler (-[AppDelegate enableCrashReporting], before launchGame),
// which is in front of bg3mse's from then on and passes nothing on
// (Platform/FaultObserver.h). Norbyte's start-up goes on below: its faults
// are the crash reporter's to see, so bg3mse's handler is put in front again
// first. Without a started reporter there are no observers, and this does
// nothing.
void CoreLibInit(void const* arguments)
{
    gCoreLibInitNext(arguments);
    PutFatalFaultObserversInFront();
    gAppCtor(nullptr);
}

// ls::StaticDataManager::Shutdown(), which -[AppDelegate launchGame] calls
// once, on the main thread, when the game exits normally: after BaseApp::Stop
// has joined the game thread and ended the worker threads and the App has
// been destroyed, before the static data, the string table and the allocator
// are ended, and right before _exit(0) (Engine/Entries/Startup.cpp). The
// extender's shutdown runs first.
void StaticDataShutdown()
{
    if (auto handler = gGameExitHandler.load(std::memory_order_acquire)) handler(GameExit::Teardown);
    gStaticDataShutdownNext();
}

// App::Terminate(int), which ecl::EocClient::StartTerminatingApp calls on the
// game thread when the player quits, and which ends in _Exit (Engine/Entries/
// Startup.cpp). The extender's shutdown runs first.
void AppTerminate(void* app, int code)
{
    if (auto handler = gGameExitHandler.load(std::memory_order_acquire)) handler(GameExit::Quit);
    gAppTerminateNext(app, code);
}

// ls::Path::UpdateRoots(), a static function: Norbyte's handler does not use
// the App it is declared with.
void PathUpdateRoots()
{
    gAppUpdatePaths(nullptr);
}

// App::LoadGraphicSettingsConfig(ls::InitStruct&), which returns a bool.
bool AppLoadGraphicSettingsConfig(void* app, void* init)
{
    GraphicSettingsFrame frame{ init };
    Entered entered(tGraphicSettings, &frame);
    gAppLoadGraphicSettings(app);
    return frame.Result;
}

// ecl::GameClient::PreUpdate(ls::GameTime const&), the call App::Update makes
// before the state machine's update: Norbyte's pre-hook runs when it returns.
void GameClientPreUpdate(void* client, void const* time)
{
    gGameThread.store(pthread_self(), std::memory_order_relaxed);
    gClientPreUpdateNext(client, time);
    if (auto handlers = gClientUpdate.load(std::memory_order_acquire); handlers && handlers->Pre) {
        handlers->Pre(handlers->Context, nullptr, time);
    }
}

// The one call of ecs::EntityWorld::Update in App::Update, the first thing
// after the state machine's update: Norbyte's post-hook runs before it.
void ClientWorldUpdate(void* world, void const* time)
{
    if (auto handlers = gClientUpdate.load(std::memory_order_acquire); handlers && handlers->Post) {
        handlers->Post(handlers->Context, nullptr, time);
    }
    gClientWorldUpdateNext(world, time);
}

// RPGStats::Start(ls::ObjectSet<ls::Path> const&).
void RPGStatsStart(void* stats, void const* paths)
{
    bool outer = tStatsLoadBeforeParse;
    tStatsLoadBeforeParse = true;
    gRPGStatsLoad(stats, const_cast<void*>(paths));
    tStatsLoadBeforeParse = outer;
}

// The call of RPGStats::ParseDataBuffer in RPGStats::Start, which runs once
// for each buffer: every stats file is read by then and none is parsed before
// the first.
void RPGStatsParseDataBuffer(void* result, void* stats, void* buffer)
{
    if (tStatsLoadBeforeParse) {
        tStatsLoadBeforeParse = false;
        gRPGStatsParseDataBuffers(stats);
    }
    gParseDataBufferNext(result, stats, buffer);
}

// ls::Visual::NotifySkeletonUpdated(bool).
void VisualNotifySkeletonUpdated(void* visual, bool flag)
{
    SkeletonFrame frame{ flag };
    Entered entered(tSkeleton, &frame);
    gVisualUpdateBlendshapeWeights(visual);
}

// gui::GameUI::UpdateFromUI(ls::GameTime const&).
void GameUIUpdateFromUI(void* ui, void const* time)
{
    GameUIFrame frame{ ui, time };
    Entered entered(tGameUI, &frame);
    gUiPredicatesManagerUpdate(nullptr);
}

// COsiris::OpenLogFile(wchar_t const*, unsigned int), which returns a bool.
bool OsirisOpenLogFile(void* osiris, wchar_t const* path, uint32_t mode)
{
    OpenLogFileFrame frame;
    Entered entered(tOpenLogFile, &frame);
    gOsirisOpenLogFile(osiris, path, mode);
    return frame.Result;
}

// The game has written and closed the file at `path` (an ls::Path): what the
// Windows game does with CreateFileW and CloseHandle, told to Norbyte's hooks
// on those two.
void StoryFileClosed(void const* path)
{
    std::wstring wide;
    if (!WideFromUtf8(GamePathText(path), wide) || wide.empty()) {
        Log(LogLevel::Warning, "a story file's path is not UTF-8 text: the story hooks are not told of it");
        return;
    }
    StoryFileFrame frame;
    Entered entered(tStoryFile, &frame);
    void* handle = gStoryCreateFile(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle) gStoryCloseHandle(handle);
}

// osi::OsirisManager::WriteHeader(ls::Path const&) const: opens the file,
// writes it, closes it, and returns whether it could open it.
bool StoryWriteHeader(void const* manager, void const* path)
{
    bool written = gWriteHeaderNext(manager, path);
    if (written) StoryFileClosed(path);
    return written;
}

// The call of ls::FileWriter::Close() in
// esv::StoryImplementation::ComposeAndBuildStory(bool) that closes the writer
// of Story/story_generated.div. The function reaches the call only when the
// writer had opened its file.
void StoryWriterClose(void* writer)
{
    gStoryWriterCloseNext(writer);
    if (auto offset = gStoryPathFromWriter.load(std::memory_order_acquire)) {
        StoryFileClosed(static_cast<char const*>(writer) + offset);
    }
}

}

char const* GamePathText(void const* path)
{
    auto bytes = static_cast<char const*>(path);
    if (static_cast<signed char>(bytes[kGamePathTextFlag]) >= 0) return bytes;
    char const* text;
    memcpy(&text, bytes, sizeof(text));
    return text;
}

void SetStoryPathFromWriter(ptrdiff_t offset)
{
    gStoryPathFromWriter.store(offset, std::memory_order_release);
}

void TakeGameThreadForTest()
{
    gGameThread.store(pthread_self(), std::memory_order_relaxed);
}

bool OnGameThread()
{
    auto game = gGameThread.load(std::memory_order_relaxed);
    return game != nullptr && pthread_equal(game, pthread_self());
}

void SetGameExitHandler(void (*handler)(GameExit how))
{
    gGameExitHandler.store(handler, std::memory_order_release);
}

void SetClientUpdateHandlers(void* context, UpdateHandler* pre, UpdateHandler* post)
{
    // Never freed: a thread of the game may be reading the one it replaces.
    gClientUpdate.store(new ClientUpdateHandlers{ context, pre, post }, std::memory_order_release);
}

// --- the tables --------------------------------------------------------------

namespace {

constexpr char kCoreLibInit[] = "CoreLibSDM::Init";
constexpr char kUpdateRoots[] = "ls::Path::UpdateRoots";
constexpr char kLoadGraphicSettings[] = "App__LoadGraphicSettings";
constexpr char kClientPreUpdate[] = "ecl::GameClient::PreUpdate";
constexpr char kClientWorldUpdate[] = "App::Update: call of ecs::EntityWorld::Update";
constexpr char kStatsStart[] = "RPGStats__Load";
constexpr char kParseDataBuffer[] = "RPGStats::Start: call of RPGStats::ParseDataBuffer";
constexpr char kNotifySkeletonUpdated[] = "ls::Visual::NotifySkeletonUpdated";
constexpr char kGameUIUpdateFromUI[] = "gui::GameUI::UpdateFromUI";
constexpr char kOsirisOpenLogFile[] = "Osiris.OpenLogFile";
constexpr auto& kStoryWriteHeader = engine::osiris::kWriteHeader;
constexpr auto& kStoryWriterClose = engine::osiris::kCloseStory;
constexpr char kStaticDataShutdown[] = "ls::StaticDataManager::Shutdown";
constexpr char kAppTerminate[] = "App::Terminate";

#define BG3MSE_GAME_HOOK(entry, at, detour, next) \
    GameHook{ entry, at, kPrototypeOf<decltype(hook::detour)>, reinterpret_cast<void*>(&hook::detour), reinterpret_cast<void**>(&next) }

const GameHook kGameHooks[] = {
    BG3MSE_GAME_HOOK(kCoreLibInit, HookAt::Entry, CoreLibInit, gCoreLibInitNext),
    BG3MSE_GAME_HOOK(kUpdateRoots, HookAt::Entry, PathUpdateRoots, gUpdateRootsNext),
    BG3MSE_GAME_HOOK(kLoadGraphicSettings, HookAt::Entry, AppLoadGraphicSettingsConfig, gLoadGraphicSettingsNext),
    BG3MSE_GAME_HOOK(kClientPreUpdate, HookAt::Entry, GameClientPreUpdate, gClientPreUpdateNext),
    BG3MSE_GAME_HOOK(kClientWorldUpdate, HookAt::CallSite, ClientWorldUpdate, gClientWorldUpdateNext),
    BG3MSE_GAME_HOOK(kStatsStart, HookAt::Entry, RPGStatsStart, gStatsStartNext),
    BG3MSE_GAME_HOOK(kParseDataBuffer, HookAt::CallSite, RPGStatsParseDataBuffer, gParseDataBufferNext),
    BG3MSE_GAME_HOOK(kNotifySkeletonUpdated, HookAt::Entry, VisualNotifySkeletonUpdated, gNotifySkeletonUpdatedNext),
    BG3MSE_GAME_HOOK(kGameUIUpdateFromUI, HookAt::Entry, GameUIUpdateFromUI, gGameUIUpdateFromUINext),
    BG3MSE_GAME_HOOK(kOsirisOpenLogFile, HookAt::Entry, OsirisOpenLogFile, gOsirisOpenLogFileNext),
    BG3MSE_GAME_HOOK(kStoryWriteHeader, HookAt::Entry, StoryWriteHeader, gWriteHeaderNext),
    BG3MSE_GAME_HOOK(kStoryWriterClose, HookAt::CallSite, StoryWriterClose, gStoryWriterCloseNext),
    BG3MSE_GAME_HOOK(kStaticDataShutdown, HookAt::Entry, StaticDataShutdown, gStaticDataShutdownNext),
    BG3MSE_GAME_HOOK(kAppTerminate, HookAt::Entry, AppTerminate, gAppTerminateNext),
};
bool gAttached[std::size(kGameHooks)]{};

#define BG3MSE_HOOK_BRIDGE(member, function, ...) \
    HookBridge{ member, kPrototypeOf<decltype(bridge::function)>, reinterpret_cast<void*>(&bridge::function), { __VA_ARGS__ } }

const HookBridge kHookBridges[] = {
    BG3MSE_HOOK_BRIDGE("App__Ctor", AppCtor, kCoreLibInit),
    BG3MSE_HOOK_BRIDGE("App__UpdatePaths", AppUpdatePaths, kUpdateRoots),
    BG3MSE_HOOK_BRIDGE("App__LoadGraphicSettings", AppLoadGraphicSettings, kLoadGraphicSettings),
    BG3MSE_HOOK_BRIDGE("RPGStats__Load", RPGStatsLoad, kStatsStart),
    BG3MSE_HOOK_BRIDGE("RPGStats__ParseDataBuffers", RPGStatsParseDataBuffers, kStatsStart, kParseDataBuffer),
    BG3MSE_HOOK_BRIDGE("ls__Visual__UpdateBlendshapeWeightsFromSkeleton", VisualUpdateBlendshapeWeightsFromSkeleton, kNotifySkeletonUpdated),
    BG3MSE_HOOK_BRIDGE("ui__deferred__PredicatesManager__UpdateFromUI", UiPredicatesManagerUpdateFromUI, kGameUIUpdateFromUI),
    BG3MSE_HOOK_BRIDGE("Osiris.OpenLogFile", OsirisOpenLogFile, kOsirisOpenLogFile),
    BG3MSE_HOOK_BRIDGE("Osiris.CreateFileW", StoryCreateFile, kStoryWriteHeader, kStoryWriterClose),
    BG3MSE_HOOK_BRIDGE("Osiris.CloseHandle", StoryCloseHandle, kStoryWriteHeader, kStoryWriterClose),
};

constexpr char const* kClientUpdateHooks[] = { kClientPreUpdate, kClientWorldUpdate };

int IndexOfHook(std::string_view entry)
{
    for (size_t i = 0; i < std::size(kGameHooks); i++) {
        if (entry == kGameHooks[i].Entry) return static_cast<int>(i);
    }
    return -1;
}

void Say(std::string& report, char const* entry, std::string const& what)
{
    report += entry;
    report += ": ";
    report += what;
    report += '\n';
}

}

std::span<GameHook const> GameHooks() { return kGameHooks; }
std::span<HookBridge const> HookBridges() { return kHookBridges; }
std::span<char const* const> ClientUpdateHooks() { return kClientUpdateHooks; }

HookBridge const* FindHookBridge(std::string_view member)
{
    for (auto const& b : kHookBridges) {
        if (member == b.Member) return &b;
    }
    return nullptr;
}

bool GameHookAttached(std::string_view entry)
{
    int i = IndexOfHook(entry);
    return i >= 0 && gAttached[i];
}

bool BridgeDriven(HookBridge const& bridge)
{
    for (auto entry : bridge.Hooks) {
        if (entry && !GameHookAttached(entry)) return false;
    }
    return true;
}

void SetGameHookForTest(std::string_view entry, void* next)
{
    int i = IndexOfHook(entry);
    if (i < 0) return;
    *kGameHooks[i].Next = next;
    gAttached[i] = next != nullptr;
}

std::string GameHookDisagreement(GameHook const& hook, engine::Entry const* entry, engine::Entry const* callee)
{
    if (!entry) return "the engine table has no such entry";
    auto taken = entry;
    if (hook.At == HookAt::CallSite) {
        if (entry->What != engine::Kind::CallSite || !entry->Needs) return "the entry is not a call site with the function it calls";
        if (!callee) return "the engine table has no entry for the function the call calls";
        taken = callee;
    } else if (entry->What != engine::Kind::FunctionToHook && entry->What != engine::Kind::FunctionToCallAndHook) {
        return "the entry is not a function to hook";
    }
    return Disagreement(hook.Game, taken->Call, Through::Hook, kOwnPrototype);
}

std::string GameHookRefusal(GameHook const& hook, engine::Resolution const& resolution)
{
    auto entry = resolution.Find(hook.Entry);
    if (!entry) return "the engine table has no such entry";
    if (!entry->Usable()) return "not resolved: " + entry->Reason;
    auto callee = entry->Of->Needs ? resolution.Find(entry->Of->Needs) : nullptr;
    if (auto problem = GameHookDisagreement(hook, entry->Of, callee ? callee->Of : nullptr); !problem.empty()) return problem;
    if (hook.At == HookAt::Entry) {
        if (entry->Hook != HookRefusal::None) return std::string("an entry hook is refused: ") + mac::Describe(entry->Hook);
    } else if (!callee || !callee->Usable()) {
        return "the function the call calls is not resolved";
    }
    return {};
}

std::string RedirectCall(void* site, void const* callee, void const* replacement)
{
    auto at = reinterpret_cast<uint64_t>(site);
    uint32_t word;
    memcpy(&word, site, sizeof(word));
    if ((word & 0xfc000000u) != 0x94000000u) return "the instruction is not a call";
    if (DirectBranchTarget(at, word) != reinterpret_cast<uint64_t>(callee)) return "the call does not go to the function expected";
    std::string problem;
    void* pad = MakeJumpPad(site, replacement, problem);
    if (!pad) return problem;
    uint32_t call = EncodeBranch(at, reinterpret_cast<uint64_t>(pad), true);
    if (call == 0) return "the jump pad is out of the call's range";
    return WriteCodeWord(site, call);
}

bool AttachGameHooks(engine::Resolution const& resolution, std::string& report)
{
    struct Planned
    {
        size_t Index;
        void* Target;       // the function, or the call instruction
        void* Callee;       // call sites: the function called
        uint32_t Word{ 0 }; // call sites: the instruction before the change
        bool Written{ false };
    };
    std::vector<Planned> entries, sites;

    for (size_t i = 0; i < std::size(kGameHooks); i++) {
        auto const& hook = kGameHooks[i];
        if (gAttached[i]) continue;
        if (auto problem = GameHookRefusal(hook, resolution); !problem.empty()) {
            Say(report, hook.Entry, problem);
            continue;
        }
        auto entry = resolution.Find(hook.Entry);
        void* target = resolution.Address(*entry);
        void* callee = hook.At == HookAt::CallSite ? resolution.Address(*resolution.Find(entry->Of->Needs)) : nullptr;
        if (!target || (hook.At == HookAt::CallSite && !callee)) {
            Say(report, hook.Entry, "its image is not loaded in this process");
            continue;
        }
        (hook.At == HookAt::Entry ? entries : sites).push_back({ i, target, callee });
    }

    // A hook's Next is never taken back once it is set: it holds the game's
    // function, or the trampoline of a hook that is in, and the Detours layer
    // keeps it so whatever becomes of a transaction. A hook that could not be
    // taken out again still has a function to pass its call on to.

    if (!entries.empty()) {
        LONG status = DetourTransactionBegin();
        if (status != NO_ERROR) {
            report += "the Detours transaction could not be opened\n";
            return false;
        }
        for (auto const& p : entries) {
            auto const& hook = kGameHooks[p.Index];
            *hook.Next = p.Target;
            status = DetourAttach(hook.Next, hook.Detour);
            if (status != NO_ERROR) {
                Say(report, hook.Entry, "the Detours layer refused the hook; no hook was made");
                DetourTransactionAbort();
                return false;
            }
        }
        status = DetourTransactionCommit();
        if (status != NO_ERROR) {
            report += "the Detours transaction failed; the lines of DetourTransactionCommit in the log say for each function whether "
                      "its hook is in. A hook left in passes every call on unchanged\n";
            return false;
        }
    }

    bool failed = false;
    for (auto& p : sites) {
        auto const& hook = kGameHooks[p.Index];
        memcpy(&p.Word, p.Target, sizeof(p.Word));
        *hook.Next = p.Callee;
        if (auto problem = RedirectCall(p.Target, p.Callee, hook.Detour); !problem.empty()) {
            Say(report, hook.Entry, problem);
            failed = true;
            break;
        }
        p.Written = true;
    }
    if (failed) {
        // Take out what went in: the call sites first, then the entry hooks.
        for (auto const& p : sites) {
            if (!p.Written) continue;
            if (auto problem = WriteCodeWord(p.Target, p.Word); !problem.empty()) Say(report, kGameHooks[p.Index].Entry, "left redirected: " + problem);
        }
        if (!entries.empty()) {
            bool out = DetourTransactionBegin() == NO_ERROR;
            if (out) {
                for (auto const& p : entries) DetourDetach(kGameHooks[p.Index].Next, kGameHooks[p.Index].Detour);
                out = DetourTransactionCommit() == NO_ERROR;
            }
            if (!out) report += "the entry hooks could not be taken out again; they pass every call on unchanged\n";
        }
        return false;
    }

    for (auto const& p : entries) gAttached[p.Index] = true;
    for (auto const& p : sites) gAttached[p.Index] = true;
    return true;
}

}
