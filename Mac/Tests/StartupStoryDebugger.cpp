// The story debugger test of the start-up harness (StartupStoryDebugger.h).
//
// Norbyte's story debugger is three things: a server on a loopback port
// (Osiris/Debugger/DebugInterface.cpp), started when the game registers its
// functions with the story library and EnableDebugger is set; the debugger
// itself (Debugger.cpp), made when a story is loaded and fed by his hooks on
// the library (the rule action call, the game's start and end) and on the
// node classes' vtables; and the messages between the two (DebugMessages.cpp,
// osidebug.proto). No frontend for it is in this repository.
//
// Here the library is the test's: five functions of this image
// (StartupStoryTargets.S) that his hooks go on as OsirisWrappers::Initialize
// puts them on the library's, node vtables in read-only memory that his
// wrappers replace slot by slot, and a story built from his structures
// (GameDefinitions/Osiris.h, which Mac/Layout/OsirisDefinitionChecks.cpp ties
// to the library's code): one goal, one database, one rule. The story runs
// through those functions and vtables only, so everything the debugger sees
// reaches it the way the game's story would; and a frontend on another
// thread identifies, synchronises the story, sets breakpoints, reads the
// database, evaluates, steps and continues.
//
// What this does not show is the library itself calling the hooks: that the
// hooked functions and vtable slots are the library's is the binding's
// matter (Mac/Binding/OsirisBinding.h), and that each argument reaches his
// handler is bg3mse_hook_tests'.
#include <stdafx.h>

#include <Extender/ScriptExtender.h>
#include <Osiris/Debugger/Debugger.h>
#include <Osiris/Shared/NodeHooks.h>
#include <detours.h>

#include "StartupStoryDebugger.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

// The loopback client (Mac/Tests/StartupTestsPosix.cpp).
int DebugClientConnect(uint16_t port, int timeoutMs);
bool DebugClientSend(int fd, void const* data, size_t size);
bool DebugClientReceive(int fd, void* data, size_t size);
void DebugClientClose(int fd);
uint16_t DebugClientFreePort();

extern "C" {
// StartupStoryTargets.S: the library's functions, each of which calls its
// handler below.
void* st_handlers[5];
int st_register_div_functions(void* osiris, bg3se::DivFunctions* functions);
int st_load(void* osiris, void* buffer);
int st_init_game(void* osiris);
void st_delete_all_data(void* osiris, bool deleteTypes);
void st_fire(bg3se::RuleActionNode* action, void*, void*, void*, void*);
}

BEGIN_NS(mac::test)

namespace {

using namespace bg3se::osidbg;

// --- what did not hold ---------------------------------------------------------

std::mutex gProblemsLock;
std::vector<std::string> gProblems;

void Problem(std::string text)
{
    std::lock_guard _(gProblemsLock);
    gProblems.push_back(std::move(text));
}

// --- the library's strings -------------------------------------------------------

// COsiStringTable: a string for a handle, counted. The debugger's thread
// reads strings while the story's thread holds values.
struct StringTable
{
    std::mutex Lock;
    std::deque<std::pair<std::string, int>> Strings;

    static StringTable& Get()
    {
        static StringTable table;
        return table;
    }
};

char const* GetStr(COsiStringTable**, OsiStringHandle handle)
{
    auto& table = StringTable::Get();
    std::lock_guard _(table.Lock);
    auto index = static_cast<uint64_t>(handle);
    if (index == 0 || index > table.Strings.size()) {
        Problem("a string was asked for by a handle the table never gave");
        return "";
    }
    return table.Strings[index - 1].first.c_str();
}

OsiStringHandle* AddStr(COsiStringTable**, OsiStringHandle& handle, const char* str, bool)
{
    auto& table = StringTable::Get();
    std::lock_guard _(table.Lock);
    for (size_t i = 0; i < table.Strings.size(); i++) {
        if (table.Strings[i].first == str) {
            table.Strings[i].second++;
            handle = static_cast<OsiStringHandle>(i + 1);
            return &handle;
        }
    }
    table.Strings.emplace_back(str, 1);
    handle = static_cast<OsiStringHandle>(table.Strings.size());
    return &handle;
}

void AddStrRef(COsiStringTable**, OsiStringHandle handle)
{
    auto& table = StringTable::Get();
    std::lock_guard _(table.Lock);
    auto index = static_cast<uint64_t>(handle);
    if (index != 0 && index <= table.Strings.size()) table.Strings[index - 1].second++;
}

void RemoveStr(COsiStringTable**, OsiStringHandle handle)
{
    auto& table = StringTable::Get();
    std::lock_guard _(table.Lock);
    auto index = static_cast<uint64_t>(handle);
    if (index != 0 && index <= table.Strings.size()) table.Strings[index - 1].second--;
}

// --- the story -------------------------------------------------------------------

constexpr char kGoalName[] = "Bg3mseDebuggerGoal";
constexpr char kDatabaseName[] = "DB_Bg3mseDebugger";
constexpr char kLastAction[] = "Bg3mseDebuggerDone";
constexpr char kDebugBreakText[] = "bg3mse: the rule fired";
constexpr uint32_t kFactsNode = 1;      // the database's node
constexpr uint32_t kRuleNode = 2;       // IF DB_Bg3mseDebugger(_Number, _Text) THEN DebugBreak("..."); Bg3mseDebuggerDone();
constexpr uint32_t kGoal = 1;           // INIT: DB_Bg3mseDebugger(1, "first");
constexpr uint32_t kDatabase = 1;
constexpr unsigned kColumns = 2;

// An object of the library's, zeroed: its structures are not ones this
// build constructs (ProtectedGameObject, the node classes' pure virtuals).
template <class T>
T* Blank()
{
    return static_cast<T*>(calloc(1, sizeof(T)));
}

struct Story
{
    // What OsirisStaticGlobals points at: the library's globals, null until
    // it has made its story.
    COsiStringTable** Strings{ nullptr };
    VariableDb* Variables{ nullptr };
    OsiTypeDb* Types{ nullptr };
    EnumDb* Enums{ nullptr };
    FunctionDb* Functions{ nullptr };
    ObjectDb* Objects{ nullptr };
    GoalDb* Goals{ nullptr };
    AdapterDb* Adapters{ nullptr };
    DatabaseDb* Databases{ nullptr };
    NodeDb* Nodes{ nullptr };
    DebugFlag Flags{};

