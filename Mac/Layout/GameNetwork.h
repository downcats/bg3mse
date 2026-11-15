#pragma once

#include <cstddef>
#include <cstdint>

// What of the game's network session the extender's networking reads, where
// the Mac game's own code shows it (Layout/GameShapes.cpp, the network
// session's proofs). Norbyte's declarations (GameDefinitions/Net.h) are
// asserted to give these offsets (Layout/NetworkChecks.cpp). The peer, the
// message factory, a message and the serializer are in Layout/GameComponents.h.

namespace bg3se::mac {

// ecl::GameClient (his net::Client and net::GameClient): the host's peer, the
// lobby manager, and the connection's status, which is 2 once the host has
// accepted the connection and 0 again when it fails.
constexpr size_t kClientHostPeer = 0x398;
constexpr size_t kGameClientLobby = 0x4b8;
constexpr size_t kGameClientStatus = 0x4c0;
constexpr uint32_t kGameClientAccepted = 2;

// eocnet::ClientConnectMessage: the game's version and the build's name, which
// the client fills in and the message carries, the version first.
constexpr size_t kConnectGameVersion = 0x28;
constexpr size_t kConnectBuild = 0x38;

// net::Protocol: the peer it is on.
constexpr size_t kProtocolPeer = 0x8;

// net::MessageContext: the user a message came from. A user is the peer in
// the upper 16 bits and the player's index on that peer in the lower 16.
constexpr size_t kContextUser = 0x0;
constexpr uint32_t kUserPeerShift = 16;

// esv::GamePeerInfo, the value of the server's map of peers: the map of the
// peer's players by index, the DLCs. A node of either map holds its value at
// +0x10, after the next node and the key.
constexpr size_t kPeerInfoPlayers = 0x8;
constexpr size_t kPeerInfoDlcs = 0x18;
constexpr size_t kPeerInfoSize = 0x58;
constexpr size_t kPlayerInfoSize = 0x48;
constexpr size_t kMapNodeObjectValue = 0x10;

}
