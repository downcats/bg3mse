// The debug adapter test of the start-up harness (StartupLuaDebugAdapter.h).
//
// The two ends of Norbyte's Lua debugger together: the server in the
// extender (Lua/Debugger), which the harness runs as the game does, and his
// debug adapter (LuaDebugger/, built for macOS by Mac/LuaDebugger), which an
// editor runs as a program and speaks the Debug Adapter Protocol to over the
// program's standard input and output. Here the editor is a thread of the
// test, and the game's Lua thread runs a script over and over. The editor
// sends each request the adapter handles at least once: it starts the
// adapter, has it attach to the server (the adapter's "launch" request),
// lists the loaded scripts and the mods, sets a breakpoint in the script and
// when the script stops there asks for the stack, the scopes, the variables
// and evaluations, steps over, into and out of a function, clears the
// breakpoint, continues, pauses the running script, breaks on an error,
// fetches a built-in library's source and disconnects. The script is a mod's
// as far as the debugger can tell: a loose file in the game's Data folder,
// under the name and the full path the server's state gives a mod's script.
#include <stdafx.h>

#include <Extender/ScriptExtender.h>
#include <Extender/Server/ExtensionStateServer.h>
#include <Lua/Debugger/LuaDebugger.h>
#include <Lua/Debugger/LuaDebugMessages.h>

#include "StartupLuaDebugAdapter.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>

// A child process on pipes (Mac/Tests/StartupTestsPosix.cpp).
int ChildStart(char const* program, int* toChild, int* fromChild);
bool ChildWrite(int fd, void const* data, size_t size);
long ChildRead(int fd, void* data, size_t size, int timeoutMs);
void ChildStop(int pid, int toChild, int fromChild);

BEGIN_NS(mac::test)

namespace {

std::mutex gProblemsLock;
std::vector<std::string> gProblems;

void Problem(std::string text)
{
    std::lock_guard _(gProblemsLock);
    gProblems.push_back("debug adapter: " + std::move(text));
}

// The files a breakpoint can name: the full path of each script the state
// loaded from the game's data (ExtensionStateBase::LuaLoadGameFile).
struct LoadedPaths : ExtensionStateBase
{
    static std::unordered_map<STDString, STDString>& Of(ExtensionStateBase& state)
    {
        return state.*(&LoadedPaths::loadedFileFullPaths_);
    }
};

// The mod's script: its folder, its file, and the name the state loads it by
// (ExtensionStateBase::LuaLoadModScript).
constexpr char kModFolder[] = "Bg3mseDebugAdapterTest";
constexpr char kScriptFile[] = "BootstrapServer.lua";
constexpr char kScriptName[] = "Bg3mseDebugAdapterTest/BootstrapServer.lua";
// The script, as the file holds it. Its sixth line has text outside ASCII:
// the adapter's answers carry it, and each says how many bytes it is long.
// The game sets Bg3mseDebugAdapterFail for one run when the editor asks for
// an error.
constexpr char kScriptLocal[] = "bg3mse \xC3\xA9\xE2\x9C\x93";  // "bg3mse é✓"
constexpr char kScriptError[] = "bg3mse: the error the debugger breaks on";
constexpr char kScript[] =
    "local function add(a, b)\n"
    "    local sum = a + b\n"
    "    return sum\n"
    "end\n"
    "local answer = 42\n"
    "local name = \"bg3mse \xC3\xA9\xE2\x9C\x93\"\n"
    "local data = { count = 3 }\n"
    "local length = #name\n"
    "local after = add(answer, 1)\n"
    "if Bg3mseDebugAdapterFail then error(\"bg3mse: the error the debugger breaks on\") end\n"
    "return after\n";
constexpr int kBreakLine = 8;
constexpr int kCallLine = 9;
constexpr int kFunctionLine = 2;
constexpr int kErrorLine = 10;
// The built-in library whose source the editor fetches, as a stack frame in
// it names it (the adapter keeps only the file name of a frame's source).
constexpr char kBuiltinName[] = "Event.lua";
constexpr char kBuiltinResource[] = "Libs/Event.lua";
// The Lua server's thread, as the adapter numbers it (DAPMessageHandler.cs).
constexpr int kServerThread = 1;

std::string Quoted(std::string_view text)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    writer.String(text.data(), (rapidjson::SizeType)text.size());
    return buffer.GetString();
}

