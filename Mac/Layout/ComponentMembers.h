#pragma once

#include <Layout/BoundMembers.h>
#include <Layout/GameFields.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Where this build puts the members of the bound component types, compared
// with where the game's own code puts the members it names, replicates,
// saves and loads (Layout/GameFields.h). What a function writes as a
// member's bytes, the address it hands on and a named member pointer must
// each be one member here (or several whole ones); a load or a store only
// confirms a member when it is exactly one, since code copies several
// members and their padding at once. The one comparison: bg3mse-layouts members prints
// it for the game binary on disk, and the start-up sequence withholds from
// Lua the component types the running game contradicts (Binding/Layouts.h).

namespace bg3se::mac {

// What this build has at `size` bytes from `offset` of a bound structure.
enum class Place : uint8_t
{
    Member,         // one member, or one element of an array member, of that size, or several whole ones
    InsideMember,   // part of a member that is not a scalar and has no property map of its own
    NotMapped,      // no mapped member covers it (the definitions hide the member, or have none)
    WrongScalar,    // a scalar member covers it, but begins elsewhere or has another size
    Straddles,      // it begins in one member and ends past it, or past the end of the structure
    Count
};

struct MemberDifference
{
    char const* Source{ nullptr };   // "pointer" (a named member pointer), or the component's function (SerializedComponent::Source)
    std::string Class;          // the game's class
    char const* EngineClass{ nullptr };   // the bound type's, when it is a bound component type
    uint32_t Offset{ 0 }, Size{ 0 };    // what the game reads or writes
    std::string GameName;       // the game's member, for a member pointer
    FieldAccess How{ FieldAccess::Written };
    Place Where{ Place::Member };
    std::string Path;           // the member here, e.g. "Hp.Value"
    uint32_t PathOffset{ 0 }, PathSize{ 0 };   // of the innermost member reached
    uint32_t ClassSize{ 0 };    // the bound structure's size
    char const* Reviewed{ nullptr };          // why the difference is not a member in another place, when it was read
    // The game's code contradicts this build: the value is not one member
    // here, it is part of a scalar or crosses a member's end, and it is not
    // a difference that was read against the game's code.
    bool Contradicts() const;
    // One line: the type, the game's member and what is here.
    std::string Describe() const;
};

struct MemberComparison
{
    size_t NamedBound{ 0 }, NamedUnbound{ 0 };      // member pointers into bound component types, and into other types
    size_t SerializedBound{ 0 };                    // serializers of bound component types
    size_t Fields{ 0 };                             // the members they write, or hand on the address of, at a constant place
    size_t CopiesOfMembers{ 0 };                    // loads and stores of exactly one member
    size_t Copies{ 0 };                             // loads and stores of anything else: no evidence either way
    size_t WritesNotRead{ 0 };
    size_t Nothing{ 0 };                            // serializers that write no member's own bytes
    size_t Counts[(size_t)Place::Count]{};          // by place, a reviewed difference not counted
    size_t ReviewedSeen{ 0 };
    std::vector<MemberDifference> Differences;      // every value written, handed on or named that is not one member here
    std::vector<std::string> Stale;                 // reviewed differences the game does not have any more

    size_t Contradictions() const;
};

MemberComparison CompareComponentMembers(GameFieldScan const& game, std::span<BoundClass const> classes);

// The game's members of the bound component types: DecodeGameFields of the
// functions of those types only, the decode both bg3mse-layouts members and
// the start-up sequence compare.
GameFieldScan DecodeBoundComponentFields(GameImage const& image, std::span<BoundClass const> classes);

}
