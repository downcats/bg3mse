// Tests of the Win32 compatibility layer (Mac/Shim). It links only the shim,
// none of Norbyte's code.

#include <windows.h>
#include <Win32Images.h>
#include <Shlwapi.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <concurrent_queue.h>
#include <concurrent_vector.h>
#include <psapi.h>
#include <ClockText.h>

#include <atomic>
#include <clocale>
#include <cstdio>
#include <dlfcn.h>
#include <functional>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/ldsyms.h>
#include <mach-o/loader.h>
#include <string>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ShimTestsPosix.cpp: a BSD-socket client, kept out of this file's Winsock names.
int TestConnectAndSend(uint16_t port, char const* data, int length);
int TestConnectAndClose(uint16_t port);
int TestConnectAndWaitForClose(uint16_t port);

// ShimTestsConsole.cpp: the console functions, each test in a child process
// on a terminal of the test's. Returns how many tests ran.
size_t RunConsoleTests(int& failures);

// ShimTestsMs.cpp: the _rotl family as clang provides it under -fms-extensions.
bool MsRotateBuiltinsMatch();

namespace {

int gFailures = 0;
char const* gTest = "";

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; name(); } static void name()

std::wstring Wide(std::string const& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(std::wstring const& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// True if `fn` ends the process by abort() (what MSVC's default invalid parameter handler does).
bool Aborts(std::function<void()> fn)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        freopen("/dev/null", "w", stderr);
        fn();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

std::string TempDir()
{
    char path[] = "/tmp/bg3mse-shim-XXXXXX";
    return mkdtemp(path);
}

}

// --- Strings ---------------------------------------------------------------

TEST(utf8_round_trips_every_scalar_value)
{
    std::wstring all;
    for (uint32_t cp = 1; cp <= 0x10FFFF; cp++) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        all.push_back((wchar_t)cp);
    }
    auto utf8 = Narrow(all);
    // 127 one-byte, 1920 two-byte, 61440 three-byte less 2048 surrogates, 1048576 four-byte.
    CHECK(utf8.size() == 127 + 1920 * 2 + (63488 - 2048) * 3 + 1048576 * 4);
    CHECK(Wide(utf8) == all);
}

TEST(utf8_encodes_known_sequences)
{
    CHECK(Narrow(L"Aé€\U0001F600") == "A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    CHECK(Wide("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80") == L"Aé€\U0001F600");
    // One wide character per code point: no surrogate pairs.
    CHECK(Wide("\xF0\x9F\x98\x80").size() == 1);
}

TEST(utf8_invalid_input_becomes_replacement_characters)
{
    // Lone continuation, overlong, surrogate, beyond U+10FFFF, truncated, invalid lead.
    CHECK(Wide("a\x80" "b") == L"a�b");
    CHECK(Wide("\xC0\xAF") == L"��");
    CHECK(Wide("\xED\xA0\x80") == L"���");
    CHECK(Wide("\xF4\x90\x80\x80") == L"����");
    CHECK(Wide("\xE2\x82") == L"�");                  // maximal subpart: one replacement
    CHECK(Wide("\xE2\x82" "A") == L"�A");
    CHECK(Wide("\xFF") == L"�");

    // Wide values that are not scalar values.
    std::wstring bad{ (wchar_t)0xD800, L'x', (wchar_t)0xDFFF, (wchar_t)0x110000 };
    CHECK(Narrow(bad) == "\xEF\xBF\xBDx\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(utf8_strict_flags_reject_invalid_input)
{
    wchar_t w[8];
    SetLastError(0);
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, "a\x80", 2, w, 8) == 0);
    CHECK(GetLastError() == ERROR_NO_UNICODE_TRANSLATION);

    wchar_t bad[] = { (wchar_t)0xDC00, 0 };
    char s[8];
    SetLastError(0);
    CHECK(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, bad, 1, s, 8, nullptr, nullptr) == 0);
    CHECK(GetLastError() == ERROR_NO_UNICODE_TRANSLATION);
}

TEST(utf8_lengths_buffers_and_errors_follow_win32)
{
    wchar_t w[8];
    // -1: the terminating zero is converted and counted.
    CHECK(MultiByteToWideChar(CP_UTF8, 0, "ab", -1, nullptr, 0) == 3);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, "ab", -1, w, 8) == 3 && w[2] == 0);
    char s[8];
    CHECK(WideCharToMultiByte(CP_UTF8, 0, L"é", -1, nullptr, 0, nullptr, nullptr) == 3);

    SetLastError(0);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, "abc", 3, w, 2) == 0);
    CHECK(GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    SetLastError(0);
    CHECK(WideCharToMultiByte(CP_UTF8, 0, L"€", 1, s, 2, nullptr, nullptr) == 0);
    CHECK(GetLastError() == ERROR_INSUFFICIENT_BUFFER);

    SetLastError(0);
    CHECK(MultiByteToWideChar(1252, 0, "a", 1, w, 8) == 0);
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, "a", 0, w, 8) == 0);
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
    BOOL used = FALSE;
    SetLastError(0);
    CHECK(WideCharToMultiByte(CP_UTF8, 0, L"a", 1, s, 8, nullptr, &used) == 0);
    CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
}

TEST(crt_bounded_string_functions)
{
    char buf[8];
    CHECK(sprintf_s(buf, "%d", 1234567) == 7 && strcmp(buf, "1234567") == 0);
    CHECK(Aborts([] { char b[4]; sprintf_s(b, "%d", 1234); }));

    // A count below the buffer size truncates and returns -1.
    CHECK(_snprintf_s(buf, std::size(buf) - 1, "%s", "abcdefghij") == -1 && strcmp(buf, "abcdefg") == 0);
    CHECK(_snprintf_s(buf, 3, "%s", "abcdefghij") == -1 && strcmp(buf, "abc") == 0);
    CHECK(_snprintf_s(buf, _TRUNCATE, "%s", "abcdefghij") == -1 && strcmp(buf, "abcdefg") == 0);
    CHECK(_snprintf_s(buf, std::size(buf), "%d", 42) == 2 && strcmp(buf, "42") == 0);
    CHECK(Aborts([] { char b[4]; _snprintf_s(b, 4, "%s", "abcdef"); }));

    CHECK(strcpy_s(buf, "lz4") == 0 && strcmp(buf, "lz4") == 0);
    CHECK(Aborts([] { char b[4]; strcpy_s(b, "abcd"); }));
    CHECK(strncpy_s(buf, "abcdef", 3) == 0 && strcmp(buf, "abc") == 0);
    CHECK(strncpy_s(buf, "abcdefghij", _TRUNCATE) == STRUNCATE && strcmp(buf, "abcdefg") == 0);
    CHECK(Aborts([] { char b[4]; strncpy_s(b, "abcdef", 5); }));

    wchar_t wbuf[4];
    CHECK(wcscpy_s(wbuf, L"abc") == 0 && wcscmp(wbuf, L"abc") == 0);
    CHECK(Aborts([] { wchar_t b[4]; wcscpy_s(b, L"abcd"); }));

    CHECK(_stricmp("Hello", "hELLO") == 0 && _stricmp("a", "b") < 0);
    CHECK(_wcsicmp(L"Hello", L"hELLO") == 0);
    char* dup = _strdup("copy");
    CHECK(strcmp(dup, "copy") == 0);
    free(dup);

    time_t t = 86400 * 365;
    struct tm tm{};
    CHECK(gmtime_s(&tm, &t) == 0 && tm.tm_year == 71 && tm.tm_yday == 0);
}

