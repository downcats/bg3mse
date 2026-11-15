#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

// ls::Variant<Ts...> as the Mac game lays it out: storage as large as the
// largest alternative and aligned as the most aligned one, and directly
// after it ONE byte holding the zero-based position of the active
// alternative. The variant is aligned as its storage and its size is the
// storage plus that byte, rounded up to the alignment. The game reads and
// writes the index as a byte only.
//
// The storage is not a union of the alternatives: a union would be padded to
// its alignment before the index. ls::Variant<entity handle, Vector3f> has
// its index at +12, after the 12-byte vector, and is 16 bytes; a union of
// the two is 16 bytes by itself.
//
// libc++'s std::variant keeps a union and a four-byte index, so it cannot
// stand for the game's type (MSVC's has a one-byte index after a union, which
// fits the game wherever the largest alternative fills the union). GameVariant
// offers what Norbyte's code uses of std::variant, with std::variant's
// meaning: construction and assignment from an alternative chosen as
// std::variant chooses it, index, emplace, get, get_if, holds_alternative,
// visit, variant_size and variant_alternative. Like std::variant it is
// trivially copyable and destructible when its alternatives are.
//
// Every std name below is written ::std: inside namespace bg3se a plain
// "std" is the namespace of Layout/StdVariant.h.

namespace bg3se::mac {

template <class... Ts>
class GameVariant;

namespace variant_detail {

inline constexpr uint8_t kValueless = 0xff;

template <size_t I, class... Ts>
using TypeAt = ::std::tuple_element_t<I, ::std::tuple<Ts...>>;

template <class T, class... Ts>
inline constexpr size_t CountOf = (size_t(::std::is_same_v<T, Ts>) + ... + 0);

template <class T, class... Ts>
constexpr size_t IndexOf()
{
    size_t index = 0, at = 0;
    ((::std::is_same_v<T, Ts> ? (void)(index = at) : (void)0, at++), ...);
    return index;
}

// The alternative a value of type U selects, by ::std::variant's rule: an
// imaginary function F(Ti) for every alternative Ti for which
// `Ti x[] = { ::std::forward<U>(u) };` is well-formed (which leaves out
// narrowing conversions), bool only from bool; the best match wins.
template <class T>
struct Array1
{
    T Value[1];
};

template <class T, size_t I>
struct Candidate
{
    template <class U>
        requires (!::std::is_same_v<::std::remove_cv_t<T>, bool> || ::std::is_same_v<::std::remove_cvref_t<U>, bool>)
            && requires (U&& u) { Array1<T>{ { ::std::forward<U>(u) } }; }
    ::std::integral_constant<size_t, I> operator()(T, U&&) const;
};

template <class... Cs>
struct Candidates : Cs...
{
    using Cs::operator()...;
};

template <class Seq, class... Ts>
struct CandidatesFor;
template <size_t... Is, class... Ts>
struct CandidatesFor<::std::index_sequence<Is...>, Ts...>
{
    using type = Candidates<Candidate<Ts, Is>...>;
};

template <class U, class... Ts>
using SelectedIndex = decltype(::std::declval<typename CandidatesFor<::std::index_sequence_for<Ts...>, Ts...>::type>()(
    ::std::declval<U>(), ::std::declval<U>()));

template <class U, class... Ts>
concept Selects = requires { typename SelectedIndex<U, Ts...>; };

template <class T>
inline constexpr bool IsInPlace = false;
template <class T>
inline constexpr bool IsInPlace<::std::in_place_type_t<T>> = true;
template <size_t I>
inline constexpr bool IsInPlace<::std::in_place_index_t<I>> = true;

}

template <class... Ts>
class GameVariant
{
    static_assert(sizeof...(Ts) > 0 && sizeof...(Ts) < variant_detail::kValueless);
    static_assert((::std::is_object_v<Ts> && ...) && (!::std::is_array_v<Ts> && ...));

public:
    static constexpr size_t Alignment = ::std::max({ alignof(Ts)... });
    static constexpr size_t StorageSize = ::std::max({ sizeof(Ts)... });
    static constexpr size_t IndexOffset = StorageSize;

private:
    template <size_t I>
    using Alt = variant_detail::TypeAt<I, Ts...>;
    using First = Alt<0>;

