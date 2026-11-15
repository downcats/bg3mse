#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <Layout/DeclaredVirtuals.h>
#include <Layout/GameShapes.h>
#include <Platform/Symbols.h>

// The slots of the game's virtual functions that Norbyte's code calls
// through his declarations, or fills with functions of his own when a class
// of his derives from a game class.
//
// His headers list a class's virtual functions in the order the Windows
// compiler lays them out. This build lays them out as the macOS game's
// compiler does, from the order they are declared in: a virtual destructor
// takes two slots, and the overloads of one name stay in the order written
// (the Windows compiler reverses them). Where the two orders differ the
// declaration has the macOS order outside MSVC, and this is the check that it
// is the game's: for each function, the slot this build calls it through
// (taken from the pointer to the member, which on arm64 holds the slot's
// offset) must be a slot in which the game's own vtable for the class, or for
// a class derived from it, holds the function that row names.
//
// The game's vtables and the functions in them are found by name; nothing
// here is an address or a slot number written down.

namespace bg3se::mac {

struct VirtualSlotResult
{
    DeclaredVirtual const* Of{ nullptr };
    int GameSlot{ -1 };         // the slot the game's vtable holds the function in (the first, if several); -1 when none
    std::string Problem;        // empty when the declared slot is one the game holds the function in
};

std::vector<VirtualSlotResult> CheckVirtualSlots(GameImage const& image, std::span<DeclaredVirtual const> declared);

// A slot whose row names a proof of how its result comes back
// (DeclaredVirtual::ResultProof) holds only while that proof does: otherwise
// it is given the proof's problem.
void RequireResultProofs(std::vector<VirtualSlotResult>& slots, GameShapes const& shapes);

// A class whose members his code reads where it asks whether the class's
// virtual functions are confirmed (Binding/VirtualCalls.h), and the shape of
// those members: the class is confirmed only with it.
struct ClassMembers
{
    char const* Class;
    GameShape Shape;
};
std::span<ClassMembers const> ClassMemberShapes();

// A slot of a class whose members are a shape (ClassMemberShapes) holds only
// while that shape is confirmed: otherwise it is given the shape's problem.
void RequireMemberShapes(std::vector<VirtualSlotResult>& slots, GameShapes const& shapes, std::span<ClassMembers const> classes);

}
