#include <Engine/Entries/Areas.h>
#include <Engine/Entries/OsirisNames.h>
#include <Layout/GameMembers.h>
#include <Layout/OsirisLayout.h>

// Osiris: what BG3Extender/GameHooks/OsirisWrappers.cpp and BG3Extender/Osiris
// take from the story library, and the two places in the game where the story
// files are written.
//
// On Windows the wrappers find the library's globals by scanning its
// constructor for ten stores, RuleActionCall by a byte search, and the node
// classes by comparing vtable slots. libOsiris.dylib exports every one of
// these by name, so each is an entry found by its name; what the scans relied
// on (the order of the constructor's stores, the slot of each method) is kept
// as a check.

namespace bg3se::mac::engine {

namespace {

// --- the wrapped functions ---------------------------------------------------

constexpr char kOsirisConstructor[] = "__ZN7COsirisC2Ev";

// The store of each global in COsiris::COsiris(), numbered as Norbyte's
// FindOsirisGlobals numbers them.
#define OSIRIS_GLOBAL(number, symbol) \
    constexpr Operand kStore##number[] = { { \
        .What = "constructor's store", \
        .In = Exact(kOsirisConstructor), \
        .How = Operand::Read::StoreOrder, \
        .Expected = number, \
        .Target = Exact(symbol), \
    } }

OSIRIS_GLOBAL(0, "_OsiStringTable");
OSIRIS_GLOBAL(1, "_RuleVariableMan");
OSIRIS_GLOBAL(2, "_OsiDIVObjectTypesMan");
OSIRIS_GLOBAL(3, "_OsiParseSymtableMan");
OSIRIS_GLOBAL(4, "_OsiFunctionMan");
OSIRIS_GLOBAL(5, "_OsiDIVObjectsMan");
OSIRIS_GLOBAL(6, "_OsiGoalMan");
OSIRIS_GLOBAL(7, "_ReteAdaptorFactory");
OSIRIS_GLOBAL(8, "_ReteDBaseFactory");
OSIRIS_GLOBAL(9, "_ReteNodeFactory");

#undef OSIRIS_GLOBAL

// --- the block of functions the game registers -------------------------------

// The library's accessors of the block. Each loads its slot and jumps there
// with the arguments it was given after its own object, one register down:
// the function's id and the arguments for Call and Query, the text for
// ErrorMessage, and (successful, text, flag) for Assert.
constexpr MaskedWord kCallThroughSlot[] = {
    { 0xf9400003, Any::Imm12 },     // ldr x3, [x0, #slot]
    { 0xaa0103e0 },                 // mov x0, x1
    { 0xaa0203e1 },                 // mov x1, x2
    { 0xd61f0060 },                 // br x3
};
constexpr MaskedWord kErrorThroughSlot[] = {
    { 0xf9400002, Any::Imm12 },     // ldr x2, [x0, #slot]
    { 0xaa0103e0 },                 // mov x0, x1
    { 0xd61f0040 },                 // br x2
};
constexpr MaskedWord kAssertThroughSlot[] = {
    { 0xf9400004, Any::Imm12 },     // ldr x4, [x0, #slot]
    { 0xaa0103e0 },                 // mov x0, x1
    { 0xaa0203e1 },                 // mov x1, x2
    { 0xaa0303e2 },                 // mov x2, x3
    { 0xd61f0080 },                 // br x4
};

constexpr Operand kDivSlots[] = {
    { .What = "Call", .In = Exact("__ZNK4CDIV4callEjP16COsiArgumentDesc"), .Pattern = kCallThroughSlot, .Expected = mac::osiris::kDivCall },
    { .What = "Query", .In = Exact("__ZNK4CDIV5queryEjP16COsiArgumentDesc"), .Pattern = kCallThroughSlot, .Expected = mac::osiris::kDivQuery },
    { .What = "ErrorMessage", .In = Exact("__ZN4CDIV11SetErrorMsgEPKc"), .Pattern = kErrorThroughSlot, .Expected = mac::osiris::kDivErrorMessage },
    { .What = "Assert", .In = Exact("__ZN4CDIV6AssertEbPKcb"), .Pattern = kAssertThroughSlot, .Expected = mac::osiris::kDivAssert },
};

// --- the node vtables --------------------------------------------------------
//
// Offsets are from the address an object's vtable pointer holds. A slot holds
// the class's own override or the one it inherits, so each vtable names the
// function it has there. The order of the four tuple methods is not Norbyte's
// NodeVMT order: here the three-argument Add and Del (PushDownTuple,
// PushDownTupleDelete) come first, then the one-argument ones (InsertTuple,
// DeleteTuple).

#define NODE_VALID(cls) "__ZN" cls "5ValidEP22COsiVarIDValuePairList13PCReteAdaptor"
#define NODE_ADD3(cls) "__ZN" cls "3AddEP22COsiVarIDValuePairList10PCReteNode15TReteEntryPoint"
#define NODE_DEL3(cls) "__ZN" cls "3DelEP22COsiVarIDValuePairList10PCReteNode15TReteEntryPoint"
#define NODE_ADD1(cls) "__ZN" cls "3AddEP18COsipParameterList"
#define NODE_DEL1(cls) "__ZN" cls "3DelEP18COsipParameterList"
#define NODE_IS_START(cls) "__ZNK" cls "11IsStartNodeEv"

