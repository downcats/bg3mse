#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>

#include <Engine/EngineTable.h>

// Where a C++ function type's parameters and result travel on arm64 macOS,
// worked out by the compiler from the type itself, in the terms the engine
// table states a game function's argument passing in (Engine/EngineTable.h).
//
// It is what ties a prototype to a table entry: Norbyte's prototype for a
// function the binding hands over unchanged, and both prototypes of an
// adapter (Norbyte's on one side, the game function's on the other). A
// prototype and an entry that do not agree are found by a test, and again
// when the game is bound, where the entry is then refused.
//
// The rules are Apple's arm64 ones for the kinds of parameter met here:
// integers, enumerations, pointers and references go in x0 to x7 in order,
// and after those on the stack, each at its own size and alignment (not in
// 8-byte slots). A parameter of any other kind (floating point, a structure
// by value) makes the prototype unknown, except a structure named below as
// travelling in one register.

namespace bg3se::mac::binding {

struct Slot
{
    engine::Place At{ engine::Place::X0 };
    uint16_t StackOffset{ 0 };
    uint8_t Bytes{ 0 };
    bool Address{ false };      // a pointer or a reference
};

struct Prototype
{
    Slot Args[engine::kMaxArguments]{};
    uint8_t Count{ 0 };
    engine::Returns Result{ engine::Returns::Nothing };
    // False when a parameter or the result is of a kind the rules above do
    // not cover: nothing may be bound through such a prototype.
    bool Known{ true };
    bool Present{ false };      // false for a row that has no prototype (a global)
};

// A structure passed by value in one general register: trivially copyable,
// at most 8 bytes, no floating point member. Named per type, never assumed.
template <class T> inline constexpr bool kTravelsInOneRegister = false;

// How a structure comes back as a result. Named per type.
template <class T> inline constexpr engine::Returns kComesBackIn = engine::Returns::Nothing;

namespace detail {

template <class T>
constexpr Slot Passed()
{
    using Plain = std::remove_cv_t<T>;
    if constexpr (std::is_reference_v<T> || std::is_pointer_v<Plain>) {
        return { engine::Place::X0, 0, 8, true };
    } else if constexpr (std::is_integral_v<Plain> || std::is_enum_v<Plain>) {
        return { engine::Place::X0, 0, static_cast<uint8_t>(sizeof(Plain)), false };
    } else if constexpr (std::is_class_v<Plain>) {
        if (kTravelsInOneRegister<Plain>) return { engine::Place::X0, 0, static_cast<uint8_t>(sizeof(Plain)), false };
        return {};
    } else {
        return {};
    }
}

template <class R>
constexpr engine::Returns Returned(bool& known)
{
    using Plain = std::remove_cv_t<R>;
    if constexpr (std::is_void_v<Plain>) {
        return engine::Returns::Nothing;
    } else if constexpr (std::is_reference_v<R> || std::is_pointer_v<Plain>) {
        return engine::Returns::X0;
    } else if constexpr (std::is_integral_v<Plain> || std::is_enum_v<Plain>) {
        return sizeof(Plain) <= 4 ? engine::Returns::W0 : engine::Returns::X0;
    } else {
        if (std::is_class_v<Plain> && kComesBackIn<Plain> != engine::Returns::Nothing) return kComesBackIn<Plain>;
        known = false;
        return engine::Returns::Nothing;
    }
}

template <class R, class... P>
constexpr Prototype Lay()
{
    Prototype p;
    p.Present = true;
    p.Result = Returned<R>(p.Known);
    if (sizeof...(P) > engine::kMaxArguments) {
        p.Known = false;
        return p;
    }
    Slot const passed[] = { Passed<P>()..., Slot{} };
    unsigned reg = 0, stack = 0;
    for (size_t i = 0; i < sizeof...(P); i++) {
        Slot slot = passed[i];
        if (slot.Bytes == 0 || slot.Bytes > 8 || (slot.Bytes & (slot.Bytes - 1)) != 0) {
            p.Known = false;
            slot.Bytes = 0;
        } else if (reg < 8) {
            slot.At = static_cast<engine::Place>(reg++);
        } else {
            stack = (stack + slot.Bytes - 1u) & ~(slot.Bytes - 1u);
            slot.At = engine::Place::Stack;
            slot.StackOffset = static_cast<uint16_t>(stack);
            stack += slot.Bytes;
        }
        p.Args[p.Count++] = slot;
    }
    return p;
}

template <class F> struct Described;
template <class R, class... P> struct Described<R(P...)> { static constexpr Prototype Value = Lay<R, P...>(); };
template <class R, class... P> struct Described<R(P...) noexcept> { static constexpr Prototype Value = Lay<R, P...>(); };

}

// The prototype of the function type F.
template <class F> inline constexpr Prototype kPrototypeOf = detail::Described<F>::Value;

constexpr bool SameSlot(Slot const& a, Slot const& b)
{
    return a.At == b.At && a.StackOffset == b.StackOffset && a.Bytes == b.Bytes && a.Address == b.Address;
}

// Whether two prototypes pass the same things in the same places.
constexpr bool SamePassing(Prototype const& a, Prototype const& b)
{
    if (!a.Known || !b.Known || a.Count != b.Count || a.Result != b.Result) return false;
    for (unsigned i = 0; i < a.Count; i++) {
        if (!SameSlot(a.Args[i], b.Args[i])) return false;
    }
    return true;
}

// What a comparison with a table entry may let pass.
enum Allow : unsigned
{
    kExactly = 0,
    // The prototype is the game function's own (the far side of an adapter):
    // which of Norbyte's parameters an argument corresponds to is not compared.
    kOwnPrototype = 1,
    // A function Norbyte only hooks: his prototype declares a result that the
    // game function does not produce. His wrapper hands back to the game's
    // caller whatever the function left in x0, and that caller reads nothing.
    kResultNotProduced = 2,
    // A function Norbyte only hooks: his prototype has parameters, in
    // registers and after the ones the game function takes, that the game
    // function does not have. His wrapper receives whatever those registers
    // hold and hands the same on to the function, which reads none of them.
    // For a row whose handler does not read them either.
    kParametersNotTaken = 4,
    // A function Norbyte only hooks: his prototype declares, as an integer of
    // a pointer's width, a parameter the game function takes a pointer in (a
    // reference to something he gave no type). His wrapper hands the eight
    // bytes on unchanged. For a row whose handler does nothing else with them.
    kPointersAsWords = 8,
};

// How the function is used through the prototype.
enum class Through : uint8_t
{
    Call,       // Norbyte's code calls it
    Hook,       // a detour of this prototype replaces it and calls the original through the same prototype
};

// Empty when a function with this prototype takes and returns what the table
// says the game function does; otherwise the first difference, for the log.
std::string Disagreement(Prototype const& prototype, engine::Signature const& game, Through use, unsigned allow = kExactly);

std::string Describe(Prototype const& prototype);

}