bool Has(rapidjson::Value const& object, char const* member)
{
    return object.IsObject() && object.HasMember(member);
}

std::string StringOf(rapidjson::Value const& object, char const* member)
{
    return Has(object, member) && object[member].IsString() ? object[member].GetString() : std::string();
}

int IntOf(rapidjson::Value const& object, char const* member)
{
    return Has(object, member) && object[member].IsInt() ? object[member].GetInt() : -1;
}

// The editor's side of the Debug Adapter Protocol: messages of JSON, each
// after a header that gives its length in bytes.
class Editor
{
public:
    bool Start(char const* adapter)
    {
        pid_ = ChildStart(adapter, &to_, &from_);
        return pid_ > 0;
    }

    void Stop()
    {
        if (pid_ > 0) ChildStop(pid_, to_, from_);
        pid_ = -1;
    }

    // Sends a request with the next sequence number (1, 2, ...: DAPStream
    // refuses any other) and returns the number.
    int Request(char const* command, std::string const& arguments = "")
    {
        int seq = ++sent_;
        std::string body = "{\"seq\":" + std::to_string(seq) + ",\"type\":\"request\",\"command\":\"" + command + "\"";
        if (!arguments.empty()) body += ",\"arguments\":" + arguments;
        body += "}";
        std::string message = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        if (!ChildWrite(to_, message.data(), message.size())) Problem(std::string("the request ") + command + " could not be written");
        return seq;
    }

    // The response to the request: events that come before it are kept.
    bool Response(int seq, char const* command, rapidjson::Document& doc)
    {
        for (;;) {
            std::string text;
            if (!Read(text, command)) return false;
            if (!Parse(text, doc)) return false;
            std::string type = doc["type"].GetString();
            if (type == "event") {
                events_.push_back(std::move(text));
                continue;
            }
            if (type != "response" || IntOf(doc, "request_seq") != seq || StringOf(doc, "command") != command) {
                Problem(std::string("something else than the response to ") + command + ": " + text);
                return false;
            }
            if (!Has(doc, "success") || !doc["success"].IsBool() || !doc["success"].GetBool()) {
                Problem(std::string(command) + " failed: " + (Has(doc, "message") ? StringOf(doc, "message") : text));
                return false;
            }
            return true;
        }
    }

    // A request and its response.
    bool Ask(char const* command, std::string const& arguments, rapidjson::Document& doc)
    {
        return Response(Request(command, arguments), command, doc);
    }

    // The next event of that name; other events before it are dropped.
    bool Event(char const* name, rapidjson::Document& doc)
    {
        for (;;) {
            std::string text;
            if (!events_.empty()) {
                text = std::move(events_.front());
                events_.pop_front();
            } else if (!Read(text, name)) {
                return false;
            }
            if (!Parse(text, doc)) return false;
            if (std::string(doc["type"].GetString()) != "event") {
                Problem(std::string("a response where the event ") + name + " was expected: " + text);
                return false;
            }
            if (StringOf(doc, "event") == name) return true;
        }
    }

    // What the adapter's output events said, as they came.
    std::string Output;

private:
    // One message: the header's Content-Length bytes, exactly.
    bool Read(std::string& body, char const* awaiting)
    {
        size_t headerEnd;
        while ((headerEnd = buffer_.find("\r\n\r\n")) == std::string::npos) {
            if (!Fill(awaiting)) return false;
        }
        std::string header = buffer_.substr(0, headerEnd);
        constexpr char kLength[] = "Content-Length: ";
        if (header.rfind(kLength, 0) != 0) {
            Problem(std::string("a header that is not a Content-Length where ") + awaiting + " was awaited: " + header);
            return false;
        }
        size_t length = (size_t)atol(header.c_str() + sizeof(kLength) - 1);
        buffer_.erase(0, headerEnd + 4);
        while (buffer_.size() < length) {
            if (!Fill(awaiting)) return false;
        }
        body = buffer_.substr(0, length);
        buffer_.erase(0, length);
        rapidjson::Document doc;
        doc.Parse(body.data(), body.size());
        if (IntOf(doc, "seq") != received_ + 1) {
            Problem(std::string("the adapter's messages are not numbered in order at: ") + body);
            return false;
        }
        received_++;
        if (StringOf(doc, "type") == "event" && StringOf(doc, "event") == "output" && Has(doc, "body")) {
            Output += StringOf(doc["body"], "output");
        }
        return true;
    }

