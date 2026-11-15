#pragma once

#include <cstddef>
#include <string>

#include <Binding/ComponentSignals.h>
#include <Platform/CodeShape.h>
#include <Platform/Symbols.h>

// The check behind ComponentSignalsConfirmed (Binding/ComponentSignals.h), in
// parts a test or a tool can run on a game binary and on stand-ins.

namespace bg3se::mac::binding {

// The game's two functions that call a component type's subscribers, as they
// were read. Each has two forms: one for a component kept in the entity's
// storage, and one, an instruction longer, for a component kept outside it,
// whose address is loaded once more before the call. Neither has an operand
// that depends on where the function is.
struct ComponentSignalSender
{
    char const* Name;
    CodeShape Inline;
    CodeShape Boxed;
};
inline constexpr ComponentSignalSender kSendComponentAttached{
    "SendComponentAttachedSignal", { 58, 0x0b329051ba5e15b2ull }, { 59, 0xa05b60de165264cfull } };
inline constexpr ComponentSignalSender kSendComponentDetached{
    "SendComponentDetachedSignal", { 58, 0xebeb300b9e3f0a36ull }, { 59, 0xd3dab5a409f07643ull } };

// Whether `code` (in `image`, `available` instructions readable from it) is
// the sender as it was read, in either form.
bool IsComponentSignalSender(ComponentSignalSender const& sender, void const* code, size_t available);

// Whether the virtual table of an ecs::ComponentOps<T> whose first slot is at
// the link-time address `table` in `image` names the two senders that were
// read, in the slots they are called through: both are in the image's code
// and are that code. Otherwise the reason, in `problem`. Works on a loaded
// image and on a file whose pointers can be read from it.
bool ChecksComponentSignalTable(GameImage const& image, uint64_t table, std::string& problem);

// Whether the component operations object `componentOps` of the game loaded
// as `image` calls the subscribers of `callbacks` as described in
// Binding/ComponentSignals.h: its virtual table is in the image and passes
// the check above, and its callbacks and world are `callbacks` and `world`.
// Otherwise the reason, in `problem`.
bool ChecksComponentSignals(GameImage const& image, void const* componentOps, void const* callbacks, void const* world,
    std::string& problem);

}
