#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

// The console's screen when the console is a window (Win32Console.cpp): for a
// process started without a terminal, a window of the extender's overlay
// (Mac/Overlay/ConsoleWindow.h) shows it and types into it.
//
// What Norbyte's console writes to it is what a Windows console is given:
// text through std::cout, its colours through SetConsoleTextAttribute, and
// the escape sequences of Console::Clear and of whatever a script prints
// (SGR colours, which mods use). The screen keeps what that shows: lines of
// coloured runs on their backgrounds, the newest kMostLines of them and
// kMostBytes of text at most. A line too long for kMostLineBytes goes on in the next one.
//
// Many threads write at once. Each writer has its own line in progress and
// its own colour, so that the lines of two threads are never mixed into one,
// and a line is shown once its end has arrived. The one exception is the
// line of the thread that reads the console: what it wrote before it began to
// read is its prompt ("S >> "), which the window shows beside the text being
// typed, and which is the start of the line that typing makes.
//
// What is typed arrives a line at a time (Type) and is read as a Windows
// console's input is: UTF-32 characters, a line end as "\r\n", read from a
// thread that waits for them (Read). The line read becomes a line of the
// screen, after the prompt, as a console echoes what is typed.
//
// Every call takes the screen's lock for a moment and calls nothing outside
// it while it holds it: in particular nothing that logs, since a log line is
// written to the screen.

namespace bg3se::shim {

// The background of text nobody gave one: the window's own.
inline constexpr uint32_t kNoBackground = 0xFF000000u;

// A run of text of one colour, 0xRRGGBB, on one background, 0xRRGGBB or
// kNoBackground.
struct ConsoleRun
{
    uint32_t Colour{ 0 };
    std::string Text;
    uint32_t Background{ kNoBackground };

    bool operator==(ConsoleRun const&) const = default;
};

using ConsoleLine = std::vector<ConsoleRun>;

// The colour of a Windows console attribute's text (its low four bits): the
// palette of the Windows console's default scheme.
uint32_t ConsoleColourOf(uint16_t attributes);
// The colour of its background (bits 4 to 7), kNoBackground for black, which
// is the background of a console nobody coloured.
uint32_t ConsoleBackgroundOf(uint16_t attributes);

class ConsoleScreen
{
public:
    // As many lines as a Windows console's screen buffer has rows unless its
    // user changed it (9001), and no more text than a window can hold.
    static constexpr size_t kMostLines = 9001;
    static constexpr size_t kMostBytes = 8u << 20;
    static constexpr size_t kMostLineBytes = 4096;

    // --- the writers: any thread --------------------------------------------

    // Text written by `writer`: UTF-8 with control characters and escape
    // sequences, in pieces of any size.
    void Write(std::thread::id writer, std::string_view text);
    // The attributes of what `writer` writes from now on
    // (SetConsoleTextAttribute). Each writer starts with the default ones.
    void SetAttributes(std::thread::id writer, uint16_t attributes);
    // Every line goes (the escape sequences ESC[2J and ESC[3J do the same).
    // A prompt stays: the reader is still waiting.
    void Clear();
    void SetTitle(std::string title);

    // --- the reader: the thread that waits for what is typed -----------------

    // Waits until something has been typed and takes up to `count`
    // characters of it. False, with nothing taken, once the screen is closed.
    bool Read(wchar_t* out, size_t count, size_t& taken);

    // --- the window -----------------------------------------------------------

    // A line typed, without its line end, which is added.
    void Type(std::string_view line);

    // The lines kept are the ones numbered [First, End): a line keeps its
    // number until it is dropped, so that a window can ask for the part it
    // shows. Cleared counts the clears.
    struct View
    {
        uint64_t First{ 0 }, End{ 0 };
        uint64_t Cleared{ 0 };
        // What the reader wrote before it began to read, while it waits.
        ConsoleLine Prompt;
        bool Reading{ false };
        std::string Title;
    };
    View Look();
    // Lines [from, to) as far as they are still kept, appended to `out`.
    // Returns the number of the first line appended.
    uint64_t Copy(uint64_t from, uint64_t to, std::vector<ConsoleLine>& out);
    // The size of the window in characters, for GetConsoleScreenBufferInfoEx.
    void SetSize(uint16_t columns, uint16_t rows);
    void Size(uint16_t& columns, uint16_t& rows);

    // --- the console ---------------------------------------------------------

    // Opened when the console is allocated, closed when it is freed: a reader
    // that waits is woken and gets nothing. Opening empties what typing left.
    void Open();
    void Close();
    bool IsOpen();

    // For the tests: how many bytes of text the lines kept hold, and how many
    // writers have a state of their own.
    size_t Bytes();
    size_t Writers();

private:
    // How far into an escape sequence a writer is.
    enum class Escape : uint8_t { None, Start, Csi, Osc, OscEnd };

    struct Writer
    {
        uint16_t Attributes{ 0x7 };
        // A colour set by an SGR sequence, in place of the attributes'.
        bool HasColour{ false };
        uint32_t Colour{ 0 };
        bool Bold{ false };
        // A basic colour (0-7) of SGR 30-37, which bold makes bright.
        int Basic{ -1 };
        // A background set by an SGR sequence, in place of the attributes'.
        bool HasBackground{ false };
        uint32_t Background{ 0 };
        // A basic background (0-7) of SGR 40-47.
        int BasicBackground{ -1 };
        // SGR 7: the colour and the background swapped.
        bool Reverse{ false };
        Escape State{ Escape::None };
        std::string Parameters;
        ConsoleLine Line;
        size_t LineBytes{ 0 };
    };

    Writer& WriterOf(std::thread::id writer);
    void Forget(std::thread::id writer);
    static uint32_t ForegroundOf(Writer const& writer);
    static uint32_t UnderOf(Writer const& writer);
    static bool Reversed(Writer const& writer);
    uint32_t ColourOf(Writer const& writer) const;
    uint32_t BackgroundOf(Writer const& writer) const;
    void Append(Writer& writer, char const* text, size_t length);
    void Commit(Writer& writer);
    void Commit(ConsoleLine line);
    void EndOf(Writer& writer, char final);
    void Select(Writer& writer);
    void ClearLines();

    std::mutex mutex_;
    std::deque<ConsoleLine> lines_;
    uint64_t first_{ 0 };
    uint64_t cleared_{ 0 };
    size_t bytes_{ 0 };
    std::unordered_map<std::thread::id, Writer> writers_;
    std::string title_;
    uint16_t columns_{ 120 }, rows_{ 30 };

    // What was typed and not read, and the line being read.
    std::condition_variable typed_;
    std::deque<wchar_t> input_;
    bool open_{ false };
    bool reading_{ false };
    ConsoleLine prompt_;
    std::u32string echo_;
};

// The screen of the process's console, when it is a window.
ConsoleScreen& WindowConsoleScreen();

// Who shows the screen: the overlay's console window. While one is set, a
// process without a terminal gets a console from AllocConsole, and that
// console is the screen; without one it gets none, as before.
class WindowConsole
{
public:
    virtual ~WindowConsole() = default;
    // AllocConsole made the screen the console, or FreeConsole ended it.
    virtual void ConsoleOpened() = 0;
    virtual void ConsoleClosed() = 0;
};

// nullptr: no window. Set before the console is allocated.
void SetWindowConsole(WindowConsole* window);
// Whether the console is allocated and is the screen.
bool ConsoleIsWindow();

}
