#include <stdafx.h>

#include <Binding/HookBridges.h>
#include <Binding/Shutdown.h>
#include <Extender/ScriptExtender.h>
#include <Extender/Client/ExtensionStateClient.h>
#include <Extender/Server/ExtensionStateServer.h>
#include <Platform/FaultGuard.h>
#include <Platform/Log.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <thread>
#include <unistd.h>

BEGIN_NS(mac::binding)

namespace {

constinit std::atomic<bool> gShutDown{ false };
constinit std::atomic<bool> gFinished{ false };

// How long the shutdown may take before the game is made to exit anyway.
constexpr unsigned kTimeLimitSeconds = 10;

// What the game does next, when the shutdown has not returned in time: a
// thread it waits for that is not going to end (Shutdown.h).
void WatchTheShutdown()
{
    std::thread([] {
        for (unsigned i = 0; i < kTimeLimitSeconds * 10; i++) {
            if (gFinished.load(std::memory_order_acquire)) return;
            usleep(100000);
        }
        // The thread that is stuck may hold the log's lock.
        LogFromFaultHandler(LogLevel::Error, "Script Extender: its shutdown has not finished in 10 seconds; the game exits now");
        _exit(0);
    }).detach();
}

FaultDecision RecoverAtExit(FaultRecord const&, void*)
{
    return FaultDecision::Recover;
}

// How long the game thread waits for the server's thread to take its part
// at a tick; a server that ticks does so many times a second.
constexpr unsigned kServerWaitMilliseconds = 1000;

// One part of the shutdown, guarded: a fault is recovered and logged, and so
// is a C++ exception. False when it did not finish.
template <class F>
bool GuardedPart(char const* what, F&& part)
{
    bool threw = false;
    FaultRecord fault{};
    bool finished = GuardedCall([&] {
        try {
            part();
        } catch (std::exception const& e) {
            threw = true;
            Log(LogLevel::Error, "Script Extender: %s threw %s", what, e.what());
        } catch (...) {
            threw = true;
            Log(LogLevel::Error, "Script Extender: %s threw an exception", what);
        }
    }, RecoverAtExit, nullptr, &fault);
    if (!finished) {
        Log(LogLevel::Error, "Script Extender: %s took a fault (%s at 0x%llx, pc 0x%llx) and was left there", what,
            FaultKindName(fault.Kind), (unsigned long long)fault.Address, (unsigned long long)fault.PC);
    }
    return finished && !threw;
}

// The player quit (App::Terminate, Binding/HookBridges.h): this is the game
// thread, and the server's thread, the main thread and the workers still run.
// What Norbyte's ScriptExtender::Shutdown does is done where each part may be
// done:
//   - the server's (its extension state reset, so its Lua state gets the
//     Shutdown event and is closed; the story layer ended) on the server's
//     thread, at the start of its next tick, as a Lua reset is
//     (ThreadedExtenderState::EnqueueTask, which esv::ScriptExtender::
//     PreUpdate runs). A server that does not tick (ServerWorker::DoWork
//     waits for an event between its runs of ticks: in the main menu there
//     may be none) is not in its update: after a second this thread takes the
//     part itself, as DllMain's thread does on Windows, and should the server
//     reach its tick meanwhile, the queued task holds it there until the part
//     is done;
//   - the client's on the game thread, which runs its Lua state, unless the
//     quit came from inside that state (then it is only given the Shutdown
//     event), or this is not the game thread (then it is left).
// What runs on other threads is left: the engine hooks and the SDL hooks stay
// in (a thread may be inside one), and the extender object stays, as on
// Windows its destructor runs only once every other thread is gone.
bool ShutdownAtQuit()
{
    bool ok = true;
    auto& server = gExtender->GetServer();
    if (server.HasExtensionState() && server.GetExtensionState().GetLua()) {
        enum : int { kQueued, kServerThread, kThisThread };
        struct Handover
        {
            std::atomic<int> TakenBy{ kQueued };
            std::atomic<bool> Done{ false };
            std::atomic<bool> Ok{ false };
        };
        auto handover = std::make_shared<Handover>();
        auto run = [](Handover& h) {
            bool ok = GuardedPart("the server's shutdown", [] { gExtender->GetServer().Shutdown(); });
            // What tells a play log that the story's wrappers were taken off
            // and its Lua state closed.
            if (ok) Log(LogLevel::Info, "Script Extender: the server is shut down");
            h.Ok.store(ok, std::memory_order_relaxed);
            h.Done.store(true, std::memory_order_release);
        };
        server.EnqueueTask([handover, run] {
            int queued = kQueued;
            if (handover->TakenBy.compare_exchange_strong(queued, kServerThread)) {
                run(*handover);
            } else {
                // The game thread is doing it: the tick goes on once it is done.
                while (!handover->Done.load(std::memory_order_acquire)) usleep(1000);
            }
        });
        auto waitDone = [&](unsigned milliseconds) {
            for (unsigned waited = 0; waited < milliseconds && !handover->Done.load(std::memory_order_acquire); waited += 5) usleep(5000);
            return handover->Done.load(std::memory_order_acquire);
        };
        if (!waitDone(kServerWaitMilliseconds)) {
            int queued = kQueued;
            if (handover->TakenBy.compare_exchange_strong(queued, kThisThread)) {
                Log(LogLevel::Info, "Script Extender: the server did not tick in %u ms; its part of the shutdown is done here", kServerWaitMilliseconds);
                run(*handover);
            } else if (!waitDone(kServerWaitMilliseconds)) {
                Log(LogLevel::Warning, "Script Extender: the server's thread took its shutdown and has not finished it");
            }
        }
        ok = handover->Done.load(std::memory_order_acquire) && handover->Ok.load(std::memory_order_relaxed);
    }

    auto& client = gExtender->GetClient();
    if (!OnGameThread()) {
        // Not the thread App::Terminate was found to be called on: the
        // client's Lua state is the game thread's, which may be running it.
        Log(LogLevel::Warning, "Script Extender: the game quit from a thread other than the game thread; the client's Lua state is left open");
        ok = false;
    } else if (client.HasExtensionState()) {
        ok = GuardedPart("the client's shutdown", [&client] {
            ContextGuardAnyThread context(ContextType::Client);
            auto& state = client.GetExtensionState();
            if (state.GetEnterCount() > 0) {
                // The quit came from inside the client's Lua state (a script's
                // UI command): the state cannot be closed under it, but it
                // is told.
                if (auto lua = state.GetLua()) lua->OnShutdown();
                Log(LogLevel::Warning, "Script Extender: the game quit from inside the client's Lua state; it got the Shutdown event and is left open");
            } else {
                client.ResetExtensionState();
            }
        }) && ok;
    }
    return ok;
}

}