// --- Intrinsics ----------------------------------------------------------------

TEST(intrinsics_match_their_x86_definitions)
{
    unsigned long index = 99;
    CHECK(_BitScanForward64(&index, 0) == 0 && index == 99);
    CHECK(_BitScanForward64(&index, 0x8000000000000000ull) == 1 && index == 63);
    CHECK(_BitScanForward64(&index, 0x50) == 1 && index == 4);
    CHECK(_BitScanReverse64(&index, 0x50) == 1 && index == 6);
    CHECK(_BitScanReverse64(&index, 1) == 1 && index == 0);
    CHECK(_BitScanForward(&index, 0x80000000u) == 1 && index == 31);
    CHECK(_BitScanReverse(&index, 0x7fffffffu) == 1 && index == 30);
    CHECK(_BitScanReverse(&index, 0) == 0);

    CHECK(_mm_popcnt_u64(0) == 0 && _mm_popcnt_u64(~0ull) == 64 && _mm_popcnt_u64(0xF0F0) == 8);
    CHECK(_byteswap_ulong(0x11223344ul) == 0x44332211ul);
    CHECK(_byteswap_ushort(0x1122) == 0x2211);
    CHECK(_byteswap_uint64(0x1122334455667788ull) == 0x8877665544332211ull);
    CHECK(_rotl(0x80000001u, 1) == 0x00000003u && _rotl(0x12345678u, 0) == 0x12345678u && _rotl(1u, 33) == 2u);
    CHECK(_rotr(0x00000003u, 1) == 0x80000001u);
    CHECK(MsRotateBuiltinsMatch());
    CHECK(_rotl64(0x8000000000000001ull, 4) == 0x18ull && _rotr64(0x18ull, 4) == 0x8000000000000001ull);

    LONG64 volatile v = 0x10;
    CHECK(InterlockedOr64(&v, 0x3) == 0x10 && v == 0x13);
    CHECK(InterlockedExchangeAdd64(&v, 5) == 0x13 && v == 0x18);

    // Concurrent adds lose nothing.
    LONG64 volatile counter = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; i++) {
        threads.emplace_back([&] { for (int j = 0; j < 100000; j++) InterlockedExchangeAdd64(&counter, 1); });
    }
    for (auto& t : threads) t.join();
    CHECK(counter == 400000);
    _mm_pause();
}

// --- Process, time and errors ---------------------------------------------------

TEST(last_error_is_per_thread)
{
    SetLastError(1234);
    DWORD other = 1;
    std::thread([&] { other = GetLastError(); SetLastError(5); }).join();
    CHECK(other == 0);
    CHECK(GetLastError() == 1234);
}

TEST(process_handles_and_time)
{
    CHECK(GetCurrentProcess() == (HANDLE)(intptr_t)-1);
    CHECK(GetCurrentThread() == (HANDLE)(intptr_t)-2);
    CHECK(GetCurrentProcessId() == (DWORD)getpid());
    SetLastError(0);
    CHECK(TerminateProcess((HANDLE)(intptr_t)1234, 1) == FALSE && GetLastError() == ERROR_INVALID_HANDLE);

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        TerminateProcess(GetCurrentProcess(), 42);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);

    LARGE_INTEGER freq, a, b;
    CHECK(QueryPerformanceFrequency(&freq) && freq.QuadPart == 10000000ll);
    auto t0 = GetTickCount64();
    QueryPerformanceCounter(&a);
    Sleep(30);
    QueryPerformanceCounter(&b);
    auto t1 = GetTickCount64();
    CHECK(b.QuadPart - a.QuadPart >= 300000ll);
    CHECK(t1 - t0 >= 29 && t1 - t0 < 5000);
    Sleep(0);
    CHECK(IsDebuggerPresent() == FALSE || IsDebuggerPresent() == TRUE);

    // A process is traced once it asks to be: a child that is not, and then is.
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        if (IsDebuggerPresent() != FALSE) _exit(1);
        if (ptrace(PT_TRACE_ME, 0, nullptr, 0) != 0) _exit(2);
        _exit(IsDebuggerPresent() == TRUE ? 0 : 3);
    }
    status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

void WriteAtExitMarker()
{
    (void)!write(STDOUT_FILENO, "x", 1);
}

TEST(exit_process_and_debug_break)
{
    // ExitProcess ends the process with the code after running exit handlers (TerminateProcess runs none).
    int fds[2];
    CHECK(pipe(fds) == 0);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        atexit(WriteAtExitMarker);
        ExitProcess(7);
    }
    close(fds[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    char marker = 0;
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    CHECK(read(fds[0], &marker, 1) == 1 && marker == 'x');
    close(fds[0]);

    // DebugBreak raises a breakpoint trap, which ends a process nobody is debugging.
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        DebugBreak();
        _exit(0);
    }
    status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTRAP);
}

TEST(word_macros_and_crt_names)
{
    CHECK(LOWORD(0x12345678u) == 0x5678 && HIWORD(0x12345678u) == 0x1234);
    CHECK(LOWORD(0xffff0000u) == 0 && HIWORD(0xffff0000u) == 0xffff);
    CHECK(MAKEWORD(0x34, 0x12) == 0x1234);

    CHECK(_strnicmp("HELLO world", "hello THERE", 5) == 0 && _strnicmp("abc", "abd", 3) < 0 && _strnicmp("abc", "abd", 2) == 0);

    // localtime_s takes its arguments in MSVC's order and fills the same fields localtime_r does.
    time_t t = 86400 * 365;
    struct tm viaShim{}, viaSystem{};
    CHECK(localtime_s(&viaShim, &t) == 0 && localtime_r(&t, &viaSystem) != nullptr);
    CHECK(viaShim.tm_year == viaSystem.tm_year && viaShim.tm_yday == viaSystem.tm_yday && viaShim.tm_hour == viaSystem.tm_hour);
}

TEST(local_memory)
{
    auto p = static_cast<unsigned char*>(LocalAlloc(LMEM_FIXED | LMEM_ZEROINIT, 64));
    CHECK(p != nullptr);
    bool zero = true;
    for (int i = 0; i < 64; i++) zero = zero && p[i] == 0;
    CHECK(zero);
    CHECK(LocalFree(p) == nullptr);
    SetLastError(0);
    CHECK(LocalAlloc(0x2 /* LMEM_MOVEABLE */, 16) == nullptr && GetLastError() == ERROR_INVALID_PARAMETER);
}

TEST(copy_memory_copies)
{
    char from[8] = "1234567", to[8] = {};
    CopyMemory(to, from, sizeof(from));
    CHECK(memcmp(to, from, sizeof(from)) == 0);
}

