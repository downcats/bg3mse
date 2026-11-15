#pragma once

#include "ComponentSignals.h"

#include <GameDefinitions/EntitySystem.h>

// Norbyte's handlers of a component's signals, subscribed as the macOS game
// calls a subscriber (Binding/ComponentSignals.h). Included by his sources:
// this is the one part of the component signals that sees his types.
//
//     callbacks->OnConstruct.Add(ComponentSignalHandler<&Hooks::OnComponentCreated>(this, type));
//
// stands where he writes MakeFunction(&Hooks::OnComponentCreated, this, type),
// for a handler void (Hooks::*)(ComponentTypeIndex const&, EntityRef*, void*):
// the game's call arrives with the entity by value, and the handler is given
// the address of an EntityRef made of it, which lives for the call, as the
// copy it gets on Windows does.

BEGIN_NS(mac::binding)

// What the game's two senders read of the objects Norbyte declares, and what
// his function object must be for the game to call, copy and destroy it.
// (offsetof on a class with virtual functions: conditionally supported, and
// supported by the one compiler this is built with.)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
static_assert(offsetof(ecs::ComponentOps, Callbacks) == kComponentOpsCallbacks);
static_assert(offsetof(ecs::ComponentOps, World) == kComponentOpsWorld);
#pragma clang diagnostic pop
static_assert(offsetof(ecs::ComponentCallbacks, OnConstruct) == kComponentCallbacksAttached);
static_assert(offsetof(ecs::ComponentCallbacks, OnDestroy) == kComponentCallbacksDetached);
static_assert(offsetof(ecs::ComponentSignal, Connections) == kSignalConnections);
static_assert(sizeof(ecs::ComponentSignal::Connection) == kSignalConnectionSize);
static_assert(sizeof(ecs::ComponentSignal::Function) == kSignalConnectionSize - sizeof(uint64_t));
static_assert(offsetof(FunctionStorage<void (GameEntityRef, void*)>, call_) == kFunctionStorageCall);
// The signal's parameters as declared outside MSVC: the entity in two
// registers, then the component.
static_assert(std::is_same_v<ecs::ComponentSignal, Signal<GameEntityRef, void*>>);
// And why EntityRef itself cannot be the parameter.
static_assert(!std::is_trivially_copyable_v<ecs::EntityRef>);
static_assert(sizeof(ecs::EntityRef) == sizeof(GameEntityRef) && offsetof(ecs::EntityRef, Handle) == offsetof(GameEntityRef, Handle)
    && offsetof(ecs::EntityRef, World) == offsetof(GameEntityRef, World));

template <auto Handler, class T>
ecs::ComponentSignal::Function ComponentSignalHandler(T* self, ecs::ComponentTypeIndex type)
{
    struct Target
    {
        T* Self;
        ecs::ComponentTypeIndex Type;
    };
    using Called = void (Target const&, GameEntityRef, void*);

    Called* called = [](Target const& target, GameEntityRef entity, void* component) {
        ecs::EntityRef ref;
        ref.Handle = EntityHandle(entity.Handle);
        ref.World = static_cast<ecs::EntityWorld*>(entity.World);
        (target.Self->*Handler)(target.Type, &ref, component);
    };
    return MakeFunction(called, Target{ self, type });
}

END_NS()