    bool Fill(char const* awaiting)
    {
        char chunk[4096];
        long n = ChildRead(from_, chunk, sizeof(chunk), 15000);
        if (n <= 0) {
            Problem(std::string(n == 0 ? "the adapter ended" : "nothing came from the adapter") + " where " + awaiting + " was awaited");
            return false;
        }
        buffer_.append(chunk, (size_t)n);
        return true;
    }

    static bool Parse(std::string const& text, rapidjson::Document& doc)
    {
        doc.Parse(text.data(), text.size());
        if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("type") || !doc["type"].IsString()) {
            // A length that counted characters, not bytes, cuts a message
            // with text outside ASCII short.
            Problem("a message that is not a JSON object of the protocol: " + text);
            return false;
        }
        return true;
    }

    int pid_{ -1 };
    int to_{ -1 };
    int from_{ -1 };
    int sent_{ 0 };
    int received_{ 0 };
    std::string buffer_;
    std::deque<std::string> events_;
};

// What the editor and the Lua thread hand each other.
struct Session
{
    std::mutex Lock;
    std::condition_variable Changed;
    int Stage{ 0 }; // 1: the session is configured and the script may run, 2: finished (or given up)
    // Set by the editor for one run of the script that ends in an error.
    std::atomic<bool> FailNext{ false };

    void Set(int stage)
    {
        std::lock_guard _(Lock);
        if (stage > Stage) Stage = stage;
        Changed.notify_all();
    }

    int Now()
    {
        std::lock_guard _(Lock);
        return Stage;
    }

    bool WaitFor(int stage, int seconds)
    {
        std::unique_lock lk(Lock);
        return Changed.wait_for(lk, std::chrono::seconds(seconds), [&] { return Stage >= stage; });
    }
};

// The stack of the server's thread, top first.
bool Stack(Editor& editor, rapidjson::Document& doc)
{
    if (!editor.Ask("stackTrace", "{\"threadId\":" + std::to_string(kServerThread) + ",\"startFrame\":0,\"levels\":20}", doc)) return false;
    if (!Has(doc, "body") || !Has(doc["body"], "stackFrames") || !doc["body"]["stackFrames"].IsArray() || doc["body"]["stackFrames"].Empty()) {
        Problem("the stack trace has no frames");
        return false;
    }
    if (IntOf(doc["body"], "totalFrames") != (int)doc["body"]["stackFrames"].Size()) {
        Problem("the stack trace's totalFrames is not the number of its frames");
        return false;
    }
    return true;
}

// That the frame is the script's at the line, under the path the breakpoint
// was set by.
bool IsScriptFrame(rapidjson::Value const& frame, std::string const& path, int line, char const* what)
{
    if (!Has(frame, "id") || !Has(frame, "source") || StringOf(frame["source"], "path") != path
        || StringOf(frame["source"], "name") != kScriptFile || IntOf(frame, "line") != line) {
        Problem(std::string(what) + " is " + StringOf(Has(frame, "source") ? frame["source"] : frame, "path") + ":"
            + std::to_string(IntOf(frame, "line")) + ", not " + path + ":" + std::to_string(line));
        return false;
    }
    return true;
}

// The variables of a reference, as name = value.
bool Variables(Editor& editor, int64_t reference, std::unordered_map<std::string, std::string>& values,
    std::unordered_map<std::string, int64_t>* references = nullptr)
{
    rapidjson::Document doc;
    if (!editor.Ask("variables", "{\"variablesReference\":" + std::to_string(reference) + "}", doc)) return false;
    if (!Has(doc, "body") || !Has(doc["body"], "variables") || !doc["body"]["variables"].IsArray()) {
        Problem("the variables have no list");
        return false;
    }
    for (auto const& variable : doc["body"]["variables"].GetArray()) {
        values[StringOf(variable, "name")] = StringOf(variable, "value");
        if (references && Has(variable, "variablesReference") && variable["variablesReference"].IsInt64()) {
            (*references)[StringOf(variable, "name")] = variable["variablesReference"].GetInt64();
        }
    }
    return true;
}

