// The Win32 API that Norbyte's code names, for the macOS build.
//
// Types that are embedded in game structures keep their Windows sizes. A
// function declared here is implemented in Mac/Shim/*.cpp (or inline below)
// with the Windows semantics and tested in Mac/Tests (ShimTests*.cpp, and
// LockTests.cpp for the locks). What only Norbyte's MSVC branches call is not
// declared.
//
// wchar_t is 4 bytes on macOS and wide strings hold UTF-32.
#pragma once

// MSVC's standard headers include <algorithm> from the containers and
// <string>; Norbyte's sources rely on that.
#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <exception>
// The file streams Norbyte's code opens with wide paths (an MSVC extension)
// take a std::filesystem::path on macOS.
#include <filesystem>
#include <list>
#include <memory>
#include <strings.h>
#include <string>
#include <type_traits>
#include <wchar.h>
#include <xlocale.h>

#include "../Layout/GameLocks.h"

// --- Types -----------------------------------------------------------------

typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int64_t LONG64;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef int BOOL;
typedef unsigned char BOOLEAN;
typedef unsigned int UINT;
typedef short SHORT;
typedef char CHAR;
typedef wchar_t WCHAR;
typedef wchar_t TCHAR;
typedef uint64_t ULONG_PTR;
typedef uint64_t DWORD_PTR;
typedef uint64_t UINT_PTR;
typedef int64_t LONG_PTR;
typedef uint64_t SIZE_T;
typedef int32_t HRESULT;
typedef void* HANDLE;
typedef void* HMODULE;
typedef void* HINSTANCE;
typedef void* HWND;
typedef void* HRSRC;
typedef void* HGLOBAL;
typedef void* HLOCAL;
typedef void* LPVOID;
typedef void* PVOID;
typedef const void* LPCVOID;
typedef char* LPSTR;
typedef const char* LPCSTR;
typedef wchar_t* LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef DWORD* LPDWORD;
typedef int64_t (*FARPROC)();
typedef long RPC_STATUS;

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    LONGLONG QuadPart;
} LARGE_INTEGER;

typedef struct _GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} GUID;
typedef GUID UUID;

typedef struct _SECURITY_ATTRIBUTES SECURITY_ATTRIBUTES;
typedef SECURITY_ATTRIBUTES* LPSECURITY_ATTRIBUTES;

// What an SEH filter is handed on Windows. Norbyte's headers declare
// HandleGuardedException(DWORD, EXCEPTION_POINTERS*) (the filter of
// BEGIN_GUARDED, defined in the crash reporter); nothing on macOS creates one.
struct _EXCEPTION_POINTERS;
typedef _EXCEPTION_POINTERS EXCEPTION_POINTERS;
typedef _EXCEPTION_POINTERS* PEXCEPTION_POINTERS;
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)

struct _WIN32_FIND_DATAW;
typedef _WIN32_FIND_DATAW* LPWIN32_FIND_DATAW;

struct VS_FIXEDFILEINFO
{
    DWORD dwSignature;
    DWORD dwStrucVersion;
    DWORD dwFileVersionMS;
    DWORD dwFileVersionLS;
    DWORD dwProductVersionMS;
    DWORD dwProductVersionLS;
    DWORD dwFileFlagsMask;
    DWORD dwFileFlags;
    DWORD dwFileOS;
    DWORD dwFileType;
    DWORD dwFileSubtype;
    DWORD dwFileDateMS;
    DWORD dwFileDateLS;
};

// Lock types embedded in game structures. Norbyte's definitions use these
// names only for locks that are part of a game structure, so on Mac they are
// what the Mac game has there (Mac/Layout/GameLocks.h), not what Windows has:
// CRITICAL_SECTION is ls::CriticalSection, 20 bytes aligned to 4, and SRWLOCK
// is ls::SRWKernelLock, 16 bytes aligned to 8.
typedef bg3se::mac::GameCriticalSection CRITICAL_SECTION;
typedef CRITICAL_SECTION* LPCRITICAL_SECTION;
typedef bg3se::mac::GameSRWKernelLock SRWLOCK;
typedef SRWLOCK* PSRWLOCK;
// The locks of the game's NoesisGUI, which Norbyte's code takes with the SRW
// lock functions, are spin locks on Mac: the functions are overloaded for them.
typedef bg3se::mac::GameNoesisSpinLock* PNOESISLOCK;

