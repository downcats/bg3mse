#include <Engine/Entries/Areas.h>
#include <Layout/GameMembers.h>
#include <Layout/GameStats.h>

// Stats: the stats object, the prototype managers, the functor executors and
// damage.
//
// The functor executors and the damage functions build their result in memory
// of the caller. In this game the address of that memory arrives as the first
// ordinary argument, in x0, not in x8 as a result address usually does: the
// same order as the Windows prototypes Norbyte declares, which spell it as an
// explicit first parameter.

namespace bg3se::mac::engine {

namespace {

constexpr char kStart[] = "__ZN8RPGStats5StartERKN2ls9ObjectSetINS0_4PathELm0ENS0_17DefaultComparatorIS2_EEEE";
constexpr char kParseDataBuffer[] = "__ZN8RPGStats15ParseDataBufferEN2ls9UniquePtrI19RPGStats_DataBuffervEE";
constexpr char kLoadStatsJob[] = "__ZN2ls8FunctionIFvvEE18FunctorMethodsVSSOIZN3ecl16ModuleLoadSystem11DoLoadStatsEvE3$_6E4CallERNS2_7StorageE";
constexpr char kDealDamage[] = "__ZN3esv7functor22StatsFunctorDealDamage7ExecuteERKN3eoc22StatsFunctorDealDamageERKN3ecs9EntityRefES9_RK8Vector3fb";
constexpr char kApplyDamage[] = "__ZN3esv11StatsSystem11ApplyDamageERKN3ecs13EntityRefViewIJKN3eoc20ResistancesComponentE";

constexpr char kParseCallName[] = "RPGStats::Start: call of RPGStats::ParseDataBuffer";

constexpr Named kStartHosts[] = { Exact(kStart) };
constexpr Named kLoadStatsJobHosts[] = { Exact(kLoadStatsJob) };
constexpr Named kParseDataBufferHosts[] = { Exact(kParseDataBuffer) };
constexpr Named kStaticBoostHosts[] = {
    Exact("__ZN3eoc11FeatManager8PostInitEv"),
    Exact("__ZN3eoc18ProgressionManager8PostInitEv"),
    Exact("__ZN3eoc22StatusPrototypeManager4InitEv"),
    Exact(kLoadStatsJob),
};
constexpr char const* kParseBuffersReplacements[] = { kParseCallName };

// eoc::ParseStaticBoosts: what each of its four callers has in its place is a
// function object of the lambda `$_1`, handed to (anonymous
// namespace)::ParseBoosts (Binding/StatsFunctions.cpp says how). The object
// the status prototypes' loader builds, and what it does with it afterwards,
// is what the stand-in for the function was written from.
constexpr char kParseBoosts[] = "__ZN12_GLOBAL__N_111ParseBoostsERKN2ls11_StringViewIcEENS0_8FunctionIFvS4_RKNS0_12DynamicArrayIS2_NS0_15TaggedAllocatorIiEEEERKNS0_11FixedStringES4_EEE";
#define BOOST_FUNCTION(method) \
    "__ZN2ls8FunctionIFvRKNS_11_StringViewIcEERKNS_12DynamicArrayIS2_NS_15TaggedAllocatorIiEEEERKNS_11FixedStringES4_EE18FunctorMethodsVSSO" \
    "IZN3eoc17ParseStaticBoostsES4_RNS5_INS_4GuidES7_EENS0_IFbSD_EEEE3$_1E" method
constexpr char kBoostCall[] = BOOST_FUNCTION("4CallERNSF_7StorageES4_SA_SD_S4_");
constexpr char kBoostCopy[] = BOOST_FUNCTION("4CopyEPNSF_11MethodTableERKNSF_7StorageERSR_");
constexpr char kBoostMoveDestroy[] = BOOST_FUNCTION("11MoveDestroyEPNSF_11MethodTableERNSF_7StorageEPSR_");
constexpr char kParseBoostsName[] = "(anonymous namespace)::ParseBoosts";
constexpr char kBoostCallName[] = "eoc::ParseStaticBoosts: the boost function's Call";
constexpr char kBoostCopyName[] = "eoc::ParseStaticBoosts: the boost function's Copy";
constexpr char kBoostMoveDestroyName[] = "eoc::ParseStaticBoosts: the boost function's MoveDestroy";
constexpr char const* kStaticBoostReplacements[] = { kParseBoostsName, kBoostCallName, kBoostCopyName, kBoostMoveDestroyName };
// Of the four hosts, the status prototypes' loader is the one read.
constexpr CodeShape kStaticBoostHostShapes[] = { {}, {}, { 3435, 0xc8fec515cb9adbb2ull }, {} };     // 4.1.1.7631656
constexpr char kStatusPrototypesInit[] = "__ZN3eoc22StatusPrototypeManager4InitEv";
constexpr Operand kStaticBoostOperands[] = {
    { .What = "the status loader hands the boosts to ParseBoosts", .In = Exact(kStatusPrototypesInit), .How = Operand::Read::CallCount,
      .Expected = 1, .Target = Exact(kParseBoosts) },
};

constexpr Derivation kParseCall{
    .Host = Exact(kStart),
    .HostShape = { 6577, 0xc169255f12e8eb87ull },     // 4.1.1.7631656
    .Anchor = Calls(Exact(kParseDataBuffer)),
};

constexpr Operand kStartCalls[] = {
    { .What = "called by the stats job", .In = Exact(kLoadStatsJob), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kStart) },
};
// eoc::Passives::Get walks the map of prototypes: the offsets it uses are
// those of Norbyte's PassivePrototypeManager.
//     mov x8, x0 ; ldr w0, [x0, #HashSize] ; cbz w0
//     msub w9, w9, w0, w1 ; ldr x1, [x8, #HashTable]
//     ldr w9, [x8, #Key] ; cmp w9, w2
constexpr char kPassivesGet[] = "__ZNK3eoc8Passives3GetERKN2ls11FixedStringE";
constexpr MaskedWord kPassivesHashSize[] = { { 0xaa0003e8 }, { 0xb9400000, Any::Imm12 }, { 0x34000000, Any::CondBranch } };
constexpr MaskedWord kPassivesHashTable[] = { { 0x1b008529 }, { 0xf9400101, Any::Imm12 } };
constexpr MaskedWord kPassivesNodeKey[] = { { 0xb9400109, Any::Imm12 }, { 0x6b02013f } };
constexpr Operand kPassivesOperands[] = {
    { .What = "HashSize", .In = Exact(kPassivesGet), .Pattern = kPassivesHashSize, .At = 1, .Expected = kPassivesMapHashSize },
    { .What = "HashTable", .In = Exact(kPassivesGet), .Pattern = kPassivesHashTable, .At = 1, .Expected = kPassivesMapHashTable },
    { .What = "a node's key", .In = Exact(kPassivesGet), .Pattern = kPassivesNodeKey, .At = 0, .Expected = kPassiveNodeKey },
};

constexpr Operand kApplyDamageCalls[] = {
    { .What = "called by the damage functor", .In = Prefix(kDealDamage), .How = Operand::Read::CallCount, .Expected = 1, .Target = Prefix(kApplyDamage) },
};

#define EXECUTOR(context) "__ZN3esv7functor20ExecuteStatsFunctorsEPKN3eoc16StatsFunctorListERNS0_" context "E"

constexpr Signature kExecutor = Takes({
    Pointer(Place::X0, 0, "the hit result to fill, in the caller's memory"),
    Pointer(Place::X1, 1, "eoc::StatsFunctorList const"),
    Pointer(Place::X2, 2, "the context data"),
}, Returns::IndirectX0, "the hit result");

#define EXECUTOR_ENTRY(name, context, mac, instructions, hash) \
    { \
        .Name = name, \
        .What = Kind::FunctionToHook, \
        .Mac = "esv::functor::ExecuteStatsFunctors(eoc::StatsFunctorList const*, esv::functor::" mac "&)", \
        .Use = "GameHooks/EngineHooks.inl and Lua/Server/FunctorEvents.inl: the functors of a list run for this kind of context", \
        .Where = Exact(EXECUTOR(context)), \
        .Call = kExecutor, \
        .Shape = { instructions, hash }, \
    }

// stats::Object::SetPropertyString: inline in RPGStats::ParseDataBuffer, where
// a `data` line is applied (Binding/StatsFunctions.cpp says what it is made
// of). Every game function that code calls is an entry below.
constexpr char kSetPropertyUse[] = "what stats::Object::SetPropertyString is inlined around (Binding/StatsFunctions.cpp)";
#define MODIFIERS "__ZNK20CNamedElementManagerI18CRPGStats_ModifieriLi0ELin1EE"
constexpr char kModifierHandleByName[] = MODIFIERS "15GetHandleByNameERKN2ls11FixedStringE";
constexpr char kModifierByHandle[] = MODIFIERS "11GetByHandleEi";
constexpr VtableMethod kModifierListSlots[] = {
    { "GetByHandle", Exact(kModifierByHandle), 0x20 },
    { "GetHandleByName", Exact(kModifierHandleByName), 0x28 },
};
#define FUNCTORS "20CNamedElementManagerIN3eoc16StatsFunctorBaseEiLi0ELin1EE"
constexpr VtableMethod kFunctorListSlots[] = {
    { "Destroy", Exact("__ZN" FUNCTORS "7DestroyEv"), kNamedManagerDestroySlot },
    { "Insert", Exact("__ZN" FUNCTORS "6InsertEPS1_"), kFunctorListInsertSlot },
    { "GetAmountOfEntries", Exact("__ZNK" FUNCTORS "18GetAmountOfEntriesEv"), kNamedManagerCountSlot },
    { "GetEntry", Exact("__ZN" FUNCTORS "8GetEntryEm"), kNamedManagerEntrySlot },
    { "GetEntry, of a list that is not changed", Exact("__ZNK" FUNCTORS "8GetEntryEm"), kNamedManagerConstEntrySlot },
};
#define FUNCTOR_GROUPS "23TextKeyStatsFunctorList"
#define ROLL_CONDITIONS "20TextKeyRollCondition"
#define ARRAY_OF(element, method) "__ZN2ls12DynamicArrayI" element "NS_15TaggedAllocatorIiEEE" method
#define MAP_SET(element) \
    "__ZN2ls7HashMapINS_11FixedStringENS_12DynamicArrayI" element "NS_15TaggedAllocatorIiEEEENS_19HashTableOpsDefaultEE12SetUniversalIRKS1_RKS6_EENS_7EMapSetEOT_OT0_"
constexpr Signature kArrayReallocate = Takes({ Pointer(Place::X0, -1, "the array"), Value(Place::X1, 4, -1, "int: the new capacity") });
constexpr Signature kArrayEnd = Takes({ Pointer(Place::X0, -1, "the array") }, Returns::X0, "the array");
constexpr Signature kArrayMapSet = Takes({
    Pointer(Place::X0, -1, "the map"),
    Pointer(Place::X1, -1, "ls::FixedString const: the key"),
    Pointer(Place::X2, -1, "the array, which is copied"),
}, Returns::W0, "ls::EMapSet");
constexpr Signature kSplitPerTextKey = Takes({
    Pointer(Place::X0, -1, "the result: an ls::HashMap<ls::FixedString, ls::STDString>, in the caller's memory"),
    Pointer(Place::X1, -1, "ls::STDString const: the text"),
    Pointer(Place::X2, -1, "ls::FixedString const: the key of text that names none"),
}, Returns::IndirectX0, "the map");
constexpr char const* kSetPropertyReplacements[] = {
    "CNamedElementManager<CRPGStats_Modifier>::GetHandleByName", "CNamedElementManager<CRPGStats_Modifier>::GetByHandle",
    "CRPGStats_Modifier::GetPosition", "RPGStats::AddCondition", "ParseRequirements",
    "eoc::TextKeyParsingHelpers::SplitStringPerTextKeyWithMerge", "eoc::TextKeyParsingHelpers::SplitStringPerTextKeyNoMerge",
    "eoc::StatsFunctorListManager::CreateStatsFunctorList", "eoc::StatsFunctorBase::ParseStatsFunctor",
};
constexpr CodeShape kParseDataBufferShapes[] = { { 2567, 0x848021d1465610d6ull } };     // 4.1.1.7631656
constexpr Operand kSetPropertyOperands[] = {
    { .What = "positions are taken", .In = Exact(kParseDataBuffer), .How = Operand::Read::CallCount, .Expected = 2,
      .Target = Exact("__ZN18CRPGStats_Modifier11GetPositionEPKc") },
    { .What = "conditions are added", .In = Exact(kParseDataBuffer), .How = Operand::Read::CallCount, .Expected = 4,
      .Target = Exact("__ZN8RPGStats12AddConditionERKN2ls9STDStringE") },
    { .What = "functors are parsed", .In = Exact(kParseDataBuffer), .How = Operand::Read::CallCount, .Expected = 2,
      .Target = Exact("__ZN3eoc16StatsFunctorBase17ParseStatsFunctorERKN2ls9STDStringEm") },
};

// eoc::PassivePrototype::Init and eoc::InterruptPrototype::Init: inline in the
// job that loads the stats (Binding/StatsFunctions.cpp says what each is made
// of). The job's whole shape is what they were read at.
constexpr CodeShape kLoadStatsJobShapes[] = { { 4987, 0x89aed80643ba9edaull } };     // 4.1.1.7631656
constexpr char kPrototypeUse[] = "what eoc::PassivePrototype::Init and eoc::InterruptPrototype::Init are inlined around (Binding/StatsFunctions.cpp)";
constexpr char kPassiveClean[] = "__ZN3eoc16PassivePrototype5CleanEv";
constexpr char kDescriptionFromObject[] = "__ZN3eoc16StatsDescription18SetFromStatsObjectERK11StatsObject";
constexpr char kParseResourceCosts[] =
    "__ZN3eoc15action_resource32ParseActionResourceCostWithErrorERKN2ls11_StringViewIcEERNS1_12DynamicArrayINS0_18ActionResourceCostENS1_15TaggedAllocatorIiEEEE";
constexpr char kInterruptEnd[] = "__ZN3eoc18InterruptPrototypeD1Ev";
constexpr char const* kPassiveInitReplacements[] = {
    "eoc::PassivePrototype::Clean", "eoc::StatsDescription::SetFromStatsObject", kParseBoostsName,
};
constexpr char const* kInterruptInitReplacements[] = {
    "eoc::StatsDescription::SetFromStatsObject", "eoc::action_resource::ParseActionResourceCostWithError",
};
constexpr Operand kPassiveInitOperands[] = {
    { .What = "a passive is cleaned once", .In = Exact(kLoadStatsJob), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kPassiveClean) },
    { .What = "a passive's and an interrupt's description", .In = Exact(kLoadStatsJob), .How = Operand::Read::CallCount, .Expected = 2,
      .Target = Exact(kDescriptionFromObject) },
    { .What = "a passive's boosts", .In = Exact(kLoadStatsJob), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kParseBoosts) },
};
constexpr Operand kInterruptInitOperands[] = {
    { .What = "a passive's and an interrupt's description", .In = Exact(kLoadStatsJob), .How = Operand::Read::CallCount, .Expected = 2,
      .Target = Exact(kDescriptionFromObject) },
    { .What = "an interrupt's costs", .In = Exact(kLoadStatsJob), .How = Operand::Read::CallCount, .Expected = 1,
      .Target = Exact(kParseResourceCosts) },
};
// The name of an interrupt's Conditions attribute is one of several
// variables of that name in the game (the others have a numeric suffix that
// the linker gave them), so it is taken from the job's code, where the
// attribute is read into the prototype's Conditions:
//     adrp x8, page ; ldr x1, [x8, #offset] ; ldr x8, [x0] ; ldr x8, [x8, #0x28] ; blr x8 ; tbnz w0, #31
//     ldp x8, x9, [x22, #8] ; sub x9, x9, x8 ; lsr x9, x9, #2 ; cmp w0, w9 ; b.le
//     mov w8, #-1 ; ldr w10, [x22, #0xe4] ; str w8, [x23, #Conditions]
constexpr MaskedWord kInterruptConditionsRead[] = {
    { 0x90000008, Any::Adrp }, { 0xf9400101, Any::Imm12 }, { 0xf9400008 }, { 0xf9401508 }, { 0xd63f0100 }, { 0x37f80000, Any::CondBranch },
    { 0xa940a6c8 }, { 0xcb080129 }, { 0xd342fd29 }, { 0x6b09001f }, { 0x5400000d, Any::CondBranch },
    { 0x12800008 }, { 0xb940e6ca }, { 0xb90002e8, Any::Imm12 },
};
constexpr Derivation kInterruptConditionsName{
    .Host = Exact(kLoadStatsJob),
    .HostShape = { 4987, 0x89aed80643ba9edaull },       // 4.1.1.7631656
    .Pattern = kInterruptConditionsRead,
    .What = Derivation::Take::PageAddress,
    .At = 0,
};
constexpr Operand kInterruptConditionsOperands[] = {
    { .What = "read into the prototype's Conditions", .In = Exact(kLoadStatsJob), .Pattern = kInterruptConditionsRead, .At = 13,
      .Expected = kInterruptConditions },
};

