// The Win32 console functions, for a process whose console is the terminal it
// was started from, or a window of its own.
//
// A Windows GUI process has no console until it makes one with AllocConsole.
// A macOS process cannot make a terminal: it was started from one or it has
// none. So here the console is that terminal, when standard input and
// standard output are both a terminal. Otherwise (the game started from Steam
// or the Finder) it is a window, when something shows one (the extender's
// overlay: SetWindowConsole, ConsoleScreen.h), and there is none when nothing
// does. AllocConsole says which it is; until it has succeeded, and after
// FreeConsole, the process has no console and every function below that
// needs one fails with ERROR_INVALID_HANDLE.
//
// A window console is a ConsoleScreen. What a Windows process writes to its
// console's output, it writes here to std::cout, once freopen_s has opened
// "CONOUT$" on it: that stream then writes to the screen. The colours, the
// title, the modes and the size are the screen's; ReadConsoleW reads what is
// typed into the window. The rest of this comment is about a terminal.
//
// What a process can do to a terminal is less than what it can do to a
// Windows console, and each function says what it does:
//
//   colours, the title, wrapping and the character set are escape sequences
//   written to the terminal; the terminal always interprets them, so
//   ENABLE_VIRTUAL_TERMINAL_PROCESSING is on whether asked for or not;
//
//   processed output and the input modes are the terminal driver's settings
//   (termios), put back by FreeConsole as AllocConsole found them.
//
// The font is the terminal user's: there are no font functions, and
// Norbyte's Console::Create sets none outside MSVC.
//
// The game may run as a background job of its shell. Changing a terminal's
// settings or reading from it would then stop the whole process (SIGTTOU,
// SIGTTIN); both are done with those signals blocked for the calling thread,
// which makes the first go through and the second fail with EIO: a read from
// the background waits until the process is in the foreground again.

#include <windows.h>
#include <ConsoleScreen.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <os/log.h>
#include <poll.h>
#include <string>
#include <streambuf>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr DWORD kOutputModes = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
// The input modes this layer knows: ENABLE_PROCESSED_INPUT, ENABLE_LINE_INPUT, ENABLE_ECHO_INPUT.
constexpr DWORD kProcessedInput = 0x1, kLineInput = 0x2, kEchoInput = 0x4;
constexpr DWORD kInputModes = kProcessedInput | kLineInput | kEchoInput;
// The attributes of a console nobody has coloured: grey on black, which a
// terminal shows as its own default colours.
constexpr WORD kDefaultAttributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

// A standard handle is the address of the entry for its descriptor.
struct StdStream
{
    int Descriptor;
};
StdStream gStreams[3] = { { STDIN_FILENO }, { STDOUT_FILENO }, { STDERR_FILENO } };

// The console's state. Held for the length of a call, never across a read.
std::mutex gLock;
bool gAllocated = false;
termios gFoundInput{}, gFoundOutput{};      // the settings AllocConsole found
bool gFoundInputKnown = false, gFoundOutputKnown = false;
struct sigaction gFoundInterrupt{};         // SIGINT, before SetConsoleCtrlHandler(NULL, TRUE)
bool gInterruptIgnored = false;
DWORD gOutputMode = kOutputModes;
WORD gAttributes = kDefaultAttributes;

// Who shows a window console, and whether the console is one.
bg3se::shim::WindowConsole* gWindow = nullptr;
bool gIsWindow = false;
DWORD gInputMode = kInputModes;

// std::cout, once "CONOUT$" is opened on a window console: unbuffered, so
// that each write reaches the screen as the writer's own, from any thread.
class ScreenStream final : public std::streambuf
{
protected:
    int_type overflow(int_type c) override
    {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        char one = traits_type::to_char_type(c);
        bg3se::shim::WindowConsoleScreen().Write(std::this_thread::get_id(), std::string_view(&one, 1));
        return c;
    }

    std::streamsize xsputn(char const* text, std::streamsize count) override
    {
        if (count > 0) bg3se::shim::WindowConsoleScreen().Write(std::this_thread::get_id(), std::string_view(text, (size_t)count));
        return count;
    }
};
// As long-lived as the screen: std::cout may still write to it while the process exits.
ScreenStream& TheScreenStream()
{
    static auto* stream = new ScreenStream();
    return *stream;
}
std::streambuf* gFoundCout = nullptr;      // std::cout's buffer before

