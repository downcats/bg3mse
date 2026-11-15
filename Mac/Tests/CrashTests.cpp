// Tests of the guarded blocks and the crash reporter (Mac/Crash). No game
// needed: every crash is a child process's, and the reporter's host is this
// file.

#include <Crash/CrashReporter.h>
#include <Crash/ThreadList.h>
#include <Platform/FaultGuard.h>
#include <Platform/FunctionExtents.h>
#include <Platform/Log.h>
#include <Release/Version.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dispatch/dispatch.h>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <pthread.h>
#include <spawn.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace bg3se::mac;
using bg3se::gDisableCrashReportingCount;
using bg3se::gRegisteredTrampolines;

extern char** environ;

extern "C" {
// CrashTargets.S
uint64_t crash_test_read_null();
uint64_t crash_test_read_null_vectors();
void crash_test_recurse();
[[noreturn]] void crash_test_spin_with_fp(uint64_t fp, uint64_t volatile* counter);
// CrashForeign.S, in a dylib of its own
void* crash_foreign_call(void* pair);
void crash_foreign_read_null(void*);
}

// CrashTestsBlocks.cpp: BEGIN_GUARDED() body(arg); END_GUARDED(), and one such
// block inside another.
void CrashTestGuarded(void (*body)(void*), void* arg);
void CrashTestGuardedTwice(void (*body)(void*), void* arg, int& before, int& after);

namespace {

int gFailures = 0;
char const* gTest = "";

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; name(); } static void name()

// What a child writes for its parent. Only vsnprintf and write: the host's
// functions call it on the reporter's thread.
void Say(char const* fmt, ...) __attribute__((format(printf, 1, 2)));
void Say(char const* fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((size_t)n > sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    write(3, line, (size_t)n);
}

// --- The reporter's host -----------------------------------------------------------

// The child's directory for reports, and the report it wrote, once the
// reporter has said so.
char gReportDirectory[PATH_MAX];
char gReportPath[PATH_MAX];
bool gForce = false;
bool gNativeApiUsed = false;
void const* gExcluded = nullptr;
char const* gLuaText = nullptr;
bool gLuaFaults = false;
bool gModulesFault = false;
// The reporter's thread, once it has been seen, when the reporter runs for a
// stopped thread: it must not allocate from then on.
bool gReporterMustNotAllocate = false;
std::atomic<pthread_t> gReporterThread{ nullptr };
// A host that breaks that rule, to see that the test would notice.
bool gHostAllocates = false;
// A host that takes its time over the report, which is open under its
// temporary name by then, and says when it has begun.
unsigned gHostTakesMs = 0;
std::atomic<bool> gHostBegan{ false };

bool HostForce()
{
    if (gReporterMustNotAllocate) gReporterThread = pthread_self();
    return gForce;
}

bool HostNativeApiUsed()
{
    return gNativeApiUsed;
}

size_t HostReportDirectory(char* out, size_t size)
{
    size_t length = strlen(gReportDirectory);
    if (length >= size) return 0;
    memcpy(out, gReportDirectory, length + 1);
    return length;
}

size_t HostExcluded(void const** out, size_t max)
{
    if (!gExcluded || max == 0) return 0;
    out[0] = gExcluded;
    return 1;
}

void HostBuildSections(crash::ReportWriter& report)
{
    Say("host: build sections");
    if (gHostTakesMs) {
        gHostBegan = true;
        usleep(gHostTakesMs * 1000);
    }
    if (gHostAllocates) {
        void* volatile p = malloc(16);
        free(p);
    }
    report.AddSection(crash::tag::ExtenderVersion, "34.0.0.0");
    report.AddSection(crash::tag::GameVersion, "4.1.1.7");
    report.AddSection(crash::tag::BuildDate, "Jan  1 2026 00:00:00");
    report.AddSection(crash::tag::ReleaseChannel, "Devel");
    report.AddSection(crash::tag::BuildType, "Devel");
}

void HostLua(FaultRecord const& fault, crash::TextBuffer& text)
{
    if (gLuaFaults) {
        text.Append("lost");
        crash_test_read_null();
    }
    if (gLuaText) text.Append(gLuaText);
}

void HostModules(crash::TextBuffer& text)
{
    if (gModulesFault) crash_test_read_null();
    text.Append("28ac9ce2-2aba-8cda-b3b5-6e922f71b6b8\tGustav\t4.0.0.1\r\n");
}

void HostState(crash::ReportWriter& report)
{
    uint32_t client = 13;
    report.AddSection(crash::tag::ClientState, { reinterpret_cast<char const*>(&client), sizeof(client) });
    uint64_t session = 123456;
    report.AddSection(crash::tag::SessionLength, { reinterpret_cast<char const*>(&session), sizeof(session) });
}

}

constinit crash::Host const bg3se::mac::crash::gHost{
    .ForceCrashReporting = HostForce,
    .NativeApiUsed = HostNativeApiUsed,
    .ReportDirectory = HostReportDirectory,
    .ExcludedFunctions = HostExcluded,
    .BuildSections = HostBuildSections,
    .LuaBacktrace = HostLua,
    .ModuleList = HostModules,
    .StateSections = HostState,
};

// What the reporter does last: here the test says that it was asked to, with
// which path, and what was there by then.
void bg3se::mac::crash::LaunchCrashReporter(char const* reportPath)
{
    // <directory>/crash_<time>.crpt, and the text beside it.
    size_t directory = strlen(gReportDirectory), length = strlen(reportPath);
    size_t name = directory + 1 + strlen(crash::kReportPrefix) + crash::kReportTimeLength;
    bool named = length == name + strlen(crash::kReportSuffix) && strncmp(reportPath, gReportDirectory, directory) == 0
        && reportPath[directory] == '/' && strncmp(reportPath + directory + 1, crash::kReportPrefix, strlen(crash::kReportPrefix)) == 0
        && strcmp(reportPath + name, crash::kReportSuffix) == 0;
    struct stat st;
    bool report = stat(reportPath, &st) == 0;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%.*s%s", (int)(named ? name : length), reportPath, crash::kBacktraceSuffix);
    bool text = stat(path, &st) == 0;
    snprintf(gReportPath, sizeof(gReportPath), "%s", reportPath);
    Say("launch crash reporter: %s, report %s, text %s", named ? "the report's path" : reportPath,
        report ? "written" : "missing", text ? "written" : "missing");
}

// The allocator of everything linked into this executable (the reporter and
// the platform layer included; the system's libraries keep their own malloc,
// and take operator new from here): it says when the reporter's thread
// allocates.
namespace {

void NoteAllocation()
{
    pthread_t reporter = gReporterThread.load(std::memory_order_relaxed);
    if (reporter && reporter == pthread_self()) Say("allocated while reporting");
}

}

// A thread of the other-threads cases that stops inside this allocator, as
// a thread of the game's may be stopped holding its lock when the reporter
// runs: it is there for good once it has called malloc.
std::atomic<pthread_t> gParkInMalloc{ nullptr };
std::atomic<bool> gParkedInMalloc{ false };

extern "C" __attribute__((noinline)) void CrashTestParkedInMalloc()
{
    gParkedInMalloc = true;
    for (;;) pause();
}

extern "C" void* malloc(size_t size)
{
    NoteAllocation();
    if (gParkInMalloc.load(std::memory_order_relaxed) == pthread_self()) CrashTestParkedInMalloc();
    return malloc_zone_malloc(malloc_default_zone(), size);
}

extern "C" void* calloc(size_t count, size_t size)
{
    NoteAllocation();
    return malloc_zone_calloc(malloc_default_zone(), count, size);
}

extern "C" void* realloc(void* p, size_t size)
{
    NoteAllocation();
    return malloc_zone_realloc(malloc_default_zone(), p, size);
}

void* operator new(size_t size)
{
    void* p = malloc(size ? size : 1);
    if (!p) abort();
    return p;
}

void* operator new[](size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }
void operator delete[](void* p, size_t) noexcept { free(p); }

// A hook of the test's: called from the foreign dylib and calling back into
// it, as a hook of the extender's is called by the game and calls the game.
extern "C" __attribute__((noinline)) void CrashTestHook(void* inner)
{
    crash_foreign_call(inner);
    __asm__ volatile("");
}

struct Unwound
{
    ~Unwound() { Say("unwound"); }
};

extern "C" __attribute__((noinline)) void CrashTestThrow(void*)
{
    Unwound unwound;
    throw std::runtime_error("thrown in a guarded block");
}

// Exceptions nothing catches.
extern "C" __attribute__((noinline)) void CrashTestThrowUncaught(void*)
{
    throw std::runtime_error("nobody catches this");
}

extern "C" __attribute__((noinline)) void CrashTestThrowInt(void*)
{
    throw 42;
}

// A std::exception whose what() faults.
struct BadWhat : std::exception
{
    char const* what() const noexcept override
    {
        crash_test_read_null();
        return "not reached";
    }
};

extern "C" __attribute__((noinline)) void CrashTestThrowBadWhat(void*)
{
    throw BadWhat();
}

namespace {

// --- Running a child -----------------------------------------------------------------

struct ChildResult
{
    bool Ran{ false };
    bool Hung{ false };
    int Status{ 0 };
    std::string Output;

    bool Exited(int code) const { return Ran && !Hung && WIFEXITED(Status) && WEXITSTATUS(Status) == code; }
    bool KilledBy(int signal) const { return Ran && !Hung && WIFSIGNALED(Status) && WTERMSIG(Status) == signal; }
    bool Killed() const { return Ran && !Hung && WIFSIGNALED(Status); }
    bool Said(std::string_view line) const { return ("\n" + Output).find("\n" + std::string(line) + "\n") != std::string::npos; }
    // The lines of the log, when the child has put its log on the pipe: those at `level` that have `part` in them.
    int Logged(std::string_view level, std::string_view part) const
    {
        int count = 0;
        std::string marker = "] [" + std::string(level) + "] ";
        for (size_t at = 0; at < Output.size();) {
            size_t end = Output.find('\n', at);
            if (end == std::string::npos) end = Output.size();
            std::string_view line(Output.data() + at, end - at);
            if (line.find(marker) != std::string_view::npos && line.find(part) != std::string_view::npos) count++;
            at = end + 1;
        }
        return count;
    }
    int Count(std::string_view line) const
    {
        int count = 0;
        std::string text = "\n" + Output, wanted = "\n" + std::string(line) + "\n";
        for (size_t at = text.find(wanted); at != std::string::npos; at = text.find(wanted, at + 1)) count++;
        return count;
    }
    // The number after "<name> " on a line the child wrote.
    uint64_t Number(std::string_view name) const
    {
        std::string text = "\n" + Output, wanted = "\n" + std::string(name) + " ";
        size_t at = text.find(wanted);
        return at == std::string::npos ? 0 : strtoull(text.c_str() + at + wanted.size(), nullptr, 0);
    }
};

std::string gSelf;

ChildResult RunChildOf(std::string const& executable, char const* name, std::string const& dir)
{
    ChildResult result;
    int fds[2];
    if (pipe(fds) != 0) return result;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // The read end is closed first: it may be fd 3 itself.
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 3);
    // What the system's libraries say of a process that is ending is not the test's.
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    std::string exe = executable, dirArg = dir;
    char* argv[] = { exe.data(), const_cast<char*>("--child"), const_cast<char*>(name), dirArg.data(), nullptr };
    pid_t pid = 0;
    int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return result;
    }
    result.Ran = true;
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        char buffer[4096];
        ssize_t n;
        while ((n = read(fds[0], buffer, sizeof(buffer))) > 0) result.Output.append(buffer, (size_t)n);
        pid_t done = waitpid(pid, &result.Status, WNOHANG);
        if (done == pid) break;
        if (std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &result.Status, 0);
            result.Hung = true;
            break;
        }
        usleep(5000);
    }
    char buffer[4096];
    ssize_t n;
    while ((n = read(fds[0], buffer, sizeof(buffer))) > 0) result.Output.append(buffer, (size_t)n);
    close(fds[0]);
    if (result.Hung) printf("  child %s hung\n", name);
    return result;
}

// --- Reading a report back -----------------------------------------------------------

struct Section
{
    uint32_t Tag;
    std::string Body;
};

struct Report
{
    bool Exists{ false };
    bool WellFormed{ false };
    std::vector<Section> Sections;

    std::string const* Find(uint32_t tag) const
    {
        for (auto const& section : Sections) {
            if (section.Tag == tag) return &section.Body;
        }
        return nullptr;
    }
    bool Has(uint32_t tag) const { return Find(tag) != nullptr; }
    std::string Body(uint32_t tag) const
    {
        auto body = Find(tag);
        return body ? *body : std::string();
    }
    // The value of "key=" on a line of its own in a text section.
    std::string Value(uint32_t tag, std::string_view key) const
    {
        std::string text = "\n" + Body(tag), wanted = "\n" + std::string(key) + "=";
        size_t at = text.find(wanted);
        if (at == std::string::npos) return {};
        size_t from = at + wanted.size();
        return text.substr(from, text.find('\n', from) - from);
    }
    // The first line of a text section that contains `part`.
    std::string Line(uint32_t tag, std::string_view part) const
    {
        std::istringstream lines(Body(tag));
        std::string line;
        while (std::getline(lines, line)) {
            if (line.find(part) != std::string::npos) return line;
        }
        return {};
    }
};

bool ReadFile(std::string const& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    std::ostringstream text;
    text << f.rdbuf();
    out = text.str();
    return true;
}

Report ReadReport(std::string const& path)
{
    Report report;
    std::string bytes;
    if (!ReadFile(path, bytes)) return report;
    report.Exists = true;
    uint32_t signature = 0;
    if (bytes.size() < 4) return report;
    memcpy(&signature, bytes.data(), 4);
    if (signature != crash::tag::Signature) return report;
    size_t at = 4;
    while (at < bytes.size()) {
        uint32_t head[2];
        if (bytes.size() - at < sizeof(head)) return report;
        memcpy(head, bytes.data() + at, sizeof(head));
        at += sizeof(head);
        if (bytes.size() - at < head[1]) return report;
        report.Sections.push_back({ head[0], bytes.substr(at, head[1]) });
        at += head[1];
    }
    report.WellFormed = true;
    return report;
}

// " key=value" inside a line of several, the value ending at a blank.
std::string Field(std::string const& line, std::string_view key)
{
    std::string text = " " + line, wanted = " " + std::string(key) + "=";
    size_t at = text.find(wanted);
    if (at == std::string::npos) return {};
    size_t from = at + wanted.size();
    return text.substr(from, text.find(' ', from) - from);
}

uint64_t Hex(std::string const& text)
{
    return strtoull(text.c_str(), nullptr, 16);
}

// One crash: a directory for its report, the child, and what the child left.
struct Crash
{
    std::string Dir;
    ChildResult Child;
    Report Written;
    std::string BacktraceText;
    bool HasBacktraceText{ false };
    std::string StackFile;
    bool HasStackFile{ false };

    // The names of the files the child left in its directory, sorted.
    std::vector<std::string> Names;
    // The report's file, without its directory; empty unless there is exactly one.
    std::string ReportName;

