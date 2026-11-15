#include <stdafx.h>
#include <GameDefinitions/EntitySystem.h>

#include <Mac/Binding/ComponentSignalHandler.h>

#include "ComponentSignalsNorbyte.h"

// What his array of connections takes from the extender, which the tests do
// not link: the game's allocator, here the C library's, and the function his
// assertions fail through.
BEGIN_SE()

void* GameAllocRaw(std::size_t size)
{
    return malloc(size);
}

void GameFree(void* ptr) noexcept
{
    free(ptr);
}

void AssertionFailed(char const* expr)
{
    fprintf(stderr, "assertion of Norbyte's code failed: %s\n", expr);
    abort();
}

END_SE()

BEGIN_NS(mac::test)

namespace {

struct Subscriber
{
    SignalSeen* Seen;

    void OnSignal(ecs::ComponentTypeIndex const& type, ecs::EntityRef* entity, void* component)
    {
        Seen->Handle = entity->Handle.Handle;
        Seen->World = entity->World;
        Seen->Component = component;
        Seen->Type = (uint32_t)type;
        Seen->Calls++;
        Seen->Entity = entity;
    }
};

ecs::ComponentSignal& SignalOf(void* callbacks, bool attached)
{
    auto object = static_cast<ecs::ComponentCallbacks*>(callbacks);
    return attached ? object->OnConstruct : object->OnDestroy;
}

void NobodysSubscriber(mac::binding::GameEntityRef, void*) {}

}

void* MakeComponentCallbacks()
{
    // The game makes these; his declaration has no constructor to call.
    auto object = static_cast<ecs::ComponentCallbacks*>(calloc(1, sizeof(ecs::ComponentCallbacks)));
    new (&object->OnConstruct) ecs::ComponentSignal();
    new (&object->OnDestroy) ecs::ComponentSignal();
    return object;
}

uint64_t SubscribeHandler(void* callbacks, bool attached, uint16_t type, SignalSeen* seen)
{
    auto subscriber = new Subscriber{ seen };
    return SignalOf(callbacks, attached).Add(
        mac::binding::ComponentSignalHandler<&Subscriber::OnSignal>(subscriber, (ecs::ComponentTypeIndex)type));
}

bool Unsubscribe(void* callbacks, bool attached, uint64_t registrant)
{
    return SignalOf(callbacks, attached).Remove(registrant);
}

uint32_t Subscribers(void* callbacks, bool attached)
{
    return SignalOf(callbacks, attached).Connections.size();
}

uint64_t ConnectGameSubscriber(void* callbacks, bool attached, void (*call)())
{
    auto& signal = SignalOf(callbacks, attached);
    auto registrant = signal.Add(MakeFunction(&NobodysSubscriber));
    // The first of the three methods at the start of the function's storage.
    auto& connection = signal.Connections[signal.Connections.size() - 1];
    memcpy(reinterpret_cast<char*>(&connection.Handler) + mac::binding::kFunctionStorage + mac::binding::kFunctionStorageCall,
        &call, sizeof(call));
    return registrant;
}

void SendAsNorbyte(void* callbacks, bool attached, uint64_t handle, void* world, void* component)
{
    ecs::EntityRef e{
        .Handle = EntityHandle(handle),
        .World = static_cast<ecs::EntityWorld*>(world)
    };
    SignalOf(callbacks, attached).Invoke(mac::binding::GameEntityRef{ e.Handle.Handle, e.World }, component);
}

END_NS()
