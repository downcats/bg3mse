#pragma once

#include <Mac/TypeNames/MsvcTypeName.h>

// Says of the types in DeclaredAsClass.inl that they are declared with
// `class`. Included after the definitions of the registered types, which the
// list names as the property maps do: from namespace bg3se.

namespace bg3se {

#define BG3MSE_DECLARED_AS_CLASS(type) \
    template <> inline constexpr bool mac::typenames::kDeclaredAsClass<type> = true;
#define BG3MSE_DECLARED_AS_CLASS_TEMPLATE(name) \
    template <class... Arguments> inline constexpr bool mac::typenames::kDeclaredAsClass<name<Arguments...>> = true;
#include <Mac/TypeNames/DeclaredAsClass.inl>
#undef BG3MSE_DECLARED_AS_CLASS
#undef BG3MSE_DECLARED_AS_CLASS_TEMPLATE

}