    static constexpr bool kTrivialDestroy = (::std::is_trivially_destructible_v<Ts> && ...);
    static constexpr bool kTrivialCopyConstruct = (::std::is_trivially_copy_constructible_v<Ts> && ...);
    static constexpr bool kTrivialMoveConstruct = (::std::is_trivially_move_constructible_v<Ts> && ...);
    static constexpr bool kTrivialCopyAssign = kTrivialDestroy && kTrivialCopyConstruct && (::std::is_trivially_copy_assignable_v<Ts> && ...);
    static constexpr bool kTrivialMoveAssign = kTrivialDestroy && kTrivialMoveConstruct && (::std::is_trivially_move_assignable_v<Ts> && ...);

public:
    GameVariant() noexcept(::std::is_nothrow_default_constructible_v<First>)
        requires ::std::is_default_constructible_v<First>
    {
        Construct<0>();
    }

    GameVariant(GameVariant const&) requires kTrivialCopyConstruct = default;
    GameVariant(GameVariant const& other)
        requires (!kTrivialCopyConstruct && (::std::is_copy_constructible_v<Ts> && ...))
    {
        index_ = variant_detail::kValueless;
        other.Each([&]<size_t I>(Alt<I> const& value) { Construct<I>(value); });
    }

    GameVariant(GameVariant&&) requires kTrivialMoveConstruct = default;
    GameVariant(GameVariant&& other) noexcept((::std::is_nothrow_move_constructible_v<Ts> && ...))
        requires (!kTrivialMoveConstruct && (::std::is_move_constructible_v<Ts> && ...))
    {
        index_ = variant_detail::kValueless;
        other.Each([&]<size_t I>(Alt<I>& value) { Construct<I>(::std::move(value)); });
    }

    template <class U>
        requires (!::std::is_same_v<::std::remove_cvref_t<U>, GameVariant> && !variant_detail::IsInPlace<::std::remove_cvref_t<U>>
            && variant_detail::Selects<U, Ts...>
            && ::std::is_constructible_v<Alt<variant_detail::SelectedIndex<U, Ts...>::value>, U>)
    GameVariant(U&& value)
    {
        Construct<variant_detail::SelectedIndex<U, Ts...>::value>(::std::forward<U>(value));
    }

    template <class T, class... Args>
        requires (variant_detail::CountOf<T, Ts...> == 1 && ::std::is_constructible_v<T, Args...>)
    explicit GameVariant(::std::in_place_type_t<T>, Args&&... args)
    {
        Construct<variant_detail::IndexOf<T, Ts...>()>(::std::forward<Args>(args)...);
    }

    template <size_t I, class... Args>
        requires (I < sizeof...(Ts) && ::std::is_constructible_v<Alt<I>, Args...>)
    explicit GameVariant(::std::in_place_index_t<I>, Args&&... args)
    {
        Construct<I>(::std::forward<Args>(args)...);
    }

    ~GameVariant() requires kTrivialDestroy = default;
    ~GameVariant() requires (!kTrivialDestroy) { Destroy(); }

    GameVariant& operator = (GameVariant const&) requires kTrivialCopyAssign = default;
    GameVariant& operator = (GameVariant const& other)
        requires (!kTrivialCopyAssign && (::std::is_copy_constructible_v<Ts> && ...) && (::std::is_copy_assignable_v<Ts> && ...))
    {
        if (this == &other) return *this;
        if (other.valueless_by_exception()) {
            Destroy();
        } else if (index_ == other.index_) {
            other.Each([&]<size_t I>(Alt<I> const& value) { Ref<I>() = value; });
        } else {
            // A copy that throws must not cost the old value.
            GameVariant copy(other);
            Destroy();
            copy.Each([&]<size_t I>(Alt<I>& value) { Construct<I>(::std::move(value)); });
        }
        return *this;
    }

    GameVariant& operator = (GameVariant&&) requires kTrivialMoveAssign = default;
    GameVariant& operator = (GameVariant&& other)
        noexcept((::std::is_nothrow_move_constructible_v<Ts> && ...) && (::std::is_nothrow_move_assignable_v<Ts> && ...))
        requires (!kTrivialMoveAssign && (::std::is_move_constructible_v<Ts> && ...) && (::std::is_move_assignable_v<Ts> && ...))
    {
        if (this == &other) return *this;
        if (other.valueless_by_exception()) {
            Destroy();
        } else if (index_ == other.index_) {
            other.Each([&]<size_t I>(Alt<I>& value) { Ref<I>() = ::std::move(value); });
        } else {
            Destroy();
            other.Each([&]<size_t I>(Alt<I>& value) { Construct<I>(::std::move(value)); });
        }
        return *this;
    }

