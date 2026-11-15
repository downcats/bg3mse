// The calling thread's last error, the process, time, the debugger and UUIDs.
#include <windows.h>

#include <crt_externs.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sched.h>
#include <string>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {
thread_local DWORD gLastError = ERROR_SUCCESS;
}

DWORD GetLastError()
{
    return gLastError;
}

void SetLastError(DWORD error)
{
    gLastError = error;
}

HANDLE GetCurrentProcess()
{
    return (HANDLE)(intptr_t)-1;
}

HANDLE GetCurrentThread()
{
    return (HANDLE)(intptr_t)-2;
}

DWORD GetCurrentProcessId()
{
    return (DWORD)getpid();
}

BOOL TerminateProcess(HANDLE process, UINT exitCode)
{
    if (process != GetCurrentProcess()) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    // Like TerminateProcess, ends the process at once: no exit handlers, no destructors.
    _exit((int)exitCode);
}

void ExitProcess(UINT exitCode)
{
    exit((int)exitCode);
}

void Sleep(DWORD milliseconds)
{
    if (milliseconds == 0) {
        sched_yield();
        return;
    }
    if (milliseconds == INFINITE) {
        for (;;) pause();
    }
    struct timespec ts{ (time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

ULONGLONG GetTickCount64()
{
    return clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000ull;
}

// The performance counter of Windows 10 and later: 10 MHz, 100 ns a tick,
// whatever the hardware (Ext.Timer.MicrosecTime shows its ticks to scripts).
BOOL QueryPerformanceCounter(LARGE_INTEGER* counter)
{
    counter->QuadPart = (LONGLONG)(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 100);
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency)
{
    frequency->QuadPart = 10000000ll;
    return TRUE;
}

BOOL IsDebuggerPresent()
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    struct kinfo_proc info{};
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0) {
        return FALSE;
    }
    return (info.kp_proc.p_flag & P_TRACED) ? TRUE : FALSE;
}

RPC_STATUS UuidCreate(UUID* uuid)
{
    arc4random_buf(uuid, sizeof(*uuid));
    // Version 4 (random) in the high bits of Data3, the RFC 4122 variant in Data4[0].
    uuid->Data3 = (uint16_t)((uuid->Data3 & 0x0FFF) | 0x4000);
    uuid->Data4[0] = (uint8_t)((uuid->Data4[0] & 0x3F) | 0x80);
    return RPC_S_OK;
}

namespace {

// One argument, written so that CommandLineToArgvW gives it back: as it is, or
// in quotes with each quote escaped and the backslashes before a quote doubled.
void AppendArgument(std::string& line, char const* argument)
{
    if (*argument && !strpbrk(argument, " \t\n\v\"")) {
        line += argument;
        return;
    }
    line += '"';
    for (char const* p = argument;; p++) {
        size_t backslashes = 0;
        while (*p == '\\') {
            p++;
            backslashes++;
        }
        if (!*p) {
            line.append(backslashes * 2, '\\');
            break;
        }
        if (*p == '"') {
            line.append(backslashes * 2 + 1, '\\');
        } else {
            line.append(backslashes, '\\');
        }
        line += *p;
    }
    line += '"';
}

}

LPWSTR GetCommandLineW()
{
    static std::wstring line = [] {
        std::string utf8;
        char** arguments = *_NSGetArgv();
        for (int i = 0, count = *_NSGetArgc(); i < count; i++) {
            if (i) utf8 += ' ';
            AppendArgument(utf8, arguments[i]);
        }
        int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
        std::wstring wide((size_t)length, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), wide.data(), length);
        return wide;
    }();
    return line.data();
}

BOOL IsBadReadPtr(const void* address, UINT_PTR size)
{
    if (size == 0) {
        return FALSE;
    }
    uintptr_t first = (uintptr_t)address, last = first + size - 1;
    if (!address || last < first) {
        return TRUE;
    }
    uintptr_t const pageMask = (uintptr_t)vm_page_size - 1;
    for (uintptr_t at = first;;) {
        char byte;
        mach_vm_size_t copied = 0;
        if (mach_vm_read_overwrite(mach_task_self(), at, 1, (mach_vm_address_t)&byte, &copied) != KERN_SUCCESS
            || copied != 1) {
            return TRUE;
        }
        uintptr_t nextPage = (at | pageMask) + 1;
        if (nextPage == 0 || nextPage > last) {
            return FALSE;
        }
        at = nextPage;
    }
}

HLOCAL LocalAlloc(UINT flags, SIZE_T bytes)
{
    if ((flags & ~(UINT)LMEM_ZEROINIT) != LMEM_FIXED) {
        // Movable memory (LMEM_MOVEABLE) is not provided.
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    void* p = (flags & LMEM_ZEROINIT) ? calloc(1, bytes ? bytes : 1) : malloc(bytes ? bytes : 1);
    if (!p) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    }
    return p;
}

HLOCAL LocalFree(HLOCAL memory)
{
    free(memory);
    return nullptr;
}
