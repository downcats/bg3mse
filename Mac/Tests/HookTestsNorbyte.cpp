#include <stdafx.h>
#include <GameDefinitions/Symbols.h>
#include <GameHooks/OsirisWrappers.h>

#include <cstdarg>

#include "HookTestsNorbyte.h"

// What his hooks take from the extender, which the tests do not link: the
// log his errors go to, the set of his handlers the crash reporter knows, and
// the address of a function as the detour layer sees it.
BEGIN_SE()

void Debug(DebugMessageType, char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fputc('\n', stdout);
}

std::unordered_set<void const*> gRegisteredTrampolines;

void const* ResolveRealFunctionAddress(void const* ptr)
{
    return ptr;
}

END_SE()

BEGIN_NS(mac::test)

namespace {

// A parameter as the handler holds it, in the format of HookProbe::Seen.
template <class T>
uint64_t Recorded(std::add_lvalue_reference_t<std::add_const_t<std::remove_reference_t<T>>> value)
{
    if constexpr (std::is_reference_v<T>) {
        return reinterpret_cast<uint64_t>(&value);
    } else {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(uint64_t), "a parameter of a kind no hook here passes");
        uint64_t bits = 0;
        memcpy(&bits, &value, sizeof(T));
        return bits;
    }
}

template <int N, class F> struct Probe;

template <int N, class R, class... P>
struct Probe<N, R(P...)>
{
    struct Tag {};
    using Hook = WrappableFunction<Tag, R(P...)>;

    static inline Hook hook;
    static inline uint64_t seen[sizeof...(P) > 0 ? sizeof...(P) : 1];
    static inline unsigned calls;

    // A handler of the shape of Norbyte's: R (Context::*)(R (*next)(P...), P...).
    struct Context
    {
        R Handle(R (*next)(P...), P... params)
        {
            calls++;
            unsigned i = 0;
            ((seen[i++] = Recorded<P>(params)), ...);
            return next(params...);
        }
    };
    static inline Context context;

    static bool Attach(void* function)
    {
        calls = 0;
        memset(seen, 0, sizeof(seen));
        // As his own hooks go on (Extender/Shared/Hooks.cpp): in a transaction.
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        hook.Wrap(function);
        if (DetourTransactionCommit() != NO_ERROR || !hook.IsWrapped()) return false;
        hook.SetWrapper(&Context::Handle, &context);
        return true;
    }

    static void Detach()
    {
        hook.ClearHook();
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        hook.Unwrap();
        DetourTransactionCommit();
    }

    static HookProbe Make(char const* entry)
    {
        return { entry, static_cast<unsigned>(sizeof...(P)), &Attach, &Detach, seen, &calls };
    }
};

template <class T> struct WrappedType;
template <class Tag, class F> struct WrappedType<WrappableFunction<Tag, F>> { using Type = F; };

#define STATIC_PROBE(member) Probe<__COUNTER__, std::remove_pointer_t<decltype(StaticSymbols::member)>>::Make(#member)
#define STORY_PROBE(member) Probe<__COUNTER__, WrappedType<decltype(OsirisWrappers::member)>::Type>::Make("Osiris." #member)

}

std::span<HookProbe const> HookProbes()
{
    static HookProbe const probes[] = {
        // The hooks of GameHooks/EngineHooks.inl that go on the game's own
        // function (Mac/Binding/StaticSymbolRows.cpp, HOOKED and the
        // interrupt executor), less the game state hooks.
        STATIC_PROBE(ecs__EntityWorld__Update),
        STATIC_PROBE(ecs__EntityWorld__FlushECBs),
        STATIC_PROBE(esv__OsirisVariableHelper__SavegameVisit),
        STATIC_PROBE(stats__DealDamageFunctor__ApplyDamage),
        STATIC_PROBE(esv__StatsSystem__ThrowDamageEvent),
        STATIC_PROBE(esv__ExecuteStatsFunctor_AttackTargetContext),
        STATIC_PROBE(esv__ExecuteStatsFunctor_AttackPositionContext),
        STATIC_PROBE(esv__ExecuteStatsFunctor_MoveContext),
        STATIC_PROBE(esv__ExecuteStatsFunctor_TargetContext),
        STATIC_PROBE(esv__ExecuteStatsFunctor_NearbyAttackedContext),
        STATIC_PROBE(esv__ExecuteStatsFunctor_NearbyAttackingContext),
        STATIC_PROBE(esv__ExecuteStatsFunctor_EquipContext),
        STATIC_PROBE(esv__ExecuteStatsFunctor_SourceContext),
        STATIC_PROBE(esv__ExecuteStatsFunctor_InterruptContext),
        STATIC_PROBE(eoc__AiGrid__FindPath),
        // The story library's (GameHooks/OsirisWrappers.cpp) that go on the
        // library's own function (Mac/Binding/OsirisRows.cpp, OSI_HOOKED).
        STORY_PROBE(RegisterDivFunctions),
        STORY_PROBE(InitGame),
        STORY_PROBE(DeleteAllData),
        STORY_PROBE(GetFunctionMappings),
        STORY_PROBE(CloseLogFile),
        STORY_PROBE(Load),
        STORY_PROBE(Compile),
        STORY_PROBE(Merge),
        STORY_PROBE(Event),
        STORY_PROBE(RuleActionCall),
    };
    return probes;
}

END_NS()
