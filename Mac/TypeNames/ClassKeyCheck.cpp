// The class key of every registered type, checked against its declaration.
//
// MSVC's name for a class type starts with the key it was declared with, and
// nothing in the language tells `struct` from `class`
// (Mac/TypeNames/MsvcTypeName.h). The compiler does know: naming a type with
// the other key is what its mismatched-tags diagnostic reports, and it
// compares with the type's definition, whose key is also the one MSVC uses.
// ClassKeyCheck.inl, made by Mac/Tools/MakeClassKeyCheck.cmake, names every
// type of GameDefinitions/Generated/PropertyMaps.inl with the key
// DeclaredAsClass.inl gives it; with the diagnostic an error, this file
// compiles only when each key is the declared one. A union or an enumeration
// among the registered types is an error here as well, so that its name is
// looked at when the first one appears.
//
// The diagnostic leaves out every declaration at a place where it is turned
// off, so it must be on for the whole file (-Wmismatched-tags, as a warning),
// not only around the lines below, where it is an error. A type defined in a
// system header (the NoesisGUI SDK) is compared with Norbyte's own forward
// declaration of it instead.
//
// Compiled as Norbyte's sources are; it has no code.

#include <stdafx.h>

#include <GameDefinitions/Generated/PropertyMapMeta.h>
#include <GameDefinitions/Generated/EnumerationMeta.h>
#include <GameDefinitions/Generated/EventsMeta.h>
#include <Lua/Shared/Proxies/PropertyMapDependencies.h>

#include <Mac/TypeNames/DeclaredAsClass.h>

BEGIN_SE()

#pragma clang diagnostic error "-Wmismatched-tags"

// The key as written is the declared one, and is the one the name is built with.
#define BG3MSE_CHECK_KEY(key, ...) \
    static_assert(sizeof(key __VA_ARGS__*) > 0); \
    static_assert(mac::typenames::ClassKey<__VA_ARGS__>() == std::string_view(#key));
#include <ClassKeyCheck.inl>
#undef BG3MSE_CHECK_KEY

END_SE()
