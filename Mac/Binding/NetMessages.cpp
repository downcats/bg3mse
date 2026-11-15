#include <Binding/NetMessages.h>

#include <atomic>
#include <cstring>

#include <Engine/EngineResolver.h>
#include <Layout/GameShapes.h>
#include <Platform/Log.h>

namespace bg3se::mac::binding {

namespace {

std::atomic<void const*> gVtable{ nullptr };
std::atomic<size_t> gSlot{ 0 };
std::atomic<void*> gFunction{ nullptr };
std::atomic<bool> gNetworkSession{ false };

// Decides NetworkSessionConfirmed on the game a resolution was made on.
void BindNetworkSession(engine::Resolution const& resolution)
{
    auto game = resolution.In.Game.Symbols;
    auto shapes = game ? CheckGameShapes(*game) : GameShapes{};
    bool confirmed = shapes.Confirmed(GameShape::NetworkSession);
    if (!confirmed) {
        std::string why = game ? "it is not proven" : "the game's code is not readable";
        for (auto const& proof : shapes.Proofs) {
            if (proof.Shape == GameShape::NetworkSession && !proof.Problem.empty()) {
                why = std::string(proof.Function) + ": " + proof.Problem;
                break;
            }
        }
        Log(LogLevel::Warning, "networking is withheld: the game's %s is not confirmed to be laid out as this build declares it (%s). "
            "Network messages of scripts, synchronised user and mod variables and the extender's handshake with a host or clients do not work",
            Describe(GameShape::NetworkSession), why.c_str());
    }
    gNetworkSession.store(confirmed, std::memory_order_release);
}

}

void* SerializeOfMessage(void const* message, void const* vtable, size_t slot, void* function)
{
    if (!message || !vtable || !function) return nullptr;
    void const* held;
    memcpy(&held, message, sizeof(held));
    if (held != vtable) return nullptr;
    void* inSlot;
    memcpy(&inSlot, static_cast<char const*>(vtable) + slot, sizeof(inSlot));
    return inSlot == function ? function : nullptr;
}

void* ClientConnectMessageSerialize(void const* message)
{
    auto function = SerializeOfMessage(message, gVtable.load(std::memory_order_acquire), gSlot.load(std::memory_order_acquire),
        gFunction.load(std::memory_order_acquire));
    if (!function) {
        Log(LogLevel::Warning, "the client's connect message is not wrapped: %s. The extender does not announce itself to a host",
            gFunction.load(std::memory_order_acquire) ? "the factory's message does not have the vtable the engine table resolved"
                                                      : "its function or its vtable is not resolved in this game");
    }
    return function;
}

void SetConnectMessageForTest(void const* vtable, size_t slot, void* function)
{
    gVtable.store(vtable, std::memory_order_release);
    gSlot.store(slot, std::memory_order_release);
    gFunction.store(function, std::memory_order_release);
}

bool NetworkSessionConfirmed()
{
    return gNetworkSession.load(std::memory_order_acquire);
}

void SetNetworkSessionConfirmedForTest(bool confirmed)
{
    gNetworkSession.store(confirmed, std::memory_order_release);
}

void BindNetMessages(engine::Resolution const& resolution)
{
    BindNetworkSession(resolution);
    auto function = resolution.Find(kConnectMessageEntryNames[0]);
    auto vtable = resolution.Find(kConnectMessageEntryNames[1]);
    void* code = function && function->Usable() ? resolution.Address(*function) : nullptr;
    void* table = vtable && vtable->Usable() ? resolution.Address(*vtable) : nullptr;
    // Norbyte's Wrap does not look at what the Detours layer answers: a
    // function that would not take an entry hook is not handed to him.
    bool hookable = function && function->Hook == HookRefusal::None;
    int slot = -1;
    if (vtable) {
        for (auto const& method : vtable->Slots) {
            if (method.Method && method.Method->Function.Symbol && function && function->Of->Where.Symbol
                && strcmp(method.Method->Function.Symbol, function->Of->Where.Symbol) == 0) {
                slot = method.Offset;
            }
        }
    }
    if (!code || !table || !hookable || slot < 0) {
        Log(LogLevel::Warning, "the client's connect message cannot be wrapped: %s%s%s",
            !code ? kConnectMessageEntryNames[0] : !table ? kConnectMessageEntryNames[1] : !hookable ? "an entry hook on the function is refused: " : "the vtable does not hold the function",
            !code || !table ? " is not resolved: " : "",
            !code ? (function ? function->Reason.c_str() : "no such entry") : !table ? (vtable ? vtable->Reason.c_str() : "no such entry")
                  : !hookable ? mac::Describe(function->Hook) : "");
        SetConnectMessageForTest(nullptr, 0, nullptr);
        return;
    }
    SetConnectMessageForTest(table, (size_t)slot, code);
}

}
