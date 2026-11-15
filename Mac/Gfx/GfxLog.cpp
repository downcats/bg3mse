#include <stdafx.h>
#include <Gfx/GfxLog.h>

#include <cstdarg>

namespace bg3se::mac::gfx {

namespace {

void Write(DebugMessageType type, char const* fmt, va_list args)
{
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, args);
    Debug(type, "%s", line);
}

}

void LogInfo(char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write(DebugMessageType::Info, fmt, args);
    va_end(args);
}

void LogWarn(char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write(DebugMessageType::Warning, fmt, args);
    va_end(args);
}

void LogError(char const* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write(DebugMessageType::Error, fmt, args);
    va_end(args);
}

}