std::string Listed(std::unordered_map<std::string, std::string> const& values)
{
    std::string text;
    for (auto const& [name, value] : values) text += name + " = " + value + "; ";
    return text;
}

// The Locals and Upvalues scopes of a frame.
bool Scopes(Editor& editor, int frameId, int64_t& locals, int64_t& upvalues)
{
    rapidjson::Document doc;
    if (!editor.Ask("scopes", "{\"frameId\":" + std::to_string(frameId) + "}", doc)) return false;
    if (!Has(doc, "body") || !Has(doc["body"], "scopes") || !doc["body"]["scopes"].IsArray() || doc["body"]["scopes"].Size() != 2
        || StringOf(doc["body"]["scopes"][0], "name") != "Locals" || StringOf(doc["body"]["scopes"][1], "name") != "Upvalues") {
        Problem("the scopes are not Locals and Upvalues");
        return false;
    }
    locals = doc["body"]["scopes"][0]["variablesReference"].GetInt64();
    upvalues = doc["body"]["scopes"][1]["variablesReference"].GetInt64();
    return true;
}

// A step of some kind and the stop it ends in.
bool Step(Editor& editor, char const* command, char const* reason)
{
    rapidjson::Document doc;
    if (!editor.Ask(command, "{\"threadId\":" + std::to_string(kServerThread) + "}", doc)) return false;
    if (!editor.Event("stopped", doc)) return false;
    if (!Has(doc, "body") || StringOf(doc["body"], "reason") != reason || IntOf(doc["body"], "threadId") != kServerThread) {
        Problem(std::string("the stop after ") + command + " is not a " + reason + " on the server's thread");
        return false;
    }
    return true;
}

bool Continue(Editor& editor)
{
    rapidjson::Document doc;
    if (!editor.Ask("continue", "{\"threadId\":" + std::to_string(kServerThread) + "}", doc)) return false;
    if (!Has(doc, "body") || !Has(doc["body"], "allThreadsContinued")) {
        Problem("the answer to continue does not say whether all threads continued");
        return false;
    }
    return true;
}

bool SetBreakpoints(Editor& editor, std::string const& path, std::vector<int> const& lines)
{
    std::string breakpoints, numbers;
    for (int line : lines) {
        if (!breakpoints.empty()) {
            breakpoints += ",";
            numbers += ",";
        }
        breakpoints += "{\"line\":" + std::to_string(line) + "}";
        numbers += std::to_string(line);
    }
    rapidjson::Document doc;
    if (!editor.Ask("setBreakpoints", "{\"source\":{\"name\":" + Quoted(kScriptFile) + ",\"path\":" + Quoted(path)
        + "},\"breakpoints\":[" + breakpoints + "],\"lines\":[" + numbers + "],\"sourceModified\":false}", doc)) return false;
    if (!Has(doc, "body") || !Has(doc["body"], "breakpoints") || doc["body"]["breakpoints"].Size() != lines.size()) {
        Problem("the breakpoints were not taken as set");
        return false;
    }
    for (size_t i = 0; i < lines.size(); i++) {
        auto const& bp = doc["body"]["breakpoints"][(rapidjson::SizeType)i];
        if (!Has(bp, "verified") || !bp["verified"].GetBool() || IntOf(bp, "line") != lines[i]) {
            Problem("the breakpoint at line " + std::to_string(lines[i]) + " was not taken as set");
            return false;
        }
    }
    return true;
}

