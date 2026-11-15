// The test of what Norbyte's extender does with a written crash report
// (Mac/Crash/CrashDelivery.cpp): one line in the log, made without a lock and
// without an allocation. A test of its own, because the reporter's tests
// define LaunchCrashReporter themselves, to see the call.

#include <Crash/CrashReporter.h>
#include <Platform/Log.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc/malloc.h>
#include <new>
#include <string>
#include <unistd.h>

using namespace bg3se::mac;

namespace {

int gFailures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); gFailures++; } } while (0)

// Counts what this executable allocates while it is told to.
std::atomic<bool> gCounting{ false };
std::atomic<unsigned> gAllocations{ 0 };

void NoteAllocation()
{
    if (gCounting.load(std::memory_order_relaxed)) gAllocations.fetch_add(1, std::memory_order_relaxed);
}

std::string ReadAll(char const* path)
{
    std::string text;
    if (FILE* f = fopen(path, "r")) {
        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, n);
        fclose(f);
    }
    return text;
}

// "[HH:MM:SS.mmm] [LEVEL] " at the start of a line.
bool HasStamp(std::string const& line, char const* level)
{
    if (line.size() < 23 || line[0] != '[' || line[3] != ':' || line[6] != ':' || line[9] != '.' || line[13] != ']') return false;
    for (int i : { 1, 2, 4, 5, 7, 8, 10, 11, 12 }) {
        if (line[(size_t)i] < '0' || line[(size_t)i] > '9') return false;
    }
    return line.compare(14, 9, std::string(" [") + level + "] ") == 0;
}

}

// The allocator of everything linked into this executable.
extern "C" void* malloc(size_t size)
{
    NoteAllocation();
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
    NoteAllocation();
    void* p = malloc_zone_malloc(malloc_default_zone(), size ? size : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

int main()
{
    char path[] = "/tmp/bg3mse-crash-delivery-XXXXXX";
    int fd = mkstemp(path);
    FILE* log = fd >= 0 ? fdopen(fd, "w") : nullptr;
    CHECK(log != nullptr);
    if (!log) return 1;
    SetLogStream(log);

    Log(LogLevel::Info, "before");
    char const* report = "/Users/someone/Library/Application Support/bg3mse/Crashes/crash_2026-11-10_21-30-05.crpt";
    gCounting = true;
    crash::LaunchCrashReporter(report);
    gCounting = false;
    CHECK(gAllocations.load() == 0);
    Log(LogLevel::Info, "after");

    // A path longer than a line is cut, not written past the line.
    std::string longPath(4000, 'x');
    crash::LaunchCrashReporter(longPath.c_str());
    // A debug line is dropped unless debug lines are on, as by Log.
    LogFromFaultHandler(LogLevel::Debug, "not written");
    SetLogDebug(true);
    LogFromFaultHandler(LogLevel::Debug, "written");
    SetLogDebug(false);
    // A silenced log gets nothing.
    SetLogStream(nullptr);
    crash::LaunchCrashReporter("/nowhere");
    fclose(log);

    std::string text = ReadAll(path);
    unlink(path);
    std::string lines[5];
    size_t at = 0;
    for (auto& line : lines) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) break;
        line = text.substr(at, end - at);
        at = end + 1;
    }
    CHECK(at == text.size());      // five lines, and nothing after them

    CHECK(HasStamp(lines[0], "INFO ") && lines[0].substr(23) == "before");
    CHECK(HasStamp(lines[1], "ERROR") && lines[1].substr(23) == std::string("crash report written to ") + report);
    CHECK(HasStamp(lines[2], "INFO ") && lines[2].substr(23) == "after");
    // The same clock as the lines around it: the hour and the minute agree
    // with one of them (the minute may have turned between two lines).
    CHECK(lines[1].compare(0, 6, lines[0], 0, 6) == 0 || lines[1].compare(0, 6, lines[2], 0, 6) == 0);
    CHECK(HasStamp(lines[3], "ERROR") && lines[3].size() == 23 + 1023 && lines[3].compare(23, 24, "crash report written to ") == 0);
    CHECK(HasStamp(lines[4], "DEBUG") && lines[4].substr(23) == "written");

    // The next start is told of the report: where it is and its backtrace,
    // once. The note is made without an allocation too.
    char directory[] = "/tmp/bg3mse-crash-notice-XXXXXX";
    CHECK(mkdtemp(directory) != nullptr);
    std::string dir = directory;
    std::string written = dir + "/crash_2026-11-12_10-00-00.crpt";
    if (FILE* f = fopen(written.c_str(), "w")) { fputs("report", f); fclose(f); }
    if (FILE* f = fopen((dir + "/crash_2026-11-12_10-00-00.bt").c_str(), "w")) { fputs("frame 1\nframe 2\n\n\nLua Backtrace:\nline\n\nThreads (2):\nthread\n", f); fclose(f); }
    CHECK(!crash::TakeCrashNotice(dir));
    gCounting = true;
    crash::LaunchCrashReporter(written.c_str());
    gCounting = false;
    CHECK(gAllocations.load() == 0);
    auto notice = crash::TakeCrashNotice(dir);
    CHECK(notice && *notice == "The game crashed last time. Crash report:\n" + written
        + "\n\nBacktrace:\nframe 1\nframe 2");
    CHECK(!crash::TakeCrashNotice(dir));
    // A long backtrace is cut to its first frames: the alert must fit a screen.
    if (FILE* f = fopen((dir + "/crash_2026-11-12_10-00-00.bt").c_str(), "w")) {
        for (int i = 0; i < 40; i++) fprintf(f, "frame %d\n", i);
        fclose(f);
    }
    crash::LaunchCrashReporter(written.c_str());
    notice = crash::TakeCrashNotice(dir);
    CHECK(notice && notice->find("frame 11") != std::string::npos && notice->find("frame 12") == std::string::npos);
    // Without its backtrace the report is still named; a report that is gone is not.
    unlink((dir + "/crash_2026-11-12_10-00-00.bt").c_str());
    crash::LaunchCrashReporter(written.c_str());
    notice = crash::TakeCrashNotice(dir);
    CHECK(notice && notice->find(written) != std::string::npos && notice->find("Backtrace") == std::string::npos);
    crash::LaunchCrashReporter(written.c_str());
    unlink(written.c_str());
    CHECK(!crash::TakeCrashNotice(dir));
    CHECK(access((dir + "/" + crash::kCrashNoticeFile).c_str(), F_OK) != 0);
    rmdir(directory);

    printf("%d failures\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
