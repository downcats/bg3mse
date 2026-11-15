#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ConsoleScreen.h>
#include <imgui.h>

#include <Overlay/InputEvents.h>
#include <Overlay/KeyCodes.h>

// The extender's console as a window of the overlay, for a game started
// without a terminal (from Steam or the Finder), where Windows would have
// opened a console window.
//
// The console is still Norbyte's (Extender/Shared/Console.cpp): his thread
// waits for what is typed and runs it as on Windows, and his messages are
// printed as there. Only the window is new: the Win32 console functions make
// it the console (Mac/Shim/Win32Console.cpp), it shows the console's screen
// (Mac/Shim/ConsoleScreen.h) and types into it what is entered in its line.
//
// The window is shown when the console is made (CreateConsole, or Ctrl+F12
// as on Windows), and a key, F6 unless bg3mse.conf names another, hides it
// and shows it again:
//
//   console_key = Ctrl+F7
//
// The key is named as ImGui names it ("F6", "GraveAccent", "Keypad0"), after
// any of the modifiers Ctrl, Shift and Alt (or Option), joined by '+'.
// F6 is bound by neither the game (the default bindings of its
// PlayerProfiles) nor the extender. The key is the window's only while the
// console exists; it is then never seen by the game or by Lua.
//
// Shown, the window takes the keyboard when its line is focused, as the
// overlay's other windows do: the line is focused when the key shows the
// window, and by a click; the keyboard is the game's again when the window
// is hidden or something else is clicked. The overlay is drawn only while a
// client Lua state has it (from the main menu on): when the window cannot be
// seen for that, the log says so the moment the key asks for it, and when the
// overlay is off in this game altogether, the moment the console is made.
//
// It is used as a Windows console window is (conhost's defaults, which
// AllocConsole gives a process): 120 columns by 30 rows at first, its lines
// wrapped at its width; text is selected with the mouse (a double click
// selects a word, Shift extends) and copied with Command+C, Enter or a right
// click, a right click without a selection pastes, Escape drops the
// selection or else empties the line, and Command+A selects everything while
// the line is not being typed into. A paste of several lines is typed as the
// Windows console types one: each line end is Enter, and what follows the
// last one stays in the line. A tab is typed into the line. Up and Down go
// through the last 50 lines entered.
//
// Threads: the key is taken on the main thread, the window is drawn on the
// client thread inside the overlay's ImGui frame, and what is typed is read
// by Norbyte's console thread.

namespace bg3se::mac::overlay {

inline constexpr char kConsoleKeySetting[] = "console_key";
inline constexpr char kDefaultConsoleKey[] = "F6";

// A key and the modifiers held with it (kFlagControl, kFlagShift, kFlagOption).
struct KeyChord
{
    uint16_t KeyCode{ 0 };
    uint32_t Modifiers{ 0 };

    bool operator==(KeyChord const&) const = default;
};

// The modifiers a chord can name; Command is not one of them: a key with
// Command held is the application's shortcut.
inline constexpr uint32_t kChordModifiers = kFlagControl | kFlagShift | kFlagOption;

// "Ctrl+Shift+F6" as a chord, or nothing and why not.
std::optional<KeyChord> ParseKeyChord(std::string_view text, std::string& problem);
std::string DescribeKeyChord(KeyChord chord);

class ConsoleWindow final : public shim::WindowConsole
{
public:
    // `overlayProblem` says why the overlay cannot be drawn in this game at
    // all, empty when it can; asked when the console is made.
    ConsoleWindow(shim::ConsoleScreen& screen, KeyChord key, std::function<std::string()> overlayProblem = {});

    // --- the main thread: every event the game is about to be sent ----------

    // Whether the event is the key's (pressed, held or let go): it shows or
    // hides the window, and goes nowhere else. `presenting` is whether the
    // overlay is drawn now.
    bool TakeEvent(CocoaEvent const& event, bool presenting);

    // --- the client thread, inside an ImGui frame -----------------------------

    void Draw();

    // --- the console (shim::WindowConsole) -----------------------------------

    void ConsoleOpened() override;
    void ConsoleClosed() override;

    bool Visible() const { return visible_.load(); }
    KeyChord Key() const { return key_; }

    // A place in the screen's text: a line's number and a byte of it.
    struct Place
    {
        uint64_t Line{ 0 };
        uint32_t Byte{ 0 };

        auto operator<=>(Place const&) const = default;
    };

    // What is selected, [from, to); empty when nothing is. For the tests.
    bool Selection(Place& from, Place& to) const;
    // The selected text, its lines ended by '\n' as the console copies them.
    std::string SelectedText();
    // Where the first row was drawn, and a row's height. For the tests.
    ImVec2 RowsOrigin() const { return origin_; }
    float RowHeight() const { return rowHeight_; }

private:
    // A row of the window: the part [Begin, End) of a line that fits its width.
    struct Row
    {
        uint64_t Line{ 0 };
        uint32_t Begin{ 0 }, End{ 0 };
    };

    static int OnInput(ImGuiInputTextCallbackData* data);
    void Keys(bool typing);
    void DrawLines(shim::ConsoleScreen::View const& view);
    void Rows(shim::ConsoleScreen::View const& view, float width);
    void Mouse(shim::ConsoleScreen::View const& view);
    Place At(ImVec2 point);
    void Copy();
    void Paste(std::string text);
    void Submit(std::string const& line);

    shim::ConsoleScreen& screen_;
    KeyChord const key_;
    std::function<std::string()> const overlayProblem_;
    std::atomic<bool> opened_{ false };
    std::atomic<bool> visible_{ false };
    std::atomic<bool> focus_{ false };

    // The main thread's.
    bool keyHeld_{ false };
    bool hiddenReported_{ false };
    bool closedReported_{ false };

    // The client thread's.
    char line_[4096]{};
    std::vector<std::string> history_;
    int historyAt_{ -1 };
    std::vector<shim::ConsoleLine> visibleLines_;
    // The window's rows, for the lines up to rowsEnd_, wrapped at rowsWidth_.
    std::vector<Row> rows_;
    uint64_t rowsEnd_{ 0 };
    uint64_t rowsCleared_{ UINT64_MAX };
    float rowsWidth_{ -1.0f }, rowsFont_{ -1.0f };
    // Where the rows were drawn: the top left of the first, and their height.
    ImVec2 origin_{};
    float rowHeight_{ 0.0f };
    // The selection: from where the mouse went down to where it is.
    Place anchor_, caret_;
    bool selecting_{ false };
    // What the line is to take in its next frame: a paste, or emptying.
    std::optional<std::string> paste_;
    bool emptyLine_{ false };
    // Enter copied the selection in this frame, and enters no line.
    bool enterTaken_{ false };
};

// --- the process's window ----------------------------------------------------

// Makes the window, with the key bg3mse.conf names, and lets the console be
// it. At the extender's start-up, before Norbyte's code makes the console.
void EnableConsoleWindow();

// The key, for the overlay's input on the main thread (Overlay/CocoaInput.cpp).
bool ConsoleWindowTakes(CocoaEvent const& event);

// Draws the window in every frame of `context` from now on. When the overlay
// makes its ImGui context.
void AddConsoleWindowHook(ImGuiContext* context);

}
