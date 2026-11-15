#pragma once

#include <cstddef>

namespace bg3se::mac::engine { class Resolution; }

// The function Norbyte wraps to see the client's connect message being
// written (Hooks::HookNetworkMessages, Extender/Shared/Hooks.cpp).
//
// On Windows he takes it from the message's vtable through net::Message::VMT,
// where it is the second slot. The macOS game's vtables begin with two
// destructors, so that slot is the deleting destructor here and the function
// is the third. It is the engine table's entry
// eocnet__ClientConnectMessage__Serialize, found by its name and checked
// there; the table's vtable entry says where the message's vtable holds it.

namespace bg3se::mac::binding {

// The two entries of the engine table BindNetMessages takes.
constexpr char const* kConnectMessageEntryNames[] = { "eocnet__ClientConnectMessage__Serialize", "eocnet::ClientConnectMessage" };

// The function to wrap for `message`, the factory's template of the client's
// connect message: the entry's function, when `message` has the vtable the
// table resolved and that vtable holds the function in the slot the table
// names. nullptr otherwise, and then nothing must be wrapped.
void* ClientConnectMessageSerialize(void const* message);

// Takes the function, the vtable and the slot from a resolution of the
// running game. Without all of them ClientConnectMessageSerialize gives
// nullptr.
void BindNetMessages(engine::Resolution const& resolution);

// The decision alone: `function` when the object at `message` starts with
// `vtable` and the pointer at `slot` bytes into that vtable is `function`.
void* SerializeOfMessage(void const* message, void const* vtable, size_t slot, void* function);

// For tests: what BindNetMessages would have taken; nullptrs unbind.
void SetConnectMessageForTest(void const* vtable, size_t slot, void* function);

// Whether the game's network session is laid out as this build declares it
// (Layout/GameShapes.h, the network session: the client's host and status,
// the connect message's strings, the server's peers and their players, the
// user a message came from). BindNetMessages decides it on the running game
// and logs why when it is not. Until then, and on a game where it is not,
// Norbyte's network managers find neither the client's nor the server's peer
// and the server no local user, so the extender registers no message and
// sends, receives and announces nothing.
bool NetworkSessionConfirmed();

// For tests: what BindNetMessages would have decided.
void SetNetworkSessionConfirmedForTest(bool confirmed);

}
