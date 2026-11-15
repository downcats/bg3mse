#include <stdafx.h>
#include <GameDefinitions/GameState.h>
#include <GameDefinitions/Resources.h>
#include <GameDefinitions/RootTemplates.h>
#include <GameDefinitions/Surface.h>
#include <GameDefinitions/Symbols.h>
#include <GameDefinitions/UI.h>

#include <Binding/CallAdapters.h>
#include <Binding/HookBridges.h>
#include <Binding/StaticSymbolBinding.h>
#include <Binding/StatsFunctions.h>

// The rows of Binding/StaticSymbolBinding.h: one for every member of
// StaticSymbols that points into the game, with the prototype Norbyte's code
// calls or detours it through, taken from the member's own type. This file
// sees his definitions and has no code that runs against them: offsets,
// prototypes and assertions only.

// offsetof on StaticSymbols, which is not standard-layout: what Norbyte's
// own SYM_OFF does, and what clang supports for it.
#pragma clang diagnostic ignored "-Winvalid-offsetof"

BEGIN_NS(mac::binding)

// Noesis::Point is two floats and comes back in s0 and s1; HitTestResult is
// one pointer and comes back in x0. Both hold only while the SDK declares
// them as plain data.
static_assert(std::is_trivially_copyable_v<Noesis::Point> && sizeof(Noesis::Point) == 2 * sizeof(float));
static_assert(std::is_trivially_copyable_v<Noesis::HitTestResult> && sizeof(Noesis::HitTestResult) == sizeof(void*));
template <> inline constexpr engine::Returns kComesBackIn<Noesis::Point> = engine::Returns::V0V1;
template <> inline constexpr engine::Returns kComesBackIn<Noesis::HitTestResult> = engine::Returns::X0;

// The plain types of the adapters against the types of Norbyte's they stand for.
static_assert(sizeof(LSStringView) == sizeof(GameStringView) && sizeof(FixedString) == sizeof(uint32_t));
static_assert(sizeof(Noesis::Ptr<Noesis::BaseComponent>) == sizeof(void*) && !std::is_trivially_copyable_v<Noesis::Ptr<Noesis::BaseComponent>>);

