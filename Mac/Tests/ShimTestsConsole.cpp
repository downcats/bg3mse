// Tests of the console functions (Mac/Shim/Win32Console.cpp), of the screen
// of a console that is a window (Mac/Shim/ConsoleScreen.cpp) and of the
// message box (Mac/Shim/Win32MessageBox.cpp), run from ShimTests.cpp.
//
// The console is the terminal a process was started from, or a window when
// it has none, so each test of the console functions runs its calls in a
// child process, whose terminal is a pseudo-terminal of the test's or which
// has none: the child says what it found through a pipe, and the test reads
// what the terminal was sent. No test shows an alert.

#include <windows.h>
#include <ConsoleScreen.h>
#include <Win32MessageBox.h>

#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <atomic>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <util.h>
#include <vector>

namespace bg3se::shim {

// Win32Console.cpp: the sequence SetConsoleTextAttribute writes.
std::string ConsoleAttributeSequence(WORD attributes);

}

namespace {

// The C library's own stdout and printf, which every image but the one the
// shim is linked into writes to (OwnStdout.cpp).
void* CLibrary()
{
    static void* library = dlopen("/usr/lib/libSystem.B.dylib", RTLD_LAZY | RTLD_NOLOAD);
    return library;
}
FILE* CLibraryStdout() { return *static_cast<FILE**>(dlsym(CLibrary(), "__stdoutp")); }
using Printf = int (*)(char const*, ...);
Printf CLibraryPrintf() { return reinterpret_cast<Printf>(dlsym(CLibrary(), "printf")); }

int gFailures = 0;
char const* gTest = "";

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)

// --- a child on a terminal ------------------------------------------------------

// The child's side of a test: `expect` reports a failed expectation, `say` a
// line for the test to look at.
struct Child
{
    int Report;

    void Say(char const* format, ...) const __attribute__((format(printf, 2, 3)))
    {
        char line[512];
        va_list args;
        va_start(args, format);
        int length = vsnprintf(line, sizeof(line) - 1, format, args);
        va_end(args);
        line[length++] = '\n';
        (void)!write(Report, line, (size_t)length);
    }
};
#define EXPECT(cond) do { if (!(cond)) child.Say("FAIL %s (line %d)", #cond, __LINE__); } while (0)

struct Run
{
    pid_t Pid{ -1 };
    int Terminal{ -1 };     // the test's end of the pseudo-terminal; -1 when the child has none
    int Report{ -1 };
    std::string Sent;       // what the terminal was sent
    std::string Said;       // what the child said

    // Reads what has arrived on both ends until `done` holds or `ms` passed.
    bool Until(std::function<bool()> done, int ms = 5000)
    {
        for (int waited = 0; !done(); waited += 20) {
            if (waited >= ms) return false;
            pollfd fds[2] = { { Terminal, POLLIN, 0 }, { Report, POLLIN, 0 } };
            poll(fds, 2, 20);
            Drain();
        }
        return true;
    }

    void Drain()
    {
        char buffer[4096];
        for (int fd : { Terminal, Report }) {
            if (fd < 0) continue;
            for (;;) {
                ssize_t got = read(fd, buffer, sizeof(buffer));
                if (got <= 0) break;
                (fd == Terminal ? Sent : Said).append(buffer, (size_t)got);
            }
        }
    }

    void Type(std::string const& text) const { (void)!write(Terminal, text.data(), text.size()); }

    bool Alive() const
    {
        int status = 0;
        return waitpid(Pid, &status, WNOHANG) == 0;
    }

    // Waits for the child to end and takes everything it left. Returns its exit status.
    int Finish()
    {
        int status = 0;
        // The child's output must be read while it runs: a terminal's buffer is small.
        Until([&] { return waitpid(Pid, &status, WNOHANG) == Pid; }, 10000);
        Drain();
        Close();
        return status;
    }

    void Kill()
    {
        kill(Pid, SIGKILL);
        int status = 0;
        waitpid(Pid, &status, 0);
        Close();
    }

    void Close()
    {
        if (Terminal >= 0) close(Terminal);
        if (Report >= 0) close(Report);
        Terminal = Report = -1;
    }

    // Every "FAIL" line the child said counts as a failure of the test.
    void CountFailures() const
    {
        size_t at = 0;
        while ((at = Said.find("FAIL", at)) != std::string::npos) {
            size_t end = Said.find('\n', at);
            printf("FAIL %s (in the child): %s\n", gTest, Said.substr(at, end - at).c_str());
            gFailures++;
            at = end == std::string::npos ? Said.size() : end;
        }
    }
};

enum class Streams
{
    Terminal,           // standard input, output and error are the terminal
    OutputIsAPipe,      // standard input is the terminal, standard output is not
    None,               // no terminal: standard input is /dev/null, output a pipe
};

Run Start(Streams streams, std::function<void(Child const&)> body)
{
    fflush(stdout);
    Run run;
    int report[2];
    if (pipe(report) != 0) return run;
    int output[2] = { -1, -1 };
    if (streams != Streams::Terminal && pipe(output) != 0) return run;

    winsize size{};
    size.ws_row = 24;
    size.ws_col = 100;
    int terminal = -1;
    pid_t pid = streams == Streams::None ? fork() : forkpty(&terminal, nullptr, nullptr, &size);
    if (pid == 0) {
        close(report[0]);
        if (streams == Streams::None) {
            int null = open("/dev/null", O_RDONLY);
            dup2(null, STDIN_FILENO);
        }
        if (streams != Streams::Terminal) {
            dup2(output[1], STDOUT_FILENO);
            dup2(output[1], STDERR_FILENO);
        }
        body(Child{ report[1] });
        fflush(stdout);
        _exit(0);
    }
    close(report[1]);
    run.Pid = pid;
    run.Report = report[0];
    if (streams == Streams::Terminal) {
        run.Terminal = terminal;
    } else {
        // What the child writes to its standard output stands in for the
        // terminal's side; the pseudo-terminal, if any, only has to exist.
        close(output[1]);
        run.Terminal = output[0];
    }
    fcntl(run.Terminal, F_SETFL, fcntl(run.Terminal, F_GETFL) | O_NONBLOCK);
    fcntl(run.Report, F_SETFL, fcntl(run.Report, F_GETFL) | O_NONBLOCK);
    return run;
}

bool Has(std::string const& text, char const* part) { return text.find(part) != std::string::npos; }

