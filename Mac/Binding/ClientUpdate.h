#pragma once

#include <CoreLib/Wrappers.h>

// For Norbyte's client (Extender/Client/ScriptExtenderClient.cpp), outside
// MSVC: where its two update handlers go.
//
// On Windows they are the pre-hook and the post-hook of one function,
// ecl::GameStateMachine::Update. The macOS game has that function inline in
// App::Update, so the two moments are two places of App::Update and cannot be
// the two ends of one wrapped call: the handlers are registered here instead,
// with the arguments SetPrePostHook takes, and Mac/Binding/HookBridges.cpp
// calls each from its place.

namespace bg3se::mac::binding {

using UpdateHandler = void(void* context, void* self, void const* time);
void SetClientUpdateHandlers(void* context, UpdateHandler* pre, UpdateHandler* post);

template <class TContext, class TTime>
void SetClientUpdateHooks(void (TContext::* pre)(void*, TTime*), void (TContext::* post)(void*, TTime*), TContext* context)
{
    using Helpers = MethodPtrHelpers<TContext, void(void*, TTime*)>;
    SetClientUpdateHandlers(context,
        reinterpret_cast<UpdateHandler*>(Helpers::ToFunction(pre)),
        reinterpret_cast<UpdateHandler*>(Helpers::ToFunction(post)));
}

}