namespace {

template <class T> struct MemberPrototype { static constexpr Prototype Value{}; };
template <class F> requires std::is_function_v<F> struct MemberPrototype<F*> { static constexpr Prototype Value = kPrototypeOf<F>; };

#define BG3MSE_OFFSET(member) static_cast<uint32_t>(offsetof(StaticSymbols, member))
#define BG3MSE_PROTOTYPE(member) MemberPrototype<decltype(StaticSymbols::member)>::Value

// A function row is given a member whose type is a function's, so that the
// prototype compared is Norbyte's own.
#define BG3MSE_IS_FUNCTION(member) \
    static_assert(BG3MSE_PROTOTYPE(member).Present && BG3MSE_PROTOTYPE(member).Known, #member ": the member's type is not a function this layer can place")
// An adapter or bridge passes what Norbyte's prototype for the member passes.
#define BG3MSE_STANDS_FOR(member, function) \
    static_assert(SamePassing(BG3MSE_PROTOTYPE(member), kPrototypeOf<decltype(function)>), #member ": " #function " does not pass what Norbyte's prototype does")
#define BG3MSE_STANDS_FOR_TYPE(type, function) \
    static_assert(SamePassing(kPrototypeOf<type>, kPrototypeOf<decltype(function)>), #type ": " #function " does not pass what Norbyte's prototype does")

constexpr bool kCritical = true, kOptional = false;

constexpr MemberRow Global(char const* member, uint32_t offset, bool critical, char const* whenNull, char const* required = nullptr)
{
    return { member, offset, Bound::Global, member, {}, Through::Call, false, kExactly, critical, { required, nullptr }, whenNull };
}
constexpr MemberRow Function(char const* member, uint32_t offset, Prototype const& prototype, Through use, unsigned allow, bool critical,
    char const* whenNull, char const* entry = nullptr, bool alsoHooked = false)
{
    return { member, offset, Bound::Function, entry ? entry : member, prototype, use, alsoHooked, allow, critical, {}, whenNull };
}
constexpr MemberRow Other(char const* member, uint32_t offset, Bound how, Prototype const& prototype, bool critical, char const* whenNull,
    char const* entry = nullptr)
{
    return { member, offset, how, entry ? entry : member, prototype, how == Bound::CallAdapter ? Through::Call : Through::Hook, false, kExactly, critical, {}, whenNull };
}

#define GLOBAL(member, critical, whenNull) Global(#member, BG3MSE_OFFSET(member), critical, whenNull)
#define GLOBAL_WITH(member, critical, required, whenNull) Global(#member, BG3MSE_OFFSET(member), critical, whenNull, required)
#define CALLED(member, critical, whenNull) Function(#member, BG3MSE_OFFSET(member), BG3MSE_PROTOTYPE(member), Through::Call, kExactly, critical, whenNull)
#define HOOKED(member, allow, critical, whenNull) Function(#member, BG3MSE_OFFSET(member), BG3MSE_PROTOTYPE(member), Through::Hook, allow, critical, whenNull)
#define CALLED_AS(member, type, whenNull) Function(#member, BG3MSE_OFFSET(member), kPrototypeOf<type>, Through::Call, kExactly, kOptional, whenNull)
#define ADAPTER(member, critical, whenNull) Other(#member, BG3MSE_OFFSET(member), Bound::CallAdapter, BG3MSE_PROTOTYPE(member), critical, whenNull)
#define ADAPTER_WITH(member, entry, critical, whenNull) \
    Other(#member, BG3MSE_OFFSET(member), Bound::CallAdapter, BG3MSE_PROTOTYPE(member), critical, whenNull, entry)
#define ADAPTER_AS(member, type, whenNull) Other(#member, BG3MSE_OFFSET(member), Bound::CallAdapter, kPrototypeOf<type>, kOptional, whenNull)
#define BRIDGE(member, critical, whenNull) Other(#member, BG3MSE_OFFSET(member), Bound::HookBridge, BG3MSE_PROTOTYPE(member), critical, whenNull)
#define BRIDGE_AS(member, type, whenNull) Other(#member, BG3MSE_OFFSET(member), Bound::HookBridge, kPrototypeOf<type>, kOptional, whenNull)
#define HANDLERS(member, critical, whenNull) Other(#member, BG3MSE_OFFSET(member), Bound::Handlers, {}, critical, whenNull)
#define UNAVAILABLE(member, whenNull) Other(#member, BG3MSE_OFFSET(member), Bound::Unavailable, {}, kOptional, whenNull)

// What a wrapped member that stays null means in every case: Norbyte tests it
// before Wrap, so nothing is detoured and the handler never runs.
#define NOT_WRAPPED "not wrapped (Norbyte tests the member before Wrap): "

// Norbyte's prototypes for the members he declares as void*, from the place
// that casts each.
using AddVisualChildProc = void(Noesis::Visual*, Noesis::Visual*);      // Lua/Libs/ClientUI/Builtins.inl

constexpr MemberRow kRows[] = {
    // --- the core library ---
    ADAPTER(ls__FixedString__GetString, kCritical,
        "no fixed string has text (BaseString.inl returns none): nothing that names a game object works"),
    CALLED(ls__FixedString__IncRef, kCritical,
        "fixed strings are copied without a reference (BaseString.inl skips the call): the game can free a string the extender holds"),
    ADAPTER(ls__GlobalStringTable__MainTable__CreateFromString, kCritical,
        "no fixed string can be made (BaseString.inl falls back to a function that is null as well, then to the null index)"),
    CALLED(ls__GlobalStringTable__MainTable__DecRef, kCritical,
        "fixed strings are never released (BaseString.inl falls back to a function that is null as well)"),
    GLOBAL(ls__gGlobalStringTable, kCritical,
        "BaseString.inl reads through it without a test when it makes, releases or inspects a fixed string: a fault"),
    CALLED(ls__ThreadRegistry__RequestThreadIndex, kCritical,
        "ThreadRegistry::RequestThreadIndex calls it without a test (Base.cpp): a fault on the first entity system access"),
    Function("ls__FileReader__ctor", BG3MSE_OFFSET(ls__FileReader__ctor), BG3MSE_PROTOTYPE(ls__FileReader__ctor), Through::Call, kExactly, kCritical,
        "no file of the game can be read (MakeFileReader returns none) and the path override hook is " NOT_WRAPPED "mods load no scripts",
        nullptr, true),
    CALLED(ls__FileReader__dtor, kCritical,
        "DestroyFileReader calls it without a test: a fault when the first file reader is released"),
    CALLED(ls__FileSystem__CollectFilesWithGlob, kOptional,
        "Ext.IO.FindFiles gives a Lua error (Binding/LuaGates.h)"),
    GLOBAL(ls__PathRoots, kCritical,
        "ToPath gives an empty path and logs; no file is read, the log directory is not found, a custom profile is not applied"),
    BRIDGE(App__Ctor, kCritical,
        NOT_WRAPPED "PostStartup never runs, so no library, no hook and no Lua state exists: nothing of the extender works"),
    BRIDGE(App__UpdatePaths, kOptional,
        NOT_WRAPPED "the CustomProfile setting has no effect"),

    // --- the application, client and server ---
    GLOBAL(ecl__EoCClient, kCritical,
        "GetEoCClient gives null: no client state, level or mod manager, and no start-up error can be shown"),
    GLOBAL(esv__EoCServer, kCritical,
        "GetEoCServer gives null: no server state, level or mod manager"),
    CALLED(ecl__EoCClient__HandleError, kOptional,
        "start-up errors are logged and not shown (ShowStartupError and CanShowMessages test it)"),
    GLOBAL(ecl__gCursorControl, kOptional, "Ext.UI's cursor control gives nothing (Module.inl tests it)"),
    GLOBAL(ls__gDragDropManager, kOptional, "Ext.UI's drag and drop manager gives nothing (Module.inl tests it)"),
    GLOBAL(ls__gTranslatedStringRepository, kCritical,
        "GetTranslatedStringRepository gives null and its callers read through it: translated strings fault"),
    GLOBAL(ls__gTranslatedStringKeyManager, kOptional,
        "GetTranslatedStringKeyManager gives null: Ext.Loca's string keys give nothing (Localization.inl tests it)"),
    GLOBAL_WITH(ecl__gGameStateEventManager, kCritical, "ecl::GameStateEventListener",
        "the client's game state listener is not registered (HookStateMachineUpdates tests it): the client never learns of a state change"),
    GLOBAL_WITH(esv__gGameStateEventManager, kCritical, "esv::GameStateEventListener",
        "the server's game state listener is not registered: the server never learns of a state change"),
    HOOKED(ecl__GameStateThreaded__GameStateWorker__DoWork, kExactly, kCritical,
        NOT_WRAPPED "the client's loading thread is not bound to the client context"),
    Function("esv__GameStateThreaded__GameStateWorker__DoWork", BG3MSE_OFFSET(esv__GameStateThreaded__GameStateWorker__DoWork),
        BG3MSE_PROTOTYPE(esv__GameStateThreaded__GameStateWorker__DoWork), Through::Hook,
        // The server worker itself is what the game passes; Norbyte's wrapper hands it on as his void* self.
        kOwnPrototype, kCritical,
        NOT_WRAPPED "the server thread is not bound to the server context",
        "ServerWorker::DoWork"),
    HANDLERS(ecl__GameStateMachine__Update, kCritical,
        "the client's update handlers never run: no client tick, no network, no ImGui update"),
    HOOKED(esv__GameStateMachine__Update, kExactly, kCritical,
        NOT_WRAPPED "the server's update handlers never run: no server tick"),
    BRIDGE(App__LoadGraphicSettings, kCritical,
        NOT_WRAPPED "the game state listeners are registered only if both managers already exist when PostStartup runs, which they do not"),
    GLOBAL(ls__gInputManager, kOptional, "Ext.Input.GetInputManager gives a Lua error (Binding/LuaGates.h)"),
    HOOKED(ecs__EntityWorld__Update, kExactly, kCritical,
        NOT_WRAPPED "the entity system helpers are never updated and no component event fires"),
    HOOKED(ecs__EntityWorld__FlushECBs, kResultNotProduced, kOptional,
        NOT_WRAPPED "the extender is not told when command buffers are applied"),

    // --- stats ---
    GLOBAL(eoc__SpellPrototypeManager, kOptional, "spell prototypes are not kept in step with stats changed from Lua (Stats.cpp tests it); Ext.Stats.GetCachedSpell gives a Lua error (Binding/LuaGates.h)"),
    CALLED(eoc__SpellPrototype__Init, kOptional, "SyncStat logs and fails: a changed spell's prototype is not rebuilt"),
    GLOBAL(eoc__StatusPrototypeManager, kOptional, "status prototypes are not kept in step (Stats.cpp tests it); Ext.Stats.GetCachedStatus gives a Lua error (Binding/LuaGates.h)"),
    CALLED(eoc__StatusPrototype__Init, kOptional, "SyncStat logs and fails: a changed status's prototype is not rebuilt"),
    GLOBAL(eoc__BoostPrototypeManager, kOptional, "Ext.Stats.GetCachedBoost gives a Lua error (Binding/LuaGates.h)"),
    GLOBAL(eoc__PassivePrototypeManager, kOptional, "passive prototypes are not kept in step (Stats.cpp tests it); Ext.Stats.GetCachedPassive gives a Lua error (Binding/LuaGates.h)"),
    ADAPTER(eoc__PassivePrototype__Init, kOptional, "SyncStat logs and fails (Stats.cpp tests it): a changed passive's prototype is not rebuilt"),
    GLOBAL(eoc__InterruptPrototypeManager, kOptional, "interrupt prototypes are not kept in step (Stats.cpp tests it); Ext.Stats.GetCachedInterrupt gives a Lua error (Binding/LuaGates.h)"),
    ADAPTER(eoc__InterruptPrototype__Init, kOptional, "SyncStat logs and fails (Stats.cpp tests it): a changed interrupt's prototype is not rebuilt"),
    GLOBAL(eoc__DynamicStatsExpressionManager, kOptional, "a stats expression given from Lua as text gives a Lua error (LuaGet.inl tests it outside MSVC)"),
    ADAPTER(eoc__ParseStaticBoosts, kOptional, "a status prototype's static boosts are not parsed again when it is rebuilt (Stats.cpp tests it)"),
    HOOKED(esv__OsirisVariableHelper__SavegameVisit, kResultNotProduced, kOptional,
        NOT_WRAPPED "the extender's persistent variables are neither saved nor loaded with a savegame"),
    // The parameter before the last is a reference to an entity in the game
    // function, and an integer his handler only hands on (FunctorEvents.inl).
    HOOKED(stats__DealDamageFunctor__ApplyDamage, kResultNotProduced | kPointersAsWords, kOptional,
        NOT_WRAPPED "no DealDamage and DealtDamage events"),
    HOOKED(esv__StatsSystem__ThrowDamageEvent, kExactly, kOptional,
        NOT_WRAPPED "no BeforeDealDamage event"),
    HOOKED(esv__ExecuteStatsFunctor_AttackTargetContext, kExactly, kOptional, NOT_WRAPPED "no functor events for this context"),
    HOOKED(esv__ExecuteStatsFunctor_AttackPositionContext, kExactly, kOptional, NOT_WRAPPED "no functor events for this context"),
    HOOKED(esv__ExecuteStatsFunctor_MoveContext, kExactly, kOptional, NOT_WRAPPED "no functor events for this context"),
    HOOKED(esv__ExecuteStatsFunctor_TargetContext, kExactly, kOptional, NOT_WRAPPED "no functor events for this context"),
    HOOKED(esv__ExecuteStatsFunctor_NearbyAttackedContext, kExactly, kOptional, NOT_WRAPPED "no functor events for this context"),
    HOOKED(esv__ExecuteStatsFunctor_NearbyAttackingContext, kExactly, kOptional, NOT_WRAPPED "no functor events for this context"),
    HOOKED(esv__ExecuteStatsFunctor_EquipContext, kExactly, kOptional, NOT_WRAPPED "no functor events for this context"),
    HOOKED(esv__ExecuteStatsFunctor_SourceContext, kExactly, kOptional, NOT_WRAPPED "no functor events for this context"),
    Function("esv__ExecuteStatsFunctor_InterruptContext", BG3MSE_OFFSET(esv__ExecuteStatsFunctor_InterruptContext),
        BG3MSE_PROTOTYPE(esv__ExecuteStatsFunctor_InterruptContext), Through::Hook, kExactly, kOptional,
        NOT_WRAPPED "no functor events for interrupts; Ext.Stats.ExecuteFunctors calls it for an interrupt context (StatFunctors.inl)"),
    GLOBAL(gRPGStats, kCritical,
        "GetStats gives null: Ext.Stats has nothing to read, and the stats load order helper reads through it without a test"),
    BRIDGE(RPGStats__Load, kOptional,
        NOT_WRAPPED "the client's extension state is not loaded around the stats load and no ModuleLoadStarted or StatsLoaded event fires"),
    BRIDGE(RPGStats__ParseDataBuffers, kOptional,
        NOT_WRAPPED "no StatsStructureLoaded event, and the stats load order helper is never finished"),
    ADAPTER(stats__Object__SetPropertyString, kOptional,
        "SetRawAttribute logs and fails (StatAttributes.inl tests it outside MSVC): an attribute of a stats object cannot be set from its text"),

    // --- world and resources ---
    GLOBAL(esv__LevelManager, kOptional, "GetServerLevelManager gives nothing: no current server level; the server's Ext.Level gives a Lua error except for the surface actions (Binding/LuaGates.h)"),
    GLOBAL(ecl__LevelManager, kOptional, "GetClientLevelManager gives nothing: no current client level; the client's Ext.Level gives a Lua error (Binding/LuaGates.h)"),
    GLOBAL(ls__GlobalTemplateManager, kOptional, "GetGlobalTemplateManager gives null: no global template bank; Ext.Template's root templates give a Lua error (Binding/LuaGates.h)"),
    GLOBAL(esv__CacheTemplateManager, kOptional, "Ext.Template's cache templates give a Lua error (Binding/LuaGates.h), and CreateCacheTemplate logs and keeps the template (GameHelpers.cpp tests it outside MSVC)"),
    GLOBAL(esv__gSurfaceActionFactory, kOptional, "CreateAction gives nothing (Surface.inl tests it): no surface action from Lua"),
    ADAPTER(esv__SurfaceActionFactory__DoCreateAction, kOptional, "CreateAction gives nothing (Surface.inl tests it): no surface action from Lua"),
    CALLED(esv__TransformSurfaceAction__Init, kOptional, "AddAction logs and adds no transform action (Surface.inl tests it outside MSVC)"),
    HOOKED(eoc__AiGrid__FindPath, kExactly, kOptional, NOT_WRAPPED "no FindPath event"),
    ADAPTER(eoc__AiGrid__FindPathImmediate, kOptional, "Ext.Level.FindPath gives a Lua error (Binding/LuaGates.h)"),
    CALLED(dlg__DialogManager__PerformDialogLoad, kOptional, "Ext.StaticData.LoadResource logs an error and gives nil (StaticData.inl tests it); no dialogue is then held, and ReleaseResource gives false"),
    GLOBAL(esv__SavegameManager, kOptional, "nothing reads it"),
    GLOBAL(AppInstance, kOptional, "Initialize takes the App as not yet built, which is right at the time this build starts"),
    // The three stand or fall together, with the entry that names where the
    // game's allocation code is.
    ADAPTER_WITH(ls__GlobalAllocator__Get, "ls__GlobalAllocator__Alloc", kCritical, "BG3Alloc and BG3Free use malloc and free, which is what the game uses"),
    ADAPTER(ls__GlobalAllocator__Alloc, kCritical, "BG3Alloc and BG3Free use malloc and free; ShowStartupError and CanShowMessages take the game as not ready and show nothing"),
    ADAPTER_WITH(ls__GlobalAllocator__Free, "ls__GlobalAllocator__Alloc", kCritical, "BG3Alloc and BG3Free use malloc and free, which is what the game uses"),
    GLOBAL(eoc__gGuidResourceManager, kOptional, "the entity system helpers find no resource definitions (EntitySystem.cpp tests it): Ext.StaticData gives nothing"),
    GLOBAL(ls__gGlobalResourceManager, kOptional, "GetCurrentResourceBank gives null; Ext.Audio and Ext.UI's game UI give a Lua error (Binding/LuaGates.h), a genome variable cannot be set and mods' virtual textures are not loaded (Animation.inl, VirtualTextures.inl test it outside MSVC), ImGui loads no game texture"),
    GLOBAL(ls__gTextureAtlasMap, kOptional, "the texture atlas functions give nothing and ImGui shows no icon (StaticData.inl and IMGUI.cpp test it)"),
    UNAVAILABLE(ls__AppliedMaterial__LoadTexture, "not called: outside MSVC ImGui's texture loading does that function's work itself (IMGUI.cpp, Mac/Overlay/GameTextures.cpp)"),
    // An adapter: it takes the texture manager from where the game's code
    // takes it (+0x80 of the resource manager, an operand of the table's
    // entry), whatever Norbyte's ResourceManager says.
    ADAPTER(ls__TextureManager__UnloadTexture, kOptional, "ImGui loads no game texture, so it never unloads one (Overlay/GameTextures.cpp tests it)"),
    ADAPTER(ls__AppliedMaterial__LoadVirtualTexture, kOptional, "SetVirtualTexture logs and fails (GameHelpers.cpp tests it outside MSVC)"),
    ADAPTER(ls__AppliedMaterial__TryOverrideTexture2DParameter, kOptional, "SetTexture2D logs and fails (GameHelpers.cpp tests it outside MSVC)"),
    BRIDGE(ls__Visual__UpdateBlendshapeWeightsFromSkeleton, kOptional, NOT_WRAPPED "blend shape weights set from Lua are not applied"),

    // --- the user interface ---
    GLOBAL(Noesis__Reflection__LockedByThreadId, kOptional, "Ext.UI's functions that reach NoesisGUI give a Lua error (Binding/LuaGates.h)"),
    GLOBAL(Noesis__Reflection__Lock, kOptional, "Ext.UI's functions that reach NoesisGUI give a Lua error (Binding/LuaGates.h)"),
    GLOBAL(Noesis__Reflection__NumLocks, kOptional, "Ext.UI's functions that reach NoesisGUI give a Lua error (Binding/LuaGates.h)"),
    GLOBAL(Noesis__Reflection__Data, kOptional, "Ext.UI's functions that reach NoesisGUI give a Lua error (Binding/LuaGates.h)"),
    GLOBAL(Noesis__SymbolManager__Lock, kOptional, "Ext.UI's functions that reach NoesisGUI give a Lua error (Binding/LuaGates.h)"),
    GLOBAL(Noesis__SymbolManager__Data, kOptional, "Ext.UI's functions that reach NoesisGUI give a Lua error (Binding/LuaGates.h)"),
    GLOBAL(Noesis__gErrorHandler, kOptional, "Ext.UI.EnableErrorReporting gives a Lua error (Binding/LuaGates.h)"),
    ADAPTER_AS(Noesis__GUI__LoadXaml, Noesis::LoadXamlProc, "LoadXaml logs and gives nothing (NsHelpers.inl tests it)"),
    CALLED_AS(Noesis__Visual__AddVisualChild, AddVisualChildProc, "Visual::AddVisualChild does nothing (Builtins.inl tests it)"),
    CALLED_AS(Noesis__Visual__RemoveVisualChild, AddVisualChildProc, "Visual::RemoveVisualChild does nothing (Builtins.inl tests it)"),
    CALLED_AS(Noesis__Visual__PointFromScreen, Noesis::Visual__PointConversionProc, "a Lua error naming the missing mapping (RequireNoesisMapping)"),
    CALLED_AS(Noesis__Visual__PointToScreen, Noesis::Visual__PointConversionProc, "a Lua error naming the missing mapping (RequireNoesisMapping)"),
    CALLED_AS(Noesis__VisualTreeHelper__HitTest, Noesis::Visual__TreeHelperHitTestProc, "a Lua error naming the missing mapping (RequireNoesisMapping)"),
    UNAVAILABLE(ui__DataContextProvider__ExecuteCommandQueues, NOT_WRAPPED "nothing sets a handler for it"),
    BRIDGE_AS(ui__deferred__PredicatesManager__UpdateFromUI, ui::DeferredPredicatesManager__UpdateFromUIProc, NOT_WRAPPED "UI events deferred to the Noesis thread are never run"),

    GLOBAL(ls__GlobalSwitches, kCritical, "GetGlobalSwitches gives null; the DisableStoryPatching setting has no effect (ApplyCodePatches tests it)"),

    // --- the Windows API ---
    UNAVAILABLE(Kernel_FindFirstFileW, NOT_WRAPPED "Norbyte sets no handler for it"),
    UNAVAILABLE(Kernel_FindNextFileW, NOT_WRAPPED "Norbyte sets no handler for it"),
    UNAVAILABLE(Kernel_FindClose, NOT_WRAPPED "Norbyte sets no handler for it"),
};

// The members that are not single pointers. The three maps are what the
// Windows engine callbacks fill with every type index variable of the game
// and its name; here they are filled from the table's TypeIdContext entries
// once the game has numbered its types (Binding/TypeIndexBinding.h).
// Libraries is not used.
constexpr OtherMember kOtherMembers[] = {
    { "IndexSymbolToContextMaps", BG3MSE_OFFSET(IndexSymbolToContextMaps), sizeof(StaticSymbols::IndexSymbolToContextMaps) },
    { "IndexSymbolToNameMaps", BG3MSE_OFFSET(IndexSymbolToNameMaps), sizeof(StaticSymbols::IndexSymbolToNameMaps) },
    { "StaticStringRegistrantMaps", BG3MSE_OFFSET(StaticStringRegistrantMaps), sizeof(StaticSymbols::StaticStringRegistrantMaps) },
    { "Libraries", BG3MSE_OFFSET(Libraries), sizeof(StaticSymbols::Libraries) },
};

// --- Norbyte's prototypes against the functions that stand for them ----------

BG3MSE_STANDS_FOR(ls__FixedString__GetString, adapter::FixedStringGetString);
BG3MSE_STANDS_FOR(ls__GlobalStringTable__MainTable__CreateFromString, adapter::MainTableCreateFromString);
BG3MSE_STANDS_FOR(esv__SurfaceActionFactory__DoCreateAction, adapter::SurfaceActionFactoryCreateAction);
BG3MSE_STANDS_FOR(eoc__AiGrid__FindPathImmediate, adapter::AiGridFindPathImmediate);
BG3MSE_STANDS_FOR(ls__AppliedMaterial__LoadVirtualTexture, adapter::AppliedMaterialLoadVirtualTexture);
BG3MSE_STANDS_FOR(ls__AppliedMaterial__TryOverrideTexture2DParameter, adapter::AppliedMaterialTryOverrideTexture2DParameter);
BG3MSE_STANDS_FOR_TYPE(Noesis::LoadXamlProc, adapter::NoesisLoadXaml);
BG3MSE_STANDS_FOR(ls__TextureManager__UnloadTexture, adapter::TextureManagerUnloadTexture);
BG3MSE_STANDS_FOR(ls__GlobalAllocator__Get, adapter::GlobalAllocatorGet);
BG3MSE_STANDS_FOR(ls__GlobalAllocator__Alloc, adapter::GlobalAllocatorAlloc);
BG3MSE_STANDS_FOR(ls__GlobalAllocator__Free, adapter::GlobalAllocatorFree);
BG3MSE_STANDS_FOR(eoc__ParseStaticBoosts, adapter::ParseStaticBoosts);
BG3MSE_STANDS_FOR(stats__Object__SetPropertyString, adapter::StatsObjectSetPropertyString);
BG3MSE_STANDS_FOR(eoc__PassivePrototype__Init, adapter::PassivePrototypeInit);
BG3MSE_STANDS_FOR(eoc__InterruptPrototype__Init, adapter::InterruptPrototypeInit);

BG3MSE_STANDS_FOR(App__Ctor, bridge::AppCtor);
// Norbyte wraps App__Ctor as CoreLibInitProc, which returns nothing: his
// wrapper then drops what the bridge returns, and nothing reads it.
static_assert(kPrototypeOf<CoreLibInitProc>.Count == 1 && kPrototypeOf<CoreLibInitProc>.Args[0].Address);
BG3MSE_STANDS_FOR(App__UpdatePaths, bridge::AppUpdatePaths);
BG3MSE_STANDS_FOR(App__LoadGraphicSettings, bridge::AppLoadGraphicSettings);
BG3MSE_STANDS_FOR(RPGStats__Load, bridge::RPGStatsLoad);
BG3MSE_STANDS_FOR(RPGStats__ParseDataBuffers, bridge::RPGStatsParseDataBuffers);
BG3MSE_STANDS_FOR(ls__Visual__UpdateBlendshapeWeightsFromSkeleton, bridge::VisualUpdateBlendshapeWeightsFromSkeleton);
BG3MSE_STANDS_FOR_TYPE(ui::DeferredPredicatesManager__UpdateFromUIProc, bridge::UiPredicatesManagerUpdateFromUI);

// The client's update handlers are called as UpdateHandler: the prototype of
// the member they belong to, after the context.
static_assert(SamePassing(kPrototypeOf<void(void*, void*, GameTime*)>, kPrototypeOf<UpdateHandler>));
static_assert(SamePassing(kPrototypeOf<ecl::GameStateMachine__UpdateProc>, kPrototypeOf<void(void*, GameTime*)>));

// Every function row has a prototype taken from a function type.
BG3MSE_IS_FUNCTION(ls__FixedString__IncRef);
BG3MSE_IS_FUNCTION(ls__FileReader__ctor);
BG3MSE_IS_FUNCTION(stats__DealDamageFunctor__ApplyDamage);
BG3MSE_IS_FUNCTION(esv__ExecuteStatsFunctor_InterruptContext);

// GetGlobalTemplateBank indexes Norbyte's Banks with the game's index: the
// member is where the game's code has the banks (the operand of the engine
// table's g_GlobalTemplateBankType entry).
static_assert(offsetof(GlobalTemplateManager, Banks) == 0x20);

// The surface action factory's adapter passes no story action and no class
// descriptions; the game function stores the two at +0x10 and +0x50 of the
// action, and Norbyte's caller then stores the class descriptions itself.
static_assert(offsetof(esv::SurfaceAction, StoryActionID) == 0x10);
static_assert(offsetof(esv::SurfaceAction, ClassDescriptionMgr) == 0x50);

// The game state event manager's listeners: Norbyte's structure is the
// game's (a vtable pointer, then the array of listener pointers).
static_assert(offsetof(GameStateEventManager, Callbacks) == 8);

}

std::span<MemberRow const> StaticSymbolRows() { return kRows; }
std::span<OtherMember const> StaticSymbolOtherMembers() { return kOtherMembers; }
size_t StaticSymbolsSize() { return sizeof(StaticSymbols); }

END_NS()
