#include <stdafx.h>
#include <Extender/Shared/Hooks.h>
#include <Extender/ScriptExtender.h>
#include <Extender/Shared/ExtensionState.h>
#include <GameDefinitions/Net.h>
#include <GameDefinitions/Stats/Cache.h>
#if !defined(_MSC_VER)
#include <Mac/Binding/NetMessages.h>
#endif

BEGIN_SE()

#if defined(_MSC_VER)
decltype(Hooks::eocnet__ClientConnectMessage__Serialize)* decltype(Hooks::eocnet__ClientConnectMessage__Serialize)::gHook;
#endif

void Hooks::Startup()
{
    if (loaded_) {
        return;
    }

    auto& lib = gExtender->GetEngineHooks();
    lib.RPGStats__ParseDataBuffers.SetPreHook(&Hooks::OnParseDataBuffers, this);
    eocnet__ClientConnectMessage__Serialize.SetWrapper(&Hooks::OnClientConnectMessage, this);
    
    loaded_ = true;
}

void Hooks::HookNetworkMessages(net::MessageFactory* factory)
{
    if (networkingInitialized_) {
        return;
    }

    if (factory->MessagePools.size() <= (unsigned)NetMessage::NETMSG_CLIENT_CONNECT) {
        ERR("MessageFactory not initialized yet");
        return;
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    auto clientConnect = factory->MessagePools[(unsigned)NetMessage::NETMSG_CLIENT_CONNECT]->Template;
#if defined(_MSC_VER)
    eocnet__ClientConnectMessage__Serialize.Wrap((*(net::Message::VMT**)clientConnect)->Serialize);
#else
    // The macOS game's vtables begin with two destructors, so the second
    // slot is not this function: it is taken from the engine table, and only
    // when this message's vtable holds it (Mac/Binding/NetMessages.h).
    if (auto serialize = mac::binding::ClientConnectMessageSerialize(clientConnect)) {
        eocnet__ClientConnectMessage__Serialize.Wrap(serialize);
    }
#endif

    DetourTransactionCommit();

    networkingInitialized_ = true;
}

void Hooks::OnParseDataBuffers(stats::RPGStats* self)
{
    stats::gStatStructureCache.Invalidate();

    gExtender->GetCurrentExtensionState()->OnStatsStructureLoaded();

    gExtender->GetStatLoadOrderHelper().OnLoadFinished();
}

void Hooks::OnClientConnectMessage(net::Message::SerializeProc* wrapped, net::Message* msg, net::BitstreamSerializer* serializer)
{
    auto m = (net::ClientConnectMessage*)msg;
    if (serializer->IsWriting) {
        gExtender->GetClient().GetNetworkManager().OnClientConnectMessage(m);
    }

    wrapped(msg, serializer);
}

END_SE()
