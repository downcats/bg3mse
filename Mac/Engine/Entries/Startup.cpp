#include <Engine/Entries/Areas.h>
#include <Layout/GameSession.h>

// Start-up, the application object, and the client and server loops.
//
// Several of the places Norbyte hooks on Windows are not functions in the
// macOS game: the App constructor is part of -[AppDelegate launchGame], and
// the client's state machine update is part of App::Update. Each such entry
// says so, and names the entries that give Norbyte's hook its moment here.

namespace bg3se::mac::engine {

namespace {

constexpr char kLaunchGame[] = "-[AppDelegate launchGame]";
constexpr char kCoreLibInit[] = "__ZN10CoreLibSDM4InitERKN2ls14ArgumentStringE";
constexpr char kUpdateRoots[] = "__ZN2ls4Path11UpdateRootsEv";
constexpr char kAppUpdate[] = "__ZN3App6UpdateERKN2ls8GameTimeE";
constexpr char kClientPreUpdate[] = "__ZN3ecl10GameClient9PreUpdateERKN2ls8GameTimeE";
constexpr char kWorldUpdate[] = "__ZN3ecs11EntityWorld6UpdateERKN2ls8GameTimeE";
constexpr char kServerWorker[] = "__ZN12ServerWorker6DoWorkEv";
constexpr char kServerStateUpdate[] = "__ZN3esv16GameStateMachine6UpdateERKN2ls8GameTimeE";
constexpr char kCreateGlobalSwitches[] = "__ZN3App20CreateGlobalSwitchesEv";
constexpr char kStaticDataShutdown[] = "__ZN2ls17StaticDataManager8ShutdownEv";
constexpr char kCoreLibShutdown[] = "__ZN10CoreLibSDM8ShutdownEv";
constexpr char kExitEngine[] = "__ZN7BaseApp10ExitEngineEv";
constexpr char kGameThreadDtor[] = "__ZN2ls10GameThreadD2Ev";
constexpr char kThreadWait[] = "__ZN2ls6Thread17WaitForCompletionEv";
constexpr char kWorkerTerminate[] = "__ZN2ls18WorkerThreadWorker9TerminateEv";
constexpr char kAppTerminate[] = "__ZN3App9TerminateEi";
constexpr char kAppVtable[] = "__ZTV3App";
constexpr char kAppVtableName[] = "vtable for App";

// The names of the entries that stand in for a place Norbyte hooks.
constexpr char kCoreLibInitName[] = "CoreLibSDM::Init";
constexpr char kUpdateRootsName[] = "ls::Path::UpdateRoots";
constexpr char kClientPreUpdateName[] = "ecl::GameClient::PreUpdate";
constexpr char kClientWorldUpdateName[] = "App::Update: call of ecs::EntityWorld::Update";
constexpr char kServerWorkerName[] = "ServerWorker::DoWork";

constexpr Named kLaunchHosts[] = { Exact(kLaunchGame) };
constexpr Named kAppUpdateHosts[] = { Exact(kAppUpdate) };
constexpr Named kServerWorkerHosts[] = { Exact(kServerWorker) };

constexpr char const* kAppCtorReplacements[] = { kCoreLibInitName };
constexpr char const* kUpdatePathsReplacements[] = { kUpdateRootsName };
constexpr char const* kClientUpdateReplacements[] = { kClientPreUpdateName, kClientWorldUpdateName };
constexpr char const* kServerWorkerReplacements[] = { kServerWorkerName };

constexpr Operand kCoreLibInitCalls[] = {
    { .What = "called once at launch", .In = Exact(kLaunchGame), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kCoreLibInit) },
};
// The game's exit: -[AppDelegate launchGame] calls it once, after BaseApp::Stop
// (whose BaseApp::ExitEngine joins the game thread and ends every worker
// thread), the App's destructor and the window manager's, and right before
// _exit(0); and it is what ends the core library, CoreLibSDM::Init's mirror.
// What the hook's moment rests on is checked too: ExitEngine destroys the
// game thread, whose destructor waits for it, and terminates the sixteen
// worker threads. (That BaseApp::Stop reaches ExitEngine, through the vtable,
// is not something the table can check.)
constexpr Operand kStaticDataShutdownCalls[] = {
    { .What = "called once, at the game's exit", .In = Exact(kLaunchGame), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kStaticDataShutdown) },
    { .What = "ends the core library", .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kCoreLibShutdown) },
    { .What = "the engine's exit destroys the game thread", .In = Exact(kExitEngine), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kGameThreadDtor) },
    { .What = "whose destructor waits for it", .In = Exact(kGameThreadDtor), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kThreadWait) },
    { .What = "the engine's exit terminates the worker threads", .In = Exact(kExitEngine), .How = Operand::Read::CallCount, .Expected = 16, .Target = Exact(kWorkerTerminate) },
};
// How the game ends when the player quits: the menus' Quit, Cmd+Q
// (-[BG3Application terminate:]) and the window's close button all come to
// ecl::EocClient::RequestQuit on the game thread, whose
// StartTerminatingApp calls App::Terminate(0) through the App's vtable
// (slot 0x90); it disconnects the peers, flushes the game's logs and calls
// _Exit, its last instruction, with every thread of the game still running
// and nothing torn down. That it ends the process is checked; that the App's
// vtable holds it where StartTerminatingApp calls it is the vtable entry's.
constexpr Operand kAppTerminateCalls[] = {
    { .What = "ends the process", .How = Operand::Read::CallCount, .Expected = 1, .Import = "__Exit" },
};
constexpr VtableMethod kAppSlots[] = {
    { "Terminate", Exact(kAppTerminate), 0x90 },
};