TEST(clock_text_is_what_msvc_formats)
{
    using namespace std::chrono;
    using bg3se::mac::ClockText;
    // std::format("{0:%F} {0:%T}", time) with MSVC's library: the date, the
    // time of day in UTC, and the second's fraction in 100 ns ticks.
    system_clock::time_point epoch{};
    CHECK(ClockText(epoch) == "1970-01-01 00:00:00.0000000");
    auto afternoon = sys_days{ 2026y / November / 11 } + 16h + 4min + 27s;
    CHECK(ClockText(afternoon) == "2026-11-11 16:04:27.0000000");
    // A system_clock time of this library is in microseconds: its seventh digit is 0.
    CHECK(ClockText(afternoon + 123456us) == "2026-11-11 16:04:27.1234560");
    CHECK(ClockText(duration_cast<bg3se::mac::ClockTicks>(afternoon.time_since_epoch()) + bg3se::mac::ClockTicks{ 1234567 })
        == "2026-11-11 16:04:27.1234567");
    CHECK(ClockText(afternoon + 1us) == "2026-11-11 16:04:27.0000010");
    CHECK(ClockText(afternoon + 999999us) == "2026-11-11 16:04:27.9999990");
    // The ends of a day, a month and a leap year's February; and a time before 1970.
    CHECK(ClockText(sys_days{ 2024y / February / 29 } + 23h + 59min + 59s) == "2024-02-29 23:59:59.0000000");
    CHECK(ClockText(sys_days{ 2024y / February / 29 } + 24h) == "2024-03-01 00:00:00.0000000");
    CHECK(ClockText(sys_days{ 2026y / January / 1 } + 9h + 5min + 3s) == "2026-01-01 09:05:03.0000000");
    CHECK(ClockText(epoch - 1us) == "1969-12-31 23:59:59.9999990");
    // The time now has the same form.
    auto now = ClockText(system_clock::now());
    CHECK(now.size() == 27 && now[4] == '-' && now[7] == '-' && now[10] == ' ' && now[13] == ':' && now[16] == ':' && now[19] == '.'
        && now.back() == '0');
}

// Ext.Timer.ClockTime's time: MSVC's system clock counts 100 ns, and so must
// the time shown, within the microsecond the system clock reads, for a
// reading taken long after the one before as well.
TEST(clock_time_has_100_ns_ticks_within_the_system_clocks_microsecond)
{
    using namespace std::chrono;
    using bg3se::mac::ClockTicks;

    // From the next change of the system clock: the time that long before it.
    ClockTicks read{ 17'000'000'000'000'000LL };
    ClockTicks next = read + ClockTicks{ 10 };
    CHECK(bg3se::mac::TimeBeforeChange(read, next, 750) == read + ClockTicks{ 2 });     // 7.5 ticks before: in the third
    CHECK(bg3se::mac::TimeBeforeChange(read, next, 800) == read + ClockTicks{ 2 });
    CHECK(bg3se::mac::TimeBeforeChange(read, next, 50) == read + ClockTicks{ 9 });
    CHECK(bg3se::mac::TimeBeforeChange(read, next, 1000) == read);
    // A change seen later than the next microsecond's start, or a clock set
    // in between: held within the microsecond read.
    CHECK(bg3se::mac::TimeBeforeChange(read, next, 5000) == read);
    CHECK(bg3se::mac::TimeBeforeChange(read, read + ClockTicks{ 10'000'000 }, 400) == read + ClockTicks{ 9 });
    CHECK(bg3se::mac::TimeBeforeChange(read, read - ClockTicks{ 10'000'000 }, 400) == read);

    // The real clocks: every time within the microsecond read, and the
    // seventh digit not always the same, for readings close together (a
    // reading waits for the next microsecond: the one after it is taken at
    // a varying time after that) and far apart.
    auto digitsOf = [](int count, milliseconds apart) {
        int digits[10] = {};
        uint64_t wait = 0;
        for (int i = 0; i < count; i++) {
            if (apart.count()) {
                std::this_thread::sleep_for(apart);
            } else {
                wait = (wait * 7 + 130) % 1000;
                for (uint64_t start = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW); clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - start < wait;) {}
            }
            auto system = system_clock::now();
            auto precise = bg3se::mac::PreciseSystemTime(system);
            auto read = duration_cast<ClockTicks>(system.time_since_epoch());
            CHECK(precise >= read && precise < read + microseconds{ 1 });
            digits[precise.count() % 10]++;
        }
        int seen = 0;
        for (int n : digits) seen += n > 0;
        return seen;
    };
    CHECK(digitsOf(20000, milliseconds{ 0 }) >= 5);
    CHECK(digitsOf(40, milliseconds{ 5 }) >= 5);
}

// The formatting and comparisons of MSVC's static C library, in its "C"
// locale whatever the process's is (the game sets its environment's).
TEST(formatting_and_comparisons_ignore_the_process_locale)
{
    for (char const* name : { "C", "de_DE.UTF-8", "ru_RU.UTF-8", "tr_TR.UTF-8", "fr_FR.ISO8859-1" }) {
        CHECK(setlocale(LC_ALL, name) != nullptr);
        if (strcmp(name, "C") != 0) CHECK(localeconv()->decimal_point[0] == ',');
        char buf[64];
        CHECK(sprintf_s(buf, "%g,%g|%f", 0.5, 1.25, 2.0) > 0 && strcmp(buf, "0.5,1.25|2.000000") == 0);
        CHECK(_snprintf_s(buf, sizeof(buf), _TRUNCATE, "(%f)", -3.5) > 0 && strcmp(buf, "(-3.500000)") == 0);
        CHECK(bg3se::mac::FloatText(0.25) == "0.250000" && bg3se::mac::FloatText(-1e20) == "-100000000000000000000.000000");
        CHECK(bg3se::mac::FloatText(1e300).size() == 301 + 7);
        // ASCII only: "\xC3\x89" (É) and "\xC3\xA9" (é) differ.
        CHECK(_stricmp("\xC3\x89", "\xC3\xA9") != 0 && _stricmp("\xC3\x89", "\xE3\x89") != 0 && _stricmp("ABC", "abc") == 0);
        CHECK(_strnicmp("\xC3\x89x", "\xE3\x89y", 2) != 0 && _strnicmp("Ix", "iy", 1) == 0);
        CHECK(_wcsicmp(L"\u00C9", L"\u00E9") != 0 && _wcsicmp(L"I", L"i") == 0);
    }
    setlocale(LC_ALL, "C");
}

TEST(uuids_are_random_version_4)
{
    UUID a, b;
    CHECK(UuidCreate(&a) == RPC_S_OK && UuidCreate(&b) == RPC_S_OK);
    CHECK(memcmp(&a, &b, sizeof(a)) != 0);
    CHECK((a.Data3 >> 12) == 4 && (a.Data4[0] & 0xC0) == 0x80);
}

// --- Files -----------------------------------------------------------------------

