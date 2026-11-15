#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <Engine/EngineResolver.h>

// Filling the type index maps of Norbyte's StaticSymbols
// (BG3Extender/GameDefinitions/Symbols.h), which his entity system helpers
// read to learn the index of every component, system and static data type
// (EntitySystemHelpersBase::UpdateComponentMappings).
//
// On Windows five engine callbacks collect them from x86 code: for each index
// variable the address, the name of its type as the compiler printed it, and
// the context it belongs to. The macOS game names every such variable in its
// symbol table (Engine/TypeIndices.h), so here:
//
//   IndexSymbolToContextMaps    one pair for each of the five contexts his
//                               code tells apart: the address of the game's
//                               state for the context, and the name his code
//                               compares
//   IndexSymbolToNameMaps       one entry for every index variable of those
//                               contexts: its address, its type's name, and
//                               the context's state as above
//   StaticStringRegistrantMaps  nothing: on Windows it is where two of the
//                               callbacks meet (a function that makes a name,
//                               and a registration that names the function);
//                               nothing reads it once the game is scanned
//
// Names. A type's name in his definitions is the one the Windows compiler
// prints; the variable's symbol has the one this game's compiler mangled.
// The game makes one name out of every spelling before it registers a type
// (RegisteredTypeName, Engine/TypeIndices.h), and that is the rule here: a
// name of Norbyte's belongs to a variable when both spellings give the same
// registered name. The map then holds his spelling, which is what his
// lookups use. A name of his that reaches no variable, or more than one,
// stays out, with a line in the report.
//
// When. The variables hold 0x7fffffff until CoreLibSDM::Init has numbered
// the types, so nothing is put into the maps before that, and nothing on
// trust: a context is taken only when its state has the game's list of
// names, and a variable only when the name the list has at the variable's
// index is the name of its type. That one comparison shows that the
// numbering has run, that the variable is the one the game numbered, and
// that the name is right.

namespace bg3se::mac::binding {

// Norbyte's ecs::TypeIdContext (GameDefinitions/EntitySystemHelpers.h).
enum class TypeContext : uint8_t
{
    Component,
    OneFrameComponent,
    System,
    Replication,
    ImmutableData,
    Count
};

struct TypeContextRow
{
    TypeContext Is;
    char const* Entry;          // the engine table's entry
    // What UpdateComponentMappings compares the context's name with. It is
    // the game's name for the context, except for the systems, whose context
    // is ecs::SystemsContext in this game.
    char const* NorbyteName;
};

std::span<TypeContextRow const> TypeContextRows();

// A type that Norbyte's definitions look up by name.
enum class BoundAs : uint8_t
{
    Component,      // GameDefinitions/Components/AllComponentTypes.inl (MapComponentIndices)
    System,         // GameDefinitions/Components/AllSystemTypes.inl (MapSystemIndex)
    StaticData,     // FOR_EACH_GUID_RESOURCE_TYPE (MapResourceManagerIndex)
    Count
};

struct BoundType
{
    char const* EngineClass;    // the name his code looks up
    BoundAs As;
    bool OneFrame;              // components: declared as a one-frame component
};

// Every such type of this build, compiled as Norbyte's sources are
// (Binding/BoundTypes.cpp).
std::span<BoundType const> BoundTypes();

// A bound type that must stay unknown although the game has it, with the
// reason: its structure in this build is not confirmed to be the game's
// (Binding/Layouts.h).
struct WithheldType
{
    char const* EngineClass;
    std::string Why;
};

struct TypeVariable
{
    TypeContext Context;
    uint64_t VA;                // link-time address of the index variable
    std::string Name;           // what the map says: Norbyte's spelling when he binds the type, otherwise the game's
    std::string Registered;     // the name the game registers the type under
    bool Bound;                 // one of BoundTypes()
};

struct TypeIndexPlan
{
    struct Context
    {
        TypeContextRow const* Row{ nullptr };
        uint64_t StateVA{ 0 };          // 0 when the entry is refused
        std::string Refused;            // why, when it is
        size_t Variables{ 0 };          // index variables the game has in the context
    };

    struct Tally
    {
        size_t Bound{ 0 };          // names Norbyte's definitions look up
        size_t Mapped{ 0 };         // each to one variable
        size_t Unmapped{ 0 };       // no variable
        size_t Ambiguous{ 0 };      // more than one variable, or two names for one
    };

    Context Contexts[(size_t)TypeContext::Count];
    std::vector<TypeVariable> Variables;    // what goes into IndexSymbolToNameMaps
    Tally Types[(size_t)BoundAs::Count];
    size_t Replicated{ 0 };         // bound components that also have a replication index
    size_t ByRule{ 0 };             // mapped names whose spelling differs from the variable's and the rule joins
    // One line for each name that is unmapped or ambiguous, for each context
    // that is refused, and for each thing worth knowing about a mapped name.
    std::vector<std::string> Report;

    bool AllMapped() const;
};

// What the maps would hold, from a resolution of the game (running, or its
// binary mapped from disk).
TypeIndexPlan PlanTypeIndices(engine::Resolution const& resolution, std::span<BoundType const> bound,
    std::span<WithheldType const> withheld = {});

// Where the maps' entries go.
struct TypeIndexSink
{
    virtual ~TypeIndexSink() = default;
    virtual void Context(int32_t* state, char const* norbyteName) = 0;
    virtual void Type(int32_t* variable, char const* name, int32_t* state) = 0;
};

struct TypeIndexFill
{
    size_t Contexts{ 0 };       // contexts whose types the game has numbered
    size_t Types{ 0 };          // variables handed to the sink
    size_t Refused{ 0 };        // variables left out: the game's list does not name their type at their index
    std::vector<std::string> Report;
};

// Reads the game's memory through `at` (a link-time address to where it is
// loaded) and hands the sink every context the game has numbered and every
// variable of it that passes the comparison above. The names handed over are
// the plan's: the plan must outlive whatever keeps them.
TypeIndexFill FillTypeIndices(TypeIndexPlan const& plan, std::function<void*(uint64_t)> const& at, TypeIndexSink& sink);

// The same into the three maps of the StaticSymbols at `staticSymbols`
// (Binding/TypeIndexMaps.cpp, compiled as Norbyte's sources are). Nothing is
// added when the maps already hold entries.
TypeIndexFill FillStaticSymbolsTypeIndices(void* staticSymbols, TypeIndexPlan const& plan, std::function<void*(uint64_t)> const& at);

}