// --- Macros ----------------------------------------------------------------

#define WINAPI
#define WINBASEAPI
#define APIENTRY
#define CALLBACK
#define STDMETHODCALLTYPE
#define TRUE 1
#define FALSE 0

#define MAX_PATH 260
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 0x8
#define WC_ERR_INVALID_CHARS 0x80
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INFINITE 0xffffffffu
#define S_OK ((HRESULT)0)
#define RPC_S_OK 0L

#define NO_ERROR 0L
#define ERROR_SUCCESS 0L
#define ERROR_INVALID_FUNCTION 1L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_TOO_MANY_OPEN_FILES 4L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_NOT_ENOUGH_MEMORY 8L
#define ERROR_WRITE_PROTECT 19L
#define ERROR_SHARING_VIOLATION 32L
#define ERROR_HANDLE_DISK_FULL 39L
#define ERROR_NOT_SUPPORTED 50L
#define ERROR_FILE_EXISTS 80L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_INVALID_NAME 123L
#define ERROR_MOD_NOT_FOUND 126L
#define ERROR_PROC_NOT_FOUND 127L
#define ERROR_DIR_NOT_EMPTY 145L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_FILENAME_EXCED_RANGE 206L
#define ERROR_NO_UNICODE_TRANSLATION 1113L
#define ERROR_RESOURCE_DATA_NOT_FOUND 1812L
#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813L
#define ERROR_RESOURCE_NAME_NOT_FOUND 1814L

#define FILE_ATTRIBUTE_READONLY 0x1
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL 0x80
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define TRUNCATE_EXISTING 5

#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE 0x10
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40

#define LMEM_FIXED 0x0
#define LMEM_ZEROINIT 0x40

#define MB_OK 0x0
#define MB_OKCANCEL 0x1
#define MB_ABORTRETRYIGNORE 0x2
#define MB_YESNOCANCEL 0x3
#define MB_YESNO 0x4
#define MB_RETRYCANCEL 0x5
#define MB_CANCELTRYCONTINUE 0x6
#define MB_ICONERROR 0x10
#define MB_ICONQUESTION 0x20
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
#define MB_DEFBUTTON2 0x100
#define MB_DEFBUTTON3 0x200
#define MB_TASKMODAL 0x2000
#define MB_SETFOREGROUND 0x10000
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7
#define IDTRYAGAIN 10
#define IDCONTINUE 11

#define VK_SHIFT 0x10

#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH 2
#define DLL_THREAD_DETACH 3

#define MAKEINTRESOURCEW(i) ((LPCWSTR)(uintptr_t)(WORD)(i))
#define MAKEINTRESOURCE MAKEINTRESOURCEW
#define RT_VERSION MAKEINTRESOURCEW(16)
#define VS_VERSION_INFO 1

// RtlCopyMemory is memcpy.
#define CopyMemory(destination, source, length) memcpy((destination), (source), (length))

#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))

// SAL annotations
#define _In_
#define _In_z_
#define _In_opt_
#define _Out_
#define _Out_opt_
#define _Inout_
#define _COM_Outptr_
#define _Pre_valid_
#define _Pre_writable_size_(x)
#define _Post_ptr_invalid_
#define _Post_writable_size_(x)
#define _Post_writable_byte_size_(x)
#define _Printf_format_string_

// --- Errors (Win32Process.cpp) -----------------------------------------------

// Per thread, as on Windows. The functions below that fail set it.
DWORD GetLastError();
void SetLastError(DWORD error);

// --- Strings (Win32Strings.cpp) -----------------------------------------------

// CP_UTF8 only (any other code page fails with ERROR_INVALID_PARAMETER). Wide
// strings are UTF-32. Invalid input becomes U+FFFD, or fails with
// ERROR_NO_UNICODE_TRANSLATION when MB_ERR_INVALID_CHARS / WC_ERR_INVALID_CHARS
// is given; with a length of -1 the terminating zero is converted too.
int MultiByteToWideChar(UINT codePage, DWORD flags, LPCSTR src, int srcLength, LPWSTR dest, int destLength);
int WideCharToMultiByte(UINT codePage, DWORD flags, LPCWSTR src, int srcLength, LPSTR dest, int destLength,
    LPCSTR defaultChar, BOOL* usedDefaultChar);

