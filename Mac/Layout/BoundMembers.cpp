#include <stdafx.h>
#include <Lua/Shared/Proxies/PropertyMapDependencies.h>
#include <Lua/Shared/Proxies/LuaStructIDs.h>
#include <GameDefinitions/Generated/PropertyMapMeta.h>

#include "BoundMembers.h"

#include <array>
#include <type_traits>

BEGIN_SE()

namespace {

template <class T>
struct ElementOf
{
    static constexpr uint32_t Size = 0;
    static constexpr int32_t Class = -1;
    static constexpr bool Scalar = false;
};

template <class T, size_t N>
struct ElementOf<std::array<T, N>>
{
    static constexpr uint32_t Size = (uint32_t)sizeof(T);
    static constexpr int32_t Class = (int32_t)StructID<T>;
    static constexpr bool Scalar = std::is_scalar_v<T>;
};

template <class T>
mac::BoundMember MemberOf(char const* name, size_t offset)
{
    return { name, (uint32_t)offset, (uint32_t)sizeof(T), (int32_t)StructID<T>, ElementOf<T>::Size, ElementOf<T>::Class, ElementOf<T>::Scalar,
        std::is_scalar_v<T> };
}

template <class T>
constexpr char const* EngineClassOf()
{
    if constexpr (IsComponentType<T>) {
        return T::EngineClass;
    } else {
        return nullptr;
    }
}

// Where the `Base` part of a `Derived` begins.
template <class Derived, class Base>
uint32_t BaseOffset()
{
    auto derived = reinterpret_cast<Derived*>(uintptr_t(0x10000));
    return (uint32_t)(reinterpret_cast<uintptr_t>(static_cast<Base*>(derived)) - uintptr_t(0x10000));
}

std::vector<mac::BoundClass> MakeClasses()
{
    std::vector<mac::BoundClass> classes((size_t)StructRegistrySize);

#define BEGIN_CLS(clsName, id) { \
    using TClass = clsName; \
    auto& cls = classes[id]; \
    cls.Name = #clsName; \
    cls.EngineClass = EngineClassOf<TClass>(); \
    cls.Size = (uint32_t)sizeof(TClass);
#define END_CLS() }
#define INHERIT(base) cls.Parent = (int32_t)StructID<base>; cls.ParentOffset = BaseOffset<TClass, base>();
#define P(prop) cls.Members.push_back(MemberOf<decltype(TClass::prop)>(#prop, offsetof(TClass, prop)));
#define P_NOTIFY(prop, notification) P(prop)
#define P_RENAMED(prop, oldName) P(prop)
#define P_RO(prop) P(prop)
#define P_BITMASK(prop) P(prop)
#define P_BITMASK_GETTER_SETTER(prop, getter, setter) P(prop)
#define PN(name, prop) P(prop)
#define PN_RO(name, prop) P(prop)
#define P_GETTER(prop, fun)
#define P_FREE_GETTER(prop, fun)
#define P_GETTER_SETTER(prop, getter, setter)
#define P_STAND_IN(prop, getter, setter)
#define P_STAND_IN_GETTER(prop, fun)
#define P_FUN(prop, fun)
#define P_FALLBACK(getter, setter, next)

#include <GameDefinitions/Generated/PropertyMaps.inl>

#undef BEGIN_CLS
#undef END_CLS
#undef INHERIT
#undef P
#undef P_NOTIFY
#undef P_RENAMED
#undef P_RO
#undef P_BITMASK
#undef P_BITMASK_GETTER_SETTER
#undef PN
#undef PN_RO
#undef P_GETTER
#undef P_FREE_GETTER
#undef P_GETTER_SETTER
#undef P_STAND_IN
#undef P_STAND_IN_GETTER
#undef P_FUN
#undef P_FALLBACK

    return classes;
}

}

std::span<mac::BoundClass const> mac::BoundClasses()
{
    static const std::vector<mac::BoundClass> classes = MakeClasses();
    return classes;
}

END_SE()