TEST(files_and_directories)
{
    auto dir = TempDir();
    auto wdir = Wide(dir);
    auto sub = wdir + L"\\sub é";      // a backslash separates, as on Windows
    CHECK(CreateDirectoryW(sub.c_str(), nullptr) == TRUE);
    struct stat st;
    CHECK(stat((dir + "/sub \xC3\xA9").c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    SetLastError(0);
    CHECK(CreateDirectoryW(sub.c_str(), nullptr) == FALSE && GetLastError() == ERROR_ALREADY_EXISTS);
    SetLastError(0);
    CHECK(CreateDirectoryW((wdir + L"/missing/x").c_str(), nullptr) == FALSE && GetLastError() == ERROR_PATH_NOT_FOUND);

    CHECK(PathIsDirectoryW(sub.c_str()) == FILE_ATTRIBUTE_DIRECTORY);
    CHECK(PathFileExistsW(sub.c_str()) == TRUE);
    CHECK(GetFileAttributesW(sub.c_str()) == FILE_ATTRIBUTE_DIRECTORY);

    auto file = sub + L"/a.txt";
    FILE* f = fopen((dir + "/sub \xC3\xA9/a.txt").c_str(), "w");
    fputs("data", f);
    fclose(f);
    CHECK(PathFileExistsW(file.c_str()) == TRUE);
    CHECK(PathIsDirectoryW(file.c_str()) == FALSE);
    CHECK(GetFileAttributesW(file.c_str()) == FILE_ATTRIBUTE_NORMAL);

    auto copy = sub + L"/b.txt";
    CHECK(CopyFileW(file.c_str(), copy.c_str(), TRUE) == TRUE);
    SetLastError(0);
    CHECK(CopyFileW(file.c_str(), copy.c_str(), TRUE) == FALSE && GetLastError() == ERROR_FILE_EXISTS);
    CHECK(CopyFileW(file.c_str(), copy.c_str(), FALSE) == TRUE);
    SetLastError(0);
    CHECK(CopyFileW((sub + L"/none").c_str(), copy.c_str(), FALSE) == FALSE && GetLastError() == ERROR_FILE_NOT_FOUND);

    SetLastError(0);
    CHECK(RemoveDirectoryW(sub.c_str()) == FALSE && GetLastError() == ERROR_DIR_NOT_EMPTY);
    SetLastError(0);
    CHECK(DeleteFileW(sub.c_str()) == FALSE && GetLastError() == ERROR_ACCESS_DENIED);
    CHECK(DeleteFileW(file.c_str()) == TRUE && DeleteFileW(copy.c_str()) == TRUE);
    SetLastError(0);
    CHECK(DeleteFileW(file.c_str()) == FALSE && GetLastError() == ERROR_FILE_NOT_FOUND);
    CHECK(PathFileExistsW(file.c_str()) == FALSE);
    CHECK(GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(RemoveDirectoryW(sub.c_str()) == TRUE);
    CHECK(RemoveDirectoryW(wdir.c_str()) == TRUE);

    wchar_t temp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp);
    CHECK(n > 0 && n < MAX_PATH && temp[n - 1] == L'/' && temp[n] == 0);
    CHECK(PathIsDirectoryW(temp));
    CHECK(GetTempPathW(1, temp) == n + 1);
}

// --- Sockets ---------------------------------------------------------------------

TEST(sockets_serve_a_loopback_connection)
{
    SOCKET early = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(early == INVALID_SOCKET && WSAGetLastError() == WSANOTINITIALISED);

    WSADATA data;
    CHECK(WSAStartup(MAKEWORD(2, 2), &data) == 0 && data.wVersion == MAKEWORD(2, 2));

    uint32_t ip = 0;
    CHECK(inet_pton(AF_INET, "127.0.0.1", &ip) == 1 && ip == htonl(0x7f000001));
    CHECK(inet_pton(AF_INET, "not an address", &ip) == 0);

    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(server != INVALID_SOCKET);
    // Find a free port from a fixed range.
    uint16_t port = 0;
    for (uint16_t p = 47100; p < 47200 && port == 0; p++) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.S_un.S_addr = ip;
        addr.sin_port = htons(p);
        if (bind(server, (sockaddr*)&addr, sizeof(addr)) == 0) port = p;
        else CHECK(WSAGetLastError() == WSAEADDRINUSE);
    }
    CHECK(port != 0);
    CHECK(listen(server, 30) == 0);

    std::thread client([&] { TestConnectAndSend(port, "hello", 5); });
    sockaddr_in peer{};
    int peerLength = sizeof(peer);
    SOCKET conn = accept(server, (sockaddr*)&peer, &peerLength);
    CHECK(conn != INVALID_SOCKET && peer.sin_family == AF_INET && peer.sin_addr.S_un.S_addr == ip);
    char buf[16];
    int total = 0;
    for (int n; total < 5 && (n = recv(conn, buf + total, sizeof(buf) - total, 0)) > 0;) total += n;
    client.join();
    CHECK(total == 5 && memcmp(buf, "hello", 5) == 0);
    CHECK(recv(conn, buf, sizeof(buf), 0) == 0);     // orderly close
    CHECK(closesocket(conn) == 0);

    // Sending to a peer that has gone fails with an error, not SIGPIPE.
    std::thread closer([&] { TestConnectAndClose(port); });
    conn = accept(server, nullptr, nullptr);
    closer.join();
    CHECK(conn != INVALID_SOCKET);
    int result = 0;
    for (int i = 0; i < 100 && result != SOCKET_ERROR; i++) {
        result = send(conn, "x", 1, 0);
        Sleep(1);
    }
    CHECK(result == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAECONNRESET);
    CHECK(closesocket(conn) == 0);

    CHECK(closesocket(server) == 0);
    CHECK(closesocket(server) == SOCKET_ERROR && WSAGetLastError() == WSAEBADF);
    CHECK(send(INVALID_SOCKET, "x", 1, 0) == SOCKET_ERROR && WSAGetLastError() == WSAENOTSOCK);
    CHECK(WSACleanup() == 0);
}

// What Norbyte's debugger servers (Osiris/Debugger/DebugInterface.cpp) take
// from Winsock beyond a connection: a game started again right after a
// session binds the port its last run listened on, and the thread waiting in
// accept is ended by closing the socket under it (SocketInterface::Shutdown).
TEST(sockets_free_a_listeners_port_and_its_accept_when_it_is_closed)
{
    WSADATA data;
    CHECK(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    uint32_t ip = 0;
    CHECK(inet_pton(AF_INET, "127.0.0.1", &ip) == 1);

    int refused = 0;
    auto listener = [&](uint16_t port) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.S_un.S_addr = ip;
        addr.sin_port = htons(port);
        if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, 30) != 0) {
            refused = WSAGetLastError();
            closesocket(s);
            return INVALID_SOCKET;
        }
        return s;
    };
    SOCKET server = INVALID_SOCKET;
    uint16_t port = 0;
    for (uint16_t p = 47200; p < 47300 && server == INVALID_SOCKET; p++) {
        server = listener(p);
        port = p;
    }
    CHECK(server != INVALID_SOCKET);

    // While it listens nothing else binds its port.
    CHECK(listener(port) == INVALID_SOCKET && refused == WSAEADDRINUSE);

    // A connection the server closes first, then the listener: the port is
    // bound again at once.
    std::thread client([&] { TestConnectAndWaitForClose(port); });
    SOCKET conn = accept(server, nullptr, nullptr);
    CHECK(conn != INVALID_SOCKET);
    CHECK(closesocket(conn) == 0);
    client.join();
    CHECK(closesocket(server) == 0);
    server = listener(port);
    CHECK(server != INVALID_SOCKET);

    // A thread in accept comes back with no socket when the listener is closed.
    std::atomic<int> accepted{ 0 };
    std::thread waiting([&] { accepted = accept(server, nullptr, nullptr) == INVALID_SOCKET ? 1 : 2; });
    Sleep(200);
    CHECK(accepted == 0);
    CHECK(closesocket(server) == 0);
    for (int i = 0; i < 500 && accepted == 0; i++) Sleep(10);
    CHECK(accepted == 1);
    if (accepted != 0) {
        waiting.join();
    } else {
        waiting.detach();
    }
    CHECK(WSACleanup() == 0);
}

