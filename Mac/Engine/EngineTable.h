#pragma once

#include <cstdint>
#include <initializer_list>
#include <span>

#include <Platform/CodeSearch.h>
#include <Platform/CodeShape.h>

// The engine table: one entry for each thing in the game that Norbyte's
// extender calls, hooks or reads, saying how it is found in the macOS game
// and what is checked before it may be used.
//
// On Windows these come from x86 byte patterns (BG3Extender/GameHooks/
// BinaryMappings.xml, loaded into StaticSymbols). The macOS game keeps its
// symbol table, so here an entry is found by its mangled name, by the start
// of that name where the full one carries a long template list, or from the
// code of a named function. Nothing in the table is an address.
//
// A name proves which function it is, not how it takes its arguments on this
// binary: link-time optimisation drops and rewrites parameters. So every
// function to call or hook states its argument passing as it was read from
// the game's code, and the resolver (Engine/EngineResolver.h) checks that
// statement, and the function's recorded shape, against the code it finds.
// An entry whose symbol is missing or ambiguous, whose code changed, or whose
// statement does not hold, is unresolved: that entry alone.
//
// The table includes none of Norbyte's headers: Norbyte's names are strings
// here, so that the layer binding the results to StaticSymbols and the hooks
// is the only place that needs both.

namespace bg3se::mac::engine {

enum class Image : uint8_t
{
    Game,           // the game executable
    Osiris,         // libOsiris.dylib
    Count
};

enum class Kind : uint8_t
{
    FunctionToCall,
    FunctionToHook,
    FunctionToCallAndHook,
    CallSite,           // one call instruction inside a named function, to be redirected
    CodeSite,           // one instruction inside a named function, to be replaced by another
    GlobalObject,       // the address of the object itself
    GlobalPointer,      // the address of a variable holding a pointer to the object
    Vtable,             // the address an object's vtable pointer holds
    TypeIndex,          // a family of type index variables (Engine/TypeIndices.h)
    NoCallableCode,     // Norbyte's function has no code of its own in this game
    Count
};

// --- how a symbol is found --------------------------------------------------

enum class Find : uint8_t
{
    Nothing,
    ExactName,      // the one symbol with this mangled name
    NamePrefix,     // the one address named by the symbols starting with this
};

struct Named
{
    Find How{ Find::Nothing };
    char const* Symbol{ nullptr };

    constexpr explicit operator bool() const { return How != Find::Nothing; }
};

constexpr Named Exact(char const* symbol) { return { Find::ExactName, symbol }; }
constexpr Named Prefix(char const* symbol) { return { Find::NamePrefix, symbol }; }

// What a call calls: a function of the same image, or an imported function
// reached through its stub (by the import's name, e.g. "_OSSpinLockLock").
struct Callee
{
    Named Function{};
    char const* Import{ nullptr };

    constexpr explicit operator bool() const { return Function || Import; }
};

constexpr Callee Calls(Named function) { return { function, nullptr }; }
constexpr Callee CallsImport(char const* import) { return { {}, import }; }

// A place inside a named function (the host), found from its code.
//
// With an anchor, the host must hold exactly one call to the anchor's callee;
// a pattern, when given, must then match around that call, with Pattern[AnchorAt]
// on the call. Without an anchor the pattern must match exactly once in the
// host. The host's whole shape is compared as for any function, so the place
// is taken from code that is known.
struct Derivation
{
    enum class Take : uint8_t
    {
        Site,           // the address of Pattern[At], or of the anchor call when there is no pattern
        PageAddress,    // the address formed by the adrp at Pattern[At] and the instruction after it
    };

