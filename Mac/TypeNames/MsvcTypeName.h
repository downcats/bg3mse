#pragma once

#include <string>
#include <string_view>
#include <type_traits>

// The name MSVC's typeid(T).name() gives a type, built on a compiler that
// spells type names otherwise.
//
// Norbyte's type information gives every registered class its "native name"
// (Lua/Shared/Proxies/LuaTypeInformation.cpp, BEGIN_CLS_TN), and scripts can
// read it. On Windows that is typeid(T).name(), which is MSVC's own spelling:
//
//     struct bg3se::esv::Character
//     class Noesis::Visual
//     struct bg3se::aspk::TLKeyBaseComponent<struct bg3se::aspk::keys::ValueKey<float> >
//
// a class key, the qualified name, and for a class template its arguments in
// the same spelling, separated by commas without a blank, with a blank
// between two closing brackets. This compiler's typeid name is the mangled
// name, and needs the type's type_info object besides.
//
// What is taken from where:
//   - the qualified name is the compiler's (__PRETTY_FUNCTION__), which
//     spells namespaces and nested classes as MSVC does;
//   - a class template's arguments are spelled one by one, by this same rule;
//   - the class key cannot be asked of the compiler: `struct` and `class`
//     declare the same kind of type. A type declared with `class` is listed
//     in DeclaredAsClass.inl, every other class type is taken to be a
//     `struct`, and the build checks both statements for every registered
//     type (Mac/TypeNames/ClassKeyCheck.cpp);
//   - a fundamental type has MSVC's keyword for it.
//
// A type whose MSVC spelling is not written here does not compile: a pointer,
// reference or qualified type, a template with an argument that is not a
// type, a class nested in a class template, a type in an unnamed namespace,
// and `long`, whose size is not the same on the two systems.

namespace bg3se::mac::typenames {

// True for a type declared with the class key `class` (DeclaredAsClass.inl).
template <class T> inline constexpr bool kDeclaredAsClass = false;

// The compiler's spelling of T: its qualified name, with template arguments.
template <class T>
constexpr std::string_view CompilerName()
{
    std::string_view text = __PRETTY_FUNCTION__;
    size_t start = text.find("T = ") + 4;
    return text.substr(start, text.rfind(']') - start);
}

template <class T>
constexpr std::string_view ClassKey()
{
    if (std::is_enum_v<T>) return "enum";
    if (std::is_union_v<T>) return "union";
    return kDeclaredAsClass<T> ? "class" : "struct";
}

template <class T>
struct MsvcName
{
    static std::string Get()
    {
        static_assert(std::is_class_v<T> || std::is_union_v<T> || std::is_enum_v<T>, "the MSVC name of this kind of type is not written");
        static constexpr std::string_view name = CompilerName<T>();
        static_assert(name.find('<') == std::string_view::npos, "the MSVC name of a type inside a class template, or of a template with an argument that is not a type, is not written");
        static_assert(name.find('(') == std::string_view::npos, "the MSVC name of a type in an unnamed namespace or a function is not written");
        std::string out(ClassKey<T>());
        out += ' ';
        out += name;
        return out;
    }
};

template <template <class...> class Template, class... Arguments>
struct MsvcName<Template<Arguments...>>
{
    static std::string Get()
    {
        using T = Template<Arguments...>;
        static constexpr std::string_view name = CompilerName<T>();
        static constexpr std::string_view templateName = name.substr(0, name.find('<'));
        static_assert(templateName.find('(') == std::string_view::npos, "the MSVC name of a type in an unnamed namespace or a function is not written");
        std::string out(ClassKey<T>());
        out += ' ';
        out += templateName;
        out += '<';
        bool first = true;
        auto add = [&](std::string const& argument) {
            if (!first) out += ',';
            out += argument;
            first = false;
        };
        (add(MsvcName<Arguments>::Get()), ...);
        if (out.back() == '>') out += ' ';
        out += '>';
        return out;
    }
};

#define BG3MSE_MSVC_KEYWORD(type, keyword) \
    template <> struct MsvcName<type> { static std::string Get() { return keyword; } }
BG3MSE_MSVC_KEYWORD(bool, "bool");
BG3MSE_MSVC_KEYWORD(char, "char");
BG3MSE_MSVC_KEYWORD(signed char, "signed char");
BG3MSE_MSVC_KEYWORD(unsigned char, "unsigned char");
BG3MSE_MSVC_KEYWORD(short, "short");
BG3MSE_MSVC_KEYWORD(unsigned short, "unsigned short");
BG3MSE_MSVC_KEYWORD(int, "int");
BG3MSE_MSVC_KEYWORD(unsigned int, "unsigned int");
BG3MSE_MSVC_KEYWORD(long long, "__int64");
BG3MSE_MSVC_KEYWORD(unsigned long long, "unsigned __int64");
BG3MSE_MSVC_KEYWORD(float, "float");
BG3MSE_MSVC_KEYWORD(double, "double");
BG3MSE_MSVC_KEYWORD(wchar_t, "wchar_t");
BG3MSE_MSVC_KEYWORD(char8_t, "char8_t");
BG3MSE_MSVC_KEYWORD(char16_t, "char16_t");
BG3MSE_MSVC_KEYWORD(char32_t, "char32_t");
#undef BG3MSE_MSVC_KEYWORD

template <class T>
std::string MsvcTypeName()
{
    return MsvcName<T>::Get();
}

}