// --- Concurrency containers ---------------------------------------------------------

TEST(concurrent_queue_is_fifo_and_loses_nothing_across_threads)
{
    concurrency::concurrent_queue<int> q;
    int v = -1;
    CHECK(q.empty() && !q.try_pop(v) && v == -1);
    q.push(1);
    q.push(2);
    CHECK(!q.empty() && q.unsafe_size() == 2);
    CHECK(q.try_pop(v) && v == 1 && q.try_pop(v) && v == 2 && !q.try_pop(v));

    concurrency::concurrent_queue<std::function<int()>> tasks;
    tasks.push([] { return 7; });
    std::function<int()> task;
    CHECK(tasks.try_pop(task) && task() == 7);

    constexpr int Producers = 4, PerProducer = 20000;
    std::atomic<long> sum{ 0 };
    std::atomic<int> popped{ 0 };
    std::vector<std::thread> threads;
    for (int p = 0; p < Producers; p++) {
        threads.emplace_back([&, p] { for (int i = 0; i < PerProducer; i++) q.push(p * PerProducer + i); });
    }
    for (int c = 0; c < 2; c++) {
        threads.emplace_back([&] {
            int x;
            while (popped.load() < Producers * PerProducer) {
                if (q.try_pop(x)) { sum += x; popped++; }
            }
        });
    }
    for (auto& t : threads) t.join();
    long n = (long)Producers * PerProducer;
    CHECK(popped == n && sum == n * (n - 1) / 2 && q.empty());

    q.push(5);
    q.clear();
    CHECK(q.empty());
}

// A queue may be declared while its element type is only forward-declared.
struct QueuedLater;
struct HoldsQueue
{
    concurrency::concurrent_queue<QueuedLater> Queue;
    void Push(int value);
    bool Pop(int& value);
};
int gQueuedAlive = 0;
struct QueuedLater
{
    explicit QueuedLater(int value = 0) : Value(value) { gQueuedAlive++; }
    QueuedLater(QueuedLater const& o) : Value(o.Value) { gQueuedAlive++; }
    QueuedLater& operator=(QueuedLater const&) = default;
    ~QueuedLater() { gQueuedAlive--; }
    int Value;
};
void HoldsQueue::Push(int value) { Queue.push(QueuedLater(value)); }
bool HoldsQueue::Pop(int& value)
{
    QueuedLater item;
    if (!Queue.try_pop(item)) return false;
    value = item.Value;
    return true;
}

TEST(concurrent_queue_takes_an_incomplete_element_type_and_destroys_every_element)
{
    {
        HoldsQueue h;
        for (int i = 0; i < 10; i++) h.Push(i);
        CHECK(gQueuedAlive == 10 && h.Queue.unsafe_size() == 10);
        int v = -1;
        CHECK(h.Pop(v) && v == 0 && h.Pop(v) && v == 1);
        CHECK(gQueuedAlive == 8);
        h.Queue.clear();
        CHECK(gQueuedAlive == 0 && h.Queue.empty() && !h.Pop(v));
        h.Push(42);
        h.Push(43);
        CHECK(h.Pop(v) && v == 42 && gQueuedAlive == 1);
    }
    // The destructor destroys what is left.
    CHECK(gQueuedAlive == 0);
}

TEST(concurrent_vector_keeps_elements_in_place_while_growing)
{
    concurrency::concurrent_vector<std::string> v;
    v.reserve(0x1000);
    CHECK(v.size() == 0 && v.begin() == v.end());
    auto it = v.push_back("first");
    CHECK(it - v.begin() == 0 && *it == "first");
    std::string* first = &v[0];
    for (int i = 1; i < 5000; i++) {
        auto at = v.push_back(std::to_string(i));
        CHECK(at - v.begin() == i);
    }
    CHECK(v.size() == 5000 && &v[0] == first && v[4999] == "4999");
    size_t count = 0;
    for (auto& s : v) { (void)s; count++; }
    CHECK(count == 5000);

    // Appends from several threads while another reads what is there.
    concurrency::concurrent_vector<int> c;
    std::atomic<bool> done{ false };
    std::atomic<bool> readOk{ true };
    std::thread reader([&] {
        while (!done) {
            size_t n = c.size();
            for (size_t i = 0; i < n; i++) {
                if (c[i] < 0) readOk = false;
            }
        }
    });
    std::vector<std::thread> writers;
    for (int w = 0; w < 4; w++) {
        writers.emplace_back([&] { for (int i = 0; i < 20000; i++) c.push_back(i); });
    }
    for (auto& t : writers) t.join();
    done = true;
    reader.join();
    CHECK(readOk && c.size() == 80000);
    long sum = 0;
    for (size_t i = 0; i < c.size(); i++) sum += c[i];
    CHECK(sum == 4L * (20000L * 19999L / 2));
}

// --- Modules -------------------------------------------------------------------

// An export of this executable, for GetProcAddress.
extern "C" __attribute__((visibility("default"), used)) int bg3mse_shim_test_export() { return 77; }
// The address of main, which C++ does not let a program take by that name.
extern "C" int AddressOfMain() __asm__("_main");

namespace {

std::string RealPath(std::string const& path)
{
    char resolved[PATH_MAX];
    return realpath(path.c_str(), resolved) ? resolved : "";
}

// GetProcAddress as a data pointer, to compare and to convert to the real type.
void* Export(HMODULE module, char const* name)
{
    return reinterpret_cast<void*>(GetProcAddress(module, name));
}

void* const kOwnExport = reinterpret_cast<void*>(&bg3mse_shim_test_export);

bool Inside(MODULEINFO const& info, void const* address)
{
    auto base = (uint8_t const*)info.lpBaseOfDll;
    return (uint8_t const*)address >= base && (uint8_t const*)address < base + info.SizeOfImage;
}

}