// The image's own stdout (OwnStdout.cpp), once "CONOUT$" is opened on a window
// console: unbuffered, as the Windows runtime's stdout on a console is, so
// that each write reaches the screen as the writer's own, from any thread,
// and printf and std::cout of one thread stay in the order they were called.
int WriteToScreen(void*, char const* text, int length)
{
    if (length > 0) bg3se::shim::WindowConsoleScreen().Write(std::this_thread::get_id(), std::string_view(text, (size_t)length));
    return length;
}
// As long-lived as the screen.
FILE* TheScreenFile()
{
    static FILE* file = [] {
        FILE* made = funopen(nullptr, nullptr, &WriteToScreen, nullptr, nullptr);
        if (made) setvbuf(made, nullptr, _IONBF, 0);
        return made;
    }();
    return file;
}
FILE* gFoundStdout = nullptr;              // the image's stdout before

// What ReadConsoleW has read from the terminal and not handed out yet.
std::mutex gReadLock;
std::deque<wchar_t> gPending;
std::string gPartial;       // the start of a UTF-8 sequence whose end has not arrived

BOOL Refuse(DWORD error)
{
    SetLastError(error);
    return FALSE;
}

StdStream const* StreamOf(HANDLE handle)
{
    for (auto const& stream : gStreams) {
        if (handle == &stream) return &stream;
    }
    return nullptr;
}

// The stream of a console handle that writes, or nullptr.
StdStream const* OutputOf(HANDLE handle)
{
    auto stream = StreamOf(handle);
    return stream && stream->Descriptor != STDIN_FILENO ? stream : nullptr;
}

// Blocks, for the calling thread, the signal that stops a background job
// which touches its terminal.
class NotStopped
{
public:
    explicit NotStopped(int signal)
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, signal);
        pthread_sigmask(SIG_BLOCK, &set, &before_);
    }
    ~NotStopped() { pthread_sigmask(SIG_SETMASK, &before_, nullptr); }

private:
    sigset_t before_;
};

bool ReadSettings(int descriptor, termios& settings)
{
    return tcgetattr(descriptor, &settings) == 0;
}

bool WriteSettings(int descriptor, termios const& settings)
{
    NotStopped guard(SIGTTOU);
    int result;
    do {
        result = tcsetattr(descriptor, TCSANOW, &settings);
    } while (result != 0 && errno == EINTR);
    return result == 0;
}

// Writes control text to the terminal behind whatever the C library has
// buffered for it, so that text printed before the call keeps what was in
// force when it was printed.
bool WriteControl(int descriptor, char const* text, size_t length)
{
    if (descriptor == STDOUT_FILENO) fflush(stdout);
    while (length > 0) {
        ssize_t wrote = write(descriptor, text, length);
        if (wrote < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return false;
        }
        text += wrote;
        length -= static_cast<size_t>(wrote);
    }
    return true;
}

bool WriteControl(int descriptor, std::string const& text)
{
    return WriteControl(descriptor, text.data(), text.size());
}

// Appends the code points of `bytes` to gPending, a line end as "\r\n" (what
// a Windows console hands out for the Enter key). An incomplete sequence at
// the end waits in gPartial for the next read.
void Decode(char const* bytes, size_t length)
{
    gPartial.append(bytes, length);
    size_t complete = gPartial.size();
    // At most the last three bytes can be the start of a sequence that goes on.
    for (size_t back = 1; back <= 3 && back <= gPartial.size(); back++) {
        auto c = static_cast<unsigned char>(gPartial[gPartial.size() - back]);
        if ((c & 0xC0) == 0x80) continue;       // a continuation byte: look further back
        size_t needs = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (needs > back) complete = gPartial.size() - back;
        break;
    }
    if (complete == 0) return;

    int count = MultiByteToWideChar(CP_UTF8, 0, gPartial.data(), static_cast<int>(complete), nullptr, 0);
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, gPartial.data(), static_cast<int>(complete), wide.data(), count);
    gPartial.erase(0, complete);
    for (wchar_t c : wide) {
        if (c == L'\n' && (gPending.empty() || gPending.back() != L'\r')) gPending.push_back(L'\r');
        gPending.push_back(c);
    }
}

// The terminal is gone (its window closed, the connection lost) and the
// process lives on: no input will ever arrive, and the call waits as a read
// does on a console nobody types at.
[[noreturn]] void WaitForever()
{
    for (;;) pause();
}