    // `before` may put files into the directory before the child runs.
    Crash(char const* name, std::string const& executable = gSelf, std::function<void(std::string const&)> before = {})
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "bg3mse-crash-tests.XXXXXX").string();
        if (!mkdtemp(pattern.data())) {
            printf("  no temporary directory for %s\n", name);
            return;
        }
        Dir = pattern;
        std::vector<std::string> there;
        if (before) {
            before(Dir);
            there = List();
        }
        Child = RunChildOf(executable.empty() ? gSelf : executable, name, Dir);
        Names = List();
        // The report is the one that was not there before.
        for (auto const& file : Names) {
            if (!file.ends_with(crash::kReportSuffix) || std::find(there.begin(), there.end(), file) != there.end()) continue;
            ReportName = ReportName.empty() ? file : std::string("more than one");
        }
        if (ReportName.empty() || ReportName == "more than one") return;
        std::string stem = Dir + "/" + ReportName.substr(0, ReportName.size() - strlen(crash::kReportSuffix));
        Written = ReadReport(stem + crash::kReportSuffix);
        HasBacktraceText = ReadFile(stem + crash::kBacktraceSuffix, BacktraceText);
        HasStackFile = ReadFile(stem + crash::kStackSuffix, StackFile);
    }
    std::vector<std::string> List() const
    {
        std::vector<std::string> names;
        for (auto const& entry : std::filesystem::directory_iterator(Dir)) names.push_back(entry.path().filename().string());
        std::sort(names.begin(), names.end());
        return names;
    }
    ~Crash()
    {
        std::error_code ignored;
        if (!Dir.empty()) std::filesystem::remove_all(Dir, ignored);
    }

    // Nothing of a report, finished or not.
    bool LeftNothing() const
    {
        std::error_code ignored;
        return !Dir.empty() && std::filesystem::is_empty(Dir, ignored);
    }
    void Show(char const* what) const
    {
        printf("  %s: status 0x%x, report %s, said:\n%s", what, Child.Status, Written.Exists ? "written" : "missing", Child.Output.c_str());
    }
};

// --- Cases run in a child ------------------------------------------------------------

int gChildFailures = 0;
#define CHILD_CHECK(cond) do { if (!(cond)) { Say("FAIL %s (line %d)", #cond, __LINE__); gChildFailures++; } } while (0)

void ReadNull(void*)
{
    crash_test_read_null();
}

// What the parent compares the report with.
void SayWhereThingsAre()
{
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    Say("fault-function 0x%llx", (unsigned long long)reinterpret_cast<uint64_t>(&crash_test_read_null));
    Say("image-base 0x%llx", (unsigned long long)reinterpret_cast<uint64_t>(_dyld_get_image_header(0)));
    Say("image-slide 0x%llx", (unsigned long long)_dyld_get_image_vmaddr_slide(0));
    Say("thread-id %llu", (unsigned long long)tid);
}

int ChildBlockRuns()
{
    int ran = 0, before = 0, after = 0;
    CrashTestGuarded([](void* p) { ++*static_cast<int*>(p); }, &ran);
    CHILD_CHECK(ran == 1);
    CrashTestGuardedTwice([](void* p) { ++*static_cast<int*>(p); }, &ran, before, after);
    CHILD_CHECK(ran == 2 && before == 1 && after == 1);
    CHILD_CHECK(crash::Start());
    CrashTestGuardedTwice([](void* p) { ++*static_cast<int*>(p); }, &ran, before, after);
    CHILD_CHECK(ran == 3 && before == 2 && after == 2);
    return gChildFailures;
}

int ChildGuardedFault()
{
    gReporterMustNotAllocate = true;
    gLuaText = "Mods/Example/Server.lua:12 OnTick\n[C++ code] xpcall\n";
    CHILD_CHECK(crash::Start());
    crash::NoteLogMessage("a warning");
    crash::NoteLogMessage("an error");
    SayWhereThingsAre();
    CrashTestGuarded(ReadNull, nullptr);
    Say("survived");
    return 0;
}

int ChildGuardedFaultHostAllocates()
{
    gReporterMustNotAllocate = true;
    gHostAllocates = true;
    CHILD_CHECK(crash::Start());
    CrashTestGuarded(ReadNull, nullptr);
    return 0;
}

int ChildGuardedFaultNotStarted()
{
    CrashTestGuarded(ReadNull, nullptr);
    Say("survived");
    return 0;
}

int ChildGuardedFaultStopped()
{
    CHILD_CHECK(crash::Start());
    crash::Stop();
    CHILD_CHECK(!crash::Started());
    CrashTestGuarded(ReadNull, nullptr);
    Say("survived");
    return 0;
}

int ChildGuardedFaultDisabled()
{
    CHILD_CHECK(crash::Start());
    ++gDisableCrashReportingCount;
    CrashTestGuarded(ReadNull, nullptr);
    Say("survived");
    return 0;
}

int ChildGuardedFaultDisabledForced()
{
    gForce = true;
    CHILD_CHECK(crash::Start());
    ++gDisableCrashReportingCount;
    CrashTestGuarded(ReadNull, nullptr);
    Say("survived");
    return 0;
}

int ChildNestedBlocksFault()
{
    gReporterMustNotAllocate = true;
    CHILD_CHECK(crash::Start());
    int before = 0, after = 0;
    CrashTestGuardedTwice(ReadNull, nullptr, before, after);
    Say("survived");
    return 0;
}

void* OverflowInBlock(void*)
{
    CrashTestGuarded([](void*) { crash_test_recurse(); }, nullptr);
    Say("survived");
    return nullptr;
}

int OverflowOnAThread()
{
    CHILD_CHECK(crash::Start());
    pthread_t thread;
    CHILD_CHECK(pthread_create(&thread, nullptr, OverflowInBlock, nullptr) == 0);
    pthread_join(thread, nullptr);
    return 0;
}

int ChildGuardedOverflow()
{
    return OverflowOnAThread();
}

int ChildGuardedOverflowForced()
{
    gForce = true;
    return OverflowOnAThread();
}

int ChildUnguardedFault()
{
    gReporterMustNotAllocate = true;
    CHILD_CHECK(crash::Start());
    SayWhereThingsAre();
    crash_test_read_null();
    Say("survived");
    return 0;
}

int ChildUnguardedFaultNotStarted()
{
    crash_test_read_null();
    return 0;
}

int ChildUnguardedFaultDisabled()
{
    CHILD_CHECK(crash::Start());
    ++gDisableCrashReportingCount;
    crash_test_read_null();
    return 0;
}

int ChildUnguardedFaultStopped()
{
    CHILD_CHECK(crash::Start());
    crash::Stop();
    CHILD_CHECK(!crash::Started());
    crash_test_read_null();
    return 0;
}

int ChildStartedAgainAndAgain()
{
    // More times than there are observer slots.
    for (int i = 0; i < 12; i++) {
        CHILD_CHECK(crash::Start());
        CHILD_CHECK(crash::Start());
        CHILD_CHECK(crash::Started());
        crash::Stop();
        crash::Stop();
        CHILD_CHECK(!crash::Started());
    }
    CHILD_CHECK(crash::Start());
    crash_test_read_null();
    return 0;
}

int ChildAbort()
{
    CHILD_CHECK(crash::Start());
    abort();
}

struct Pair
{
    void (*Fn)(void*);
    void* Arg;
};

// A thread that starts in the foreign dylib and runs `first` from there.
void RunOnForeignThread(Pair& first)
{
    pthread_t thread;
    if (pthread_create(&thread, nullptr, crash_foreign_call, &first) != 0) return;
    pthread_join(thread, nullptr);
    Say("survived");
}

int ChildForeignOnly()
{
    CHILD_CHECK(crash::Start());
    Pair fault{ crash_foreign_read_null, nullptr };
    RunOnForeignThread(fault);
    return 0;
}

int ChildForeignOnlyForced()
{
    gForce = true;
    return ChildForeignOnly();
}

int ForeignThroughHook()
{
    CHILD_CHECK(crash::Start());
    Pair fault{ crash_foreign_read_null, nullptr };
    Pair hook{ CrashTestHook, &fault };
    RunOnForeignThread(hook);
    return 0;
}

int ChildForeignThroughHook()
{
    return ForeignThroughHook();
}

int ChildForeignThroughRegisteredHook()
{
    gRegisteredTrampolines.insert(reinterpret_cast<void const*>(&CrashTestHook));
    return ForeignThroughHook();
}

int ChildForeignThroughExcludedHook()
{
    gExcluded = reinterpret_cast<void const*>(&CrashTestHook);
    return ForeignThroughHook();
}

int ChildFaultInRegisteredFunction()
{
    gRegisteredTrampolines.insert(reinterpret_cast<void const*>(&crash_test_read_null));
    CHILD_CHECK(crash::Start());
    crash_test_read_null();
    return 0;
}

FaultRecord RecordOf(std::initializer_list<FaultFrame> frames)
{
    FaultRecord record{};
    record.Kind = FaultKind::BadAccess;
    for (auto const& frame : frames) record.Frames[record.FrameCount++] = frame;
    return record;
}

// The rule on records made by hand, which no crash is needed for.
int ChildRule()
{
    CHILD_CHECK(crash::Start());
    FunctionExtent hook = FindLoadedFunction(reinterpret_cast<void const*>(&CrashTestHook));
    CHILD_CHECK(hook.Ok() && hook.Bytes() >= 12);
    if (!hook.Ok()) return 1;

    FaultFrame foreign{ reinterpret_cast<uint64_t>(&crash_foreign_read_null), 0, nullptr, 0 };
    auto ours = [](uint64_t pc) { return FaultFrame{ pc, 0, nullptr, FrameInThisImage }; };
    auto related = [](FaultRecord const& record) { return crash::IsExtensionRelatedCrash(record, false); };

    CHILD_CHECK(!related(RecordOf({})));
    CHILD_CHECK(!related(RecordOf({ foreign, foreign })));
    CHILD_CHECK(related(RecordOf({ ours(hook.Start + 4) })));
    CHILD_CHECK(related(RecordOf({ foreign, ours(hook.Start + 8) })));

    // A registered function's frames do not count, unless the fault is in it.
    gRegisteredTrampolines.insert(reinterpret_cast<void const*>(hook.Start));
    CHILD_CHECK(!related(RecordOf({ foreign, ours(hook.Start + 8) })));
    CHILD_CHECK(!related(RecordOf({ foreign, foreign, ours(hook.Start + 4), foreign })));
    CHILD_CHECK(related(RecordOf({ ours(hook.Start + 8) })));
    CHILD_CHECK(related(RecordOf({ foreign, ours(hook.Start + 8), ours(reinterpret_cast<uint64_t>(&crash_test_read_null) + 4) })));
    // A return address is the address after a call: after the function's
    // last instruction it is still the function's, and at its first
    // instruction it is the one before's.
    CHILD_CHECK(!related(RecordOf({ foreign, ours(hook.End) })));
    CHILD_CHECK(related(RecordOf({ foreign, ours(hook.Start) })));
    gRegisteredTrampolines.erase(reinterpret_cast<void const*>(hook.Start));
    CHILD_CHECK(related(RecordOf({ foreign, ours(hook.Start + 8) })));

    // So do those of the host's fixed list.
    gExcluded = reinterpret_cast<void const*>(hook.Start);
    CHILD_CHECK(!related(RecordOf({ foreign, ours(hook.Start + 8) })));
    CHILD_CHECK(related(RecordOf({ ours(hook.Start + 8) })));
    gExcluded = nullptr;

    // An address that starts no function excludes nothing.
    gRegisteredTrampolines.insert(reinterpret_cast<void const*>(hook.Start + 4));
    gRegisteredTrampolines.insert(reinterpret_cast<void const*>(0x10));
    CHILD_CHECK(related(RecordOf({ foreign, ours(hook.Start + 8) })));
    gRegisteredTrampolines.clear();

    // A stack overflow is not reported, and nothing is while the count is raised.
    FaultRecord overflow = RecordOf({ ours(hook.Start + 4) });
    overflow.Kind = FaultKind::StackOverflow;
    CHILD_CHECK(!related(overflow));
    CHILD_CHECK(!crash::IsExtensionRelatedCrash(overflow, true));
    ++gDisableCrashReportingCount;
    CHILD_CHECK(!related(RecordOf({ ours(hook.Start + 4) })));
    CHILD_CHECK(!crash::IsExtensionRelatedCrash(RecordOf({}), true));
    // Forcing reports everything.
    gForce = true;
    CHILD_CHECK(related(RecordOf({ ours(hook.Start + 4) })));
    CHILD_CHECK(related(RecordOf({ foreign })));
    CHILD_CHECK(related(overflow));
    --gDisableCrashReportingCount;
    gForce = false;

    // A guarded block's own function is a frame of the extender.
    CHILD_CHECK(crash::IsExtensionRelatedCrash(RecordOf({}), true));
    CHILD_CHECK(crash::IsExtensionRelatedCrash(RecordOf({ foreign }), true));

    // Once a native module has used the extender's API nothing is reported,
    // forced or not: the crash may be the module's.
    gNativeApiUsed = true;
    CHILD_CHECK(!related(RecordOf({ ours(hook.Start + 4) })));
    CHILD_CHECK(!crash::IsExtensionRelatedCrash(RecordOf({ foreign }), true));
    gForce = true;
    CHILD_CHECK(!related(RecordOf({ ours(hook.Start + 4) })));
    gForce = false;
    gNativeApiUsed = false;
    CHILD_CHECK(related(RecordOf({ ours(hook.Start + 4) })));
    return gChildFailures;
}

void ThrowOutOfABlock()
{
    try {
        CrashTestGuarded(CrashTestThrow, nullptr);
    } catch (std::exception const& e) {
        Say("caught outside: %s", e.what());
    }
}

int ChildExceptionLeavesBlock()
{
    CHILD_CHECK(crash::Start());
    SayWhereThingsAre();
    ThrowOutOfABlock();
    return 0;
}

int ChildExceptionLeavesBlockNotStarted()
{
    ThrowOutOfABlock();
    return 0;
}

int ChildExceptionLeavesBlockDisabled()
{
    CHILD_CHECK(crash::Start());
    ++gDisableCrashReportingCount;
    ThrowOutOfABlock();
    --gDisableCrashReportingCount;
    // The block is as it was before: the next one runs.
    int ran = 0;
    CrashTestGuarded([](void* p) { ++*static_cast<int*>(p); }, &ran);
    CHILD_CHECK(ran == 1);
    return gChildFailures;
}

// Once a native module has used the extender's API, forced or not:
// ShouldReportCrashes, which his filter asks first.
int ChildExceptionLeavesBlockNativeApi()
{
    gForce = true;
    gNativeApiUsed = true;
    CHILD_CHECK(crash::Start());
    ThrowOutOfABlock();
    int ran = 0;
    CrashTestGuarded([](void* p) { ++*static_cast<int*>(p); }, &ran);
    CHILD_CHECK(ran == 1);
    return gChildFailures;
}

// The same exception with nothing outside the block to catch it: it ends in
// std::terminate, whose report says what the rule makes of it.
int ChildExceptionLeavesBlockNativeApiUncaught()
{
    gNativeApiUsed = true;
    CHILD_CHECK(crash::Start());
    CrashTestGuarded(CrashTestThrowUncaught, nullptr);
    Say("survived");
    return 0;
}

int ChildExceptionLeavesBlockDisabledForced()
{
    gForce = true;
    CHILD_CHECK(crash::Start());
    ++gDisableCrashReportingCount;
    ThrowOutOfABlock();
    return 0;
}

int ChildExceptionCaughtInBlock()
{
    CHILD_CHECK(crash::Start());
    int caught = 0;
    CrashTestGuarded([](void* p) {
        try {
            CrashTestThrow(nullptr);
        } catch (std::exception const&) {
            ++*static_cast<int*>(p);
        }
    }, &caught);
    CHILD_CHECK(caught == 1);
    return gChildFailures;
}

int ChildHostFaults()
{
    gLuaFaults = true;
    gModulesFault = true;
    CHILD_CHECK(crash::Start());
    crash_test_read_null();
    return 0;
}