TEST(modules_are_loaded_mach_o_images)
{
    // The main executable: NULL, or its file name in any case.
    HMODULE exe = GetModuleHandleW(nullptr);
    CHECK(exe == (HMODULE)&_mh_execute_header);
    CHECK(GetModuleHandleA(nullptr) == exe);
    CHECK(GetModuleHandleW(L"bg3mse_shim_tests") == exe);
    CHECK(GetModuleHandleW(L"BG3MSE_Shim_Tests") == exe);
    CHECK(GetModuleHandleA("bg3mse_shim_tests") == exe);
    // A name no image has, such as the Windows game's.
    SetLastError(0);
    CHECK(GetModuleHandleW(L"bg3.exe") == nullptr && GetLastError() == ERROR_MOD_NOT_FOUND);
    SetLastError(0);
    CHECK(GetModuleHandleA("DWrite.dll") == nullptr && GetLastError() == ERROR_MOD_NOT_FOUND);
    // No ".dll" or other extension is assumed.
    CHECK(GetModuleHandleW(L"libSystem.B") == nullptr);
    // A system library by file name and by path, with either separator.
    HMODULE sys = GetModuleHandleW(L"libSystem.B.dylib");
    CHECK(sys != nullptr && sys != exe);
    CHECK(sys && ((mach_header_64 const*)sys)->magic == MH_MAGIC_64);
    CHECK(GetModuleHandleW(L"/usr/lib/libSystem.B.dylib") == sys);
    CHECK(GetModuleHandleA("\\usr\\lib\\libsystem.b.dylib") == sys);

    // The executable's path: fully qualified, and the file that is running.
    wchar_t path[2048];
    DWORD length = GetModuleFileNameW(nullptr, path, (DWORD)std::size(path));
    CHECK(length > 0 && length < std::size(path) && path[length] == 0 && wcslen(path) == length);
    CHECK(path[0] == L'/');
    char own[PATH_MAX];
    uint32_t ownSize = sizeof(own);
    CHECK(_NSGetExecutablePath(own, &ownSize) == 0);
    CHECK(RealPath(Narrow(path)) == RealPath(own));
    wchar_t byHandle[2048];
    CHECK(GetModuleFileNameW(exe, byHandle, (DWORD)std::size(byHandle)) == length && wcscmp(byHandle, path) == 0);
    // A buffer too small: what fits, terminated; the buffer size; an error.
    wchar_t small[8];
    wmemset(small, L'x', std::size(small));
    SetLastError(0);
    CHECK(GetModuleFileNameW(nullptr, small, 8) == 8 && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    CHECK(small[7] == 0 && wcsncmp(small, path, 7) == 0);
    // A buffer of exactly the length has no room for the terminator.
    std::wstring exact(length, L'x');
    SetLastError(0);
    CHECK(GetModuleFileNameW(nullptr, exact.data(), length) == length && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    // Not a module.
    int notAModule = 0;
    SetLastError(0);
    CHECK(GetModuleFileNameW((HMODULE)&notAModule, path, (DWORD)std::size(path)) == 0 && GetLastError() == ERROR_MOD_NOT_FOUND);

    // Exports of the executable and of a system library.
    CHECK(Export(exe, "bg3mse_shim_test_export") == kOwnExport);
    CHECK(Export(nullptr, "bg3mse_shim_test_export") == kOwnExport);
    HMODULE kernel = GetModuleHandleW(L"libsystem_kernel.dylib");
    CHECK(kernel != nullptr);
    CHECK(Export(kernel, "getpid") == dlsym(RTLD_DEFAULT, "getpid"));
    // ... looked up in that image only.
    SetLastError(0);
    CHECK(GetProcAddress(exe, "getpid") == nullptr && GetLastError() == ERROR_PROC_NOT_FOUND);

    // A library that is not loaded yet.
    std::wstring library = Wide(BG3MSE_TEST_FUNCTIONS);
    CHECK(GetModuleHandleW(L"libbg3mse_test_functions.dylib") == nullptr);
    HMODULE module = LoadLibraryW(library.c_str());
    CHECK(module != nullptr);
    if (!module) return;
    auto header = (mach_header_64 const*)module;
    CHECK(header->magic == MH_MAGIC_64 && header->filetype == MH_DYLIB);
    CHECK(GetModuleHandleW(L"libbg3mse_test_functions.dylib") == module);
    CHECK(GetModuleHandleW(library.c_str()) == module);
    CHECK(GetModuleFileNameW(module, path, (DWORD)std::size(path)) > 0);
    CHECK(RealPath(Narrow(path)) == RealPath(BG3MSE_TEST_FUNCTIONS));

    auto addOne = reinterpret_cast<long (*)(long)>(Export(module, "lib_add_one"));
    CHECK(addOne != nullptr && addOne(41) == 42);
    auto data = static_cast<uint64_t const*>(Export(module, "lib_data"));
    CHECK(data != nullptr && *data == 5);
    // A symbol the library has and does not export; another image's export; an ordinal.
    SetLastError(0);
    CHECK(GetProcAddress(module, "lib_local") == nullptr && GetLastError() == ERROR_PROC_NOT_FOUND);
    CHECK(GetProcAddress(module, "getpid") == nullptr);
    CHECK(GetProcAddress(module, (LPCSTR)1) == nullptr);

    // Its range holds its code and data and nothing of the executable's.
    MODULEINFO info{};
    CHECK(GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)));
    CHECK(info.lpBaseOfDll == module && info.EntryPoint == nullptr);
    CHECK(info.SizeOfImage > 0 && info.SizeOfImage % (DWORD)getpagesize() == 0);
    CHECK(Inside(info, Export(module, "lib_add_one")) && Inside(info, data) && !Inside(info, kOwnExport));
    MODULEINFO exeInfo{};
    CHECK(GetModuleInformation(GetCurrentProcess(), exe, &exeInfo, sizeof(exeInfo)));
    CHECK(exeInfo.lpBaseOfDll == exe && exeInfo.EntryPoint == reinterpret_cast<void*>(&AddressOfMain));
    CHECK(Inside(exeInfo, kOwnExport) && Inside(exeInfo, &gFailures) && !Inside(exeInfo, data));
    SetLastError(0);
    CHECK(!GetModuleInformation((HANDLE)(intptr_t)123, module, &info, sizeof(info)) && GetLastError() == ERROR_INVALID_HANDLE);
    SetLastError(0);
    CHECK(!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info) - 1) && GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    SetLastError(0);
    CHECK(!GetModuleInformation(GetCurrentProcess(), &notAModule, &info, sizeof(info)) && GetLastError() == ERROR_INVALID_HANDLE);
    // A library of the dyld shared cache is not one range.
    SetLastError(0);
    CHECK(!GetModuleInformation(GetCurrentProcess(), sys, &info, sizeof(info)) && GetLastError() == ERROR_NOT_SUPPORTED);

    // A second load, by bare name, is the same module with a second reference.
    CHECK(LoadLibraryW(L"LIBBG3MSE_TEST_FUNCTIONS.dylib") == module);
    CHECK(FreeLibrary(module));
    CHECK(GetModuleHandleW(library.c_str()) == module);
    CHECK(addOne(1) == 2);
    CHECK(FreeLibrary(module));
    CHECK(GetModuleHandleW(library.c_str()) == nullptr);
    SetLastError(0);
    CHECK(!FreeLibrary(module) && GetLastError() == ERROR_MOD_NOT_FOUND);
    SetLastError(0);
    CHECK(!FreeLibrary(nullptr) && GetLastError() == ERROR_MOD_NOT_FOUND);

    // What the process was started with stays, whatever is freed.
    CHECK(LoadLibraryW(L"bg3mse_shim_tests") == exe);
    CHECK(FreeLibrary(exe) && GetModuleHandleW(nullptr) == exe);
    CHECK(LoadLibraryW(L"libSystem.B.dylib") == sys);
    CHECK(FreeLibrary(sys) && FreeLibrary(sys) && GetModuleHandleW(L"libSystem.B.dylib") == sys);

    SetLastError(0);
    CHECK(LoadLibraryW(L"/nonexistent/library.dylib") == nullptr && GetLastError() == ERROR_MOD_NOT_FOUND);
    SetLastError(0);
    CHECK(LoadLibraryW(L"") == nullptr && GetLastError() == ERROR_INVALID_PARAMETER);
}

