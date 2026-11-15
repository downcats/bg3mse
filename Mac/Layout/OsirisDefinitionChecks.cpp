#include <stdafx.h>
#include <GameDefinitions/Osiris.h>

#include "OsirisLayout.h"

// offsetof on Norbyte's structures, which are not standard-layout (most have
// a base class): what his property maps do throughout, and what clang
// supports for them.
#pragma clang diagnostic ignored "-Winvalid-offsetof"

// What Norbyte's GameDefinitions/Osiris.h must come to in this build for his
// structures to be the story library's: every offset his code reads or
// writes through, against the constants of Layout/OsirisLayout.h, which are
// proven from the library's code. This file has no code.

BEGIN_SE()

namespace shape = mac::osiris;

// --- libc++ containers ---------------------------------------------------------

using CheckedList = List<void*>;
using CheckedListNode = ListNode<void*>;
static_assert(sizeof(CheckedList) == shape::kListBytes);
static_assert(offsetof(CheckedList, Last) == shape::kListPrev && offsetof(CheckedList, First) == shape::kListNext
    && offsetof(CheckedList, Size) == shape::kListSize);
static_assert(offsetof(CheckedListNode, Head) == shape::kListNodePrev && offsetof(CheckedListNode, Next) == shape::kListNodeNext
    && offsetof(CheckedListNode, Item) == shape::kListNodeItem);
// The end node of a list is read as a node up to its second pointer.
static_assert(offsetof(CheckedList, Last) == offsetof(CheckedListNode, Head) && offsetof(CheckedList, First) == offsetof(CheckedListNode, Next));

using CheckedMap = TMap<uint32_t, void*>;
using CheckedMapNode = TMapNode<uint32_t, void*>;
using CheckedNamedNode = TMapNode<OsiString, void*>;
// A TMap is the map without its size, which each structure declares after it.
static_assert(sizeof(CheckedMap) == shape::kTreeSize && sizeof(CheckedMap) + sizeof(uint64_t) == shape::kTreeBytes);
static_assert(offsetof(CheckedMap, First) == shape::kTreeBegin && offsetof(CheckedMap, Root) == shape::kTreeRoot);
static_assert(offsetof(CheckedMapNode, Left) == shape::kTreeNodeLeft && offsetof(CheckedMapNode, Right) == shape::kTreeNodeRight
    && offsetof(CheckedMapNode, Root) == shape::kTreeNodeParent && offsetof(CheckedMapNode, KV) == shape::kTreeNodeValue);
static_assert(sizeof(OsiString) == shape::kStringBytes);
static_assert(offsetof(CheckedNamedNode, KV) == shape::kTreeNodeValue
    && offsetof(CheckedNamedNode, KV) + offsetof(CheckedNamedNode::KeyValuePair, Value) == shape::kTreeNodeValue + shape::kStringBytes);

// --- the tables ------------------------------------------------------------------

static_assert(sizeof(TypeDb<void*>::HashSlot) == shape::kTreeBytes);
static_assert(sizeof(TypeDb<void*>::Hash) == shape::kIndexBuckets * sizeof(TypeDb<void*>::HashSlot));
static_assert(offsetof(TypeDb<void*>, Hash) == 0 && offsetof(TypeDb<void*>, NumItems) == shape::kIndexCount);

static_assert(sizeof(FunctionDb::FunctionIdHashSlot) == shape::kTreeBytes && offsetof(FunctionDb, FunctionIdHash) == shape::kFunctionsById);
static_assert(offsetof(CheckedNamedNode, KV) + offsetof(CheckedNamedNode::KeyValuePair, Value) == shape::kFunctionByNameValue);
static_assert(offsetof(CheckedMapNode, KV) + offsetof(CheckedMapNode::KeyValuePair, Value) == shape::kFunctionByIdValue);

using CheckedTypeNode = TMapNode<OsiString, OsirisTypeInfo>;
static_assert(offsetof(CheckedTypeNode, KV) + offsetof(CheckedTypeNode::KeyValuePair, Value) == shape::kTypeByNameValue);
static_assert(offsetof(OsiTypeDb, Aliases) == shape::kTypeAliases);
static_assert(sizeof(OsiTypeDb::AliasInfo) == shape::kTypeAliasBytes && offsetof(OsiTypeDb::AliasInfo, AliasTypeId) == shape::kTypeAliasOf);

