#pragma once

// Logging for the graphics files compiled as Objective-C++, which do not take
// the extender's precompiled header: the same log as INFO / WARN / ERR.

namespace bg3se::mac::gfx {

void LogInfo(char const* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogWarn(char const* fmt, ...) __attribute__((format(printf, 1, 2)));
void LogError(char const* fmt, ...) __attribute__((format(printf, 1, 2)));

}