// Stand-ins for the game's own crash handling.
char const* ReportState()
{
    struct stat st;
    if (gReportPath[0]) return stat(gReportPath, &st) == 0 ? "written" : "missing";
    // A crash Norbyte's rule leaves to the game is not told to
    // LaunchCrashReporter: its report is the one in the directory.
    bool found = false;
    if (DIR* dir = opendir(gReportDirectory)) {
        while (dirent* entry = readdir(dir)) {
            std::string_view name = entry->d_name;
            found = found || (name.starts_with(crash::kReportPrefix) && name.ends_with(crash::kReportSuffix));
        }
        closedir(dir);
    }
    return found ? "written" : "missing";
}

constexpr int kFatalSignals[] = { SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGTRAP };

// A handler that was there before the reporter's: it is what the fault goes
// on to.
void EarlierHandler(int signal, siginfo_t*, void*)
{
    Say("earlier handler: report %s", ReportState());
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigaction(signal, &action, nullptr);
}

// A handler installed after the reporter's that passes the signal on: it
// does its work, puts the six signals back to their defaults and calls the
// handler that was installed before it. (The game's own passes nothing on:
// GameHandler, below.)
struct sigaction gBeforeLater[NSIG]{};

void LaterHandler(int signal, siginfo_t* info, void* context)
{
    Say("later handler: report %s", ReportState());
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    for (int s : kFatalSignals) sigaction(s, &action, nullptr);
    auto const& before = gBeforeLater[signal];
    if (before.sa_flags & SA_SIGINFO) {
        before.sa_sigaction(signal, info, context);
    } else if (before.sa_handler != SIG_DFL && before.sa_handler != SIG_IGN) {
        before.sa_handler(signal);
    }
    Say("later handler: done, report %s", ReportState());
}

void StartBetweenTwoHandlers()
{
    struct sigaction earlier{};
    earlier.sa_sigaction = EarlierHandler;
    earlier.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &earlier, nullptr);

    CHILD_CHECK(crash::Start());

    struct sigaction later{};
    later.sa_sigaction = LaterHandler;
    later.sa_flags = SA_SIGINFO;
    for (int s : kFatalSignals) sigaction(s, &later, &gBeforeLater[s]);
}

int ChildHandlersUnguarded()
{
    StartBetweenTwoHandlers();
    crash_test_read_null();
    Say("survived");
    return 0;
}

int ChildHandlersGuarded()
{
    StartBetweenTwoHandlers();
    CrashTestGuarded(ReadNull, nullptr);
    Say("survived");
    return 0;
}

int ChildHandlersException()
{
    StartBetweenTwoHandlers();
    ThrowOutOfABlock();
    Say("survived");
    return 0;
}

// --- The game's handler ---

// A handler installed after the reporter's that behaves as the game's does
// (PLCrashReporter as the game uses it, read in the game's code): it puts
// the six signals back to their defaults, does its work, calls nothing that
// was installed before it, and raises the signal again.
void GameHandler(int signal, siginfo_t*, void*)
{
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    for (int s : kFatalSignals) sigaction(s, &action, nullptr);
    Say("game handler: report %s", ReportState());
    raise(signal);
}

void InstallGameHandler()
{
    struct sigaction action{};
    action.sa_sigaction = GameHandler;
    action.sa_flags = SA_SIGINFO;
    for (int s : kFatalSignals) sigaction(s, &action, nullptr);
}

// Norbyte's start-up as the game sees it: the reporter is started and
// stopped while the dylib is loaded, and the game installs its handler when
// it has finished launching.
void StartThenTheGameInstallsItsHandler()
{
    CHILD_CHECK(crash::Start());
    crash::Stop();
    InstallGameHandler();
}

// A fault before the reporter is started again: the game's handler is in
// front and tells the reporter nothing.
int ChildGameHandlerInFront()
{
    StartThenTheGameInstallsItsHandler();
    crash_test_read_null();
    Say("survived");
    return 0;
}

// Started again, as at a change of the game's state (and as the hook on the
// game's core library does for what comes before the first one).
int ChildGameHandlerUnguarded()
{
    gReporterMustNotAllocate = true;
    StartThenTheGameInstallsItsHandler();
    CHILD_CHECK(crash::Start());
    SayWhereThingsAre();
    crash_test_read_null();
    Say("survived");
    return 0;
}

int ChildGameHandlerGuarded()
{
    StartThenTheGameInstallsItsHandler();
    CHILD_CHECK(crash::Start());
    CrashTestGuarded(ReadNull, nullptr);
    Say("survived");
    return 0;
}

int ChildGameHandlerAbort()
{
    StartThenTheGameInstallsItsHandler();
    CHILD_CHECK(crash::Start());
    abort();
}

// --- A second crash while a report is being written ---

void* FaultInABlockOnAThread(void*)
{
    CrashTestGuarded(ReadNull, nullptr);
    return nullptr;
}

void* ThrowOutOfABlockOnAThread(void*)
{
    ThrowOutOfABlock();
    return nullptr;
}

// Starts `first` on a thread and returns once its report is being written.
void WhileAReportIsWritten(void* (*first)(void*), bool withTheGamesHandler = false)
{
    gHostTakesMs = 700;
    CHILD_CHECK(crash::Start());
    if (withTheGamesHandler) {
        InstallGameHandler();
        CHILD_CHECK(crash::Start());
    }
    pthread_t thread;
    CHILD_CHECK(pthread_create(&thread, nullptr, first, nullptr) == 0);
    while (!gHostBegan) usleep(1000);
}

int ChildFaultWhileAReportIsWritten()
{
    WhileAReportIsWritten(FaultInABlockOnAThread);
    crash_test_read_null();
    Say("survived");
    return 0;
}

int ChildFaultWhileAReportIsWrittenInTheGame()
{
    WhileAReportIsWritten(FaultInABlockOnAThread, true);
    crash_test_read_null();
    Say("survived");
    return 0;
}

int ChildExceptionWhileAReportIsWritten()
{
    WhileAReportIsWritten(FaultInABlockOnAThread);
    ThrowOutOfABlock();
    Say("survived");
    return 0;
}

int ChildFaultWhileAnExceptionsReportIsWritten()
{
    WhileAReportIsWritten(ThrowOutOfABlockOnAThread);
    crash_test_read_null();
    Say("survived");
    return 0;
}

// A fault that is reported and then recovered by a guard around the block,
// and the process's next one: another fault, somewhere else, a second later
// (a report is named for its second).
int ChildFaultAfterAReportedOne()
{
    CHILD_CHECK(crash::Start());
    bool completed = GuardedCall([] { CrashTestGuarded(ReadNull, nullptr); });
    CHILD_CHECK(!completed);
    Say("recovered");
    usleep(1200 * 1000);
    crash_foreign_read_null(nullptr);
    Say("survived");
    return 0;
}

// --- A fault in a handler of the extender's that the game calls from its own update ---

// What the first crash of a play session was (2026-11-11): on one of the
// game's worker threads, ScriptExtender::OnECSUpdate runs the game's update
// in a guarded block with crash reporting disabled around it, as every hook
// disables it around the game's function; the game's update calls a handler
// the extender subscribed, and the handler faults.
__attribute__((noinline)) void HandlerFaults(void*)
{
    crash_test_read_null();
    // Not a tail call: the handler is a frame, as the extender's was.
    __asm__ volatile("");
}

void GameUpdateInABlock(void*)
{
    ++gDisableCrashReportingCount;
    Pair handler{ HandlerFaults, nullptr };
    crash_foreign_call(&handler);
    --gDisableCrashReportingCount;
}

void* GameWorker(void*)
{
    SayWhereThingsAre();
    CrashTestGuarded(GameUpdateInABlock, nullptr);
    return nullptr;
}

// The log as the parent reads it: on the child's pipe, among what it says.
void LogToTheParent()
{
    SetLogStream(fdopen(3, "w"));
}

int FaultInAHandlerTheGameCalls(bool withTheGamesHandler)
{
    gReporterMustNotAllocate = true;
    LogToTheParent();
    if (withTheGamesHandler) {
        StartThenTheGameInstallsItsHandler();
        // Not started again since: the game's handler is in front.
    } else {
        CHILD_CHECK(crash::Start());
    }
    pthread_t thread;
    CHILD_CHECK(pthread_create(&thread, nullptr, GameWorker, nullptr) == 0);
    pthread_join(thread, nullptr);
    Say("survived");
    return 0;
}

int ChildFaultInAHandlerTheGameCalls()
{
    return FaultInAHandlerTheGameCalls(false);
}

int ChildFaultInAHandlerTheGameCallsBehindTheGamesHandler()
{
    return FaultInAHandlerTheGameCalls(true);
}

// The game's own crash, on a thread of its own with no frame of the
// extender's, outside any block.
int ChildForeignOnlyLogged()
{
    LogToTheParent();
    return ChildForeignOnly();
}

int ChildGuardedFaultVectors()
{
    CHILD_CHECK(crash::Start());
    CrashTestGuarded([](void*) { crash_test_read_null_vectors(); }, nullptr);
    Say("survived");
    return 0;
}

// std::terminate.
int ChildTerminateUncaught()
{
    LogToTheParent();
    CHILD_CHECK(crash::Start());
    SayWhereThingsAre();
    CrashTestThrowUncaught(nullptr);
    Say("survived");
    return 0;
}

int ChildTerminateDisabled()
{
    LogToTheParent();
    CHILD_CHECK(crash::Start());
    ++gDisableCrashReportingCount;
    CrashTestThrowUncaught(nullptr);
    Say("survived");
    return 0;
}

int ChildTerminateInt()
{
    CHILD_CHECK(crash::Start());
    CrashTestThrowInt(nullptr);
    Say("survived");
    return 0;
}

int ChildTerminateBadWhat()
{
    CHILD_CHECK(crash::Start());
    CrashTestThrowBadWhat(nullptr);
    Say("survived");
    return 0;
}

int ChildTerminateWithoutException()
{
    CHILD_CHECK(crash::Start());
    std::terminate();
}

int ChildTerminateNotStarted()
{
    CrashTestThrowUncaught(nullptr);
    Say("survived");
    return 0;
}

void* ThrowUncaughtOnAThread(void*)
{
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    Say("thread-id %llu", (unsigned long long)tid);
    CrashTestThrowUncaught(nullptr);
    return nullptr;
}

int ChildTerminateOnAThread()
{
    CHILD_CHECK(crash::Start());
    pthread_t thread;
    CHILD_CHECK(pthread_create(&thread, nullptr, ThrowUncaughtOnAThread, nullptr) == 0);
    pthread_join(thread, nullptr);
    Say("survived");
    return 0;
}

// The handler that was there before the reporter's: what ends the process.
void PreviousTerminate()
{
    Say("previous terminate handler: report %s", ReportState());
    abort();
}

int ChildTerminatePrevious()
{
    std::set_terminate(PreviousTerminate);
    CHILD_CHECK(crash::Start());
    CrashTestThrowUncaught(nullptr);
    Say("survived");
    return 0;
}

// A handler installed after the reporter's that calls the one it replaced,
// and the reporter put in front of it again: each calls the other.
std::terminate_handler gReplacedTerminate = nullptr;

void GameTerminate()
{
    Say("game terminate handler");
    if (gReplacedTerminate) gReplacedTerminate();
    abort();
}

int ChildTerminateHandlersCallEachOther()
{
    CHILD_CHECK(crash::Start());
    gReplacedTerminate = std::set_terminate(GameTerminate);
    CHILD_CHECK(crash::Start());
    CrashTestThrowUncaught(nullptr);
    Say("survived");
    return 0;
}

// --- Other threads ---------------------------------------------------------------------
//
// Named threads, each stopped in a function of its own that it says it has
// reached, for the report's other threads (MTHR) and the backtrace text.

std::atomic<int> gThreadsReady{ 0 };
uint64_t volatile gSpins[5]{};
int gParkedPipe[2] = { -1, -1 };
// A frame record that points at itself.
alignas(16) uint64_t gLoopingRecord[2];

extern "C" __attribute__((noinline)) void CrashTestParked(int fd)
{
    gThreadsReady++;
    char c;
    while (read(fd, &c, 1) < 0 && errno == EINTR) {}
    __asm__ volatile("" ::: "memory");
}

extern "C" __attribute__((noinline)) void CrashTestSpinning(uint64_t volatile* counter)
{
    gThreadsReady++;
    for (;;) *counter = *counter + 1;
}

extern "C" __attribute__((noinline)) void CrashTestOnQueue(void*)
{
    gThreadsReady++;
    for (;;) {
        gSpins[4] = gSpins[4] + 1;
        usleep(1000);
    }
}

void* ParkedThread(void*)
{
    pthread_setname_np("bg3mse-test parked");
    CrashTestParked(gParkedPipe[0]);
    Say("parked thread woke");
    return nullptr;
}

void* SpinningThread(void*)
{
    pthread_setname_np("bg3mse-test spinning");
    CrashTestSpinning(&gSpins[0]);
    return nullptr;
}

// `which`: 1, an aligned frame pointer to unmapped memory; 2, a misaligned
// one; 3, a record that points at itself.
void* BadFramePointerThread(void* arg)
{
    auto which = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
    char const* names[] = { "", "bg3mse-test unmapped fp", "bg3mse-test misaligned fp", "bg3mse-test looping fp" };
    pthread_setname_np(names[which]);
    uint64_t local[2] = {};
    uint64_t fp = which == 1 ? 0x1000 : which == 2 ? reinterpret_cast<uint64_t>(local) + 4 : reinterpret_cast<uint64_t>(gLoopingRecord);
    gThreadsReady++;
    crash_test_spin_with_fp(fp, &gSpins[which]);
}

// 40 calls deep, more than a thread's frames are kept of. It stays there.
volatile bool gDeepMayReturn = false;
extern "C" __attribute__((noinline)) void CrashTestDeep(int depth)
{
    if (depth > 0) {
        CrashTestDeep(depth - 1);
    } else {
        gThreadsReady++;
        while (!gDeepMayReturn) pause();
    }
    __asm__ volatile("" ::: "memory");
}

void* DeepThread(void*)
{
    pthread_setname_np("bg3mse-test deep");
    CrashTestDeep(40);
    return nullptr;
}

void* MallocThread(void*)
{
    pthread_setname_np("bg3mse-test in malloc");
    gParkInMalloc = pthread_self();
    void* volatile p = malloc(32);
    free(p);
    return nullptr;
}

pthread_t gOtherThreads[7];

void StartOtherThreads()
{
    gLoopingRecord[0] = reinterpret_cast<uint64_t>(gLoopingRecord);
    gLoopingRecord[1] = reinterpret_cast<uint64_t>(&crash_test_spin_with_fp);
    CHILD_CHECK(pipe(gParkedPipe) == 0);
    CHILD_CHECK(pthread_create(&gOtherThreads[0], nullptr, ParkedThread, nullptr) == 0);
    CHILD_CHECK(pthread_create(&gOtherThreads[1], nullptr, SpinningThread, nullptr) == 0);
    for (uintptr_t which = 1; which <= 3; which++) {
        CHILD_CHECK(pthread_create(&gOtherThreads[1 + which], nullptr, BadFramePointerThread, reinterpret_cast<void*>(which)) == 0);
    }
    CHILD_CHECK(pthread_create(&gOtherThreads[5], nullptr, MallocThread, nullptr) == 0);
    CHILD_CHECK(pthread_create(&gOtherThreads[6], nullptr, DeepThread, nullptr) == 0);
    dispatch_queue_t queue = dispatch_queue_create("org.bg3mse.test-queue", DISPATCH_QUEUE_SERIAL);
    dispatch_async_f(queue, nullptr, CrashTestOnQueue);
    // Each has reached its function: seven threads and the queue's.
    // The spinning ones have gone round their loops.
    auto spinning = [] { return gSpins[0] && gSpins[1] && gSpins[2] && gSpins[3] && gSpins[4]; };
    for (int i = 0; i < 5000 && (gThreadsReady < 7 || !gParkedInMalloc || !spinning()); i++) usleep(1000);
    CHILD_CHECK(gThreadsReady == 7 && gParkedInMalloc && spinning());
    usleep(20000);
    uint64_t tid = 0;
    pthread_threadid_np(nullptr, &tid);
    Say("thread-id %llu", (unsigned long long)tid);
}

