#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Platform/Symbols.h>

// Type indices by name.
//
// On Windows Norbyte learns which variable holds the index of which component,
// system, query or replicated type by running five byte patterns over the
// game (the engine callbacks BindECSContext, BindECSIndex,
// BindECSStaticStringConstructor, BindECSStaticRegistrant and
// BindComponentReplicationIDRef). The macOS game names every such variable in
// its symbol table:
//
//     ls::TypeId<T, Context>::m_TypeIndex
//
// so one pass over the table, demangling the names of that form, gives for
// every type and every context the address of its index variable. This is the
// lookup for all of them: the context is a parameter.
//
// The type is returned as the demangler prints it; comparing it with the names
// Norbyte uses is the binding layer's business.

namespace bg3se::mac::engine {

struct TypeIndex
{
    std::string Type;           // T, demangled: "eoc::HealthComponent"
    std::string Context;        // Context, demangled: "ecs::ComponentTypeIdContext"
    std::string_view Symbol;    // the mangled name, in the image's string table
    uint64_t VA{ 0 };           // link-time address of the index variable
};

class TypeIndices
{
public:
    // Every ls::TypeId<T, Context>::m_TypeIndex the image's symbol table names.
    static TypeIndices Collect(GameImage const& image);

    std::span<TypeIndex const> All() const { return all_; }
    size_t CountIn(std::string_view context) const;
    // The index variable of `type` in `context`, or nullptr when there is none
    // or the name is given to more than one address.
    TypeIndex const* Find(std::string_view type, std::string_view context) const;
    // The contexts met, each once, in the order first met.
    std::vector<std::string_view> Contexts() const;

private:
    std::vector<TypeIndex> all_;
    std::vector<uint32_t> byName_;      // indices into all_, sorted by (Context, Type)
};

// Splits "ls::TypeId<T, Context>::m_TypeIndex" as the demangler prints it.
// False when `demangled` is not of that form.
bool SplitTypeIndexName(std::string_view demangled, std::string& type, std::string& context);

// When the variables hold their indices, and how that is known.
//
// A variable's dynamic initialiser registers the type with its context and
// leaves 0x7fffffff in it (ls::TypeContext<Context>::RegisterType<T>(int*),
// called from the image's initialisers and nowhere else: it links a record
// holding the variable's address and the type's name into the context's
// list). The indices are given later, once, by ls::InitializeTypeRegistry,
// which is inline in CoreLibSDM::Init: for each context it sorts the records
// by name, numbers the distinct names from 0, stores each number through its
// record's pointer and appends each distinct name to a list of the context.
// Only when a context is numbered does the address of that list go into the
// first word of the context's state, ls::TypeContext<Context>::m_State.
//
// So a context whose state has a list is final, and the list is the game's
// own statement of which name has which index: names()[index] is the name of
// the type whose variable holds index. The structures below are the two the
// game builds there; the engine table ties their offsets to the instructions
// of CoreLibSDM::Init that store through them (Entries/World.cpp).

// ls::_StringView<char>, as the registry stores a type's name.
struct GameTypeName
{
    char const* Text;
    int32_t Length;
};

// ls::_private::ContextInfo: the names of a context's types, by index.
struct GameTypeNames
{
    GameTypeName* Names;
    uint32_t Capacity;
    uint32_t Count;
};

// ls::TypeContext<Context>::State, the object at m_State.
struct GameTypeContextState
{
    GameTypeNames* Numbered;    // null until the context's types have their indices
    void* Records;              // the registered types, most recent first
};

static_assert(sizeof(GameTypeName) == 16 && sizeof(GameTypeNames) == 16 && sizeof(GameTypeContextState) == 16);

// The name the game registers a type under, from the type's name as a
// compiler spells it. ls::_private::GenerateTypeName does this to the name
// its own compiler printed, so that every platform's build gives a type the
// same name: the words "const ", "class ", "struct ", "union " and "enum "
// are taken out where they begin a word, then " __cdecl", then
// "`anonymous namespace'", then the spaces. Two spellings of a type are the
// same type to the game exactly when they come out equal here.
std::string RegisteredTypeName(std::string_view spelled);

}
