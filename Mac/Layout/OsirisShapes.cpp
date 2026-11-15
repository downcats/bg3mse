#include <Layout/OsirisShapes.h>

#include <Layout/CodeProof.h>

#include <iterator>
#include <span>

namespace bg3se::mac::osiris {

namespace {

using proof::Code;
using proof::Expect;
using proof::Operand;

struct Proof
{
    Shape Of;
    char const* Function;
    char const* Symbol;
    CodeShape Expected;
    std::span<Expect const> Operands;
};

// A negative offset as an operand's value.
constexpr uint64_t Back(size_t from, size_t to)
{
    return (uint64_t)((int64_t)to - (int64_t)from);
}

// What follows from the constants the proofs below read back, each stated
// with the proof that carries it:
// a list is its three words (the database's column types follow its facts);
static_assert(kDatabaseColumnTypes - kDatabaseFacts == kListBytes && kListSize + 8 == kListBytes);
// a map is its three words (the by-id index is stepped through in map sizes);
static_assert(kTreeSize + 8 == kTreeBytes);
// a string is what lies between a by-name node's key and its value;
static_assert(kFunctionByNameValue - kTreeNodeValue == kStringBytes && kTypeByNameValue - kTreeNodeValue == kStringBytes);
// the count of a table follows its buckets, which the code multiplies by as (1 << 10) - 1;
constexpr uint64_t kIndexBucketShift = 10;
static_assert(kIndexBuckets == (1u << kIndexBucketShift) - 1 && kIndexCount == kIndexBuckets * kTreeBytes && kIndexEnd == kIndexCount + 8);
// an alias entry and a value are the sizes the code shifts by;
constexpr uint64_t kTypeAliasShift = 2, kValueShift = 4;
static_assert(kTypeAliasBytes == 1u << kTypeAliasShift && kValueBytes == 1u << kValueShift);
// and a pair of 32-bit handles stored with one instruction lies four bytes apart.
static_assert(kUnaryAdaptor == kUnaryParent + 4 && kBinaryRight == kBinaryLeft + 4 && kBinaryRightAdaptor == kBinaryLeftAdaptor + 4);

// ---------------------------------------------------------------------------
// The function table, and the map and the list through it
// ---------------------------------------------------------------------------

// COsiFunctionMan::pFunctionData(unsigned int): after a look in the set of
// all ids, the by-id map of bucket id % 1023 is searched from its root (the
// second word of the map): the key at +0x20 of a node, right child at +8,
// left at +0, until a child is null; the function is the node's value at +0x28.
constexpr Expect kFunctionById[] = {
    { 4, Operand::Move, kFunctionsById },
    { 42, Operand::ShiftLeft, kIndexBucketShift },
    { 44, Operand::Move, kTreeBytes },
    { 46, Operand::Unscaled, kTreeRoot },
    { 49, Operand::Offset, kTreeNodeValue },
    { 50, Operand::Add, kTreeNodeRight },
    { 54, Operand::Offset, kTreeNodeLeft },
    { 69, Operand::Offset, kFunctionByIdValue },
};

// COsiFunctionMan::SetHandle(unsigned int, COsiFunctionData*): a new node of
// the by-id map, 0x30 bytes with the id at +0x20 and the function at +0x28,
// is linked under its parent (+0x10 of a node), the map's size (its third
// word) goes up, and the function is given its id.
constexpr Expect kFunctionSetHandle[] = {
    { 66, Operand::Move, kFunctionByIdValue + 8 },
    { 69, Operand::Offset, kTreeNodeValue },
    { 70, Operand::Offset, kFunctionByIdValue },
    { 72, Operand::Offset, kTreeNodeParent },
    { 83, Operand::Offset, kTreeSize },
    { 85, Operand::Offset, kTreeSize },
    { 138, Operand::Offset, kFunctionId },
};

// COsiris::Event(unsigned int, COsiArgumentDesc*): the function found by its
// id has its node's handle at +0x20, which is compared with the node
// factory's count, and its signature at +0x18.
constexpr Expect kEventFunction[] = {
    { 95, Operand::Offset, kFunctionByIdValue },
    { 113, Operand::Offset, kFunctionNode },
    { 117, Operand::Offset, kFactoryCount },
    { 122, Operand::Offset, kFunctionSignature },
};

// COsiFunctionMan::pFunctionData(CKey const&): the by-name map of the key's
// bucket, its nodes' keys (a string at +0x20) compared without case, the
// function at +0x38.
constexpr Expect kFunctionByName[] = {
    { 15, Operand::Move, kTreeBytes },
    { 17, Operand::Unscaled, kTreeRoot },
    { 27, Operand::Unscaled, kTreeNodeValue },
    { 31, Operand::CallsImport, 0, "_strcasecmp" },
    { 32, Operand::Add, kTreeNodeRight },
    { 60, Operand::Offset, kFunctionByNameValue },
};

// COsiFunctionMan::FastSearchKey(char const*, unsigned char) const: the hash
// of a function's name, without regard to case, plus its parameter count.
// The recorded code holds the modulus whole; its low half, the multiplier's
// shift and the case bit are read back.
constexpr Expect kFunctionNameHash[] = {
    { 6, Operand::Move, kNameHashModulus & 0xffff },
    { 11, Operand::ShiftLeft, kNameHashShift },
    { 12, Operand::Mask, kNameHashCaseBit },
};

// COsiris::GetFunctionMappings(MappingInfo**, unsigned int*): walks every
// by-name map in order (a node's right child and parent), and for each
// function of type 1 to 3 fills a MappingInfo from the function's signature,
// id and parameter types, the last by walking the signature's list of value
// types from the end node inside it.
constexpr Expect kFunctionMappings[] = {
    { 12, Operand::Move, kIndexCount },
    { 88, Operand::Offset, kFunctionByNameValue },
    { 89, Operand::Offset, kFunctionType },
    { 93, Operand::Offset, kFunctionSignature },
    { 94, Operand::Offset, kSignatureName },
    { 95, Operand::Offset, kMappingName },
    { 96, Operand::Offset, kFunctionId },
    { 97, Operand::Offset, kMappingId },
    { 99, Operand::Offset, kSignatureParams },
    { 101, Operand::Offset, kListInObject + kListSize },
    { 103, Operand::Offset, kMappingParamCount },
    { 114, Operand::Offset, kMappingParamTypes },
    { 121, Operand::Add, kListInObject },
    { 122, Operand::Offset, kListInObject + kListNext },
    { 125, Operand::ByteOffset, kListNodeItem },
    { 128, Operand::Offset, kListNodeNext },
    { 131, Operand::Add, kMappingBytes },
    { 137, Operand::Offset, kTreeNodeRight },
    { 143, Operand::Offset, kTreeNodeParent },
    { 171, Operand::PairOffset, kFunctionKey },
};

// COsiFunctionDef::COsiFunctionDef(char const*, COsipParameterList*): the
// name, the list of value types (one node of 0x18 bytes for each parameter,
// its type taken from the parameter's value) and the out-parameter bits.
constexpr Expect kSignatureConstructor[] = {
    { 13, Operand::Unscaled, kSignatureParams },
    { 15, Operand::Unscaled, kSignatureOutBits },
    { 16, Operand::Offset, kSignatureOutBytes },
    { 32, Operand::Offset, kSignatureName },
    { 94, Operand::Offset, kListNodeItem },
    { 95, Operand::Offset, kValueType },
    { 99, Operand::Move, kListNodeItem + 8 },
    { 103, Operand::Offset, kListNodeItem },
};

// COsiBitVector::IsSet(unsigned int) const: byte index / 8 of the bits the
// first word points to, the bit counted from the top.
constexpr Expect kBitVectorIsSet[] = {
    { 0, Operand::Offset, 0 },
    { 2, Operand::ShiftRight, 3 },
    { 3, Operand::Mask, 7 },
    { 6, Operand::ShiftRight, 7 },
};

// ---------------------------------------------------------------------------
// The type table
// ---------------------------------------------------------------------------

// COsiTypedValueBase::SetValueType(TOsiValueType): follows the aliases of
// the value's type through the type table's vector of 4-byte entries, the
// alias in the second half of an entry; then clears the valid bit.
constexpr Expect kSetValueType[] = {
    { 7, Operand::Offset, kValueType },
    { 12, Operand::Offset, kValue },
    { 16, Operand::ByteOffset, kValueFlags },
    { 17, Operand::Mask, 0xffffffffu & ~(uint32_t)kValueValid },
    { 26, Operand::Offset, kTypeAliases },
    { 30, Operand::Offset, kTypeAliases + 8 },
    { 39, Operand::ShiftRight, kTypeAliasShift },
    { 48, Operand::Offset, kTypeAliasOf },
};

// COsiDIVObjectTypesMan::Type(CKey const&): the by-name map of the key's
// bucket, a node's key (a string) at +0x20 and the type after it. The keys
// are compared as bytes here, where the function table compares without case.
constexpr Expect kTypeByName[] = {
    { 16, Operand::Move, kTreeBytes },
    { 18, Operand::Unscaled, kTreeRoot },
    { 32, Operand::Unscaled, kTreeNodeValue },
    { 40, Operand::CallsImport, 0, "_memcmp" },
    { 46, Operand::Add, kTreeNodeRight },
    { 85, Operand::Offset, kTypeByNameValue },
};

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

// COsiTypedValueBase::COsiTypedValueBase(TOsiValueSpecializationType): type
// 0 at +8, no index (0xff) at +0xa, the kind in the low bits of +0xb.
constexpr Expect kValueConstructor[] = {
    { 2, Operand::Offset, kValueType },
    { 2, Operand::StoresZero },
    { 3, Operand::Move, kValueNoIndex },
    { 4, Operand::ByteOffset, kValueIndex },
    { 5, Operand::Mask, kValueKindMask },
    { 6, Operand::ByteOffset, kValueFlags },
};

// COsiParameter::COsiParameter(): type 0, no index, kind 2. A value Norbyte's
// code makes for a tuple it inserts says the same.
constexpr Expect kParameterConstructor[] = {
    { 2, Operand::Move, ((uint64_t)kValueKindParameter << 24) | ((uint64_t)kValueNoIndex << 16) },
    { 3, Operand::Offset, kValueType },
};

// COsiTypedValueBase::IsValid() const: bit 3 of the flags.
constexpr Expect kValueIsValid[] = {
    { 3, Operand::ByteOffset, kValueFlags },
    { 4, Operand::ShiftRight, 3 },
};

// COsiTypedValueBase::pArgumentDesc() const: a new argument of 0x18 bytes,
// its type at +0x10 and its value at +8.
constexpr Expect kValueToArgument[] = {
    { 12, Operand::Move, kArgumentBytes },
    { 46, Operand::Offset, kArgumentType },
    { 47, Operand::Offset, kArgumentValue },
};

// COsipParameterList::COsipParameterList(COsiArgumentDesc*): an empty list
// (its end node at +8 pointing at itself, size 0), then one parameter for
// each argument of the chain: type at +0x10, value at +8, next at +0.
constexpr Expect kParametersFromArguments[] = {
    { 9, Operand::Add, kListInObject },
    { 10, Operand::PairOffset, kListInObject + kListNext },
    { 22, Operand::Offset, kArgumentType },
    { 100, Operand::Offset, kArgumentValue },
    { 205, Operand::Offset, kArgumentNext },
};

// COsiVarIDValuePairList::Value(unsigned char): the values are in place while
// the capacity at +0x84 is at most 8, else behind the pointer at +0; the
// count is at +0x80, a value is 0x10 bytes and its index is at +0xa.
constexpr Expect kSmallTupleValue[] = {
    { 2, Operand::Offset, kSmallTupleCapacity },
    { 3, Operand::Add, kSmallTupleInline },
    { 5, Operand::Offset, kSmallTupleSize },
    { 7, Operand::ShiftLeftBy, kValueShift },
    { 8, Operand::ByteOffset, kValueIndex },
    { 17, Operand::Offset, 0 },
};

// ---------------------------------------------------------------------------
// The rete network
// ---------------------------------------------------------------------------

// CReteNodeFactory::CreateReteEvent(): a new node with no function (+0x10)
// and no database (+0x18) is added to the vector at +8 of the factory, the
// count at +0 goes up, and the new count is the node's id at +8.
constexpr Expect kCreateEventNode[] = {
    { 14, Operand::Offset, kNodeFunction },
    { 15, Operand::Offset, kNodeDatabase },
    { 25, Operand::Add, kFactoryItems },
    { 28, Operand::Offset, kFactoryCount },
    { 31, Operand::Offset, kNodeId },
};

// CReteAdaptorFactory::CreateReteAdaptor(PCReteAdaptor): the same for an
// adaptor, whose id is at +0.
constexpr Expect kCreateAdaptor[] = {
    { 20, Operand::Add, kFactoryItems },
    { 23, Operand::Offset, kFactoryCount },
    { 26, Operand::Offset, kAdaptorId },
};

// CReteDBaseFactory::CreateReteDBase(PCReteDBase): and for a database.
constexpr Expect kCreateDatabase[] = {
    { 20, Operand::Add, kFactoryItems },
    { 23, Operand::Offset, kFactoryCount },
    { 26, Operand::Offset, kDatabaseId },
};

// CReteNode::pDBase(): a node's database is the handle at +0x18.
constexpr Expect kNodeDatabaseRead[] = {
    { 0, Operand::Offset, kNodeDatabase },
};

// CReteDIVQuery::_ExecuteQuery(COsiArgumentDesc*): a node's function is the
// pointer at +0x10; the query is made with that function's id, and its
// arguments are checked against that function's signature.
constexpr Expect kNodeFunctionRead[] = {
    { 12, Operand::Offset, kNodeFunction },
    { 13, Operand::Offset, kFunctionId },
    { 17, Operand::Offset, kNodeFunction },
    { 18, Operand::Offset, kFunctionSignature },
};

// CReteStartNode::Add(COsipParameterList*), a node's InsertTuple: makes a
// tuple of as many values as the list at +8 of the parameters has (each a
// typed value of 0x10 bytes, kind 1, no index), assigns each from the
// parameter the list's node points to, finds the node's database through the
// factory (count at +0, pointers at +8) and inserts unless the database has
// no columns (the byte at +0x40).
constexpr Expect kStartNodeAdd[] = {
    { 11, Operand::ByteOffset, kListInObject + kListSize },
    { 14, Operand::ShiftLeftBy, kValueShift },
    { 23, Operand::Move, ((uint64_t)kValueKindTyped << 24) | ((uint64_t)kValueNoIndex << 16) },
    { 27, Operand::Offset, kValueType },
    { 38, Operand::Add, kListInObject },
    { 39, Operand::Offset, kListInObject + kListNext },
    { 56, Operand::Offset, kListNodeItem },
    { 59, Operand::Offset, kListNodeNext },
    { 69, Operand::Offset, kFactoryCount },
    { 73, Operand::Offset, kFactoryItems },
    { 89, Operand::ByteOffset, kDatabaseColumns },
};

// CReteDBase::_insert(CTuple&&): a new fact, a list node of 0x28 bytes with
// the tuple's values and size at +0x10 and +0x18, is linked before the end
// node of the list at +0x10 of the database, whose size at +0x20 goes up.
constexpr Expect kDatabaseInsert[] = {
    { 8, Operand::ByteOffset, kDatabaseColumns },
    { 16, Operand::Move, kListNodeItem + 0x18 },
    { 22, Operand::Offset, kListNodeItem + kTupleValues },
    { 24, Operand::Offset, kListNodeItem + kTupleSize },
    { 29, Operand::Unscaled, kDatabaseFacts + kListPrev },
    { 30, Operand::PairOffset, kListNodePrev },
    { 31, Operand::Offset, kListNodeNext },
    { 33, Operand::Offset, kDatabaseFacts + kListSize },
};

// CReteDBase::_CommonPartOfConstructors(COsiValueTypeList*): one column type
// is pushed to the vector at +0x28 for each value type of the list ...
constexpr char kPushColumnType[] = "__ZNSt3__16vectorI13TOsiValueTypeN3osi12StlAllocatorIS1_EEE9push_backB8ne180100ERKS1_";
constexpr Expect kDatabaseColumnTypesFilled[] = {
    { 12, Operand::Add, kDatabaseColumnTypes },
    { 15, Operand::Calls, 0, kPushColumnType },
};
// ... and that push stores 16 bits and moves on by two bytes.
constexpr Expect kPushColumnTypeStep[] = {
    { 11, Operand::Unscaled, kDatabaseColumnTypeBytes },
};

// CReteAdaptor::CReteAdaptor(COsiSmartBuf*): from the address of the column
// vector at +0x20, an empty map at +8 (its first node is its end node at
// +0x10), and constants at +0x38 with room for 8 in place.
constexpr char kReadConstants[] = "__ZN26COsiColumnIdxValuePairList4ReadEP12COsiSmartBuf";
constexpr Expect kAdaptorConstructor[] = {
    { 12, Operand::Unscaled, kAdaptorColumns },
    { 14, Operand::Unscaled, Back(kAdaptorColumns, kAdaptorVarMap + kTreeRoot) },
    { 17, Operand::Unscaled, Back(kAdaptorColumns, kAdaptorVarMap + kTreeBegin) },
    { 18, Operand::Add, kAdaptorConstants - kAdaptorColumns },
    { 24, Operand::Move, kSmallTupleInline },
    { 25, Operand::Offset, kAdaptorConstants + kSmallTupleCapacity - kAdaptorColumns },
    { 27, Operand::Calls, 0, kReadConstants },
};

// CReteAdaptor::pColumns() const: reads the column vector's two ends.
constexpr Expect kAdaptorColumnsRead[] = {
    { 20, Operand::PairOffset, kAdaptorColumns },
};

// The constructors of the node classes with a parent, with two parents, and
// of a rule: where each stores what it is given.
constexpr Expect kUnaryNodeConstructor[] = {
    { 5, Operand::PairOffset, kUnaryParent },
};
constexpr Expect kBinaryNodeConstructor[] = {
    { 5, Operand::PairOffset, kBinaryLeft },
    { 6, Operand::PairOffset, kBinaryLeftAdaptor },
};
constexpr Expect kRuleNodeConstructor[] = {
    { 10, Operand::PairOffset, kUnaryParent },
    { 16, Operand::Offset, kRuleLine },
    { 17, Operand::ByteOffset, kRuleIsQuery },
    { 19, Operand::Offset, kRuleActions },
    { 70, Operand::Unscaled, kRuleVariables },
};

// ---------------------------------------------------------------------------
// Goals and rule actions
// ---------------------------------------------------------------------------

// COsiGoalMan::pGoal(int): the vector of goals after the table's count.
constexpr Expect kGoalById[] = {
    { 4, Operand::Offset, kGoals + 8 },
    { 5, Operand::Offset, kGoals },
};

// COsiris::InitGame(): for each goal of that vector, its name and id for the
// log, then every action of the list its first word points to is fired.
constexpr char kFire[] = "__ZN21COsiFunctionParseData4FireEv";
constexpr Expect kInitGameGoals[] = {
    { 183, Operand::Offset, kGoalName },
    { 184, Operand::Offset, kGoalId },
    { 211, Operand::Offset, kGoalInit },
    { 212, Operand::Add, kListInObject },
    { 213, Operand::Offset, kListInObject + kListNext },
    { 222, Operand::Offset, kGoals + 8 },
};

// COsiGoal::SetCompleted(): the same with the list at +8, the exit actions.
constexpr Expect kGoalCompleted[] = {
    { 25, Operand::Offset, kGoalName },
    { 26, Operand::Offset, kGoalId },
    { 58, Operand::Offset, kGoalExit },
    { 59, Operand::Add, kListInObject },
    { 60, Operand::Offset, kListInObject + kListNext },
};

// CpOsiFunctionParseDataList::Fire(): walks the list at +8 and fires the
// action each node points to.
constexpr Expect kActionListFire[] = {
    { 3, Operand::Add, kListInObject },
    { 4, Operand::Offset, kListInObject + kListNext },
    { 7, Operand::Offset, kListNodeItem },
    { 8, Operand::Calls, 0, kFire },
    { 9, Operand::Offset, kListNodeNext },
};

// COsiFunctionParseData::Fire(): the goal id or debug hook at +0x14, the
// function's name at +0 and its arguments at +8 (a parameter list, walked
// for the log), and the name of the goal it completes.
constexpr Expect kActionFire[] = {
    { 9, Operand::Offset, kActionGoalOrHook },
    { 72, Operand::Offset, kActionFunction },
    { 79, Operand::Offset, kActionArguments },
    { 84, Operand::Add, kListInObject },
    { 85, Operand::Offset, kListInObject + kListNext },
    { 88, Operand::Offset, kListNodeItem },
    { 92, Operand::Offset, kListNodeNext },
    { 123, Operand::Offset, kGoalName },
};

// The shapes are those of game build 4.1.1.7631656.
constexpr Proof kProofs[] = {
    { Shape::Containers, "COsiFunctionMan::pFunctionData(unsigned int)", "__ZN15COsiFunctionMan13pFunctionDataEj",
        { 71, 0x200a97a9955dbbf8ull }, kFunctionById },
    { Shape::Containers, "COsiris::GetFunctionMappings(MappingInfo**, unsigned int*)", "__ZN7COsiris19GetFunctionMappingsEPP11MappingInfoPj",
        { 184, 0x879657381f05b0a1ull }, kFunctionMappings },
    { Shape::Containers, "COsiFunctionMan::SetHandle(unsigned int, COsiFunctionData*)", "__ZN15COsiFunctionMan9SetHandleEjP16COsiFunctionData",
        { 243, 0x7f06d69174a6ddb5ull }, kFunctionSetHandle },
    { Shape::Containers, "CReteDBase::_insert(CTuple&&)", "__ZN10CReteDBase7_insertEO6CTuple",
        { 139, 0xf59389267bb1c09aull }, kDatabaseInsert },
    { Shape::Containers, "CpOsiFunctionParseDataList::Fire()", "__ZN26CpOsiFunctionParseDataList4FireEv",
        { 15, 0x0f5504d5c9b550afull }, kActionListFire },

    { Shape::FunctionTable, "COsiFunctionMan::pFunctionData(unsigned int)", "__ZN15COsiFunctionMan13pFunctionDataEj",
        { 71, 0x200a97a9955dbbf8ull }, kFunctionById },
    { Shape::FunctionTable, "COsiFunctionMan::SetHandle(unsigned int, COsiFunctionData*)", "__ZN15COsiFunctionMan9SetHandleEjP16COsiFunctionData",
        { 243, 0x7f06d69174a6ddb5ull }, kFunctionSetHandle },
    { Shape::FunctionTable, "COsiris::Event(unsigned int, COsiArgumentDesc*)", "__ZN7COsiris5EventEjP16COsiArgumentDesc",
        { 379, 0x409742d2b6f56a52ull }, kEventFunction },
    { Shape::FunctionTable, "COsiFunctionMan::pFunctionData(CSearchIndex<...>::CKey const&)",
        "__ZN15COsiFunctionMan13pFunctionDataERKN12CSearchIndexIP16COsiFunctionData10COsiStringLj1023EE4CKeyE",
        { 62, 0x355986771a39a079ull }, kFunctionByName },
    { Shape::FunctionTable, "COsiFunctionMan::FastSearchKey(char const*, unsigned char) const", "__ZNK15COsiFunctionMan13FastSearchKeyEPKch",
        { 20, 0x86b2b36952ddd2a4ull }, kFunctionNameHash },
    { Shape::FunctionTable, "COsiris::GetFunctionMappings(MappingInfo**, unsigned int*)", "__ZN7COsiris19GetFunctionMappingsEPP11MappingInfoPj",
        { 184, 0x879657381f05b0a1ull }, kFunctionMappings },
    { Shape::FunctionTable, "COsiFunctionDef::COsiFunctionDef(char const*, COsipParameterList*)", "__ZN15COsiFunctionDefC2EPKcP18COsipParameterList",
        { 131, 0xae7a908664980161ull }, kSignatureConstructor },
    { Shape::FunctionTable, "COsiBitVector::IsSet(unsigned int) const", "__ZNK13COsiBitVector5IsSetEj",
        { 8, 0x4aaa9ed1d1094981ull }, kBitVectorIsSet },

    { Shape::TypeTable, "COsiTypedValueBase::SetValueType(TOsiValueType)", "__ZN18COsiTypedValueBase12SetValueTypeE13TOsiValueType",
        { 54, 0x5d90470b1449e52full }, kSetValueType },
    { Shape::TypeTable, "COsiDIVObjectTypesMan::Type(CSearchIndex<...>::CKey const&)",
        "__ZN21COsiDIVObjectTypesMan4TypeERKN12CSearchIndexI18OsiDivObjectType_tNSt3__112basic_stringIcNS2_11char_traitsIcEEN3osi12StlAllocatorIcEEEELj1023EE4CKeyE",
        { 87, 0xc12126a768fa35efull }, kTypeByName },

    { Shape::Values, "COsiTypedValueBase::COsiTypedValueBase(TOsiValueSpecializationType)", "__ZN18COsiTypedValueBaseC2E27TOsiValueSpecializationType",
        { 8, 0xe6bf27247d765460ull }, kValueConstructor },
    { Shape::Values, "COsiParameter::COsiParameter()", "__ZN13COsiParameterC2Ev",
        { 5, 0xc2ed279a1470f5c9ull }, kParameterConstructor },
    { Shape::Values, "COsiTypedValueBase::SetValueType(TOsiValueType)", "__ZN18COsiTypedValueBase12SetValueTypeE13TOsiValueType",
        { 54, 0x5d90470b1449e52full }, kSetValueType },
    { Shape::Values, "COsiTypedValueBase::IsValid() const", "__ZNK18COsiTypedValueBase7IsValidEv",
        { 16, 0x9e96b6e883310fc1ull }, kValueIsValid },
    { Shape::Values, "COsiTypedValueBase::pArgumentDesc() const", "__ZNK18COsiTypedValueBase13pArgumentDescEv",
        { 107, 0x43719f5b1cbf21f2ull }, kValueToArgument },
    { Shape::Values, "COsipParameterList::COsipParameterList(COsiArgumentDesc*)", "__ZN18COsipParameterListC2EP16COsiArgumentDesc",
        { 232, 0xb5d320a0216d6341ull }, kParametersFromArguments },
    { Shape::Values, "COsiVarIDValuePairList::Value(unsigned char)", "__ZN22COsiVarIDValuePairList5ValueEh",
        { 36, 0xfedf0ea45ef703b4ull }, kSmallTupleValue },
    { Shape::Values, "CReteStartNode::Add(COsipParameterList*)", "__ZN14CReteStartNode3AddEP18COsipParameterList",
        { 236, 0xf61b9c7f6c2bcf61ull }, kStartNodeAdd },

    { Shape::Network, "CReteNodeFactory::CreateReteEvent()", "__ZN16CReteNodeFactory15CreateReteEventEv",
        { 37, 0x6c360aad8b0323eeull }, kCreateEventNode },
    { Shape::Network, "CReteAdaptorFactory::CreateReteAdaptor(PCReteAdaptor)", "__ZN19CReteAdaptorFactory17CreateReteAdaptorE13PCReteAdaptor",
        { 33, 0x80f805bac85b4753ull }, kCreateAdaptor },
    { Shape::Network, "CReteDBaseFactory::CreateReteDBase(PCReteDBase)", "__ZN17CReteDBaseFactory15CreateReteDBaseE11PCReteDBase",
        { 33, 0x5d2c4b2c624268e7ull }, kCreateDatabase },
    { Shape::Network, "CReteNode::pDBase()", "__ZN9CReteNode6pDBaseEv",
        { 2, 0x47d12fcb2b1932f2ull }, kNodeDatabaseRead },
    { Shape::Network, "CReteDIVQuery::_ExecuteQuery(COsiArgumentDesc*)", "__ZN13CReteDIVQuery13_ExecuteQueryEP16COsiArgumentDesc",
        { 122, 0x5a8154681376e0d9ull }, kNodeFunctionRead },
    { Shape::Network, "CReteStartNode::Add(COsipParameterList*)", "__ZN14CReteStartNode3AddEP18COsipParameterList",
        { 236, 0xf61b9c7f6c2bcf61ull }, kStartNodeAdd },
    { Shape::Network, "CReteDBase::_insert(CTuple&&)", "__ZN10CReteDBase7_insertEO6CTuple",
        { 139, 0xf59389267bb1c09aull }, kDatabaseInsert },
    { Shape::Network, "CReteDBase::_CommonPartOfConstructors(COsiValueTypeList*)", "__ZN10CReteDBase25_CommonPartOfConstructorsEP17COsiValueTypeList",
        { 56, 0xf2b6a65021873ee7ull }, kDatabaseColumnTypesFilled },
    { Shape::Network, "std::vector<TOsiValueType, osi::StlAllocator<TOsiValueType>>::push_back(TOsiValueType const&)", kPushColumnType,
        { 99, 0xa9f8861eaa7a1ad6ull }, kPushColumnTypeStep },
    { Shape::Network, "CReteAdaptor::CReteAdaptor(COsiSmartBuf*)", "__ZN12CReteAdaptorC2EP12COsiSmartBuf",
        { 456, 0x5692f5192a5569f0ull }, kAdaptorConstructor },
    { Shape::Network, "CReteAdaptor::pColumns() const", "__ZNK12CReteAdaptor8pColumnsEv",
        { 59, 0x3db02e422e943a8aull }, kAdaptorColumnsRead },
    { Shape::Network, "CReteUnaryNode::CReteUnaryNode(PCReteNode, PCReteAdaptor)", "__ZN14CReteUnaryNodeC2E10PCReteNode13PCReteAdaptor",
        { 8, 0xd7cba51558ea894bull }, kUnaryNodeConstructor },
    { Shape::Network, "CReteBinaryNode::CReteBinaryNode(PCReteNode, PCReteAdaptor, PCReteNode, PCReteAdaptor)",
        "__ZN15CReteBinaryNodeC2E10PCReteNode13PCReteAdaptorS0_S1_", { 12, 0xcd2fd2fe96375017ull }, kBinaryNodeConstructor },
    { Shape::Network, "CReteRuleActionPart::CReteRuleActionPart(PCReteNode, PCReteAdaptor, CpOsiFunctionParseDataList*)",
        "__ZN19CReteRuleActionPartC2E10PCReteNode13PCReteAdaptorP26CpOsiFunctionParseDataList", { 499, 0x6b9604edf87e3e01ull }, kRuleNodeConstructor },

    { Shape::Goals, "COsiGoalMan::pGoal(int)", "__ZN11COsiGoalMan5pGoalEi",
        { 26, 0x50ac1a6f90c20fbbull }, kGoalById },
    { Shape::Goals, "COsiris::InitGame()", "__ZN7COsiris8InitGameEv",
        { 294, 0xaf0ec04b6455d902ull }, kInitGameGoals },
    { Shape::Goals, "COsiGoal::SetCompleted()", "__ZN8COsiGoal12SetCompletedEv",
        { 197, 0xd196dd975f361c44ull }, kGoalCompleted },
    { Shape::Goals, "CpOsiFunctionParseDataList::Fire()", "__ZN26CpOsiFunctionParseDataList4FireEv",
        { 15, 0x0f5504d5c9b550afull }, kActionListFire },
    { Shape::Goals, "COsiFunctionParseData::Fire()", kFire,
        { 878, 0x01d2411a1b7bb9bbull }, kActionFire },
};

}

char const* Describe(Shape shape)
{
    switch (shape) {
    case Shape::Containers: return "containers";
    case Shape::FunctionTable: return "function table";
    case Shape::TypeTable: return "type table";
    case Shape::Values: return "values";
    case Shape::Network: return "rete network";
    case Shape::Goals: return "goals and actions";
    case Shape::Count: break;
    }
    return "?";
}

bool Shapes::Confirmed(Shape shape) const
{
    bool any = false;
    for (auto const& proof : Proofs) {
        if (proof.Of != shape) continue;
        if (!proof.Problem.empty()) return false;
        any = true;
    }
    return any;
}

bool Shapes::AllConfirmed() const
{
    for (uint8_t shape = 0; shape < (uint8_t)Shape::Count; shape++) {
        if (!Confirmed((Shape)shape)) return false;
    }
    return true;
}

std::string Shapes::FirstProblem() const
{
    for (auto const& proof : Proofs) {
        if (!proof.Problem.empty()) return std::string(Describe(proof.Of)) + ": " + proof.Function + ": " + proof.Problem;
    }
    if (Proofs.empty()) return "no proof was run";
    return {};
}

Shapes CheckOsirisShapes(GameImage const& image)
{
    constexpr size_t count = std::size(kProofs);
    char const* names[count];
    uint64_t vas[count], ends[count];
    for (size_t n = 0; n < count; n++) names[n] = kProofs[n].Symbol;
    image.FindVAs(names, vas);
    image.NextSymbolVAs(vas, ends);

    Shapes shapes;
    for (size_t n = 0; n < count; n++) {
        auto const& proof = kProofs[n];
        FunctionProof result;
        result.Of = proof.Of;
        result.Function = proof.Function;
        if (!vas[n] || vas[n] < image.TextBegin() || ends[n] <= vas[n] || ends[n] > image.TextEnd()) {
            result.Problem = "not in the symbol table under the expected signature";
            shapes.Proofs.push_back(std::move(result));
            continue;
        }
        Code f{ image, vas[n], ends[n] };
        result.Found = true;
        result.Code = ShapeOfRange(image.CodeAt(f.Begin), f.Count());
        if (result.Code != proof.Expected) {
            result.Problem = "its code changed";
        } else {
            for (auto const& operand : proof.Operands) {
                if (!proof::CheckOperand(f, operand, result.Problem)) break;
            }
        }
        shapes.Proofs.push_back(std::move(result));
    }
    return shapes;
}

}