void ShutdownScriptExtender(GameExit how)
{
    if (gShutDown.exchange(true) || !gExtender) return;
    if (how == GameExit::Quit) {
        Log(LogLevel::Info, "Script Extender: the player quit the game; shutting down");
        WatchTheShutdown();
        bool ok = ShutdownAtQuit();
        // What exit() does for the C library's streams, which the game's
        // _Exit skips: the story log's last lines are in a buffer.
        fflush(nullptr);
        gFinished.store(true, std::memory_order_release);
        if (ok) Log(LogLevel::Info, "Script Extender: shut down");
        return;
    }
    Log(LogLevel::Info, "Script Extender: the game is exiting; shutting down");
    WatchTheShutdown();
    // The Lua debugger's thread uses the extender object and is never ended
    // (ScriptExtender::Initialize starts it): the object stays.
    bool keep = gExtender->GetConfig().EnableLuaDebugger;

    bool threw = false;
    FaultRecord fault{};
    bool finished = GuardedCall([&] {
        try {
            // DllMain, DLL_PROCESS_DETACH.
            gExtender->Shutdown();
            if (keep) {
                Log(LogLevel::Info, "Script Extender: the extender object is kept, for the Lua debugger's thread");
            } else {
                gExtender.reset();
            }
        } catch (std::exception const& e) {
            threw = true;
            Log(LogLevel::Error, "Script Extender: its shutdown threw %s", e.what());
        } catch (...) {
            threw = true;
            Log(LogLevel::Error, "Script Extender: its shutdown threw an exception");
        }
    }, RecoverAtExit, nullptr, &fault);

    // What exit() does for the C library's streams, which the game's _exit(0)
    // skips: the story log (COsiris writes it through fopen) has its last
    // lines in a buffer.
    fflush(nullptr);
    gFinished.store(true, std::memory_order_release);
    if (!finished) {
        Log(LogLevel::Error, "Script Extender: its shutdown took a fault (%s at 0x%llx, pc 0x%llx) and was left there; the game exits",
            FaultKindName(fault.Kind), (unsigned long long)fault.Address, (unsigned long long)fault.PC);
    } else if (!threw) {
        Log(LogLevel::Info, "Script Extender: shut down");
    }
}

END_NS()