    template <class U>
        requires (!::std::is_same_v<::std::remove_cvref_t<U>, GameVariant> && variant_detail::Selects<U, Ts...>
            && ::std::is_constructible_v<Alt<variant_detail::SelectedIndex<U, Ts...>::value>, U>
            && ::std::is_assignable_v<Alt<variant_detail::SelectedIndex<U, Ts...>::value>&, U>)
    GameVariant& operator = (U&& value)
    {
        constexpr size_t I = variant_detail::SelectedIndex<U, Ts...>::value;
        if (index_ == I) {
            Ref<I>() = ::std::forward<U>(value);
        } else if (::std::is_nothrow_constructible_v<Alt<I>, U> || !::std::is_nothrow_move_constructible_v<Alt<I>>) {
            Destroy();
            Construct<I>(::std::forward<U>(value));
        } else {
            Alt<I> made(::std::forward<U>(value));
            Destroy();
            Construct<I>(::std::move(made));
        }
        return *this;
    }

    constexpr size_t index() const noexcept { return index_ == variant_detail::kValueless ? ::std::variant_npos : index_; }
    constexpr bool valueless_by_exception() const noexcept { return index_ == variant_detail::kValueless; }

    template <class T, class... Args>
        requires (variant_detail::CountOf<T, Ts...> == 1 && ::std::is_constructible_v<T, Args...>)
    T& emplace(Args&&... args)
    {
        Destroy();
        return Construct<variant_detail::IndexOf<T, Ts...>()>(::std::forward<Args>(args)...);
    }

    template <size_t I, class... Args>
        requires (I < sizeof...(Ts) && ::std::is_constructible_v<Alt<I>, Args...>)
    Alt<I>& emplace(Args&&... args)
    {
        Destroy();
        return Construct<I>(::std::forward<Args>(args)...);
    }

    // The alternative at position I, which must be the active one.
    template <size_t I>
    Alt<I>& Ref() noexcept { return *::std::launder(reinterpret_cast<Alt<I>*>(storage_)); }
    template <size_t I>
    Alt<I> const& Ref() const noexcept { return *::std::launder(reinterpret_cast<Alt<I> const*>(storage_)); }

private:
    template <size_t I, class... Args>
    Alt<I>& Construct(Args&&... args)
    {
        // Valueless until the constructor has returned.
        index_ = variant_detail::kValueless;
        auto made = ::new (static_cast<void*>(storage_)) Alt<I>(::std::forward<Args>(args)...);
        index_ = (uint8_t)I;
        return *made;
    }

    void Destroy() noexcept
    {
        if constexpr (!kTrivialDestroy) {
            Each([]<size_t I>(Alt<I>& value) { ::std::destroy_at(&value); });
        }
        index_ = variant_detail::kValueless;
    }

    // Calls f<I>(alternative I) for the active alternative; nothing when valueless.
    template <class F>
    void Each(F&& f)
    {
        [&]<size_t... Is>(::std::index_sequence<Is...>) {
            (void)((index_ == Is ? (f.template operator()<Is>(Ref<Is>()), true) : false) || ...);
        }(::std::index_sequence_for<Ts...>{});
    }
    template <class F>
    void Each(F&& f) const
    {
        [&]<size_t... Is>(::std::index_sequence<Is...>) {
            (void)((index_ == Is ? (f.template operator()<Is>(Ref<Is>()), true) : false) || ...);
        }(::std::index_sequence_for<Ts...>{});
    }