constexpr Operand kUpdateRootsCalls[] = {
    { .What = "called once at launch", .In = Exact(kLaunchGame), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kUpdateRoots) },
};
constexpr Operand kClientPreUpdateCalls[] = {
    { .What = "called once a frame", .In = Exact(kAppUpdate), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kClientPreUpdate) },
};
constexpr Operand kServerWorkerCalls[] = {
    { .What = "runs the server's states", .How = Operand::Read::CallCount, .Expected = 2, .Target = Exact(kServerStateUpdate) },
    { .What = "runs the server's world", .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kWorldUpdate) },
};

// App::LoadGraphicSettingsConfig is a call of BaseApp's and `return true`.
constexpr MaskedWord kAnyCall[] = { { 0x94000000, Any::Branch } };
constexpr Operand kLoadGraphicSettingsOperands[] = {
    {
        .What = "its one call",
        .Pattern = kAnyCall,
        .How = Operand::Read::BranchTarget,
        .Target = Exact("__ZN7BaseApp25LoadGraphicSettingsConfigERN2ls10InitStructE"),
    },
};

// The client's world update, inside App::Update.
constexpr Derivation kClientWorldUpdate{
    .Host = Exact(kAppUpdate),
    .HostShape = { 2077, 0x3c32fc78e0dbaadbull },     // 4.1.1.7631656
    .Anchor = Calls(Exact(kWorldUpdate)),
};

// App::CreateGlobalSwitches stores the object it has just constructed:
//     bl eoc::EoCGlobalSwitches::EoCGlobalSwitches() ; adrp x8, page ; str x19, [x8, #offset]
constexpr MaskedWord kStoreGlobalSwitches[] = {
    { 0x94000000, Any::Branch },
    { 0x90000008, Any::Adrp },
    { 0xf9000113, Any::Imm12 },
};
constexpr Derivation kGlobalSwitches{
    .Host = Exact(kCreateGlobalSwitches),
    .HostShape = { 48, 0x274ffad3e5367c3full },     // 4.1.1.7631656
    .Anchor = Calls(Exact("__ZN3eoc17EoCGlobalSwitchesC2Ev")),
    .Pattern = kStoreGlobalSwitches,
    .AnchorAt = 0,
    .What = Derivation::Take::PageAddress,
    .At = 1,
};

constexpr char kListenerCaveat[] =
    "the manager's listener array (pointer at +8, 32-bit size at +0x14) holds pointers to listener objects, and the game calls "
    "the function in the third slot of each object's own vtable (after the two destructor slots) with (object, {int from, int to}). "
    "Norbyte's HookStateMachineUpdates pushes the address of a member; here the listener object itself must be pushed";

