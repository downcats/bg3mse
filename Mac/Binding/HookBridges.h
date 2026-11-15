#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <Binding/Prototype.h>
#include <Engine/EngineResolver.h>
#include <Layout/GameMembers.h>

// The places Norbyte hooks that are not, in the macOS game, a function with
// the prototype he declares: a function that takes other arguments or returns
// something, a function standing in for one that was inlined, or one call
// instruction inside a function.
//
// Norbyte's hooks are WrappableFunctions: Wrap(address) detours the function
// at that address with a function of his prototype, which runs his handler
// and calls the original through the trampoline, again with his prototype.
// For each such place there is a bridge here: a function of Norbyte's
// prototype, in this image, whose address StaticSymbols holds. His Wrap
// detours the bridge, through the Detours layer like any other function.
//
// The game side is a hook of this layer with the game function's own argument
// passing. When the game runs the place, the hook keeps the arguments
// Norbyte's prototype has no room for in a frame on its own stack, and calls
// the bridge with Norbyte's arguments. Detoured, the bridge is Norbyte's
// wrapper: his handler runs, and where it calls the original it reaches the
// bridge's own body, which calls the game function with the arguments it was
// given and the ones in the frame, and keeps the result there for the hook to
// return to the game. Not detoured, the bridge's body runs at once, and the
// game function runs as if nothing were there.
//
// A frame belongs to one call on one thread; frames nest, so a game function
// that runs again inside itself or on another thread has its own.
//
// The types are plain ones with the passing of Norbyte's, as in
// Binding/CallAdapters.h, and the same proofs tie the two sides.