// --- tests -------------------------------------------------------------------------

void attributes_are_colour_sequences()
{
    gTest = "attributes_are_colour_sequences";
    using bg3se::shim::ConsoleAttributeSequence;
    // The five Console::SetColor uses (CoreLib/Console.cpp).
    CHECK(ConsoleAttributeSequence(FOREGROUND_RED | FOREGROUND_INTENSITY) == "\x1b[0;91m");
    CHECK(ConsoleAttributeSequence(FOREGROUND_RED | FOREGROUND_GREEN) == "\x1b[0;33m");
    CHECK(ConsoleAttributeSequence(FOREGROUND_INTENSITY | FOREGROUND_GREEN | FOREGROUND_BLUE) == "\x1b[0;96m");
    CHECK(ConsoleAttributeSequence(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY) == "\x1b[0;97m");
    // Grey on black is the terminal's own colours, not those two.
    CHECK(ConsoleAttributeSequence(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE) == "\x1b[0m");
    // Black text, and a background: blue is bit 0 on Windows and 4 in a sequence.
    CHECK(ConsoleAttributeSequence(0) == "\x1b[0;30m");
    CHECK(ConsoleAttributeSequence(0x1F) == "\x1b[0;97;44m");
    CHECK(ConsoleAttributeSequence(0xC7) == "\x1b[0;101m");
}

void console_is_the_terminal_the_process_was_started_from()
{
    gTest = "console_is_the_terminal_the_process_was_started_from";
    Run run = Start(Streams::Terminal, [](Child const& child) {
        // Before AllocConsole the process has no console.
        EXPECT(GetStdHandle(STD_OUTPUT_HANDLE) == NULL);
        SetLastError(0);
        EXPECT(SetConsoleTitleW(L"early") == FALSE && GetLastError() == ERROR_INVALID_HANDLE);

        EXPECT(AllocConsole() == TRUE);
        SetLastError(0);
        EXPECT(AllocConsole() == FALSE && GetLastError() == ERROR_ACCESS_DENIED);

        HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
        EXPECT(in != NULL && out != NULL && err != NULL && in != out && out != err && in != err);
        SetLastError(0);
        EXPECT(GetStdHandle(7) == INVALID_HANDLE_VALUE && GetLastError() == ERROR_INVALID_HANDLE);

        // Console::Create's calls, in its order.
        EXPECT(IsValidCodePage(CP_UTF8) == TRUE && IsValidCodePage(1252) == FALSE);
        EXPECT(SetConsoleCP(CP_UTF8) == TRUE);
        termios settings{};
        EXPECT(tcgetattr(STDIN_FILENO, &settings) == 0 && (settings.c_iflag & IUTF8) != 0);
        EXPECT(SetConsoleOutputCP(CP_UTF8) == TRUE);
        SetLastError(0);
        EXPECT(SetConsoleCP(1252) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER);
        EXPECT(SetConsoleOutputCP(1252) == FALSE);

        EXPECT(SetConsoleMode(out, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING) == TRUE);
        DWORD mode = 0;
        EXPECT(GetConsoleMode(out, &mode) == TRUE && mode == (ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING));
        SetLastError(0);
        EXPECT(SetConsoleMode(out, 0x100) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER);
        EXPECT(GetConsoleMode(in, &mode) == TRUE && mode == 0x7);
        SetLastError(0);
        EXPECT(SetConsoleMode(&mode, 0) == FALSE && GetLastError() == ERROR_INVALID_HANDLE);

        struct sigaction action{};
        EXPECT(SetConsoleCtrlHandler(NULL, TRUE) == TRUE);
        EXPECT(sigaction(SIGINT, nullptr, &action) == 0 && action.sa_handler == SIG_IGN);
        SetLastError(0);
        EXPECT(SetConsoleCtrlHandler([](DWORD) -> BOOL { return TRUE; }, TRUE) == FALSE && GetLastError() == ERROR_NOT_SUPPORTED);

        // The standard streams already are the console.
        FILE* stream = nullptr;
        EXPECT(freopen_s(&stream, "CONOUT$", "w", stdout) == 0 && stream == stdout);
        EXPECT(freopen_s(&stream, "CONIN$", "r", stdin) == 0 && stream == stdin);

        // DebugConsole::Create and UpdateConsoleSize.
        EXPECT(SetConsoleTitleW(L"BG3 Script Extender Debug Console") == TRUE);
        CONSOLE_SCREEN_BUFFER_INFOEX info;
        info.cbSize = sizeof(info);
        EXPECT(GetConsoleScreenBufferInfoEx(out, &info) == TRUE);
        EXPECT(info.srWindow.Right - info.srWindow.Left + 1 == 100 && info.srWindow.Bottom - info.srWindow.Top + 1 == 24);
        EXPECT(info.dwSize.X == 100 && info.dwSize.Y == 24);
        info.cbSize = 4;
        SetLastError(0);
        EXPECT(GetConsoleScreenBufferInfoEx(out, &info) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER);
        info.cbSize = sizeof(info);
        EXPECT(GetConsoleScreenBufferInfoEx(in, &info) == FALSE);

        // Console::LocalPrint: colour, the line, the default colour again.
        EXPECT(SetConsoleTextAttribute(out, FOREGROUND_RED | FOREGROUND_INTENSITY) == TRUE);
        printf("an error\n");
        fflush(stdout);
        EXPECT(SetConsoleTextAttribute(out, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE) == TRUE);
        printf("plain\n");
        SetLastError(0);
        EXPECT(SetConsoleTextAttribute(out, 0x4000) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER);
        EXPECT(SetConsoleTextAttribute(in, 7) == FALSE);
        // Text still in the C library's buffer keeps the colour it was printed under.
        EXPECT(SetConsoleTextAttribute(out, FOREGROUND_GREEN) == TRUE);
        printf("green, unflushed");
        EXPECT(SetConsoleTextAttribute(out, FOREGROUND_BLUE) == TRUE);
        printf("blue\n");
        fflush(stdout);

        // FreeConsole gives the terminal back as it was found.
        EXPECT(FreeConsole() == TRUE);
        EXPECT(sigaction(SIGINT, nullptr, &action) == 0 && action.sa_handler == SIG_DFL);
        EXPECT(tcgetattr(STDIN_FILENO, &settings) == 0 && (settings.c_iflag & IUTF8) == 0);
        EXPECT(GetStdHandle(STD_OUTPUT_HANDLE) == NULL);
        SetLastError(0);
        EXPECT(FreeConsole() == FALSE && GetLastError() == ERROR_INVALID_PARAMETER);
        EXPECT(SetConsoleTextAttribute(out, 7) == FALSE);
        child.Say("done");
    });
    int status = run.Finish();
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(Has(run.Said, "done"));
    run.CountFailures();

    CHECK(Has(run.Sent, "\x1b%G"));
    CHECK(Has(run.Sent, "\x1b[?7h"));
    CHECK(Has(run.Sent, "\x1b]0;BG3 Script Extender Debug Console\a"));
    CHECK(Has(run.Sent, "\x1b[0;91man error\r\n\x1b[0mplain\r\n"));
    CHECK(Has(run.Sent, "\x1b[0;32mgreen, unflushed\x1b[0;34mblue\r\n"));
    CHECK(!Has(run.Sent, "early"));
    // The last attribute was not the default one: FreeConsole put the default back.
    CHECK(run.Sent.size() >= 4 && run.Sent.compare(run.Sent.size() - 4, 4, "\x1b[0m") == 0);
}

