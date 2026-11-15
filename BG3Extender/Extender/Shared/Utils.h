#pragma once

#include <GameDefinitions/Base/Base.h>
#include <CoreLib/Utils.h>

#if defined(_MSC_VER)
namespace std
{
    class thread;
}
#else
// libc++ declares std::thread in an inline namespace, which a declaration in
// namespace std itself would make ambiguous.
#include <thread>
#endif

BEGIN_SE()

#if !defined(OSI_NO_DEBUG_LOG)
#define LuaError(msg) { \
    std::stringstream ss; \
    ss << __FUNCTION__ "(): " msg; \
    LogLuaError(ss.str()); \
}

#define OsiError(msg) { \
    std::stringstream ss; \
    ss << __FUNCTION__ "(): " msg; \
    LogOsirisError(ss.str()); \
}

#define OsiWarn(msg) { \
    std::stringstream ss; \
    ss << __FUNCTION__ "(): " msg; \
    LogOsirisWarning(ss.str()); \
}

#define OsiMsg(msg) { \
    std::stringstream ss; \
    ss << msg; \
    LogOsirisMsg(ss.str()); \
}

#define OsiErrorS(msg) LogOsirisError(__FUNCTION__ "(): " msg)
#define OsiWarnS(msg) LogOsirisWarning(__FUNCTION__ "(): " msg)
#define OsiMsgS(msg) LogOsirisMsg(__FUNCTION__ "(): " msg)
#else
#define LuaError(msg) (void)0
#define OsiError(msg) (void)0
#define OsiWarn(msg) (void)0
#define OsiMsg(msg) (void)0
#define OsiErrorS(msg) (void)0
#define OsiWarnS(msg) (void)0
#define OsiMsgS(msg) (void)0
#endif


void LogLuaError(std::string_view msg);
void LogOsirisError(std::string_view msg);
void LogOsirisWarning(std::string_view msg);
void LogOsirisMsg(std::string_view msg);

extern std::atomic<uint32_t> gDisableCrashReportingCount;

struct DisableCrashReporting
{
    inline DisableCrashReporting()
    {
        ++gDisableCrashReportingCount;
    }

    inline ~DisableCrashReporting()
    {
        --gDisableCrashReportingCount;
    }

    DisableCrashReporting(DisableCrashReporting const&) = delete;
    DisableCrashReporting& operator = (DisableCrashReporting const&) = delete;
};

END_SE()
