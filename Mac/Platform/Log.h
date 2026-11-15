#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>
#include <string_view>

// The log of one module: a stream, a minimum level, and log files that are
// opened and pruned by name. CoreLib's logging entry points (Debug /
// DebugLocal, behind the DEBUG / INFO / WARN / ERR macros) write here
// (Platform/DebugLog.cpp).

namespace bg3se::mac {

enum class LogLevel
{
    Debug,
    Info,
    Osiris,
    Warning,
    Error
};

// Opens <dir>/<prefix>_<timestamp>.log and keeps the newest `keep` logs with
// that prefix. The log with the prefix "bg3mse" is also linked as latest.log,
// any other as <prefix>_latest.log, so modules can share the directory.
// Returns the path, or an empty string when the directory cannot be used
// (logging then stays on stderr).
std::string OpenLogFile(std::string const& dir, std::string_view prefix = "bg3mse", unsigned keep = 20);

// Tools and tests log to a stream of their choice; nullptr silences logging.
void SetLogStream(FILE* stream);

// Minimum level written: DEBUG messages are dropped unless this is set.
void SetLogDebug(bool enabled);

// Every line written is also written to stderr (the terminal the process
// was started from), when the stream is another: a console beside the file.
void SetLogEcho(bool enabled);

// A function given every line written, after it is written: its level and
// its text without the time. It is called without the log's lock held, on
// the thread that logged, and may log itself (that line is not given to it).
using LogSink = void (*)(LogLevel level, char const* text);
void SetLogSink(LogSink sink);

// One line at `level`, timestamped.
void LogV(LogLevel level, char const* fmt, va_list args);
void Log(LogLevel level, char const* fmt, ...) __attribute__((format(printf, 2, 3)));

// One line at `level`, for a place that may neither take a lock nor allocate:
// a fault is being handled, and the thread it stopped may be inside this log
// or the C library. The line has the form of every other and is written with
// write(2) straight to the log's descriptor (again for what a short write
// left over), after whatever lines are in the file; text beyond about a
// thousand characters is cut. The descriptor is the log file's, and stderr
// until a log file is open: a write to a pipe nobody reads waits.
void LogFromFaultHandler(LogLevel level, char const* text);

}