void console_input_is_what_is_typed_at_the_terminal()
{
    gTest = "console_input_is_what_is_typed_at_the_terminal";
    Run run = Start(Streams::Terminal, [](Child const& child) {
        signal(SIGHUP, SIG_IGN);    // the test closes the terminal at the end
        EXPECT(AllocConsole() == TRUE);
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        wchar_t c = 0;
        DWORD read = 7;
        SetLastError(0);
        EXPECT(ReadConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), &c, 1, &read, NULL) == FALSE && GetLastError() == ERROR_INVALID_HANDLE);
        EXPECT(ReadConsoleW(in, &c, 0, &read, NULL) == TRUE && read == 0);
        child.Say("ready");
        // One character at a time, as DebugConsole::ConsoleThread reads.
        for (int i = 0; i < 7; i++) {
            EXPECT(ReadConsoleW(in, &c, 1, &read, NULL) == TRUE && read == 1);
            child.Say("char %x", (unsigned)c);
        }
        // Several at once: what the line has, and no more.
        wchar_t many[16]{};
        EXPECT(ReadConsoleW(in, many, 16, &read, NULL) == TRUE && read == 4);
        child.Say("line %x %x %x %x", (unsigned)many[0], (unsigned)many[1], (unsigned)many[2], (unsigned)many[3]);
        // The end-of-file key on an empty line.
        EXPECT(ReadConsoleW(in, &c, 1, &read, NULL) == TRUE && read == 0);
        child.Say("end of file");
        // The terminal goes away: no input ever arrives, and the call does not return.
        child.Say("waiting");
        BOOL result = ReadConsoleW(in, &c, 1, &read, NULL);
        child.Say("returned %d", result);
    });
    CHECK(run.Until([&] { return Has(run.Said, "ready"); }));
    run.Type("ab\n");
    run.Type("\xc3\xa9\n");         // U+00E9
    CHECK(run.Until([&] { return Has(run.Said, "char 61\nchar 62\nchar d\nchar a\nchar e9\nchar d\nchar a\n"); }));
    run.Type("xy\n");
    CHECK(run.Until([&] { return Has(run.Said, "line 78 79 d a\n"); }));
    run.Type("\x04");
    CHECK(run.Until([&] { return Has(run.Said, "end of file\nwaiting\n"); }));
    close(run.Terminal);
    run.Terminal = -1;
    usleep(400 * 1000);
    run.Drain();
    CHECK(run.Alive());
    CHECK(!Has(run.Said, "returned"));
    run.CountFailures();
    run.Kill();
}

// The extender's console (Extender/Shared/Console.cpp): ConsoleThread waits
// for the '\r' of an Enter, one character at a time, and InputLoop then reads
// its commands a line at a time.
void console_lines_are_read_through_the_console()
{
    gTest = "console_lines_are_read_through_the_console";
    Run run = Start(Streams::Terminal, [](Child const& child) {
        signal(SIGHUP, SIG_IGN);
        std::string line = "unread";
        // Without a console there is no line.
        EXPECT(!bg3se::mac::ReadConsoleLine(line) && line.empty());
        EXPECT(AllocConsole() == TRUE);
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        child.Say("ready");
        wchar_t c = 0;
        DWORD read = 0;
        while (ReadConsoleW(in, &c, 1, &read, NULL) && !(read == 1 && c == L'\r')) {}
        child.Say("opened");
        for (int i = 0; i < 6; i++) {
            bool got = bg3se::mac::ReadConsoleLine(line);
            child.Say("%s [%s]", got ? "line" : "end", line.c_str());
        }
        child.Say("done");
    });
    CHECK(run.Until([&] { return Has(run.Said, "ready"); }));
    run.Type("\n");
    CHECK(run.Until([&] { return Has(run.Said, "opened"); }));
    // A command, an empty line, and text that is not ASCII.
    run.Type("print(1)\n");
    run.Type("\n");
    run.Type("caf\xc3\xa9\n");
    CHECK(run.Until([&] { return Has(run.Said, "line [print(1)]\nline []\nline [caf\xc3\xa9]\n"); }));
    // The end-of-file key on an empty line: no line, and the next read waits
    // for the next line instead of ending at once.
    run.Type("\x04");
    CHECK(run.Until([&] { return Has(run.Said, "end []\n"); }));
    usleep(200 * 1000);
    run.Drain();
    CHECK(!Has(run.Said, "end []\nend") && !Has(run.Said, "end []\nline"));
    run.Type("exit\n");
    CHECK(run.Until([&] { return Has(run.Said, "end []\nline [exit]\n"); }));
    // The key after some text hands the text over without ending the line;
    // the key again, on what is then an empty line, drops it.
    run.Type("ab");
    run.Type("\x04");
    run.Type("\x04");
    CHECK(run.Until([&] { return Has(run.Said, "line [exit]\nend []\ndone\n"); }));
    run.CountFailures();
    run.Kill();
}