// --- Process, threads and time (Win32Process.cpp) -------------------------------

// The pseudo-handles Windows returns: (HANDLE)-1 and (HANDLE)-2.
HANDLE GetCurrentProcess();
HANDLE GetCurrentThread();
DWORD GetCurrentProcessId();
// The current process only; any other handle fails with ERROR_INVALID_HANDLE.
BOOL TerminateProcess(HANDLE process, UINT exitCode);
[[noreturn]] void ExitProcess(UINT exitCode);
// Sleep(0) yields the rest of the time slice.
void Sleep(DWORD milliseconds);
// Milliseconds since boot, sleep included.
ULONGLONG GetTickCount64();
// Nanoseconds of a monotonic clock that does not stop during sleep; the frequency is 10^9.
BOOL QueryPerformanceCounter(LARGE_INTEGER* counter);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency);
BOOL IsDebuggerPresent();
inline void DebugBreak() { __builtin_debugtrap(); }
// A random (version 4) UUID, as UuidCreate makes.
RPC_STATUS UuidCreate(UUID* uuid);
// The command line the process was started with, as one string: the arguments
// separated by spaces, each quoted the way CommandLineToArgvW reads it back
// (an argument that is empty or has a space, a tab or a quote is put in quotes).
LPWSTR GetCommandLineW();
// Nonzero unless every byte of the range can be read. One byte of each page is
// read by the kernel on the caller's behalf, so a page that cannot be read
// fails that call instead of faulting. A range of no bytes is readable.
BOOL IsBadReadPtr(const void* address, UINT_PTR size);
// LMEM_FIXED memory (LMEM_ZEROINIT zeroes it); LocalFree returns NULL on success.
HLOCAL LocalAlloc(UINT flags, SIZE_T bytes);
HLOCAL LocalFree(HLOCAL memory);

// --- Files and paths (Win32Files.cpp) -------------------------------------------
//
// Paths are UTF-32, converted to UTF-8 for the file system; '\' separates
// components as '/' does, as on Windows.

BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES security);
BOOL RemoveDirectoryW(LPCWSTR path);
BOOL DeleteFileW(LPCWSTR path);
BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists);
DWORD GetFileAttributesW(LPCWSTR path);
// The directory for temporary files, with a trailing separator.
DWORD GetTempPathW(DWORD size, LPWSTR buffer);
#define GetFileAttributes GetFileAttributesW

// --- Modules (Win32Modules.cpp) --------------------------------------------------
//
// An HMODULE is the address of a loaded Mach-O image's header (the main
// executable's or a dylib's), as on Windows it is the module's base address.
// Module names are compared without regard to case: a bare name with an
// image's file name, a name with a directory with its whole path. No ".dll"
// is added to a name without an extension. A failed lookup sets
// ERROR_MOD_NOT_FOUND.

// NULL: the main executable.
HMODULE GetModuleHandleW(LPCWSTR moduleName);
HMODULE GetModuleHandleA(LPCSTR moduleName);
// The image's full path (NULL: the main executable's). A buffer too small gets
// what fits, terminated; the result is then `size`, with ERROR_INSUFFICIENT_BUFFER.
DWORD GetModuleFileNameW(HMODULE module, LPWSTR fileName, DWORD size);
// A bare name some loaded image has is that image; anything else is given to
// dlopen, which searches as dyld does. The image keeps a reference until FreeLibrary.
HMODULE LoadLibraryW(LPCWSTR fileName);
// Gives back one reference. An image the process was started with stays loaded.
BOOL FreeLibrary(HMODULE module);
// A symbol the image exports, by its C name (dlsym's naming: "f" finds _f), in
// that image only; ERROR_PROC_NOT_FOUND otherwise. There are no ordinals.
FARPROC GetProcAddress(HMODULE module, LPCSTR procName);
#define GetModuleFileName GetModuleFileNameW
#define LoadLibrary LoadLibraryW
#define FreeModule FreeLibrary

