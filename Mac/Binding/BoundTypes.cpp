#include <stdafx.h>
#include <GameDefinitions/Character.h>
#include <GameDefinitions/Item.h>
#include <GameDefinitions/Components/All.h>
#include <GameDefinitions/GuidResources.h>
#include <GameDefinitions/Resources.h>
#include <GameDefinitions/Stats/Stats.h>
#include <GameDefinitions/Symbols.h>
#include <GameDefinitions/UI.h>
#include <Lua/Shared/Proxies/PropertyMapDependencies.h>

#include <Binding/Layouts.h>
#include <Binding/TypeIndexBinding.h>

// The names EntitySystemHelpersBase::UpdateComponentMappings looks up
// (GameDefinitions/EntitySystem.cpp), from the same three lists it uses.

BEGIN_NS(mac::binding)

namespace {

constexpr BoundType kBoundTypes[] = {
#define T(cls) { cls::EngineClass, BoundAs::Component, cls::OneFrame },
#include <GameDefinitions/Components/AllComponentTypes.inl>
#undef T

#define T(cls) { cls::EngineClass, BoundAs::System, false },
#include <GameDefinitions/Components/AllSystemTypes.inl>
#undef T

#define FOR_RESOURCE_TYPE(cls) { resource::cls::EngineClass, BoundAs::StaticData, false },
    FOR_EACH_GUID_RESOURCE_TYPE()
#undef FOR_RESOURCE_TYPE
};

// What leads his code to a structure whose layout is proven per game
// (Binding/Layouts.h).
static_assert(std::is_same_v<decltype(StaticSymbols::ls__gGlobalResourceManager), ResourceManager**>);
static_assert(std::is_same_v<decltype(ResourceManager::UI), ui::GameUI*>);
static_assert(std::is_same_v<decltype(StaticSymbols::ls__gGlobalStringTable), GlobalStringTable**>);
static_assert(std::is_same_v<decltype(StaticSymbols::gRPGStats), stats::RPGStats**>);
static_assert(std::is_same_v<decltype(StaticSymbols::ecl__EoCClient), ecl::EoCClient**>);
static_assert(std::is_same_v<decltype(StaticSymbols::esv__EoCServer), esv::EoCServer**>);
constexpr LayoutDependent kLayoutDependents[] = {
    // The table of fixed strings, and with it the four functions his fixed
    // strings go through: BaseString.inl reads through the table's pointer
    // without a test wherever it has the function to make or release a
    // string, and takes a string's header from before the text it is given.
    // Without all five a fixed string of his is the null one and stays it.
    { GameShape::StringTable, "ls__gGlobalStringTable", nullptr },
    { GameShape::StringTable, "ls__GlobalStringTable__MainTable__CreateFromString", nullptr },
    { GameShape::StringTable, "ls__GlobalStringTable__MainTable__DecRef", nullptr },
    { GameShape::StringTable, "ls__FixedString__GetString", nullptr },
    { GameShape::StringTable, "ls__FixedString__IncRef", nullptr },
    { GameShape::ResourceManager, "ls__gGlobalResourceManager", nullptr },
    // The game UI is reached through the resource manager (ResourceManager::UI).
    { GameShape::GameUi, "ls__gGlobalResourceManager", nullptr },
    // And the UI's state machine through the game UI (StateMachine.StateMachineComponent).
    { GameShape::UiStates, "ls__gGlobalResourceManager", nullptr },
    { GameShape::EffectsManager, nullptr, EffectsManager::EngineClass },
    // The stats object: his code reads its members wherever GetStats gives
    // one, and the stats load order helper does without a test.
    { GameShape::Stats, "gRPGStats", nullptr },
    // What does the work of a stats function the game has only inline
    // (Binding/StatsFunctions.h) reads the stats object itself, not through
    // the member above: it is kept back with it.
    { GameShape::Stats, "eoc__ParseStaticBoosts", nullptr },
    { GameShape::Stats, "stats__Object__SetPropertyString", nullptr },
    { GameShape::Files, "stats__Object__SetPropertyString", nullptr },
    { GameShape::Stats, "eoc__PassivePrototype__Init", nullptr },
    { GameShape::Stats, "eoc__InterruptPrototype__Init", nullptr },
    // The four prototype managers and the functions that fill a prototype:
    // his SyncStat and Ext.Stats' cached prototypes read and write the
    // prototypes through them.
    { GameShape::Prototypes, "eoc__SpellPrototypeManager", nullptr },
    { GameShape::Prototypes, "eoc__SpellPrototype__Init", nullptr },
    { GameShape::Prototypes, "eoc__StatusPrototypeManager", nullptr },
    { GameShape::Prototypes, "eoc__StatusPrototype__Init", nullptr },
    { GameShape::Prototypes, "eoc__PassivePrototypeManager", nullptr },
    { GameShape::Prototypes, "eoc__PassivePrototype__Init", nullptr },
    { GameShape::Prototypes, "eoc__InterruptPrototypeManager", nullptr },
    { GameShape::Prototypes, "eoc__InterruptPrototype__Init", nullptr },
    // The client and server objects, and the mod manager each leads to:
    // the game states, the entity worlds and the modules are read off them.
    { GameShape::Client, "ecl__EoCClient", nullptr },
    { GameShape::Server, "esv__EoCServer", nullptr },
    { GameShape::Modules, "ecl__EoCClient", nullptr },
    { GameShape::Modules, "esv__EoCServer", nullptr },
    // The path roots, read as pointers to strings, and the file reader the
    // extender's own file reads construct and read back.
    { GameShape::Files, "ls__PathRoots", nullptr },
    { GameShape::Files, "ls__FileReader__ctor", nullptr },
    // The game state event managers: his listeners are appended to the
    // game's array and called by the game through their third slot.
    { GameShape::StateEvents, "ecl__gGameStateEventManager", nullptr },
    { GameShape::StateEvents, "esv__gGameStateEventManager", nullptr },
    // The global switches: scripts read and write them through
    // Ext.Utils.GetGlobalSwitches, and the overlay reads the language
    // without a test when it is initialised.
    { GameShape::Switches, "ls__GlobalSwitches", nullptr },
    // The translated string repository: his translated strings and Ext.Loca
    // read its pools and take its lock.
    { GameShape::TranslatedStrings, "ls__gTranslatedStringRepository", nullptr },
    // The server's template cache: his caching of templates reads its maps
    // and arrays and takes its lock.
    { GameShape::TemplateCache, "esv__CacheTemplateManager", nullptr },
    // The GUID resource manager: Ext.StaticData looks the managers up in it.
    { GameShape::StaticData, "eoc__gGuidResourceManager", nullptr },
    // The translated string key manager: Ext.Loca reads and inserts keys.
    { GameShape::TranslatedStringKeys, "ls__gTranslatedStringKeyManager", nullptr },
    // The drag and drop manager: Ext.UI.GetDragDrop reads a player's data.
    { GameShape::DragDrop, "ls__gDragDropManager", nullptr },
    // The cursor control: Ext.UI.GetCursorControl hands it to Lua.
    { GameShape::Cursor, "ecl__gCursorControl", nullptr },
    // The UI library's error handler: Ext.UI.EnableErrorReporting stores
    // Norbyte's handler, or none, where the library calls it from.
    { GameShape::UiErrorHandler, "Noesis__gErrorHandler", nullptr },
    // NoesisGUI's symbol and reflection tables, what it allocates with, its
    // types, the stored values of its objects and its visual tree: every
    // Noesis object reaches Lua through Ext.UI's GetRoot, GetStateMachine,
    // RegisterType, Instantiate or Box, all shut while the tables are kept
    // back (Binding/LuaGates.cpp).
    { GameShape::NoesisTables, "Noesis__Reflection__Data", nullptr },
    { GameShape::NoesisTables, "Noesis__SymbolManager__Data", nullptr },
    { GameShape::NoesisMemory, "Noesis__Reflection__Data", nullptr },
    { GameShape::NoesisMemory, "Noesis__SymbolManager__Data", nullptr },
    { GameShape::NoesisTypes, "Noesis__Reflection__Data", nullptr },
    { GameShape::NoesisTypes, "Noesis__SymbolManager__Data", nullptr },
    { GameShape::NoesisValues, "Noesis__Reflection__Data", nullptr },
    { GameShape::NoesisValues, "Noesis__SymbolManager__Data", nullptr },
    { GameShape::NoesisTree, "Noesis__Reflection__Data", nullptr },
    { GameShape::NoesisTree, "Noesis__SymbolManager__Data", nullptr },
    // The texture atlases: Ext.StaticData finds an atlas by its path in the
    // manager's map.
    { GameShape::AtlasPaths, "ls__gTextureAtlasMap", nullptr },
    // The boost prototype manager: Ext.Stats.GetCachedBoost looks a boost up
    // in it and hands its prototype to Lua.
    { GameShape::Boosts, "eoc__BoostPrototypeManager", nullptr },
    // The input manager: Ext.Input.GetInputManager hands it to Lua.
    { GameShape::Input, "ls__gInputManager", nullptr },
    // The level managers: the current level, and the levels by name.
    { GameShape::Levels, "esv__LevelManager", nullptr },
    { GameShape::Levels, "ecl__LevelManager", nullptr },
    // The entity system: the world, its storages and handles, the bit sets
    // and lock-free queues in it, the command buffers, the immediate cache,
    // the queries, the systems and the replication buffers. His code reads
    // them without a test from the update of the world on: kept back with
    // that hook, a critical member, start-up refuses.
    { GameShape::EntityWorld, "ecs__EntityWorld__Update", nullptr },
    { GameShape::EntityCommands, "ecs__EntityWorld__Update", nullptr },
    { GameShape::EntityQueries, "ecs__EntityWorld__Update", nullptr },
    { GameShape::EntityReplication, "ecs__EntityWorld__Update", nullptr },
    { GameShape::Queues, "ecs__EntityWorld__Update", nullptr },
    // The variants are in components and in the stats object.
    { GameShape::Variant, "ecs__EntityWorld__Update", nullptr },
    { GameShape::Variant, "gRPGStats", nullptr },
    // The network peer: his network code reaches the client's and the
    // server's peer through the client and server objects.
    { GameShape::NetPeer, "ecl__EoCClient", nullptr },
    { GameShape::NetPeer, "esv__EoCServer", nullptr },
    // The members proven of components: the three triggers, the projectile,
    // the timeline actor control, the companion definition, the construction;
    // and the timeline shot, reached through the effects' and the client
    // timeline system's timelines.
    { GameShape::Components, nullptr, "esv::SoundVolumeTrigger" },
    { GameShape::Components, nullptr, "esv::TimelineSceneTrigger" },
    { GameShape::Components, nullptr, "ecl::TimelineSceneTrigger" },
    { GameShape::Components, nullptr, "esv::Projectile" },
    { GameShape::Components, nullptr, "eoc::SyncedTimelineActorControlComponent" },
    { GameShape::Components, nullptr, "eoc::character_creation::CompanionDefinitionComponent" },
    { GameShape::Components, nullptr, "ls::Construction" },
    { GameShape::Components, nullptr, "ls::EffectComponent" },
    { GameShape::Components, nullptr, "ecl::TimelineSystem" },
    // The channels of a timeline's material and reflection parameters, read
    // through the same timelines.
    { GameShape::TimelineChannels, nullptr, "ls::EffectComponent" },
    { GameShape::TimelineChannels, nullptr, "ecl::TimelineSystem" },
    // Systems whose Mac objects lack a member or have a shorter one. The
    // genome manager, also of the shape, is reached by none of his code.
    { GameShape::Systems, nullptr, "ls::level::LevelInstanceLoadSystem" },
    { GameShape::Systems, nullptr, "esv::progression::ManagementSystem" },
    { GameShape::Systems, nullptr, "esv::DialogSystem" },
    // The server's character: Ext.Net reads the user that owns it to send to
    // its client, to leave its client out of a broadcast and to say whether
    // its client has the extender; its owners and its users are read and
    // written through its proxy.
    { GameShape::Characters, nullptr, "esv::Character" },
    // The UI's widgets and state instances: scripts reach them from the
    // state machine and the element tree, behind the same roots as every
    // other Noesis object.
    { GameShape::UiWidgets, "Noesis__Reflection__Data", nullptr },
    { GameShape::UiWidgets, "Noesis__SymbolManager__Data", nullptr },
    // The server's item: scripts read it through its proxy component.
    { GameShape::Items, nullptr, "esv::Item" },
    // The client's character: scripts read it through its proxy component.
    { GameShape::ClientCharacters, nullptr, "ecl::Character" },
    // The server's statuses: scripts reach them through the status machine
    // of a character or an item.
    { GameShape::Statuses, nullptr, "esv::Character" },
    { GameShape::Statuses, nullptr, "esv::Item" },
    // The camera controller: scripts reach it through the camera component's
    // member.
    { GameShape::Cameras, nullptr, "ls::CameraComponent" },
    // A dialogue's members, which Ext.StaticData.LoadResource and
    // ReleaseResource read and write once the function that loads one is
    // bound, and the dialog manager the client's timeline system holds
    // (Ext.Utils.GetDialogManager on the client).
    { GameShape::Dialogs, "dlg__DialogManager__PerformDialogLoad", nullptr },
    { GameShape::Dialogs, nullptr, "ecl::TimelineSystem" },
};

}

std::span<BoundType const> BoundTypes()
{
    return kBoundTypes;
}

std::span<LayoutDependent const> LayoutDependents()
{
    return kLayoutDependents;
}

END_NS()
