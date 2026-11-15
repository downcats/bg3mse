#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <Layout/OsirisLayout.h>
#include <Platform/CodeShape.h>
#include <Platform/Symbols.h>

// The proofs of Layout/OsirisLayout.h: for each structure of the story
// library, functions of libOsiris.dylib whose code shows its fields. A proof
// names a function by its exact mangled symbol, requires its code to be the
// code that was read (Platform/CodeShape.h), and compares the offsets and
// constants in that code with the constants of Layout/OsirisLayout.h. A
// shape is confirmed only when every proof of it holds; a missing symbol,
// changed code or a differing operand leaves it unconfirmed, and the story
// layer then stays off (Binding/OsirisBinding.h).
//
// The same check runs on the loaded library and on the file mapped from disk.

namespace bg3se::mac::osiris {

enum class Shape : uint8_t
{
    Containers,     // libc++'s list and map as the library walks them
    FunctionTable,  // COsiFunctionMan, COsiFunctionData, COsiFunctionDef, MappingInfo
    TypeTable,      // COsiDIVObjectTypesMan
    Values,         // COsiTypedValueBase, COsiArgumentDesc, CTuple, the small tuple
    Network,        // the factories, CReteNode and its subclasses, CReteDBase, CReteAdaptor
    Goals,          // COsiGoalMan, COsiGoal, COsiFunctionParseData and its list
    Count
};

char const* Describe(Shape shape);

struct FunctionProof
{
    Shape Of;
    char const* Function;       // readable name
    bool Found{ false };
    CodeShape Code{};           // what the library has, when found
    std::string Problem;        // empty when the proof holds
};

struct Shapes
{
    std::vector<FunctionProof> Proofs;

    // Every proof of `shape` holds (and there is at least one).
    bool Confirmed(Shape shape) const;
    bool AllConfirmed() const;
    // The first proof that does not hold, as one line; empty when all hold.
    std::string FirstProblem() const;
};

// Runs every proof on libOsiris.dylib: the loaded library, or the file
// mapped from disk. Reads code only.
Shapes CheckOsirisShapes(GameImage const& osiris);

}