// A variable that holds a reference to the fixed string of an attribute's name.
#define ATTRIBUTE_NAME(name, text, symbol, ...) \
    { .Name = name, .What = Kind::GlobalPointer, .Mac = name ": a reference to the fixed string \"" text "\"", \
      .Use = "the name of the attribute as the job that loads the stats passes it", .Where = Exact(symbol), __VA_ARGS__ }
// Another file of the game has a variable of the same name, which the linker
// gave a numeric suffix: the one without a suffix is the interrupts'.
constexpr char kSuffixedSibling[] =
    "another variable of this name exists with a numeric suffix the linker gave it; this one, without a suffix, is the one the job "
    "that loads the stats reads for an interrupt (read from its code in build 4.1.1.7631656, and not checked against it here)";

#define STATS_NAME(name, text, symbol) \
    { .Name = name, .What = Kind::GlobalObject, .Mac = name, \
      .Use = "the fixed string \"" text "\": the name of the list of values whose attributes are set in a way of their own", \
      .Where = Exact(symbol), .Bytes = 4 }
#define STATS_CALLED(name, mac, symbol, call, instructions, hash) \
    { .Name = name, .What = Kind::FunctionToCall, .Mac = mac, .Use = kSetPropertyUse, .Where = Exact(symbol), .Call = call, \
      .Shape = { instructions, hash } }

