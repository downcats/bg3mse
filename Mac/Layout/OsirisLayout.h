#pragma once

#include <cstddef>
#include <cstdint>

// The shapes of the story library's data in the Mac game: what Norbyte's
// GameDefinitions/Osiris.h describes, as libOsiris.dylib has it.
//
// The library is built with libc++, where the Windows one is built with
// MSVC's standard library, so the containers inside its structures are
// libc++'s: a map is three words with its end node inside it and nodes whose
// children end in null, a list keeps its end node inside it, a string is 24
// bytes. Norbyte's List, TMap and the structures built on them have these
// shapes outside MSVC. What does not hold a container has the Windows shape,
// with two exceptions that are stated below (the element of a database's
// column types, the order of four virtual functions of a node).
//
// Every offset here was read from the library's code, and each is proven
// against that code before the extender's story support is switched on
// (CheckOsirisShapes, Layout/OsirisShapes.h): a function named by its exact
// symbol, its code required to be the code that was read, and the offsets in
// that code compared with these constants. Norbyte's definitions are
// compared with the same constants at compile time
// (Layout/OsirisDefinitionChecks.cpp), so the shape is stated once.

namespace bg3se::mac::osiris {

// --- libc++ containers --------------------------------------------------------

// std::list: its end node, then the size.
inline constexpr size_t kListPrev = 0x00, kListNext = 0x08, kListSize = 0x10, kListBytes = 0x18;
inline constexpr size_t kListNodePrev = 0x00, kListNodeNext = 0x08, kListNodeItem = 0x10;

// std::map and std::set: the first node, the end node (whose left child is
// the root) and the size. A node's value follows its colour byte, aligned.
inline constexpr size_t kTreeBegin = 0x00, kTreeRoot = 0x08, kTreeSize = 0x10, kTreeBytes = 0x18;
inline constexpr size_t kTreeNodeLeft = 0x00, kTreeNodeRight = 0x08, kTreeNodeParent = 0x10, kTreeNodeValue = 0x20;

inline constexpr size_t kStringBytes = 0x18;

// --- the tables with 1023 buckets (CSearchIndex) --------------------------------

inline constexpr uint32_t kIndexBuckets = 1023;     // (1 << 10) - 1, which is how the library's code multiplies by it
// The count of items follows the buckets, and what a table adds follows that.
inline constexpr size_t kIndexCount = kIndexBuckets * kTreeBytes;
inline constexpr size_t kIndexEnd = kIndexCount + 8;

// COsiFunctionMan (FunctionDb): by name, then by id.
inline constexpr size_t kFunctionsById = kIndexEnd;
inline constexpr size_t kFunctionByNameValue = kTreeNodeValue + kStringBytes;
inline constexpr size_t kFunctionByIdValue = kTreeNodeValue + 8;

// The hash a function's bucket is chosen by: over the name's characters,
// hash = (c | 0x20) + 129 * (hash % 4294967), then plus the number of
// parameters. Norbyte's FunctionNameHash is the same.
inline constexpr uint32_t kNameHashCaseBit = 0x20, kNameHashShift = 7, kNameHashModulus = 4294967;

// COsiDIVObjectTypesMan (OsiTypeDb): the alias table is a vector of
// { type, alias of }, 16 bits each.
inline constexpr size_t kTypeByNameValue = kTreeNodeValue + kStringBytes;
inline constexpr size_t kTypeAliases = kIndexEnd;
inline constexpr size_t kTypeAliasBytes = 4, kTypeAliasOf = 2;

// COsiGoalMan (GoalDb): the goals by id.
inline constexpr size_t kGoals = kIndexEnd;

// --- functions ------------------------------------------------------------------

// COsiFunctionData (OsiFunctionDef).
inline constexpr size_t kFunctionSignature = 0x18, kFunctionNode = 0x20, kFunctionType = 0x24, kFunctionKey = 0x28, kFunctionId = 0x38;
// COsiFunctionDef (FunctionSignature): the out-parameter bits are a pointer
// and a count of bytes, the first parameter in the top bit.
inline constexpr size_t kSignatureName = 0x08, kSignatureParams = 0x10, kSignatureOutBits = 0x18, kSignatureOutBytes = 0x20;
// The lists with a vtable pointer before them: COsiValueTypeList
// (FunctionParamList), COsipParameterList (TuplePtrLL, RuleActionArguments),
// CpOsiFunctionParseDataList (RuleActionList).
inline constexpr size_t kListInObject = 0x08;
// MappingInfo.
inline constexpr size_t kMappingName = 0x00, kMappingId = 0x08, kMappingParamCount = 0x0c, kMappingParamTypes = 0x10, kMappingBytes = 0x18;

// --- values ---------------------------------------------------------------------

// COsiTypedValueBase (TypedValue).
inline constexpr size_t kValue = 0x00, kValueType = 0x08, kValueIndex = 0x0a, kValueFlags = 0x0b, kValueBytes = 0x10;
inline constexpr uint8_t kValueKindMask = 0x07, kValueValid = 0x08, kValueNoIndex = 0xff;
inline constexpr uint8_t kValueKindTyped = 1, kValueKindParameter = 2;
// COsiArgumentDesc.
inline constexpr size_t kArgumentNext = 0x00, kArgumentValue = 0x08, kArgumentType = 0x10, kArgumentBytes = 0x18;
// CTuple (TupleVec). The library's tuple has a 32-bit word after these two
// that Norbyte's does not declare; his code only reads tuples.
inline constexpr size_t kTupleValues = 0x00, kTupleSize = 0x08;
// COsiVarIDValuePairList, COsiColumnIdxValuePairList (SmallTuple): eight
// values in place, or a pointer once the capacity is above that.
inline constexpr size_t kSmallTupleInline = 8, kSmallTupleSize = 0x80, kSmallTupleCapacity = 0x84;

// --- the rete network -------------------------------------------------------------

// The three factories (TypedDb): a count and a vector of pointers.
inline constexpr size_t kFactoryCount = 0x00, kFactoryItems = 0x08;
// CReteNode (Node).
inline constexpr size_t kNodeId = 0x08, kNodeFunction = 0x10, kNodeDatabase = 0x18;
// CReteUnaryNode (RelNode), CReteBinaryNode (JoinNode), CReteRuleActionPart (RuleNode).
inline constexpr size_t kUnaryParent = 0x28, kUnaryAdaptor = 0x2c;
inline constexpr size_t kBinaryLeft = 0x28, kBinaryRight = 0x2c, kBinaryLeftAdaptor = 0x30, kBinaryRightAdaptor = 0x34;
inline constexpr size_t kRuleLine = 0x44, kRuleIsQuery = 0x48, kRuleVariables = 0x50, kRuleActions = 0x58;
// CReteDBase (Database). The column types are 16 bits each.
inline constexpr size_t kDatabaseId = 0x00, kDatabaseFacts = 0x10, kDatabaseColumnTypes = 0x28, kDatabaseColumns = 0x40;
inline constexpr size_t kDatabaseColumnTypeBytes = 2;
// CReteAdaptor (Adapter): a map whose size is Norbyte's VarToColumnMapCount.
inline constexpr size_t kAdaptorId = 0x00, kAdaptorVarMap = 0x08, kAdaptorColumns = 0x20, kAdaptorConstants = 0x38;

// The slots of a node's vtable that Norbyte's code calls or replaces, from the
// address an object's vtable pointer holds. Which function each node class
// has in each is the engine table's to prove (Engine/Entries/Osiris.cpp): the
// binding compares these with the table before a vtable is handed over.
inline constexpr size_t kSlotIsDataNode = 0x20, kSlotIsValid = 0x28;
inline constexpr size_t kSlotPushDownTuple = 0x58, kSlotPushDownTupleDelete = 0x60, kSlotInsertTuple = 0x68, kSlotDeleteTuple = 0x70;
// The query nodes only.
inline constexpr size_t kSlotCallQuery = 0xc0, kSlotGetQueryName = 0xc8;

// --- goals and rule actions -------------------------------------------------------

// COsiGoal (Goal).
inline constexpr size_t kGoalInit = 0x00, kGoalExit = 0x08, kGoalId = 0x10, kGoalName = 0x18;
// COsiFunctionParseData (RuleActionNode).
inline constexpr size_t kActionFunction = 0x00, kActionArguments = 0x08, kActionGoalOrHook = 0x14;

// --- the block of functions the game registers ------------------------------------

// CDIV's accessors each jump through one slot of the block (DivFunctions):
// the engine table's entry for the block reads the four offsets from them
// (Engine/Entries/Osiris.cpp).
inline constexpr size_t kDivCall = 0x08, kDivQuery = 0x10, kDivErrorMessage = 0x18, kDivAssert = 0x20;

}