// --- Resources (Win32Resources.cpp) ------------------------------------------------
//
// A module's resources are sections of its Mach-O image: the segment is named
// for the resource type and the section for the resource name, an integer one
// in decimal (MAKEINTRESOURCE(101) of type L"SCRIPT_EXTENDER" is section "101"
// of segment "SCRIPT_EXTENDER"; ld -sectcreate makes one from a file). String
// names are upper case in the image and matched without regard to case.

// NULL: the main executable. Fails with ERROR_RESOURCE_TYPE_NOT_FOUND or
// ERROR_RESOURCE_NAME_NOT_FOUND.
HRSRC FindResourceW(HMODULE module, LPCWSTR name, LPCWSTR type);
DWORD SizeofResource(HMODULE module, HRSRC resource);
// The resource's bytes in the loaded image; LockResource returns the same address.
HGLOBAL LoadResource(HMODULE module, HRSRC resource);
LPVOID LockResource(HGLOBAL data);
// Nothing to free, as in Win32: always 0, which is this function's success value.
BOOL FreeResource(HGLOBAL data);
#define FindResource FindResourceW

// --- Intrinsics -----------------------------------------------------------------

inline unsigned char _BitScanForward(unsigned long* index, uint32_t mask)
{
    if (!mask) return 0;
    *index = (unsigned long)__builtin_ctz(mask);
    return 1;
}
inline unsigned char _BitScanReverse(unsigned long* index, uint32_t mask)
{
    if (!mask) return 0;
    *index = 31 - (unsigned long)__builtin_clz(mask);
    return 1;
}
inline unsigned char _BitScanForward64(unsigned long* index, uint64_t mask)
{
    if (!mask) return 0;
    *index = (unsigned long)__builtin_ctzll(mask);
    return 1;
}
inline unsigned char _BitScanReverse64(unsigned long* index, uint64_t mask)
{
    if (!mask) return 0;
    *index = 63 - (unsigned long)__builtin_clzll(mask);
    return 1;
}
inline uint64_t _mm_popcnt_u64(uint64_t v) { return (uint64_t)__builtin_popcountll(v); }
inline unsigned long _byteswap_ulong(unsigned long v) { return __builtin_bswap32((uint32_t)v); }
inline unsigned short _byteswap_ushort(unsigned short v) { return __builtin_bswap16(v); }
inline uint64_t _byteswap_uint64(uint64_t v) { return __builtin_bswap64(v); }
// With -fms-extensions (as Norbyte's sources are built) clang provides _rotl,
// _rotr, _rotl64 and _rotr64 itself, with MSVC's semantics.
#if !__has_builtin(_rotl)
inline unsigned int _rotl(unsigned int v, int shift) { return __builtin_rotateleft32(v, (uint32_t)shift); }
inline unsigned int _rotr(unsigned int v, int shift) { return __builtin_rotateright32(v, (uint32_t)shift); }
inline uint64_t _rotl64(uint64_t v, int shift) { return __builtin_rotateleft64(v, (uint64_t)shift); }
inline uint64_t _rotr64(uint64_t v, int shift) { return __builtin_rotateright64(v, (uint64_t)shift); }
#endif

// The spin-wait hint: PAUSE on x86-64, YIELD on arm64.
inline void _mm_pause() { __builtin_arm_yield(); }