void without_a_terminal_there_is_no_console()
{
    gTest = "without_a_terminal_there_is_no_console";
    auto body = [](Child const& child) {
        struct sigaction before{}, after{};
        sigaction(SIGINT, nullptr, &before);

        SetLastError(0);
        EXPECT(AllocConsole() == FALSE && GetLastError() == ERROR_NOT_SUPPORTED);
        EXPECT(GetStdHandle(STD_INPUT_HANDLE) == NULL && GetStdHandle(STD_OUTPUT_HANDLE) == NULL);
        HANDLE none = GetStdHandle(STD_OUTPUT_HANDLE);
        // What Console::Create would go on to call, were it not to stop there.
        SetLastError(0);
        EXPECT(SetConsoleCP(CP_UTF8) == FALSE && GetLastError() == ERROR_INVALID_HANDLE);
        EXPECT(SetConsoleOutputCP(CP_UTF8) == FALSE);
        EXPECT(SetConsoleMode(none, ENABLE_PROCESSED_OUTPUT) == FALSE);
        EXPECT(SetConsoleCtrlHandler(NULL, TRUE) == FALSE);
        sigaction(SIGINT, nullptr, &after);
        EXPECT(before.sa_handler == after.sa_handler);
        EXPECT(SetConsoleTextAttribute(none, FOREGROUND_RED) == FALSE);
        EXPECT(SetConsoleTitleW(L"nobody sees this") == FALSE);
        CONSOLE_SCREEN_BUFFER_INFOEX info;
        info.cbSize = sizeof(info);
        EXPECT(GetConsoleScreenBufferInfoEx(none, &info) == FALSE);
        // Input fails at once: nothing waits on a console that is not there.
        wchar_t c = 0;
        DWORD read = 0;
        SetLastError(0);
        EXPECT(ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), &c, 1, &read, NULL) == FALSE && GetLastError() == ERROR_INVALID_HANDLE);
        SetLastError(0);
        EXPECT(FreeConsole() == FALSE && GetLastError() == ERROR_INVALID_PARAMETER);

        // The standard streams stay what the process was started with.
        FILE* stream = stdout;
        EXPECT(freopen_s(&stream, "CONOUT$", "w", stdout) == ENOENT && stream == nullptr);
        EXPECT(freopen_s(&stream, "CONIN$", "r", stdin) == ENOENT && stream == nullptr);
        EXPECT(printf("still here\n") > 0 && fflush(stdout) == 0);

        // The debugger's string is not the standard streams' business.
        OutputDebugStringA("a line for the debug output");
        OutputDebugStringA("\r\n");
        child.Say("done");
    };
    for (Streams streams : { Streams::None, Streams::OutputIsAPipe }) {
        Run run = Start(streams, body);
        int status = run.Finish();
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        CHECK(Has(run.Said, "done"));
        run.CountFailures();
        // Not one control sequence, and nothing of the debug output.
        CHECK(run.Sent == "still here\n");
    }
}

void a_background_job_is_not_stopped_by_its_console()
{
    gTest = "a_background_job_is_not_stopped_by_its_console";
    // The child is the terminal's session leader, in the foreground; its own
    // child is put in a process group of its own: a background job.
    Run run = Start(Streams::Terminal, [](Child const& child) {
        signal(SIGTTOU, SIG_IGN);       // for the tcsetpgrp below
        int go[2];
        EXPECT(pipe(go) == 0);
        pid_t job = fork();
        if (job == 0) {
            setpgid(0, 0);
            char token;
            (void)!read(go[0], &token, 1);
            signal(SIGTTOU, SIG_DFL);
            EXPECT(tcgetpgrp(STDIN_FILENO) != getpgrp());
            EXPECT(AllocConsole() == TRUE);
            // Changing the terminal's settings from the background would stop the job.
            EXPECT(SetConsoleCP(CP_UTF8) == TRUE);
            EXPECT(SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT) == TRUE);
            child.Say("settings changed");
            // So would reading from it: the read waits for the foreground instead.
            wchar_t c = 0;
            DWORD read = 0;
            EXPECT(ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), &c, 1, &read, NULL) == TRUE && read == 1);
            child.Say("read %x", (unsigned)c);
            _exit(0);
        }
        setpgid(job, job);
        (void)!write(go[1], "g", 1);
        // While the job waits in the background it must be running, not stopped.
        usleep(300 * 1000);
        int status = 0;
        EXPECT(waitpid(job, &status, WNOHANG | WUNTRACED) == 0);
        child.Say("not stopped");
        EXPECT(tcsetpgrp(STDIN_FILENO, job) == 0);
        child.Say("in the foreground");
        EXPECT(waitpid(job, &status, 0) == job && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        child.Say("done");
    });
    CHECK(run.Until([&] { return Has(run.Said, "settings changed") && Has(run.Said, "not stopped") && Has(run.Said, "in the foreground"); }));
    CHECK(!Has(run.Said, "read "));
    run.Type("k\n");
    CHECK(run.Until([&] { return Has(run.Said, "read 6b\n"); }));
    int status = run.Finish();
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(Has(run.Said, "done"));
    run.CountFailures();
}

// --- the console's screen, when the console is a window ----------------------

using bg3se::shim::ConsoleLine;
using bg3se::shim::ConsoleRun;
using bg3se::shim::ConsoleScreen;

constexpr uint32_t kGrey = 0xCCCCCC, kBrightRed = 0xE74856, kRed = 0xC50F1F;

ConsoleLine Line(std::initializer_list<ConsoleRun> runs) { return ConsoleLine(runs); }

std::vector<ConsoleLine> LinesOf(ConsoleScreen& screen)
{
    auto view = screen.Look();
    std::vector<ConsoleLine> lines;
    screen.Copy(view.First, view.End, lines);
    return lines;
}

std::string TextOf(ConsoleLine const& line)
{
    std::string text;
    for (auto const& run : line) text += run.Text;
    return text;
}

