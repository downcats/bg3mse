#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Engine/EngineTable.h>
#include <Engine/EntryUse.h>
#include <Engine/TypeIndices.h>
#include <Platform/FunctionExtents.h>
#include <Platform/HookPlanner.h>
#include <Platform/Symbols.h>

// Runs the engine table against the game: for every entry, the address and
// the facts that were checked, or the reason it is refused.
//
// It works the same on the running game and on its binaries mapped from disk,
// so the tool and the tests say what the game will say. It reads the symbol
// tables, code and, for vtables, the constant slots of the vtable itself; it
// writes nothing and never reads a variable of the game (the game may not have
// built the object yet). Whether a global is the object or a pointer to it is
// stated by the table and compared with what the symbol table shows (its
// section, its distance to the next symbol).
//
// Shapes are compared with the code as it is. A function that already carries
// an entry hook no longer has its recorded shape, and its entry is then
// refused like any other changed function: in the game, resolve before any
// hook is committed.
//
// Every name the table asks for is looked up in one pass over each symbol
// table per kind of lookup (exact names, name prefixes, following symbols,
// type indices).

namespace bg3se::mac::engine {

struct ImageView
{
    GameImage const* Symbols{ nullptr };
    // Function extents; optional. Without it a function ends at the next symbol.
    FunctionTable const* Functions{ nullptr };
};

struct Images
{
    ImageView Game;
    ImageView Osiris;       // entries of an image that is not given are unresolved

    ImageView const& Of(Image image) const { return image == Image::Game ? Game : Osiris; }
};

enum class Status : uint8_t
{
    Resolved,               // found, and every check passed
    ResolvedWithCaveat,     // the same; the table's caveat says what the user of it must mind
    NoCallableCode,         // confirmed: no symbol, and the functions the code went to are there
    Unresolved,             // refused; Reason says why
    Count
};

struct CheckResult
{
    char const* What;       // "symbol", "shape", "arguments", ...
    bool Passed;
    std::string Detail;
};

struct SlotResult
{
    VtableMethod const* Method;
    uint64_t Function{ 0 };     // link-time address of the method
    int32_t Offset{ -1 };       // from the address point; -1 when not found in exactly one slot
};

struct Resolved
{
    Entry const* Of{ nullptr };
    char const* Area{ nullptr };

    Status Is{ Status::Unresolved };
    std::string Reason;             // why it is unresolved

    // Link-time address in its image: the function, the call instruction, the
    // variable, the vtable's address point. 0 unless the entry is resolved, so
    // that nothing can use an address whose checks did not pass.
    uint64_t VA{ 0 };
    uint64_t End{ 0 };              // functions: where it ends
    // Where the entry was found, whatever became of its checks (for reports).
    uint64_t SeenVA{ 0 };

    CodeShape Shape{};              // functions: the shape found
    bool Scanned{ false };
    EntryUse Use{};                 // functions: what the entry does with its registers
    ReturnUse Returns{};            // functions: what its returns say about a result
    HookRefusal Hook{ HookRefusal::None };      // functions to hook: the planner's verdict
    std::vector<SlotResult> Slots;  // vtables
    std::vector<uint64_t> Operands; // the values read for Entry::Operands, in order (0 where not read)
    size_t TypeCount{ 0 };          // type indices: how many the context has

    std::vector<CheckResult> Checks;

    bool Usable() const { return Is == Status::Resolved || Is == Status::ResolvedWithCaveat; }
    // What the table expects of this entry on a game it was written for.
    Status Expected() const;
    bool AsExpected() const { return Is == Expected(); }
};

class Resolution
{
public:
    std::vector<Resolved> Entries;      // in the table's order
    TypeIndices Types;                  // every type index of the game image, for lookups by name
    Images In;

    Resolved const* Find(std::string_view name) const;

    // The address in this process, when the entry is resolved and its image
    // is loaded here; nullptr otherwise.
    void* Address(Resolved const& entry) const;

    size_t Count(Status status) const;
    bool AllAsExpected() const;
};

Resolution Resolve(std::span<Area const> table, Images const& images);

// The game and libOsiris.dylib as they are loaded in this process: what
// Resolve is given in the running game.
class LoadedGame
{
public:
    // False, with the reason, when the executable's symbol table or its table
    // of functions cannot be read. libOsiris.dylib not being loaded is no
    // failure: its entries are then unresolved, each for that reason.
    bool Open(std::string& problem);

    Images View() const;
    bool HasOsiris() const { return hasOsiris_; }

private:
    GameImage const* game_{ nullptr };
    FunctionTable gameFunctions_;
    GameImage osiris_;
    FunctionTable osirisFunctions_;
    bool hasOsiris_{ false };
};

char const* Describe(Status status);

}