static_assert(offsetof(GoalDb, Goals) == shape::kGoals);

// --- functions -------------------------------------------------------------------

static_assert(offsetof(OsiFunctionDef, Signature) == shape::kFunctionSignature && offsetof(OsiFunctionDef, Node) == shape::kFunctionNode
    && offsetof(OsiFunctionDef, Type) == shape::kFunctionType && offsetof(OsiFunctionDef, Key) == shape::kFunctionKey
    && offsetof(OsiFunctionDef, OsiFunctionId) == shape::kFunctionId);
static_assert(offsetof(FunctionSignature, Name) == shape::kSignatureName && offsetof(FunctionSignature, Params) == shape::kSignatureParams);
static_assert(offsetof(FunctionSignature, OutParamList) + offsetof(FuncSigOutParamList, Params) == shape::kSignatureOutBits
    && offsetof(FunctionSignature, OutParamList) + offsetof(FuncSigOutParamList, Count) == shape::kSignatureOutBytes);
static_assert(offsetof(FunctionParamList, Params) == shape::kListInObject && offsetof(TuplePtrLL, Items) == shape::kListInObject
    && offsetof(RuleActionArguments, Args) == shape::kListInObject && offsetof(RuleActionList, Actions) == shape::kListInObject);
// A value type's list node is the two links and eight bytes, the type first.
static_assert(sizeof(ListNode<FunctionParamDesc>) == shape::kListNodeItem + 8 && offsetof(FunctionParamDesc, Type) == 0);
static_assert(offsetof(MappingInfo, Name) == shape::kMappingName && offsetof(MappingInfo, Id) == shape::kMappingId
    && offsetof(MappingInfo, NumParams) == shape::kMappingParamCount && offsetof(MappingInfo, ParamTypes) == shape::kMappingParamTypes
    && sizeof(MappingInfo) == shape::kMappingBytes);

// --- values ----------------------------------------------------------------------

static_assert(sizeof(TypedValue) == shape::kValueBytes && offsetof(TypedValue, Value) == shape::kValue
    && offsetof(TypedValue, TypeId) == shape::kValueType && offsetof(TypedValue, Index) == shape::kValueIndex
    && offsetof(TypedValue, Flags) == shape::kValueFlags);
static_assert((uint8_t)ValueFlags::IsValid == shape::kValueValid && (uint8_t)ValueFlags::Variable == 3);
// A value made here says it is a parameter: what the list a tuple is inserted
// with holds in the library.
static_assert((uint8_t)ValueFlags::TypedValue == shape::kValueKindParameter && (uint8_t)ValueFlags::SimpleValue == shape::kValueKindTyped);
static_assert(sizeof(OsiArgumentDesc) == shape::kArgumentBytes && offsetof(OsiArgumentDesc, NextParam) == shape::kArgumentNext
    && offsetof(OsiArgumentDesc, Value) == shape::kArgumentValue
    && offsetof(OsiArgumentDesc, Value) + offsetof(OsiArgumentValue, TypeId) == shape::kArgumentType);
static_assert(offsetof(TupleVec, Values) == shape::kTupleValues && offsetof(TupleVec, Size) == shape::kTupleSize);
// The small tuple's members are private: eight values, then the size and
// the capacity, four bytes each, is the one layout of this size.
static_assert(SmallTuple::InlineSize == shape::kSmallTupleInline && shape::kSmallTupleSize == shape::kSmallTupleInline * shape::kValueBytes
    && sizeof(SmallTuple) == shape::kSmallTupleCapacity + 4);

// --- the rete network --------------------------------------------------------------

static_assert(offsetof(NodeDb, Db) + offsetof(OsiSequentialArray<Node*>, Size) == shape::kFactoryCount
    && offsetof(NodeDb, Db) + offsetof(OsiSequentialArray<Node*>, Elements) == shape::kFactoryItems);