// The escape sequence for a console's character attributes: the low four
// bits are the text colour, the next four the background, each blue, green,
// red and intensity. Grey on black, the attributes of an untouched console,
// is the terminal's own default pair rather than those two colours.
std::string AttributeSequence(WORD attributes)
{
    auto ansi = [](unsigned windows) { return ((windows & 4) ? 1 : 0) | ((windows & 2) ? 2 : 0) | ((windows & 1) ? 4 : 0); };
    unsigned text = attributes & 0xF, background = (attributes >> 4) & 0xF;
    std::string sequence = "\x1b[0";
    if (text != kDefaultAttributes) {
        sequence += ';';
        sequence += std::to_string(((text & FOREGROUND_INTENSITY) ? 90 : 30) + ansi(text));
    }
    if (background != 0) {
        sequence += ';';
        sequence += std::to_string(((background & FOREGROUND_INTENSITY) ? 100 : 40) + ansi(background));
    }
    sequence += 'm';
    return sequence;
}

os_log_t DebugLog()
{
    static os_log_t log = os_log_create("bg3mse", "debug-output");
    return log;
}

}

namespace bg3se::shim {

// For the tests: the sequence SetConsoleTextAttribute writes.
std::string ConsoleAttributeSequence(WORD attributes)
{
    return AttributeSequence(attributes);
}

}

// TRUE when standard input and standard output are a terminal: that terminal
// is the process's console from here on; otherwise, when a window shows a
// console (SetWindowConsole), the window's screen is. ERROR_ACCESS_DENIED
// when the process already has its console (as on Windows),
// ERROR_NOT_SUPPORTED when it was not started from a terminal and there is
// no window.
BOOL AllocConsole()
{
    bg3se::shim::WindowConsole* window = nullptr;
    {
        std::lock_guard lock(gLock);
        if (gAllocated) return Refuse(ERROR_ACCESS_DENIED);
        if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)) {
            gFoundInputKnown = ReadSettings(STDIN_FILENO, gFoundInput);
            gFoundOutputKnown = ReadSettings(STDOUT_FILENO, gFoundOutput);
            gOutputMode = kOutputModes;
            gAttributes = kDefaultAttributes;
            gIsWindow = false;
            gAllocated = true;
            return TRUE;
        }
        if (!gWindow) return Refuse(ERROR_NOT_SUPPORTED);
        gOutputMode = kOutputModes;
        gInputMode = kInputModes;
        gAttributes = kDefaultAttributes;
        bg3se::shim::WindowConsoleScreen().Open();
        gIsWindow = true;
        gAllocated = true;
        window = gWindow;
    }
    // Told without the console's lock, which the window may need.
    window->ConsoleOpened();
    return TRUE;
}

// The process has no console any more. The terminal gets back the settings
// AllocConsole found, its default colours, and Ctrl+C as it was; a window's
// reader stops waiting, and std::cout and stdout write where they did before.
BOOL FreeConsole()
{
    bg3se::shim::WindowConsole* window = nullptr;
    {
        std::lock_guard lock(gLock);
        if (!gAllocated) return Refuse(ERROR_INVALID_PARAMETER);
        if (!gIsWindow) {
            if (gAttributes != kDefaultAttributes) WriteControl(STDOUT_FILENO, AttributeSequence(kDefaultAttributes));
            if (gFoundInputKnown) WriteSettings(STDIN_FILENO, gFoundInput);
            if (gFoundOutputKnown) WriteSettings(STDOUT_FILENO, gFoundOutput);
            if (gInterruptIgnored) {
                sigaction(SIGINT, &gFoundInterrupt, nullptr);
                gInterruptIgnored = false;
            }
            gAllocated = false;
            return TRUE;
        }
        if (gFoundCout) {
            std::cout.flush();
            std::cout.rdbuf(gFoundCout);
            gFoundCout = nullptr;
        }
        if (gFoundStdout) {
            fflush(stdout);
            stdout = gFoundStdout;
            gFoundStdout = nullptr;
        }
        bg3se::shim::WindowConsoleScreen().Close();
        gIsWindow = false;
        gAllocated = false;
        window = gWindow;
    }
    if (window) window->ConsoleClosed();
    return TRUE;
}

