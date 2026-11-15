#pragma once

#include <cstddef>
#include <cstdint>

// The Mac game's client and server objects (ecl::EocClient behind
// ecl::EocClient::m_ptr, esv::EocServer behind esv::EocServer::m_ptr) and the
// mod manager each has, as the game's own code shows them
// (Layout/GameShapes.cpp says which code). Norbyte's declarations
// (GameDefinitions/GameState.h, Module.h) are asserted to give the same
// offsets (Layout/DefinitionChecks.cpp).
//
// The client object begins with four vtable pointers that follow one
// another, where the Windows object has a word between the second and the
// third: every member of it is 8 bytes earlier than there. The server object
// and the mod manager are as on Windows.

namespace bg3se::mac {

// ecl::EocClient.
constexpr size_t kClientVtables = 4;
constexpr size_t kClientGameStateMachine = 0x88;
constexpr size_t kClientGameClient = 0x90;
constexpr size_t kClientModManager = 0x138;         // ls::ModManager*
constexpr size_t kClientEntityWorld = 0x1a0;

// esv::EocServer.
constexpr size_t kServerGameStateMachine = 0xa0;
constexpr size_t kServerGameServer = 0xa8;
constexpr size_t kServerModManager = 0xd0;          // ls::ModManager, in the object
constexpr size_t kServerEntityWorld = 0x288;
// esv::GameStateMachine: the state's number.
constexpr size_t kServerStateMachineState = 0x10;

// ecl::GameStateMachine: the state's number.
constexpr size_t kClientStateMachineState = 0x08;

// ecl::GameStateEventManager and esv::GameStateEventManager: the listeners,
// an array of pointers to objects (pointer, 32-bit capacity, 32-bit size).
// A change of state calls each through the slot after its two destructors
// with the address of two 32-bit numbers, the state left and the state
// entered; the client's listeners have the loading tick in the next slot.
constexpr size_t kStateEventListeners = 0x08;
constexpr size_t kStateEventListenersSize = 0x14;
constexpr size_t kStateListenerChanged = 0x10;      // offset in the listener's vtable
constexpr size_t kStateListenerTickLoad = 0x18;

// esv::GameServer: Norbyte's net::GameServer, behind the server object's
// pointer. After the peer it has three vtable pointers where the Windows
// server has four: every member is 8 bytes earlier than there.
constexpr size_t kGameServerReplication = 0x3a8;        // ecs::sync::EntityReplicationAuthority
constexpr size_t kGameServerConnectedPeers = 0x5f8;     // array of peer ids
constexpr size_t kGameServerUserIds = 0x608;            // array: the first of the user mappings
constexpr size_t kGameServerActivePeers = 0x650;        // array of peer ids
constexpr size_t kGameServerKickedPeers = 0x680;        // array of peer ids
constexpr size_t kGameServerPeerInfo = 0x690;           // ref map: count, 32-bit bucket count, buckets
constexpr size_t kGameServerLocalPeer = 0x6b4;          // 32 bits

// ls::ModManager.
constexpr size_t kModManagerBaseModule = 0x38;      // ls::Module
constexpr size_t kModManagerLoadOrder = 0x130;      // array of ls::Module: pointer, 32-bit capacity, 32-bit size
constexpr size_t kModManagerLoadOrderSize = 0x13c;

// ls::Module.
constexpr size_t kModuleSize = 0xf0;
constexpr size_t kModuleInfo = 0x08;
constexpr size_t kModuleDependencies = 0xc0, kModuleConflicts = 0xd0, kModuleAddons = 0xe0;     // arrays of ls::ModuleShortDesc

// ls::ModuleInfo. Its texts are the game's 16-byte strings: the last byte
// says whether the characters are in the string or behind its pointer.
constexpr size_t kModuleInfoId = 0x00;              // fixed string: the mod's UUID as text
constexpr size_t kModuleInfoGuid = 0x08;            // the UUID, 16 bytes
constexpr size_t kModuleInfoName = 0x18;
constexpr size_t kModuleInfoLevels = 0x28;          // five fixed strings
constexpr size_t kModuleInfoHash = 0x50;
constexpr size_t kModuleInfoDirectory = 0x60;
constexpr size_t kModuleInfoScripts = 0x70;
constexpr size_t kModuleInfoAuthor = 0x80;
constexpr size_t kModuleInfoDescription = 0x90;
constexpr size_t kGameStringSize = 0x10;

}