static_assert(sizeof(DatabaseDb) == sizeof(NodeDb) && sizeof(AdapterDb) == sizeof(NodeDb));
static_assert(offsetof(Node, Id) == shape::kNodeId && offsetof(Node, Function) == shape::kNodeFunction
    && offsetof(Node, Database) == shape::kNodeDatabase);
static_assert(offsetof(RelNode, Parent) == shape::kUnaryParent && offsetof(RelNode, Adapter) == shape::kUnaryAdaptor);
static_assert(offsetof(JoinNode, Left) == shape::kBinaryLeft && offsetof(JoinNode, Right) == shape::kBinaryRight
    && offsetof(JoinNode, LeftAdapter) == shape::kBinaryLeftAdaptor && offsetof(JoinNode, RightAdapter) == shape::kBinaryRightAdaptor);
static_assert(offsetof(RuleNode, Line) == shape::kRuleLine && offsetof(RuleNode, IsQuery) == shape::kRuleIsQuery
    && offsetof(RuleNode, Variables) == shape::kRuleVariables && offsetof(RuleNode, Calls) == shape::kRuleActions);

static_assert(offsetof(Database, DatabaseId) == shape::kDatabaseId);
static_assert(offsetof(Database, Facts) == shape::kDatabaseFacts && offsetof(Database, ParamTypes) == shape::kDatabaseColumnTypes
    && offsetof(Database, NumParams) == shape::kDatabaseColumns);
static_assert(sizeof(decltype(Database::ParamTypes)::value_type) == shape::kDatabaseColumnTypeBytes);
static_assert(offsetof(ListNode<TupleVec>, Item) + offsetof(TupleVec, Values) == shape::kListNodeItem + shape::kTupleValues);

static_assert(offsetof(Adapter, Id) == shape::kAdaptorId && offsetof(Adapter, VarToColumnMaps) == shape::kAdaptorVarMap
    && offsetof(Adapter, VarToColumnMapCount) == shape::kAdaptorVarMap + shape::kTreeSize
    && offsetof(Adapter, ColumnToVarMaps) == shape::kAdaptorColumns && offsetof(Adapter, Constants) == shape::kAdaptorConstants);

// A node's vtable, as NodeVMT lays the slots out.
static_assert(offsetof(NodeVMT, IsDataNode) == shape::kSlotIsDataNode && offsetof(NodeVMT, IsValid) == shape::kSlotIsValid
    && offsetof(NodeVMT, PushDownTuple) == shape::kSlotPushDownTuple && offsetof(NodeVMT, PushDownTupleDelete) == shape::kSlotPushDownTupleDelete
    && offsetof(NodeVMT, InsertTuple) == shape::kSlotInsertTuple && offsetof(NodeVMT, DeleteTuple) == shape::kSlotDeleteTuple
    && offsetof(NodeVMT, CallQuery) == shape::kSlotCallQuery && offsetof(NodeVMT, GetQueryName) == shape::kSlotGetQueryName);

// --- goals and rule actions --------------------------------------------------------

static_assert(offsetof(Goal, InitCalls) == shape::kGoalInit && offsetof(Goal, ExitCalls) == shape::kGoalExit
    && offsetof(Goal, Id) == shape::kGoalId && offsetof(Goal, Name) == shape::kGoalName);
static_assert(offsetof(RuleActionNode, FunctionName) == shape::kActionFunction && offsetof(RuleActionNode, Arguments) == shape::kActionArguments
    && offsetof(RuleActionNode, GoalIdOrDebugHook) == shape::kActionGoalOrHook);

// --- text ------------------------------------------------------------------------

// The library's paths and its log are wide strings of 32-bit characters, as
// wchar_t is in this build: nothing is converted on the way in.
static_assert(sizeof(wchar_t) == 4);

// --- the block of functions the game registers -----------------------------------

static_assert(offsetof(DivFunctions, Call) == shape::kDivCall && offsetof(DivFunctions, Query) == shape::kDivQuery
    && offsetof(DivFunctions, ErrorMessage) == shape::kDivErrorMessage && offsetof(DivFunctions, Assert) == shape::kDivAssert);

END_SE()
