#pragma once

#include <span>
#include <string>
#include <vector>

#include <Binding/TypeIndexBinding.h>
#include <Engine/EngineResolver.h>
#include <Layout/BoundMembers.h>
#include <Layout/GameFields.h>
#include <Layout/GameShapes.h>
#include <Layout/VirtualSlots.h>

// What of the binding depends on a structure of Norbyte's being the game's.
//
// Most of his structures are reached through pointers the game hands out at
// run time and cannot be withheld one by one. Some are reached through
// something the binding gives him: the table of fixed strings, the resource
// manager, the stats object, the client and server objects (with the mod
// manager of each), the path roots and the file reader through the members
// of StaticSymbols that lead to them,
// and the effects manager through its system type's index. Each is proven
// against the game's code (Layout/GameShapes.h); on a game where the proof
// does not hold the binding keeps back what leads to the structure, so his
// code finds nothing instead of reading the wrong members. Where his code
// reads through the member without a test, the member is a critical one,
// and start-up refuses instead (Binding/Libraries.h).
//
// Component types are reached through their type index. Besides the types a
// shape proves, the members of every bound component type are compared with
// where the game's own code puts the members it names or replicates
// (Layout/ComponentMembers.h): a type the game contradicts is left unknown
// to Lua. A type the game's code says nothing about stays as it is.

namespace bg3se::mac::binding {

struct LayoutDependent
{
    GameShape Shape;
    char const* Member;         // the member of StaticSymbols to leave null, or nullptr
    char const* EngineClass;    // the bound type to leave unknown, or nullptr
};

// Compiled as Norbyte's sources are (Binding/BoundTypes.cpp), where the
// names are taken from his declarations.
std::span<LayoutDependent const> LayoutDependents();

// What of the binding depends on the game's vtables holding a class's
// virtual functions in the slots this build calls them through
// (Layout/VirtualSlots.h). For each class the check covers, what leads his
// code to objects of the class. A class his code reaches only through
// pointers the game hands out, at one place of his code, is asked about there
// instead (Binding/VirtualCalls.h): `Asked`. A class his code reaches from
// everywhere, through game objects it reads without a test, cannot be kept
// back: start-up refuses (`Refuses`).
struct VirtualDependent
{
    char const* Class;          // DeclaredVirtual::Class
    char const* Member;         // the member of StaticSymbols to leave null, or nullptr
    char const* EngineClass;    // the bound type to leave unknown, or nullptr
    bool Asked{ false };        // his code asks VirtualsConfirmed before it calls
    bool Refuses{ false };      // start-up refuses
};

// Compiled as Norbyte's sources are (Binding/VirtualDependents.cpp).
std::span<VirtualDependent const> VirtualDependents();

struct LayoutsWithheld
{
    struct Member
    {
        char const* Name;
        std::string Why;
    };
    std::vector<Member> Members;
    std::vector<WithheldType> Types;
    // The classes every checked virtual function of which is in the game's
    // slot; a class with a function that is not, or that was not checked,
    // is not here.
    std::vector<std::string> ConfirmedClasses;
    // For each class that is not, why.
    std::vector<std::string> UnconfirmedClasses;
    // Of those, the ones start-up must refuse for.
    std::vector<std::string> Refusing;
    // Why the members of the component types could not be compared with the
    // game's; empty when they were. Nothing is withheld for it.
    std::string MembersUnchecked;
    // Which of the game's functions of component types were not found, when
    // the members were compared with the others; empty when all were.
    std::string MembersPartlyUnchecked;
};

// What must be kept back on a game with these shapes: nothing when every
// shape a dependent names is confirmed.
LayoutsWithheld WithheldFor(GameShapes const& shapes, std::span<LayoutDependent const> dependents);

// Adds to `withheld` what must be kept back on a game whose vtables gave
// these results: what leads to each class that has a result with a problem,
// or none at all.
void WithholdFor(LayoutsWithheld& withheld, std::span<VirtualSlotResult const> slots, std::span<VirtualDependent const> dependents);

// Adds to `withheld` the bound component types whose members the game's
// code contradicts (MemberDifference::Contradicts), each once, with the
// first such member in the reason.
void WithholdContradicted(LayoutsWithheld& withheld, GameFieldScan const& game, std::span<BoundClass const> classes);

// What this build's dependents must keep back on the game a resolution was
// made on. Without the game's image nothing is confirmed, and everything
// that depends on a layout is kept back.
LayoutsWithheld LayoutsWithheldOn(engine::Resolution const& resolution);

// What the start-up sequence found on the running game, before any hook went
// on (Binding/Startup.h). It is kept for the binding and for
// VirtualsConfirmed, so that each reads the one answer found then.
void KeepLayoutsOfTheGame(LayoutsWithheld const* layouts);
LayoutsWithheld const* LayoutsOfTheGame();

// The layouts kept for the game, or else those found now on the game of `resolution`.
LayoutsWithheld LayoutsFor(engine::Resolution const& resolution);

}