// --- Resources -----------------------------------------------------------------

namespace {

// The bytes of a resource of `module`, or "<error N>".
std::string Resource(HMODULE module, LPCWSTR name, LPCWSTR type)
{
    SetLastError(0);
    HRSRC resource = FindResourceW(module, name, type);
    if (!resource) return "<error " + std::to_string(GetLastError()) + ">";
    HGLOBAL loaded = LoadResource(module, resource);
    auto data = (char const*)LockResource(loaded);
    if (!data) return "<not loaded>";
    return std::string(data, SizeofResource(module, resource));
}

}

TEST(resources_are_sections_of_the_image)
{
    // The executable has three (CMakeLists.txt): by integer name, by string
    // name in any case, and of an integer type (RT_VERSION is 16).
    CHECK(Resource(nullptr, MAKEINTRESOURCE(7), L"SHIM_TEST") == "resource seven\n");
    CHECK(Resource(nullptr, L"#7", L"SHIM_TEST") == "resource seven\n");
    CHECK(Resource(nullptr, L"NAMED", L"SHIM_TEST") == "a resource with a name");
    CHECK(Resource(nullptr, L"named", L"shim_test") == "a resource with a name");
    CHECK(Resource(nullptr, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION) == "of an integer type");
    CHECK(Resource(GetModuleHandleW(nullptr), MAKEINTRESOURCE(7), L"SHIM_TEST") == "resource seven\n");

    // The data is inside the image, and stays where it is.
    HRSRC seven = FindResourceW(nullptr, MAKEINTRESOURCE(7), L"SHIM_TEST");
    CHECK(seven != nullptr && SizeofResource(nullptr, seven) == 15);
    HGLOBAL loaded = LoadResource(nullptr, seven);
    MODULEINFO info{};
    CHECK(GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &info, sizeof(info)));
    CHECK(Inside(info, loaded) && LockResource(loaded) == loaded);
    CHECK(LoadResource(nullptr, seven) == loaded);
    CHECK(FreeResource(loaded) == FALSE);
    CHECK(memcmp(LockResource(loaded), "resource seven\n", 15) == 0);

    // What is not there: the name, the type, a name no section can have.
    CHECK(Resource(nullptr, MAKEINTRESOURCE(8), L"SHIM_TEST") == "<error 1814>");
    CHECK(Resource(nullptr, L"OTHER", L"SHIM_TEST") == "<error 1814>");
    CHECK(Resource(nullptr, MAKEINTRESOURCE(7), L"SCRIPT_EXTENDER") == "<error 1813>");
    CHECK(Resource(nullptr, MAKEINTRESOURCE(7), MAKEINTRESOURCE(17)) == "<error 1813>");
    CHECK(Resource(nullptr, L"A_NAME_OF_MORE_THAN_16", L"SHIM_TEST") == "<error 1814>");
    CHECK(Resource(nullptr, L"#x", L"SHIM_TEST") == "<error 1814>");
    // A module without resources, and something that is no module.
    HMODULE sys = GetModuleHandleW(L"libSystem.B.dylib");
    CHECK(Resource(sys, MAKEINTRESOURCE(7), L"SHIM_TEST") == "<error 1813>");
    int notAModule = 0;
    CHECK(Resource((HMODULE)&notAModule, MAKEINTRESOURCE(7), L"SHIM_TEST") == "<error 126>");
    // A resource handle belongs to its module.
    SetLastError(0);
    CHECK(SizeofResource(sys, seven) == 0 && GetLastError() == ERROR_INVALID_HANDLE);
    SetLastError(0);
    CHECK(LoadResource(sys, seven) == nullptr && GetLastError() == ERROR_INVALID_HANDLE);
    SetLastError(0);
    CHECK(LoadResource(nullptr, (HRSRC)&notAModule) == nullptr && GetLastError() == ERROR_INVALID_HANDLE);
}

// --- Memory probing --------------------------------------------------------------

TEST(is_bad_read_ptr_asks_without_faulting)
{
    size_t const page = (size_t)getpagesize();
    // Three pages: readable and writable, no access, read-only.
    auto base = (uint8_t*)mmap(nullptr, 3 * page, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(base != MAP_FAILED);
    if (base == MAP_FAILED) return;
    memset(base, 0x5a, 3 * page);
    CHECK(mprotect(base + page, page, PROT_NONE) == 0);
    CHECK(mprotect(base + 2 * page, page, PROT_READ) == 0);

    CHECK(!IsBadReadPtr(base, 1) && !IsBadReadPtr(base, page) && !IsBadReadPtr(base + page - 1, 1));
    CHECK(!IsBadReadPtr(base + 2 * page, page) && !IsBadReadPtr(base + 3 * page - 1, 1));
    // The page without access, alone and as any part of a range.
    CHECK(IsBadReadPtr(base + page, 1) && IsBadReadPtr(base + 2 * page - 1, 1));
    CHECK(IsBadReadPtr(base, page + 1) && IsBadReadPtr(base + page - 1, 2));
    CHECK(IsBadReadPtr(base, 3 * page) && IsBadReadPtr(base + 2 * page - 1, 2));
    // Nothing was written by asking.
    CHECK(base[0] == 0x5a && base[page - 1] == 0x5a && base[2 * page] == 0x5a);

    // No bytes at all can always be read; a null pointer never.
    CHECK(!IsBadReadPtr(nullptr, 0) && !IsBadReadPtr(base + page, 0));
    CHECK(IsBadReadPtr(nullptr, 1));
    CHECK(IsBadReadPtr((void const*)0x10, 8));
    // A range that runs past the end of the address space.
    CHECK(IsBadReadPtr(base, ~(UINT_PTR)0));
    CHECK(IsBadReadPtr((void const*)~(uintptr_t)0, 2));

    // Ordinary memory of each kind, and code.
    int onStack = 1;
    static int const constant = 2;
    CHECK(!IsBadReadPtr(&onStack, sizeof(onStack)) && !IsBadReadPtr(&constant, sizeof(constant)));
    CHECK(!IsBadReadPtr(&gFailures, sizeof(gFailures)));
    CHECK(!IsBadReadPtr(reinterpret_cast<void const*>(&getpid), 4));

    // Memory that has been given back.
    CHECK(munmap(base, 3 * page) == 0);
    CHECK(IsBadReadPtr(base, 1) && IsBadReadPtr(base + 2 * page, 1));
}

// --- The command line ---------------------------------------------------------

namespace {

// What GetCommandLineW gives a process started with these arguments: this
// executable is run again as `arguments[0]`, to print it (see main).
std::string CommandLineOf(std::vector<std::string> arguments)
{
    char own[PATH_MAX];
    uint32_t ownSize = sizeof(own);
    if (_NSGetExecutablePath(own, &ownSize) != 0) return "<no path>";
    std::vector<char*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    int fds[2];
    if (pipe(fds) != 0) return "<no pipe>";
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        execv(own, argv.data());
        _exit(127);
    }
    close(fds[1]);
    std::string output;
    char buffer[256];
    for (ssize_t n; (n = read(fds[0], buffer, sizeof(buffer))) > 0;) output.append(buffer, (size_t)n);
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? output : "<failed>";
}

char const* const kPrintCommandLine = "--print-command-line";

}