// How the game tells its listeners of a change of state. The server's state
// machine does it in two places of its update:
//     adrp x8, m_ptr ; ldr x8, [x8] ; ldr w9, [x25] ; stp w23, w9, [sp, #0x20] ; ldr x19, [x8, #listeners] ; ldr w8, [x8, #size]
//     ... ; lsl x21, x8, #3 ; ldr x0, [x19], #8 ; ldr x8, [x0] ; ldr x8, [x8, #slot] ; add x1, sp, #0x20 ; blr x8
// and the client's, inlined into App::Update, in one, with the state entered
// read from the state machine:
//     ldr w9, [x25, #state] ; stp w27, w9, [sp, #0x40] ; ldr x20, [x8, #listeners] ; ldr w8, [x8, #size]
//     ... ; ldr x0, [x20], #8 ; ldr x8, [x0] ; ldr x8, [x8, #slot] ; add x1, sp, #0x40 ; blr x8
constexpr char kServerEventManager[] = "__ZN3esv21GameStateEventManager5m_ptrE";
constexpr MaskedWord kServerLoadsManager[] = { { 0x90000008, Any::Adrp }, { 0xf9400108, Any::Imm12 }, { 0xb9400329 }, { 0x290427f7 } };
constexpr MaskedWord kServerLoadsManagerAgain[] = { { 0x90000008, Any::Adrp }, { 0xf9400108, Any::Imm12 }, { 0xb9400329 }, { 0x290427f6 } };
constexpr MaskedWord kServerStates[] = { { 0x290427f7 }, { 0xf9400113, Any::Imm12 }, { 0xb9400108, Any::Imm12 } };
constexpr MaskedWord kServerStatesAgain[] = { { 0x290427f6 }, { 0xf9400113, Any::Imm12 }, { 0xb9400108, Any::Imm12 } };
constexpr MaskedWord kServerTells[] = {
    { 0xd37df115 }, { 0xf8408660 }, { 0xf9400008 }, { 0xf9400108, Any::Imm12 }, { 0x910083e1 }, { 0xd63f0100 },
};
constexpr MaskedWord kServerTellsAgain[] = {
    { 0xd37df116 }, { 0xf8408660 }, { 0xf9400008 }, { 0xf9400108, Any::Imm12 }, { 0x910083e1 }, { 0xd63f0100 },
};
constexpr MaskedWord kClientStates[] = { { 0xb9400329, Any::Imm12 }, { 0x290827fb }, { 0xf9400114, Any::Imm12 }, { 0xb9400108, Any::Imm12 } };
constexpr MaskedWord kClientTells[] = { { 0xf8408680 }, { 0xf9400008 }, { 0xf9400108, Any::Imm12 }, { 0x910103e1 }, { 0xd63f0100 } };
constexpr Operand kServerListenerOperands[] = {
    { .What = "the manager whose listeners are told", .In = Exact(kServerStateUpdate), .Pattern = kServerLoadsManager,
      .How = Operand::Read::PointerLoad, .Target = Exact(kServerEventManager) },
    { .What = "the listeners", .In = Exact(kServerStateUpdate), .Pattern = kServerStates, .At = 1, .Expected = kStateEventListeners },
    { .What = "the listeners' count", .In = Exact(kServerStateUpdate), .Pattern = kServerStates, .At = 2, .Expected = kStateEventListenersSize },
    { .What = "the slot a listener is told through", .In = Exact(kServerStateUpdate), .Pattern = kServerTells, .At = 3,
      .Expected = kStateListenerChanged },
    { .What = "the manager whose listeners are told (second place)", .In = Exact(kServerStateUpdate), .Pattern = kServerLoadsManagerAgain,
      .How = Operand::Read::PointerLoad, .Target = Exact(kServerEventManager) },
    { .What = "the listeners (second place)", .In = Exact(kServerStateUpdate), .Pattern = kServerStatesAgain, .At = 1,
      .Expected = kStateEventListeners },
    { .What = "the listeners' count (second place)", .In = Exact(kServerStateUpdate), .Pattern = kServerStatesAgain, .At = 2,
      .Expected = kStateEventListenersSize },
    { .What = "the slot a listener is told through (second place)", .In = Exact(kServerStateUpdate), .Pattern = kServerTellsAgain, .At = 3,
      .Expected = kStateListenerChanged },
};
constexpr Operand kClientListenerOperands[] = {
    { .What = "the state entered, in the state machine", .In = Exact(kAppUpdate), .Pattern = kClientStates, .At = 0,
      .Expected = kClientStateMachineState },
    { .What = "the listeners", .In = Exact(kAppUpdate), .Pattern = kClientStates, .At = 2, .Expected = kStateEventListeners },
    { .What = "the listeners' count", .In = Exact(kAppUpdate), .Pattern = kClientStates, .At = 3, .Expected = kStateEventListenersSize },
    { .What = "the slot a listener is told through", .In = Exact(kAppUpdate), .Pattern = kClientTells, .At = 2, .Expected = kStateListenerChanged },
};