    Named Host;
    CodeShape HostShape;
    Callee Anchor{};
    std::span<MaskedWord const> Pattern{};
    uint8_t AnchorAt{ 0 };
    Take What{ Take::Site };
    uint8_t At{ 0 };
};

// --- argument passing -------------------------------------------------------
//
// The format the binding layer acts on. An argument is in a general register
// or in the caller's stack block (at the call: [sp + StackOffset], packed at
// natural size and alignment as Apple's arm64 ABI does). It is passed by
// value, or as a pointer to the thing its type names. `Norbyte` is the
// position of the parameter of Norbyte's prototype that it corresponds to, or
// -1 when Norbyte's prototype has no such parameter.

enum class Place : uint8_t
{
    X0, X1, X2, X3, X4, X5, X6, X7,
    Stack,
};

enum class Passing : uint8_t
{
    Value,          // the register or stack slot holds the value, `Bytes` wide
    Pointer,        // it holds a pointer to the value
    Unread,         // callers pass it, and the function never reads it
};

struct Argument
{
    Place At{ Place::X0 };
    Passing How{ Passing::Value };
    uint8_t Bytes{ 8 };
    int8_t Norbyte{ -1 };
    uint16_t StackOffset{ 0 };
    char const* Type{ nullptr };        // the type on macOS, as text
};

enum class Returns : uint8_t
{
    Nothing,
    W0,             // up to 32 bits in w0
    X0,             // 64 bits in x0
    X0W1,           // a two-register value: x0 and w1
    V0V1,           // two floats in s0 and s1
    IndirectX8,     // the caller passes the address of the result in x8
    IndirectX0,     // the address of the result is the first ordinary argument (x0)
};

constexpr size_t kMaxArguments = 18;

struct Signature
{
    Argument Args[kMaxArguments]{};
    uint8_t Count{ 0 };
    Returns Result{ Returns::Nothing };
    char const* ResultType{ nullptr };
    // How this differs from the prototype Norbyte declares, or nullptr when a
    // call through Norbyte's prototype passes the same registers.
    char const* Differs{ nullptr };

    constexpr std::span<Argument const> Arguments() const { return { Args, Count }; }
};

constexpr Argument Value(Place at, uint8_t bytes, int8_t norbyte, char const* type) { return { at, Passing::Value, bytes, norbyte, 0, type }; }
constexpr Argument Pointer(Place at, int8_t norbyte, char const* type) { return { at, Passing::Pointer, 8, norbyte, 0, type }; }
constexpr Argument Unread(Place at, int8_t norbyte, char const* type) { return { at, Passing::Unread, 8, norbyte, 0, type }; }
constexpr Argument StackValue(uint16_t offset, uint8_t bytes, int8_t norbyte, char const* type) { return { Place::Stack, Passing::Value, bytes, norbyte, offset, type }; }
constexpr Argument StackPointer(uint16_t offset, int8_t norbyte, char const* type) { return { Place::Stack, Passing::Pointer, 8, norbyte, offset, type }; }

constexpr Signature Takes(std::initializer_list<Argument> args, Returns result = Returns::Nothing, char const* resultType = nullptr,
    char const* differs = nullptr)
{
    Signature s;
    for (auto const& arg : args) s.Args[s.Count++] = arg;
    s.Result = result;
    s.ResultType = resultType;
    s.Differs = differs;
    return s;
}

// --- further checks ---------------------------------------------------------

// A value or a branch target read back from one instruction of a function
// (the pattern must match exactly once in it), or a count of its calls.
struct Operand
{
    enum class Read : uint8_t
    {
        LoadStoreOffset,    // the byte offset of a load or store
        AddImmediate,       // the immediate of an add or sub
        MoveValue,          // the value of a movz
        BranchTarget,       // where a b or bl goes: must be `Target`
        PointerLoad,        // an adrp and a 64-bit load after it: must load from `Target`
        CallCount,          // how many calls the function holds to `Target` (or to the import `Import` through its
                            // stub): must be `Expected` (no pattern)
        StoreOrder,         // of the function's stores of a pointer to a global (adrp, then a 64-bit str through
                            // it), the one numbered `Expected` from 0 must be to `Target` (no pattern)
    };