// Interlocked operations are full barriers and return the previous value, as on Windows.
inline LONG64 InterlockedOr64(LONG64 volatile* p, LONG64 v) { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedExchangeAdd64(LONG64 volatile* p, LONG64 v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

// --- MSVC C runtime ---------------------------------------------------------------
//
// The _s functions (Win32Strings.cpp) behave as MSVC's do with the default
// invalid parameter handler, which Norbyte's code does not replace: an invalid
// argument, or a destination too small for the result, ends the process.
// _TRUNCATE, or a count below the buffer size, asks for the output to be cut
// to fit instead (the function then returns -1, or STRUNCATE for strncpy_s).

#define _TRUNCATE ((size_t)-1)
#define STRUNCATE 80
using std::terminate;

// The locale. Norbyte's extender is linked with MSVC's static C library,
// whose locale is its own and stays "C" whatever the game sets; the macOS
// game sets the locale of its environment for the whole process (see
// Mac/Lua/WindowsConfiguration.h). These functions, and the formatting
// below, use the C locale of this system's library (LC_C_LOCALE).
inline int _stricmp(const char* a, const char* b) { return strcasecmp_l(a, b, LC_C_LOCALE); }
inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp_l(a, b, n, LC_C_LOCALE); }
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { return wcscasecmp_l(a, b, LC_C_LOCALE); }
inline char* _strdup(const char* s) { return strdup(s); }

int vsprintf_s(char* buffer, size_t size, const char* format, va_list args);

namespace bg3se::mac {
// std::to_string of a float or double as MSVC's library writes it in its "C"
// locale ("%f"); libc++'s writes it in the process's.
inline std::string FloatText(double value)
{
    char text[64];
    int n = snprintf_l(text, sizeof(text), LC_C_LOCALE, "%f", value);
    if (n < 0) return {};
    if ((size_t)n < sizeof(text)) return std::string(text, (size_t)n);
    std::string longer((size_t)n + 1, '\0');
    snprintf_l(longer.data(), longer.size(), LC_C_LOCALE, "%f", value);
    longer.resize((size_t)n);
    return longer;
}
}
int _vsnprintf_s(char* buffer, size_t size, size_t count, const char* format, va_list args);
int strcpy_s(char* dest, size_t size, const char* src);
int strncpy_s(char* dest, size_t size, const char* src, size_t count);
int wcscpy_s(wchar_t* dest, size_t size, const wchar_t* src);

inline int sprintf_s(char* buffer, size_t size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int n = vsprintf_s(buffer, size, format, args);
    va_end(args);
    return n;
}
// The array form takes its arguments as a pack: Optick declares a C-variadic
// sprintf_s(char (&)[N], ...) of its own on macOS (optick_common.h), and the
// two must be distinct overloads; this one is the better match for any call
// with arguments.
template <size_t N, class... Args>
inline int sprintf_s(char (&buffer)[N], const char* format, Args... args)
{
    return sprintf_s(buffer, N, format, args...);
}
inline int _snprintf_s(char* buffer, size_t size, size_t count, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int n = _vsnprintf_s(buffer, size, count, format, args);
    va_end(args);
    return n;
}
template <size_t N>
inline int _snprintf_s(char (&buffer)[N], size_t count, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    int n = _vsnprintf_s(buffer, N, count, format, args);
    va_end(args);
    return n;
}
template <size_t N>
inline int strcpy_s(char (&dest)[N], const char* src) { return strcpy_s(dest, N, src); }
template <size_t N>
inline int strncpy_s(char (&dest)[N], const char* src, size_t count) { return strncpy_s(dest, N, src, count); }
template <size_t N>
inline int wcscpy_s(wchar_t (&dest)[N], const wchar_t* src) { return wcscpy_s(dest, N, src); }

inline int gmtime_s(struct tm* result, const time_t* time) { return gmtime_r(time, result) ? 0 : EINVAL; }
inline int localtime_s(struct tm* result, const time_t* time) { return localtime_r(time, result) ? 0 : EINVAL; }

// --- Locks and the thread id (Win32Locks.cpp) -----------------------------------------
// On the locks of the Mac game, taken as the game takes them
// (Layout/GameLockCalls.h). GetCurrentThreadId is the calling thread's Mach
// port, which is what the game writes into the owner words of its locks.
DWORD GetCurrentThreadId();
void InitializeSRWLock(PSRWLOCK lock);
void AcquireSRWLockExclusive(PSRWLOCK lock);
void AcquireSRWLockShared(PSRWLOCK lock);
BOOLEAN TryAcquireSRWLockExclusive(PSRWLOCK lock);
BOOLEAN TryAcquireSRWLockShared(PSRWLOCK lock);
void ReleaseSRWLockExclusive(PSRWLOCK lock);
void ReleaseSRWLockShared(PSRWLOCK lock);
void InitializeSRWLock(PNOESISLOCK lock);
void AcquireSRWLockExclusive(PNOESISLOCK lock);
void AcquireSRWLockShared(PNOESISLOCK lock);
void ReleaseSRWLockExclusive(PNOESISLOCK lock);
void ReleaseSRWLockShared(PNOESISLOCK lock);
void InitializeCriticalSection(LPCRITICAL_SECTION section);
void DeleteCriticalSection(LPCRITICAL_SECTION section);
void EnterCriticalSection(LPCRITICAL_SECTION section);
BOOL TryEnterCriticalSection(LPCRITICAL_SECTION section);
void LeaveCriticalSection(LPCRITICAL_SECTION section);

// --- What DllMain and SetupScriptExtender call (Extender/Shared/dllmain.cpp) ----
//
// DisableThreadLibraryCalls succeeds for a loaded image (a Mach-O image is
// never called for a thread's start or end); NULL fails with
// ERROR_INVALID_HANDLE, anything else with ERROR_MOD_NOT_FOUND.
// GetAsyncKeyState answers for VK_SHIFT, VK_CONTROL and VK_MENU from the
// session's keyboard state (the high bit when down) and 0 for other keys.
// CreateMutexA makes or finds a mutex object by name within this process
// (ERROR_ALREADY_EXISTS when found; names are not shared with other
// processes); the handle is not one that any other function here takes yet.
BOOL DisableThreadLibraryCalls(HMODULE module);
short GetAsyncKeyState(int key);
HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES attributes, BOOL initialOwner, LPCSTR name);