TEST(command_line_is_the_arguments_quoted_as_windows_reads_them)
{
    // The same string every time, starting with this program's own name.
    LPWSTR line = GetCommandLineW();
    CHECK(line != nullptr && line == GetCommandLineW());
    CHECK(Narrow(line).find("bg3mse_shim_tests") != std::string::npos);

    CHECK(CommandLineOf({ "program", kPrintCommandLine }) == "program --print-command-line");
    // An argument with a space or a tab, and an empty one, are quoted.
    CHECK(CommandLineOf({ "/Applications/A Game.app/Contents/MacOS/A Game", kPrintCommandLine, "--flag", "two words", "", "tab\there" })
        == "\"/Applications/A Game.app/Contents/MacOS/A Game\" --print-command-line --flag \"two words\" \"\" \"tab\there\"");
    // Quotes are escaped; backslashes are doubled only before a quote.
    CHECK(CommandLineOf({ "p", kPrintCommandLine, "say \"hi\"", "back\\slash", "ends with\\", "a\\\"b" })
        == "p --print-command-line \"say \\\"hi\\\"\" back\\slash \"ends with\\\\\" \"a\\\\\\\"b\"");
    // Text that is not ASCII arrives whole.
    CHECK(CommandLineOf({ "p", kPrintCommandLine, "h\xc3\xa9llo", "\xe6\x97\xa5\xe6\x9c\xac" }) == "p --print-command-line h\xc3\xa9llo \xe6\x97\xa5\xe6\x9c\xac");
}

TEST(code_section_is_where_the_images_functions_are)
{
    uint8_t const* start = nullptr;
    size_t size = 0;
    CHECK(bg3se::shim::CodeSection(GetModuleHandleW(NULL), start, size));
    auto here = reinterpret_cast<uint8_t const*>(&GetModuleHandleW);
    CHECK(start != nullptr && size > 0 && here >= start && here < start + size);
    int notAnImage = 0;
    start = nullptr;
    CHECK(!bg3se::shim::CodeSection(reinterpret_cast<HMODULE>(&notAnImage), start, size) && start == nullptr);
}

TEST(dllmain_calls_have_their_mac_meaning)
{
    // DisableThreadLibraryCalls: any loaded image, and nothing else.
    HMODULE self = GetModuleHandleW(NULL);
    CHECK(self != NULL && DisableThreadLibraryCalls(self) == TRUE);
    SetLastError(0);
    CHECK(DisableThreadLibraryCalls(NULL) == FALSE && GetLastError() == ERROR_INVALID_HANDLE);
    int notAnImage = 0;
    SetLastError(0);
    CHECK(DisableThreadLibraryCalls(reinterpret_cast<HMODULE>(&notAnImage)) == FALSE && GetLastError() == ERROR_MOD_NOT_FOUND);

    // CreateMutexA: one object for a name, and the caller told when it was there.
    char name[64];
    snprintf(name, sizeof(name), "BG3SE_Ext_%d", (int)GetCurrentProcessId());
    SetLastError(ERROR_INVALID_PARAMETER);
    HANDLE first = CreateMutexA(NULL, TRUE, name);
    CHECK(first != NULL && GetLastError() == 0);
    HANDLE again = CreateMutexA(NULL, TRUE, name);
    CHECK(again == first && GetLastError() == ERROR_ALREADY_EXISTS);
    HANDLE other = CreateMutexA(NULL, FALSE, "Global\\bg3mse-shim-test");
    CHECK(other != NULL && other != first && GetLastError() == 0);
    CHECK(CreateMutexA(NULL, FALSE, "bg3mse-shim-test") == other && GetLastError() == ERROR_ALREADY_EXISTS);
    HANDLE unnamed = CreateMutexA(NULL, FALSE, NULL);
    CHECK(unnamed != NULL && unnamed != CreateMutexA(NULL, FALSE, NULL));
    SetLastError(0);
    CHECK(CreateMutexA(NULL, FALSE, "a\\b") == NULL && GetLastError() == ERROR_INVALID_PARAMETER);
    CHECK(CreateMutexA(NULL, FALSE, "") == NULL);

    // GetAsyncKeyState: the high bit or nothing for a modifier, nothing for
    // a key it cannot answer for. Which it is depends on the keyboard.
    short shift = GetAsyncKeyState(VK_SHIFT);
    CHECK(shift == 0 || shift == static_cast<short>(0x8000));
    CHECK(GetAsyncKeyState('A') == 0 && GetAsyncKeyState(0) == 0);
}

int main(int argc, char** argv)
{
    // Run by the command line test: print what GetCommandLineW returns.
    if (argc >= 2 && strcmp(argv[1], kPrintCommandLine) == 0) {
        fputs(Narrow(GetCommandLineW()).c_str(), stdout);
        return 0;
    }
    void (*tests[])() = {
        utf8_round_trips_every_scalar_value_run,
        utf8_encodes_known_sequences_run,
        utf8_invalid_input_becomes_replacement_characters_run,
        utf8_strict_flags_reject_invalid_input_run,
        utf8_lengths_buffers_and_errors_follow_win32_run,
        crt_bounded_string_functions_run,
        intrinsics_match_their_x86_definitions_run,
        last_error_is_per_thread_run,
        code_section_is_where_the_images_functions_are_run,
        dllmain_calls_have_their_mac_meaning_run,
        process_handles_and_time_run,
        exit_process_and_debug_break_run,
        word_macros_and_crt_names_run,
        local_memory_run,
        copy_memory_copies_run,
        clock_text_is_what_msvc_formats_run,
        clock_time_has_100_ns_ticks_within_the_system_clocks_microsecond_run,
        formatting_and_comparisons_ignore_the_process_locale_run,
        uuids_are_random_version_4_run,
        files_and_directories_run,
        sockets_serve_a_loopback_connection_run,
        sockets_free_a_listeners_port_and_its_accept_when_it_is_closed_run,
        concurrent_queue_is_fifo_and_loses_nothing_across_threads_run,
        concurrent_queue_takes_an_incomplete_element_type_and_destroys_every_element_run,
        concurrent_vector_keeps_elements_in_place_while_growing_run,
        modules_are_loaded_mach_o_images_run,
        resources_are_sections_of_the_image_run,
        is_bad_read_ptr_asks_without_faulting_run,
        command_line_is_the_arguments_quoted_as_windows_reads_them_run,
    };
    for (auto test : tests) test();
    size_t consoleTests = RunConsoleTests(gFailures);
    printf("%zu tests, %d failures\n", std::size(tests) + consoleTests, gFailures);
    return gFailures ? 1 : 0;
}