    char const* What;                       // what the value is, for the report
    Named In{};                             // the function searched; the entry's own when empty
    std::span<MaskedWord const> Pattern{};
    uint8_t At{ 0 };
    Read How{ Read::LoadStoreOffset };
    uint64_t Expected{ 0 };
    Named Target{};
    char const* Import{ nullptr };          // CallCount: an imported function's name instead of `Target`
};

// One method of a vtable: the function the vtable holds for it, and where.
// For a slot that is replaced or called, `Call` is the slot's argument
// passing. It is compared with the function the vtable holds as far as a
// virtual function allows: an override may ignore an argument, so only a
// register it reads that the slot does not pass, or reads as another kind of
// value, contradicts the statement.
struct VtableMethod
{
    char const* Name;       // Norbyte's name for the slot
    Named Function;         // the function this class's vtable holds there
    uint16_t Offset;        // from the address an object's vtable pointer holds
    Signature const* Call{ nullptr };
};

// For an entry without callable code: where the code went.
struct Absence
{
    char const* Fate{ nullptr };            // what was found about where the code is
    char const* NoSymbol{ nullptr };        // no symbol may start with this: the function would then exist
    std::span<Named const> Hosts{};         // the functions the code was found in; each must exist
    // When something in this image does the missing function's work, it was
    // written from the code of hosts: one shape for each host, recorded from
    // the game build the entry's comment names, which the host must still
    // have; an empty shape for a host that was not read for it. No shapes
    // when nothing was derived from that code.
    std::span<CodeShape const> HostShapes{};

    constexpr bool ReadFromHosts() const
    {
        for (auto const& shape : HostShapes) {
            if (shape != CodeShape{}) return true;
        }
        return false;
    }
    std::span<char const* const> Replacements{};    // the entries (by Name) that give Norbyte's use its place, if any
};

// --- the entry --------------------------------------------------------------

struct Entry
{
    char const* Name;               // Norbyte's name for it, or the macOS name of a place Norbyte has no name for
    Image In{ Image::Game };
    Kind What{ Kind::FunctionToCall };
    char const* Mac{ nullptr };     // what it is in the macOS game, for people
    char const* Use{ nullptr };     // what Norbyte's code does with it

    Named Where{};                              // by name ...
    Derivation const* Derived{ nullptr };       // ... or from a named function's code

    // Functions.
    Signature Call{};
    CodeShape Shape{};              // the whole function, recorded from the game build the entry's comment names
    bool CallsItself{ false };      // a hook on it also runs for the inner call
    Named Caller{};                 // a caller whose stores before the call are compared with the stack block

    // Globals. A pointer is 8 bytes exactly; for an object, `Bytes` is the
    // least distance to the next symbol (0: not compared, as for a derived
    // address inside a block that has one symbol for many variables).
    uint32_t Bytes{ 0 };
    Named Agrees{};                 // a symbol that must name the derived address

    // Vtables.
    std::span<VtableMethod const> Methods{};

    // Type indices: the context, as the demangler prints it. `Where` names
    // the context's state (Engine/TypeIndices.h), 16 bytes of data.
    char const* Context{ nullptr };

    std::span<Operand const> Operands{};

    // Code sites: the instruction word the site is given. The instruction it
    // replaces is the derivation's pattern at the site, which states it in
    // full (a call: the anchor's callee).
    uint32_t Becomes{ 0 };

    Absence Gone{};                 // Kind::NoCallableCode

    // The entry (by Name) this one is of no use without: the function a call
    // site calls, the global a function's first argument comes from. When
    // that entry is refused, this one is refused with it.
    char const* Needs{ nullptr };

    // Resolved, but the binding layer must mind this.
    char const* Caveat{ nullptr };
    // Not settled by reading the game: the entry stays unresolved, for this reason.
    char const* Unsettled{ nullptr };
};

struct Area
{
    char const* Name;
    std::span<Entry const> Entries;
};

// The whole table.
std::span<Area const> EngineTable();

char const* Describe(Image image);
char const* Describe(Kind kind);
char const* Describe(Place place);
char const* Describe(Returns result);

}
