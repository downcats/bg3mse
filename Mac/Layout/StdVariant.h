#pragma once

#include "GameVariant.h"

// Norbyte's definitions declare the game's variants as std::variant, and his
// Lua bindings handle them through templates written for std::variant<Args...>
// and through std::visit, std::get and the like. All of that code is in
// namespace bg3se. On Mac the game's variant is not libc++'s std::variant
// (Layout/GameVariant.h), so inside namespace bg3se the name std::variant
// means the game's variant: namespace bg3se::std takes everything from the
// real namespace std and declares these few names itself. Qualified lookup
// finds a name declared here before it looks into ::std, so std::variant<...>
// written anywhere in namespace bg3se is GameVariant<...>, every other std
// name is what it always was, and the functions that also serve tuples and
// arrays (std::get) keep their std overloads next to the variant ones.
//
// Code outside namespace bg3se, the standard library and the other libraries
// are untouched: there std::variant is libc++'s.

namespace bg3se::std {

using namespace ::std;

template <class... Ts>
using variant = ::bg3se::mac::GameVariant<Ts...>;

using ::bg3se::mac::variant_size;
using ::bg3se::mac::variant_size_v;
using ::bg3se::mac::variant_alternative;
using ::bg3se::mac::variant_alternative_t;

using ::std::get;
using ::bg3se::mac::get;
using ::std::get_if;
using ::bg3se::mac::get_if;
using ::std::holds_alternative;
using ::bg3se::mac::holds_alternative;
using ::std::visit;
using ::bg3se::mac::visit;

}

// Norbyte's code in namespace Noesis works with `using namespace bg3se;`
// (GameDefinitions/UI.h). A using-directive makes an unqualified "std" there
// name two namespaces at once, so namespace Noesis says which it means: the
// same one as the code in namespace bg3se it shares its types with.
namespace Noesis {

namespace std = ::bg3se::std;

}
