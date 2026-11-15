#include <stdafx.h>
#include <GameDefinitions/GameState.h>
#include <GameDefinitions/Net.h>

#include "GameComponents.h"
#include "GameNetwork.h"
#include "GameSession.h"
#include "GameStats.h"

// offsetof on Norbyte's structures, which are not standard-layout (most have
// a base class), as Layout/DefinitionChecks.cpp does.
#pragma clang diagnostic ignored "-Winvalid-offsetof"

// What Norbyte's network definitions (GameDefinitions/Net.h) must come to in
// this build for the Mac game's network session (Layout/GameNetwork.h) to be
// the one his networking reads: the client's host and status
// (Extender/Client/ClientNetworking.inl), the build's name of the connect
// message he appends to (Extender/Shared/Hooks.cpp) and reads on the server
// (Extender/Server/ServerNetworking.inl), the user a message came from, and
// the peers' players the server's local user is found among
// (Extender/Shared/ExtenderNet.cpp). None needed an edit. This file has no
// code.

BEGIN_SE()

static_assert(offsetof(net::Client, HostPeerId) == mac::kClientHostPeer && sizeof(net::Client::HostPeerId) == 4);
static_assert(offsetof(net::GameClient, LobbyManager) == mac::kGameClientLobby
    && offsetof(net::GameClient, Status) == mac::kGameClientStatus && sizeof(net::GameClient::Status) == 4);
static_assert(std::is_same_v<decltype(ecl::EoCClient::GameClient), net::GameClient*>);

static_assert(offsetof(net::ClientConnectMessage, GameVersion) == mac::kConnectGameVersion
    && offsetof(net::ClientConnectMessage, Build) == mac::kConnectBuild);
static_assert(std::is_same_v<decltype(net::ClientConnectMessage::Build), STDString> && sizeof(STDString) == mac::kGameStringSize);

static_assert(offsetof(net::Protocol, Peer) == mac::kProtocolPeer);

static_assert(offsetof(net::MessageContext, UserID) == mac::kContextUser && sizeof(UserId) == 4);
static_assert(UserId(PeerId{ 3 }, 2).Id == ((3u << mac::kUserPeerShift) | 2) && UserId(0x30002u).GetPeerId() == PeerId{ 3 });

static_assert(std::is_same_v<decltype(net::GameServer::PeerInfo), LegacyRefMap<PeerId, net::GamePeerInfo>>);
static_assert(std::is_same_v<decltype(net::GamePeerInfo::Players), LegacyRefMap<uint16_t, net::GamePlayerInfo>>);
static_assert(offsetof(net::GamePeerInfo, Players) == mac::kPeerInfoPlayers && offsetof(net::GamePeerInfo, DLCs) == mac::kPeerInfoDlcs
    && sizeof(net::GamePeerInfo) == mac::kPeerInfoSize && sizeof(net::GamePlayerInfo) == mac::kPlayerInfoSize);
static_assert(sizeof(LegacyRefMap<uint16_t, net::GamePlayerInfo>) == 0x10);

using PeerNode = MapNode<PeerId, net::GamePeerInfo>;
using PlayerNode = MapNode<uint16_t, net::GamePlayerInfo>;
static_assert(offsetof(PeerNode, Next) == mac::kMapNodeNext && offsetof(PeerNode, Key) == mac::kMapNodeKey
    && offsetof(PeerNode, Value) == mac::kMapNodeObjectValue && sizeof(PeerNode) == mac::kMapNodeObjectValue + mac::kPeerInfoSize);
static_assert(offsetof(PlayerNode, Next) == mac::kMapNodeNext && offsetof(PlayerNode, Key) == mac::kMapNodeKey && sizeof(PlayerNode::Key) == 2
    && offsetof(PlayerNode, Value) == mac::kMapNodeObjectValue && sizeof(PlayerNode) == mac::kMapNodeObjectValue + mac::kPlayerInfoSize);

END_SE()