// The argument passing of the slots Norbyte replaces or calls, the same for
// every node class.
constexpr Signature kIsDataNode = Takes({ Pointer(Place::X0, 0, "the node") }, Returns::W0, "bool");
constexpr Signature kIsValid = Takes({
    Pointer(Place::X0, 0, "the node"),
    Pointer(Place::X1, 1, "COsiVarIDValuePairList: the tuple"),
    Value(Place::X2, 4, 2, "PCReteAdaptor: the adaptor's handle, 32 bits"),
}, Returns::W0, "bool");
constexpr Signature kPushDownTuple = Takes({
    Pointer(Place::X0, 0, "the node"),
    Pointer(Place::X1, 1, "COsiVarIDValuePairList: the tuple"),
    Value(Place::X2, 4, 2, "PCReteNode: a node's handle, 32 bits (Norbyte calls it the adapter)"),
    Value(Place::X3, 4, 3, "TReteEntryPoint"),
});
constexpr Signature kInsertTuple = Takes({ Pointer(Place::X0, 0, "the node"), Pointer(Place::X1, 1, "COsipParameterList: the tuple") });
constexpr Signature kCallQuery = Takes({ Pointer(Place::X0, 0, "the node"), Pointer(Place::X1, 1, "COsiArgumentDesc") }, Returns::W0, "bool");
constexpr Signature kGetQueryName = Takes({ Unread(Place::X0, 0, "the node") }, Returns::X0, "char const*");

#define NODE_SLOTS(isStart, valid, add3, del3, add1, del1) \
    { "IsDataNode", Exact(isStart), 0x20, &kIsDataNode }, \
    { "IsValid", Exact(valid), 0x28, &kIsValid }, \
    { "PushDownTuple", Exact(add3), 0x58, &kPushDownTuple }, \
    { "PushDownTupleDelete", Exact(del3), 0x60, &kPushDownTuple }, \
    { "InsertTuple", Exact(add1), 0x68, &kInsertTuple }, \
    { "DeleteTuple", Exact(del1), 0x70, &kInsertTuple }

constexpr VtableMethod kFactSlots[] = {
    NODE_SLOTS(NODE_IS_START("14CReteStartNode"), NODE_VALID("9CReteFact"), NODE_ADD3("14CReteStartNode"), NODE_DEL3("14CReteStartNode"),
        NODE_ADD1("14CReteStartNode"), NODE_DEL1("14CReteStartNode")),
};
constexpr VtableMethod kEventSlots[] = {
    NODE_SLOTS(NODE_IS_START("14CReteStartNode"), NODE_VALID("10CReteEvent"), NODE_ADD3("14CReteStartNode"), NODE_DEL3("14CReteStartNode"),
        NODE_ADD1("14CReteStartNode"), NODE_DEL1("14CReteStartNode")),
};

#define QUERY_SLOTS(cls) \
    NODE_SLOTS(NODE_IS_START("9CReteNode"), NODE_VALID("10CReteQuery"), NODE_ADD3("10CReteQuery"), NODE_DEL3("10CReteQuery"), \
        NODE_ADD1("9CReteNode"), NODE_DEL1("9CReteNode")), \
    { "CallQuery", Exact("__ZN" cls "13_ExecuteQueryEP16COsiArgumentDesc"), 0xc0, &kCallQuery }, \
    { "GetQueryName", Exact("__ZNK" cls "15_LogDescriptionEv"), 0xc8, &kGetQueryName }

constexpr VtableMethod kDivQuerySlots[] = { QUERY_SLOTS("13CReteDIVQuery") };
constexpr VtableMethod kUserQuerySlots[] = { QUERY_SLOTS("13CReteOsiQuery") };
constexpr VtableMethod kInternalQuerySlots[] = { QUERY_SLOTS("18CReteInternalQuery") };

constexpr VtableMethod kAndSlots[] = {
    NODE_SLOTS(NODE_IS_START("9CReteNode"), NODE_VALID("8CReteAnd"), NODE_ADD3("8CReteAnd"), NODE_DEL3("8CReteAnd"),
        NODE_ADD1("9CReteNode"), NODE_DEL1("9CReteNode")),
};
constexpr VtableMethod kNotAndSlots[] = {
    NODE_SLOTS(NODE_IS_START("9CReteNode"), NODE_VALID("9CReteNAnd"), NODE_ADD3("9CReteNAnd"), NODE_DEL3("9CReteNAnd"),
        NODE_ADD1("9CReteNode"), NODE_DEL1("9CReteNode")),
};
constexpr VtableMethod kRelOpSlots[] = {
    NODE_SLOTS(NODE_IS_START("9CReteNode"), NODE_VALID("17CReteRelCondition"), NODE_ADD3("17CReteRelCondition"), NODE_DEL3("14CReteChildNode"),
        NODE_ADD1("9CReteNode"), NODE_DEL1("9CReteNode")),
};
constexpr VtableMethod kRuleSlots[] = {
    NODE_SLOTS(NODE_IS_START("9CReteNode"), NODE_VALID("19CReteRuleActionPart"), NODE_ADD3("19CReteRuleActionPart"), NODE_DEL3("19CReteRuleActionPart"),
        NODE_ADD1("9CReteNode"), NODE_DEL1("9CReteNode")),
};

