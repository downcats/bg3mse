// Tests of the MSVC type names (Mac/TypeNames/MsvcTypeName.h), on types of
// this file. Compiled in the dialect Norbyte's sources are compiled in, since
// that is where the names are built.

#include <TypeNames/MsvcTypeName.h>

#include <cstdio>
#include <string>

namespace bg3se::esv {

struct Character
{
    struct Inner {};
    class Hidden {};
};
class Manager {};
union Either { int a; float b; };
enum class Kind { A };
enum Plain { B };

template <class T> struct Box { T value; };
template <class A, class B> class Pair {};
template <class T = float> struct Defaulted {};

}

template <> inline constexpr bool bg3se::mac::typenames::kDeclaredAsClass<bg3se::esv::Manager> = true;
template <> inline constexpr bool bg3se::mac::typenames::kDeclaredAsClass<bg3se::esv::Character::Hidden> = true;
template <class... Arguments> inline constexpr bool bg3se::mac::typenames::kDeclaredAsClass<bg3se::esv::Pair<Arguments...>> = true;

namespace {

int gFailures = 0;

template <class T>
void Expect(char const* expected)
{
    std::string name = bg3se::mac::typenames::MsvcTypeName<T>();
    if (name != expected) {
        printf("FAIL: got '%s', expected '%s'\n", name.c_str(), expected);
        gFailures++;
    }
}

}

int main()
{
    using namespace bg3se::esv;

    // A class key, then the qualified name.
    Expect<Character>("struct bg3se::esv::Character");
    Expect<Manager>("class bg3se::esv::Manager");
    Expect<Either>("union bg3se::esv::Either");
    Expect<Kind>("enum bg3se::esv::Kind");
    Expect<Plain>("enum bg3se::esv::Plain");
    Expect<Character::Inner>("struct bg3se::esv::Character::Inner");
    Expect<Character::Hidden>("class bg3se::esv::Character::Hidden");

    // A template's arguments in the same spelling: no blank after a comma,
    // a blank between two closing brackets, every argument written out.
    Expect<Box<Character>>("struct bg3se::esv::Box<struct bg3se::esv::Character>");
    Expect<Box<Manager>>("struct bg3se::esv::Box<class bg3se::esv::Manager>");
    Expect<Box<float>>("struct bg3se::esv::Box<float>");
    Expect<Box<Box<float>>>("struct bg3se::esv::Box<struct bg3se::esv::Box<float> >");
    Expect<Box<Box<Box<Kind>>>>("struct bg3se::esv::Box<struct bg3se::esv::Box<struct bg3se::esv::Box<enum bg3se::esv::Kind> > >");
    Expect<Pair<int, Either>>("class bg3se::esv::Pair<int,union bg3se::esv::Either>");
    Expect<Pair<Box<int>, Box<bool>>>("class bg3se::esv::Pair<struct bg3se::esv::Box<int>,struct bg3se::esv::Box<bool> >");
    Expect<Defaulted<>>("struct bg3se::esv::Defaulted<float>");

    // MSVC's keywords for the fundamental types that have one size on both systems.
    Expect<Box<unsigned int>>("struct bg3se::esv::Box<unsigned int>");
    Expect<Box<long long>>("struct bg3se::esv::Box<__int64>");
    Expect<Box<unsigned long long>>("struct bg3se::esv::Box<unsigned __int64>");
    Expect<Box<unsigned char>>("struct bg3se::esv::Box<unsigned char>");
    Expect<Box<double>>("struct bg3se::esv::Box<double>");

    printf("%d failures\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
