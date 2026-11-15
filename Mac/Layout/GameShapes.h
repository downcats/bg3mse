#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <Platform/CodeShape.h>
#include <Platform/Symbols.h>

// The proofs that the Mac game has the shapes the build assumes: for each
// type that is declared for Mac under Mac/Layout, game functions whose code
// shows the type's fields, size or protocol. A proof names a function by its
// exact mangled symbol, requires its code to be the code that was read
// (Platform/CodeShape.h: unchanged wherever a patch moves it, different as
// soon as an instruction changes), and compares the offsets and constants in
// that code with offsetof, sizeof and the constants of the Mac type, so the
// type is stated once. A shape is confirmed only when every proof of it
// holds; a missing symbol, changed code or a differing operand leaves it
// unconfirmed, and whatever depends on the shape must then not be used.
//
// The same check runs in the game and on a game binary mapped from disk.

namespace bg3se::mac {

enum class GameShape : uint8_t
{
    CriticalSection,        // GameCriticalSection: ls::CriticalSection
    SRWKernelLock,          // GameSRWKernelLock: ls::SRWKernelLock
    RecursiveKernelLock,    // GameRecursiveKernelLock: ls::RecursiveSRWLock<ls::SRWKernelLock>
    RecursiveSpinLock,      // GameRecursiveSpinLock: ls::RecursiveSRWLock<ls::SRWSpinLock>
    Variant,                // GameVariant: ls::Variant
    Queues,                 // Layout/GameQueues.h: ls::MPMCQueueBounded, ls::MPMCQueueUnbounded
    NoesisLocks,            // GameNoesisSpinLock: the locks of the game's NoesisGUI
    EntityWorld,            // Layout/GameEntityWorld.h: ecs::EntityWorld and the component registry
    EntityCommands,         // Layout/GameEntityWorld.h: command buffers, frame storages and allocator, the immediate cache
    EntityQueries,          // Layout/GameEntityWorld.h: queries and systems
    EntityReplication,      // Layout/GameEntityWorld.h: dirty fields, replication peer and authority
    EffectsManager,         // Layout/GameEffectsManager.h: ls::EffectsManager
    ResourceManager,        // Layout/GameResourceManager.h: ls::ResourceManager
    StringTable,            // Layout/GameStringTable.h: ls::gst, the table of fixed strings
    GameUi,                 // Layout/GameUi.h: gui::GameUI
    Systems,                // Layout/GameSystems.h: three objects without a member the Windows ones have
    Components,             // Layout/GameComponents.h: members of six component objects and a timeline component
    NetPeer,                // Layout/GameComponents.h: net::AbstractPeer, the message factory, a message, the serializer
    Stats,                  // Layout/GameStats.h: RPGStats, its managers, a stats object
    Client,                 // Layout/GameSession.h: ecl::EocClient
    Server,                 // Layout/GameSession.h: esv::EocServer
    Modules,                // Layout/GameSession.h: ls::ModManager, ls::Module, ls::ModuleInfo
    Files,                  // Layout/GameFiles.h: ls::STDString, ls::Path::s_Roots, ls::FileReader
    StateEvents,            // Layout/GameSession.h: the game state event managers and how a listener is called
    UiStates,               // Layout/GameUi.h: ui::StateMachineComponent
    TimelineChannels,       // how the channels of a timeline parameter are returned (Layout/GameShapes.cpp)
    Switches,               // Layout/GameSwitches.h: eoc::EoCGlobalSwitches
    Prototypes,             // Layout/GameStats.h: the spell, status, passive and interrupt prototypes and their managers
    VirtualResults,         // Layout/GameVirtualResults.h: how virtual functions hand back what is not one integer or pointer
    TranslatedStrings,      // Layout/GameGlobals.h: ls::TranslatedStringRepository
    TemplateCache,          // Layout/GameGlobals.h: ls::CacheTemplateManagerBase
    StaticData,             // Layout/GameGlobals.h: ls::ImmutableDataHeadmaster
    TranslatedStringKeys,   // Layout/GameGlobals.h: ls::TranslatedStringKeyManager
    DragDrop,               // Layout/GameGlobals.h: ls::DragDropManager
    Cursor,                 // Layout/GameGlobals.h: ecl::CursorControl
    UiErrorHandler,         // how NoesisGUI calls the error handler in (anonymous namespace)::gErrorHandler
    AtlasPaths,             // Layout/GameGlobals.h: how the map of texture atlases hashes a path
    Boosts,                 // Layout/GameGlobals.h: eoc::BoostPrototypeManager and its prototypes
    Input,                  // Layout/GameGlobals.h: ls::InputManager, at the members its code shows
    Audio,                  // Layout/GameAudio.h: ww::WwiseManager's sound objects
    Levels,                 // Layout/GameGlobals.h: ls::LevelManager's current level and levels
    NetworkSession,         // Layout/GameNetwork.h: the client's connection, the connect message, the server's peers, a message's user
    Characters,             // Layout/GameCharacter.h: esv::Character, its owner, its users
    PhysicsObjects,         // Layout/GameResourceManager.h: a physics resource's boxes and capsules
    NoesisTables,           // Layout/GameNoesis.h: NoesisGUI's symbol table and type registry
    NoesisMemory,           // Layout/GameNoesis.h: what NoesisGUI allocates and frees with
    NoesisTypes,            // Layout/GameNoesis.h: Noesis types, properties, references, vectors, strings
    NoesisValues,           // Layout/GameNoesis.h: Noesis dependency objects, properties, stored values, events
    NoesisTree,             // Layout/GameNoesis.h: Noesis visual and logical parents, routed event handlers
    UiWidgets,              // Layout/GameUi.h: ui::Widget and ui::State, the widgets and state instances of the UI
    Items,                  // Layout/GameCharacter.h: esv::Item
    ClientCharacters,       // Layout/GameCharacter.h: ecl::Character, and the controller mode of its input controller
    Statuses,               // Layout/GameCharacter.h: esv::Status, the members every server status has
    Cameras,                // Layout/GameCamera.h: rf::CameraController, the rf::Camera in it, the head of the camera component
    Dialogs,                // Layout/GameSystems.h: LSDialogs::Dialog, and ecl::TimelineSystem with its dialog manager
    Count
};

char const* Describe(GameShape shape);

struct FunctionProof
{
    GameShape Shape;
    char const* Function;       // readable name
    bool Found{ false };
    CodeShape Code{};           // what the game has, when found
    std::string Problem;        // empty when the proof holds
};

struct GameShapes
{
    std::vector<FunctionProof> Proofs;

    // Every proof of `shape` holds (and there is at least one).
    bool Confirmed(GameShape shape) const;
    bool AllConfirmed() const;
};

GameShapes CheckGameShapes(GameImage const& image);

// One line per shape that is not confirmed, naming the function and the reason.
void LogUnconfirmedShapes(GameShapes const& shapes);

}