#undef QUERY_SLOTS
#undef NODE_SLOTS
#undef NODE_IS_START
#undef NODE_DEL1
#undef NODE_ADD1
#undef NODE_DEL3
#undef NODE_ADD3
#undef NODE_VALID

constexpr char kNodeUse[] =
    "Osiris/Shared/NodeHooks.cpp: the node class of an object by its vtable pointer; IsValid, PushDownTuple, PushDownTupleDelete, "
    "InsertTuple, DeleteTuple and CallQuery are replaced in the vtable";

// The slots the query classes have and the others do not.
constexpr char kNodeCaveat[] =
    "the vtable has two destructor slots and ends after its last slot (0xc0 bytes of slots; 0xd0 for the query classes, 0xc8 for "
    "the rule class): Norbyte's NodeVMT, laid out for Windows, must not be used to index it or to copy it whole. Use the slots found";

// --- the story files ---------------------------------------------------------

constexpr char kComposeStory[] = "__ZN3esv19StoryImplementation20ComposeAndBuildStoryEb";
constexpr char kWriteHeader[] = "__ZNK3osi13OsirisManager11WriteHeaderERKN2ls4PathE";
constexpr char kWriterClose[] = "__ZN2ls10FileWriter5CloseEv";

constexpr char kWriterConstructor[] = "__ZN2ls10FileWriterC1ERKNS_4PathENS0_5ETypeEi";
constexpr char kPathToWide[] = "__ZNK2ls4Path9ToWStringEv";

constexpr auto& kWriteHeaderName = osiris::kWriteHeader;
constexpr auto& kCloseStoryName = osiris::kCloseStory;

constexpr Derivation kCloseStory{
    .Host = Exact(kComposeStory),
    .HostShape = { 3865, 0x3e21fd198bf0f29eull },     // 4.1.1.7631656
    .Anchor = Calls(Exact(kWriterClose)),
};

// WriteHeader begins by taking the text of its path for fopen: the path
// itself, unless the byte at +0xf is negative, then the pointer at +0.
constexpr MaskedWord kPathText[] = {
    { 0x39c00028, Any::Imm12 },     // ldrsb w8, [x1, #flag]
    { 0xf9400029 },                 // ldr x9, [x1]
    { 0x7100011f },                 // cmp w8, #0
    { 0x9a81b120 },                 // csel x0, x9, x1, lt
    { 0x90000001, Any::Adrp },      // adrp x1, the mode
    { 0x91000021, Any::Imm12 },     // add x1, x1, #...
    { 0x94000000, Any::Branch },    // bl fopen
};

constexpr Operand kWriteHeaderOperands[] = {
    { .What = "calls in the story build", .In = Exact(kComposeStory), .How = Operand::Read::CallCount, .Expected = 2, .Target = Exact(kWriteHeader) },
    { .What = osiris::kPathTextFlag, .Pattern = kPathText, .At = 0, .How = Operand::Read::LoadStoreOffset, .Expected = kGamePathTextFlag },
};

// The generated story's writer is a local of the story build, constructed
// from a path that is another local ...
constexpr MaskedWord kConstructStoryWriter[] = {
    { 0x910003e0, Any::Imm12 },     // add x0, sp, #writer
    { 0x910003e1, Any::Imm12 },     // add x1, sp, #path
    { 0x52800002 },                 // mov w2, #0
    { 0x94000000, Any::Branch },    // bl ls::FileWriter::FileWriter
    { 0xf94003e8, Any::Imm12 },     // ldr x8, [sp, #writer]: the file it opened
    { 0x39403108 },                 // ldrb w8, [x8, #0xc]
};
// ... and closed by the call this entry is, after which that path is made the
// wide string COsiris::Compile is given.
constexpr MaskedWord kCloseStoryWriter[] = {
    { 0x910003e0, Any::Imm12 },     // add x0, sp, #writer
    { 0x94000000, Any::Branch },    // bl ls::FileWriter::Close
    { 0xf9400354 },                 // ldr x20, [x26]
    { 0x910003e0, Any::Imm12 },     // add x0, sp, #wide string
    { 0x910003e1, Any::Imm12 },     // add x1, sp, #path
    { 0x94000000, Any::Branch },    // bl ls::Path::ToWString
};