    alignas(Alignment) unsigned char storage_[StorageSize];
    uint8_t index_;
};

// --- What std offers for ::std::variant, for GameVariant -----------------------

template <class V>
struct variant_size;
template <class... Ts>
struct variant_size<GameVariant<Ts...>> : ::std::integral_constant<size_t, sizeof...(Ts)> {};
template <class V>
struct variant_size<V const> : variant_size<V> {};
template <class V>
inline constexpr size_t variant_size_v = variant_size<V>::value;

template <size_t I, class V>
struct variant_alternative;
template <size_t I, class... Ts>
struct variant_alternative<I, GameVariant<Ts...>>
{
    using type = variant_detail::TypeAt<I, Ts...>;
};
template <size_t I, class V>
struct variant_alternative<I, V const>
{
    using type = ::std::add_const_t<typename variant_alternative<I, V>::type>;
};
template <size_t I, class V>
using variant_alternative_t = typename variant_alternative<I, V>::type;

template <class T, class... Ts>
    requires (variant_detail::CountOf<T, Ts...> == 1)
constexpr bool holds_alternative(GameVariant<Ts...> const& v) noexcept
{
    return v.index() == variant_detail::IndexOf<T, Ts...>();
}

template <size_t I, class... Ts>
    requires (I < sizeof...(Ts))
variant_detail::TypeAt<I, Ts...>& get(GameVariant<Ts...>& v)
{
    if (v.index() != I) throw ::std::bad_variant_access();
    return v.template Ref<I>();
}
template <size_t I, class... Ts>
    requires (I < sizeof...(Ts))
variant_detail::TypeAt<I, Ts...> const& get(GameVariant<Ts...> const& v)
{
    if (v.index() != I) throw ::std::bad_variant_access();
    return v.template Ref<I>();
}
template <size_t I, class... Ts>
    requires (I < sizeof...(Ts))
variant_detail::TypeAt<I, Ts...>&& get(GameVariant<Ts...>&& v)
{
    if (v.index() != I) throw ::std::bad_variant_access();
    return ::std::move(v.template Ref<I>());
}
template <size_t I, class... Ts>
    requires (I < sizeof...(Ts))
variant_detail::TypeAt<I, Ts...> const&& get(GameVariant<Ts...> const&& v)
{
    if (v.index() != I) throw ::std::bad_variant_access();
    return ::std::move(v.template Ref<I>());
}

template <class T, class... Ts>
    requires (variant_detail::CountOf<T, Ts...> == 1)
T& get(GameVariant<Ts...>& v) { return get<variant_detail::IndexOf<T, Ts...>()>(v); }
template <class T, class... Ts>
    requires (variant_detail::CountOf<T, Ts...> == 1)
T const& get(GameVariant<Ts...> const& v) { return get<variant_detail::IndexOf<T, Ts...>()>(v); }
template <class T, class... Ts>
    requires (variant_detail::CountOf<T, Ts...> == 1)
T&& get(GameVariant<Ts...>&& v) { return get<variant_detail::IndexOf<T, Ts...>()>(::std::move(v)); }
template <class T, class... Ts>
    requires (variant_detail::CountOf<T, Ts...> == 1)
T const&& get(GameVariant<Ts...> const&& v) { return get<variant_detail::IndexOf<T, Ts...>()>(::std::move(v)); }

template <size_t I, class... Ts>
    requires (I < sizeof...(Ts))
variant_detail::TypeAt<I, Ts...>* get_if(GameVariant<Ts...>* v) noexcept
{
    return v && v->index() == I ? &v->template Ref<I>() : nullptr;
}
template <size_t I, class... Ts>
    requires (I < sizeof...(Ts))
variant_detail::TypeAt<I, Ts...> const* get_if(GameVariant<Ts...> const* v) noexcept
{
    return v && v->index() == I ? &v->template Ref<I>() : nullptr;
}
template <class T, class... Ts>
    requires (variant_detail::CountOf<T, Ts...> == 1)
T* get_if(GameVariant<Ts...>* v) noexcept { return get_if<variant_detail::IndexOf<T, Ts...>()>(v); }
template <class T, class... Ts>
    requires (variant_detail::CountOf<T, Ts...> == 1)
T const* get_if(GameVariant<Ts...> const* v) noexcept { return get_if<variant_detail::IndexOf<T, Ts...>()>(v); }

namespace variant_detail {

template <class F, class V, size_t... Is>
decltype(auto) Visit(F&& f, V&& v, ::std::index_sequence<Is...>)
{
    using R = ::std::invoke_result_t<F, decltype(get<0>(::std::forward<V>(v)))>;
    static_assert((::std::is_same_v<R, ::std::invoke_result_t<F, decltype(get<Is>(::std::forward<V>(v)))>> && ...),
        "the visitor must return the same type for every alternative");
    using Call = R (*)(F&&, V&&);
    static constexpr Call kCalls[] = {
        [](F&& f, V&& v) -> R {
            return ::std::invoke(::std::forward<F>(f), static_cast<decltype(get<Is>(::std::forward<V>(v)))>(v.template Ref<Is>()));
        }...
    };
    size_t index = v.index();
    if (index >= sizeof...(Is)) throw ::std::bad_variant_access();
    return kCalls[index](::std::forward<F>(f), ::std::forward<V>(v));
}

}

template <class F, class... Ts>
decltype(auto) visit(F&& f, GameVariant<Ts...>& v)
{
    return variant_detail::Visit(::std::forward<F>(f), v, ::std::index_sequence_for<Ts...>{});
}
template <class F, class... Ts>
decltype(auto) visit(F&& f, GameVariant<Ts...> const& v)
{
    return variant_detail::Visit(::std::forward<F>(f), v, ::std::index_sequence_for<Ts...>{});
}
template <class F, class... Ts>
decltype(auto) visit(F&& f, GameVariant<Ts...>&& v)
{
    return variant_detail::Visit(::std::forward<F>(f), ::std::move(v), ::std::index_sequence_for<Ts...>{});
}

}
