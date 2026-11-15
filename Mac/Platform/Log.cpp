#include <Platform/Log.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
#include <xlocale.h>

namespace bg3se::mac {

namespace {

// Constant-initialised: the dylib's constructor can run before this file's
// dynamic initialisers, which must not reset what it set up.
constinit std::mutex gLogMutex;
constinit FILE* gLogStream = nullptr;     // nullptr: stderr
constinit bool gLogSilenced = false;
constinit bool gLogDebug = false;
constinit bool gLogEcho = false;
constinit std::atomic<LogSink> gLogSink{ nullptr };
thread_local bool tInSink = false;
// For LogFromFaultHandler, which can ask neither the stream for its
// descriptor (that locks the stream) nor the C library for local time:
// the descriptor lines go to (-1: nowhere), and local time's distance from
// UTC as the last line written found it.
constinit std::atomic<int> gLogDescriptor{ STDERR_FILENO };
constinit std::atomic<long> gLogUtcOffset{ 0 };

char const* LevelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info: return "INFO ";
    case LogLevel::Osiris: return "OSI  ";
    case LogLevel::Warning: return "WARN ";
    case LogLevel::Error: return "ERROR";
    }
    return "?";
}

// <prefix>_<timestamp>.log: the timestamp starts with a digit, which keeps
// <prefix>_latest.log out.
bool IsLogOf(std::string_view name, std::string_view prefix)
{
    return name.size() > prefix.size() + 1 && name.starts_with(prefix) && name[prefix.size()] == '_'
        && isdigit((unsigned char)name[prefix.size() + 1]) && name.ends_with(".log");
}

void PruneLogs(std::string const& dir, std::string_view prefix, unsigned keep)
{
    std::vector<std::string> logs;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string_view name(e->d_name);
            if (IsLogOf(name, prefix)) logs.emplace_back(name);
        }
        closedir(d);
    }
    // The timestamp in the name sorts chronologically.
    std::sort(logs.begin(), logs.end());
    for (size_t i = 0; i + keep < logs.size(); i++) {
        unlink((dir + "/" + logs[i]).c_str());
    }
}

}

std::string OpenLogFile(std::string const& dir, std::string_view prefix, unsigned keep)
{
    mkdir(dir.c_str(), 0755);

    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
    std::string path = dir + "/" + std::string(prefix) + "_" + stamp + ".log";

    // Closed in a program this process starts (the game starts its crash
    // reporter and others), which has no use for this process's log. By
    // open(2): fopen's "e" is not honoured by this C library.
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    FILE* f = fd >= 0 ? fdopen(fd, "w") : nullptr;
    if (!f) {
        if (fd >= 0) close(fd);
        return {};
    }
    SetLogStream(f);

    std::string latest = dir + "/" + (prefix == "bg3mse" ? std::string("latest.log") : std::string(prefix) + "_latest.log");
    unlink(latest.c_str());
    symlink(path.c_str(), latest.c_str());
    PruneLogs(dir, prefix, keep);
    return path;
}

void SetLogStream(FILE* stream)
{
    std::lock_guard lock(gLogMutex);
    gLogStream = stream;
    gLogSilenced = stream == nullptr;
    gLogDescriptor.store(stream ? fileno(stream) : -1, std::memory_order_release);
    time_t now = time(nullptr);
    tm local{};
    if (localtime_r(&now, &local)) gLogUtcOffset.store(local.tm_gmtoff, std::memory_order_relaxed);
}

void SetLogDebug(bool enabled)
{
    gLogDebug = enabled;
}

void SetLogEcho(bool enabled)
{
    std::lock_guard lock(gLogMutex);
    gLogEcho = enabled;
}

void SetLogSink(LogSink sink)
{
    gLogSink.store(sink, std::memory_order_release);
}

void LogV(LogLevel level, char const* fmt, va_list args)
{
    if (level == LogLevel::Debug && !gLogDebug) return;

    LogSink sink = tInSink ? nullptr : gLogSink.load(std::memory_order_acquire);
    if (!sink) {
        std::lock_guard lock(gLogMutex);
        if (gLogSilenced) return;
    }
    std::string text;
    {
        va_list copy;
        va_copy(copy, args);
        char small[512];
        // The "C" locale, whatever the game sets: as Norbyte's static C library on Windows.
        int n = vsnprintf_l(small, sizeof(small), LC_C_LOCALE, fmt, copy);
        va_end(copy);
        if (n < 0) return;
        if ((size_t)n < sizeof(small)) {
            text.assign(small, (size_t)n);
        } else {
            text.resize((size_t)n + 1);
            va_copy(copy, args);
            vsnprintf_l(text.data(), text.size(), LC_C_LOCALE, fmt, copy);
            va_end(copy);
            text.resize((size_t)n);
        }
    }

    {
        std::lock_guard lock(gLogMutex);
        if (!gLogSilenced) {
            FILE* out = gLogStream ? gLogStream : stderr;

            timeval tv{};
            gettimeofday(&tv, nullptr);
            tm local{};
            localtime_r(&tv.tv_sec, &local);
            gLogUtcOffset.store(local.tm_gmtoff, std::memory_order_relaxed);
            char stamp[32];
            strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);

            for (FILE* to : { out, gLogEcho && out != stderr ? stderr : nullptr }) {
                if (!to) continue;
                fprintf(to, "[%s.%03d] [%s] %s\n", stamp, (int)(tv.tv_usec / 1000), LevelName(level), text.c_str());
                fflush(to);
            }
        }
    }

    if (sink) {
        tInSink = true;
        sink(level, text.c_str());
        tInSink = false;
    }
}

void LogFromFaultHandler(LogLevel level, char const* text)
{
    if (level == LogLevel::Debug && !gLogDebug) return;
    int descriptor = gLogDescriptor.load(std::memory_order_acquire);
    if (descriptor < 0) return;

    timeval tv{};
    gettimeofday(&tv, nullptr);
    long second = static_cast<long>((tv.tv_sec + gLogUtcOffset.load(std::memory_order_relaxed)) % 86400);
    if (second < 0) second += 86400;

    // "[HH:MM:SS.mmm] [LEVEL] text\n"
    char line[1100];
    size_t length = 0;
    auto digits = [&](long value, int count) {
        for (int i = count - 1; i >= 0; i--) {
            line[length + static_cast<size_t>(i)] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
        length += static_cast<size_t>(count);
    };
    auto put = [&](char const* part) {
        while (*part && length < sizeof(line) - 1) line[length++] = *part++;
    };
    put("[");
    digits(second / 3600, 2);
    put(":");
    digits(second / 60 % 60, 2);
    put(":");
    digits(second % 60, 2);
    put(".");
    digits(tv.tv_usec / 1000, 3);
    put("] [");
    put(LevelName(level));
    put("] ");
    put(text);
    line[length++] = '\n';

    char const* at = line;
    while (length > 0) {
        ssize_t wrote = write(descriptor, at, length);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return;
        }
        at += wrote;
        length -= static_cast<size_t>(wrote);
    }
}

void Log(LogLevel level, char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogV(level, fmt, args);
    va_end(args);
}

}