int ChildThreadsUnguarded()
{
    gReporterMustNotAllocate = true;
    CHILD_CHECK(crash::Start());
    StartOtherThreads();
    crash_test_read_null();
    Say("survived");
    return 0;
}

int ChildThreadsGuarded()
{
    gReporterMustNotAllocate = true;
    CHILD_CHECK(crash::Start());
    StartOtherThreads();
    CrashTestGuarded(ReadNull, nullptr);
    Say("survived");
    return 0;
}

int ChildThreadsTerminate()
{
    CHILD_CHECK(crash::Start());
    StartOtherThreads();
    CrashTestThrowUncaught(nullptr);
    Say("survived");
    return 0;
}

// A fault reported from a guarded block and then recovered by a guard around
// it: every thread the reporter suspended runs again.
int ChildThreadsRecovered()
{
    gReporterMustNotAllocate = true;
    CHILD_CHECK(crash::Start());
    StartOtherThreads();
    bool returned = GuardedCall([] { CrashTestGuarded(ReadNull, nullptr); });
    CHILD_CHECK(!returned);
    gReporterMustNotAllocate = false;
    gReporterThread = nullptr;
    Say("recovered");

    uint64_t before[5];
    for (int i = 0; i < 5; i++) before[i] = gSpins[i];
    usleep(100000);
    int running = 0;
    for (int i = 0; i < 5; i++) running += gSpins[i] != before[i];
    Say("spinning threads running %d", running);
    int suspended = 0;
    for (int i = 1; i < 5; i++) {
        thread_basic_info_data_t info{};
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        if (thread_info(pthread_mach_thread_np(gOtherThreads[i]), THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) != KERN_SUCCESS
            || info.suspend_count != 0) {
            suspended++;
        }
    }
    Say("threads still suspended %d", suspended);
    CHILD_CHECK(write(gParkedPipe[1], "x", 1) == 1);
    pthread_join(gOtherThreads[0], nullptr);
    Say("done");
    // The threads that never return stay where they are.
    _exit(gChildFailures);
}

struct ChildCase
{
    char const* Name;
    int (*Run)();
};

ChildCase const kChildCases[] = {
    { "block-runs", ChildBlockRuns },
    { "guarded-fault", ChildGuardedFault },
    { "guarded-fault-host-allocates", ChildGuardedFaultHostAllocates },
    { "guarded-fault-not-started", ChildGuardedFaultNotStarted },
    { "guarded-fault-stopped", ChildGuardedFaultStopped },
    { "guarded-fault-disabled", ChildGuardedFaultDisabled },
    { "guarded-fault-disabled-forced", ChildGuardedFaultDisabledForced },
    { "nested-blocks-fault", ChildNestedBlocksFault },
    { "guarded-overflow", ChildGuardedOverflow },
    { "guarded-overflow-forced", ChildGuardedOverflowForced },
    { "unguarded-fault", ChildUnguardedFault },
    { "unguarded-fault-not-started", ChildUnguardedFaultNotStarted },
    { "unguarded-fault-disabled", ChildUnguardedFaultDisabled },
    { "unguarded-fault-stopped", ChildUnguardedFaultStopped },
    { "started-again-and-again", ChildStartedAgainAndAgain },
    { "abort", ChildAbort },
    { "foreign-only", ChildForeignOnly },
    { "foreign-only-forced", ChildForeignOnlyForced },
    { "foreign-through-hook", ChildForeignThroughHook },
    { "foreign-through-registered-hook", ChildForeignThroughRegisteredHook },
    { "foreign-through-excluded-hook", ChildForeignThroughExcludedHook },
    { "fault-in-registered-function", ChildFaultInRegisteredFunction },
    { "rule", ChildRule },
    { "exception-leaves-block", ChildExceptionLeavesBlock },
    { "exception-leaves-block-not-started", ChildExceptionLeavesBlockNotStarted },
    { "exception-leaves-block-disabled", ChildExceptionLeavesBlockDisabled },
    { "exception-leaves-block-disabled-forced", ChildExceptionLeavesBlockDisabledForced },
    { "exception-leaves-block-native-api", ChildExceptionLeavesBlockNativeApi },
    { "exception-leaves-block-native-api-uncaught", ChildExceptionLeavesBlockNativeApiUncaught },
    { "exception-caught-in-block", ChildExceptionCaughtInBlock },
    { "host-faults", ChildHostFaults },
    { "handlers-unguarded", ChildHandlersUnguarded },
    { "handlers-guarded", ChildHandlersGuarded },
    { "handlers-exception", ChildHandlersException },
    { "fault-after-a-reported-one", ChildFaultAfterAReportedOne },
    { "game-handler-in-front", ChildGameHandlerInFront },
    { "game-handler-unguarded", ChildGameHandlerUnguarded },
    { "game-handler-guarded", ChildGameHandlerGuarded },
    { "game-handler-abort", ChildGameHandlerAbort },
    { "fault-while-a-report-is-written", ChildFaultWhileAReportIsWritten },
    { "fault-while-a-report-is-written-in-the-game", ChildFaultWhileAReportIsWrittenInTheGame },
    { "exception-while-a-report-is-written", ChildExceptionWhileAReportIsWritten },
    { "fault-while-an-exceptions-report-is-written", ChildFaultWhileAnExceptionsReportIsWritten },
    { "fault-in-a-handler-the-game-calls", ChildFaultInAHandlerTheGameCalls },
    { "fault-in-a-handler-the-game-calls-behind-the-games-handler", ChildFaultInAHandlerTheGameCallsBehindTheGamesHandler },
    { "foreign-only-logged", ChildForeignOnlyLogged },
    { "guarded-fault-vectors", ChildGuardedFaultVectors },
    { "terminate-uncaught", ChildTerminateUncaught },
    { "terminate-disabled", ChildTerminateDisabled },
    { "terminate-int", ChildTerminateInt },
    { "terminate-bad-what", ChildTerminateBadWhat },
    { "terminate-without-exception", ChildTerminateWithoutException },
    { "terminate-not-started", ChildTerminateNotStarted },
    { "terminate-on-a-thread", ChildTerminateOnAThread },
    { "terminate-previous", ChildTerminatePrevious },
    { "terminate-handlers-call-each-other", ChildTerminateHandlersCallEachOther },
    { "threads-unguarded", ChildThreadsUnguarded },
    { "threads-guarded", ChildThreadsGuarded },
    { "threads-terminate", ChildThreadsTerminate },
    { "threads-recovered", ChildThreadsRecovered },
};

// --- The tests -----------------------------------------------------------------------