constexpr Operand kCloseStoryOperands[] = {
    { .What = osiris::kWriterConstructed, .In = Exact(kComposeStory), .Pattern = kConstructStoryWriter, .At = 0,
      .How = Operand::Read::AddImmediate, .Expected = 0x180 },
    { .What = osiris::kWriterConstructedFrom, .In = Exact(kComposeStory), .Pattern = kConstructStoryWriter, .At = 1,
      .How = Operand::Read::AddImmediate, .Expected = 0x1e8 },
    { .What = "constructor called", .In = Exact(kComposeStory), .Pattern = kConstructStoryWriter, .At = 3,
      .How = Operand::Read::BranchTarget, .Target = Exact(kWriterConstructor) },
    { .What = osiris::kWriterClosed, .In = Exact(kComposeStory), .Pattern = kCloseStoryWriter, .At = 0,
      .How = Operand::Read::AddImmediate, .Expected = 0x180 },
    { .What = "close called", .In = Exact(kComposeStory), .Pattern = kCloseStoryWriter, .At = 1,
      .How = Operand::Read::BranchTarget, .Target = Exact(kWriterClose) },
    { .What = osiris::kPathCompiled, .In = Exact(kComposeStory), .Pattern = kCloseStoryWriter, .At = 4,
      .How = Operand::Read::AddImmediate, .Expected = 0x1e8 },
    { .What = "path made wide", .In = Exact(kComposeStory), .Pattern = kCloseStoryWriter, .At = 5,
      .How = Operand::Read::BranchTarget, .Target = Exact(kPathToWide) },
};

constexpr char const* kStoryFileReplacements[] = { kWriteHeaderName, kCloseStoryName };

constexpr char kStoryFileFate[] =
    "Windows API. The game writes Story/story_header.div inside osi::OsirisManager::WriteHeader (fopen, fprintf, fclose: the file is "
    "closed when the function returns) and Story/story_generated.div through an ls::FileWriter that "
    "esv::StoryImplementation::ComposeAndBuildStory closes with its one call of ls::FileWriter::Close, just before it compiles the story";