void RunEditor(Session& session, Editor& editor, char const* adapter, uint16_t port, std::string const& path, bool& finished)
{
    rapidjson::Document doc;
    auto GiveUp = [&] {
        // Without its frontend the server resumes a paused script
        // (lua::dbg::DebugMessageHandler::HandleDisconnect).
        editor.Stop();
        session.Set(2);
    };

    if (!editor.Start(adapter)) {
        Problem(std::string("could not start ") + adapter);
        return GiveUp();
    }

    // initialize: the capabilities the editor's views rely on, and the
    // adapter's own event.
    if (!editor.Ask("initialize", "{\"clientID\":\"bg3mse\",\"clientName\":\"bg3mse_startup_tests\",\"adapterID\":\"bg3lua\","
        "\"locale\":\"en\",\"linesStartAt1\":true,\"columnsStartAt1\":true,\"pathFormat\":\"path\",\"supportsVariableType\":true}", doc)) return GiveUp();
    for (char const* capability : { "supportsConfigurationDoneRequest", "supportsEvaluateForHovers", "supportsModulesRequest", "supportsLoadedSourcesRequest" }) {
        if (!Has(doc, "body") || !Has(doc["body"], capability) || !doc["body"][capability].GetBool()) {
            Problem(std::string("the adapter's capabilities lack ") + capability);
            return GiveUp();
        }
    }
    if (!editor.Event("luaProtocolVersion", doc)) return GiveUp();

    // launch, as Norbyte's editor extension writes it: the adapter connects
    // to the game and answers once the game has answered it; "initialized"
    // follows the game's debuggerReady.
    if (!editor.Ask("launch", "{\"type\":\"bg3lua\",\"request\":\"launch\",\"name\":\"BG3 Script Extender Lua Debug\",\"program\":\"\","
        "\"backendHost\":\"127.0.0.1\",\"backendPort\":" + std::to_string(port)
        + ",\"dbgOptions\":{\"breakOnError\":true,\"breakOnGenericError\":false,\"omitCppFrames\":false}}", doc)) return GiveUp();
    if (!editor.Event("initialized", doc)) return GiveUp();
    if (editor.Output.find("Debugger backend ready") == std::string::npos) {
        Problem("the adapter did not say the backend is ready");
        return GiveUp();
    }

    if (!SetBreakpoints(editor, path, { kBreakLine })) return GiveUp();
    if (!editor.Ask("configurationDone", "", doc)) return GiveUp();
    if (!editor.Ask("threads", "", doc)) return GiveUp();
    if (!Has(doc, "body") || !Has(doc["body"], "threads") || doc["body"]["threads"].Size() != 2
        || IntOf(doc["body"]["threads"][0], "id") != kServerThread) {
        Problem("the threads are not the Lua server and the Lua client");
        return GiveUp();
    }

    // The editor's Loaded Scripts view: the script, by the path the game
    // loaded it from.
    if (!editor.Ask("loadedSources", "{}", doc)) return GiveUp();
    {
        bool listed = false;
        if (Has(doc, "body") && Has(doc["body"], "sources") && doc["body"]["sources"].IsArray()) {
            for (auto const& source : doc["body"]["sources"].GetArray()) {
                if (StringOf(source, "name") == kScriptName && StringOf(source, "path") == path) listed = true;
            }
        }
        if (!listed) {
            Problem("the loaded sources lack " + std::string(kScriptName) + " at " + path);
            return GiveUp();
        }
    }
    // Its Modules view: the mods the server's state has, as many as it says.
    if (!editor.Ask("modules", "{\"startModule\":0,\"moduleCount\":0}", doc)) return GiveUp();
    if (!Has(doc, "body") || !Has(doc["body"], "modules") || !doc["body"]["modules"].IsArray()
        || IntOf(doc["body"], "totalModules") != (int)doc["body"]["modules"].Size()) {
        Problem("the modules are not a list as long as totalModules");
        return GiveUp();
    }

    // The adapter passes the breakpoint on to the game and does not wait for
    // its answer: the Lua thread runs the script until it stops.
    session.Set(1);

    if (!editor.Event("stopped", doc)) return GiveUp();
    if (!Has(doc, "body") || StringOf(doc["body"], "reason") != "breakpoint" || IntOf(doc["body"], "threadId") != kServerThread) {
        Problem("the stop is not the breakpoint's on the server's thread");
        return GiveUp();
    }

    if (!Stack(editor, doc)) return GiveUp();
    if (!IsScriptFrame(doc["body"]["stackFrames"][0], path, kBreakLine, "the top frame at the breakpoint")) return GiveUp();
    int frameId = IntOf(doc["body"]["stackFrames"][0], "id");

    int64_t locals = 0, upvalues = 0;
    if (!Scopes(editor, frameId, locals, upvalues)) return GiveUp();
    {
        std::unordered_map<std::string, std::string> values;
        std::unordered_map<std::string, int64_t> references;
        if (!Variables(editor, locals, values, &references)) return GiveUp();
        if (values["answer"] != "42" || values["name"] != std::string("\"") + kScriptLocal + "\"" || references["data"] <= 0) {
            Problem("the frame's locals lack answer = 42, name = \"" + std::string(kScriptLocal) + "\" or the table data: " + Listed(values));
            return GiveUp();
        }
        // A table opened in the Variables view.
        std::unordered_map<std::string, std::string> fields;
        if (!Variables(editor, references["data"], fields)) return GiveUp();
        if (fields["count"] != "3") {
            Problem("the table data lacks count = 3: " + Listed(fields));
            return GiveUp();
        }
    }
    {
        std::unordered_map<std::string, std::string> values;
        if (!Variables(editor, upvalues, values)) return GiveUp();
        if (!values.contains("_ENV")) {
            Problem("the frame's upvalues lack _ENV: " + Listed(values));
            return GiveUp();
        }
    }

    // A watch and a hover.
    if (!editor.Ask("evaluate", "{\"expression\":\"answer + 1\",\"frameId\":" + std::to_string(frameId) + ",\"context\":\"watch\"}", doc)) return GiveUp();
    if (!Has(doc, "body") || StringOf(doc["body"], "result") != "43") {
        Problem("answer + 1 was not evaluated to 43");
        return GiveUp();
    }
    if (!editor.Ask("evaluate", "{\"expression\":\"name\",\"frameId\":" + std::to_string(frameId) + ",\"context\":\"hover\"}", doc)) return GiveUp();
    if (!Has(doc, "body") || StringOf(doc["body"], "result") != std::string("\"") + kScriptLocal + "\"") {
        Problem("name was not evaluated to the script's text");
        return GiveUp();
    }

    // Over the line, into the function on the next, and out of it again.
    if (!Step(editor, "next", "step")) return GiveUp();
    if (!Stack(editor, doc)) return GiveUp();
    if (!IsScriptFrame(doc["body"]["stackFrames"][0], path, kCallLine, "the top frame after next")) return GiveUp();
    size_t depth = doc["body"]["stackFrames"].Size();

    if (!Step(editor, "stepIn", "step")) return GiveUp();
    if (!Stack(editor, doc)) return GiveUp();
    {
        auto const& frames = doc["body"]["stackFrames"];
        if (frames.Size() != depth + 1 || !IsScriptFrame(frames[0], path, kFunctionLine, "the top frame after stepIn")
            || !IsScriptFrame(frames[1], path, kCallLine, "the caller's frame after stepIn")) {
            if (frames.Size() != depth + 1) Problem("stepIn did not add a frame");
            return GiveUp();
        }
        if (StringOf(frames[0], "name") != "add") {
            Problem("the function stepped into is named " + StringOf(frames[0], "name") + ", not add");
            return GiveUp();
        }
        frameId = IntOf(frames[0], "id");
    }
    if (!Scopes(editor, frameId, locals, upvalues)) return GiveUp();
    {
        std::unordered_map<std::string, std::string> values;
        if (!Variables(editor, locals, values)) return GiveUp();
        if (values["a"] != "42" || values["b"] != "1") {
            Problem("the function's locals lack a = 42 and b = 1: " + Listed(values));
            return GiveUp();
        }
    }

    if (!Step(editor, "stepOut", "step")) return GiveUp();
    if (!Stack(editor, doc)) return GiveUp();
    {
        // Back in the caller: on the line of the call or on the next.
        auto const& frames = doc["body"]["stackFrames"];
        int line = IntOf(frames[0], "line");
        if (frames.Size() != depth || !IsScriptFrame(frames[0], path, line == kCallLine ? kCallLine : kCallLine + 1, "the top frame after stepOut")) {
            if (frames.Size() != depth) Problem("stepOut did not leave the function's frame");
            return GiveUp();
        }
    }

    // The breakpoint removed, the script runs through and goes on running.
    if (!SetBreakpoints(editor, path, {})) return GiveUp();
    if (!Continue(editor)) return GiveUp();

    // The pause button: the running script stops wherever it is.
    if (!Step(editor, "pause", "pause")) return GiveUp();
    if (!Stack(editor, doc)) return GiveUp();
    {
        auto const& frame = doc["body"]["stackFrames"][0];
        if (!IsScriptFrame(frame, path, IntOf(frame, "line"), "the top frame at the pause") || IntOf(frame, "line") < 1) return GiveUp();
    }
    if (!Continue(editor)) return GiveUp();

    // An error in the script, with breakOnError.
    session.FailNext = true;
    if (!editor.Event("stopped", doc)) return GiveUp();
    if (!Has(doc, "body") || StringOf(doc["body"], "reason") != "exception" || IntOf(doc["body"], "threadId") != kServerThread
        || StringOf(doc["body"], "text").find(kScriptError) == std::string::npos) {
        Problem("the stop at the error is not an exception's with the error's text");
        return GiveUp();
    }
    if (!Stack(editor, doc)) return GiveUp();
    {
        bool found = false;
        for (auto const& frame : doc["body"]["stackFrames"].GetArray()) {
            if (Has(frame, "source") && StringOf(frame["source"], "path") == path) {
                found = IsScriptFrame(frame, path, kErrorLine, "the script's frame at the error");
                break;
            }
        }
        if (!found) {
            Problem("the stack at the error has no frame of the script at line " + std::to_string(kErrorLine));
            return GiveUp();
        }
    }
    if (!Continue(editor)) return GiveUp();
    // The game logs the error, and the log reaches the editor's console.
    while (editor.Output.find(kScriptError) == std::string::npos) {
        if (!editor.Event("output", doc)) {
            Problem("the error's log line did not reach the editor's console");
            return GiveUp();
        }
    }

    // A built-in library's source, as the editor asks for a frame in one: by
    // the name the frame has, with no path.
    if (!editor.Ask("source", "{\"source\":{\"name\":" + Quoted(kBuiltinName) + ",\"path\":\"\"},\"sourceReference\":0}", doc)) return GiveUp();
    {
        auto expected = gExtender->GetLuaBuiltinBundle().GetResource(kBuiltinResource);
        if (!expected || !Has(doc, "body") || StringOf(doc["body"], "content") != expected->c_str()) {
            Problem(std::string("the source of ") + kBuiltinName + " is not the built-in " + kBuiltinResource);
            return GiveUp();
        }
    }

    if (!editor.Ask("disconnect", "{\"restart\":false}", doc)) return GiveUp();
    finished = true;
    // An editor ends the adapter once it has answered.
    editor.Stop();
    session.Set(2);
}

}