// This executable's LC_UUID, as the report writes one.
std::string OwnUuid()
{
    auto mh = reinterpret_cast<mach_header_64 const*>(_dyld_get_image_header(0));
    auto p = reinterpret_cast<uint8_t const*>(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        auto lc = reinterpret_cast<load_command const*>(p);
        if (lc->cmd == LC_UUID) {
            auto u = reinterpret_cast<uuid_command const*>(lc)->uuid;
            char text[40];
            snprintf(text, sizeof(text), "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
            return text;
        }
        p += lc->cmdsize;
    }
    return {};
}

// The fault of crash_test_read_null, as a report must describe it.
// The stack file beside a report: what the report says of it, its header,
// and the memory in it, which holds the frame record the backtrace's third
// frame was read from.
void CheckStackFile(Crash const& crash)
{
    auto const& report = crash.Written;
    std::string stem = crash.ReportName.substr(0, crash.ReportName.size() - strlen(crash::kReportSuffix));
    CHECK(report.Value(crash::tag::MacFault, "stack_file") == stem + crash::kStackSuffix);
    CHECK(crash.HasStackFile && crash.StackFile.size() > sizeof(crash::StackFileHeader));
    if (!crash.HasStackFile || crash.StackFile.size() <= sizeof(crash::StackFileHeader)) return;

    crash::StackFileHeader header;
    memcpy(&header, crash.StackFile.data(), sizeof(header));
    CHECK(crash.StackFile.compare(0, 4, "KTSM") == 0);
    CHECK(header.Signature == crash::kStackSignature && header.Version == crash::kStackVersion);
    CHECK(header.HeaderSize == sizeof(header) && crash.StackFile.size() == header.HeaderSize + header.Length);
    CHECK(std::to_string(header.ThreadId) == report.Value(crash::tag::MacFault, "thread_id"));
    char port[16];
    snprintf(port, sizeof(port), "0x%x", header.ThreadPort);
    CHECK(port == report.Value(crash::tag::MacFault, "thread_port"));
    uint64_t sp = Hex(report.Value(crash::tag::MacRegisters, "sp"));
    CHECK(header.StackPointer == sp);
    CHECK(header.Base == Hex(report.Value(crash::tag::MacFault, "stack_base")));
    CHECK(header.Length == Hex(report.Value(crash::tag::MacFault, "stack_length")));
    CHECK(header.StackLow == Hex(report.Value(crash::tag::MacFault, "stack_low")));
    CHECK(header.StackHigh == Hex(report.Value(crash::tag::MacFault, "stack_high")));

    // From the red zone below the stack pointer to the top of the stack, or
    // as much of it as the limit allows; every page of it could be read.
    CHECK(header.StackLow < header.StackHigh && sp >= header.StackLow && sp < header.StackHigh);
    CHECK(header.Base == ((sp - crash::kStackRedZone) & ~uint64_t{ 15 }));
    uint64_t end = std::min(header.StackHigh, header.Base + crash::kStackLimit);
    CHECK(header.Base + header.Length == end);
    bool cut = header.StackHigh - header.Base > crash::kStackLimit;
    CHECK(header.Flags == (crash::StackBoundsKnown | (cut ? crash::StackCutAtLimit : 0)));
    CHECK(report.Value(crash::tag::MacFault, "stack_flags") == (cut ? "BC--" : "B---"));

    // The faulting function saved nothing: fp is its caller's frame record,
    // whose return address is the backtrace's third frame.
    uint64_t fp = Hex(report.Value(crash::tag::MacRegisters, "fp"));
    CHECK(fp >= header.Base && fp + 16 <= header.Base + header.Length);
    if (fp < header.Base || fp + 16 > header.Base + header.Length) return;
    uint64_t record[2];
    memcpy(record, crash.StackFile.data() + header.HeaderSize + (fp - header.Base), sizeof(record));
    CHECK(record[1] == Hex(Field(report.Line(crash::tag::MacBacktrace, "frame=2 "), "pc")));
    CHECK(record[0] > fp && record[0] < header.StackHigh);
}

void CheckNullReadReport(Crash const& crash, char const* origin)
{
    auto const& report = crash.Written;
    uint64_t faultPC = crash.Child.Number("fault-function") + 12;
    uint64_t imageBase = crash.Child.Number("image-base");
    CHECK(faultPC != 12 && imageBase != 0);

    CHECK(report.Value(crash::tag::MacFault, "cause") == "fault");
    CHECK(report.Value(crash::tag::MacFault, "kind") == "bad access");
    CHECK(report.Value(crash::tag::MacFault, "origin") == origin);
    CHECK(report.Value(crash::tag::MacFault, "signal") == std::to_string(SIGSEGV));
    CHECK(Hex(report.Value(crash::tag::MacFault, "address")) == 0x10);
    CHECK(Hex(report.Value(crash::tag::MacFault, "pc")) == faultPC);
    CHECK(report.Value(crash::tag::MacFault, "thread_id") == std::to_string(crash.Child.Number("thread-id")));

    // The registers at the faulting instruction.
    CHECK(Hex(report.Value(crash::tag::MacRegisters, "pc")) == faultPC);
    CHECK(Hex(report.Value(crash::tag::MacRegisters, "x8")) == 0x10);
    CHECK(Hex(report.Value(crash::tag::MacRegisters, "x9")) == 0xdeadbeef);
    CHECK(Hex(report.Value(crash::tag::MacRegisters, "sp")) != 0 && Hex(report.Value(crash::tag::MacRegisters, "fp")) != 0);
    CHECK(Hex(report.Value(crash::tag::MacRegisters, "lr")) != 0);
    for (int i = 0; i < 29; i++) CHECK(!report.Value(crash::tag::MacRegisters, "x" + std::to_string(i)).empty());

    // The faulting frame, its image and its symbol.
    std::string frame = report.Line(crash::tag::MacBacktrace, "frame=0 ");
    CHECK(Hex(Field(frame, "pc")) == faultPC);
    CHECK(Hex(Field(frame, "image")) == imageBase);
    CHECK(Field(frame, "flags") == "-ME");
    CHECK(Field(frame, "symbol") == "_crash_test_read_null+0xc");
    // Its caller, from the link register: the function had no frame of its own.
    std::string caller = report.Line(crash::tag::MacBacktrace, "frame=1 ");
    CHECK(Field(caller, "flags") == "LME" && Hex(Field(caller, "image")) == imageBase);
    CHECK(!report.Line(crash::tag::MacBacktrace, "frame=2 ").empty());

    // The image the frame is in, with what symbolising it later needs.
    std::string image = report.Line(crash::tag::MacImages, "flags=ME ");
    CHECK(Hex(Field(image, "base")) == imageBase);
    CHECK(Hex(Field(image, "slide")) == crash.Child.Number("image-slide"));
    CHECK(Hex(Field(image, "size")) != 0 && faultPC < imageBase + Hex(Field(image, "size")));
    CHECK(!OwnUuid().empty() && Field(image, "uuid") == OwnUuid());
    CHECK(image.find(" path=" + gSelf) != std::string::npos);
    // And the others: the system's libraries, the foreign dylib, dyld.
    CHECK(!report.Line(crash::tag::MacImages, "libsystem_kernel.dylib").empty());
    CHECK(!report.Line(crash::tag::MacImages, "libbg3mse_crash_test_foreign.dylib").empty());
    CHECK(!report.Line(crash::tag::MacImages, "/usr/lib/dyld").empty());
    std::string foreign = report.Line(crash::tag::MacImages, "libbg3mse_crash_test_foreign.dylib");
    CHECK(Field(foreign, "flags") == "--" && Field(foreign, "uuid").size() == 36);

    CHECK(!report.Value(crash::tag::MacSystem, "os_version").empty());
    CHECK(!report.Value(crash::tag::MacSystem, "os_build").empty());
    CHECK(report.Value(crash::tag::MacSystem, "executable") == gSelf);

    // The backtrace as text, Norbyte's way: first line the faulting function.
    CHECK(report.Body(crash::tag::StackTrace).rfind("_crash_test_read_null+0xc\n", 0) == 0);

    CheckStackFile(crash);
}

TEST(a_guarded_block_runs_its_body_once_and_goes_on)
{
    Crash crash("block-runs");
    if (!crash.Child.Exited(0)) crash.Show("block-runs");
    CHECK(crash.Child.Exited(0));
    CHECK(crash.LeftNothing());
}

TEST(a_fault_in_a_guarded_block_is_reported_and_then_takes_its_course)
{
    Crash crash("guarded-fault");
    Crash control("unguarded-fault-not-started");
    if (!crash.Written.WellFormed) crash.Show("guarded-fault");
    // Nothing is recovered: the process ends as it would without bg3mse.
    CHECK(control.Child.KilledBy(SIGSEGV));
    CHECK(crash.Child.KilledBy(SIGSEGV) && crash.Child.Status == control.Child.Status);
    CHECK(!crash.Child.Said("survived"));
    CHECK(crash.Written.WellFormed);
    CheckNullReadReport(crash, "guarded");
    CHECK(crash.Child.Count("host: build sections") == 1);
    // Norbyte's last step, once, with both files there.
    CHECK(crash.Child.Count("launch crash reporter: the report's path, report written, text written") == 1);
    CHECK(!crash.Child.Said("allocated while reporting"));
}

TEST(an_allocation_on_the_reporters_thread_would_be_noticed)
{
    Crash crash("guarded-fault-host-allocates");
    CHECK(crash.Child.KilledBy(SIGSEGV));
    CHECK(crash.Child.Said("allocated while reporting"));
}

TEST(the_report_has_norbytes_sections_in_his_order_and_the_mac_ones)
{
    Crash crash("guarded-fault");
    auto const& report = crash.Written;
    CHECK(report.WellFormed);
    using namespace crash::tag;
    std::vector<uint32_t> expected = { Header, ExtenderVersion, GameVersion, BuildDate, ReleaseChannel, BuildType, StackTrace,
        LuaStackTrace, LoadedModules, ErrorLog, ClientState, SessionLength, MacFault, MacRegisters, MacBacktrace, MacImages, MacSystem,
        MacBuild, MacMemory, MacThreads };
    std::vector<uint32_t> found;
    for (auto const& section : report.Sections) found.push_back(section.Tag);
    CHECK(found == expected);

    uint32_t version = 0;
    CHECK(report.Body(Header).size() == 4);
    memcpy(&version, report.Body(Header).data(), std::min<size_t>(4, report.Body(Header).size()));
    CHECK(version == 1);
    CHECK(report.Body(ExtenderVersion) == "34.0.0.0");
    CHECK(report.Body(GameVersion) == "4.1.1.7");
    CHECK(report.Body(LuaStackTrace) == "Mods/Example/Server.lua:12 OnTick\n[C++ code] xpcall\n");
    CHECK(report.Body(LoadedModules) == "28ac9ce2-2aba-8cda-b3b5-6e922f71b6b8\tGustav\t4.0.0.1\r\n");
    CHECK(report.Body(ErrorLog) == "a warning\r\nan error\r\n");
    CHECK(report.Body(SessionLength).size() == 8);

    // The text shown to the user: the backtrace, then the Lua backtrace.
    CHECK(crash.HasBacktraceText);
    // Norbyte's text, then the other threads.
    std::string norbytes = report.Body(StackTrace) + "\r\n\r\nLua Backtrace:\r\n" + report.Body(LuaStackTrace);
    CHECK(crash.BacktraceText.starts_with(norbytes + "\r\n\r\nThreads ("));
    // At most 13 frames, each a line, and the mark that there were more.
    std::string trace = report.Body(StackTrace);
    size_t lines = (size_t)std::count(trace.begin(), trace.end(), '\n');
    CHECK(lines >= 3 && lines <= 14);
    if (lines == 14) CHECK(trace.size() > 6 && trace.substr(trace.size() - 6) == "(...)\n");

    // Only the three files, under their final names: crash_<time>, the time
    // being now by this machine's clock (the child ran a moment ago).
    CHECK(crash.Names.size() == 3 && crash.ReportName.size() == strlen("crash_2026-11-10_21-30-05.crpt"));
    if (crash.Names.size() == 3 && crash.ReportName.size() == 30) {
        std::string stem = crash.ReportName.substr(0, 25);
        CHECK(crash.Names[0] == stem + ".bt" && crash.Names[1] == stem + ".crpt" && crash.Names[2] == stem + ".stack");
        tm local{};
        CHECK(strptime(stem.c_str() + 6, "%Y-%m-%d_%H-%M-%S", &local) != nullptr);
        local.tm_isdst = -1;
        double age = difftime(time(nullptr), mktime(&local));
        CHECK(age >= 0 && age < 120);
    }
    struct stat st{};
    CHECK(stat((crash.Dir + "/" + crash.ReportName).c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    std::string stack = crash.ReportName.substr(0, crash.ReportName.size() - strlen(crash::kReportSuffix)) + crash::kStackSuffix;
    CHECK(stat((crash.Dir + "/" + stack).c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
}

// The stack file of a made-up fault, over pages of this process laid out as
// a stack can be: unreadable pages around readable ones.
struct StackFileResult
{
    bool Read{ false };
    crash::StackFileHeader Header{};
    std::string Bytes;          // the whole file
};

StackFileResult WriteStackOf(FaultRecord const& fault)
{
    StackFileResult result;
    std::string path = (std::filesystem::temp_directory_path() / "bg3mse-crash-stack.XXXXXX").string();
    int fd = mkstemp(path.data());
    if (fd < 0) return result;
    close(fd);
    crash::ReportWriter file;
    if (file.Open(path.c_str())) {
        result.Read = crash::WriteStackFile(file, fault, result.Header);
        CHECK(file.Close());
    }
    ReadFile(path, result.Bytes);
    unlink(path.c_str());
    return result;
}

TEST(the_stack_file_holds_the_stack_from_below_the_stack_pointer_to_its_top)
{
    size_t page = getpagesize();
    constexpr size_t kPages = 6;
    auto map = static_cast<uint8_t*>(mmap(nullptr, kPages * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    CHECK(map != MAP_FAILED);
    if (map == MAP_FAILED) return;
    for (size_t i = 0; i < kPages * page; i++) map[i] = static_cast<uint8_t>(i * 7 + i / page);
    // Page 0 is a guard, 1 to 3 the stack, 4 cannot be read either, 5 can.
    CHECK(mprotect(map, page, PROT_NONE) == 0);
    CHECK(mprotect(map + 4 * page, page, PROT_NONE) == 0);
    auto at = [&](size_t offset) { return reinterpret_cast<uint64_t>(map) + offset; };

    auto fault = std::make_unique<FaultRecord>();
    fault->ThreadId = 1234;
    fault->Thread = 0x1503;
    fault->StackLow = at(page);
    fault->StackHigh = at(4 * page);
    auto contents = [&](StackFileResult const& r) {
        return r.Bytes.size() == sizeof(crash::StackFileHeader) + r.Header.Length
            && memcmp(r.Bytes.data() + sizeof(crash::StackFileHeader), reinterpret_cast<void const*>(r.Header.Base), r.Header.Length) == 0;
    };

    // The stack pointer in the stack: from 128 bytes below it to the top.
    arm_thread_state64_set_sp(fault->Registers, at(2 * page + 0x238));
    auto inside = WriteStackOf(*fault);
    CHECK(inside.Read && contents(inside));
    CHECK(inside.Header.Signature == crash::kStackSignature && inside.Header.Version == crash::kStackVersion);
    CHECK(inside.Header.HeaderSize == sizeof(crash::StackFileHeader));
    CHECK(inside.Header.ThreadId == 1234 && inside.Header.ThreadPort == 0x1503);
    CHECK(inside.Header.StackPointer == at(2 * page + 0x238));
    CHECK(inside.Header.StackLow == at(page) && inside.Header.StackHigh == at(4 * page));
    CHECK(inside.Header.Base == at(2 * page + 0x1b0) && inside.Header.Length == 2 * page - 0x1b0);
    CHECK(inside.Header.Flags == crash::StackBoundsKnown);
    // An address on the stack is found at its offset from the base.
    uint64_t local = at(3 * page + 0x40);
    CHECK(static_cast<uint8_t>(inside.Bytes[sizeof(crash::StackFileHeader) + (local - inside.Header.Base)]) == map[3 * page + 0x40]);

    // A stack that overflowed: the stack pointer in the guard. The guard is
    // left out; the memory begins with the stack.
    arm_thread_state64_set_sp(fault->Registers, at(page - 0x20));
    auto overflowed = WriteStackOf(*fault);
    CHECK(overflowed.Read && contents(overflowed));
    CHECK(overflowed.Header.Base == at(page) && overflowed.Header.Length == 3 * page);
    CHECK(overflowed.Header.Flags == (crash::StackBoundsKnown | crash::StackStartsLater));

    // Bounds that are not the stack pointer's are not used: from below the
    // stack pointer to the first page that cannot be read.
    fault->StackLow = at(6 * page) + (64 << 20);
    fault->StackHigh = fault->StackLow + page;
    arm_thread_state64_set_sp(fault->Registers, at(2 * page + 0x238));
    auto elsewhere = WriteStackOf(*fault);
    CHECK(elsewhere.Read && contents(elsewhere));
    CHECK(elsewhere.Header.Base == at(2 * page + 0x1b0) && elsewhere.Header.Length == 2 * page - 0x1b0);
    CHECK(elsewhere.Header.Flags == crash::StackEndsUnreadable);
    fault->StackLow = fault->StackHigh = 0;
    auto unknown = WriteStackOf(*fault);
    CHECK(unknown.Read && unknown.Header.Flags == crash::StackEndsUnreadable && unknown.Header.Length == 2 * page - 0x1b0);

    // Nothing readable: a header and nothing after it.
    arm_thread_state64_set_sp(fault->Registers, at(4 * page + 0x400));
    fault->StackLow = at(4 * page);
    fault->StackHigh = at(5 * page);
    auto none = WriteStackOf(*fault);
    CHECK(!none.Read && none.Header.Length == 0 && none.Bytes.size() == sizeof(crash::StackFileHeader));
    munmap(map, kPages * page);

    // A stack larger than the limit: cut there.
    size_t big = crash::kStackLimit + 4 * page;
    auto large = static_cast<uint8_t*>(mmap(nullptr, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    CHECK(large != MAP_FAILED);
    if (large == MAP_FAILED) return;
    fault->StackLow = reinterpret_cast<uint64_t>(large);
    fault->StackHigh = fault->StackLow + big;
    arm_thread_state64_set_sp(fault->Registers, fault->StackLow + page);
    auto cut = WriteStackOf(*fault);
    CHECK(cut.Read && cut.Header.Length == crash::kStackLimit && cut.Header.Base == fault->StackLow + page - crash::kStackRedZone);
    CHECK(cut.Header.Flags == (crash::StackBoundsKnown | crash::StackCutAtLimit));
    CHECK(cut.Bytes.size() == sizeof(crash::StackFileHeader) + crash::kStackLimit);
    munmap(large, big);
}

TEST(report_names_carry_the_time_of_the_crash)
{
    char text[crash::kReportTimeLength + 1];
    crash::FormatReportTime(0, 0, text);
    CHECK(std::string(text) == "1970-01-01_00-00-00");
    // 2026-11-10 21:30:05 UTC.
    crash::FormatReportTime(1794346205, 0, text);
    CHECK(std::string(text) == "2026-11-10_21-30-05");
    // The same moment five hours west, and across midnight to the east.
    crash::FormatReportTime(1794346205, -5 * 3600, text);
    CHECK(std::string(text) == "2026-11-10_16-30-05");
    crash::FormatReportTime(1794346205, 3 * 3600, text);
    CHECK(std::string(text) == "2026-11-11_00-30-05");
    // A leap day, the last second of a year, and a time before 1970.
    crash::FormatReportTime(1709251199, 0, text);
    CHECK(std::string(text) == "2024-02-29_23-59-59");
    crash::FormatReportTime(1767225599, 0, text);
    CHECK(std::string(text) == "2025-12-31_23-59-59");
    crash::FormatReportTime(-1, 0, text);
    CHECK(std::string(text) == "1969-12-31_23-59-59");
    // What the C library says for now, here.
    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    char expected[32];
    strftime(expected, sizeof(expected), "%Y-%m-%d_%H-%M-%S", &local);
    crash::FormatReportTime(now, local.tm_gmtoff, text);
    CHECK(std::string(text) == expected);
}

// Twelve reports of earlier crashes, one of them without its text, and files
// that are nobody's reports.
void PutEarlierReports(std::string const& dir)
{
    for (int day = 1; day <= 12; day++) {
        char stem[64];
        snprintf(stem, sizeof(stem), "/crash_2020-01-%02d_10-00-00", day);
        std::ofstream(dir + stem + ".crpt") << "report";
        if (day != 5) std::ofstream(dir + stem + ".bt") << "text";
        if (day % 2 == 0) std::ofstream(dir + stem + ".stack") << "stack";
    }
    std::ofstream(dir + "/notes.txt") << "not a report";
    std::ofstream(dir + "/crash_latest.crpt") << "not named for a time";
}

TEST(the_directory_keeps_the_newest_ten_reports)
{
    // On their own: the oldest go, each with all its files.
    std::string pattern = (std::filesystem::temp_directory_path() / "bg3mse-crash-prune.XXXXXX").string();
    CHECK(mkdtemp(pattern.data()) != nullptr);
    PutEarlierReports(pattern);
    crash::PruneReports(pattern.c_str(), 3);
    std::vector<std::string> names;
    for (auto const& entry : std::filesystem::directory_iterator(pattern)) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    CHECK((names == std::vector<std::string>{ "crash_2020-01-10_10-00-00.bt", "crash_2020-01-10_10-00-00.crpt",
        "crash_2020-01-10_10-00-00.stack", "crash_2020-01-11_10-00-00.bt", "crash_2020-01-11_10-00-00.crpt",
        "crash_2020-01-12_10-00-00.bt", "crash_2020-01-12_10-00-00.crpt", "crash_2020-01-12_10-00-00.stack",
        "crash_latest.crpt", "notes.txt" }));
    // Nothing to do, and a directory that is not there.
    crash::PruneReports(pattern.c_str(), 3);
    crash::PruneReports((pattern + "/none").c_str(), 3);
    std::error_code ignored;
    CHECK(std::filesystem::remove_all(pattern, ignored) == 11);

    // In a process that crashes: nine are left when the reporter is prepared,
    // and its own report makes ten.
    Crash crash("guarded-fault", gSelf, PutEarlierReports);
    CHECK(crash.Written.WellFormed);
    unsigned reports = 0, earlier = 0;
    for (auto const& name : crash.Names) {
        if (!name.ends_with(".crpt") || name == "crash_latest.crpt") continue;
        reports++;
        if (name.starts_with("crash_2020-01-")) earlier++;
    }
    CHECK(reports == crash::kReportsKept && earlier == crash::kReportsKept - 1);
    CHECK(std::find(crash.Names.begin(), crash.Names.end(), "crash_2020-01-03_10-00-00.crpt") == crash.Names.end());
    CHECK(std::find(crash.Names.begin(), crash.Names.end(), "crash_2020-01-03_10-00-00.bt") == crash.Names.end());
    CHECK(std::find(crash.Names.begin(), crash.Names.end(), "crash_2020-01-02_10-00-00.stack") == crash.Names.end());
    CHECK(std::find(crash.Names.begin(), crash.Names.end(), "crash_2020-01-04_10-00-00.stack") != crash.Names.end());
    CHECK(std::find(crash.Names.begin(), crash.Names.end(), "crash_2020-01-04_10-00-00.crpt") != crash.Names.end());
    CHECK(std::find(crash.Names.begin(), crash.Names.end(), "notes.txt") != crash.Names.end());
}

TEST(a_fault_is_not_reported_by_a_reporter_that_was_never_started)
{
    Crash guarded("guarded-fault-not-started");
    CHECK(guarded.Child.KilledBy(SIGSEGV) && !guarded.Child.Said("survived"));
    CHECK(guarded.LeftNothing());
    Crash unguarded("unguarded-fault-not-started");
    CHECK(unguarded.Child.KilledBy(SIGSEGV));
    CHECK(unguarded.LeftNothing());
}

// Norbyte's start-up stops the reporter at its end and starts it again at the
// first change of the game's state; the extender's start-up goes on in
// between, from a hook on the game.
TEST(a_reporter_that_was_started_and_stopped_still_reports)
{
    Crash unguarded("unguarded-fault-stopped");
    if (!unguarded.Written.WellFormed) unguarded.Show("unguarded-fault-stopped");
    CHECK(unguarded.Child.KilledBy(SIGSEGV) && !unguarded.Child.Said("FAIL"));
    CHECK(unguarded.Written.WellFormed);
    CHECK(unguarded.Written.Value(crash::tag::MacFault, "origin") == "unguarded");
    CHECK(unguarded.Child.Count("launch crash reporter: the report's path, report written, text written") == 1);

    Crash guarded("guarded-fault-stopped");
    if (!guarded.Written.WellFormed) guarded.Show("guarded-fault-stopped");
    CHECK(guarded.Child.KilledBy(SIGSEGV) && !guarded.Child.Said("survived") && !guarded.Child.Said("FAIL"));
    CHECK(guarded.Written.WellFormed);
    CHECK(guarded.Written.Value(crash::tag::MacFault, "origin") == "guarded");
    CHECK(guarded.Child.Count("launch crash reporter: the report's path, report written, text written") == 1);
}

// Norbyte's reporter leaves a crash alone while crash reporting is disabled
// (it is, around every call of a hook into the game's own function). Here
// the crash is written down all the same, and the report says what his rule
// makes of it.
TEST(a_fault_while_crash_reporting_is_disabled_is_reported_as_not_the_extenders)
{
    Crash guarded("guarded-fault-disabled");
    CHECK(guarded.Child.KilledBy(SIGSEGV) && !guarded.Child.Said("survived"));
    CHECK(guarded.Written.WellFormed);
    CHECK(guarded.Written.Value(crash::tag::MacFault, "origin") == "guarded");
    CHECK(guarded.Written.Value(crash::tag::MacFault, "extender_related") == "no");
    CHECK(guarded.Written.Value(crash::tag::MacFault, "crash_reporting_disabled") == "1");
    Crash unguarded("unguarded-fault-disabled");
    CHECK(unguarded.Child.KilledBy(SIGSEGV));
    CHECK(unguarded.Written.WellFormed);
    CHECK(unguarded.Written.Value(crash::tag::MacFault, "origin") == "unguarded");
    CHECK(unguarded.Written.Value(crash::tag::MacFault, "extender_related") == "no");

    // The same faults with reporting not disabled are the extender's.
    Crash counted("guarded-fault");
    CHECK(counted.Written.Value(crash::tag::MacFault, "extender_related") == "yes");
    CHECK(counted.Written.Value(crash::tag::MacFault, "crash_reporting_disabled") == "0");
    CHECK(counted.Written.Value(crash::tag::MacFault, "norbytes_rule") == "reports");
}

TEST(forcing_reports_a_fault_while_crash_reporting_is_disabled)
{
    Crash crash("guarded-fault-disabled-forced");
    CHECK(crash.Child.KilledBy(SIGSEGV));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "origin") == "guarded");
    CHECK(crash.Written.Value(crash::tag::MacFault, "extender_related") == "yes");
}

TEST(a_fault_in_a_block_inside_a_block_is_reported_once)
{
    Crash crash("nested-blocks-fault");
    CHECK(crash.Child.KilledBy(SIGSEGV) && !crash.Child.Said("survived"));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "origin") == "guarded");
    CHECK(crash.Child.Count("host: build sections") == 1);
    CHECK(!crash.Child.Said("allocated while reporting"));
}

// "Osiris is riddled with various stack overflow issues, skip reporting those".
TEST(a_stack_overflow_is_the_extenders_only_when_forced)
{
    Crash crash("guarded-overflow");
    CHECK(crash.Child.Killed() && !crash.Child.Said("survived"));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "kind") == "stack overflow");
    CHECK(crash.Written.Value(crash::tag::MacFault, "extender_related") == "no");
    CHECK(crash.Written.Value(crash::tag::MacFault, "norbytes_rule") == "a stack overflow");
    Crash forced("guarded-overflow-forced");
    CHECK(forced.Child.Killed() && !forced.Child.Said("survived"));
    CHECK(forced.Written.WellFormed);
    CHECK(forced.Written.Value(crash::tag::MacFault, "extender_related") == "yes");
    CHECK(forced.Written.Value(crash::tag::MacFault, "kind") == "stack overflow");
    CHECK(Field(forced.Written.Line(crash::tag::MacBacktrace, "frame=0 "), "symbol").rfind("_crash_test_recurse+", 0) == 0);
}

TEST(a_fault_outside_any_block_reaches_the_reporter)
{
    Crash crash("unguarded-fault");
    Crash control("unguarded-fault-not-started");
    if (!crash.Written.WellFormed) crash.Show("unguarded-fault");
    CHECK(crash.Child.KilledBy(SIGSEGV) && crash.Child.Status == control.Child.Status);
    CHECK(!crash.Child.Said("survived"));
    CHECK(control.LeftNothing());
    CHECK(crash.Written.WellFormed);
    CheckNullReadReport(crash, "unguarded");
    CHECK(crash.Child.Count("host: build sections") == 1);
    CHECK(crash.Child.Count("launch crash reporter: the report's path, report written, text written") == 1);
    CHECK(!crash.Child.Said("allocated while reporting"));
    // Seen as a signal, the fault comes with the exception state.
    CHECK(Hex(crash.Written.Value(crash::tag::MacRegisters, "far")) == 0x10);
}

TEST(a_reporter_is_started_and_stopped_again_and_again)
{
    Crash again("started-again-and-again");
    if (!again.Written.WellFormed) again.Show("started-again-and-again");
    CHECK(again.Child.KilledBy(SIGSEGV) && !again.Child.Said("FAIL"));
    CHECK(again.Written.WellFormed);
}

TEST(abort_with_the_extenders_frames_on_the_stack_is_reported)
{
    Crash crash("abort");
    CHECK(crash.Child.KilledBy(SIGABRT));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "kind") == "abort");
    CHECK(crash.Written.Value(crash::tag::MacFault, "origin") == "unguarded");
    // abort() is the system's; the test's frames are below it.
    CHECK(Field(crash.Written.Line(crash::tag::MacBacktrace, "frame=0 "), "flags") == "---");
    CHECK(!crash.Written.Line(crash::tag::MacBacktrace, "flags=-ME ").empty());
    // The backtrace as text leaves out the system's lowest layer at its top,
    // where abort() stops the thread, as Norbyte's leaves out ntdll.dll and
    // kernelbase.dll: it starts at abort().
    CHECK(Field(crash.Written.Line(crash::tag::MacBacktrace, "symbol=_abort+"), "flags") == "---");
    CHECK(crash.Written.Body(crash::tag::StackTrace).rfind("libsystem_c.dylib!0x", 0) == 0);
    CHECK(crash.Written.Body(crash::tag::StackTrace).find("libsystem_kernel.dylib!0x") == std::string::npos);
}

TEST(a_crash_with_no_frame_of_the_extender_is_the_extenders_only_when_forced)
{
    Crash crash("foreign-only");
    CHECK(crash.Child.KilledBy(SIGSEGV) && !crash.Child.Said("survived"));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "extender_related") == "no");
    Crash forced("foreign-only-forced");
    CHECK(forced.Child.KilledBy(SIGSEGV));
    CHECK(forced.Written.WellFormed);
    CHECK(forced.Written.Value(crash::tag::MacFault, "extender_related") == "yes");
    CHECK(Field(forced.Written.Line(crash::tag::MacBacktrace, "frame=0 "), "flags") == "---");
    CHECK(forced.Written.Body(crash::tag::StackTrace).rfind("libbg3mse_crash_test_foreign.dylib!0x", 0) == 0);
}