constexpr VtableMethod kClientListenerSlots[] = {
    { "destructor", Exact("__ZN3ecl22GameStateEventListenerD1Ev"), 0x00 },
    { "deleting destructor", Exact("__ZN3ecl22GameStateEventListenerD0Ev"), 0x08 },
    { "OnTickLoad", Exact("__ZN3ecl22GameStateEventListener10OnTickLoadERKNS_13TickLoadEventE"), 0x18 },
};
constexpr VtableMethod kServerListenerSlots[] = {
    { "destructor", Exact("__ZN3esv22GameStateEventListenerD1Ev"), 0x00 },
    { "deleting destructor", Exact("__ZN3esv22GameStateEventListenerD0Ev"), 0x08 },
};
constexpr char kListenerVtableCaveat[] =
    "the handler OnGameStateChanged is pure in this class (its slot, +0x10, holds no function of the game), so it cannot be placed "
    "by name: the slots around it are what is checked";

constexpr Entry kEntries[] = {
    {
        .Name = "App__Ctor",
        .What = Kind::NoCallableCode,
        .Use = "Extender/ScriptExtender.cpp: a hook that runs PostStartup before the App object is built, once the core library "
               "(allocator, string table) is initialised",
        .Gone = {
            .Fate = "the App constructor is inlined into -[AppDelegate launchGame], between the game's one call of CoreLibSDM::Init "
                    "and its call of ls::Path::UpdateRoots; neither App nor BaseApp has a constructor symbol",
            .NoSymbol = "__ZN3AppC",
            .Hosts = kLaunchHosts,
            .Replacements = kAppCtorReplacements,
        },
    },
    {
        .Name = kCoreLibInitName,
        .What = Kind::FunctionToHook,
        .Mac = "CoreLibSDM::Init(ls::ArgumentString const&)",
        .Use = "takes the place of the App__Ctor hook: when it returns, the string table and the thread registry exist and the App "
               "object and the client and server singletons do not yet",
        .Where = Exact(kCoreLibInit),
        .Call = Takes({ Pointer(Place::X0, -1, "ls::ArgumentString const") }, Returns::Nothing, nullptr,
            "it stands in for App__Ctor, declared App* (App*): here the argument is the game's argument string and there is no result"),
        .Shape = { 9610, 0x1707f22144cbe40eull },     // 4.1.1.7631656
        .Operands = kCoreLibInitCalls,
        .Caveat = "whether everything PostStartup does can run at this moment is not known from the code alone: it needs a run in the game",
    },
    {
        .Name = "ls::StaticDataManager::Shutdown",
        .What = Kind::FunctionToHook,
        .Mac = "ls::StaticDataManager::Shutdown()",
        .Use = "where bg3mse shuts the extender down at the game's exit, as DllMain does for DLL_PROCESS_DETACH on Windows: on entry the "
               "game thread is joined and the worker threads are ended (BaseApp::Stop), the App is destroyed, and the static data "
               "managers, the string table and the allocator (CoreLibSDM::Shutdown) are not yet",
        .Where = Exact(kStaticDataShutdown),
        .Call = Takes({}, Returns::Nothing, nullptr, "a static function without arguments"),
        .Shape = { 27, 0xc5e213d0d0cb25bbull },       // 4.1.1.7631656
        .Operands = kStaticDataShutdownCalls,
    },
    {
        .Name = kAppVtableName,
        .What = Kind::Vtable,
        .Mac = "vtable for App",
        .Use = "where ecl::EocClient::StartTerminatingApp finds App::Terminate(int), which ends the game when the player quits",
        .Where = Exact(kAppVtable),
        .Methods = kAppSlots,
    },
    {
        .Name = "App::Terminate",
        .What = Kind::FunctionToHook,
        .Mac = "App::Terminate(int)",
        .Use = "where bg3mse shuts the extender down when the player quits (the menus, Cmd+Q, the window's close button), as DllMain "
               "does for DLL_PROCESS_DETACH on Windows: on entry, on the game thread, the game is whole and its other threads run",
        .Where = Exact(kAppTerminate),
        .Call = Takes({ Pointer(Place::X0, -1, "App"), Value(Place::X1, 4, -1, "int: the exit code") }, Returns::Nothing, nullptr,
            "a virtual function of the App that does not return"),
        .Shape = { 74, 0xf6c0561449a1beabull },      // 4.1.1.7631656
        .Operands = kAppTerminateCalls,
        .Needs = kAppVtableName,
    },
    {
        .Name = "App__UpdatePaths",
        .What = Kind::NoCallableCode,
        .Use = "Extender/ScriptExtender.cpp: a hook that, after the game has computed its path roots, rewrites root 4 for a custom profile",
        .Gone = {
            .Fate = "there is no App::UpdatePaths; the roots are rebuilt by the static function ls::Path::UpdateRoots()",
            .NoSymbol = "__ZN3App11UpdatePaths",
            .Replacements = kUpdatePathsReplacements,
        },
    },
    {
        .Name = kUpdateRootsName,
        .What = Kind::FunctionToHook,
        .Mac = "ls::Path::UpdateRoots()",
        .Use = "takes the place of the App__UpdatePaths hook: it rebuilds every path root (the launch, App::LoadConfig, "
               "App::LoadLanguage, CoreLibSDM::Init and ls::FileSystem::SetMediaPath call it), so the override is applied after each rebuild",
        .Where = Exact(kUpdateRoots),
        .Call = Takes({}, Returns::Nothing, nullptr, "a static function: there is no App* argument (Norbyte's handler does not use it)"),
        .Shape = { 2072, 0x59ded4dc31b7da58ull },     // 4.1.1.7631656
        .Operands = kUpdateRootsCalls,
    },
    {
        .Name = "AppInstance",
        .What = Kind::GlobalPointer,
        .Mac = "BaseApp::s_AppInstance",
        .Use = "GameHooks/EngineHooks.cpp: the application object",
        .Where = Exact("__ZN7BaseApp13s_AppInstanceE"),
    },
    {
        .Name = "App__LoadGraphicSettings",
        .What = Kind::FunctionToHook,
        .Mac = "App::LoadGraphicSettingsConfig(ls::InitStruct&)",
        .Use = "Extender/ScriptExtender.cpp: a hook that, once both game state event managers exist, registers the extender's listeners",
        .Where = Exact("__ZN3App25LoadGraphicSettingsConfigERN2ls10InitStructE"),
        .Call = Takes({ Pointer(Place::X0, 0, "App"), Pointer(Place::X1, -1, "ls::InitStruct") }, Returns::W0, "bool: always true",
            "Norbyte declares void (App*); here there is a second argument to pass on and a result to return"),
        .Shape = { 6, 0x92991dd97ab0b87dull },     // 4.1.1.7631656
        .Operands = kLoadGraphicSettingsOperands,
    },
    {
        .Name = "ecl__gGameStateEventManager",
        .What = Kind::GlobalPointer,
        .Mac = "ecl::GameStateEventManager::m_ptr",
        .Use = "Extender/ScriptExtender.cpp (HookStateMachineUpdates): the client's listeners for game state changes",
        .Where = Exact("__ZN3ecl21GameStateEventManager5m_ptrE"),
        .Operands = kClientListenerOperands,
        .Caveat = kListenerCaveat,
    },
    {
        .Name = "esv__gGameStateEventManager",
        .What = Kind::GlobalPointer,
        .Mac = "esv::GameStateEventManager::m_ptr",
        .Use = "Extender/ScriptExtender.cpp (HookStateMachineUpdates): the server's listeners for game state changes",
        .Where = Exact(kServerEventManager),
        .Operands = kServerListenerOperands,
        .Caveat = kListenerCaveat,
    },
    {
        .Name = "ecl::GameStateEventListener",
        .What = Kind::Vtable,
        .Mac = "vtable for ecl::GameStateEventListener",
        .Use = "the class Norbyte's client listener stands in for: its slot order is what the game's dispatch relies on",
        .Where = Exact("__ZTVN3ecl22GameStateEventListenerE"),
        .Methods = kClientListenerSlots,
        .Caveat = kListenerVtableCaveat,
    },
    {
        .Name = "esv::GameStateEventListener",
        .What = Kind::Vtable,
        .Mac = "vtable for esv::GameStateEventListener",
        .Use = "the class Norbyte's server listener stands in for",
        .Where = Exact("__ZTVN3esv22GameStateEventListenerE"),
        .Methods = kServerListenerSlots,
        .Caveat = kListenerVtableCaveat,
    },
    {
        .Name = "ecl__GameStateMachine__Update",
        .What = Kind::NoCallableCode,
        .Use = "Extender/Client/ScriptExtenderClient.cpp: pre- and post-hooks that are the client's entry points once a frame",
        .Gone = {
            .Fate = "ecl::EocClient::Update and ecl::GameStateMachine::Update are inlined into App::Update, between its one call of "
                    "ecl::GameClient::PreUpdate and its one call of ecs::EntityWorld::Update; every path through App::Update runs "
                    "both calls once, in that order",
            .NoSymbol = "__ZN3ecl16GameStateMachine6Update",
            .Hosts = kAppUpdateHosts,
            .Replacements = kClientUpdateReplacements,
        },
    },
    {
        .Name = kClientPreUpdateName,
        .What = Kind::FunctionToHook,
        .Mac = "ecl::GameClient::PreUpdate(ls::GameTime const&)",
        .Use = "takes the place of the pre-hook of ecl__GameStateMachine__Update: run it when this returns",
        .Where = Exact(kClientPreUpdate),
        .Call = Takes({ Pointer(Place::X0, -1, "ecl::GameClient"), Pointer(Place::X1, 1, "ls::GameTime const") }, Returns::Nothing, nullptr,
            "the first argument is the game client, not the state machine (Norbyte's handlers use only the time)"),
        .Shape = { 630, 0x251377115a5a7c3cull },     // 4.1.1.7631656
        .Operands = kClientPreUpdateCalls,
    },
    {
        .Name = kClientWorldUpdateName,
        .What = Kind::CallSite,
        .Mac = "the one call of ecs::EntityWorld::Update(ls::GameTime const&) in App::Update(ls::GameTime const&): x0 is the client's "
               "world, x1 the time",
        .Use = "takes the place of the post-hook of ecl__GameStateMachine__Update: the state machine's update has just finished",
        .Derived = &kClientWorldUpdate,
        .Needs = "ecs__EntityWorld__Update",
    },
    {
        .Name = "esv__GameStateMachine__Update",
        .What = Kind::FunctionToHook,
        .Mac = "esv::GameStateMachine::Update(ls::GameTime const&)",
        .Use = "Extender/Server/ScriptExtenderServer.cpp: pre- and post-hooks that are the server's entry points once a frame",
        .Where = Exact(kServerStateUpdate),
        .Call = Takes({ Pointer(Place::X0, 0, "esv::GameStateMachine"), Pointer(Place::X1, 1, "ls::GameTime const") }),
        .Shape = { 318, 0x729c0a832755803eull },     // 4.1.1.7631656
    },
    {
        .Name = "ecl__GameStateThreaded__GameStateWorker__DoWork",
        .What = Kind::FunctionToHook,
        .Mac = "ecl::GameStateThreaded::GameStateWorker::DoWork()",
        .Use = "Extender/Client/ScriptExtenderClient.cpp: a wrapper that binds the client's loading thread to the client context",
        .Where = Exact("__ZN3ecl17GameStateThreaded15GameStateWorker6DoWorkEv"),
        .Call = Takes({ Pointer(Place::X0, 0, "ecl::GameStateThreaded::GameStateWorker") }),
        .Shape = { 170, 0x268fcb96277e3b5bull },     // 4.1.1.7631656
    },
    {
        .Name = "esv__GameStateThreaded__GameStateWorker__DoWork",
        .What = Kind::NoCallableCode,
        .Use = "Extender/Server/ScriptExtenderServer.cpp: a wrapper that binds the server's loading thread to the server context",
        .Gone = {
            .Fate = "the server has no GameStateThreaded class in this game: its states run on the server thread itself, whose body "
                    "is ServerWorker::DoWork",
            .NoSymbol = "__ZN3esv17GameStateThreaded",
            .Hosts = kServerWorkerHosts,
            .Replacements = kServerWorkerReplacements,
        },
    },
    {
        .Name = kServerWorkerName,
        .What = Kind::FunctionToHook,
        .Mac = "ServerWorker::DoWork()",
        .Use = "takes the place of the server's worker wrapper: the body of the server thread, which runs the server's states and world",
        .Where = Exact(kServerWorker),
        .Call = Takes({ Pointer(Place::X0, -1, "ServerWorker") }),
        .Shape = { 1459, 0x14a946694f13dc62ull },     // 4.1.1.7631656
        .Operands = kServerWorkerCalls,
    },
    {
        .Name = "ecs__EntityWorld__Update",
        .What = Kind::FunctionToHook,
        .Mac = "ecs::EntityWorld::Update(ls::GameTime const&)",
        .Use = "GameHooks/EngineHooks.inl (OnECSUpdate): a wrapper around each world's update, client and server",
        .Where = Exact(kWorldUpdate),
        .Call = Takes({ Pointer(Place::X0, 0, "ecs::EntityWorld"), Pointer(Place::X1, 1, "ls::GameTime const") }),
        .Shape = { 254, 0xab3bdbcd95596115ull },     // 4.1.1.7631656
    },
    {
        .Name = "ecs__EntityWorld__FlushECBs",
        .What = Kind::FunctionToHook,
        .Mac = "ecs::EntityWorld::FlushECBs(); ecs::EntityWorld::Update ends by jumping to it, so an entry hook also runs for that",
        .Use = "GameHooks/EngineHooks.inl (OnECSFlushECBs): a pre-hook before a world's command buffers are applied",
        .Where = Exact("__ZN3ecs11EntityWorld9FlushECBsEv"),
        .Call = Takes({ Pointer(Place::X0, 0, "ecs::EntityWorld") }, Returns::Nothing, nullptr,
            "Norbyte declares a bool result; the function returns nothing"),
        .Shape = { 1174, 0x6ca6d4fcd7e44c52ull },     // 4.1.1.7631656
    },
    {
        .Name = "ecl__EoCClient",
        .What = Kind::GlobalPointer,
        .Mac = "ecl::EocClient::m_ptr",
        .Use = "everywhere: the client",
        .Where = Exact("__ZN3ecl9EocClient5m_ptrE"),
    },
    {
        .Name = "esv__EoCServer",
        .What = Kind::GlobalPointer,
        .Mac = "esv::EocServer::m_ptr",
        .Use = "everywhere: the server",
        .Where = Exact("__ZN3esv9EocServer5m_ptrE"),
    },
    {
        .Name = "ls__GlobalSwitches",
        .What = Kind::GlobalPointer,
        .Mac = "the variable App::CreateGlobalSwitches stores the new eoc::EoCGlobalSwitches in (its symbol is the C name Global)",
        .Use = "GameHooks/DataLibraries.cpp and GameDefinitions/Symbols.h: the global switches",
        .Derived = &kGlobalSwitches,
        .Agrees = Exact("_Global"),
    },
    {
        .Name = "ecl__EoCClient__HandleError",
        .What = Kind::FunctionToCall,
        .Mac = "ecl::EocClient::HandleError(ls::TranslatedString const&, bool, ls::TranslatedString const&, eoc::EMessageBoxLocalId)",
        .Use = "GameHooks/DataLibraries.cpp (ShowStartupError): show an error box in the game",
        .Where = Exact("__ZN3ecl9EocClient11HandleErrorERKN2ls16TranslatedStringEbS4_N3eoc18EMessageBoxLocalIdE"),
        .Call = Takes({
            Pointer(Place::X0, 0, "ecl::EocClient"),
            Pointer(Place::X1, 1, "ls::TranslatedString const: the message"),
            Value(Place::X2, 1, 2, "bool: exit the game"),
            Pointer(Place::X3, 3, "ls::TranslatedString const"),
            Value(Place::X4, 1, 4, "eoc::EMessageBoxLocalId"),
        }),
        .Shape = { 241, 0x5ecf1db803873430ull },     // 4.1.1.7631656
    },
    {
        .Name = "ls__gTranslatedStringRepository",
        .What = Kind::GlobalPointer,
        .Mac = "ls::TranslatedStringRepository::m_ptr",
        .Use = "GameHooks/DataLibraries.cpp and GameDefinitions/Symbols.h: the translated strings",
        .Where = Exact("__ZN2ls26TranslatedStringRepository5m_ptrE"),
    },
    {
        .Name = "ls__gTranslatedStringKeyManager",
        .What = Kind::GlobalPointer,
        .Mac = "ls::TranslatedStringKeyManager::m_ptr",
        .Use = "GameDefinitions/Symbols.h: the translated string keys",
        .Where = Exact("__ZN2ls26TranslatedStringKeyManager5m_ptrE"),
    },
};

}

std::span<Entry const> StartupEntries()
{
    return kEntries;
}

}