// --- Guarded blocks (Mac/Crash/CrashReporter.h) ------------------------------------
//
// BEGIN_GUARDED() ... END_GUARDED() (GameDefinitions/Base/Base.h) is, on
// Windows, a __try block whose __except filter is HandleGuardedException:
// a fault inside the block is given to the crash reporter. Outside MSVC the
// block is the body of a callable handed to GuardedBlock, which runs it
// through RunGuardedBlock: fn(arg) in one of the platform layer's guarded
// calls (Mac/Platform/FaultGuard.h) with the Mac crash reporter as its
// filter, which Mac/Crash defines.
//
// A block that returns a value from its function does not mean the same as a
// callable's body (Extender/Client/SDL.cpp has one, in a file the Mac build
// leaves out): GuardedBlock refuses it.
namespace bg3se::mac {

void RunGuardedBlock(void (*fn)(void*), void* arg);

template <class F>
inline void GuardedBlock(F&& body)
{
    static_assert(std::is_void_v<std::invoke_result_t<F&>>,
        "a guarded block that returns a value cannot be the body of a callable");
    using Body = std::remove_reference_t<F>;
    RunGuardedBlock([](void* p) { (*static_cast<Body*>(p))(); },
        const_cast<void*>(static_cast<void const*>(std::addressof(body))));
}

// Whether Ext.Debug.Crash, which Norbyte's code runs only in a debug build
// (Lua/Libs/Debug.inl), may fault in this release build: developer mode
// (ScriptExtenderSettings.json) and debug_crash = 1 in bg3mse.conf, both,
// so that a crash report of this build can be seen on purpose. Mac/Crash
// defines it.
bool DebugCrashAllowed();

}

// --- Message boxes (Win32MessageBox.cpp) ----------------------------------------------
//
// A modal alert shown by the system, with the caption, the text and the
// buttons of the type; the call returns the button the user chose (IDOK,
// IDCANCEL, ...) from whatever thread it was made on. The icon is the alert's
// level, MB_DEFBUTTON2 and 3 choose the default button, and the owner and the
// modality flags mean nothing for an alert that is not a window of the
// process. 0 for a type with unknown buttons (ERROR_INVALID_PARAMETER) or when
// the system cannot show an alert (ERROR_NOT_SUPPORTED). Text is UTF-8.
int MessageBoxA(HWND owner, LPCSTR text, LPCSTR caption, UINT type);
int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);