TEST(a_crash_below_a_hook_is_the_extenders_unless_the_hook_is_registered_or_listed)
{
    Crash crash("foreign-through-hook");
    if (!crash.Written.WellFormed) crash.Show("foreign-through-hook");
    CHECK(crash.Child.KilledBy(SIGSEGV) && !crash.Child.Said("survived"));
    CHECK(crash.Written.WellFormed);
    CHECK(Field(crash.Written.Line(crash::tag::MacBacktrace, "symbol=_CrashTestHook+"), "flags") == "-ME");
    CHECK(crash.Written.Value(crash::tag::MacFault, "extender_related") == "yes");

    Crash registered("foreign-through-registered-hook");
    CHECK(registered.Child.KilledBy(SIGSEGV) && !registered.Child.Said("survived"));
    CHECK(registered.Written.WellFormed);
    CHECK(registered.Written.Value(crash::tag::MacFault, "extender_related") == "no");

    Crash listed("foreign-through-excluded-hook");
    CHECK(listed.Child.KilledBy(SIGSEGV) && !listed.Child.Said("survived"));
    CHECK(listed.Written.WellFormed);
    CHECK(listed.Written.Value(crash::tag::MacFault, "extender_related") == "no");
}

TEST(a_fault_inside_a_registered_function_is_reported)
{
    Crash crash("fault-in-registered-function");
    CHECK(crash.Child.KilledBy(SIGSEGV));
    CHECK(crash.Written.WellFormed);
    CHECK(Field(crash.Written.Line(crash::tag::MacBacktrace, "frame=0 "), "symbol") == "_crash_test_read_null+0xc");
}

TEST(the_rule_for_which_crashes_involve_the_extender)
{
    Crash crash("rule");
    if (!crash.Child.Exited(0)) crash.Show("rule");
    CHECK(crash.Child.Exited(0));
    CHECK(crash.LeftNothing());
}

TEST(a_cpp_exception_leaving_a_guarded_block_is_reported_with_the_thrower_on_the_stack)
{
    Crash crash("exception-leaves-block");
    if (!crash.Written.WellFormed) crash.Show("exception-leaves-block");
    // The process ends as an uncaught exception ends it; nothing was unwound.
    CHECK(crash.Child.KilledBy(SIGABRT));
    CHECK(!crash.Child.Said("caught outside: thrown in a guarded block") && !crash.Child.Said("unwound"));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "cause") == "c++ exception left a guarded block");
    CHECK(crash.Written.Value(crash::tag::MacFault, "kind") == "abort");
    CHECK(crash.Written.Value(crash::tag::MacFault, "origin") == "guarded");
    CHECK(crash.Written.Value(crash::tag::MacFault, "thread_id") == std::to_string(crash.Child.Number("thread-id")));
    CHECK(Field(crash.Written.Line(crash::tag::MacBacktrace, "symbol=_CrashTestThrow+"), "flags") == "-ME");
    CHECK(!crash.Written.Line(crash::tag::MacBacktrace, "symbol=___cxa_throw+").empty());
    CHECK(Hex(crash.Written.Value(crash::tag::MacRegisters, "sp")) != 0);
    // The exception, by its type and what().
    CHECK(crash.Written.Value(crash::tag::MacFault, "exception") == "std::runtime_error");
    CHECK(crash.Written.Value(crash::tag::MacFault, "exception_what") == "thrown in a guarded block");
    // Once: the abort that follows is the same crash.
    CHECK(crash.Child.Count("host: build sections") == 1);
    CHECK(crash.Child.Count("launch crash reporter: the report's path, report written, text written") == 1);
}

TEST(a_cpp_exception_leaving_a_guarded_block_is_reported_whether_or_not_reporting_is_started)
{
    Crash crash("exception-leaves-block-not-started");
    CHECK(crash.Child.KilledBy(SIGABRT));
    CHECK(!crash.Child.Said("caught outside: thrown in a guarded block") && !crash.Child.Said("unwound"));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "cause") == "c++ exception left a guarded block");
}

TEST(a_cpp_exception_passes_through_a_guarded_block_while_crash_reporting_is_disabled)
{
    Crash crash("exception-leaves-block-disabled");
    if (!crash.Child.Exited(0)) crash.Show("exception-leaves-block-disabled");
    CHECK(crash.Child.Exited(0));
    CHECK(crash.Child.Said("unwound") && crash.Child.Said("caught outside: thrown in a guarded block"));
    CHECK(crash.LeftNothing());

    Crash forced("exception-leaves-block-disabled-forced");
    CHECK(forced.Child.KilledBy(SIGABRT) && !forced.Child.Said("unwound"));
    CHECK(forced.Written.WellFormed);

    Crash native("exception-leaves-block-native-api");
    if (!native.Child.Exited(0)) native.Show("exception-leaves-block-native-api");
    CHECK(native.Child.Exited(0));
    CHECK(native.Child.Said("unwound") && native.Child.Said("caught outside: thrown in a guarded block"));
    CHECK(native.LeftNothing());

    Crash uncaught("exception-leaves-block-native-api-uncaught");
    if (!uncaught.Written.WellFormed) uncaught.Show("exception-leaves-block-native-api-uncaught");
    CHECK(uncaught.Child.KilledBy(SIGABRT) && !uncaught.Child.Said("survived") && !uncaught.Child.Said("FAIL"));
    CHECK(uncaught.Written.Value(crash::tag::MacFault, "cause") == "std::terminate");
    CHECK(uncaught.Written.Value(crash::tag::MacFault, "extender_related") == "no");
    CHECK(uncaught.Written.Value(crash::tag::MacFault, "norbytes_rule") == "a native module uses the extender's API");
    CHECK(uncaught.Child.Output.find("launch crash reporter") == std::string::npos);
}

// The report of a std::terminate: the thread that ended, the exception it
// ended with, the reporter's one report and the line of the log.
//
// `signal`: what ends the process after the report, the runtime's own
// terminate handler aborting, or what that handler runs into.
void CheckTerminateReport(Crash const& crash, char const* what, char const* exception, int signal = SIGABRT)
{
    if (!crash.Written.WellFormed) crash.Show(what);
    CHECK(crash.Child.KilledBy(signal) && !crash.Child.Said("survived") && !crash.Child.Said("FAIL"));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "cause") == "std::terminate");
    CHECK(crash.Written.Value(crash::tag::MacFault, "kind") == "abort");
    CHECK(crash.Written.Value(crash::tag::MacFault, "origin") == "unguarded");
    CHECK(crash.Written.Value(crash::tag::MacFault, "exception") == exception);
    CHECK(crash.Child.Count("host: build sections") == 1);
    // The notice only for a crash Norbyte's rule reports.
    bool reported = crash.Written.Value(crash::tag::MacFault, "extender_related") == "yes";
    CHECK(crash.Child.Count("launch crash reporter: the report's path, report written, text written") == (reported ? 1 : 0));
}

TEST(an_exception_nobody_catches_is_reported_by_the_terminate_handler_with_its_type_and_what)
{
    Crash crash("terminate-uncaught");
    CheckTerminateReport(crash, "terminate-uncaught", "std::runtime_error");
    CHECK(crash.Written.Value(crash::tag::MacFault, "exception_what") == "nobody catches this");
    CHECK(crash.Written.Value(crash::tag::MacFault, "thread_id") == std::to_string(crash.Child.Number("thread-id")));
    CHECK(crash.Written.Value(crash::tag::MacFault, "extender_related") == "yes");
    // The thrower is on the stack: nothing was unwound.
    // (The runtime's frames between are named by the nearest symbol it exports.)
    CHECK(Field(crash.Written.Line(crash::tag::MacBacktrace, "symbol=_CrashTestThrowUncaught+"), "flags") == "-ME");
    CHECK(!crash.Written.Line(crash::tag::MacBacktrace, "symbol=__ZSt11__terminatePFvvE+").empty());
    CHECK(crash.Child.Logged("ERROR", "std::terminate: c++ exception std::runtime_error: nobody catches this, thread ") == 1);
    // Then the abort, which is the same crash: said, not reported again.
    CHECK(crash.Child.Logged("ERROR", "fatal fault: abort") == 1);
}

TEST(the_terminate_handler_applies_norbytes_rule_below_its_own_frames)
{
    Crash crash("terminate-disabled");
    CheckTerminateReport(crash, "terminate-disabled", "std::runtime_error");
    CHECK(crash.Written.Value(crash::tag::MacFault, "extender_related") == "no");
    CHECK(crash.Written.Value(crash::tag::MacFault, "norbytes_rule") == "crash reporting disabled, as around a call into the game");
    CHECK(crash.Child.Logged("ERROR", "nobody catches this, thread ") == 1);
    CHECK(crash.Child.Logged("ERROR", "; Norbyte's reporter would leave it to the game's (crash reporting disabled") == 2);
}