constexpr Entry kEntries[] = {
    {
        .Name = "gRPGStats",
        .What = Kind::GlobalPointer,
        .Mac = "RPGStats::m_ptr",
        .Use = "everywhere in the stats code: the stats object",
        .Where = Exact("__ZN8RPGStats5m_ptrE"),
    },
    {
        .Name = "RPGStats__Load",
        .What = Kind::FunctionToHook,
        .Mac = "RPGStats::Start(ls::ObjectSet<ls::Path, 0ul, ls::DefaultComparator<ls::Path>> const&)",
        .Use = "GameHooks/EngineHooks.inl (OnStatsLoad): a wrapper around the loading of all stats files",
        .Where = Exact(kStart),
        .Call = Takes({
            Pointer(Place::X0, 0, "RPGStats"),
            Pointer(Place::X1, 1, "ls::ObjectSet<ls::Path> const: { buffer, 32-bit capacity, 32-bit size }, 16-byte elements"),
        }, Returns::Nothing, nullptr,
            "Norbyte declares the second parameter as Array<STDString>*; here it is a set of ls::Path. A wrapper that only passes "
            "it on is unaffected"),
        .Shape = { 6577, 0xc169255f12e8eb87ull },     // 4.1.1.7631656
        .Operands = kStartCalls,
    },
    {
        .Name = "RPGStats__ParseDataBuffers",
        .What = Kind::NoCallableCode,
        .Use = "Extender/Shared/Hooks.cpp: a pre-hook for \"every stats file is read and none is parsed yet\"",
        .Gone = {
            .Fate = "inlined into RPGStats::Start as a loop over the buffers that follows the loop reading the files; the loop calls "
                    "RPGStats::ParseDataBuffer (singular) once for each buffer, and that function also calls itself",
            .NoSymbol = "__ZN8RPGStats16ParseDataBuffers",
            .Hosts = kStartHosts,
            .Replacements = kParseBuffersReplacements,
        },
    },
    {
        .Name = kParseCallName,
        .What = Kind::CallSite,
        .Mac = "the one call of RPGStats::ParseDataBuffer in RPGStats::Start",
        .Use = "takes the place of the RPGStats__ParseDataBuffers pre-hook",
        .Derived = &kParseCall,
        .Needs = "RPGStats::ParseDataBuffer",
        .Caveat = "the call is in a loop: it runs once for each buffer, and not at all when there is no buffer or a stats file "
                  "could not be opened. Norbyte's hook runs once, before the first buffer: the replacement has to tell the first "
                  "call after each RPGStats::Start from the rest",
    },
    {
        .Name = "RPGStats::ParseDataBuffer",
        .What = Kind::FunctionToCall,
        .Mac = "RPGStats::ParseDataBuffer(ls::UniquePtr<RPGStats_DataBuffer, void>)",
        .Use = "what the call site above calls: its replacement calls this",
        .Where = Exact(kParseDataBuffer),
        .Call = Takes({
            Pointer(Place::X0, -1, "the result: a pointer to the parsed StatsObject, in the caller's memory"),
            Pointer(Place::X1, 0, "RPGStats"),
            Pointer(Place::X2, -1, "ls::UniquePtr<RPGStats_DataBuffer>: the buffer, taken over"),
        }, Returns::IndirectX0, "StatsObject*",
            "it stands in for RPGStats__ParseDataBuffers, declared bool (RPGStats*): here the stats object is the second argument, "
            "after the address of the result, and the buffer is the third"),
        .Shape = { 2567, 0x848021d1465610d6ull },     // 4.1.1.7631656
        .CallsItself = true,
    },
    {
        .Name = "stats__Object__SetPropertyString",
        .What = Kind::NoCallableCode,
        .Use = "Lua/Libs/StatAttributes.inl: set one attribute of a stats object from its text, for the attribute types Norbyte "
               "does not set itself",
        .Gone = {
            .Fate = "the stats object (StatsObject here) has no set-by-text function: the assignment for each attribute type is "
                    "inlined into RPGStats::ParseDataBuffer, the only caller of StatsObject::AddCombinationCategory",
            .Hosts = kParseDataBufferHosts,
            .HostShapes = kParseDataBufferShapes,
            .Replacements = kSetPropertyReplacements,
        },
        .Operands = kSetPropertyOperands,
    },
    STATS_CALLED("CNamedElementManager<CRPGStats_Modifier>::GetHandleByName",
        "CNamedElementManager<CRPGStats_Modifier, int, 0, -1>::GetHandleByName(ls::FixedString const&) const", kModifierHandleByName,
        Takes({ Pointer(Place::X0, -1, "the list of modifiers"), Pointer(Place::X1, -1, "ls::FixedString const: an attribute's name") },
            Returns::W0, "int: the attribute's handle, -1 when the list has none of that name"), 30, 0xb68a07b0638e4797ull),     // 4.1.1.7631656
    STATS_CALLED("CNamedElementManager<CRPGStats_Modifier>::GetByHandle",
        "CNamedElementManager<CRPGStats_Modifier, int, 0, -1>::GetByHandle(int) const", kModifierByHandle,
        Takes({ Pointer(Place::X0, -1, "the list of modifiers"), Value(Place::X1, 4, -1, "int: a handle") },
            Returns::X0, "CRPGStats_Modifier*, or null"), 9, 0x681b05a7ae2590bcull),     // 4.1.1.7631656
    {
        .Name = "CRPGStats_Modifier_List",
        .What = Kind::Vtable,
        .Mac = "vtable for CRPGStats_Modifier_List",
        .Use = "the two functions above are what a list of modifiers has in the slots the inlined code calls",
        .Where = Exact("__ZTV23CRPGStats_Modifier_List"),
        .Methods = kModifierListSlots,
    },
    STATS_CALLED("CRPGStats_Modifier::GetPosition", "CRPGStats_Modifier::GetPosition(char const*)", "__ZN18CRPGStats_Modifier11GetPositionEPKc",
        Takes({
            Value(Place::X0, 4, -1, "int: the index of the modifier's list of values; not a modifier"),
            Pointer(Place::X1, -1, "char const: the text"),
        }, Returns::W0, "int: the number, the label's value, or the text's position in the stats' pool for the type; -1 for none"), 503, 0x5052d3d5f3538059ull),     // 4.1.1.7631656
    STATS_CALLED("RPGStats::AddCondition", "RPGStats::AddCondition(ls::STDString const&)", "__ZN8RPGStats12AddConditionERKN2ls9STDStringE",
        Takes({ Pointer(Place::X0, -1, "RPGStats"), Pointer(Place::X1, -1, "ls::STDString const: the condition's text") },
            Returns::W0, "int: the condition's index"), 89, 0x2e03440facfe86cfull),     // 4.1.1.7631656
    STATS_CALLED("ParseRequirements", "ParseRequirements(ls::STDString const&, ls::DynamicArray<Requirement>&)",
        "__Z17ParseRequirementsRKN2ls9STDStringERNS_12DynamicArrayI11RequirementNS_15TaggedAllocatorIiEEEE",
        Takes({ Pointer(Place::X0, -1, "ls::STDString const: the text"), Pointer(Place::X1, -1, "the requirements, emptied first") }), 423, 0x0d02054bbefed63aull),     // 4.1.1.7631656
    STATS_CALLED("ls::STDString::STDString(char const*)", "ls::STDString::STDString(char const*)", "__ZN2ls9STDStringC1EPKc",
        Takes({ Pointer(Place::X0, -1, "the string"), Pointer(Place::X1, -1, "char const") }, Returns::X0, "the string"), 96, 0xd61a26b75bc0a0aeull),     // 4.1.1.7631656
    STATS_CALLED("ls::STDString::STDString(ls::_StringView<char> const&)", "ls::STDString::STDString(ls::_StringView<char> const&)",
        "__ZN2ls9STDStringC1ERKNS_11_StringViewIcEE",
        Takes({
            Pointer(Place::X0, -1, "the string"),
            Pointer(Place::X1, -1, "char const: the text; the view arrives as its two members"),
            Value(Place::X2, 4, -1, "the text's length"),
        }, Returns::X0, "the string"), 71, 0x88a3a733437ead75ull),     // 4.1.1.7631656
    STATS_CALLED("ls::STDString::TrimWhiteSpaceLeftAndRight", "ls::STDString::TrimWhiteSpaceLeftAndRight()",
        "__ZN2ls9STDString26TrimWhiteSpaceLeftAndRightEv", Takes({ Pointer(Place::X0, -1, "the string") }, Returns::X0, "the string"), 161, 0xffae31ee641ef3cbull),     // 4.1.1.7631656
    STATS_CALLED("ls::STDString::~STDString", "ls::STDString::~STDString()", "__ZN2ls9STDStringD1Ev",
        Takes({ Pointer(Place::X0, -1, "the string") }, Returns::X0, "the string"), 36, 0x0492b774a70ff077ull),     // 4.1.1.7631656
    STATS_CALLED("eoc::TextKeyParsingHelpers::SplitStringPerTextKeyWithMerge",
        "eoc::TextKeyParsingHelpers::SplitStringPerTextKeyWithMerge(ls::STDString const&, ls::FixedString const&)",
        "__ZN3eoc21TextKeyParsingHelpers30SplitStringPerTextKeyWithMergeERKN2ls9STDStringERKNS1_11FixedStringE", kSplitPerTextKey, 64, 0x1da58846c1ceb75dull),     // 4.1.1.7631656
    STATS_CALLED("eoc::TextKeyParsingHelpers::SplitStringPerTextKeyNoMerge",
        "eoc::TextKeyParsingHelpers::SplitStringPerTextKeyNoMerge(ls::STDString const&, char const*, ls::FixedString const&): the "
        "second parameter is not passed",
        "__ZN3eoc21TextKeyParsingHelpers28SplitStringPerTextKeyNoMergeERKN2ls9STDStringEPKcRKNS1_11FixedStringE", kSplitPerTextKey, 65, 0x1639aa8104119f5full),     // 4.1.1.7631656
    {
        .Name = "eoc::strDefault",
        .What = Kind::GlobalPointer,
        .Mac = "eoc::strDefault: a reference to the fixed string \"Default\"",
        .Use = "the text key of text that names none, as the inlined code passes it to the two functions above",
        .Where = Exact("__ZN3eoc10strDefaultE"),
    },
    STATS_CALLED("ls::HashMap<ls::FixedString, ls::STDString>::~HashMap", "ls::HashMap<ls::FixedString, ls::STDString, ls::HashTableOpsDefault>::~HashMap()",
        "__ZN2ls7HashMapINS_11FixedStringENS_9STDStringENS_19HashTableOpsDefaultEED1Ev",
        Takes({ Pointer(Place::X0, -1, "the map") }, Returns::X0, "the map"), 88, 0x5edf24f1d3e0e554ull),     // 4.1.1.7631656
    STATS_CALLED("ls::Split<2>", "ls::Split<2>(ls::_StringView<char> const&, ls::_StringView<char> const&, bool)",
        "__ZN2ls5SplitILi2EEENS_11StaticArrayINS_11_StringViewIcEEXT_EEERKS3_S6_b",
        Takes({
            Pointer(Place::X0, -1, "ls::_StringView<char> const: the text"),
            Pointer(Place::X1, -1, "ls::_StringView<char> const: the delimiters"),
            Value(Place::X2, 1, -1, "bool"),
        }, Returns::IndirectX8, "two views: the text before the first delimiter, and the rest"), 146, 0xa07f27fed637a38dull),     // 4.1.1.7631656
    STATS_CALLED("eoc::StatsFunctorListManager::CreateStatsFunctorList",
        "eoc::StatsFunctorListManager::CreateStatsFunctorList(StatsObject const*, ls::FixedString const&, ls::FixedString const&)",
        "__ZN3eoc23StatsFunctorListManager22CreateStatsFunctorListEPK11StatsObjectRKN2ls11FixedStringES7_",
        Takes({
            Pointer(Place::X0, -1, "the stats' functor lists"),
            Pointer(Place::X1, -1, "StatsObject const"),
            Pointer(Place::X2, -1, "ls::FixedString const: the attribute's name"),
            Pointer(Place::X3, -1, "ls::FixedString const: the text key"),
        }, Returns::X0, "eoc::StatsFunctorList*: named <object>_<attribute>_<text key>"), 463, 0xc394f80b9447feb6ull),     // 4.1.1.7631656
    STATS_CALLED("eoc::StatsFunctorBase::ParseStatsFunctor", "eoc::StatsFunctorBase::ParseStatsFunctor(ls::STDString const&, unsigned long)",
        "__ZN3eoc16StatsFunctorBase17ParseStatsFunctorERKN2ls9STDStringEm",
        Takes({ Pointer(Place::X0, -1, "ls::STDString const: one functor's text"), Value(Place::X1, 8, -1, "its number among the attribute's") },
            Returns::X0, "eoc::StatsFunctorBase*, or null"), 1252, 0x3f20c8bcb9395920ull),     // 4.1.1.7631656
    {
        .Name = "eoc::StatsFunctorList",
        .What = Kind::Vtable,
        .Mac = "vtable for eoc::StatsFunctorList",
        .Use = "the slot through which the inlined code gives a list each functor it parsed",
        .Where = Exact("__ZTVN3eoc16StatsFunctorListE"),
        .Methods = kFunctorListSlots,
    },
    STATS_CALLED("ls::FixedString::Create", "ls::FixedString::Create(char const*, int)", "__ZN2ls11FixedString6CreateEPKci",
        Takes({
            Pointer(Place::X0, -1, "the result: an ls::FixedString, in the caller's memory"),
            Pointer(Place::X1, -1, "char const: the text"),
            Value(Place::X2, 4, -1, "int: its length, or below zero for a C string's"),
        }, Returns::IndirectX0, "ls::FixedString"), 28, 0x7fb6677389c24086ull),     // 4.1.1.7631656
    STATS_CALLED("ls::FixedString::operator=(ls::FixedString&&)", "ls::FixedString::operator=(ls::FixedString&&)", "__ZN2ls11FixedStringaSEOS0_",
        Takes({ Pointer(Place::X0, -1, "the string assigned to"), Pointer(Place::X1, -1, "the string moved from") }, Returns::X0, "the string"), 26, 0xeadc9c013e7ac13aull),     // 4.1.1.7631656
    STATS_CALLED("ls::FixedString::~FixedString", "ls::FixedString::~FixedString()", "__ZN2ls11FixedStringD1Ev",
        Takes({ Pointer(Place::X0, -1, "the string") }, Returns::X0, "the string"), 21, 0xbf0aee187e1501f7ull),     // 4.1.1.7631656
    STATS_CALLED("ls::DynamicArray<TextKeyStatsFunctorList>::Reallocate", "ls::DynamicArray<TextKeyStatsFunctorList>::Reallocate(int)",
        ARRAY_OF(FUNCTOR_GROUPS, "10ReallocateEi"), kArrayReallocate, 194, 0x1e2f1e3183aabd2bull),     // 4.1.1.7631656
    STATS_CALLED("ls::DynamicArray<TextKeyStatsFunctorList>::~DynamicArray", "ls::DynamicArray<TextKeyStatsFunctorList>::~DynamicArray()",
        ARRAY_OF(FUNCTOR_GROUPS, "D2Ev"), kArrayEnd, 120, 0xa4d43f4841d3dce6ull),     // 4.1.1.7631656
    STATS_CALLED("ls::HashMap<ls::FixedString, ls::DynamicArray<TextKeyStatsFunctorList>>::SetUniversal",
        "ls::HashMap<ls::FixedString, ls::DynamicArray<TextKeyStatsFunctorList>>::SetUniversal(ls::FixedString const&, the array const&)",
        MAP_SET(FUNCTOR_GROUPS), kArrayMapSet, 303, 0xc3f217846f9709a1ull),     // 4.1.1.7631656
    STATS_CALLED("ls::DynamicArray<TextKeyRollCondition>::Reallocate", "ls::DynamicArray<TextKeyRollCondition>::Reallocate(int)",
        ARRAY_OF(ROLL_CONDITIONS, "10ReallocateEi"), kArrayReallocate, 194, 0x933404e4f142e82dull),     // 4.1.1.7631656
    STATS_CALLED("ls::DynamicArray<TextKeyRollCondition>::~DynamicArray", "ls::DynamicArray<TextKeyRollCondition>::~DynamicArray()",
        ARRAY_OF(ROLL_CONDITIONS, "D2Ev"), kArrayEnd, 120, 0x59182884b5e34a06ull),     // 4.1.1.7631656
    STATS_CALLED("ls::HashMap<ls::FixedString, ls::DynamicArray<TextKeyRollCondition>>::SetUniversal",
        "ls::HashMap<ls::FixedString, ls::DynamicArray<TextKeyRollCondition>>::SetUniversal(ls::FixedString const&, the array const&)",
        MAP_SET(ROLL_CONDITIONS), kArrayMapSet, 309, 0x7309e4af6219be62ull),     // 4.1.1.7631656
    STATS_NAME("EoCFS::strStatsFunctors", "StatsFunctors", "__ZN5EoCFS16strStatsFunctorsE"),
    STATS_NAME("EoCFS::strConditions", "Conditions", "__ZN5EoCFS13strConditionsE"),
    STATS_NAME("EoCFS::strTargetConditions", "TargetConditions", "__ZN5EoCFS19strTargetConditionsE"),
    STATS_NAME("EoCFS::strUseConditions", "UseConditions", "__ZN5EoCFS16strUseConditionsE"),
    STATS_NAME("EoCFS::strRollConditions", "RollConditions", "__ZN5EoCFS17strRollConditionsE"),
    STATS_NAME("EoCFS::strAIFlags", "AIFlags", "__ZN5EoCFS10strAIFlagsE"),
    STATS_NAME("EoCFS::strConstantInt", "ConstantInt", "__ZN5EoCFS14strConstantIntE"),
    STATS_NAME("EoCFS::strRequirements", "Requirements", "__ZN5EoCFS15strRequirementsE"),

    {
        .Name = "eoc__SpellPrototypeManager",
        .What = Kind::GlobalPointer,
        .Mac = "eoc::SpellPrototypeManager::m_ptr",
        .Use = "GameDefinitions/Stats/Stats.cpp and Lua/Libs/Stats.inl",
        .Where = Exact("__ZN3eoc21SpellPrototypeManager5m_ptrE"),
    },
    {
        .Name = "eoc__SpellPrototype__Init",
        .What = Kind::FunctionToCall,
        .Mac = "eoc::SpellPrototype::Init(ls::FixedString const&)",
        .Use = "GameDefinitions/Stats/Stats.cpp: fill a spell prototype again from its stats entry",
        .Where = Exact("__ZN3eoc14SpellPrototype4InitERKN2ls11FixedStringE"),
        .Call = Takes({ Pointer(Place::X0, 0, "eoc::SpellPrototype"), Pointer(Place::X1, 1, "ls::FixedString const: the spell's name") }),
        .Shape = { 4446, 0x6bf6520cf6f46c25ull },     // 4.1.1.7631656
    },
    {
        .Name = "eoc__StatusPrototypeManager",
        .What = Kind::GlobalPointer,
        .Mac = "eoc::StatusPrototypeManager::m_ptr",
        .Use = "GameDefinitions/Stats/Stats.cpp and Lua/Libs/Stats.inl",
        .Where = Exact("__ZN3eoc22StatusPrototypeManager5m_ptrE"),
    },
    {
        .Name = "eoc__StatusPrototype__Init",
        .What = Kind::FunctionToCall,
        .Mac = "eoc::StatusPrototype::Init(ls::FixedString const&, bool)",
        .Use = "GameDefinitions/Stats/Stats.cpp: fill a status prototype again from its stats entry",
        .Where = Exact("__ZN3eoc15StatusPrototype4InitERKN2ls11FixedStringEb"),
        .Call = Takes({
            Pointer(Place::X0, 0, "eoc::StatusPrototype"),
            Pointer(Place::X1, 1, "ls::FixedString const: the status's name"),
            Value(Place::X2, 1, 2, "bool"),
        }),
        .Shape = { 1236, 0x5111711387fade85ull },     // 4.1.1.7631656
    },
    {
        .Name = "eoc__BoostPrototypeManager",
        .What = Kind::GlobalPointer,
        .Mac = "eoc::BoostPrototypeManager::m_ptr",
        .Use = "Lua/Libs/Stats.inl",
        .Where = Exact("__ZN3eoc21BoostPrototypeManager5m_ptrE"),
    },
    {
        .Name = "eoc__PassivePrototypeManager",
        .What = Kind::GlobalPointer,
        .Mac = "eoc::Passives::m_ptr: the class is eoc::Passives, and eoc::Passives::Get reads its map of prototypes at the offsets "
               "of Norbyte's PassivePrototypeManager",
        .Use = "GameDefinitions/Stats/Stats.cpp and Lua/Libs/Stats.inl",
        .Where = Exact("__ZN3eoc8Passives5m_ptrE"),
        .Operands = kPassivesOperands,
        .Caveat = "the game finds a prototype's bucket by the fixed string's index modulo the hash size; that Norbyte's map hashes "
                  "a fixed string the same way was not compared, and neither was the layout of a prototype",
    },
    {
        .Name = "eoc__PassivePrototype__Init",
        .What = Kind::NoCallableCode,
        .Use = "GameDefinitions/Stats/Stats.cpp (PassivePrototypeManager::SyncStat): fill a passive prototype again after its stats "
               "entry was changed from Lua",
        .Gone = {
            .Fate = "inlined into the job that loads the stats (the function object of ecl::ModuleLoadSystem::DoLoadStats), between "
                    "its calls of eoc::PassivePrototype::PassivePrototype() and eoc::PassivePrototype::Clean(); the code uses that "
                    "function's frame and cannot be called",
            .NoSymbol = "__ZN3eoc16PassivePrototype4Init",
            .Hosts = kLoadStatsJobHosts,
            .HostShapes = kLoadStatsJobShapes,
            .Replacements = kPassiveInitReplacements,
        },
        .Operands = kPassiveInitOperands,
    },
    {
        .Name = "eoc::PassivePrototype::Clean",
        .What = Kind::FunctionToCall,
        .Mac = "eoc::PassivePrototype::Clean()",
        .Use = kPrototypeUse,
        .Where = Exact(kPassiveClean),
        .Call = Takes({ Pointer(Place::X0, -1, "eoc::PassivePrototype") }),
        .Shape = { 170, 0x4a16de07ae29b525ull },     // 4.1.1.7631656
    },
    {
        .Name = "eoc::StatsDescription::SetFromStatsObject",
        .What = Kind::FunctionToCall,
        .Mac = "eoc::StatsDescription::SetFromStatsObject(StatsObject const&)",
        .Use = kPrototypeUse,
        .Where = Exact(kDescriptionFromObject),
        .Call = Takes({ Pointer(Place::X0, -1, "eoc::StatsDescription: a prototype's Description"), Pointer(Place::X1, -1, "StatsObject const") }),
        .Shape = { 510, 0xec1a80cd3948a4e2ull },     // 4.1.1.7631656
    },
    {
        .Name = "ls::FixedString::operator=(ls::FixedString const&)",
        .What = Kind::FunctionToCall,
        .Mac = "ls::FixedString::operator=(ls::FixedString const&)",
        .Use = kPrototypeUse,
        .Where = Exact("__ZN2ls11FixedStringaSERKS0_"),
        .Call = Takes({ Pointer(Place::X0, -1, "the string assigned to"), Value(Place::X1, 4, -1, "the string assigned: its index, by value") },
            Returns::X0, "the string"),
        .Shape = { 30, 0xd37431c55b28acb2ull },     // 4.1.1.7631656
    },
    {
        .Name = "ls::FixedString::GetHash",
        .What = Kind::FunctionToCall,
        .Mac = "ls::FixedString::GetHash() const",
        .Use = kPrototypeUse,
        .Where = Exact("__ZNK2ls11FixedString7GetHashEv"),
        .Call = Takes({ Value(Place::X0, 4, -1, "the string: its index, by value") }, Returns::W0, "the hash a hash map keeps the string under"),
        .Shape = { 47, 0xa90d898097124a7eull },     // 4.1.1.7631656
    },
    {
        .Name = "ls::FixedString::Empty",
        .What = Kind::GlobalObject,
        .Mac = "ls::FixedString::Empty",
        .Use = "what a fixed string attribute is when the object has none",
        .Where = Exact("__ZN2ls11FixedString5EmptyE"),
        .Bytes = 4,
    },
    {
        .Name = "ls::StaticFixedStringRegistry: the entry for Default",
        .What = Kind::GlobalObject,
        .Mac = "ls::StaticFixedStringRegistry::Request<17127472872438232369>(ls::_StringView<char> const&)::entry: the registry's entry "
               "for the text \"Default\", whose fixed string is the text key of functors that name none",
        .Use = "the text key the job that loads the stats takes a prototype's functors for",
        .Where = Exact("__ZZN2ls25StaticFixedStringRegistry7RequestILy17127472872438232369EEERKNS_11FixedStringERKNS_11_StringViewIcEEE5entry"),
    },
    ATTRIBUTE_NAME("(anonymous namespace)::strToggleOnEffect", "ToggleOnEffect", "__ZN12_GLOBAL__N_117strToggleOnEffectE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strToggleOffEffect", "ToggleOffEffect", "__ZN12_GLOBAL__N_118strToggleOffEffectE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strEnabledContext", "EnabledContext", "__ZN12_GLOBAL__N_117strEnabledContextE"),
    ATTRIBUTE_NAME("eoc::strStatsFunctorContext", "StatsFunctorContext", "__ZN3eoc22strStatsFunctorContextE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strEnabledConditions", "EnabledConditions", "__ZN12_GLOBAL__N_120strEnabledConditionsE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strToggleOnFunctors", "ToggleOnFunctors", "__ZN12_GLOBAL__N_119strToggleOnFunctorsE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strToggleOffFunctors", "ToggleOffFunctors", "__ZN12_GLOBAL__N_120strToggleOffFunctorsE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strToggleOffContext", "ToggleOffContext", "__ZN12_GLOBAL__N_119strToggleOffContextE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strToggleGroup", "ToggleGroup", "__ZN12_GLOBAL__N_114strToggleGroupE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strPriorityOrder", "PriorityOrder", "__ZN12_GLOBAL__N_116strPriorityOrderE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strTooltipConditionalDamage", "TooltipConditionalDamage", "__ZN12_GLOBAL__N_127strTooltipConditionalDamageE"),
    STATS_NAME("EoCFS::strProperties", "Properties", "__ZN5EoCFS13strPropertiesE"),
    STATS_NAME("EoCFS::strBoostConditions", "BoostConditions", "__ZN5EoCFS18strBoostConditionsE"),
    STATS_NAME("EoCFS::strBoostContext", "BoostContext", "__ZN5EoCFS15strBoostContextE"),
    STATS_NAME("EoCFS::strBoosts", "Boosts", "__ZN5EoCFS9strBoostsE"),
    {
        .Name = "eoc__InterruptPrototypeManager",
        .What = Kind::GlobalPointer,
        .Mac = "eoc::InterruptPrototypeManager::m_ptr",
        .Use = "GameDefinitions/Stats/Stats.cpp and Lua/Libs/Stats.inl",
        .Where = Exact("__ZN3eoc25InterruptPrototypeManager5m_ptrE"),
    },
    {
        .Name = "eoc__InterruptPrototype__Init",
        .What = Kind::NoCallableCode,
        .Use = "GameDefinitions/Stats/Stats.cpp (InterruptPrototypeManager::SyncStat): fill an interrupt prototype again after its "
               "stats entry was changed from Lua",
        .Gone = {
            .Fate = "inlined into the job that loads the stats (the function object of ecl::ModuleLoadSystem::DoLoadStats), around "
                    "its call of eoc::InterruptPrototype::~InterruptPrototype(); the code uses that function's frame and cannot be called",
            .NoSymbol = "__ZN3eoc18InterruptPrototype4Init",
            .Hosts = kLoadStatsJobHosts,
            .HostShapes = kLoadStatsJobShapes,
            .Replacements = kInterruptInitReplacements,
        },
        .Operands = kInterruptInitOperands,
    },
    {
        .Name = "eoc::action_resource::ParseActionResourceCostWithError",
        .What = Kind::FunctionToCall,
        .Mac = "eoc::action_resource::ParseActionResourceCostWithError(ls::_StringView<char> const&, "
               "ls::DynamicArray<eoc::action_resource::ActionResourceCost>&)",
        .Use = kPrototypeUse,
        .Where = Exact(kParseResourceCosts),
        .Call = Takes({
            Pointer(Place::X0, -1, "ls::_StringView<char> const: the text of a Cost attribute"),
            Pointer(Place::X1, -1, "the costs it is parsed into"),
        }),
        .Shape = { 1181, 0xc4e30de900db97e3ull },     // 4.1.1.7631656
    },
    ATTRIBUTE_NAME("(anonymous namespace)::strInterruptContext", "InterruptContext", "__ZN12_GLOBAL__N_119strInterruptContextE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strInterruptContextScope", "InterruptContextScope", "__ZN12_GLOBAL__N_124strInterruptContextScopeE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strInterruptFlags", "InterruptFlags", "__ZN12_GLOBAL__N_117strInterruptFlagsE"),
    {
        .Name = "(anonymous namespace)::strConditions of the interrupts",
        .What = Kind::GlobalPointer,
        .Mac = "(anonymous namespace)::strConditions, the one of the file of the interrupts' prototypes: a reference to the fixed "
               "string \"Conditions\"",
        .Use = "the name of the attribute as the job that loads the stats passes it",
        .Derived = &kInterruptConditionsName,
        .Operands = kInterruptConditionsOperands,
    },
    ATTRIBUTE_NAME("(anonymous namespace)::strContainer", "Container", "__ZN12_GLOBAL__N_112strContainerE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strRoll", "Roll", "__ZN12_GLOBAL__N_17strRollE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strProperties", "Properties", "__ZN12_GLOBAL__N_113strPropertiesE", .Caveat = kSuffixedSibling),
    ATTRIBUTE_NAME("(anonymous namespace)::strSuccess", "Success", "__ZN12_GLOBAL__N_110strSuccessE", .Caveat = kSuffixedSibling),
    ATTRIBUTE_NAME("(anonymous namespace)::strFailure", "Failure", "__ZN12_GLOBAL__N_110strFailureE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strCost", "Cost", "__ZN12_GLOBAL__N_17strCostE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strStack", "Stack", "__ZN12_GLOBAL__N_18strStackE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strInterruptDefaultValue", "InterruptDefaultValue", "__ZN12_GLOBAL__N_124strInterruptDefaultValueE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strEnableCondition", "EnableCondition", "__ZN12_GLOBAL__N_118strEnableConditionE"),
    ATTRIBUTE_NAME("(anonymous namespace)::strEnableContext", "EnableContext", "__ZN12_GLOBAL__N_116strEnableContextE"),
    {
        .Name = "eoc__DynamicStatsExpressionManager",
        .What = Kind::GlobalPointer,
        .Mac = "eoc::DynamicStatsExpressionManager::m_ptr",
        .Use = "Lua/Shared/LuaGet.inl",
        .Where = Exact("__ZN3eoc29DynamicStatsExpressionManager5m_ptrE"),
    },
    {
        .Name = "eoc__ParseStaticBoosts",
        .What = Kind::NoCallableCode,
        .Use = "GameDefinitions/Stats/Stats.cpp: parse a status's static boosts when its prototype is filled again",
        .Gone = {
            .Fate = "eoc::ParseStaticBoosts(ls::_StringView<char> const&, ls::DynamicArray<ls::Guid>&, ls::Function<bool (ls::FixedString "
                    "const&)>) has no code of its own: it is inlined into each function that uses it (eoc::FeatManager::PostInit, "
                    "eoc::ProgressionManager::PostInit, eoc::StatusPrototypeManager::Init and the job that loads the stats); only "
                    "the function objects of its lambdas have symbols",
            .NoSymbol = "__ZN3eoc17ParseStaticBoostsE",
            .Hosts = kStaticBoostHosts,
            .HostShapes = kStaticBoostHostShapes,
            .Replacements = kStaticBoostReplacements,
        },
        .Operands = kStaticBoostOperands,
    },
    {
        .Name = kParseBoostsName,
        .What = Kind::FunctionToCall,
        .Mac = "(anonymous namespace)::ParseBoosts(ls::_StringView<char> const&, ls::Function<void (ls::_StringView<char> const&, "
               "ls::DynamicArray<ls::_StringView<char>> const&, ls::FixedString const&, ls::_StringView<char> const&)>)",
        .Use = "what eoc::ParseStaticBoosts is inlined around: splits the text of a Boosts attribute and calls the function for each boost",
        .Where = Exact(kParseBoosts),
        .Call = Takes({
            Pointer(Place::X0, -1, "char const: the text; the view arrives as its two members"),
            Value(Place::X1, 4, -1, "the text's length"),
            Pointer(Place::X2, -1, "ls::Function: called for each boost; the caller's object, which the caller ends"),
        }, Returns::W0, "bool"),
        .Shape = { 281, 0x99881c31291fa6dfull },     // 4.1.1.7631656
    },
    {
        .Name = kBoostCallName,
        .What = Kind::FunctionToCall,
        .Mac = "ls::Function<...>::FunctorMethodsVSSO<eoc::ParseStaticBoosts(...)::$_1>::Call",
        .Use = "the first method of the function object handed to ParseBoosts: makes the boost's prototype and appends its GUID to "
               "the array the object captured, unless the filter it captured refuses the boost's name",
        .Where = Exact(kBoostCall),
        .Call = Takes({
            Pointer(Place::X0, -1, "the object's methods; the captures follow them: the filter, then the array of GUIDs"),
            Pointer(Place::X1, -1, "ls::_StringView<char> const: the boost's text"),
            Pointer(Place::X2, -1, "ls::DynamicArray<ls::_StringView<char>> const: its parameters"),
            Pointer(Place::X3, -1, "ls::FixedString const: its name"),
            Pointer(Place::X4, -1, "ls::_StringView<char> const"),
        }),
        .Shape = { 289, 0x643c66bf8dd1f1dcull },     // 4.1.1.7631656
    },
    {
        .Name = kBoostCopyName,
        .What = Kind::FunctionToCall,
        .Mac = "ls::Function<...>::FunctorMethodsVSSO<eoc::ParseStaticBoosts(...)::$_1>::Copy",
        .Use = "the second method of that object",
        .Where = Exact(kBoostCopy),
        .Call = Takes({
            Unread(Place::X0, -1, "the object's methods"),
            Pointer(Place::X1, -1, "the methods and captures copied from"),
            Pointer(Place::X2, -1, "where to"),
        }, Returns::X0, "where to"),
        .Shape = { 8, 0x8d42032934a98db4ull },     // 4.1.1.7631656
    },
    {
        .Name = kBoostMoveDestroyName,
        .What = Kind::FunctionToCall,
        .Mac = "ls::Function<...>::FunctorMethodsVSSO<eoc::ParseStaticBoosts(...)::$_1>::MoveDestroy",
        .Use = "the third method of that object, which its owner ends it with: moves to the destination when there is one, and "
               "has nothing to destroy",
        .Where = Exact(kBoostMoveDestroy),
        .Call = Takes({
            Unread(Place::X0, -1, "the object's methods"),
            Pointer(Place::X1, -1, "the methods and captures moved from"),
            Pointer(Place::X2, -1, "where to, or null"),
        }, Returns::X0, "where to"),
        .Shape = { 9, 0xc6e0b68fb72af196ull },     // 4.1.1.7631656
    },
    {
        .Name = "esv__OsirisVariableHelper__SavegameVisit",
        .What = Kind::FunctionToHook,
        .Mac = "esv::OsirisVariableHelper::SavegameVisit(eoc::SavegameVisitor*)",
        .Use = "GameHooks/EngineHooks.inl (OnSavegameVisit): a pre-hook when the story variables are saved or loaded",
        .Where = Exact("__ZN3esv20OsirisVariableHelper13SavegameVisitEPN3eoc15SavegameVisitorE"),
        .Call = Takes({ Pointer(Place::X0, 0, "esv::OsirisVariableHelper"), Pointer(Place::X1, 1, "eoc::SavegameVisitor") },
            Returns::Nothing, nullptr, "Norbyte declares a bool result; the function sets none"),
        .Shape = { 74, 0x010a7dd0927e6afbull },     // 4.1.1.7631656
    },

    {
        .Name = "stats__DealDamageFunctor__ApplyDamage",
        .What = Kind::FunctionToHook,
        .Mac = "esv::functor::StatsFunctorDealDamage::Execute(eoc::StatsFunctorDealDamage const&, ecs::EntityRef const&, ecs::EntityRef const&, "
               "Vector3f const&, bool, eoc::spell::SpellInfo const&, story action id, eoc::ActionOriginator const&, eoc::ClassDescriptions const&, "
               "eoc::HitDesc const&, eoc::AttackDesc const&, ls::ID<ecs::EntityHandleTraits> const&, eoc::EHitWith, int, bool, "
               "ls::ID<ecs::EntityHandleTraits> const&, eoc::spell::SpellId const&)",
        .Use = "GameHooks/EngineHooks.inl and Lua/Server/FunctorEvents.inl: a wrapper around a deal-damage functor",
        .Where = Prefix(kDealDamage),
        .Call = Takes({
            Pointer(Place::X0, 0, "the hit result to fill, in the caller's memory"),
            Pointer(Place::X1, 1, "eoc::StatsFunctorDealDamage const"),
            Pointer(Place::X2, 2, "ecs::EntityRef const: the caster"),
            Pointer(Place::X3, 3, "ecs::EntityRef const: the target"),
            Pointer(Place::X4, 4, "Vector3f const: the position"),
            Value(Place::X5, 1, 5, "bool: from an item"),
            Pointer(Place::X6, 6, "eoc::spell::SpellInfo const"),
            Value(Place::X7, 4, 7, "ls::TypeWrap<unsigned int, eoc::TStoryActionIDClassname, true>: the story action id"),
            StackPointer(0x00, 8, "eoc::ActionOriginator const"),
            StackPointer(0x08, 9, "eoc::ClassDescriptions const"),
            StackPointer(0x10, 10, "eoc::HitDesc const"),
            StackPointer(0x18, 11, "eoc::AttackDesc const"),
            StackPointer(0x20, 12, "ls::ID<ecs::EntityHandleTraits> const"),
            StackValue(0x28, 1, 13, "eoc::EHitWith"),
            StackValue(0x2c, 4, 14, "int: the condition roll index"),
            StackValue(0x30, 1, 15, "bool"),
            StackPointer(0x38, 16, "ls::ID<ecs::EntityHandleTraits> const"),
            StackPointer(0x40, 17, "eoc::spell::SpellId const"),
        }, Returns::IndirectX0, "the hit result",
            "Norbyte's seventeenth parameter is an __int64; here it is a pointer to an entity handle. The ten stack arguments are "
            "packed as Apple's ABI packs them (one byte at 0x28, four at 0x2c, one at 0x30), not one to an 8-byte slot"),
        .Shape = { 2459, 0xdcadab5688fc5b30ull },     // 4.1.1.7631656
        .Caller = Prefix("__ZNK3esv21FallingUnitTestHelper10DealDamageE"),
    },
    {
        .Name = "esv__StatsSystem__ThrowDamageEvent",
        .What = Kind::FunctionToHook,
        .Mac = "esv::StatsSystem::ApplyDamage(ecs::EntityRefView<...> const&, eoc::HitDesc&, eoc::AttackDesc&, EAbility, bool): the "
               "function the damage functor calls with these six arguments, and the one that raises the entity-damaged event",
        .Use = "GameHooks/EngineHooks.inl and Lua/Server/FunctorEvents.inl: a wrapper around the damage being applied",
        .Where = Prefix(kApplyDamage),
        .Call = Takes({
            Pointer(Place::X0, 0, "esv::StatsSystem"),
            Pointer(Place::X1, 1, "ecs::EntityRefView<...> const: the target's components"),
            Pointer(Place::X2, 2, "eoc::HitDesc"),
            Pointer(Place::X3, 3, "eoc::AttackDesc"),
            Value(Place::X4, 1, 4, "EAbility"),
            Value(Place::X5, 1, 5, "bool"),
        }, Returns::Nothing, nullptr, "the fifth parameter is an ability, one byte, as Norbyte's declaration on macOS has it"),
        .Shape = { 456, 0x5ef58d7d7523f3a3ull },     // 4.1.1.7631656
        .Operands = kApplyDamageCalls,
    },

    EXECUTOR_ENTRY("esv__ExecuteStatsFunctor_AttackTargetContext", "23AttackTargetContextData", "AttackTargetContextData", 213, 0x806f79e4dfada7a5ull),     // 4.1.1.7631656
    EXECUTOR_ENTRY("esv__ExecuteStatsFunctor_AttackPositionContext", "25AttackPositionContextData", "AttackPositionContextData", 1724, 0x6a67b9cd14775d8bull),     // 4.1.1.7631656
    EXECUTOR_ENTRY("esv__ExecuteStatsFunctor_MoveContext", "15MoveContextData", "MoveContextData", 1135, 0x6e7ce2901185d2c1ull),     // 4.1.1.7631656
    EXECUTOR_ENTRY("esv__ExecuteStatsFunctor_TargetContext", "17TargetContextData", "TargetContextData", 3824, 0xafa249fbe6d9eebeull),     // 4.1.1.7631656
    EXECUTOR_ENTRY("esv__ExecuteStatsFunctor_NearbyAttackedContext", "25NearbyAttackedContextData", "NearbyAttackedContextData", 1460, 0x4c05cc237d483ca6ull),     // 4.1.1.7631656
    EXECUTOR_ENTRY("esv__ExecuteStatsFunctor_NearbyAttackingContext", "26NearbyAttackingContextData", "NearbyAttackingContextData", 928, 0xdeb85b26190e8075ull),     // 4.1.1.7631656
    EXECUTOR_ENTRY("esv__ExecuteStatsFunctor_EquipContext", "16EquipContextData", "EquipContextData", 2074, 0x105c6a5a4c0a9906ull),     // 4.1.1.7631656
    EXECUTOR_ENTRY("esv__ExecuteStatsFunctor_SourceContext", "17SourceContextData", "SourceContextData", 3797, 0x25bf2ece2cb376d5ull),     // 4.1.1.7631656
    {
        .Name = "esv__ExecuteStatsFunctor_InterruptContext",
        .What = Kind::FunctionToHook,
        .Mac = "esv::functor::ExecuteStatsFunctors(ecs::EntityWorld&, eoc::StatsFunctorList const*, esv::functor::InterruptContextData&)",
        .Use = "GameHooks/EngineHooks.inl, Lua/Server/FunctorEvents.inl and Lua/Libs/StatFunctors.inl: the functors of a list run for an interrupt",
        .Where = Exact("__ZN3esv7functor20ExecuteStatsFunctorsERN3ecs11EntityWorldEPKN3eoc16StatsFunctorListERNS0_20InterruptContextDataE"),
        .Call = Takes({
            Pointer(Place::X0, 0, "the hit result to fill, in the caller's memory"),
            Pointer(Place::X1, 1, "ecs::EntityWorld"),
            Pointer(Place::X2, 2, "eoc::StatsFunctorList const"),
            Pointer(Place::X3, 3, "esv::functor::InterruptContextData"),
        }, Returns::IndirectX0, "the hit result"),
        .Shape = { 5887, 0xbb663cf9bb017e92ull },     // 4.1.1.7631656
    },
};

#undef EXECUTOR_ENTRY
#undef EXECUTOR

}

std::span<Entry const> StatsEntries()
{
    return kEntries;
}

}
