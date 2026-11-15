#include <stdafx.h>
#include <Platform/DebugLog.h>
#include <Platform/Log.h>

#include <atomic>
#include <cstdarg>
#include <xlocale.h>

// The macOS implementation of CoreLib's logging entry points (Debug /
// DebugLocal, behind the DEBUG / INFO / WARN / ERR macros): the extender's
// console object when it takes messages, the platform log otherwise
// (Platform/DebugLog.h).

namespace bg3se {

namespace {

constinit std::atomic<mac::DebugConsoleSink*> gConsoleSink{ nullptr };

mac::LogLevel LevelOf(DebugMessageType type)
{
    switch (type) {
    case DebugMessageType::Debug: return mac::LogLevel::Debug;
    case DebugMessageType::Info: return mac::LogLevel::Info;
    case DebugMessageType::Osiris: return mac::LogLevel::Osiris;
    case DebugMessageType::Warning: return mac::LogLevel::Warning;
    case DebugMessageType::Error: return mac::LogLevel::Error;
    }
    return mac::LogLevel::Error;
}

// True when the console object took the message.
bool ToConsoleSink(DebugMessageType type, bool local, char const* fmt, va_list args)
{
    auto sink = gConsoleSink.load(std::memory_order_acquire);
    if (!sink) return false;
    // CoreLib formats into 1024 bytes and cuts what does not fit.
    char text[1024];
    vsnprintf_l(text, sizeof(text), LC_C_LOCALE, fmt, args);
    return sink(static_cast<int>(type), text, local);
}

void Message(DebugMessageType type, bool local, char const* fmt, va_list args)
{
    va_list again;
    va_copy(again, args);
    if (!ToConsoleSink(type, local, fmt, again)) mac::LogV(LevelOf(type), fmt, args);
    va_end(again);
}

}

void Debug(DebugMessageType type, char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Message(type, false, fmt, args);
    va_end(args);
}

void DebugLocal(DebugMessageType type, char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Message(type, true, fmt, args);
    va_end(args);
}

void mac::SetDebugConsoleSink(DebugConsoleSink* sink)
{
    gConsoleSink.store(sink, std::memory_order_release);
}

void mac::LogConsoleMessage(int type, char const* text)
{
    Log(LevelOf(static_cast<DebugMessageType>(type)), "%s", text);
}

}
