// The story's queries called from Lua (StartupStoryQueries.h).
//
// Osi.<name>(...) for a user query (QRY_) reaches OsiFunction::OsiUserQuery
// (Lua/Osiris/Function.inl), which builds the tuple of the IN arguments and
// asks the node of the query's function whether it holds (IsValid). The
// story library of the macOS game gives that function its node, a query node
// (CReteOsiQuery), in one place: CreateStartNode, when it builds a rule's
// condition on the query (TStartNode 6). The query's own definition is the
// node of the function named <name>__DEF__ (TStartNode 5, a CReteEvent), and
// the query node finds it by that name when it runs (_ExecuteQuery). A query
// that no rule calls is in the function table with no node.
//
// Here the story is the test's: a function table with two such queries in
// it, a node table, and an adaptor for each number of columns that passes a
// tuple on as it is, put where the binding puts the library's
// (OsirisStaticGlobals). The query node is an object of Norbyte's Node with a
// vtable of the test's own whose IsValid holds for the value 1. What the
// server's Lua state does at a story's load is done for it as
// OsirisBinding::StoryLoaded does it, without the listeners (the node
// classes' vtables of a real story are not here).
#include <stdafx.h>

#include <Extender/ScriptExtender.h>
#include <Lua/Osiris/Binding.h>

#include "StartupStoryQueries.h"

#include <cstdlib>
#include <cstring>
#include <map>
#include <strings.h>

BEGIN_NS(mac::test)

namespace {

constexpr char kCalled[] = "QRY_Bg3mseCalled";
constexpr char kUncalled[] = "QRY_Bg3mseUncalled";
constexpr uint32_t kQueryNode = 1;

std::vector<std::string> gProblems;

template <class T>
T* Blank()
{
    return static_cast<T*>(calloc(1, sizeof(T)));
}

// The identity adaptor for one column, which OsiFunction::Bind gives the query.
uint32_t gOneColumnAdapter = 0;

uint64_t UnexpectedSlot()
{
    gProblems.push_back("a slot of the query node's vtable was called that OsiUserQuery does not call");
    return 0;
}

// The query holds for 1, given as a tuple of one integer through the
// identity adaptor for one column.
bool QueryIsValid(Node*, SmallTuple* values, uint32_t adapter)
{
    if (adapter != gOneColumnAdapter) gProblems.push_back("the query node was asked through an adaptor other than the identity adaptor for one column");
    if (values->Size() != 1 || (*values)[0].TypeId != ValueType::Integer) {
        gProblems.push_back("the query node was given a tuple that is not one integer");
        return false;
    }
    return (*values)[0].Value.Int32 == 1;
}

void* gVtable[sizeof(NodeVMT) / sizeof(void*)];

struct NoCaseLess
{
    bool operator()(std::string const& a, std::string const& b) const { return strcasecmp(a.c_str(), b.c_str()) < 0; }
};
// A bucket of the function table: the library's map of "name/arity" to the
// function (TypeDb::HashSlot, a libc++ map with the game's strings, whose
// bytes are the standard string's).
using Bucket = std::map<std::string, OsiFunctionDef*, NoCaseLess>;
static_assert(sizeof(Bucket) == sizeof(FunctionDb::HashSlot) && sizeof(OsiString) == sizeof(std::string));

// The bucket LookupOsiFunction looks a name up in (FunctionNameHash plus the arity).
uint32_t BucketOf(char const* name, uint32_t arity)
{
    uint32_t hash = 0;
    for (; *name; name++) hash = (uint32_t)(*name | 0x20) + 129 * (hash % 4294967);
    return (hash + arity) % 0x3FF;
}

OsiFunctionDef* MakeQuery(FunctionDb* table, char const* name, uint32_t node)
{
    auto signature = Blank<FunctionSignature>();
    signature->Name = name;
    signature->Params = Blank<FunctionParamList>();
    signature->Params->Params.Init();
    auto& params = signature->Params->Params;
    params.Insert(FunctionParamDesc{ (uint16_t)ValueType::Integer, 0 }, params.Head->Head);
    static uint8_t noOutParams[1]{ 0 };
    signature->OutParamList.Params = noOutParams;
    signature->OutParamList.Count = 1;
    auto function = Blank<OsiFunctionDef>();
    function->Signature = signature;
    function->Node.Id = node;
    function->Type = FunctionType::UserQuery;

    auto& slot = table->Hash[BucketOf(name, 1)];
    auto bucket = reinterpret_cast<Bucket*>(&slot);
    // An empty bucket is zeroed; a map is made in it once.
    if (reinterpret_cast<void* const&>(slot) == nullptr) new (bucket) Bucket();
    (*bucket)[std::string(name) + "/1"] = function;
    table->NumItems++;
    return function;
}

}