TEST(the_terminate_handler_names_any_exception_and_none)
{
    Crash thrownInt("terminate-int");
    CheckTerminateReport(thrownInt, "terminate-int", "int");
    CHECK(thrownInt.Written.Line(crash::tag::MacFault, "exception_what=").empty());

    // what() faults: the report is written without it. The runtime's own
    // handler, called next, faults on it too.
    Crash badWhat("terminate-bad-what");
    CheckTerminateReport(badWhat, "terminate-bad-what", "BadWhat", SIGSEGV);
    CHECK(badWhat.Written.Line(crash::tag::MacFault, "exception_what=").empty());

    Crash none("terminate-without-exception");
    CheckTerminateReport(none, "terminate-without-exception", "none");
    CHECK(none.Written.Line(crash::tag::MacFault, "exception_what=").empty());
}

TEST(the_terminate_handler_reports_the_thread_that_ends)
{
    Crash crash("terminate-on-a-thread");
    CheckTerminateReport(crash, "terminate-on-a-thread", "std::runtime_error");
    CHECK(crash.Child.Number("thread-id") != 0);
    CHECK(crash.Written.Value(crash::tag::MacFault, "thread_id") == std::to_string(crash.Child.Number("thread-id")));
    CHECK(!crash.Written.Line(crash::tag::MacBacktrace, "symbol=_CrashTestThrowUncaught+").empty());
}

TEST(the_terminate_handler_is_installed_by_start_and_calls_the_one_before_it)
{
    Crash notStarted("terminate-not-started");
    CHECK(notStarted.Child.KilledBy(SIGABRT) && !notStarted.Child.Said("survived"));
    CHECK(notStarted.LeftNothing());

    Crash previous("terminate-previous");
    CheckTerminateReport(previous, "terminate-previous", "std::runtime_error");
    CHECK(previous.Child.Count("previous terminate handler: report written") == 1);

    // Two handlers that each call the other end the process once, with one report.
    Crash both("terminate-handlers-call-each-other");
    CheckTerminateReport(both, "terminate-handlers-call-each-other", "std::runtime_error");
    CHECK(!both.Child.Hung && both.Child.Count("game terminate handler") == 1);
}

// What the report has of the process beyond Norbyte's sections: the vector
// registers, the memory map, the build, the machine.
TEST(the_report_has_the_vector_registers_of_a_fault_caught_as_a_mach_exception)
{
    Crash crash("guarded-fault-vectors");
    if (!crash.Written.WellFormed) crash.Show("guarded-fault-vectors");
    CHECK(crash.Child.KilledBy(SIGSEGV) && !crash.Child.Said("survived"));
    CHECK(crash.Written.Value(crash::tag::MacFault, "mach_exception") != "0");
    CHECK(Hex(crash.Written.Value(crash::tag::MacFault, "address")) == 0x10);
    CHECK(crash.Written.Value(crash::tag::MacRegisters, "q0") == "0xff00ff00ff00ff00ff00ff00ff00ff00");
    CHECK(crash.Written.Value(crash::tag::MacRegisters, "q31") == "0x5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a");
    CHECK(!crash.Written.Value(crash::tag::MacRegisters, "fpsr").empty() && !crash.Written.Value(crash::tag::MacRegisters, "fpcr").empty());
    // A thread reporting on itself has none: they are not the crash's.
    Crash thrown("terminate-int");
    CHECK(thrown.Written.WellFormed && thrown.Written.Value(crash::tag::MacRegisters, "q0").empty());
}

TEST(the_report_has_the_memory_map_the_build_and_the_machine)
{
    Crash crash("guarded-fault");
    auto const& report = crash.Written;
    CHECK(report.WellFormed);
    using crash::tag::MacMemory;
    // The faulting address is in no region.
    CHECK(report.Line(MacMemory, "where=address ").rfind("where=address value=0x10 region=none", 0) == 0);
    // pc in this executable's code, sp in a stack.
    std::string pc = report.Line(MacMemory, "where=pc ");
    CHECK(Hex(Field(pc, "value")) == crash.Child.Number("fault-function") + 12);
    CHECK(Field(pc, "prot").rfind("r-x/", 0) == 0 && Field(pc, "image") == std::filesystem::path(gSelf).filename().string());
    std::string sp = report.Line(MacMemory, "where=sp ");
    CHECK(Field(sp, "tag") == "30(STACK)" && Field(sp, "prot").rfind("rw-/", 0) == 0);
    // x8 and x9 point nowhere, and get no line.
    CHECK(report.Line(MacMemory, "where=x8 ").empty() && report.Line(MacMemory, "where=x9 ").empty());
    CHECK(!report.Line(MacMemory, "summary tag=30(STACK) ").empty());
    std::string total = report.Line(MacMemory, "total ");
    CHECK(std::stoul(Field(total, "regions").empty() ? "0" : Field(total, "regions")) > 10 && Hex(Field(total, "virtual")) > 0);

    namespace version = bg3se::mac::version;
    using crash::tag::MacBuild;
    CHECK(report.Value(MacBuild, "version") == version::kVersion);
    CHECK(report.Value(MacBuild, "commit") == version::kCommit);
    CHECK(report.Value(MacBuild, "extender_version") == version::kExtenderVersion);
    CHECK(report.Value(MacBuild, "channel") == version::kChannel);
    CHECK(!OwnUuid().empty() && report.Value(MacBuild, "uuid") == OwnUuid());

    using crash::tag::MacSystem;
    CHECK(!report.Value(MacSystem, "model").empty() && !report.Value(MacSystem, "cpu").empty());
    CHECK(std::stoull("0" + report.Value(MacSystem, "memory")) > 0 && std::stoull("0" + report.Value(MacSystem, "cpus")) > 0);
    CHECK(std::stoull("0" + report.Value(MacSystem, "pid")) > 0);
}

TEST(a_cpp_exception_caught_inside_a_guarded_block_is_nothing_to_report)
{
    Crash crash("exception-caught-in-block");
    if (!crash.Child.Exited(0)) crash.Show("exception-caught-in-block");
    CHECK(crash.Child.Exited(0));
    CHECK(crash.Child.Said("unwound"));
    CHECK(crash.LeftNothing());
}

TEST(a_fault_in_what_the_host_writes_loses_only_that_part)
{
    Crash crash("host-faults");
    if (!crash.Written.WellFormed) crash.Show("host-faults");
    CHECK(crash.Child.KilledBy(SIGSEGV));
    CHECK(crash.Written.WellFormed);
    CHECK(!crash.Written.Has(crash::tag::LuaStackTrace) && !crash.Written.Has(crash::tag::LoadedModules));
    CHECK(crash.Written.Has(crash::tag::ClientState) && crash.Written.Has(crash::tag::MacImages));
    CHECK(crash.Written.Value(crash::tag::MacFault, "origin") == "unguarded");
    CHECK(crash.HasBacktraceText && crash.BacktraceText.starts_with(crash.Written.Body(crash::tag::StackTrace) + "\r\n\r\nThreads ("));
}

TEST(handlers_installed_before_and_after_run_around_the_reporter_for_a_fault_outside_any_block)
{
    Crash crash("handlers-unguarded");
    if (!crash.Written.WellFormed) crash.Show("handlers-unguarded");
    // The handler installed later runs first and passes the fault on; the
    // reporter writes; the fault goes on to the handler that was there
    // before; the process ends by the fault's own signal.
    CHECK(crash.Child.Output.find("later handler: report missing\nhost: build sections\n"
        "launch crash reporter: the report's path, report written, text written\nearlier handler: report written\n"
        "later handler: done, report written\n") != std::string::npos);
    CHECK(crash.Child.KilledBy(SIGSEGV) && !crash.Child.Said("survived"));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "origin") == "unguarded");
}

TEST(handlers_installed_before_and_after_still_run_after_a_guarded_block_has_reported)
{
    Crash crash("handlers-guarded");
    if (!crash.Written.WellFormed) crash.Show("handlers-guarded");
    // The block's filter has written before any signal handler runs.
    CHECK(crash.Child.Output.find("later handler: report written\nearlier handler: report written\nlater handler: done, report written\n")
        != std::string::npos);
    CHECK(crash.Child.KilledBy(SIGSEGV) && !crash.Child.Said("survived"));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "origin") == "guarded");

    Crash thrown("handlers-exception");
    CHECK(thrown.Child.Said("later handler: report written") && thrown.Child.Said("later handler: done, report written"));
    CHECK(thrown.Child.KilledBy(SIGABRT) && !thrown.Child.Said("survived"));
    CHECK(thrown.Written.Value(crash::tag::MacFault, "cause") == "c++ exception left a guarded block");
}

// The game's own handler is installed after the reporter's and passes
// nothing on (see GameHandler). What the first run in the game showed: the
// reporter, started and stopped while the dylib was loaded, was told nothing
// of a fault that came after the game had installed its handler.
TEST(the_reporter_is_told_before_the_games_handler_once_it_is_started_again)
{
    Crash behind("game-handler-in-front");
    CHECK(behind.Child.KilledBy(SIGSEGV) && !behind.Child.Said("survived") && !behind.Child.Said("FAIL"));
    CHECK(behind.Child.Said("game handler: report missing"));
    CHECK(behind.LeftNothing());

    // Started again, the reporter's handler is in front: the report is
    // written, then the game's handler gets the signal and ends the process.
    Crash crash("game-handler-unguarded");
    Crash control("unguarded-fault-not-started");
    if (!crash.Written.WellFormed) crash.Show("game-handler-unguarded");
    CHECK(crash.Child.KilledBy(SIGSEGV) && crash.Child.Status == control.Child.Status);
    CHECK(!crash.Child.Said("survived") && !crash.Child.Said("FAIL"));
    CHECK(crash.Written.WellFormed);
    CheckNullReadReport(crash, "unguarded");
    CHECK(crash.Child.Output.find("launch crash reporter: the report's path, report written, text written\ngame handler: report written\n")
        != std::string::npos);
    CHECK(crash.Child.Count("host: build sections") == 1 && crash.Child.Count("game handler: report written") == 1);
    CHECK(!crash.Child.Said("allocated while reporting"));

    Crash guarded("game-handler-guarded");
    if (!guarded.Written.WellFormed) guarded.Show("game-handler-guarded");
    CHECK(guarded.Child.KilledBy(SIGSEGV) && !guarded.Child.Said("survived") && !guarded.Child.Said("FAIL"));
    CHECK(guarded.Written.Value(crash::tag::MacFault, "origin") == "guarded");
    CHECK(guarded.Child.Count("host: build sections") == 1 && guarded.Child.Count("game handler: report written") == 1);

    Crash aborted("game-handler-abort");
    if (!aborted.Written.WellFormed) aborted.Show("game-handler-abort");
    CHECK(aborted.Child.KilledBy(SIGABRT) && !aborted.Child.Said("FAIL"));
    CHECK(aborted.Written.Value(crash::tag::MacFault, "kind") == "abort");
    CHECK(aborted.Child.Count("game handler: report written") == 1);
}

// The crash of 2026-11-11 (see GameWorker): nothing was written of it, no
// report and no line of the log. It is the extender's handler that faulted,
// but Norbyte's rule does not count a crash while crash reporting is
// disabled, and it is disabled for as long as the game's update runs.
TEST(a_fault_in_a_handler_the_game_calls_from_its_update_leaves_a_report_and_a_line_of_the_log)
{
    // With bg3mse's handler in front, and with the game's in front of it,
    // which tells bg3mse's nothing: the block's filter is asked either way.
    for (char const* name : { "fault-in-a-handler-the-game-calls", "fault-in-a-handler-the-game-calls-behind-the-games-handler" }) {
        Crash crash(name);
        if (!crash.Written.WellFormed) crash.Show(name);
        CHECK(crash.Child.KilledBy(SIGSEGV) && !crash.Child.Said("survived") && !crash.Child.Said("FAIL"));
        CHECK(crash.Written.WellFormed);
        CheckNullReadReport(crash, "guarded");
        // What Norbyte's rule makes of it is in the report.
        CHECK(crash.Written.Value(crash::tag::MacFault, "extender_related") == "no");
        CHECK(crash.Written.Value(crash::tag::MacFault, "crash_reporting_disabled") == "1");
        CHECK(crash.Written.Value(crash::tag::MacFault, "norbytes_rule") == "crash reporting disabled, as around a call into the game");
        // The game's code between the block and the handler.
        CHECK(!crash.Written.Line(crash::tag::MacBacktrace, "symbol=_crash_foreign_call+").empty());
        // The rule decides the notice, as it decides CrashReporter.exe: none, and the log says where the report is.
        CHECK(crash.Child.Output.find("launch crash reporter") == std::string::npos);
        CHECK(crash.Child.Logged("ERROR", "crash report written to " + crash.Dir + "/crash_") == 1);
        CHECK(crash.Child.Logged("ERROR", "(not the extender's crash: no notice)") == 1);
        CHECK(!crash.Child.Said("allocated while reporting"));

        // One line for the fault, with where it was.
        CHECK(crash.Child.Logged("ERROR", "fatal fault: bad access at 0x10") == 1);
        CHECK(crash.Child.Logged("ERROR", "_crash_test_read_null+0xc") == 1);
        CHECK(crash.Child.Logged("ERROR", "in a guarded block; Norbyte's reporter would leave it to the game's (crash reporting disabled") == 1);
        CHECK(crash.Child.Logged("ERROR", "fatal fault") == 1);
    }

    Crash behind("fault-in-a-handler-the-game-calls-behind-the-games-handler");
    CHECK(behind.Child.Count("game handler: report written") == 1);
}

// A crash of the game's own, which Norbyte's reporter leaves to the game's.
// Here it is written down as well: the report stays on this machine, and a
// log that stops without a word says nothing of what happened.
TEST(a_crash_with_no_frame_of_the_extender_leaves_a_report_that_says_so)
{
    Crash crash("foreign-only-logged");
    if (!crash.Written.WellFormed) crash.Show("foreign-only-logged");
    CHECK(crash.Child.KilledBy(SIGSEGV) && !crash.Child.Said("survived"));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "extender_related") == "no");
    CHECK(crash.Written.Value(crash::tag::MacFault, "crash_reporting_disabled") == "0");
    CHECK(crash.Written.Value(crash::tag::MacFault, "norbytes_rule") == "no frame of the extender");
    CHECK(crash.Written.Value(crash::tag::MacFault, "origin") == "unguarded");
    CHECK(Field(crash.Written.Line(crash::tag::MacBacktrace, "frame=0 "), "flags") == "---");
    CHECK(crash.Child.Logged("ERROR", "fatal fault: bad access at 0x10") == 1);
    CHECK(crash.Child.Logged("ERROR", "libbg3mse_crash_test_foreign.dylib") == 1);
}

// The report is whole, under its final name, and nothing is left of the
// temporary one.
void CheckOneWholeReport(Crash const& crash, char const* what, char const* cause)
{
    if (!crash.Written.WellFormed || crash.Names.size() != 3) {
        crash.Show(what);
        for (auto const& name : crash.Names) printf("  left: %s\n", name.c_str());
    }
    CHECK(crash.Child.Killed() && !crash.Child.Said("survived") && !crash.Child.Said("FAIL"));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Names.size() == 3 && crash.HasBacktraceText && crash.HasStackFile);
    CHECK(crash.Written.Has(crash::tag::MacSystem));
    CHECK(crash.Written.Value(crash::tag::MacFault, "cause") == cause);
    CHECK(crash.Child.Count("host: build sections") == 1);
    CHECK(crash.Child.Count("launch crash reporter: the report's path, report written, text written") == 1);
}

