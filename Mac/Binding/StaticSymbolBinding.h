#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <Binding/Prototype.h>
#include <Engine/EngineResolver.h>

// Filling Norbyte's StaticSymbols (BG3Extender/GameDefinitions/Symbols.h).
//
// On Windows the symbol mapper fills it from the x86 byte patterns of
// BinaryMappings.xml. Here each member gets its value from the engine table's
// entry of the same name, and only when that entry resolved on this game: an
// entry that is refused leaves its member null, and the report says so.
//
// There is one row for every member that points into the game. A row says
// what the member receives:
//
//   Global        the address of the entry's variable or object
//   Function      the game function itself: a call or a detour through
//                 Norbyte's prototype passes what the function takes, which
//                 is compared (Binding/Prototype.h) before it is bound
//   CallAdapter   a function of Norbyte's prototype that calls the game
//                 function as it must be called here (Binding/CallAdapters.h)
//   HookBridge    a function of Norbyte's prototype for his Wrap to detour,
//                 driven by this layer's hooks on the game (Binding/HookBridges.h)
//   Handlers      nothing: Norbyte's handlers for it are driven by this
//                 layer's hooks directly
//   Unavailable   nothing: the game has no code for it that can be called or
//                 hooked, and no place serves Norbyte's use of it
//
// The rows (Binding/StaticSymbolRows.cpp) are the one file here that sees
// Norbyte's definitions; everything else works on offsets, so the tests and
// the tool link the binding without his code.

namespace bg3se::mac::binding {

enum class Bound : uint8_t
{
    Global,
    Function,
    CallAdapter,
    HookBridge,
    Handlers,
    Unavailable,
    Count
};

struct MemberRow
{
    char const* Member;
    uint32_t Offset{ 0 };           // in StaticSymbols
    Bound How{ Bound::Unavailable };
    // The table entry the value comes from: the member's own, or the entry
    // of the place that stands in for it.
    char const* Entry{ nullptr };
    Prototype Norbyte{};            // functions: the prototype his code calls or detours it through
    Through Use{ Through::Call };
    bool AlsoHooked{ false };       // a function his code both calls and detours
    unsigned Allow{ kExactly };
    // His mapping for it is marked Critical, or nothing of the extender runs
    // without it here: when it stays null, start-up reports a critical
    // failure and the extender's features stay off.
    bool Critical{ false };
    char const* Requires[2]{};      // further entries that must have resolved
    // What his code does when the member is null.
    char const* WhenNull{ nullptr };
};

std::span<MemberRow const> StaticSymbolRows();

// A member of StaticSymbols that is not one pointer into the game: the
// three maps of type index variables (Binding/TypeIndexBinding.h fills
// them), and Libraries.
struct OtherMember
{
    char const* Name;
    uint32_t Offset;
    uint32_t Size;
};
std::span<OtherMember const> StaticSymbolOtherMembers();
size_t StaticSymbolsSize();

// What one row comes to, for the layers that have rows of their own for
// another structure (Binding/OsirisBinding.h).
//
// CheckMemberRow: what does not depend on a game. Empty when the row says
// what a null means, can take its entry and requires entries the table has;
// otherwise every finding, one to a line.
std::string CheckMemberRow(MemberRow const& row, std::span<engine::Area const> table);

// Empty when the rows hold together: every byte of StaticSymbols belongs to
// exactly one row or other member, every row's entry is in the table and of a
// kind the row can take, every function row's prototype agrees with its
// entry, every adapter, bridge and game hook agrees with its own, and every
// entry of the table is used by the binding.
// Otherwise every finding, one to a line.
std::string CheckRows(std::span<engine::Area const> table);

struct MemberResult
{
    MemberRow const* Row{ nullptr };
    void* Value{ nullptr };
    std::string Reason;             // why it is null, when it is and the row is not Unavailable
};

// The value for one row on a game, or why there is none. With `live` the
// value is an address in this process, otherwise a link-time address. A
// bridged row has a value only while its game hooks are attached, or, when
// `hooksAttached` is false, would go on.
MemberResult DecideMember(MemberRow const& row, engine::Resolution const& resolution, bool hooksAttached, bool live);

struct BindReport
{
    std::vector<MemberResult> Members;      // in the rows' order
    size_t BoundCount{ 0 };
    // A critical member is null.
    bool CriticalFailed{ false };
    // A member that this game is expected to provide is null. The members
    // that are Unavailable by their row do not count: they are null on every
    // game the table was written for, and the log lists them once.
    bool Failed{ false };
};

// What BindStaticSymbols would store, without storing anything. On a
// resolution of binaries mapped from disk the values are link-time addresses.
BindReport PlanStaticSymbols(engine::Resolution const& resolution, bool hooksAttached);

// Fills the StaticSymbols at `symbols` from the resolution of the running
// game, gives each adapter its target, and reports. Bridged members are bound
// only when their game hooks are attached (AttachGameHooks).
BindReport BindStaticSymbols(void* symbols, engine::Resolution const& resolution);

// A game function that carries a hook of this image by the time StaticSymbols
// is bound (the platform's hook on the file reader's constructor,
// Platform/FileReaderHook.h): the members bound from `entry` then get
// `function`, the hook's own function, which has the game function's
// prototype and runs it. A call through the member takes the route the game's
// own calls take, and Norbyte's Wrap puts its detour on that function; the
// game function takes one hook only. nullptr takes the statement back. Not
// used when planning for a game on disk.
void SetHookedEntry(std::string_view entry, void* function);

char const* Describe(Bound how);

}