std::vector<std::string> RunStoryQueryTest(esv::lua::OsirisBinding& binding, std::function<std::string(char const* chunk)> const& run)
{
    gProblems.clear();
    auto& globals = gExtender->GetServer().Osiris().GetWrappers().Globals;
    auto const saved = globals;

    static FunctionDb* functions = nullptr;
    static NodeDb* nodes = nullptr;
    static AdapterDb* adapters = nullptr;
    static OsiTypeDb* types = nullptr;
    if (!functions) {
        functions = Blank<FunctionDb>();
        nodes = Blank<NodeDb>();
        adapters = Blank<AdapterDb>();
        types = Blank<OsiTypeDb>();
        for (unsigned columns = 0; columns <= 16; columns++) {
            auto adapter = Blank<Adapter>();
            adapter->Id = columns + 1;
            adapter->VarToColumnMapCount = columns;
            for (unsigned i = 0; i < columns; i++) adapter->ColumnToVarMaps.push_back((int8_t)i);
            new (&adapter->Constants) SmallTuple();
            adapters->Db.Elements.push_back(adapter);
            adapters->Db.Size++;
        }
        gOneColumnAdapter = 2;

        for (auto& slot : gVtable) slot = reinterpret_cast<void*>(&UnexpectedSlot);
        reinterpret_cast<NodeVMT*>(gVtable)->IsValid = &QueryIsValid;
        auto node = Blank<UserQueryNode>();
        *reinterpret_cast<void**>(node) = gVtable;
        node->Id = kQueryNode;
        nodes->Db.Elements.push_back(node);
        nodes->Db.Size++;

        // A query that a rule calls has the query node; one that none calls has none.
        node->Function = MakeQuery(functions, kCalled, kQueryNode);
        MakeQuery(functions, kUncalled, 0);
    }

    globals.Functions = &functions;
    globals.Nodes = &nodes;
    globals.Adapters = &adapters;
    globals.Types = &types;
    binding.GetIdentityAdapterMap().UpdateAdapters();
    binding.GetNameResolver().InvalidateCache();

    auto error = run(R"lua(
        local function same(value, wanted, what)
            if value ~= wanted then error(what .. " gave " .. tostring(value), 2) end
        end
        same(Osi.QRY_Bg3mseCalled(1), true, "Osi.QRY_Bg3mseCalled(1)")
        same(Osi.QRY_Bg3mseCalled(2), false, "Osi.QRY_Bg3mseCalled(2)")
        same(Osi.QRY_Bg3mseUncalled:Exists(1), true, "Osi.QRY_Bg3mseUncalled:Exists(1)")
        local ok, err = pcall(Osi.QRY_Bg3mseUncalled, 1)
        same(ok, false, "Osi.QRY_Bg3mseUncalled(1)")
        same(tostring(err):find("Query 'QRY_Bg3mseUncalled' has no node in the story", 1, true) ~= nil, true,
            "the error of Osi.QRY_Bg3mseUncalled(1), " .. tostring(err) .. ",")
    )lua");
    if (!error.empty()) gProblems.push_back(error);

    // The story is gone again, as it was before.
    globals = saved;
    binding.GetNameResolver().InvalidateCache();
    return gProblems;
}

END_NS()