constexpr Entry kEntries[] = {
    {
        .Name = "Osiris.RegisterDivFunctions",
        .In = Image::Osiris,
        .What = Kind::FunctionToHook,
        .Mac = "COsiris::RegisterDIVFunctions(TOsirisInitFunction*)",
        .Use = "OsirisWrappers: before the library copies the game's Call, Query, ErrorMessage and Assert functions, put the wrappers in their place",
        .Where = Exact("__ZN7COsiris20RegisterDIVFunctionsEP19TOsirisInitFunction"),
        .Call = Takes({
            Unread(Place::X0, 0, "COsiris"),
            Pointer(Place::X1, 1, "TOsirisInitFunction: five function pointers, 0x28 bytes, copied to the library's DIV"),
        }, Returns::Nothing, nullptr, "Norbyte declares an int result; the function returns nothing"),
        .Shape = { 7, 0xa2e84eb72a76f629ull },     // 4.1.1.7631656
    },
    {
        .Name = "Osiris.InitGame",
        .In = Image::Osiris,
        .What = Kind::FunctionToHook,
        .Mac = "COsiris::InitGame()",
        .Use = "OsirisWrappers: the story is initialised (pre- and post-hooks)",
        .Where = Exact("__ZN7COsiris8InitGameEv"),
        .Call = Takes({ Pointer(Place::X0, 0, "COsiris") }, Returns::W0, "bool", "Norbyte declares an int result; it is a bool"),
        .Shape = { 294, 0xaf0ec04b6455d902ull },     // 4.1.1.7631656
    },
    {
        .Name = "Osiris.DeleteAllData",
        .In = Image::Osiris,
        .What = Kind::FunctionToHook,
        .Mac = "COsiris::DeleteAllData(bool)",
        .Use = "OsirisWrappers: the story is being unloaded",
        .Where = Exact("__ZN7COsiris13DeleteAllDataEb"),
        .Call = Takes({ Pointer(Place::X0, 0, "COsiris"), Value(Place::X1, 1, 1, "bool") }),
        .Shape = { 292, 0xa5c0dfc381d16caaull },     // 4.1.1.7631656
    },
    {
        .Name = "Osiris.GetFunctionMappings",
        .In = Image::Osiris,
        .What = Kind::FunctionToHook,
        .Mac = "COsiris::GetFunctionMappings(MappingInfo**, unsigned int*)",
        .Use = "OsirisWrappers: the game asks for the function table; the extender's functions are added to the answer",
        .Where = Exact("__ZN7COsiris19GetFunctionMappingsEPP11MappingInfoPj"),
        .Call = Takes({
            Unread(Place::X0, 0, "COsiris"),
            Pointer(Place::X1, 1, "MappingInfo*: receives the array"),
            Pointer(Place::X2, 2, "unsigned int: receives the count"),
        }),
        .Shape = { 184, 0x879657381f05b0a1ull },     // 4.1.1.7631656
    },
    {
        .Name = "Osiris.OpenLogFile",
        .In = Image::Osiris,
        .What = Kind::FunctionToHook,
        .Mac = "COsiris::OpenLogFile(wchar_t const*, unsigned int)",
        .Use = "OsirisWrappers and Osiris/OsirisExtender.cpp: the story log is opened; also called to open the extender's own log",
        .Where = Exact("__ZN7COsiris11OpenLogFileEPKwj"),
        .Call = Takes({
            Unread(Place::X0, 0, "COsiris"),
            Pointer(Place::X1, 1, "wchar_t const: the path, in 32-bit characters"),
            Value(Place::X2, 4, 2, "unsigned int: a std::ios_base::openmode as libc++ numbers it (app 0x01, binary 0x04, in 0x08, out 0x10, trunc 0x20)"),
        }, Returns::W0, "bool: the file is open",
            "Norbyte declares no result. wchar_t is 32 bits here: a UTF-16 path is not what the function reads"),
        .Shape = { 195, 0xd2661b90f37b8ac5ull },     // 4.1.1.7631656
    },
    {
        .Name = "Osiris.CloseLogFile",
        .In = Image::Osiris,
        .What = Kind::FunctionToHook,
        .Mac = "COsiris::CloseLogFile()",
        .Use = "OsirisWrappers and Osiris/OsirisExtender.cpp: the story log is closed",
        .Where = Exact("__ZN7COsiris12CloseLogFileEv"),
        .Call = Takes({ Unread(Place::X0, 0, "COsiris") }),
        .Shape = { 44, 0x4bfb647df7de845bull },     // 4.1.1.7631656
    },
    {
        .Name = "Osiris.Compile",
        .In = Image::Osiris,
        .What = Kind::FunctionToHook,
        .Mac = "COsiris::Compile(wchar_t const*, wchar_t const*)",
        .Use = "OsirisWrappers: the story is compiled",
        .Where = Exact("__ZN7COsiris7CompileEPKwS1_"),
        .Call = Takes({
            Pointer(Place::X0, 0, "COsiris"),
            Pointer(Place::X1, 1, "wchar_t const: 32-bit characters"),
            Pointer(Place::X2, 2, "wchar_t const: 32-bit characters"),
        }, Returns::W0, "bool"),
        .Shape = { 1976, 0x9386f753ab4183d7ull },     // 4.1.1.7631656
    },
    {
        .Name = "Osiris.Load",
        .In = Image::Osiris,
        .What = Kind::FunctionToHook,
        .Mac = "COsiris::Load(COsiSmartBuf&)",
        .Use = "OsirisWrappers: a compiled story is loaded",
        .Where = Exact("__ZN7COsiris4LoadER12COsiSmartBuf"),
        .Call = Takes({ Pointer(Place::X0, 0, "COsiris"), Pointer(Place::X1, 1, "COsiSmartBuf") }, Returns::W0, "bool",
            "Norbyte declares an int result; it is a bool"),
        .Shape = { 431, 0x806f0e89eae906feull },     // 4.1.1.7631656
    },
    {
        .Name = "Osiris.Merge",
        .In = Image::Osiris,
        .What = Kind::FunctionToHook,
        .Mac = "COsiris::Merge(wchar_t const*)",
        .Use = "OsirisWrappers: a story patch is merged (pre- and post-hooks)",
        .Where = Exact("__ZN7COsiris5MergeEPKw"),
        .Call = Takes({ Pointer(Place::X0, 0, "COsiris"), Pointer(Place::X1, 1, "wchar_t const: 32-bit characters") }, Returns::W0, "bool"),
        .Shape = { 696, 0x04cf483c53133ff0ull },     // 4.1.1.7631656
    },
    {
        .Name = "Osiris.Event",
        .In = Image::Osiris,
        .What = Kind::FunctionToHook,
        .Mac = "COsiris::Event(unsigned int, COsiArgumentDesc*)",
        .Use = "OsirisWrappers: the game raises a story event",
        .Where = Exact("__ZN7COsiris5EventEjP16COsiArgumentDesc"),
        .Call = Takes({
            Unread(Place::X0, 0, "COsiris"),
            Value(Place::X1, 4, 1, "unsigned int: the function's id"),
            Pointer(Place::X2, 2, "COsiArgumentDesc"),
        }, Returns::W0, "ReturnCode: 0 processed, 1 undefined, 2 ignored"),
        .Shape = { 379, 0x409742d2b6f56a52ull },     // 4.1.1.7631656
    },
    {
        .Name = "Osiris.RuleActionCall",
        .In = Image::Osiris,
        .What = Kind::FunctionToHook,
        .Mac = "COsiFunctionParseData::Fire(); it also runs for the actions of a goal it completes: it jumps to COsiGoal::SetCompleted, which calls it",
        .Use = "OsirisWrappers: a rule action is about to run (the debugger's pre- and post-hooks)",
        .Where = Exact("__ZN21COsiFunctionParseData4FireEv"),
        .Call = Takes({ Pointer(Place::X0, 0, "COsiFunctionParseData: Norbyte's RuleActionNode (goal id or debug hook at +0x14)") },
            Returns::Nothing, nullptr, "Norbyte's prototype has four more parameters; the function has none of them"),
        .Shape = { 878, 0x01d2411a1b7bb9bbull },     // 4.1.1.7631656
    },

    {
        .Name = "Osiris.DivFunctions",
        .In = Image::Osiris,
        .What = Kind::GlobalObject,
        .Mac = "DIV, the library's copy of the block the game registers: { Handle, Call, Query, ErrorMessage, Assert }",
        .Use = "OsirisWrappers (Call, Query, Error, Assert): the wrappers replace four pointers of the block the game passes to "
               "RegisterDIVFunctions; the library calls through its copy",
        .Where = Exact("_DIV"),
        .Bytes = 0x28,
        .Operands = kDivSlots,
    },

    {
        .Name = osiris::kGetHandle,
        .In = Image::Game,
        .What = Kind::FunctionToCall,
        .Mac = "osi::OsirisInterface::GetHandle(unsigned int, unsigned int, unsigned int, unsigned int): the first function of the block the "
               "game registers. It packs a function's four key parts into the handle Call and Query are given",
        .Use = "GameDefinitions/Osiris.h (OsirisFunctionHandle): Norbyte packs the same handle himself for the Call and Query he makes from "
               "Lua; the function is not called, its code is what his packing has to equal",
        .Where = Exact("__ZN3osi15OsirisInterface9GetHandleEjjjj"),
        .Call = Takes({
            Value(Place::X0, 4, -1, "unsigned int: the type, three bits"),
            Value(Place::X1, 4, -1, "unsigned int: eight bits at bit 20, for a type above 3"),
            Value(Place::X2, 4, -1, "unsigned int: the function's id, at bit 3: 25 bits for a type up to 3, 17 above"),
            Value(Place::X3, 4, -1, "unsigned int: bit 31"),
        }, Returns::W0, "the handle", "Norbyte has no prototype for it: it stands for his own OsirisFunctionHandle"),
        .Shape = { 9, 0x4f6911e5c7d9b0e9ull },     // 4.1.1.7631656
    },

    { .Name = "Osiris.Globals.StringTable", .In = Image::Osiris, .What = Kind::GlobalPointer, .Mac = "OsiStringTable",
      .Use = "OsirisWrappers (FindOsirisGlobals): Globals.StringTable", .Where = Exact("_OsiStringTable"), .Operands = kStore0 },
    { .Name = "Osiris.Globals.Variables", .In = Image::Osiris, .What = Kind::GlobalPointer, .Mac = "RuleVariableMan",
      .Use = "OsirisWrappers (FindOsirisGlobals): Globals.Variables", .Where = Exact("_RuleVariableMan"), .Operands = kStore1 },
    { .Name = "Osiris.Globals.Types", .In = Image::Osiris, .What = Kind::GlobalPointer, .Mac = "OsiDIVObjectTypesMan",
      .Use = "OsirisWrappers (FindOsirisGlobals): Globals.Types", .Where = Exact("_OsiDIVObjectTypesMan"), .Operands = kStore2 },
    { .Name = "Osiris.Globals.Enums", .In = Image::Osiris, .What = Kind::GlobalPointer, .Mac = "OsiParseSymtableMan",
      .Use = "OsirisWrappers (FindOsirisGlobals): Globals.Enums", .Where = Exact("_OsiParseSymtableMan"), .Operands = kStore3 },
    { .Name = "Osiris.Globals.Functions", .In = Image::Osiris, .What = Kind::GlobalPointer, .Mac = "OsiFunctionMan",
      .Use = "OsirisWrappers (FindOsirisGlobals): Globals.Functions", .Where = Exact("_OsiFunctionMan"), .Operands = kStore4 },
    { .Name = "Osiris.Globals.Objects", .In = Image::Osiris, .What = Kind::GlobalPointer, .Mac = "OsiDIVObjectsMan",
      .Use = "OsirisWrappers (FindOsirisGlobals): Globals.Objects", .Where = Exact("_OsiDIVObjectsMan"), .Operands = kStore5 },
    { .Name = "Osiris.Globals.Goals", .In = Image::Osiris, .What = Kind::GlobalPointer, .Mac = "OsiGoalMan",
      .Use = "OsirisWrappers (FindOsirisGlobals): Globals.Goals", .Where = Exact("_OsiGoalMan"), .Operands = kStore6 },
    { .Name = "Osiris.Globals.Adapters", .In = Image::Osiris, .What = Kind::GlobalPointer, .Mac = "ReteAdaptorFactory",
      .Use = "OsirisWrappers (FindOsirisGlobals): Globals.Adapters", .Where = Exact("_ReteAdaptorFactory"), .Operands = kStore7 },
    { .Name = "Osiris.Globals.Databases", .In = Image::Osiris, .What = Kind::GlobalPointer, .Mac = "ReteDBaseFactory",
      .Use = "OsirisWrappers (FindOsirisGlobals): Globals.Databases", .Where = Exact("_ReteDBaseFactory"), .Operands = kStore8 },
    { .Name = "Osiris.Globals.Nodes", .In = Image::Osiris, .What = Kind::GlobalPointer, .Mac = "ReteNodeFactory",
      .Use = "OsirisWrappers (FindOsirisGlobals): Globals.Nodes", .Where = Exact("_ReteNodeFactory"), .Operands = kStore9 },
    {
        .Name = "Osiris.Globals.DebugFlags",
        .In = Image::Osiris,
        .What = Kind::GlobalObject,
        .Mac = "ParserFlags, the 32-bit word COsiris::SetOption reads and writes",
        .Use = "OsirisWrappers (FindDebugFlags) and Osiris/OsirisExtender.cpp: the story's logging flags, read and replaced",
        .Where = Exact("_ParserFlags"),
        .Bytes = 4,
    },

    {
        .Name = "Osiris.Globals.GetString",
        .In = Image::Osiris,
        .What = Kind::FunctionToCall,
        .Mac = "COsiStringTable::GetStr(COsiStringHandle)",
        .Use = "GameDefinitions/Osiris.h: the text of a story string handle",
        .Where = Exact("__ZN15COsiStringTable6GetStrE16COsiStringHandle"),
        .Call = Takes({
            Pointer(Place::X0, 0, "COsiStringTable: the object OsiStringTable points to"),
            Value(Place::X1, 8, 1, "COsiStringHandle: 64 bits, the index in the low 21"),
        }, Returns::X0, "char const*"),
        .Shape = { 10, 0x8aebdd8e26afdf89ull },     // 4.1.1.7631656
        .Needs = "Osiris.Globals.StringTable",
    },
    {
        .Name = "Osiris.Globals.AddString",
        .In = Image::Osiris,
        .What = Kind::FunctionToCall,
        .Mac = "COsiStringTable::AddStr(char const*, bool)",
        .Use = "GameDefinitions/Osiris.h: a story string handle for a text",
        .Where = Exact("__ZN15COsiStringTable6AddStrEPKcb"),
        .Call = Takes({
            Pointer(Place::X0, 0, "COsiStringTable: the object OsiStringTable points to"),
            Pointer(Place::X1, 2, "char const: the text"),
            Value(Place::X2, 1, 3, "bool: the text is a GUID string"),
        }, Returns::X0, "COsiStringHandle, by value",
            "Norbyte declares (table, OsiStringHandle& out, text, bool) returning a pointer to the handle; here there is no out "
            "parameter and the handle is the result"),
        .Shape = { 790, 0x8df00f27ed14ef81ull },     // 4.1.1.7631656
        .Needs = "Osiris.Globals.StringTable",
    },
    {
        .Name = "Osiris.Globals.AddStringRef",
        .In = Image::Osiris,
        .What = Kind::FunctionToCall,
        .Mac = "COsiStringTable::AddStrRef(COsiStringHandle)",
        .Use = "GameDefinitions/Osiris.h: one more reference to a story string",
        .Where = Exact("__ZN15COsiStringTable9AddStrRefE16COsiStringHandle"),
        .Call = Takes({ Pointer(Place::X0, 0, "COsiStringTable: the object OsiStringTable points to"), Value(Place::X1, 8, 1, "COsiStringHandle") }),
        .Shape = { 25, 0x6591984f9a828896ull },     // 4.1.1.7631656
        .Needs = "Osiris.Globals.StringTable",
    },
    {
        .Name = "Osiris.Globals.RemoveString",
        .In = Image::Osiris,
        .What = Kind::FunctionToCall,
        .Mac = "COsiStringTable::RemoveStr(COsiStringHandle)",
        .Use = "GameDefinitions/Osiris.h: one reference fewer to a story string",
        .Where = Exact("__ZN15COsiStringTable9RemoveStrE16COsiStringHandle"),
        .Call = Takes({ Pointer(Place::X0, 0, "COsiStringTable: the object OsiStringTable points to"), Value(Place::X1, 8, 1, "COsiStringHandle") }),
        .Shape = { 242, 0xdd9a9385f753fad1ull },     // 4.1.1.7631656
        .Needs = "Osiris.Globals.StringTable",
    },

    { .Name = "Osiris.NodeVMT.Database", .In = Image::Osiris, .What = Kind::Vtable, .Mac = "vtable for CReteFact", .Use = kNodeUse,
      .Where = Exact("__ZTV9CReteFact"), .Methods = kFactSlots, .Caveat = kNodeCaveat },
    { .Name = "Osiris.NodeVMT.Proc", .In = Image::Osiris, .What = Kind::Vtable, .Mac = "vtable for CReteEvent", .Use = kNodeUse,
      .Where = Exact("__ZTV10CReteEvent"), .Methods = kEventSlots, .Caveat = kNodeCaveat },
    { .Name = "Osiris.NodeVMT.DivQuery", .In = Image::Osiris, .What = Kind::Vtable, .Mac = "vtable for CReteDIVQuery", .Use = kNodeUse,
      .Where = Exact("__ZTV13CReteDIVQuery"), .Methods = kDivQuerySlots, .Caveat = kNodeCaveat },
    { .Name = "Osiris.NodeVMT.And", .In = Image::Osiris, .What = Kind::Vtable, .Mac = "vtable for CReteAnd", .Use = kNodeUse,
      .Where = Exact("__ZTV8CReteAnd"), .Methods = kAndSlots, .Caveat = kNodeCaveat },
    { .Name = "Osiris.NodeVMT.NotAnd", .In = Image::Osiris, .What = Kind::Vtable, .Mac = "vtable for CReteNAnd", .Use = kNodeUse,
      .Where = Exact("__ZTV9CReteNAnd"), .Methods = kNotAndSlots, .Caveat = kNodeCaveat },
    { .Name = "Osiris.NodeVMT.RelOp", .In = Image::Osiris, .What = Kind::Vtable, .Mac = "vtable for CReteRelCondition", .Use = kNodeUse,
      .Where = Exact("__ZTV17CReteRelCondition"), .Methods = kRelOpSlots, .Caveat = kNodeCaveat },
    { .Name = "Osiris.NodeVMT.Rule", .In = Image::Osiris, .What = Kind::Vtable, .Mac = "vtable for CReteRuleActionPart", .Use = kNodeUse,
      .Where = Exact("__ZTV19CReteRuleActionPart"), .Methods = kRuleSlots, .Caveat = kNodeCaveat },
    { .Name = "Osiris.NodeVMT.InternalQuery", .In = Image::Osiris, .What = Kind::Vtable, .Mac = "vtable for CReteInternalQuery", .Use = kNodeUse,
      .Where = Exact("__ZTV18CReteInternalQuery"), .Methods = kInternalQuerySlots, .Caveat = kNodeCaveat },
    { .Name = "Osiris.NodeVMT.UserQuery", .In = Image::Osiris, .What = Kind::Vtable, .Mac = "vtable for CReteOsiQuery", .Use = kNodeUse,
      .Where = Exact("__ZTV13CReteOsiQuery"), .Methods = kUserQuerySlots, .Caveat = kNodeCaveat },

    {
        .Name = "Osiris.CreateFileW",
        .In = Image::Game,
        .What = Kind::NoCallableCode,
        .Use = "Osiris/Shared/CustomFunctions.cpp (OnCreateFile): notice the game opening story_header.div or story_generated.div for writing",
        .Gone = { .Fate = kStoryFileFate, .Replacements = kStoryFileReplacements },
    },
    {
        .Name = "Osiris.CloseHandle",
        .In = Image::Game,
        .What = Kind::NoCallableCode,
        .Use = "Osiris/Shared/CustomFunctions.cpp (OnCloseHandle): when that file is closed, append the extender's declarations to the "
               "header, or what forces a recompile to the generated story",
        .Gone = { .Fate = kStoryFileFate, .Replacements = kStoryFileReplacements },
    },
    {
        .Name = kWriteHeaderName,
        .In = Image::Game,
        .What = Kind::FunctionToHook,
        .Mac = "osi::OsirisManager::WriteHeader(ls::Path const&) const",
        .Use = "takes the place of the CreateFileW and CloseHandle hooks for story_header.div: the file is written and closed when the "
               "function returns, and its path is the argument",
        .Where = Exact(kWriteHeader),
        .Call = Takes({ Pointer(Place::X0, -1, "osi::OsirisManager const"), Pointer(Place::X1, -1, "ls::Path const: the header file") },
            Returns::W0, "bool", "it stands in for hooks on the Windows API's CreateFileW and CloseHandle, with which it shares nothing"),
        .Shape = { 386, 0x257fc361e1977b21ull },     // 4.1.1.7631656
        .Operands = kWriteHeaderOperands,
    },
    {
        .Name = kCloseStoryName,
        .In = Image::Game,
        .What = Kind::CallSite,
        .Mac = "the one call of ls::FileWriter::Close() in esv::StoryImplementation::ComposeAndBuildStory(bool): it closes "
               "Story/story_generated.div before the story is compiled",
        .Use = "takes the place of the CloseHandle hook for story_generated.div",
        .Derived = &kCloseStory,
        .Operands = kCloseStoryOperands,
        .Needs = osiris::kWriterClose,
    },
    {
        .Name = osiris::kWriterClose,
        .In = Image::Game,
        .What = Kind::FunctionToCall,
        .Mac = "ls::FileWriter::Close()",
        .Use = "what the call site above calls: its replacement calls this first",
        .Where = Exact(kWriterClose),
        .Call = Takes({ Pointer(Place::X0, -1, "ls::FileWriter") }, Returns::Nothing, nullptr,
            "it stands in for the Windows API's CloseHandle: its argument is the file writer"),
        .Shape = { 140, 0xfa8ac8c010030c45ull },     // 4.1.1.7631656
    },
};

}

std::span<Entry const> OsirisEntries()
{
    return kEntries;
}

}