std::vector<std::string> RunLuaDebugAdapterTest(char const* adapter, uint16_t port, std::string& summary)
{
    auto Done = [] {
        std::lock_guard _(gProblemsLock);
        return gProblems;
    };
    auto& server = gExtender->GetServer();
    auto* debugger = gExtender->GetLuaDebugger();
    if (!server.HasExtensionState() || !debugger) {
        Problem("no server state or no debugger");
        return Done();
    }
    auto& state = server.GetExtensionState();

    // The game's Data folder, a stand-in in a folder of its own: on the Mac
    // it is inside the application, and its path has spaces and a quote. The
    // state's full path of a mod's script is the game's (StaticSymbols::ToPath
    // of the mod's path under the Data root), and an editor opens the file by
    // the path it has on disk.
    char const* temporary = getenv("TMPDIR");
    std::string pattern = std::string(temporary && *temporary ? temporary : "/tmp") + "/bg3mse debug adapter XXXXXX";
    char* folder = pattern.data();
    if (!mkdtemp(folder)) {
        Problem("no temporary folder for the game's data");
        return Done();
    }
    char resolved[PATH_MAX];
    std::filesystem::path top = realpath(folder, resolved) ? resolved : folder;
    std::filesystem::path data = top / "Baldur's Gate 3.app" / "Contents" / "Data";
    std::filesystem::path script = data / "Mods" / kModFolder / "ScriptExtender" / "Lua" / kScriptFile;
    std::error_code error;
    std::filesystem::create_directories(script.parent_path(), error);
    std::string path = script.string();
    FILE* file = fopen(path.c_str(), "wb");
    if (!file || fwrite(kScript, 1, sizeof(kScript) - 1, file) != sizeof(kScript) - 1) {
        Problem("the script could not be written to " + path);
        if (file) fclose(file);
        std::filesystem::remove_all(top, error);
        return Done();
    }
    fclose(file);

    auto& symbols = GetStaticSymbols();
    auto gameRoots = symbols.ls__PathRoots;
    STDString dataRoot(data.string().c_str());
    STDString* roots[19]{};
    roots[(unsigned)PathRootType::Data] = &dataRoot;
    symbols.ls__PathRoots = roots;
    STDString fullPath = symbols.ToPath(STDString("Mods/") + kModFolder + "/ScriptExtender/Lua/" + kScriptFile, PathRootType::Data);
    symbols.ls__PathRoots = gameRoots;
    if (path != fullPath.c_str()) {
        Problem(std::string("the state's path of the script is ") + fullPath.c_str() + ", not the file's " + path);
    }
    LoadedPaths::Of(state)[kScriptName] = fullPath;

    Session session;
    Editor editor;
    bool finished = false;
    std::thread thread([&] { RunEditor(session, editor, adapter, port, path, finished); });

    // The game's side: the debugger's hooks go on at the server's tick, a
    // breakpoint takes effect at the next, and the script runs every 10 ms
    // until the editor is done. Until the breakpoint has reached the game the
    // script runs through.
    unsigned runs = 0, failed = 0;
    int result = 0;
    if (session.WaitFor(1, 60)) {
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (session.Now() < 2 && std::chrono::steady_clock::now() < until) {
            debugger->ServerTick();
            LuaVirtualPin lua(state);
            if (!lua) break;
            auto L = lua->GetState();
            int top = lua_gettop(L);
            bool fail = session.FailNext.exchange(false);
            if (fail) {
                lua_pushboolean(L, 1);
                lua_setglobal(L, "Bg3mseDebugAdapterFail");
            }
            auto returned = lua->LoadScript(kScript, kScriptName);
            if (returned && *returned >= 1 && lua_isinteger(L, -1)) result = (int)lua_tointeger(L, -1);
            lua_settop(L, top);
            if (fail) {
                if (!returned) failed++;
                lua_pushnil(L);
                lua_setglobal(L, "Bg3mseDebugAdapterFail");
            }
            runs++;
            usleep(10000);
        }
    }
    if (!session.WaitFor(2, 60)) Problem("the editor did not finish");
    thread.join();

    // The adapter has gone: the server notices, and turns debugging off at
    // the next tick.
    for (int i = 0; i < 500 && debugger->IsDebuggerReady(); i++) usleep(10000);
    if (debugger->IsDebuggerReady()) Problem("the server did not notice that the adapter had gone");
    debugger->ServerTick();
    LoadedPaths::Of(state).erase(kScriptName);
    std::filesystem::remove_all(top, error);

    if (finished && result != 43) Problem("the script did not return 43 after the session (it returned " + std::to_string(result) + ")");
    if (finished && failed != 1) Problem("the script ended in its error " + std::to_string(failed) + " times, not once");
    if (finished) {
        summary = "initialize, launch (attached to 127.0.0.1:" + std::to_string(port) + "), loadedSources, modules, a breakpoint at "
            + kScriptName + ":" + std::to_string(kBreakLine) + ", the stack with the script's path, the locals with text outside ASCII, "
              "a table, the upvalues, two evaluations, next, stepIn, stepOut, continue, pause, a break on an error, "
              "a built-in library's source, disconnect; the script ran " + std::to_string(runs) + " times";
    }
    return Done();
}

END_NS()