    // The node classes' vtables by NodeType, in read-only memory.
    NodeVMT* Vtables[(unsigned)NodeType::Max + 1]{};

    Node* Facts{ nullptr };
    RuleNode* Rule{ nullptr };
    Goal* TheGoal{ nullptr };
    Database* Table{ nullptr };

    // The library's object, as the game passes it: only its address is used.
    uint64_t Osiris{ 0 };
    // What the library's functions were asked, in order.
    std::vector<std::string> Fired;
    unsigned Registered{ 0 }, Loaded{ 0 }, Started{ 0 }, Deleted{ 0 };
};

Story gStory;

NodeVMT* VtableOf(Node* node)
{
    return *reinterpret_cast<NodeVMT**>(node);
}

bool SameValue(TypedValue const& a, TypedValue const& b)
{
    if (a.TypeId != b.TypeId) return false;
    switch (a.TypeId) {
    case ValueType::Integer: return a.Value.Int32 == b.Value.Int32;
    case ValueType::String: return strcmp(a.GetString(), b.GetString()) == 0;
    default: return false;
    }
}

// A slot the story never calls.
uint64_t UnexpectedSlot()
{
    Problem("a slot of a node's vtable was called that the story does not call");
    return 0;
}

// CReteDBase's node: is the tuple a fact?
bool FactsIsValid(Node* self, SmallTuple* values, uint32_t)
{
    auto db = self->Database.Get();
    if (!db || values->Size() != db->NumParams) return false;
    for (auto fact = db->Facts.Head->Next; fact != db->Facts.Head; fact = fact->Next) {
        bool same = true;
        for (unsigned i = 0; i < kColumns; i++) same = same && SameValue(fact->Item.Values[i], (*values)[i]);
        if (same) return true;
    }
    return false;
}

void PushDown(TupleVec const& fact, bool deleted)
{
    SmallTuple pushed;
    for (unsigned i = 0; i < fact.Size; i++) {
        auto& value = pushed.Add();
        value = fact.Values[i];
        value.Index = (int8_t)i;
    }
    // The rule that follows the database, through its vtable.
    Node* rule = gStory.Rule;
    if (deleted) {
        VtableOf(rule)->PushDownTupleDelete(rule, &pushed, kColumns + 1, EntryPoint::None);
    } else {
        VtableOf(rule)->PushDownTuple(rule, &pushed, kColumns + 1, EntryPoint::None);
    }
}

// The fact is added to the database and pushed down to the rule.
void FactsInsert(Node* self, TuplePtrLL* tuple)
{
    auto db = self->Database.Get();
    if (!db || tuple->Items.Size != db->NumParams) {
        Problem("a tuple was inserted that is not the database's");
        return;
    }
    auto fact = new ListNode<TupleVec>();
    fact->Item.Values = GameAllocArray<TypedValue>(kColumns);
    fact->Item.Size = kColumns;
    unsigned column = 0;
    for (auto item = tuple->Items.Head->Next; item != tuple->Items.Head; item = item->Next) {
        fact->Item.Values[column++] = *item->Item;
    }
    db->Facts.Insert(fact, db->Facts.Head->Head);
    PushDown(fact->Item, false);
}

// The fact is taken from the database, and the rule told.
void FactsDelete(Node* self, TuplePtrLL* tuple)
{
    auto db = self->Database.Get();
    if (!db || tuple->Items.Size != db->NumParams) {
        Problem("a tuple was deleted that is not the database's");
        return;
    }
    for (auto fact = db->Facts.Head->Next; fact != db->Facts.Head; fact = fact->Next) {
        bool same = true;
        unsigned column = 0;
        for (auto item = tuple->Items.Head->Next; item != tuple->Items.Head; item = item->Next) {
            same = same && SameValue(fact->Item.Values[column++], *item->Item);
        }
        if (!same) continue;
        fact->Head->Next = fact->Next;
        fact->Next->Head = fact->Head;
        db->Facts.Size--;
        PushDown(fact->Item, true);
        return;
    }
}

bool RuleIsValid(Node*, SmallTuple*, uint32_t)
{
    return true;
}

// The rule's actions are carried out, each through the library's function.
void RulePushDown(Node* self, SmallTuple*, uint32_t, EntryPoint)
{
    auto rule = static_cast<RuleNode*>(self);
    auto& actions = rule->Calls->Actions;
    for (auto action = actions.Head->Next; action != actions.Head; action = action->Next) {
        st_fire(action->Item, nullptr, nullptr, nullptr, nullptr);
    }
}

void RulePushDownDelete(Node*, SmallTuple*, uint32_t, EntryPoint)
{
}

// The nine classes' vtables, one after the other as the library's lie in
// its read-only data: those of the classes that are not queries end with
// SetLineNumber.
bool MakeVtables()
{
    size_t const pageSize = (size_t)getpagesize();
    auto memory = static_cast<void**>(mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (memory == MAP_FAILED) return false;
    size_t const allSlots = sizeof(NodeVMT) / sizeof(void*);
    size_t const plainSlots = offsetof(NodeVMT, CallQuery) / sizeof(void*);
    void** next = memory;
    for (unsigned type = (unsigned)NodeType::Database; type <= (unsigned)NodeType::Max; type++) {
        bool query = type == (unsigned)NodeType::DivQuery || type == (unsigned)NodeType::InternalQuery || type == (unsigned)NodeType::UserQuery;
        size_t slots = query ? allSlots : plainSlots;
        for (size_t i = 0; i < slots; i++) next[i] = reinterpret_cast<void*>(&UnexpectedSlot);
        gStory.Vtables[type] = reinterpret_cast<NodeVMT*>(next);
        next += slots;
    }
    auto facts = gStory.Vtables[(unsigned)NodeType::Database];
    facts->IsValid = &FactsIsValid;
    facts->InsertTuple = &FactsInsert;
    facts->DeleteTuple = &FactsDelete;
    auto rule = gStory.Vtables[(unsigned)NodeType::Rule];
    rule->IsValid = &RuleIsValid;
    rule->PushDownTuple = &RulePushDown;
    rule->PushDownTupleDelete = &RulePushDownDelete;
    return mprotect(memory, pageSize, PROT_READ) == 0;
}

template <class T>
T* MakeNode(NodeType type, uint32_t id)
{
    auto node = Blank<T>();
    *reinterpret_cast<NodeVMT**>(node) = gStory.Vtables[(unsigned)type];
    node->Id = id;
    gStory.Nodes->Db.Elements.push_back(node);
    gStory.Nodes->Db.Size++;
    return node;
}

RuleActionList* MakeActions()
{
    auto list = Blank<RuleActionList>();
    list->Actions.Init();
    return list;
}

template <class... Args>
void AddAction(RuleActionList* list, char const* function, bool withArguments, Args... args)
{
    auto action = Blank<RuleActionNode>();
    action->FunctionName = function;
    if (withArguments) {
        action->Arguments = Blank<RuleActionArguments>();
        action->Arguments->Args.Init();
        auto add = [&](auto value) {
            auto typed = new TypedValue();
            if constexpr (std::is_integral_v<decltype(value)>) {
                typed->SetValue(ValueType::Integer, (int32_t)value);
            } else {
                typed->SetValue(ValueType::String, value);
            }
            auto& arguments = action->Arguments->Args;
            arguments.Insert(typed, arguments.Head->Head);
        };
        (add(args), ...);
    }
    list->Actions.Insert(action, list->Actions.Head->Head);
}

// What COsiris::Load leaves: the story.
void MakeStory()
{
    static COsiStringTable* noTable = nullptr;
    gStory.Strings = &noTable;
    gStory.Variables = Blank<VariableDb>();
    gStory.Types = Blank<OsiTypeDb>();
    gStory.Enums = Blank<EnumDb>();
    gStory.Functions = Blank<FunctionDb>();
    gStory.Objects = Blank<ObjectDb>();
    gStory.Goals = Blank<GoalDb>();
    gStory.Adapters = Blank<AdapterDb>();
    gStory.Databases = Blank<DatabaseDb>();
    gStory.Nodes = Blank<NodeDb>();

    // An adaptor for each number of columns that passes a tuple on as it is,
    // which the debugger evaluates with.
    for (unsigned columns = 0; columns <= 16; columns++) {
        auto adapter = Blank<Adapter>();
        adapter->Id = columns + 1;
        adapter->VarToColumnMapCount = columns;
        for (unsigned i = 0; i < columns; i++) adapter->ColumnToVarMaps.push_back((int8_t)i);
        new (&adapter->Constants) SmallTuple();
        gStory.Adapters->Db.Elements.push_back(adapter);
        gStory.Adapters->Db.Size++;
    }

    gStory.Table = Blank<Database>();
    gStory.Table->DatabaseId = kDatabase;
    gStory.Table->Facts.Init();
    gStory.Table->ParamTypes.push_back((uint16_t)ValueType::Integer);
    gStory.Table->ParamTypes.push_back((uint16_t)ValueType::String);
    gStory.Table->NumParams = kColumns;
    gStory.Databases->Db.Elements.push_back(gStory.Table);
    gStory.Databases->Db.Size++;

    auto signature = Blank<FunctionSignature>();
    signature->Name = kDatabaseName;
    signature->Params = Blank<FunctionParamList>();
    signature->Params->Params.Init();
    for (auto type : { ValueType::Integer, ValueType::String }) {
        auto& params = signature->Params->Params;
        params.Insert(FunctionParamDesc{ (uint16_t)type, 0 }, params.Head->Head);
    }
    static uint8_t noOutParams[1]{ 0 };
    signature->OutParamList.Params = noOutParams;
    signature->OutParamList.Count = 1;
    auto function = Blank<OsiFunctionDef>();
    function->Signature = signature;
    function->Node.Id = kFactsNode;
    function->Type = FunctionType::Database;

    gStory.Facts = MakeNode<DatabaseNode>(NodeType::Database, kFactsNode);
    gStory.Facts->Function = function;
    gStory.Facts->Database.Id = kDatabase;

    gStory.Rule = MakeNode<RuleNode>(NodeType::Rule, kRuleNode);
    gStory.Rule->Parent.Id = kFactsNode;
    gStory.Rule->Adapter.Id = kColumns + 1;
    gStory.Rule->Calls = MakeActions();
    AddAction(gStory.Rule->Calls, "DebugBreak", true, kDebugBreakText);
    AddAction(gStory.Rule->Calls, kLastAction, false);

    gStory.TheGoal = Blank<Goal>();
    gStory.TheGoal->Id = kGoal;
    gStory.TheGoal->Name = kGoalName;
    gStory.TheGoal->InitCalls = MakeActions();
    AddAction(gStory.TheGoal->InitCalls, kDatabaseName, true, 1, "first");
    gStory.TheGoal->ExitCalls = MakeActions();
    // A goal is found at its id; there is none at 0.
    gStory.Goals->Goals.push_back(nullptr);
    gStory.Goals->Goals.push_back(gStory.TheGoal);
    gStory.Goals->NumItems = 1;
}

// --- the library's functions ---------------------------------------------------------

// What the game registers: its side of the story's calls, queries and messages.
bool GameCallOrQuery(uint32_t, OsiArgumentDesc*)
{
    return false;
}

void GameErrorMessage(char const*)
{
}

void GameAssert(bool, char const*, bool)
{
}

int OnRegisterDivFunctions(void* osiris, DivFunctions*)
{
    if (osiris != &gStory.Osiris) Problem("COsiris::RegisterDIVFunctions was not passed the library's object");
    gStory.Registered++;
    return 1;
}

int OnLoad(void* osiris, void*)
{
    if (osiris != &gStory.Osiris) Problem("COsiris::Load was not passed the library's object");
    MakeStory();
    gStory.Loaded++;
    return 1;
}

int OnInitGame(void* osiris)
{
    if (osiris != &gStory.Osiris) Problem("COsiris::InitGame was not passed the library's object");
    gStory.Started++;
    return 1;
}

void OnDeleteAllData(void* osiris, bool)
{
    if (osiris != &gStory.Osiris) Problem("COsiris::DeleteAllData was not passed the library's object");
    gStory.Deleted++;
}

// An action is carried out: an insert into the database goes to the
// database's node, through its vtable; the others only say they ran.
void OnFire(RuleActionNode* action, void*, void*, void*, void*)
{
    gStory.Fired.push_back(action->FunctionName ? action->FunctionName : "(a goal)");
    if (!action->FunctionName || strcmp(action->FunctionName, kDatabaseName) != 0) return;
    TuplePtrLL tuple;
    tuple.Items.Init();
    auto& arguments = action->Arguments->Args;
    for (auto argument = arguments.Head->Next; argument != arguments.Head; argument = argument->Next) {
        tuple.Items.Insert(argument->Item, tuple.Items.Head->Head);
    }
    VtableOf(gStory.Facts)->InsertTuple(gStory.Facts, &tuple);
    for (auto item = tuple.Items.Head->Next; item != tuple.Items.Head;) {
        auto next = item->Next;
        delete item;
        item = next;
    }
}

// --- the frontend --------------------------------------------------------------------

// osidebug.proto, framed as Osiris/Debugger/DebugInterface.cpp frames it: a
// little-endian length that counts itself, then the message.
class Frontend
{
public:
    bool Connect(uint16_t port)
    {
        for (int attempt = 0; attempt < 50 && fd_ < 0; attempt++) {
            fd_ = DebugClientConnect(port, 10000);
            if (fd_ < 0) usleep(20000);
        }
        return fd_ >= 0;
    }

    void Close()
    {
        DebugClientClose(fd_);
        fd_ = -1;
    }

    // Sends with the next sequence number (1, 2, ...: DebugMessageHandler
    // refuses any other) and returns it.
    uint32_t Send(DebuggerToBackend& msg)
    {
        msg.set_seq_no(++sent_);
        std::string body = msg.SerializeAsString();
        uint32_t length = (uint32_t)body.size() + 4;
        if (!DebugClientSend(fd_, &length, 4) || !DebugClientSend(fd_, body.data(), body.size())) {
            Problem("story debugger: a message could not be sent");
        }
        return sent_;
    }

    // The next message, which must be numbered one after the last.
    bool Receive(BackendToDebugger& msg, char const* what)
    {
        uint32_t length = 0;
        if (!DebugClientReceive(fd_, &length, 4)) {
            Problem(std::string("story debugger: no message came where ") + what + " was expected");
            return false;
        }
        if (length < 4 || length > 0x100000) {
            Problem("story debugger: a message of an impossible length");
            return false;
        }
        std::string body(length - 4, '\0');
        if (!DebugClientReceive(fd_, body.data(), body.size()) || !msg.ParseFromString(body)) {
            Problem(std::string("story debugger: a message could not be read where ") + what + " was expected");
            return false;
        }
        if (msg.seq_no() != ++received_) {
            Problem("story debugger: the server's message " + std::to_string(msg.seq_no()) + " came as its message " + std::to_string(received_));
            return false;
        }
        return true;
    }

    bool Expect(BackendToDebugger& msg, BackendToDebugger::MsgCase kind, char const* what, uint32_t request = 0)
    {
        if (!Receive(msg, what)) return false;
        if (msg.msg_case() != kind) {
            Problem(std::string("story debugger: message of kind ") + std::to_string((int)msg.msg_case()) + " where " + what + " was expected");
            return false;
        }
        if (request != 0 && msg.reply_seq_no() != request) {
            Problem(std::string("story debugger: ") + what + " answers request " + std::to_string(msg.reply_seq_no()) + ", not " + std::to_string(request));
            return false;
        }
        return true;
    }

    // The result of a request.
    bool ExpectResult(uint32_t request, StatusCode code, char const* what)
    {
        BackendToDebugger msg;
        if (!Expect(msg, BackendToDebugger::kResults, what, request)) return false;
        if (msg.results().status_code() != code) {
            Problem(std::string("story debugger: ") + what + " is " + std::to_string((int)msg.results().status_code()) + ", not " + std::to_string((int)code));
            return false;
        }
        return true;
    }

private:
    int fd_{ -1 };
    uint32_t sent_{ 0 };
    uint32_t received_{ 0 };
};

// The steps the frontend and the story's thread hand each other.
struct Session
{
    std::mutex Lock;
    std::condition_variable Changed;
    int Stage{ 0 }; // 1: identified, 2: asked for a pause, 3: finished (or given up)

    void Set(int stage)
    {
        std::lock_guard _(Lock);
        if (stage > Stage) Stage = stage;
        Changed.notify_all();
    }

    bool WaitFor(int stage)
    {
        std::unique_lock lk(Lock);
        return Changed.wait_for(lk, std::chrono::seconds(30), [&] { return Stage >= stage; });
    }
};

bool IsRow(MsgTuple const& row, int number, char const* text)
{
    return row.column_size() == 2
        && row.column(0).type_id() == (uint32_t)ValueType::Integer && row.column(0).intval() == number
        && row.column(1).type_id() == (uint32_t)ValueType::String && row.column(1).stringval() == text;
}

void SetRow(MsgTuple* row, int number, char const* text)
{
    auto first = row->add_column();
    first->set_type_id((uint32_t)ValueType::Integer);
    first->set_intval(number);
    auto second = row->add_column();
    second->set_type_id((uint32_t)ValueType::String);
    second->set_stringval(text);
}

bool IsFrame(MsgFrame const& frame, MsgFrame::FrameType type, uint32_t node, uint32_t goal, uint32_t action)
{
    return frame.type() == type && frame.node_id() == node && frame.goal_id() == goal && frame.action_index() == action;
}

DebuggerToBackend Continue(DbgContinue::Action action, uint32_t mask = 0)
{
    DebuggerToBackend msg;
    msg.mutable_continue_()->set_action(action);
    msg.mutable_continue_()->set_breakpoint_mask(mask);
    return msg;
}

// The database's rows, as DbgGetDatabaseContents gives them.
bool ReadDatabase(Frontend& front, std::vector<MsgTuple>& rows)
{
    DebuggerToBackend ask;
    ask.mutable_getdatabasecontents()->set_database_id(kDatabase);
    uint32_t request = front.Send(ask);
    BackendToDebugger in;
    if (!front.Expect(in, BackendToDebugger::kBeginDatabaseContents, "the start of the database's contents")) return false;
    if (in.begindatabasecontents().database_id() != kDatabase) {
        Problem("story debugger: the contents are another database's");
        return false;
    }
    for (;;) {
        if (!front.Receive(in, "a row or the end of the database's contents")) return false;
        if (in.msg_case() == BackendToDebugger::kEndDatabaseContents) break;
        if (in.msg_case() != BackendToDebugger::kDatabaseRow || in.databaserow().database_id() != kDatabase) {
            Problem("story debugger: something else than a row of the database among its contents");
            return false;
        }
        for (auto const& row : in.databaserow().row()) rows.push_back(row);
    }
    return front.ExpectResult(request, SUCCESS, "the result of reading the database");
}

// What the frontend saw, for the summary.
struct Seen
{
    unsigned Goals{ 0 }, Databases{ 0 }, Nodes{ 0 }, Rules{ 0 };
    bool Finished{ false };
};

void RunFrontend(Session& session, Frontend& front, uint16_t port, Seen& seen)
{
    BackendToDebugger in;
    // Closing the connection clears every breakpoint and resumes a paused
    // story (DebugMessageHandler::HandleDisconnect): the story's thread is
    // never left waiting.
    auto GiveUp = [&] {
        front.Close();
        session.Set(3);
    };
#define NEED(condition, text) do { if (!(condition)) { Problem(std::string("story debugger: ") + (text)); return GiveUp(); } } while (0)

    NEED(front.Connect(port), "no connection to the server");

    // Before a story is loaded.
    DebuggerToBackend identify;
    identify.mutable_identify()->set_protocol_version(DebugMessageHandler::ProtocolVersion);
    front.Send(identify);
    if (!front.Expect(in, BackendToDebugger::kVersionInfo, "the version")) return GiveUp();
    NEED(in.versioninfo().protocol_version() == DebugMessageHandler::ProtocolVersion && !in.versioninfo().story_loaded()
        && !in.versioninfo().story_initialized(), "the version says a story is loaded before one is");
    DebuggerToBackend early;
    early.mutable_setglobalbreakpoints()->set_breakpoint_mask(DbgSetGlobalBreakpoints::GAME_INIT);
    if (!front.ExpectResult(front.Send(early), NO_DEBUGGEE, "the result of a breakpoint set without a story")) return GiveUp();
    session.Set(1);

    // The story is loaded with a frontend connected: the debugger stops there.
    if (!front.Expect(in, BackendToDebugger::kStoryLoaded, "the story's load")) return GiveUp();
    if (!front.Expect(in, BackendToDebugger::kGlobalBreakpointTriggered, "the stop at the story's load")) return GiveUp();
    NEED(in.globalbreakpointtriggered().reason() == BkGlobalBreakpointTriggered::STORY_LOADED, "the stop is not at the story's load");

    // The story as the debugger reads it from the library's structures.
    DebuggerToBackend sync;
    sync.mutable_syncstory();
    front.Send(sync);
    for (;;) {
        if (!front.Receive(in, "the story's data")) return GiveUp();
        if (in.msg_case() == BackendToDebugger::kSyncStoryFinished) break;
        NEED(in.msg_case() == BackendToDebugger::kSyncStoryData, "something else than the story's data during the synchronisation");
        auto const& data = in.syncstorydata();
        for (auto const& goal : data.goal()) {
            seen.Goals++;
            NEED(goal.id() == kGoal && goal.name() == kGoalName, "the goal is not the story's");
            NEED(goal.initactions_size() == 1 && goal.initactions(0).function() == kDatabaseName && goal.initactions(0).arity() == 2
                && goal.exitactions_size() == 0, "the goal's actions are not the story's");
        }
        for (auto const& database : data.database()) {
            seen.Databases++;
            NEED(database.id() == kDatabase && database.argumenttype_size() == 2
                && database.argumenttype(0) == (uint32_t)ValueType::Integer && database.argumenttype(1) == (uint32_t)ValueType::String,
                "the database's columns are not the story's");
        }
        for (auto const& node : data.node()) {
            seen.Nodes++;
            NEED((node.id() == kFactsNode && node.type() == (uint32_t)NodeType::Database && node.name() == kDatabaseName)
                || (node.id() == kRuleNode && node.type() == (uint32_t)NodeType::Rule && node.name().empty()), "a node is not the story's");
        }
        for (auto const& rule : data.rule()) {
            seen.Rules++;
            NEED(rule.node_id() == kRuleNode && rule.actions_size() == 2 && rule.actions(0).function() == "DebugBreak"
                && rule.actions(0).arity() == 1 && rule.actions(1).function() == kLastAction && rule.actions(1).arity() == 0,
                "the rule's actions are not the story's");
        }
    }
    NEED(seen.Goals == 1 && seen.Databases == 1 && seen.Nodes == 2 && seen.Rules == 1, "the story's data is not one goal, one database, two nodes and one rule");

    // Stops at the game's start and end, and at the rule's first action.
    DebuggerToBackend global;
    global.mutable_setglobalbreakpoints()->set_breakpoint_mask(DbgSetGlobalBreakpoints::GAME_INIT | DbgSetGlobalBreakpoints::GAME_EXIT);
    if (!front.ExpectResult(front.Send(global), SUCCESS, "the result of the global breakpoints")) return GiveUp();
    DebuggerToBackend unknown;
    unknown.mutable_setbreakpoints()->add_breakpoint()->set_node_id(kRuleNode + 5);
    unknown.mutable_setbreakpoints()->mutable_breakpoint(0)->set_action_index(-1);
    unknown.mutable_setbreakpoints()->mutable_breakpoint(0)->set_breakpoint_mask(MsgBreakpoint::VALID);
    if (!front.ExpectResult(front.Send(unknown), INVALID_NODE_ID, "the result of a breakpoint on a node the story lacks")) return GiveUp();
    DebuggerToBackend breakpoints;
    auto bp = breakpoints.mutable_setbreakpoints()->add_breakpoint();
    bp->set_node_id(kRuleNode);
    bp->set_action_index(0);
    bp->set_breakpoint_mask(MsgBreakpoint::RULE_ACTION);
    if (!front.ExpectResult(front.Send(breakpoints), SUCCESS, "the result of the breakpoint")) return GiveUp();
    {
        std::vector<MsgTuple> rows;
        if (!ReadDatabase(front, rows)) return GiveUp();
        NEED(rows.empty(), "the database has rows before the story ran");
    }
    auto resume = Continue(DbgContinue::CONTINUE);
    if (!front.ExpectResult(front.Send(resume), SUCCESS, "the result of continue at the story's load")) return GiveUp();

    // The game starts.
    if (!front.Expect(in, BackendToDebugger::kGlobalBreakpointTriggered, "the stop at the game's start")) return GiveUp();
    NEED(in.globalbreakpointtriggered().reason() == BkGlobalBreakpointTriggered::GAME_INIT, "the stop is not at the game's start");
    resume = Continue(DbgContinue::CONTINUE);
    if (!front.ExpectResult(front.Send(resume), SUCCESS, "the result of continue at the game's start")) return GiveUp();

    // While the story runs: nothing to read, and a pause at whatever it does next.
    DebuggerToBackend running;
    running.mutable_getdatabasecontents()->set_database_id(kDatabase);
    if (!front.ExpectResult(front.Send(running), NOT_IN_PAUSE, "the result of reading the database while the story runs")) return GiveUp();
    auto pause = Continue(DbgContinue::PAUSE, BreakpointTypeAll);
    if (!front.ExpectResult(front.Send(pause), SUCCESS, "the result of the pause")) return GiveUp();
    session.Set(2);

    // The goal's first action.
    if (!front.Expect(in, BackendToDebugger::kBreakpointTriggered, "the pause")) return GiveUp();
    {
        auto const& hit = in.breakpointtriggered();
        NEED(hit.call_stack_size() == 1 && IsFrame(hit.call_stack(0), MsgFrame::GOAL_INIT_ACTION, 0, kGoal, 0),
            "the pause is not at the goal's first action");
    }
    pause = Continue(DbgContinue::PAUSE, BreakpointTypeAll);
    if (!front.ExpectResult(front.Send(pause), IN_PAUSE, "the result of a pause while paused")) return GiveUp();
    resume = Continue(DbgContinue::CONTINUE);
    if (!front.ExpectResult(front.Send(resume), SUCCESS, "the result of continue at the pause")) return GiveUp();

    // The breakpoint: the goal inserted a fact, the fact went down to the
    // rule, the rule is about to carry out its first action.
    if (!front.Expect(in, BackendToDebugger::kBreakpointTriggered, "the breakpoint")) return GiveUp();
    {
        auto const& hit = in.breakpointtriggered();
        NEED(hit.call_stack_size() == 4, "the breakpoint's call stack has " + std::to_string(hit.call_stack_size()) + " frames, not four");
        NEED(IsFrame(hit.call_stack(0), MsgFrame::GOAL_INIT_ACTION, 0, kGoal, 0), "the first frame is not the goal's action");
        NEED(IsFrame(hit.call_stack(1), MsgFrame::INSERT, kFactsNode, 0, 0) && IsRow(hit.call_stack(1).tuple(), 1, "first"),
            "the second frame is not the insert of (1, \"first\") into the database (the node's InsertTuple, wrapped)");
        NEED(IsFrame(hit.call_stack(2), MsgFrame::PUSHDOWN, kRuleNode, 0, 0) && IsRow(hit.call_stack(2).tuple(), 1, "first"),
            "the third frame is not the fact pushed down to the rule (the node's PushDownTuple, wrapped)");
        NEED(IsFrame(hit.call_stack(3), MsgFrame::RULE_ACTION, kRuleNode, 0, 0), "the last frame is not the rule's first action");
        NEED(hit.query_succeeded() == BkBreakpointTriggered::NOT_A_QUERY, "the breakpoint reports a query");
    }
    front.Send(identify);
    if (!front.Expect(in, BackendToDebugger::kVersionInfo, "the version, paused")) return GiveUp();
    NEED(in.versioninfo().story_loaded() && in.versioninfo().story_initialized(), "the version does not say the story is loaded and started");
    {
        std::vector<MsgTuple> rows;
        if (!ReadDatabase(front, rows)) return GiveUp();
        NEED(rows.size() == 1 && IsRow(rows[0], 1, "first"), "the database's contents are not the one fact (1, \"first\")");
    }
    DebuggerToBackend missing;
    missing.mutable_getdatabasecontents()->set_database_id(kDatabase + 6);
    if (!front.ExpectResult(front.Send(missing), INVALID_DATABASE_ID, "the result of reading a database the story lacks")) return GiveUp();

    // Evaluation on the story's thread while it is paused: is a tuple a fact
    // (the node's IsValid, called as a virtual function of his Node), ...
    for (bool present : { true, false }) {
        DebuggerToBackend evaluate;
        evaluate.mutable_evaluate()->set_type(DbgEvaluate::IS_VALID);
        evaluate.mutable_evaluate()->set_node_id(kFactsNode);
        SetRow(evaluate.mutable_evaluate()->mutable_params(), present ? 1 : 2, present ? "first" : "second");
        uint32_t request = front.Send(evaluate);
        if (!front.Expect(in, BackendToDebugger::kEvaluateRow, "the evaluation's row", request)) return GiveUp();
        NEED(in.evaluaterow().row_size() == 1 && IsRow(in.evaluaterow().row(0), present ? 1 : 2, present ? "first" : "second"),
            "the evaluation's row is not the tuple asked about");
        if (!front.Expect(in, BackendToDebugger::kEvaluateFinished, "the evaluation's end", request)) return GiveUp();
        NEED(in.evaluatefinished().result_code() == SUCCESS && in.evaluatefinished().query_succeeded() == present,
            present ? "the fact is not found by the evaluation" : "a tuple that is no fact is found by the evaluation");
    }
    // ... a fact inserted (the rule fires for it, without stopping, and its
    // DebugBreak text is sent), and deleted again.
    {
        DebuggerToBackend evaluate;
        evaluate.mutable_evaluate()->set_type(DbgEvaluate::INSERT);
        evaluate.mutable_evaluate()->set_node_id(kFactsNode);
        SetRow(evaluate.mutable_evaluate()->mutable_params(), 2, "second");
        uint32_t request = front.Send(evaluate);
        if (!front.Expect(in, BackendToDebugger::kDebugOutput, "the text of the rule's DebugBreak for the inserted fact")) return GiveUp();
        NEED(in.debugoutput().message() == kDebugBreakText, "the DebugBreak text is not the rule's");
        if (!front.Expect(in, BackendToDebugger::kEvaluateFinished, "the insert's end", request)) return GiveUp();
        NEED(in.evaluatefinished().result_code() == SUCCESS, "the insert failed");
        std::vector<MsgTuple> rows;
        if (!ReadDatabase(front, rows)) return GiveUp();
        NEED(rows.size() == 2 && IsRow(rows[0], 1, "first") && IsRow(rows[1], 2, "second"), "the database does not hold the inserted fact");

        evaluate.mutable_evaluate()->set_type(DbgEvaluate::DELETE_);
        request = front.Send(evaluate);
        if (!front.Expect(in, BackendToDebugger::kEvaluateFinished, "the delete's end", request)) return GiveUp();
        NEED(in.evaluatefinished().result_code() == SUCCESS, "the delete failed");
        rows.clear();
        if (!ReadDatabase(front, rows)) return GiveUp();
        NEED(rows.size() == 1 && IsRow(rows[0], 1, "first"), "the database still holds the deleted fact");

        DebuggerToBackend wrong;
        wrong.mutable_evaluate()->set_type(DbgEvaluate::IS_VALID);
        wrong.mutable_evaluate()->set_node_id(kFactsNode);
        wrong.mutable_evaluate()->mutable_params()->add_column()->set_type_id((uint32_t)ValueType::Integer);
        request = front.Send(wrong);
        if (!front.Expect(in, BackendToDebugger::kEvaluateFinished, "the end of an evaluation with too few columns", request)) return GiveUp();
        NEED(in.evaluatefinished().result_code() == INVALID_PARAM_TUPLE_ARITY, "an evaluation with too few columns was not refused");
    }

    // A step over the first action: its DebugBreak text, then the second.
    auto step = Continue(DbgContinue::STEP_OVER, BreakOnRuleAction);
    if (!front.ExpectResult(front.Send(step), SUCCESS, "the result of the step")) return GiveUp();
    if (!front.Expect(in, BackendToDebugger::kDebugOutput, "the text of the rule's DebugBreak")) return GiveUp();
    NEED(in.debugoutput().message() == kDebugBreakText, "the DebugBreak text is not the rule's");
    if (!front.Expect(in, BackendToDebugger::kBreakpointTriggered, "the stop after the step")) return GiveUp();
    {
        auto const& hit = in.breakpointtriggered();
        NEED(hit.call_stack_size() == 4 && IsFrame(hit.call_stack(3), MsgFrame::RULE_ACTION, kRuleNode, 0, 1),
            "the step did not stop at the rule's second action");
    }
    resume = Continue(DbgContinue::CONTINUE);
    if (!front.ExpectResult(front.Send(resume), SUCCESS, "the result of continue after the step")) return GiveUp();

    // The game ends: a stop, then the debugger goes with the story.
    if (!front.Expect(in, BackendToDebugger::kGlobalBreakpointTriggered, "the stop at the game's end")) return GiveUp();
    NEED(in.globalbreakpointtriggered().reason() == BkGlobalBreakpointTriggered::GAME_EXIT, "the stop is not at the game's end");
    resume = Continue(DbgContinue::CONTINUE);
    if (!front.ExpectResult(front.Send(resume), SUCCESS, "the result of continue at the game's end")) return GiveUp();
    if (!front.Expect(in, BackendToDebugger::kDebugSessionEnded, "the end of the debugging session")) return GiveUp();
    front.Send(identify);
    if (!front.Expect(in, BackendToDebugger::kVersionInfo, "the version, after the story")) return GiveUp();
    NEED(!in.versioninfo().story_loaded(), "the version says a story is loaded after it was deleted");
#undef NEED

    seen.Finished = true;
    front.Close();
    session.Set(3);
}

}

std::vector<std::string> RunStoryDebuggerTest(std::string& summary)
{
    auto& osiris = gExtender->GetServer().Osiris();
    auto& wrappers = osiris.GetWrappers();
    auto& config = gExtender->GetConfig();
    auto Done = [] {
        std::lock_guard _(gProblemsLock);
        return gProblems;
    };

    if (!MakeVtables()) {
        Problem("story debugger: no memory for the node classes' vtables");
        return Done();
    }

    // What the binding gives OsirisWrappers::Initialize when the game's
    // library is there (Mac/Binding/OsirisBinding.h): the globals, the node
    // classes' vtables, and the functions to hook, which are hooked as
    // Initialize hooks them.
    auto& globals = wrappers.Globals;
    globals.StringTable = &gStory.Strings;
    globals.Variables = &gStory.Variables;
    globals.Types = &gStory.Types;
    globals.Enums = &gStory.Enums;
    globals.Functions = &gStory.Functions;
    globals.Objects = &gStory.Objects;
    globals.Goals = &gStory.Goals;
    globals.Adapters = &gStory.Adapters;
    globals.Databases = &gStory.Databases;
    globals.Nodes = &gStory.Nodes;
    globals.DebugFlags = &gStory.Flags;
    globals.GetString = &GetStr;
    globals.AddString = &AddStr;
    globals.AddStringRef = &AddStrRef;
    globals.RemoveString = &RemoveStr;
    for (unsigned i = 0; i < std::size(wrappers.VMTs); i++) wrappers.VMTs[i] = gStory.Vtables[i];

    st_handlers[0] = reinterpret_cast<void*>(&OnRegisterDivFunctions);
    st_handlers[1] = reinterpret_cast<void*>(&OnLoad);
    st_handlers[2] = reinterpret_cast<void*>(&OnInitGame);
    st_handlers[3] = reinterpret_cast<void*>(&OnDeleteAllData);
    st_handlers[4] = reinterpret_cast<void*>(&OnFire);
    bool hooked = DetourTransactionBegin() == NO_ERROR;
    if (hooked) {
        DetourUpdateThread(GetCurrentThread());
        wrappers.RegisterDivFunctions.Wrap(reinterpret_cast<void*>(&st_register_div_functions));
        wrappers.InitGame.Wrap(reinterpret_cast<void*>(&st_init_game));
        wrappers.DeleteAllData.Wrap(reinterpret_cast<void*>(&st_delete_all_data));
        wrappers.Load.Wrap(reinterpret_cast<void*>(&st_load));
        wrappers.OriginalRuleActionCallProc = &st_fire;
        wrappers.RuleActionCall.Wrap(reinterpret_cast<void*>(wrappers.OriginalRuleActionCallProc));
        hooked = DetourTransactionCommit() == NO_ERROR;
    }
    if (!hooked) {
        Problem("story debugger: the stand-ins for the story library's functions could not be hooked");
        return Done();
    }

    // The game registers its functions with the library: with EnableDebugger
    // the server starts (OsirisExtender::OnRegisterDIVFunctions).
    uint16_t port = DebugClientFreePort();
    if (port == 0) {
        Problem("story debugger: no free port");
        return Done();
    }
    config.EnableDebugger = true;
    config.DebuggerPort = port;
    DivFunctions functions{ nullptr, &GameCallOrQuery, &GameCallOrQuery, &GameErrorMessage, &GameAssert };
    st_register_div_functions(&gStory.Osiris, &functions);
    if (functions.Call == &GameCallOrQuery || functions.ErrorMessage == &GameErrorMessage) {
        Problem("story debugger: the hook on COsiris::RegisterDIVFunctions did not run");
        return Done();
    }

    Session session;
    Frontend front;
    Seen seen;
    std::thread frontend([&] { RunFrontend(session, front, port, seen); });

    if (session.WaitFor(1) && session.Stage < 3) {
        // The story is loaded (the debugger is made, and stops), the game
        // starts (it stops again).
        st_load(&gStory.Osiris, nullptr);
        if (!osiris.GetVMTWrappers()) Problem("story debugger: the node classes' vtables were not wrapped at the story's load");
        st_init_game(&gStory.Osiris);
        if (session.WaitFor(2) && session.Stage < 3) {
            // The goal's INIT section runs.
            auto& actions = gStory.TheGoal->InitCalls->Actions;
            for (auto action = actions.Head->Next; action != actions.Head; action = action->Next) {
                st_fire(action->Item, nullptr, nullptr, nullptr, nullptr);
            }
        }
        // The game ends its session.
        st_delete_all_data(&gStory.Osiris, false);
    }
    if (!session.WaitFor(3)) Problem("story debugger: the frontend did not finish");
    frontend.join();

    // The insert and the rule's two actions, with the evaluation's insert
    // between the breakpoint and the first action it stopped before.
    std::vector<std::string> expected{ kDatabaseName, "DebugBreak", kLastAction, "DebugBreak", kLastAction };
    if (seen.Finished && gStory.Fired != expected) {
        std::string list;
        for (auto const& name : gStory.Fired) list += (list.empty() ? "" : ", ") + name;
        Problem("story debugger: the story's actions ran as " + list);
    }
    if (gStory.Registered != 1 || gStory.Loaded != 1 || gStory.Started != 1 || gStory.Deleted != 1) {
        Problem("story debugger: the library's functions did not each run once under their hooks");
    }

    // As in the game, the server stays for the next frontend and the next story.
    if (seen.Finished) {
        summary = "port " + std::to_string(port) + ", protocol " + std::to_string(DebugMessageHandler::ProtocolVersion)
            + ": identified; stopped at the story's load, the game's start, a pause, a rule action's breakpoint (four frames: goal action, "
              "insert, push-down, rule action), a step and the game's end; the story synchronised (1 goal, 1 database, 2 nodes, 1 rule); "
              "the database read; a fact evaluated, inserted and deleted; the DebugBreak text received";
    }
    return Done();
}

END_NS()
