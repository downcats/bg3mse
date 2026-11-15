#pragma once

#include <cstddef>
#include <cstdint>

#include "GameEntityRef.h"

// How the macOS game calls a subscriber of a component's "attached" and
// "detached" signals, which Norbyte's code subscribes to for
// Ext.Entity.OnCreate, OnDestroy and Subscribe
// (Lua/Shared/EntityComponentEvents.inl), and the check that the game in this
// process still calls it that way. Included by his sources and by bg3mse's.
//
// The signals are ls::Signal<void (ecs::EntityRef, T&)>, one pair for each
// component type, in the type's ecs::ComponentCallbacks. Read in game build
// 4.1.1.7631656:
//
//   - ls::Signal<...>::Connect and the three methods of a connected function
//     (FunctorMethodsVSSO<...>::Call, Copy, MoveDestroy) show the objects
//     Norbyte declares in CoreLib/Base/BaseFunction.h: a signal is the next
//     registrant's number and an array of connections; a connection is 0x48
//     bytes, a function and its registrant's number at +0x40; a function is a
//     pointer to its storage followed by the storage, which starts with the
//     three methods. So his Signal, Function and FunctionStorage need no
//     change, and neither does how a function is copied, moved or destroyed.
//   - ecs::ComponentOps<T>::SendComponentAttachedSignal and
//     SendComponentDetachedSignal, the third and fourth virtual functions of a
//     component type's operations (after the two destructors), call each
//     connection of the type's signal: the storage in x0, the entity's handle
//     in x1, the entity world (the operations' own, at +0x20) in x2 and the
//     component in x3. That is ecs::EntityRef passed by value: sixteen bytes
//     of plain data, which this platform passes in two registers.
//
// On Windows a structure of sixteen bytes is passed by the address of a copy,
// which is how Norbyte's ComponentSignal declares it: Signal<EntityRef*,
// void*>. A function subscribed with that declaration here takes the entity's
// handle for the address of an EntityRef. So outside MSVC ComponentSignal is
// declared with GameEntityRef by value (Binding/GameEntityRef.h,
// GameDefinitions/EntitySystem.h); his handlers are subscribed through
// ComponentSignalHandler (Binding/ComponentSignalHandler.h), which hands them
// the address of an EntityRef as they expect; and where his own code sends a
// signal (ImmediateWorldCache, GameDefinitions/EntitySystem.cpp), it passes
// the entity as the game's subscribers take it.
//
// His own EntityRef cannot be the parameter: its EntityHandle has a copy
// constructor written out, and a structure with one is passed by address on
// this platform too, whatever its size.

namespace bg3se::mac::binding {

// Where the two are in the virtual table of ecs::ComponentOps<T>.
constexpr size_t kComponentOpsSendAttached = 2;
constexpr size_t kComponentOpsSendDetached = 3;

// What the two functions read, which Norbyte's declarations must give
// (asserted in Binding/ComponentSignalHandler.h).
constexpr size_t kComponentOpsCallbacks = 0x10;
constexpr size_t kComponentOpsWorld = 0x20;
constexpr size_t kComponentCallbacksAttached = 0x08;        // the signal: registrant number, then the array
constexpr size_t kComponentCallbacksDetached = 0x20;
constexpr size_t kSignalConnections = 0x08;                 // pointer, capacity at +8, size at +0xc
constexpr size_t kSignalConnectionSize = 0x48;
constexpr size_t kFunctionStorage = 0x08;                   // in a function: after the pointer to it
constexpr size_t kFunctionStorageCall = 0x00;

// Whether a subscriber of the signals in `callbacks` (the component type's
// ecs::ComponentCallbacks) is called as described above, by the component
// operations object `componentOps` of the same type in the entity world
// `world`, in the game of this process: the object's virtual table is in the
// game's data, the two functions it names there are in the game's code and
// are the two that were read (Binding/ComponentSignalCheck.h), and the
// object's callbacks and world, which those functions read, are the ones
// given. A virtual table that passed is remembered. False too when the
// game's image cannot be read. A refusal is written to the log once for each
// operations object, with the reason.
bool ComponentSignalsConfirmed(void const* componentOps, void const* callbacks, void const* world);

}