// --- Console (Win32Console.cpp) ------------------------------------------------------
//
// The console is the terminal the process was started from, when standard
// input and standard output are both a terminal; a process started any other
// way has a window as its console when something shows one
// (bg3se::shim::SetWindowConsole, ConsoleScreen.h), and none otherwise.
// AllocConsole says which: TRUE makes that terminal or that window the
// console, FALSE (ERROR_NOT_SUPPORTED) means there is none to have. Without a
// console GetStdHandle gives NULL and every function that needs one fails
// with ERROR_INVALID_HANDLE, at once. FreeConsole gives the terminal back as
// AllocConsole found it.
//
// What follows is a terminal's; a window's is said in Win32Console.cpp.
//
// Colours, the title, wrapping and the character set are escape sequences
// written to the terminal; processed output and the input modes are the
// terminal driver's settings. SetConsoleCtrlHandler takes only NULL (the
// process ignores SIGINT, or no longer does). Only CP_UTF8 is a code page.
// ReadConsoleW gives what is typed as UTF-32 with "\r\n" for a line end,
// and waits for good when the terminal has gone away. There are no font
// functions: a terminal's font is its user's. GetConsoleScreenBufferInfoEx
// reports the terminal's size. freopen_s leaves the standard streams alone
// when asked for "CONOUT$" or "CONIN$": they are the console, or there is
// none.
// OutputDebugStringA writes to the system log at its debug level, which is
// kept only while something watches it, and never to the terminal.

#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)
#define FOREGROUND_BLUE 0x1
#define FOREGROUND_GREEN 0x2
#define FOREGROUND_RED 0x4
#define FOREGROUND_INTENSITY 0x8
#define BACKGROUND_BLUE 0x10
#define BACKGROUND_GREEN 0x20
#define BACKGROUND_RED 0x40
#define BACKGROUND_INTENSITY 0x80
#define COMMON_LVB_REVERSE_VIDEO 0x4000
#define ENABLE_PROCESSED_OUTPUT 0x1
#define ENABLE_WRAP_AT_EOL_OUTPUT 0x2
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x4

typedef struct _COORD { SHORT X; SHORT Y; } COORD;
typedef struct _SMALL_RECT { SHORT Left; SHORT Top; SHORT Right; SHORT Bottom; } SMALL_RECT;
typedef struct _CONSOLE_SCREEN_BUFFER_INFOEX {
    ULONG cbSize;
    COORD dwSize;
    COORD dwCursorPosition;
    WORD wAttributes;
    SMALL_RECT srWindow;
    COORD dwMaximumWindowSize;
    WORD wPopupAttributes;
    BOOL bFullscreenSupported;
    DWORD ColorTable[16];
} CONSOLE_SCREEN_BUFFER_INFOEX;
typedef BOOL (*PHANDLER_ROUTINE)(DWORD ctrlType);

BOOL AllocConsole();
BOOL FreeConsole();
HANDLE GetStdHandle(DWORD stdHandle);
BOOL GetConsoleMode(HANDLE console, DWORD* mode);
BOOL SetConsoleMode(HANDLE console, DWORD mode);
BOOL SetConsoleTextAttribute(HANDLE console, WORD attributes);
BOOL SetConsoleTitleW(LPCWSTR title);
BOOL SetConsoleCtrlHandler(PHANDLER_ROUTINE handler, BOOL add);
BOOL IsValidCodePage(UINT codePage);
BOOL ReadConsoleW(HANDLE console, LPVOID buffer, DWORD charsToRead, LPDWORD charsRead, LPVOID inputControl);
// Not a Win32 function. One line typed at the console, as UTF-8 without its
// line end, read through ReadConsoleW: what the extender's console reads its
// commands with outside MSVC, where it reads std::cin on Windows. On macOS a
// read of std::cin stops the whole process when the game is a background
// job of its shell, and after the end-of-file key every later read of it
// ends at once. False for the end-of-file key on an empty line (what was
// typed before it on the line is dropped) and when there is no console.
namespace bg3se::mac { bool ReadConsoleLine(std::string& line); }
BOOL SetConsoleCP(UINT codePage);
BOOL SetConsoleOutputCP(UINT codePage);
BOOL GetConsoleScreenBufferInfoEx(HANDLE console, CONSOLE_SCREEN_BUFFER_INFOEX* info);
int freopen_s(FILE** result, const char* path, const char* mode, FILE* stream);
void OutputDebugStringA(LPCSTR text);
