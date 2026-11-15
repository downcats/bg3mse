#include <Extender/Shared/API.h>
#if !defined(_MSC_VER)
#include <Mac/Platform/Process.h>
#endif

BEGIN_SE()

void APIHelpers::ScheduleCallbackOnThread(bg3api::APIContext context, bg3api::ThreadedCallbackProc* callback, void* userData)
{
    activated_ = true;
    if (context == bg3api::APIContext::Server) {
        gExtender->GetServer().EnqueueTask([=] () {
            callback(userData);
        });
    } else {
        se_assert(context == bg3api::APIContext::Client);
        gExtender->GetClient().EnqueueTask([=] () {
            callback(userData);
        });
    }
}

void APIHelpers::AddStateChangeListener(bg3api::LuaStateChangeCallbackProc* callback, void* userData)
{
    activated_ = true;
    stateChangeCallbacks_.push_back(APIStateChangeCallback{
        .Callback = callback,
        .UserData = userData
    });
}

void APIHelpers::OnLuaStateEvent(ContextType context, bg3api::APILuaStateEvent stateEvent, lua_State* L)
{
    auto ctx = context == ContextType::Server ? bg3api::APIContext::Server : bg3api::APIContext::Client;

    for (auto callback : stateChangeCallbacks_) {
        callback.Callback(ctx, stateEvent, L, callback.UserData);
    }
}

ExtensionStateBase* APIHelpers::GetExtensionState(bg3api::APIContext context)
{
    if (context == bg3api::APIContext::Server) {
        if (gExtender->GetServer().HasExtensionState()) {
            return &gExtender->GetServer().GetExtensionState();
        }
    } else {
        se_assert(context == bg3api::APIContext::Client);
        if (gExtender->GetClient().HasExtensionState()) {
            return &gExtender->GetClient().GetExtensionState();
        }
    }

    return nullptr;
}

lua_State* APIHelpers::EnterVM(bg3api::APIContext context)
{
    auto ctx = (context == bg3api::APIContext::Server) ? ContextType::Server : ContextType::Client;
    if (ctx != GetCurrentContextType()) {
        ERR("Attempted to enter the Lua VM from an incorrect thread");
        return nullptr;
    }

    activated_ = true;
    auto state = GetExtensionState(context);

    if (state) {
        state->IncLuaRefs();
        auto lua = state->GetLuaIfReachable();
        if (lua) {
            state->EnteredCheck();
            return lua->GetState();
        } else {
            state->DecLuaRefs(true);
        }
    }

    return nullptr;
}

void APIHelpers::ExitVM(lua_State* state)
{
    if (!state) return;

    auto& extensionState = lua::State::FromLua(state)->GetExtensionState();

    if (extensionState.GetContextType() != GetCurrentContextType()
        || extensionState.GetOwningThread() != GetCurrentThreadId()) {
        ERR("Attempted to release the Lua VM from an incorrect thread");
        return;
    }

    extensionState.EnteredCheck();
    extensionState.DecLuaRefs(true);
}

END_SE()

#if !defined(_MSC_VER)
// macOS: there is no module definition file. The functions Exports.def names
// are exported by their visibility (the build's default is hidden).
#pragma GCC visibility push(default)
#endif

extern "C"
{
    // Updater compatibility shims in case SE is loaded directly, rather than through the updater
#if !defined(_MSC_VER)
    // macOS: what a native module is handed as the extender's module is what
    // it gives dlsym, as on Windows it gives an HMODULE to GetProcAddress: a
    // dlopen handle of this image, not the address of its header.
    static HMODULE ThisNativeModule()
    {
        return reinterpret_cast<HMODULE>(bg3se::mac::NativeModuleHandle(reinterpret_cast<void const*>(&ThisNativeModule)));
    }
#endif

    HMODULE SEUpdaterGetModule()
    {
#if defined(_MSC_VER)
        return bg3se::gCoreLibPlatformInterface.ThisModule;
#else
        return ThisNativeModule();
#endif
    }

    void SEUpdaterNotifyWhenLoaded(bg3api::LoadCompletionCallbackProc* callback, void* userData)
    {
#if defined(_MSC_VER)
        callback(bg3se::gCoreLibPlatformInterface.ThisModule, userData);
#else
        callback(ThisNativeModule(), userData);
#endif
    }

    bg3api::APIVersion SEGetAPIVersion()
    {
        return bg3api::APIVersion::VerCurrent;
    }

    void SERegisterLuaStateChangeCallback(bg3api::LuaStateChangeCallbackProc* callback, void* userData)
    {
#if !defined(_MSC_VER)
        // macOS: there is no extender object in a process the start-up left
        // alone (a game it does not support): no Lua state is ever made there.
        if (!bg3se::gExtender) return;
#endif
        bg3se::gExtender->GetExternalAPI().AddStateChangeListener(callback, userData);
    }

    void SEScheduleCallbackOnThread(bg3api::APIContext context, bg3api::ThreadedCallbackProc* callback, void* userData)
    {
#if !defined(_MSC_VER)
        // macOS: without an extender object neither thread runs its tasks.
        if (!bg3se::gExtender) return;
#endif
        bg3se::gExtender->GetExternalAPI().ScheduleCallbackOnThread(context, callback, userData);
    }

    lua_State* SEEnterLuaVM(bg3api::APIContext context)
    {
#if !defined(_MSC_VER)
        // macOS: without an extender object there is no Lua state to enter.
        if (!bg3se::gExtender) return nullptr;
#endif
        return bg3se::gExtender->GetExternalAPI().EnterVM(context);
    }

    void SEExitLuaVM(lua_State* state)
    {
#if !defined(_MSC_VER)
        // macOS: nothing was entered without an extender object.
        if (!bg3se::gExtender) return;
#endif
        bg3se::gExtender->GetExternalAPI().ExitVM(state);
    }
}

#if !defined(_MSC_VER)
#pragma GCC visibility pop
#endif