void the_screen_keeps_lines_in_their_colours()
{
    gTest = "the_screen_keeps_lines_in_their_colours";
    ConsoleScreen screen;
    auto me = std::this_thread::get_id();
    // A line is there once its end is.
    screen.Write(me, "first\nsec");
    CHECK(LinesOf(screen).size() == 1);
    screen.Write(me, "ond\n");
    auto lines = LinesOf(screen);
    CHECK(lines.size() == 2 && lines[0] == Line({ { kGrey, "first" } }) && lines[1] == Line({ { kGrey, "second" } }));

    // Norbyte's console: the attributes, the message, the default attributes again.
    screen.SetAttributes(me, FOREGROUND_RED | FOREGROUND_INTENSITY);
    screen.Write(me, "an error");
    screen.Write(me, "\n");
    screen.SetAttributes(me, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    CHECK(LinesOf(screen).back() == Line({ { kBrightRed, "an error" } }));
    CHECK(bg3se::shim::ConsoleColourOf(FOREGROUND_RED | FOREGROUND_GREEN) == 0xC19C00);

    // A script's own colours: what a mod printed in the game (run2's log).
    screen.Write(me, "\x1b[38;2;255;100;30;48;2;12;12;12m[Mod Configuration Menu][WARN][C]: text\x1b[0m after\n");
    CHECK(LinesOf(screen).back() == Line({ { 0xFF641E, "[Mod Configuration Menu][WARN][C]: text", 0x0C0C0C }, { kGrey, " after" } }));
    screen.Write(me, "\x1b[31mred\x1b[1m bright\x1b[22m\x1b[39m plain\x1b[92m green\x1b[38;5;196m cube\x1b[38;5;244m grey\x1b[m\n");
    CHECK(LinesOf(screen).back() == Line({ { kRed, "red" }, { kBrightRed, " bright" }, { kGrey, " plain" }, { 0x16C60C, " green" },
        { 0xFF0000, " cube" }, { 0x808080, " grey" } }));
    // A sequence in two writes, one that names no colour, a title.
    screen.Write(me, "\x1b[3");
    screen.Write(me, "1mx\x1b[48;5;1my\x1b[5mz\x1b]0;a title\x07!\x1b[0m\n");
    CHECK(LinesOf(screen).back() == Line({ { kRed, "x" }, { kRed, "yz!", kRed } }));

    // Backgrounds: of the attributes, of SGR 40-47, 100-107 and 48, back to
    // the console's own with 49 or 0; black is the console's own.
    screen.SetAttributes(me, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY | BACKGROUND_BLUE);
    screen.Write(me, "on blue");
    screen.SetAttributes(me, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    screen.Write(me, "\x1b[41mon red\x1b[101mon bright red\x1b[49mplain\x1b[40mon black\x1b[48;5;46mon green\x1b[0mplain\n");
    CHECK(LinesOf(screen).back() == Line({ { 0xF2F2F2, "on blue", 0x0037DA }, { kGrey, "on red", kRed }, { kGrey, "on bright red", kBrightRed },
        { kGrey, "plainon black" }, { kGrey, "on green", 0x00FF00 }, { kGrey, "plain" } }));
    CHECK(bg3se::shim::ConsoleBackgroundOf(BACKGROUND_RED | BACKGROUND_INTENSITY) == kBrightRed
        && bg3se::shim::ConsoleBackgroundOf(FOREGROUND_RED) == bg3se::shim::kNoBackground);
    // Reversed, by SGR 7 or by the attribute: on the console's own background
    // the text is its black.
    screen.Write(me, "\x1b[7mreversed\x1b[27m\x1b[33;44m\x1b[7myellow\x1b[27mblue\x1b[0m\n");
    CHECK(LinesOf(screen).back() == Line({ { 0x0C0C0C, "reversed", kGrey }, { 0x0037DA, "yellow", 0xC19C00 }, { 0xC19C00, "blue", 0x0037DA } }));
    screen.SetAttributes(me, FOREGROUND_GREEN | COMMON_LVB_REVERSE_VIDEO);
    screen.Write(me, "attribute\n");
    screen.SetAttributes(me, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    CHECK(LinesOf(screen).back() == Line({ { 0x0C0C0C, "attribute", 0x13A10E } }));

    // Control characters: a tab to the next stop, the rest move a cursor there is not.
    screen.Write(me, "a\tb\r\x08\x01" "c\x7f\n");
    CHECK(LinesOf(screen).back() == Line({ { kGrey, "a       bc" } }));
    // Text that is not ASCII is kept as it is.
    screen.Write(me, "caf\xc3\xa9 \xe2\x9c\x93\n");
    CHECK(TextOf(LinesOf(screen).back()) == "caf\xc3\xa9 \xe2\x9c\x93");
    // A writer that is back to how it started has no state left.
    CHECK(screen.Writers() == 0);

    screen.SetTitle("BG3 Script Extender Debug Console");
    CHECK(screen.Look().Title == "BG3 Script Extender Debug Console");
}

void the_screen_is_bounded()
{
    gTest = "the_screen_is_bounded";
    ConsoleScreen screen;
    auto me = std::this_thread::get_id();
    for (size_t i = 0; i < ConsoleScreen::kMostLines + 10; i++) screen.Write(me, "line " + std::to_string(i) + "\n");
    auto view = screen.Look();
    CHECK(view.End - view.First == ConsoleScreen::kMostLines && view.First == 10);
    std::vector<ConsoleLine> some;
    screen.Copy(0, 12, some);
    CHECK(some.size() == 2 && TextOf(some[0]) == "line 10");

    // A long line goes on in the next, never in the middle of a character.
    std::string longLine(ConsoleScreen::kMostLineBytes - 1, 'x');
    longLine += "\xc3\xa9";
    longLine += std::string(100, 'y');
    screen.Write(me, longLine + "\n");
    view = screen.Look();
    some.clear();
    screen.Copy(view.End - 2, view.End, some);
    CHECK(some.size() == 2 && TextOf(some[0]) == std::string(ConsoleScreen::kMostLineBytes - 1, 'x'));
    CHECK(some.size() == 2 && TextOf(some[1]) == "\xc3\xa9" + std::string(100, 'y'));

    // And the bytes kept are bounded too.
    std::string full(ConsoleScreen::kMostLineBytes - 1, 'z');
    for (size_t i = 0; i < 2 * ConsoleScreen::kMostBytes / full.size(); i++) screen.Write(me, full + "\n");
    CHECK(screen.Bytes() <= ConsoleScreen::kMostBytes);
    CHECK(screen.Bytes() > ConsoleScreen::kMostBytes - ConsoleScreen::kMostLineBytes);
}

void the_lines_of_threads_are_not_mixed()
{
    gTest = "the_lines_of_threads_are_not_mixed";
    ConsoleScreen screen;
    constexpr int kThreads = 8, kLines = 1000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&screen, t] {
            auto me = std::this_thread::get_id();
            for (int i = 0; i < kLines; i++) {
                // As Norbyte's console writes a message: colour, text in pieces, line end, colour.
                screen.SetAttributes(me, (WORD)(t + 1));
                screen.Write(me, "thread ");
                screen.Write(me, std::to_string(t));
                screen.Write(me, " line ");
                screen.Write(me, std::to_string(i) + "\n");
                screen.SetAttributes(me, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    auto lines = LinesOf(screen);
    CHECK(lines.size() == kThreads * kLines);
    std::vector<int> next(kThreads, 0);
    bool whole = true;
    for (auto const& line : lines) {
        int t = -1, i = -1;
        if (line.size() != 1 || sscanf(line[0].Text.c_str(), "thread %d line %d", &t, &i) != 2 || t < 0 || t >= kThreads) {
            whole = false;
            break;
        }
        // Each thread's lines in its order, in its colour, and nothing else in them.
        whole = whole && i == next[(size_t)t]++ && line[0].Colour == bg3se::shim::ConsoleColourOf((WORD)(t + 1))
            && line[0].Text == "thread " + std::to_string(t) + " line " + std::to_string(i);
    }
    CHECK(whole);
    CHECK(screen.Writers() == 0);
}

void clearing_the_screen_keeps_the_prompt()
{
    gTest = "clearing_the_screen_keeps_the_prompt";
    ConsoleScreen screen;
    screen.Open();
    auto me = std::this_thread::get_id();
    screen.Write(me, "one\ntwo\n");
    auto before = screen.Look();
    // Console::Clear.
    screen.Write(me, "\x1b[2J" "\x1b[H" "\x1b[3J");
    auto after = screen.Look();
    CHECK(after.First == after.End && after.First == before.End && after.Cleared == before.Cleared + 2);
    screen.Write(me, "three\n");
    std::vector<ConsoleLine> lines;
    screen.Copy(before.First, before.End + 1, lines);
    CHECK(lines.size() == 1 && TextOf(lines[0]) == "three");

    // The reader's prompt is not a line: a clear leaves it.
    std::thread reader([&] {
        screen.Write(std::this_thread::get_id(), "S >> ");
        wchar_t got[8];
        size_t taken = 0;
        screen.Read(got, 8, taken);
    });
    for (int i = 0; i < 500 && !screen.Look().Reading; i++) usleep(2000);
    screen.Clear();
    auto view = screen.Look();
    CHECK(view.Reading && view.Prompt == Line({ { kGrey, "S >> " } }));
    screen.Type("");
    reader.join();
    // An empty line, echoed after its prompt.
    CHECK(LinesOf(screen) == std::vector<ConsoleLine>{ Line({ { kGrey, "S >> " } }) });
}

void what_is_typed_is_read_and_echoed()
{
    gTest = "what_is_typed_is_read_and_echoed";
    ConsoleScreen screen;
    // Nothing is typed into a screen that is no console.
    screen.Type("lost");
    screen.Open();
    std::u32string read;
    std::atomic<bool> bothRead{ false };
    bool readerEnded = false;
    std::thread reader([&] {
        auto me = std::this_thread::get_id();
        screen.SetAttributes(me, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        screen.Write(me, "C");
        screen.SetAttributes(me, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        screen.Write(me, " >> ");
        wchar_t got[4];
        size_t taken = 0;
        // A few characters at a time, as a caller with a small buffer reads.
        while (screen.Read(got, 4, taken)) {
            read.append(reinterpret_cast<char32_t*>(got), taken);
            if (read.size() >= 2 && read.substr(read.size() - 2) == U"\r\n" && read.find(U"caf") != std::u32string::npos) break;
        }
        bothRead = true;
        readerEnded = !screen.Read(got, 4, taken);
    });
    for (int i = 0; i < 500 && !screen.Look().Reading; i++) usleep(2000);
    auto view = screen.Look();
    CHECK(view.Reading && view.Prompt == Line({ { 0x16C60C, "C" }, { kGrey, " >> " } }));
    // A control character in what is typed would end the line early.
    screen.Type("print(1)\x0d\x0a");
    for (int i = 0; i < 500 && !screen.Look().Prompt.empty(); i++) usleep(2000);
    screen.Type("caf\xc3\xa9");
    // Closing lets the reader go; what it has not taken by then is not read.
    for (int i = 0; i < 500 && !(bothRead && screen.Look().Reading); i++) usleep(2000);
    screen.Close();
    reader.join();
    CHECK(read == U"print(1)\r\ncafé\r\n");
    CHECK(readerEnded);
    auto lines = LinesOf(screen);
    CHECK(lines.size() == 2 && lines[0] == Line({ { 0x16C60C, "C" }, { kGrey, " >> print(1)" } }) && lines[1] == Line({ { kGrey, "caf\xc3\xa9" } }));
}

// The Win32 console functions on a window: what Norbyte's Console::Create and
// LocalPrint call, in a child without a terminal.
struct CountingWindow final : bg3se::shim::WindowConsole
{
    std::atomic<int> Opened{ 0 }, Closed{ 0 };
    void ConsoleOpened() override { Opened++; }
    void ConsoleClosed() override { Closed++; }
};

void without_a_terminal_a_window_is_the_console()
{
    gTest = "without_a_terminal_a_window_is_the_console";
    Run run = Start(Streams::None, [](Child const& child) {
        auto& screen = bg3se::shim::WindowConsoleScreen();
        CountingWindow window;
        bg3se::shim::SetWindowConsole(&window);
        struct sigaction before{}, after{};
        sigaction(SIGINT, nullptr, &before);

        EXPECT(!bg3se::shim::ConsoleIsWindow());
        EXPECT(AllocConsole() == TRUE && window.Opened == 1 && bg3se::shim::ConsoleIsWindow());
        EXPECT(AllocConsole() == FALSE && GetLastError() == ERROR_ACCESS_DENIED);
        // Console::Create.
        EXPECT(IsValidCodePage(CP_UTF8) && SetConsoleCP(CP_UTF8) == TRUE && SetConsoleOutputCP(CP_UTF8) == TRUE);
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        EXPECT(SetConsoleMode(out, ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING) == TRUE);
        DWORD mode = 0;
        EXPECT(GetConsoleMode(out, &mode) == TRUE && mode == (ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING));
        EXPECT(SetConsoleMode(out, 0x100) == FALSE && GetLastError() == ERROR_INVALID_PARAMETER);
        EXPECT(SetConsoleCtrlHandler(NULL, TRUE) == TRUE);
        sigaction(SIGINT, nullptr, &after);
        EXPECT(before.sa_handler == after.sa_handler);
        FILE* stream = nullptr;
        EXPECT(freopen_s(&stream, "CONOUT$", "w", stdout) == 0 && stream == stdout);
        EXPECT(freopen_s(&stream, "CONIN$", "r", stdin) == 0 && stream == stdin);
        // DebugConsole::Create.
        EXPECT(SetConsoleTitleW(L"BG3 Script Extender Debug Console") == TRUE);
        EXPECT(screen.Look().Title == "BG3 Script Extender Debug Console");

        // Console::LocalPrint.
        EXPECT(SetConsoleTextAttribute(out, FOREGROUND_RED | FOREGROUND_INTENSITY) == TRUE);
        std::cout << "an error" << std::endl;
        EXPECT(SetConsoleTextAttribute(out, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE) == TRUE);
        EXPECT(LinesOf(screen).size() == 1 && LinesOf(screen)[0] == Line({ { kBrightRed, "an error" } }));
        // The image's own stdout is the window's too, in the writer's colour,
        // in order with std::cout, as the extender's own runtime's stdout is
        // on Windows; the C library's, which every other image writes to,
        // stays where it was.
        EXPECT(stdout != CLibraryStdout());
        printf("printed %s", "to ");
        fwrite("stdout", 1, 6, stdout);
        std::cout << " and cout";
        puts("");
        putchar('!');
        putchar('\n');
        EXPECT(LinesOf(screen).size() == 3 && LinesOf(screen)[1] == Line({ { kGrey, "printed to stdout and cout" } })
            && LinesOf(screen)[2] == Line({ { kGrey, "!" } }));
        CLibraryPrintf()("printed to the C library's stdout\n");
        fflush(CLibraryStdout());
        EXPECT(LinesOf(screen).size() == 3);
        // Console::Clear.
        std::cout << "\x1b[2J" "\x1b[H" "\x1b[3J";
        EXPECT(LinesOf(screen).empty());

        screen.SetSize(80, 25);
        CONSOLE_SCREEN_BUFFER_INFOEX info;
        info.cbSize = sizeof(info);
        EXPECT(GetConsoleScreenBufferInfoEx(out, &info) == TRUE && info.dwSize.X == 80 && info.dwSize.Y == 25
            && info.srWindow.Right == 79 && info.srWindow.Bottom == 24 && info.wAttributes == (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE));

        EXPECT(FreeConsole() == TRUE && window.Closed == 1 && !bg3se::shim::ConsoleIsWindow());
        // std::cout and stdout write where they did before.
        EXPECT(stdout == CLibraryStdout());
        std::cout << "after" << std::endl;
        printf("and printf\n");
        fflush(stdout);
        EXPECT(LinesOf(screen).empty());
        // Without a window there is no console again.
        bg3se::shim::SetWindowConsole(nullptr);
        EXPECT(AllocConsole() == FALSE && GetLastError() == ERROR_NOT_SUPPORTED);
        child.Say("done");
    });
    int status = run.Finish();
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(Has(run.Said, "done"));
    run.CountFailures();
    CHECK(run.Sent == "printed to the C library's stdout\nafter\nand printf\n");
}

// Norbyte's console thread on a window: ConsoleThread waits for Enter one
// character at a time, InputLoop prints its prompt and reads a line; other
// threads print meanwhile.
void a_window_console_hands_its_lines_to_the_console_thread()
{
    gTest = "a_window_console_hands_its_lines_to_the_console_thread";
    Run run = Start(Streams::None, [](Child const& child) {
        auto& screen = bg3se::shim::WindowConsoleScreen();
        CountingWindow window;
        bg3se::shim::SetWindowConsole(&window);
        EXPECT(AllocConsole() == TRUE);
        FILE* stream = nullptr;
        EXPECT(freopen_s(&stream, "CONOUT$", "w", stdout) == 0);

        std::mutex lock;
        std::vector<std::string> commands;
        std::atomic<bool> ended{ false };
        std::thread console([&] {
            HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
            wchar_t c = 0;
            DWORD read = 0;
            // ConsoleThread: what is typed before Enter is not a command.
            while (ReadConsoleW(in, &c, 1, &read, NULL) && !(read == 1 && c == L'\r')) {}
            std::cout << "Entering server Lua console." << std::endl;
            for (;;) {
                std::cout << "S";
                std::cout << " >> ";
                std::cout.flush();
                std::string line;
                if (!bg3se::mac::ReadConsoleLine(line)) break;
                std::lock_guard _(lock);
                commands.push_back(line);
                if (line == "exit") break;
            }
            ended = true;
        });
        auto until = [&](auto done) {
            for (int i = 0; i < 1000 && !done(); i++) usleep(2000);
            return done();
        };
        EXPECT(until([&] { return screen.Look().Reading; }));
        EXPECT(screen.Look().Prompt.empty());
        screen.Type("typed before the console was entered");
        EXPECT(until([&] { auto v = screen.Look(); return v.Reading && v.Prompt == Line({ { kGrey, "S >> " } }); }));
        // Others print while the console waits; their lines are their own.
        std::thread([] { std::cout << "a log line" << std::endl; }).join();
        screen.Type("print(1)");
        screen.Type("!cmd arg");
        screen.Type("exit");
        EXPECT(until([&] { return ended.load(); }));
        console.join();
        EXPECT((commands == std::vector<std::string>{ "print(1)", "!cmd arg", "exit" }));
        std::vector<std::string> shown;
        for (auto const& line : LinesOf(screen)) shown.push_back(TextOf(line));
        EXPECT((shown == std::vector<std::string>{ "typed before the console was entered", "Entering server Lua console.", "a log line",
            "S >> print(1)", "S >> !cmd arg", "S >> exit" }));

        // A console that is freed lets its reader go.
        std::atomic<int> result{ -1 };
        std::thread waiting([&] {
            wchar_t c = 0;
            DWORD read = 0;
            result = ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), &c, 1, &read, NULL);
        });
        EXPECT(until([&] { return screen.Look().Reading; }));
        EXPECT(FreeConsole() == TRUE);
        waiting.join();
        EXPECT(result == FALSE);
        child.Say("done");
    });
    int status = run.Finish();
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(Has(run.Said, "done"));
    run.CountFailures();
    CHECK(run.Sent.empty());
}

void message_box_is_an_alert_with_the_same_buttons()
{
    gTest = "message_box_is_an_alert_with_the_same_buttons";
    using bg3se::shim::BuildMessageBoxAlert;
    using bg3se::shim::MessageBoxResult;
    // CFUserNotification's levels and responses, as its header numbers them.
    constexpr unsigned long kStop = 0, kNote = 1, kCaution = 2, kPlain = 3;
    constexpr unsigned long kDefault = 0, kAlternate = 1, kOther = 2, kCancelled = 3;

    // Fail() (CoreLib/Utils.cpp).
    auto fail = BuildMessageBoxAlert("Could not load the library", "BG3 Script Extender Error", MB_OK | MB_ICONERROR);
    CHECK(fail.Valid && fail.Level == kStop);
    CHECK(fail.Caption == "BG3 Script Extender Error" && fail.Text == "Could not load the library");
    CHECK(fail.Buttons[0] && strcmp(fail.Buttons[0], "OK") == 0 && fail.Buttons[1] == nullptr && fail.Buttons[2] == nullptr);
    CHECK(MessageBoxResult(fail, kDefault) == IDOK && MessageBoxResult(fail, kCancelled) == IDOK);
    // A response for a button the alert does not have is the alert going away.
    CHECK(MessageBoxResult(fail, kAlternate) == IDOK && MessageBoxResult(fail, kOther) == IDOK);
    // LibraryManager::ShowStartupError's fallback: the modality flags change nothing.
    auto startup = BuildMessageBoxAlert("text", "Script Extender Error", MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND);
    CHECK(startup.Valid && startup.Level == kStop && startup.Buttons[1] == nullptr);

    auto yesNoCancel = BuildMessageBoxAlert("t", "c", MB_YESNOCANCEL | MB_ICONQUESTION);
    CHECK(yesNoCancel.Level == kNote);
    CHECK(strcmp(yesNoCancel.Buttons[0], "Yes") == 0 && strcmp(yesNoCancel.Buttons[1], "No") == 0 && strcmp(yesNoCancel.Buttons[2], "Cancel") == 0);
    CHECK(MessageBoxResult(yesNoCancel, kDefault) == IDYES && MessageBoxResult(yesNoCancel, kAlternate) == IDNO);
    CHECK(MessageBoxResult(yesNoCancel, kOther) == IDCANCEL && MessageBoxResult(yesNoCancel, kCancelled) == IDCANCEL);
    // The response's upper bits are not the button.
    CHECK(MessageBoxResult(yesNoCancel, kAlternate | 0x100) == IDNO);

    // The default button comes first, the others keep their order.
    auto second = BuildMessageBoxAlert("t", "c", MB_ABORTRETRYIGNORE | MB_DEFBUTTON2 | MB_ICONWARNING);
    CHECK(second.Level == kCaution);
    CHECK(strcmp(second.Buttons[0], "Retry") == 0 && strcmp(second.Buttons[1], "Abort") == 0 && strcmp(second.Buttons[2], "Ignore") == 0);
    CHECK(MessageBoxResult(second, kDefault) == IDRETRY && MessageBoxResult(second, kAlternate) == IDABORT && MessageBoxResult(second, kOther) == IDIGNORE);
    // No Cancel button: going away is the default button.
    CHECK(MessageBoxResult(second, kCancelled) == IDRETRY);
    auto third = BuildMessageBoxAlert("t", "c", MB_CANCELTRYCONTINUE | MB_DEFBUTTON3);
    CHECK(strcmp(third.Buttons[0], "Continue") == 0 && strcmp(third.Buttons[1], "Cancel") == 0 && strcmp(third.Buttons[2], "Try Again") == 0);
    CHECK(MessageBoxResult(third, kDefault) == IDCONTINUE && MessageBoxResult(third, kOther) == IDTRYAGAIN && MessageBoxResult(third, kCancelled) == IDCANCEL);
    // A default button the type does not have is the first.
    auto noThird = BuildMessageBoxAlert("t", "c", MB_YESNO | MB_DEFBUTTON3);
    CHECK(strcmp(noThird.Buttons[0], "Yes") == 0 && strcmp(noThird.Buttons[1], "No") == 0 && noThird.Buttons[2] == nullptr);
    CHECK(MessageBoxResult(noThird, kAlternate) == IDNO);

    auto okCancel = BuildMessageBoxAlert("t", "c", MB_OKCANCEL | MB_ICONINFORMATION);
    CHECK(okCancel.Level == kNote && MessageBoxResult(okCancel, kAlternate) == IDCANCEL && MessageBoxResult(okCancel, kCancelled) == IDCANCEL);
    auto retryCancel = BuildMessageBoxAlert("t", "c", MB_RETRYCANCEL);
    CHECK(retryCancel.Level == kPlain && MessageBoxResult(retryCancel, kDefault) == IDRETRY && MessageBoxResult(retryCancel, kAlternate) == IDCANCEL);

    // No caption is MessageBox's "Error"; no text is an empty alert.
    auto bare = BuildMessageBoxAlert(nullptr, nullptr, MB_OK);
    CHECK(bare.Valid && bare.Caption == "Error" && bare.Text.empty());
    // A set of buttons MessageBox does not have.
    CHECK(!BuildMessageBoxAlert("t", "c", 0x7).Valid && !BuildMessageBoxAlert("t", "c", 0xF).Valid);
    SetLastError(0);
    CHECK(MessageBoxA(NULL, "never shown", "never shown", 0x7) == 0 && GetLastError() == ERROR_INVALID_PARAMETER);
    SetLastError(0);
    CHECK(MessageBoxW(NULL, L"never shown", L"never shown", 0xF) == 0 && GetLastError() == ERROR_INVALID_PARAMETER);
}

}

// The number of tests run; the failures are added to `failures`.
size_t RunConsoleTests(int& failures)
{
    void (*tests[])() = {
        attributes_are_colour_sequences,
        console_is_the_terminal_the_process_was_started_from,
        console_input_is_what_is_typed_at_the_terminal,
        console_lines_are_read_through_the_console,
        without_a_terminal_there_is_no_console,
        a_background_job_is_not_stopped_by_its_console,
        the_screen_keeps_lines_in_their_colours,
        the_screen_is_bounded,
        the_lines_of_threads_are_not_mixed,
        clearing_the_screen_keeps_the_prompt,
        what_is_typed_is_read_and_echoed,
        without_a_terminal_a_window_is_the_console,
        a_window_console_hands_its_lines_to_the_console_thread,
        message_box_is_an_alert_with_the_same_buttons,
    };
    for (auto test : tests) test();
    failures += gFailures;
    return std::size(tests);
}
