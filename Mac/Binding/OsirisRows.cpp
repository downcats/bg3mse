#include <stdafx.h>
#include <GameDefinitions/Osiris.h>
#include <GameHooks/OsirisWrappers.h>

#include <Binding/CallAdapters.h>
#include <Binding/HookBridges.h>
#include <Binding/OsirisBinding.h>
#include <Engine/Entries/OsirisNames.h>

// The rows of Binding/OsirisBinding.h: one for every member of
// OsirisStaticGlobals, one for every function OsirisWrappers::Initialize
// wraps, and the node classes with the vtable slots Norbyte's code uses. This
// file sees his definitions and has no code that runs against the game:
// offsets, prototypes and assertions only.

// offsetof on Norbyte's structures (NodeVMT is standard-layout; Node is not).
#pragma clang diagnostic ignored "-Winvalid-offsetof"

BEGIN_NS(mac::binding)

namespace {

template <class T> struct MemberPrototype { static constexpr Prototype Value{}; };
template <class F> requires std::is_function_v<F> struct MemberPrototype<F*> { static constexpr Prototype Value = kPrototypeOf<F>; };

// The prototype a WrappableFunction detours its function with and calls the
// original through.
template <class W> struct WrappedPrototype;
template <class Tag, class F> struct WrappedPrototype<WrappableFunction<Tag, F>> { static constexpr Prototype Value = kPrototypeOf<F>; };

#define BG3MSE_GLOBAL_PROTOTYPE(member) MemberPrototype<decltype(OsirisStaticGlobals::member)>::Value
#define BG3MSE_HOOK_PROTOTYPE(member) WrappedPrototype<decltype(OsirisWrappers::member)>::Value

// The handle of a story string is 64 bits and travels in a register, as the
// adapter's plain type does.
static_assert(sizeof(OsiStringHandle) == sizeof(uint64_t) && std::is_enum_v<OsiStringHandle>);
static_assert(sizeof(DebugFlag) == sizeof(uint32_t));
// The adapter and the bridges pass what Norbyte's prototypes pass.
static_assert(SamePassing(BG3MSE_GLOBAL_PROTOTYPE(AddString), kPrototypeOf<decltype(adapter::OsirisAddString)>));
static_assert(SamePassing(BG3MSE_HOOK_PROTOTYPE(OpenLogFile), kPrototypeOf<decltype(bridge::OsirisOpenLogFile)>));
static_assert(SamePassing(BG3MSE_HOOK_PROTOTYPE(CreateFileW), kPrototypeOf<decltype(bridge::StoryCreateFile)>));
static_assert(SamePassing(BG3MSE_HOOK_PROTOTYPE(CloseHandle), kPrototypeOf<decltype(bridge::StoryCloseHandle)>));
// RuleActionCall is hooked through the prototype OriginalRuleActionCallProc has.
static_assert(SamePassing(BG3MSE_HOOK_PROTOTYPE(RuleActionCall), kPrototypeOf<std::remove_pointer_t<RuleActionCallProc>>));
// Every function row has a prototype this layer can place.
static_assert(BG3MSE_GLOBAL_PROTOTYPE(GetString).Known && BG3MSE_GLOBAL_PROTOTYPE(AddStringRef).Known && BG3MSE_GLOBAL_PROTOTYPE(RemoveString).Known);
static_assert(BG3MSE_HOOK_PROTOTYPE(RegisterDivFunctions).Known && BG3MSE_HOOK_PROTOTYPE(InitGame).Known && BG3MSE_HOOK_PROTOTYPE(DeleteAllData).Known
    && BG3MSE_HOOK_PROTOTYPE(GetFunctionMappings).Known && BG3MSE_HOOK_PROTOTYPE(CloseLogFile).Known && BG3MSE_HOOK_PROTOTYPE(Load).Known
    && BG3MSE_HOOK_PROTOTYPE(Compile).Known && BG3MSE_HOOK_PROTOTYPE(Merge).Known && BG3MSE_HOOK_PROTOTYPE(Event).Known
    && BG3MSE_HOOK_PROTOTYPE(RuleActionCall).Known);

constexpr bool kCritical = true, kOptional = false;

constexpr MemberRow Row(char const* member, uint32_t offset, Bound how, Prototype const& prototype, Through use, unsigned allow, bool critical,
    char const* whenNull, char const* required = nullptr, char const* alsoRequired = nullptr)
{
    return { member, offset, how, member, prototype, use, false, allow, critical, { required, alsoRequired }, whenNull };
}

constexpr char kStringTable[] = "Osiris.Globals.StringTable";

#define OSI_OBJECT(member, whenNull) \
    Row("Osiris.Globals." #member, offsetof(OsirisStaticGlobals, member), Bound::Global, {}, Through::Call, kExactly, kCritical, whenNull)
#define OSI_CALLED(member, how, whenNull) \
    Row("Osiris.Globals." #member, offsetof(OsirisStaticGlobals, member), how, BG3MSE_GLOBAL_PROTOTYPE(member), Through::Call, kExactly, \
        kCritical, whenNull, kStringTable)

// What Norbyte's code does with a global that is null: he has no such case,
// FindOsirisGlobals ends the process when it finds fewer than ten.
#define NO_NULL "on Windows the process ends when it is not found; "

constexpr MemberRow kGlobals[] = {
    OSI_OBJECT(StringTable, NO_NULL "every story string made, read or released goes through it"),
    OSI_OBJECT(Variables, NO_NULL "nothing reads it"),
    OSI_OBJECT(Types, NO_NULL "ResolveAlias reads through it for every story value converted to or from Lua"),
    OSI_OBJECT(Enums, NO_NULL "nothing reads it"),
    OSI_OBJECT(Functions, NO_NULL "every story event looks its function up through it"),
    OSI_OBJECT(Objects, NO_NULL "nothing reads it"),
    OSI_OBJECT(Goals, NO_NULL "the story debugger reads the goals through it"),
    OSI_OBJECT(Adapters, NO_NULL "the identity adapters of queries are taken from it"),
    OSI_OBJECT(Databases, NO_NULL "a database's facts are read through it"),
    OSI_OBJECT(Nodes, NO_NULL "a function's node is taken from it for every insert, delete and query"),
    OSI_OBJECT(DebugFlags, NO_NULL "the story's logging flags are read and replaced when a log is opened"),
    OSI_CALLED(GetString, Bound::Function, "called without a test for the text of every story string"),
    OSI_CALLED(AddString, Bound::CallAdapter, "called without a test when a string is given to the story"),
    OSI_CALLED(AddStringRef, Bound::Function, "called without a test when a story value holding a string is copied"),
    OSI_CALLED(RemoveString, Bound::Function, "called without a test when a story value holding a string is released"),
};

#define OSI_HOOKED(member, allow, critical, whenNull, ...) \
    Row("Osiris." #member, offsetof(OsirisHookTargets, member), Bound::Function, BG3MSE_HOOK_PROTOTYPE(member), Through::Hook, allow, critical, \
        whenNull __VA_OPT__(,) __VA_ARGS__)
#define OSI_BRIDGED(member, critical, whenNull) \
    Row("Osiris." #member, offsetof(OsirisHookTargets, member), Bound::HookBridge, BG3MSE_HOOK_PROTOTYPE(member), Through::Hook, kExactly, critical, whenNull)

// A wrap whose export is missing is logged on Windows and left out; the rest
// goes on (OsirisWrappers::Initialize). So it is here, for the hooks whose
// originals his code calls only through the hook: without one of them, its
// handler never runs. Critical is RegisterDivFunctions, which hands him the
// COsiris object and the story's Call and Query. The originals of OpenLogFile,
// CloseLogFile and Event are called only once they are wrapped
// (OsirisExtender::RestartLogging, CompileWrapper,
// CustomFunctionInjector::ThrowEvent).
#define NOT_WRAPPED "its handlers never run: "

constexpr MemberRow kHooks[] = {
    // His prototype has an int result the function does not produce: the
    // game's caller reads none. What he does with the block it registers
    // needs the block's slots as the library calls them, and the handles he
    // makes for Call and Query packed as the game packs them.
    OSI_HOOKED(RegisterDivFunctions, kResultNotProduced, kCritical,
        NOT_WRAPPED "the story's Call, Query and Error functions are not replaced and the COsiris object is never learnt",
        "Osiris.DivFunctions", engine::osiris::kGetHandle),
    OSI_HOOKED(InitGame, kExactly, kOptional, NOT_WRAPPED "the story debugger is not told that the story starts"),
    OSI_HOOKED(DeleteAllData, kExactly, kOptional, NOT_WRAPPED "the story debugger is not told that the story is unloaded"),
    OSI_HOOKED(GetFunctionMappings, kExactly, kOptional, NOT_WRAPPED "the extender's own story functions are not mapped and the extension state is not reset"),
    OSI_BRIDGED(OpenLogFile, kOptional, NOT_WRAPPED "the story's log files (the Osiris log, the compile log) are not opened"),
    OSI_HOOKED(CloseLogFile, kExactly, kOptional, NOT_WRAPPED "the story's log files are not closed or opened again"),
    OSI_HOOKED(Load, kExactly, kOptional, NOT_WRAPPED "the extender never learns that a story is loaded: no story function resolves in Lua"),
    OSI_HOOKED(Compile, kExactly, kOptional, NOT_WRAPPED "the story is not preprocessed and its compile log is not kept"),
    OSI_HOOKED(Merge, kExactly, kOptional, NOT_WRAPPED "the extension state is not told of a story merge"),
    OSI_HOOKED(Event, kExactly, kOptional, NOT_WRAPPED "Lua listeners for story events do not run, and the events of the extender's own story functions are not thrown"),
    // His prototype has four more parameters than COsiFunctionParseData::Fire
    // takes; his handler passes them on and reads none.
    OSI_HOOKED(RuleActionCall, kParametersNotTaken, kOptional, NOT_WRAPPED "the story debugger is not told of rule actions"),
    OSI_BRIDGED(CreateFileW, kOptional,
        NOT_WRAPPED "the story header the game writes is not extended with the extender's functions, so a story that uses them does not compile"),
    OSI_BRIDGED(CloseHandle, kOptional,
        NOT_WRAPPED "the story header the game writes is not extended with the extender's functions, so a story that uses them does not compile"),
};

// The node classes, by Norbyte's NodeType.
constexpr NodeClassRow kNodeClasses[] = {
    { static_cast<uint8_t>(NodeType::Database), "Osiris.NodeVMT.Database", false },
    { static_cast<uint8_t>(NodeType::Proc), "Osiris.NodeVMT.Proc", false },
    { static_cast<uint8_t>(NodeType::DivQuery), "Osiris.NodeVMT.DivQuery", true },
    { static_cast<uint8_t>(NodeType::And), "Osiris.NodeVMT.And", false },
    { static_cast<uint8_t>(NodeType::NotAnd), "Osiris.NodeVMT.NotAnd", false },
    { static_cast<uint8_t>(NodeType::RelOp), "Osiris.NodeVMT.RelOp", false },
    { static_cast<uint8_t>(NodeType::Rule), "Osiris.NodeVMT.Rule", false },
    { static_cast<uint8_t>(NodeType::InternalQuery), "Osiris.NodeVMT.InternalQuery", true },
    { static_cast<uint8_t>(NodeType::UserQuery), "Osiris.NodeVMT.UserQuery", true },
};
static_assert(static_cast<size_t>(NodeType::Max) + 1 == kNodeTypes && std::size(kNodeClasses) == kNodeTypes - 1);
static_assert(sizeof(OsirisWrappers::VMTs) == kNodeTypes * sizeof(void*));

// Where a call of a virtual function of Norbyte's class Node looks in the
// vtable: the Itanium C++ ABI's pointer to a virtual member function holds
// that offset (and, on arm64, the low bit of its second word set).
template <class F>
int32_t VirtualOffset(F Node::* function)
{
    struct { uintptr_t Ptr; ptrdiff_t Adj; } pointer{};
    static_assert(sizeof(pointer) == sizeof(function));
    memcpy(&pointer, &function, sizeof(pointer));
    if ((pointer.Adj & 1) == 0 || (pointer.Adj >> 1) != 0) return INT32_MAX;       // not virtual, or of another base: no slot
    return static_cast<int32_t>(pointer.Ptr);
}

namespace shape = mac::osiris;

#define OSI_SLOT(name, expected, queryOnly) \
    NodeSlot{ #name, static_cast<uint32_t>(offsetof(NodeVMT, name)), VirtualOffset(&Node::name), static_cast<uint32_t>(expected), queryOnly }

}

std::span<MemberRow const> OsirisGlobalRows() { return kGlobals; }
std::span<MemberRow const> OsirisHookRows() { return kHooks; }
size_t OsirisGlobalsSize() { return sizeof(OsirisStaticGlobals); }
std::span<NodeClassRow const> OsirisNodeClasses() { return kNodeClasses; }

std::span<NodeSlot const> OsirisNodeSlots()
{
    static const NodeSlot slots[] = {
        OSI_SLOT(IsDataNode, shape::kSlotIsDataNode, false),
        OSI_SLOT(IsValid, shape::kSlotIsValid, false),
        OSI_SLOT(PushDownTuple, shape::kSlotPushDownTuple, false),
        OSI_SLOT(PushDownTupleDelete, shape::kSlotPushDownTupleDelete, false),
        OSI_SLOT(InsertTuple, shape::kSlotInsertTuple, false),
        OSI_SLOT(DeleteTuple, shape::kSlotDeleteTuple, false),
        OSI_SLOT(CallQuery, shape::kSlotCallQuery, true),
        OSI_SLOT(GetQueryName, shape::kSlotGetQueryName, true),
    };
    return slots;
}

END_NS()