namespace bg3se::mac::binding {

namespace bridge {

// What StaticSymbols holds. Each has Norbyte's prototype for the member named.

// App__Ctor, wrapped as CoreLibInitProc: its pre-hook is PostStartup. The App
// constructor is inline in -[AppDelegate launchGame]; the bridge is called
// when CoreLibSDM::Init returns, which is before that code. There is no
// original to run: the body returns its argument.
void* AppCtor(void* app);
// App__UpdatePaths: the body is ls::Path::UpdateRoots().
void AppUpdatePaths(void* app);
// App__LoadGraphicSettings: the body is App::LoadGraphicSettingsConfig(ls::InitStruct&).
void AppLoadGraphicSettings(void* app);
// RPGStats__Load: the body is RPGStats::Start. Bridged, though the prototypes
// agree, because the hook on RPGStats::Start is what tells the first
// RPGStats::ParseDataBuffer call of a load from the rest.
void RPGStatsLoad(void* stats, void* paths);
// RPGStats__ParseDataBuffers: called before the first call of
// RPGStats::ParseDataBuffer that each RPGStats::Start makes. The parsing is
// inline in RPGStats::Start and goes on when the bridge returns: the body
// only returns true.
bool RPGStatsParseDataBuffers(void* stats);
// ls__Visual__UpdateBlendshapeWeightsFromSkeleton: the body is
// ls::Visual::NotifySkeletonUpdated(bool), which the function is inlined into.
void VisualUpdateBlendshapeWeightsFromSkeleton(void* visual);
// ui__deferred__PredicatesManager__UpdateFromUI: the body is
// gui::GameUI::UpdateFromUI(ls::GameTime const&), which the function is
// inlined into. The predicates manager is not known at that place: the
// argument is null (Norbyte's handler does not read it).
void UiPredicatesManagerUpdateFromUI(void* manager);

// --- the story library (OsirisWrappers) ---------------------------------------

// OsirisWrappers::OpenLogFile: the body is COsiris::OpenLogFile(wchar_t
// const*, unsigned int), which returns whether the file is open where
// Norbyte's prototype returns nothing. The game's caller gets the function's
// answer; false when Norbyte's wrapper did not call it.
void OsirisOpenLogFile(void* osiris, wchar_t const* path, uint32_t mode);
// OsirisWrappers::CreateFileW and CloseHandle, which Norbyte hooks to learn
// that the game has written and closed Story/story_header.div or
// Story/story_generated.div. The game writes neither through a function of
// its own that could stand for the Windows API's, so each bridge is called
// once for such a file, right after the game has closed it: CreateFileW with
// the file's path, opened for writing, then CloseHandle with what it
// returned. The bodies open and close nothing: CreateFileW returns a handle
// that names this one file, CloseHandle says it succeeded.
void* StoryCreateFile(wchar_t const* path, uint32_t access, uint32_t share, void* security, uint32_t creation, uint32_t flags, void* templateFile);
int StoryCloseHandle(void* handle);

}

// The text of an ls::Path as the game reads it: in the path itself, unless
// the byte at kGamePathTextFlag (Layout/GameMembers.h) is negative, then
// behind the pointer at its start (osi::OsirisManager::WriteHeader begins by
// doing this for fopen).
char const* GamePathText(void const* path);

// Where the path of the generated story is from the writer that the game
// closes at the call site hooked for it: both are locals of
// esv::StoryImplementation::ComposeAndBuildStory, and the engine table's
// entry for the call reads the two offsets from that function's code. Until
// this is given (and when it is given 0), closing the writer tells Norbyte's
// hooks nothing.
void SetStoryPathFromWriter(ptrdiff_t offset);

// The two halves of the client's ecl__GameStateMachine__Update hook. The
// state machine's update is inline in App::Update, so the pre-hook and the
// post-hook are two places of that function and cannot be the two ends of one
// wrapped call: Norbyte's client registers its two handlers here
// (Binding/ClientUpdate.h) and StaticSymbols holds nothing for the member.
using UpdateHandler = void(void* context, void* self, void const* time);
void SetClientUpdateHandlers(void* context, UpdateHandler* pre, UpdateHandler* post);

// The two ways the game ends that bg3mse shuts down at (Binding/Shutdown.h).
enum class GameExit : uint8_t
{
    Quit,           // the player quits: the entry of App::Terminate, on the game thread, before _Exit
    Teardown,       // the run loop ended: the entry of ls::StaticDataManager::Shutdown, on the main thread, before _exit
};

// Whether the calling thread is the one that runs the client's update (the
// game thread, which runs the client's Lua state); false before its first.
bool OnGameThread();

// What runs when the game exits, from the hooks on App::Terminate and
// ls::StaticDataManager::Shutdown; nullptr for nothing.
void SetGameExitHandler(void (*handler)(GameExit how));

enum class HookAt : uint8_t
{
    Entry,          // an entry hook, through the Detours layer
    CallSite,       // one call instruction, made to call the hook instead
};

// A hook of this layer on the game.
struct GameHook
{
    char const* Entry;      // the table entry: a function to hook, or a call site
    HookAt At;
    Prototype Game;         // the hook's prototype, which is the game function's
    void* Detour;
    void** Next;            // receives what runs the original: the trampoline, or the function the call calls
};

// A bridge and the game hooks that drive it.
struct HookBridge
{
    char const* Member;     // the StaticSymbols member
    Prototype Norbyte;      // the bridge's prototype
    void* Bridge;
    char const* Hooks[2];   // the entries of the game hooks it needs, all of them
};

std::span<GameHook const> GameHooks();
std::span<HookBridge const> HookBridges();
HookBridge const* FindHookBridge(std::string_view member);
// The entries of the game hooks that serve the client's update handlers.
std::span<char const* const> ClientUpdateHooks();

// Whether the hook's prototype is what the table says the place takes. For a
// call site the table's statement is that of the function called (the entry
// its `Needs` names, given as `callee`).
std::string GameHookDisagreement(GameHook const& hook, engine::Entry const* entry, engine::Entry const* callee);

// Empty when the hook would go on this game: its entry is resolved, the
// prototypes agree, and for a function an entry hook would go in, for a call
// site the function called is resolved too. Otherwise why not.
std::string GameHookRefusal(GameHook const& hook, engine::Resolution const& resolution);

// Puts every game hook on the game whose entry is resolved, would take an
// entry hook, and agrees with the hook's prototype. The entry hooks go in as
// one Detours transaction, then the call sites, each one instruction word.
// False when a write fails: what went in is taken out again, and `report`
// names whatever could not be (such a hook passes every call on unchanged).
// A hook whose entry is refused is left out, with the reason in `report`,
// and its bridges stay unbound.
bool AttachGameHooks(engine::Resolution const& resolution, std::string& report);
bool GameHookAttached(std::string_view entry);
// Whether every game hook the bridge needs is attached.
bool BridgeDriven(HookBridge const& bridge);

// Makes the `bl` at `site`, which must call `callee`, call `replacement`
// instead, through a jump pad near it. One instruction word changes.
std::string RedirectCall(void* site, void const* callee, void const* replacement);

// For tests: marks a game hook as attached with `next` as its original, or as
// not attached with nullptr, without touching any code.
void SetGameHookForTest(std::string_view entry, void* next);
// Makes the calling thread the game thread, as the client's update does.
void TakeGameThreadForTest();

}
