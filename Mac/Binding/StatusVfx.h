#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bg3se::mac::engine { class Resolution; }

// Which class an object behind an ecl::StatusVFX pointer has, for Norbyte's
// MakePolymorphicRef (GameDefinitions/Status.inl).
//
// His Windows code reads x86 instruction bytes of three of the object's
// virtual functions, through a vtable of five slots. The macOS game's vtables
// have seven slots and arm64 code, and keep their names: here the object's
// vtable pointer is compared with the vtables of the classes he tells apart,
// which are engine table entries. Each entry states which functions its
// vtable holds, so a class is known only while it overrides what it did when
// the comparison was written (Engine/Entries/World.cpp says what his bytes
// identify and how that reads here).

namespace bg3se::mac::binding {

// The classes Norbyte's code makes a reference of. StatusVFXAura and
// StatusVFXMaterial are not among them on Windows either: an object of
// those, like one of a class not known here, is the base class.
enum class StatusVfxClass : uint8_t
{
    Base,
    BoostMaterial,
    ApplyEffect,
    Effect,
    Visual,
    Managed,
    Beam,
    Count
};

// The engine table's entry for each class's vtable; none for the base class.
char const* StatusVfxEntryName(StatusVfxClass cls);
// The two classes that are told apart only to be left as the base class.
constexpr char const* kStatusVfxBaseEntryNames[] = { "ecl::StatusVFXAura", "ecl::StatusVFXMaterial" };

// The class of `object`: the one whose vtable its first word points to.
// Base for any other object, for null, and while the vtables are not bound.
StatusVfxClass ClassOfStatusVfx(void const* object);

// Empty when every one of the vtables resolved, the two that are left as the
// base class included, and no two are at one address; otherwise which is not
// and why.
std::string StatusVfxRefusal(engine::Resolution const& resolution);

// Takes the vtables from a resolution of the running game. When
// StatusVfxRefusal refuses it, ClassOfStatusVfx gives Base for every object:
// a game whose classes are not all the ones known here may have others, and
// none is guessed.
void BindStatusVfx(engine::Resolution const& resolution);

// For tests: the address an object's vtable pointer holds for each class,
// indexed by StatusVfxClass (the entry for Base is not read); nullptr unbinds.
void SetStatusVfxForTest(void const* const* vtables);

}