TEST(a_crash_on_another_thread_waits_for_the_report_that_is_being_written)
{
    // A second thread faults outside any block while the first one's report
    // is open: the process ends when the report is finished, not before.
    Crash fault("fault-while-a-report-is-written");
    CheckOneWholeReport(fault, "fault-while-a-report-is-written", "fault");
    CHECK(fault.Child.KilledBy(SIGSEGV));

    // The same with the game's handler installed, which ends the process
    // itself once it gets the second thread's signal: it gets it after the
    // wait.
    Crash inGame("fault-while-a-report-is-written-in-the-game");
    CheckOneWholeReport(inGame, "fault-while-a-report-is-written-in-the-game", "fault");
    CHECK(inGame.Child.KilledBy(SIGSEGV) && inGame.Child.Said("game handler: report written"));
    CHECK(!inGame.Child.Said("game handler: report missing"));

    // The same for a C++ exception leaving a guarded block on the second thread...
    Crash thrown("exception-while-a-report-is-written");
    CheckOneWholeReport(thrown, "exception-while-a-report-is-written", "fault");

    // ...and for a fault while the report of such an exception is written.
    Crash faultSecond("fault-while-an-exceptions-report-is-written");
    CheckOneWholeReport(faultSecond, "fault-while-an-exceptions-report-is-written", "c++ exception left a guarded block");
}

TEST(a_process_writes_one_report)
{
    Crash crash("fault-after-a-reported-one", gSelf, PutEarlierReports);
    if (!crash.Written.WellFormed) crash.Show("fault-after-a-reported-one");
    CHECK(crash.Child.KilledBy(SIGSEGV) && crash.Child.Said("recovered") && !crash.Child.Said("survived") && !crash.Child.Said("FAIL"));
    // The first fault's, from its block.
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "origin") == "guarded");
    CHECK(crash.Child.Count("host: build sections") == 1);
    // And the directory holds ten, as it is said to.
    unsigned reports = 0;
    for (auto const& name : crash.Names) {
        if (name.ends_with(".crpt") && name != "crash_latest.crpt") reports++;
    }
    CHECK(reports == crash::kReportsKept);
}

// The process's bundle, as the game's is the game: a copy of this executable
// inside an application bundle with a version.
TEST(the_report_names_the_bundle_the_process_runs_from_and_its_version)
{
    std::string pattern = (std::filesystem::temp_directory_path() / "bg3mse-crash-bundle.XXXXXX").string();
    CHECK(mkdtemp(pattern.data()) != nullptr);
    std::filesystem::path app = std::filesystem::path(pattern) / "Stand In.app" / "Contents";
    std::error_code error;
    std::filesystem::create_directories(app / "MacOS", error);
    std::filesystem::copy_file(gSelf, app / "MacOS" / "Stand In", error);
    CHECK(!error);
    std::ofstream(app / "Info.plist") << R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Stand In</string>
<key>CFBundleIdentifier</key><string>test.bg3mse.stand-in</string>
<key>CFBundleShortVersionString</key><string>4.1.1.7631656</string>
<key>CFBundleVersion</key><string>4.1.1.7631656</string>
</dict></plist>
)";
    {
        Crash crash("unguarded-fault", (app / "MacOS" / "Stand In").string());
        if (!crash.Written.WellFormed) crash.Show("unguarded-fault in a bundle");
        CHECK(crash.Child.KilledBy(SIGSEGV));
        CHECK(crash.Written.Value(crash::tag::MacSystem, "bundle_identifier") == "test.bg3mse.stand-in");
        CHECK(crash.Written.Value(crash::tag::MacSystem, "bundle_short_version") == "4.1.1.7631656");
        CHECK(crash.Written.Value(crash::tag::MacSystem, "bundle_version") == "4.1.1.7631656");
    }
    std::filesystem::remove_all(pattern, error);
}

}

// --- Other threads ---------------------------------------------------------------------

namespace {

// The lines of MTHR that begin with `head` and are about thread `thread`.
std::vector<std::string> ThreadLines(Report const& report, char const* head, std::string const& thread)
{
    std::vector<std::string> out;
    std::istringstream lines(report.Body(crash::tag::MacThreads));
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind(head, 0) == 0 && Field(line, "thread") == thread) out.push_back(line);
    }
    return out;
}

// The number of the thread whose header line ends in name=<name>; empty when none.
std::string ThreadNumber(Report const& report, std::string const& name)
{
    std::istringstream lines(report.Body(crash::tag::MacThreads));
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("thread=", 0) == 0 && line.ends_with(" name=" + name)) return Field(line, "thread");
    }
    return {};
}

std::string ThreadHead(Report const& report, std::string const& number)
{
    std::istringstream lines(report.Body(crash::tag::MacThreads));
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("thread=" + number + " ", 0) == 0) return line;
    }
    return {};
}

bool AnyFrameNamed(std::vector<std::string> const& frames, std::string_view function)
{
    for (auto const& line : frames) {
        if (Field(line, "symbol").find(function) != std::string::npos) return true;
    }
    return false;
}

// What every case of other threads leaves: each test thread listed by name,
// read, in its function; the crashing thread and bg3mse's own listed and
// left alone; and the same threads in the backtrace text.
void CheckOtherThreads(Crash const& crash, bool reporterIsCrashed)
{
    auto const& report = crash.Written;
    CHECK(report.Has(crash::tag::MacThreads));
    if (!report.Has(crash::tag::MacThreads)) {
        crash.Show("other threads");
        return;
    }
    std::string summary = report.Line(crash::tag::MacThreads, "threads ");
    CHECK(strtoul(Field(summary, "total").c_str(), nullptr, 10) >= 10);
    CHECK(Field(summary, "total") == Field(summary, "listed"));

    struct Expected
    {
        char const* Name;
        char const* Function;
    };
    Expected const expected[] = {
        { "bg3mse-test parked", "CrashTestParked" },
        { "bg3mse-test spinning", "CrashTestSpinning" },
        { "bg3mse-test unmapped fp", "crash_test_spin_with_fp" },
        { "bg3mse-test misaligned fp", "crash_test_spin_with_fp" },
        { "bg3mse-test looping fp", "crash_test_spin_with_fp" },
        { "bg3mse-test in malloc", "CrashTestParkedInMalloc" },
        { "bg3mse-test deep", "CrashTestDeep" },
    };
    for (auto const& thread : expected) {
        std::string number = ThreadNumber(report, thread.Name);
        CHECK(!number.empty());
        if (number.empty()) {
            printf("  no thread %s in:\n%s\n", thread.Name, report.Body(crash::tag::MacThreads).c_str());
            continue;
        }
        std::string head = ThreadHead(report, number);
        CHECK(Field(head, "role") == "other" && Field(head, "captured") == "yes");
        // Suspended by nobody but the reporter, which has resumed it by now.
        CHECK(Field(head, "suspend_count") == "0");
        auto frames = ThreadLines(report, "frame ", number);
        CHECK(!frames.empty() && frames.size() <= crash::ThreadList::kMaxFrames);
        CHECK(Field(head, "frames") == std::to_string(frames.size()));
        if (!AnyFrameNamed(frames, thread.Function)) {
            printf("  thread %s (%s) has no frame in %s:\n", thread.Name, number.c_str(), thread.Function);
            for (auto const& line : frames) printf("    %s\n", line.c_str());
        }
        CHECK(AnyFrameNamed(frames, thread.Function));
        auto registers = ThreadLines(report, "registers ", number);
        CHECK(registers.size() == 1 && !Field(registers.empty() ? "" : registers[0], "pc").empty());
        // The backtrace text names it and its function.
        std::string title = std::string("\nThread ") + number + " \"" + thread.Name + "\"";
        size_t at = crash.BacktraceText.find(title);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) {
            size_t end = crash.BacktraceText.find("\nThread ", at + 1);
            CHECK(crash.BacktraceText.substr(at, end == std::string::npos ? std::string::npos : end - at).find(thread.Function) != std::string::npos);
        }
    }
    // The frame-pointer walks that cannot go on stop at once: a thread with
    // an unmapped or misaligned frame pointer has its pc and link register,
    // one whose record points at itself one more.
    CHECK(ThreadLines(report, "frame ", ThreadNumber(report, "bg3mse-test unmapped fp")).size() == 2);
    CHECK(ThreadLines(report, "frame ", ThreadNumber(report, "bg3mse-test misaligned fp")).size() == 2);
    CHECK(ThreadLines(report, "frame ", ThreadNumber(report, "bg3mse-test looping fp")).size() == 3);
    // A thread deeper than the bound has as many frames as it allows.
    CHECK(ThreadLines(report, "frame ", ThreadNumber(report, "bg3mse-test deep")).size() == crash::ThreadList::kMaxFrames);
    // The queue's thread, by its queue's label.
    std::istringstream lines(report.Body(crash::tag::MacThreads));
    std::string queueThread;
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("queue ", 0) == 0 && line.ends_with(" label=org.bg3mse.test-queue")) queueThread = Field(line, "thread");
    }
    CHECK(!queueThread.empty() && AnyFrameNamed(ThreadLines(report, "frame ", queueThread), "CrashTestOnQueue"));
    CHECK(crash.BacktraceText.find(", queue \"org.bg3mse.test-queue\"") != std::string::npos);

    // The crashing thread is listed, not read: its frames are MBTR's.
    std::string crashed = report.Line(crash::tag::MacThreads, " role=crashed ");
    CHECK(Field(crashed, "id") == std::to_string(crash.Child.Number("thread-id")));
    CHECK(Field(crashed, "captured") == "no" && ThreadLines(report, "frame ", Field(crashed, "thread")).empty());
    // The thread that wrote the report, and bg3mse's fault handling.
    CHECK(report.Line(crash::tag::MacThreads, " role=reporter ").empty() == reporterIsCrashed);
    std::string handler = report.Line(crash::tag::MacThreads, " role=fault-handling ");
    CHECK(!handler.empty() && Field(handler, "captured") == "no");
    CHECK(!crash.Child.Said("allocated while reporting"));
}

}

TEST(the_report_has_every_other_thread_from_a_fault_seen_as_a_signal)
{
    Crash crash("threads-unguarded");
    CHECK(crash.Child.KilledBy(SIGSEGV) && !crash.Child.Said("survived") && !crash.Child.Said("FAIL"));
    CHECK(crash.Written.WellFormed);
    CheckOtherThreads(crash, false);
}

TEST(the_report_has_every_other_thread_from_a_guarded_block)
{
    Crash crash("threads-guarded");
    CHECK(crash.Child.KilledBy(SIGSEGV) && !crash.Child.Said("survived") && !crash.Child.Said("FAIL"));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "origin") == "guarded");
    CheckOtherThreads(crash, false);
}

TEST(the_report_has_every_other_thread_from_the_terminate_handler)
{
    Crash crash("threads-terminate");
    CHECK(crash.Child.KilledBy(SIGABRT) && !crash.Child.Said("survived") && !crash.Child.Said("FAIL"));
    CHECK(crash.Written.WellFormed);
    CHECK(crash.Written.Value(crash::tag::MacFault, "cause") == "std::terminate");
    CheckOtherThreads(crash, true);
}

TEST(every_thread_the_reporter_suspended_runs_again)
{
    Crash crash("threads-recovered");
    if (!crash.Child.Exited(0)) crash.Show("threads-recovered");
    CHECK(crash.Child.Exited(0) && crash.Child.Said("recovered") && crash.Child.Said("done"));
    CHECK(crash.Written.WellFormed);
    CheckOtherThreads(crash, false);
    CHECK(crash.Child.Said("spinning threads running 5"));
    CHECK(crash.Child.Said("threads still suspended 0"));
    CHECK(crash.Child.Said("parked thread woke"));
}

int main(int argc, char** argv)
{
    SetLogStream(nullptr);
    if (argc == 4 && std::string_view(argv[1]) == "--child") {
        snprintf(gReportDirectory, sizeof(gReportDirectory), "%s", argv[3]);
        for (auto const& child : kChildCases) {
            if (std::string_view(argv[2]) == child.Name) return child.Run();
        }
        return 100;
    }

    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return 1;
    gSelf = std::filesystem::canonical(path).string();

    void (*tests[])() = {
        a_guarded_block_runs_its_body_once_and_goes_on_run,
        a_fault_in_a_guarded_block_is_reported_and_then_takes_its_course_run,
        an_allocation_on_the_reporters_thread_would_be_noticed_run,
        the_report_has_norbytes_sections_in_his_order_and_the_mac_ones_run,
        the_stack_file_holds_the_stack_from_below_the_stack_pointer_to_its_top_run,
        report_names_carry_the_time_of_the_crash_run,
        the_directory_keeps_the_newest_ten_reports_run,
        a_fault_is_not_reported_by_a_reporter_that_was_never_started_run,
        a_reporter_that_was_started_and_stopped_still_reports_run,
        a_fault_while_crash_reporting_is_disabled_is_reported_as_not_the_extenders_run,
        forcing_reports_a_fault_while_crash_reporting_is_disabled_run,
        a_fault_in_a_block_inside_a_block_is_reported_once_run,
        a_stack_overflow_is_the_extenders_only_when_forced_run,
        a_fault_outside_any_block_reaches_the_reporter_run,
        a_reporter_is_started_and_stopped_again_and_again_run,
        abort_with_the_extenders_frames_on_the_stack_is_reported_run,
        a_crash_with_no_frame_of_the_extender_is_the_extenders_only_when_forced_run,
        a_crash_below_a_hook_is_the_extenders_unless_the_hook_is_registered_or_listed_run,
        a_fault_inside_a_registered_function_is_reported_run,
        the_rule_for_which_crashes_involve_the_extender_run,
        a_cpp_exception_leaving_a_guarded_block_is_reported_with_the_thrower_on_the_stack_run,
        a_cpp_exception_leaving_a_guarded_block_is_reported_whether_or_not_reporting_is_started_run,
        a_cpp_exception_passes_through_a_guarded_block_while_crash_reporting_is_disabled_run,
        a_cpp_exception_caught_inside_a_guarded_block_is_nothing_to_report_run,
        a_fault_in_what_the_host_writes_loses_only_that_part_run,
        handlers_installed_before_and_after_run_around_the_reporter_for_a_fault_outside_any_block_run,
        handlers_installed_before_and_after_still_run_after_a_guarded_block_has_reported_run,
        the_reporter_is_told_before_the_games_handler_once_it_is_started_again_run,
        a_fault_in_a_handler_the_game_calls_from_its_update_leaves_a_report_and_a_line_of_the_log_run,
        a_crash_with_no_frame_of_the_extender_leaves_a_report_that_says_so_run,
        a_process_writes_one_report_run,
        a_crash_on_another_thread_waits_for_the_report_that_is_being_written_run,
        the_report_names_the_bundle_the_process_runs_from_and_its_version_run,
        an_exception_nobody_catches_is_reported_by_the_terminate_handler_with_its_type_and_what_run,
        the_terminate_handler_applies_norbytes_rule_below_its_own_frames_run,
        the_terminate_handler_names_any_exception_and_none_run,
        the_terminate_handler_reports_the_thread_that_ends_run,
        the_terminate_handler_is_installed_by_start_and_calls_the_one_before_it_run,
        the_report_has_the_vector_registers_of_a_fault_caught_as_a_mach_exception_run,
        the_report_has_the_memory_map_the_build_and_the_machine_run,
        the_report_has_every_other_thread_from_a_fault_seen_as_a_signal_run,
        the_report_has_every_other_thread_from_a_guarded_block_run,
        the_report_has_every_other_thread_from_the_terminate_handler_run,
        every_thread_the_reporter_suspended_runs_again_run,
    };
    for (auto test : tests) test();
    printf("%zu tests, %d failures\n", std::size(tests), gFailures);
    return gFailures ? 1 : 0;
}
