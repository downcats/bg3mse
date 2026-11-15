#pragma once

#include <cstdint>

// Norbyte's component signals (GameDefinitions/EntitySystem.h,
// CoreLib/Base/BaseFunction.h) and the handler bg3mse subscribes his
// functions through (Mac/Binding/ComponentSignalHandler.h), compiled as his
// sources are, behind plain functions for the component signal tests.

namespace bg3se::mac::test {

// What a handler of Norbyte's signature was last called with, and how often.
struct SignalSeen
{
    uint64_t Handle;
    void* World;
    void* Component;
    uint32_t Type;
    unsigned Calls;
    void const* Entity;     // the address the handler was given the entity at
};

// An ecs::ComponentCallbacks with no subscribers. Never freed.
void* MakeComponentCallbacks();

// Subscribes a handler void (T::*)(ComponentTypeIndex const&, EntityRef*,
// void*) to the attached or the detached signal, as
// EntityComponentEventHooks::AddComponentType subscribes its two. The
// registrant's number.
uint64_t SubscribeHandler(void* callbacks, bool attached, uint16_t type, SignalSeen* seen);
bool Unsubscribe(void* callbacks, bool attached, uint64_t registrant);
uint32_t Subscribers(void* callbacks, bool attached);

// Connects a subscriber of the game's: a function object like any other
// whose call goes to `call`.
uint64_t ConnectGameSubscriber(void* callbacks, bool attached, void (*call)());

// Sends the signal as ImmediateWorldCache::FinalizeAddComponent and
// RemoveComponent do (GameDefinitions/EntitySystem.cpp).
void SendAsNorbyte(void* callbacks, bool attached, uint64_t handle, void* world, void* component);

}