// The console's handle for descriptor 0, 1 or 2; NULL while the process has
// no console, as for a Windows process without standard handles.
HANDLE GetStdHandle(DWORD stdHandle)
{
    int index = stdHandle == STD_INPUT_HANDLE ? 0 : stdHandle == STD_OUTPUT_HANDLE ? 1 : stdHandle == STD_ERROR_HANDLE ? 2 : -1;
    if (index < 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return INVALID_HANDLE_VALUE;
    }
    std::lock_guard lock(gLock);
    return gAllocated ? &gStreams[index] : NULL;
}

BOOL GetConsoleMode(HANDLE console, DWORD* mode)
{
    std::lock_guard lock(gLock);
    auto stream = StreamOf(console);
    if (!gAllocated || !stream) return Refuse(ERROR_INVALID_HANDLE);
    if (!mode) return Refuse(ERROR_INVALID_PARAMETER);
    if (stream->Descriptor != STDIN_FILENO) {
        *mode = gOutputMode;
        return TRUE;
    }
    if (gIsWindow) {
        *mode = gInputMode;
        return TRUE;
    }
    termios settings{};
    if (!ReadSettings(STDIN_FILENO, settings)) return Refuse(ERROR_INVALID_HANDLE);
    *mode = ((settings.c_lflag & ISIG) ? kProcessedInput : 0) | ((settings.c_lflag & ICANON) ? kLineInput : 0)
        | ((settings.c_lflag & ECHO) ? kEchoInput : 0);
    return TRUE;
}

// An output handle: ENABLE_PROCESSED_OUTPUT is the driver's output
// processing (a line feed also returns the carriage), ENABLE_WRAP_AT_EOL_OUTPUT
// the terminal's automatic wrapping. An input handle: processed input is the
// driver's signal keys, line input its line editing, echo its echo. A flag
// outside those fails with ERROR_INVALID_PARAMETER.
BOOL SetConsoleMode(HANDLE console, DWORD mode)
{
    std::lock_guard lock(gLock);
    auto stream = StreamOf(console);
    if (!gAllocated || !stream) return Refuse(ERROR_INVALID_HANDLE);

    if (gIsWindow) {
        // A window keeps the modes it is given and works the same in each:
        // it wraps nothing, takes the escape sequences it knows, and hands
        // over a line at a time, which it echoes.
        bool input = stream->Descriptor == STDIN_FILENO;
        if (mode & ~(input ? kInputModes : kOutputModes)) return Refuse(ERROR_INVALID_PARAMETER);
        if (input) gInputMode = mode;
        else gOutputMode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        return TRUE;
    }

    termios settings{};
    if (stream->Descriptor == STDIN_FILENO) {
        if (mode & ~kInputModes) return Refuse(ERROR_INVALID_PARAMETER);
        if (!ReadSettings(STDIN_FILENO, settings)) return Refuse(ERROR_INVALID_HANDLE);
        auto set = [&](tcflag_t flag, bool on) { settings.c_lflag = on ? (settings.c_lflag | flag) : (settings.c_lflag & ~flag); };
        set(ISIG, mode & kProcessedInput);
        set(ICANON, mode & kLineInput);
        set(ECHO, mode & kEchoInput);
        return WriteSettings(STDIN_FILENO, settings) ? TRUE : Refuse(ERROR_INVALID_HANDLE);
    }

    if (mode & ~kOutputModes) return Refuse(ERROR_INVALID_PARAMETER);
    if (!ReadSettings(stream->Descriptor, settings)) return Refuse(ERROR_INVALID_HANDLE);
    if (mode & ENABLE_PROCESSED_OUTPUT) settings.c_oflag |= OPOST | ONLCR;
    else settings.c_oflag &= ~static_cast<tcflag_t>(OPOST);
    if (!WriteSettings(stream->Descriptor, settings)) return Refuse(ERROR_INVALID_HANDLE);
    char const* wrap = (mode & ENABLE_WRAP_AT_EOL_OUTPUT) ? "\x1b[?7h" : "\x1b[?7l";
    if (!WriteControl(stream->Descriptor, wrap, strlen(wrap))) return Refuse(ERROR_INVALID_HANDLE);
    gOutputMode = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    return TRUE;
}

// The colours of the text written from now on. Only the eight colour bits
// have a meaning here; anything else fails with ERROR_INVALID_PARAMETER.
BOOL SetConsoleTextAttribute(HANDLE console, WORD attributes)
{
    std::lock_guard lock(gLock);
    auto stream = OutputOf(console);
    if (!gAllocated || !stream) return Refuse(ERROR_INVALID_HANDLE);
    if (attributes & ~0xFF) return Refuse(ERROR_INVALID_PARAMETER);
    if (gIsWindow) {
        // The colour of the calling thread's text: each writer's lines are its own.
        bg3se::shim::WindowConsoleScreen().SetAttributes(std::this_thread::get_id(), attributes);
        gAttributes = attributes;
        return TRUE;
    }
    if (!WriteControl(stream->Descriptor, AttributeSequence(attributes))) return Refuse(ERROR_INVALID_HANDLE);
    gAttributes = attributes;
    return TRUE;
}

// The terminal window's title, as far as the terminal lets a program set it.
BOOL SetConsoleTitleW(LPCWSTR title)
{
    std::lock_guard lock(gLock);
    if (!gAllocated) return Refuse(ERROR_INVALID_HANDLE);
    if (!title) return Refuse(ERROR_INVALID_PARAMETER);
    size_t length = wcsnlen(title, 1024);
    int count = WideCharToMultiByte(CP_UTF8, 0, title, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, title, static_cast<int>(length), text.data(), count, nullptr, nullptr);
    // A control character would end the sequence or start another.
    std::erase_if(text, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
    if (gIsWindow) {
        bg3se::shim::WindowConsoleScreen().SetTitle(std::move(text));
        return TRUE;
    }
    return WriteControl(STDOUT_FILENO, "\x1b]0;" + text + "\a") ? TRUE : Refuse(ERROR_INVALID_HANDLE);
}

// SetConsoleCtrlHandler(NULL, TRUE): the process ignores Ctrl+C, which here
// means it ignores SIGINT, from the terminal or from anywhere else;
// (NULL, FALSE) puts back what was there. A handler routine is not
// supported (ERROR_NOT_SUPPORTED): nothing in the extender registers one.
BOOL SetConsoleCtrlHandler(PHANDLER_ROUTINE handler, BOOL add)
{
    std::lock_guard lock(gLock);
    if (!gAllocated) return Refuse(ERROR_INVALID_HANDLE);
    if (handler) return Refuse(ERROR_NOT_SUPPORTED);
    // A window has no Ctrl+C to ignore: its keys are typed, not signals.
    if (gIsWindow) return TRUE;
    if (add && !gInterruptIgnored) {
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        if (sigaction(SIGINT, &ignore, &gFoundInterrupt) != 0) return Refuse(ERROR_INVALID_PARAMETER);
        gInterruptIgnored = true;
    } else if (!add && gInterruptIgnored) {
        sigaction(SIGINT, &gFoundInterrupt, nullptr);
        gInterruptIgnored = false;
    }
    return TRUE;
}

// UTF-8 is the one code page this layer converts (Win32Strings.cpp).
BOOL IsValidCodePage(UINT codePage)
{
    return codePage == CP_UTF8;
}

// The console's input is UTF-8: the terminal driver is told, so that its line
// editing erases a whole character.
BOOL SetConsoleCP(UINT codePage)
{
    std::lock_guard lock(gLock);
    if (!gAllocated) return Refuse(ERROR_INVALID_HANDLE);
    if (codePage != CP_UTF8) return Refuse(ERROR_INVALID_PARAMETER);
    // A window's text is UTF-8 already.
    if (gIsWindow) return TRUE;
    termios settings{};
    if (!ReadSettings(STDIN_FILENO, settings)) return Refuse(ERROR_INVALID_HANDLE);
    settings.c_iflag |= IUTF8;
    return WriteSettings(STDIN_FILENO, settings) ? TRUE : Refuse(ERROR_INVALID_HANDLE);
}

// The console's output is UTF-8: the terminal is asked to read it so (ISO
// 2022's "select UTF-8"; a terminal that is always UTF-8 ignores it).
BOOL SetConsoleOutputCP(UINT codePage)
{
    std::lock_guard lock(gLock);
    if (!gAllocated) return Refuse(ERROR_INVALID_HANDLE);
    if (codePage != CP_UTF8) return Refuse(ERROR_INVALID_PARAMETER);
    if (gIsWindow) return TRUE;
    return WriteControl(STDOUT_FILENO, "\x1b%G", 3) ? TRUE : Refuse(ERROR_INVALID_HANDLE);
}

// Characters typed at the terminal, as UTF-32, a line end as "\r\n". The
// terminal hands over a line at a time (unless line input was turned off);
// what a call does not take stays for the next. TRUE with nothing read is
// the end-of-file key on an empty line. The last parameter must be NULL.
// A window hands over the lines typed into it, a line at a time whatever
// the input mode.
BOOL ReadConsoleW(HANDLE console, LPVOID buffer, DWORD charsToRead, LPDWORD charsRead, LPVOID inputControl)
{
    bool window;
    {
        std::lock_guard lock(gLock);
        auto stream = StreamOf(console);
        if (!gAllocated || !stream || stream->Descriptor != STDIN_FILENO) return Refuse(ERROR_INVALID_HANDLE);
        window = gIsWindow;
    }
    if (!buffer || !charsRead) return Refuse(ERROR_INVALID_PARAMETER);
    if (inputControl) return Refuse(ERROR_NOT_SUPPORTED);
    *charsRead = 0;
    if (charsToRead == 0) return TRUE;

    if (window) {
        // Waits for the window to be typed into, for as long as the console is there.
        size_t taken = 0;
        if (!bg3se::shim::WindowConsoleScreen().Read(static_cast<wchar_t*>(buffer), charsToRead, taken)) {
            return Refuse(ERROR_INVALID_HANDLE);
        }
        *charsRead = static_cast<DWORD>(taken);
        return TRUE;
    }

    std::lock_guard reading(gReadLock);
    while (gPending.empty()) {
        char bytes[1024];
        ssize_t got;
        {
            NotStopped guard(SIGTTIN);
            got = read(STDIN_FILENO, bytes, sizeof(bytes));
        }
        if (got > 0) {
            Decode(bytes, static_cast<size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        if (!isatty(STDIN_FILENO)) WaitForever();
        if (got == 0) {
            // The end-of-file key, unless the other end of the terminal has hung up.
            pollfd state{ STDIN_FILENO, POLLIN, 0 };
            if (poll(&state, 1, 0) > 0 && (state.revents & (POLLHUP | POLLERR | POLLNVAL))) WaitForever();
            return TRUE;
        }
        if (errno == EIO && tcgetpgrp(STDIN_FILENO) != getpgrp()) {
            // A background job may not read its terminal: until it is in the
            // foreground again.
            sleep(1);
            continue;
        }
        WaitForever();
    }

    auto out = static_cast<wchar_t*>(buffer);
    DWORD count = 0;
    while (count < charsToRead && !gPending.empty()) {
        out[count++] = gPending.front();
        gPending.pop_front();
    }
    *charsRead = count;
    return TRUE;
}

namespace bg3se::mac {

bool ReadConsoleLine(std::string& line)
{
    line.clear();
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring typed;
    bool first = true;
    for (;;) {
        wchar_t c = 0;
        DWORD read = 0;
        if (!ReadConsoleW(in, &c, 1, &read, NULL)) return false;
        if (read == 0) return false;
        // A line ends "\r\n". The caller may have taken the '\r' of the line
        // before this one itself (DebugConsole::ConsoleThread waits for one):
        // its '\n' is then the first thing here, and no line.
        if (c == L'\n' && first) {
            first = false;
            continue;
        }
        first = false;
        if (c == L'\r') {
            // The '\n' was queued with it.
            ReadConsoleW(in, &c, 1, &read, NULL);
            break;
        }
        if (c == L'\n') break;
        typed.push_back(c);
    }
    if (typed.empty()) return true;
    int size = WideCharToMultiByte(CP_UTF8, 0, typed.data(), static_cast<int>(typed.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return true;
    line.resize(static_cast<size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, typed.data(), static_cast<int>(typed.size()), line.data(), size, nullptr, nullptr);
    return true;
}

}

// The terminal's or the window's size, as a screen buffer exactly as large as its window.
// Where the cursor is and which colours the terminal shows cannot be asked
// without reading the terminal's answer from its input, so the cursor is
// reported at the origin and the colour table as zeros; the attributes are
// the ones last set.
BOOL GetConsoleScreenBufferInfoEx(HANDLE console, CONSOLE_SCREEN_BUFFER_INFOEX* info)
{
    std::lock_guard lock(gLock);
    auto stream = OutputOf(console);
    if (!gAllocated || !stream) return Refuse(ERROR_INVALID_HANDLE);
    if (!info || info->cbSize != sizeof(*info)) return Refuse(ERROR_INVALID_PARAMETER);
    winsize size{};
    if (gIsWindow) {
        // The characters the window has room for.
        bg3se::shim::WindowConsoleScreen().Size(size.ws_col, size.ws_row);
    } else if (ioctl(stream->Descriptor, TIOCGWINSZ, &size) != 0) {
        return Refuse(ERROR_INVALID_HANDLE);
    }
    auto columns = static_cast<SHORT>(size.ws_col > 0x7fff ? 0x7fff : size.ws_col);
    auto rows = static_cast<SHORT>(size.ws_row > 0x7fff ? 0x7fff : size.ws_row);
    *info = {};
    info->cbSize = sizeof(*info);
    info->dwSize = { columns, rows };
    info->wAttributes = gAttributes;
    info->srWindow = { 0, 0, static_cast<SHORT>(columns - 1), static_cast<SHORT>(rows - 1) };
    info->dwMaximumWindowSize = { columns, rows };
    return TRUE;
}

// The MSVC runtime's freopen_s. "CONOUT$" and "CONIN$" name the console: a
// stream that already is the terminal stays as it is, any other is opened on
// the terminal; without a console the call fails with ENOENT and, unlike a
// failed freopen, leaves the stream open, so that a process without a
// terminal keeps the standard streams it was started with. Any other path is
// freopen's. A null argument ends the process, as the runtime's default
// handler for an invalid parameter does. On a window console, "CONOUT$" for
// stdout makes std::cout and this image's stdout (OwnStdout.cpp) write to the
// window, as on Windows it does the extender's own runtime's stdout; the C
// library's stdout, which the game and every other library write to, stays.
int freopen_s(FILE** result, const char* path, const char* mode, FILE* stream)
{
    if (!result || !mode || !stream) {
        fprintf(stderr, "freopen_s: invalid parameter\n");
        abort();
    }
    *result = nullptr;
    if (path && (strcasecmp(path, "CONOUT$") == 0 || strcasecmp(path, "CONIN$") == 0)) {
        {
            std::lock_guard lock(gLock);
            if (!gAllocated) return ENOENT;
            if (gIsWindow) {
                // The C++ stream Norbyte's console writes to, and this
                // image's C one, are the window's from here. Nothing outside
                // MSVC reads std::cin or stdin.
                if (strcasecmp(path, "CONOUT$") == 0 && stream == stdout && !gFoundCout) {
                    std::cout.flush();
                    gFoundCout = std::cout.rdbuf(&TheScreenStream());
                    if (auto screen = TheScreenFile()) {
                        fflush(stdout);
                        gFoundStdout = stdout;
                        stdout = screen;
                    }
                }
                *result = stream == gFoundStdout ? stdout : stream;
                return 0;
            }
        }
        if (isatty(fileno(stream))) {
            *result = stream;
            return 0;
        }
        path = "/dev/tty";
    }
    *result = freopen(path, mode, stream);
    return *result ? 0 : errno;
}

// A string for whoever watches the process's debug output. On Windows that
// is an attached debugger or a viewer of the debug channel, and nobody
// otherwise; here it is the system log at its debug level, which is kept
// only while something is watching it (Console.app, `log stream --level
// debug --predicate 'subsystem == "bg3mse"'`). Never the terminal: the
// console prints the same text itself. A line is sent when its end arrives,
// which Norbyte's console passes in a call of its own.
void OutputDebugStringA(LPCSTR text)
{
    if (!text) return;
    thread_local char line[1024];
    thread_local size_t length = 0;
    auto send = [&] {
        while (length > 0 && (line[length - 1] == '\r' || line[length - 1] == '\n')) length--;
        line[length] = '\0';
        if (length > 0) os_log_with_type(DebugLog(), OS_LOG_TYPE_DEBUG, "%{public}s", line);
        length = 0;
    };
    for (; *text; text++) {
        line[length++] = *text;
        if (*text == '\n' || length == sizeof(line) - 1) send();
    }
}

namespace bg3se::shim {

void SetWindowConsole(WindowConsole* window)
{
    std::lock_guard lock(gLock);
    gWindow = window;
}

bool ConsoleIsWindow()
{
    std::lock_guard lock(gLock);
    return gAllocated && gIsWindow;
}

}
