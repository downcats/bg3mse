#include <Overlay/ConsoleWindow.h>

#include <Overlay/Dependencies.h>
#include <Overlay/KeyCodes.h>
#include <Overlay/OverlayState.h>
#include <Platform/Config.h>
#include <Platform/Log.h>

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstring>

// Nothing here logs through the extender's console, whose lines are written
// to the screen this window shows: only to bg3mse's own log (Platform/Log.h).

namespace bg3se::mac::overlay {

namespace {

// The Windows console's default: conhost keeps 50 commands (HistoryBufferSize).
constexpr size_t kMostHistory = 50;
// The size of a console AllocConsole makes, in characters.
constexpr int kColumns = 120, kRows = 30;

std::string_view Trim(std::string_view text)
{
    while (!text.empty() && isspace((unsigned char)text.front())) text.remove_prefix(1);
    while (!text.empty() && isspace((unsigned char)text.back())) text.remove_suffix(1);
    return text;
}

bool SameName(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return tolower((unsigned char)x) == tolower((unsigned char)y);
    });
}

ImU32 ColourOf(uint32_t rgb)
{
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF);
}

std::string TextOf(shim::ConsoleLine const& line)
{
    std::string text;
    for (auto const& run : line) text += run.Text;
    return text;
}

// The next character of UTF-8 text at `at`, and where the one after it starts.
unsigned NextCharacter(std::string_view text, size_t& at)
{
    unsigned c = 0;
    int length = ImTextCharFromUtf8(&c, text.data() + at, text.data() + text.size());
    at += (size_t)std::max(length, 1);
    return c;
}

// How far a character moves the pen in the current font, as ImGui draws it.
float Advance(unsigned c)
{
    // The font as it is baked for the size in use, scaled as ImGui's own
    // measuring scales it (ImFont::CalcTextSizeA).
    auto* baked = ImGui::GetFontBaked();
    return baked->GetCharAdvance((ImWchar)c) * (ImGui::GetFontSize() / baked->Size);
}

float Width(std::string_view text)
{
    float width = 0.0f;
    for (size_t at = 0; at < text.size();) width += Advance(NextCharacter(text, at));
    return width;
}

bool Blank(unsigned c)
{
    return c == ' ' || c == '\t';
}

// The prompt, in its colours on its backgrounds, as a line of text.
void DrawRuns(shim::ConsoleLine const& line)
{
    auto* draw = ImGui::GetWindowDrawList();
    auto at = ImGui::GetCursorScreenPos();
    float x = at.x, height = ImGui::GetTextLineHeight();
    for (auto const& run : line) {
        float width = Width(run.Text);
        if (run.Background != shim::kNoBackground) draw->AddRectFilled(ImVec2(x, at.y), ImVec2(x + width, at.y + height), ColourOf(run.Background));
        draw->AddText(ImVec2(x, at.y), ColourOf(run.Colour), run.Text.data(), run.Text.data() + run.Text.size());
        x += width;
    }
    ImGui::Dummy(ImVec2(x - at.x, height));
}

}

std::optional<KeyChord> ParseKeyChord(std::string_view text, std::string& problem)
{
    KeyChord chord;
    std::vector<std::string_view> parts;
    while (true) {
        auto plus = text.find('+');
        // "Ctrl++" would name the key '+', which ImGui calls "Equal" (with Shift).
        parts.push_back(Trim(text.substr(0, plus)));
        if (plus == std::string_view::npos) break;
        text.remove_prefix(plus + 1);
    }
    for (size_t i = 0; i + 1 < parts.size(); i++) {
        auto part = parts[i];
        if (SameName(part, "Ctrl") || SameName(part, "Control")) {
            chord.Modifiers |= kFlagControl;
        } else if (SameName(part, "Shift")) {
            chord.Modifiers |= kFlagShift;
        } else if (SameName(part, "Alt") || SameName(part, "Option")) {
            chord.Modifiers |= kFlagOption;
        } else if (SameName(part, "Cmd") || SameName(part, "Command") || SameName(part, "Super")) {
            problem = "a key with Command held is the application's shortcut";
            return {};
        } else {
            problem = "\"" + std::string(part) + "\" is no modifier (Ctrl, Shift, Alt)";
            return {};
        }
    }
    auto name = parts.back();
    if (name.empty()) {
        problem = "no key is named";
        return {};
    }
    for (uint16_t code = 0; code <= kLastKeyCode; code++) {
        auto key = ImGuiKeyOf(code);
        if (key == ImGuiKey_None || !SameName(name, ImGui::GetKeyName(key))) continue;
        if (ModifierBitOf(code) != 0) {
            problem = "a modifier key on its own is no key for the console";
            return {};
        }
        chord.KeyCode = code;
        return chord;
    }
    problem = "\"" + std::string(name) + "\" is not the name of a key (F6, GraveAccent, Keypad0, ...)";
    return {};
}

std::string DescribeKeyChord(KeyChord chord)
{
    std::string text;
    if (chord.Modifiers & kFlagControl) text += "Ctrl+";
    if (chord.Modifiers & kFlagShift) text += "Shift+";
    if (chord.Modifiers & kFlagOption) text += "Alt+";
    auto key = ImGuiKeyOf(chord.KeyCode);
    text += key == ImGuiKey_None ? "?" : ImGui::GetKeyName(key);
    return text;
}

ConsoleWindow::ConsoleWindow(shim::ConsoleScreen& screen, KeyChord key, std::function<std::string()> overlayProblem)
    : screen_(screen), key_(key), overlayProblem_(std::move(overlayProblem))
{}

bool ConsoleWindow::TakeEvent(CocoaEvent const& event, bool presenting)
{
    if (event.KeyCode != key_.KeyCode) return false;
    if (event.Kind == CocoaEventKind::KeyUp) {
        // The key's release, whatever is held by then, if its press was taken.
        bool held = keyHeld_;
        keyHeld_ = false;
        return held;
    }
    if (event.Kind != CocoaEventKind::KeyDown || (event.ModifierFlags & (kChordModifiers | kFlagCommand)) != key_.Modifiers) {
        return false;
    }
    if (!opened_.load()) {
        // No console, or one in the terminal: the key is the game's.
        if (!closedReported_) {
            closedReported_ = true;
            Log(LogLevel::Info, "Console: %s shows the console window once the game has a console and no terminal "
                "(CreateConsole in ScriptExtenderSettings.json, or Ctrl+F12, makes one); until then the key is the game's",
                DescribeKeyChord(key_).c_str());
        }
        return false;
    }
    keyHeld_ = true;
    if (event.Repeat) return true;

    bool show = !visible_.load();
    if (show) focus_.store(true);
    visible_.store(show);
    if (show && !presenting) {
        if (!hiddenReported_) {
            hiddenReported_ = true;
            Log(LogLevel::Warning, "Console: the overlay is not drawn now, so the console window cannot be seen; it is "
                "drawn while a client Lua state exists (the IMGUI lines above say what it is waiting for). The window shows "
                "once the overlay is drawn");
        }
    } else if (presenting) {
        hiddenReported_ = false;
    }
    return true;
}

void ConsoleWindow::ConsoleOpened()
{
    opened_.store(true);
    // As a console window appears on Windows, though without the keyboard,
    // which stays the game's until the key or a click gives it to the window.
    visible_.store(true);
    auto problem = overlayProblem_ ? overlayProblem_() : std::string();
    if (!problem.empty()) {
        Log(LogLevel::Warning, "Console: the game has no terminal, so the console is a window of the overlay, and the "
            "overlay is off in this game (%s): the console cannot be seen or typed into; what it prints is in this log",
            problem.c_str());
        return;
    }
    Log(LogLevel::Info, "Console: the game has no terminal, so the console is a window of the overlay, shown whenever "
        "the overlay is drawn; %s hides it and shows it again", DescribeKeyChord(key_).c_str());
}

void ConsoleWindow::ConsoleClosed()
{
    opened_.store(false);
    visible_.store(false);
}

int ConsoleWindow::OnInput(ImGuiInputTextCallbackData* data)
{
    auto& self = *static_cast<ConsoleWindow*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        if (self.emptyLine_) {
            self.emptyLine_ = false;
            data->DeleteChars(0, data->BufTextLen);
            self.historyAt_ = -1;
        }
        if (self.paste_) {
            std::string text = std::move(*self.paste_);
            self.paste_.reset();
            // Typed as the Windows console types a paste: at the cursor, in
            // place of what is selected, each line end being Enter, which
            // enters the whole line.
            int from = data->CursorPos, to = data->CursorPos;
            if (data->HasSelection()) {
                from = std::min(data->SelectionStart, data->SelectionEnd);
                to = std::max(data->SelectionStart, data->SelectionEnd);
            }
            std::vector<std::string> lines(1);
            for (size_t i = 0; i < text.size(); i++) {
                if (text[i] == '\r' || text[i] == '\n') {
                    if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') i++;
                    lines.emplace_back();
                } else {
                    lines.back() += text[i];
                }
            }
            if (lines.size() == 1) {
                data->DeleteChars(from, to - from);
                data->InsertChars(from, lines[0].c_str());
            } else {
                std::string first(data->Buf, (size_t)from);
                first += lines[0];
                first.append(data->Buf + to, (size_t)(data->BufTextLen - to));
                self.Submit(first);
                for (size_t i = 1; i + 1 < lines.size(); i++) self.Submit(lines[i]);
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, lines.back().c_str());
            }
            data->SelectionStart = data->SelectionEnd = data->CursorPos;
        }
        return 0;
    }
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || self.history_.empty()) return 0;
    // Up goes back through what was entered, Down forward to an empty line,
    // as in the Windows console.
    int at = self.historyAt_;
    if (data->EventKey == ImGuiKey_UpArrow) {
        at = at < 0 ? (int)self.history_.size() - 1 : std::max(at - 1, 0);
    } else if (data->EventKey == ImGuiKey_DownArrow) {
        at = at < 0 ? -1 : at + 1 >= (int)self.history_.size() ? -1 : at + 1;
    }
    if (at == self.historyAt_) return 0;
    self.historyAt_ = at;
    data->DeleteChars(0, data->BufTextLen);
    if (at >= 0) data->InsertChars(0, self.history_[(size_t)at].c_str());
    return 0;
}

void ConsoleWindow::Submit(std::string const& line)
{
    screen_.Type(line);
    if (!line.empty() && (history_.empty() || history_.back() != line)) {
        history_.push_back(line);
        if (history_.size() > kMostHistory) history_.erase(history_.begin());
    }
    historyAt_ = -1;
}

void ConsoleWindow::Paste(std::string text)
{
    if (text.empty()) return;
    paste_ = paste_ ? *paste_ + text : std::move(text);
}

bool ConsoleWindow::Selection(Place& from, Place& to) const
{
    from = std::min(anchor_, caret_);
    to = std::max(anchor_, caret_);
    return from != to;
}

std::string ConsoleWindow::SelectedText()
{
    Place from, to;
    if (!Selection(from, to)) return {};
    std::vector<shim::ConsoleLine> lines;
    uint64_t first = screen_.Copy(from.Line, to.Line + 1, lines);
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        uint64_t number = first + i;
        auto line = TextOf(lines[i]);
        size_t begin = number == from.Line ? std::min<size_t>(from.Byte, line.size()) : 0;
        size_t end = number == to.Line ? std::min<size_t>(to.Byte, line.size()) : line.size();
        if (begin < end) text.append(line, begin, end - begin);
        if (number != to.Line) text += '\n';
    }
    return text;
}

void ConsoleWindow::Copy()
{
    auto text = SelectedText();
    if (!text.empty()) ImGui::SetClipboardText(text.c_str());
    anchor_ = caret_;
    selecting_ = false;
}

// The keys the window acts on before its line sees them; a key it acts on is
// not the line's in this frame.
void ConsoleWindow::Keys(bool typing)
{
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) return;
    ImGuiID const owner = ImGui::GetID("##console.keys");
    auto take = [&](std::initializer_list<ImGuiKey> keys) {
        for (auto key : keys) ImGui::SetKeyOwner(key, owner, ImGuiInputFlags_LockThisFrame);
    };
    Place from, to;
    bool selected = Selection(from, to);
    if (selected && (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Insert)
        || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
        Copy();
        // The line takes Enter whoever else claims it: it is told to drop it.
        take({ ImGuiKey_C, ImGuiKey_Insert });
        enterTaken_ = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (selected) anchor_ = caret_;
        else if (typing) emptyLine_ = true;
        take({ ImGuiKey_Escape });
    } else if (!typing && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) {
        auto view = screen_.Look();
        anchor_ = Place{ view.First, 0 };
        caret_ = Place{ view.End, 0 };
        take({ ImGuiKey_A });
    } else if (typing && (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V, ImGuiInputFlags_Repeat)
        || ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_Insert, ImGuiInputFlags_Repeat))) {
        // ImGui's own paste makes one line of several: a paste is the window's.
        if (auto clipboard = ImGui::GetClipboardText()) Paste(clipboard);
        take({ ImGuiKey_V, ImGuiKey_Insert });
    }
}

// The rows of the lines kept, wrapped at `width` as the Windows console wraps
// a line at its window's width: at any character.
void ConsoleWindow::Rows(shim::ConsoleScreen::View const& view, float width)
{
    float font = ImGui::GetFontSize();
    if (view.Cleared != rowsCleared_ || width != rowsWidth_ || font != rowsFont_ || rowsEnd_ < view.First || rowsEnd_ > view.End) {
        rows_.clear();
        rowsEnd_ = view.First;
        rowsCleared_ = view.Cleared;
        rowsWidth_ = width;
        rowsFont_ = font;
    }
    // The lines dropped from the screen are dropped here.
    auto kept = std::find_if(rows_.begin(), rows_.end(), [&](Row const& row) { return row.Line >= view.First; });
    rows_.erase(rows_.begin(), kept);
    if (rowsEnd_ >= view.End) return;

    std::vector<shim::ConsoleLine> lines;
    uint64_t number = screen_.Copy(std::max(rowsEnd_, view.First), view.End, lines);
    for (auto const& line : lines) {
        auto text = TextOf(line);
        Row row{ number, 0, 0 };
        float x = 0.0f;
        for (size_t at = 0; at < text.size();) {
            size_t start = at;
            float advance = Advance(NextCharacter(text, at));
            if (x + advance > width && start > row.Begin) {
                row.End = (uint32_t)start;
                rows_.push_back(row);
                row.Begin = (uint32_t)start;
                x = 0.0f;
            }
            x += advance;
        }
        row.End = (uint32_t)text.size();
        rows_.push_back(row);
        number++;
    }
    rowsEnd_ = number;
}

ConsoleWindow::Place ConsoleWindow::At(ImVec2 point)
{
    if (rows_.empty()) return Place{};
    float y = (point.y - origin_.y) / std::max(rowHeight_, 1.0f);
    if (y < 0.0f) return Place{ rows_.front().Line, rows_.front().Begin };
    auto index = (size_t)y;
    if (index >= rows_.size()) return Place{ rows_.back().Line, rows_.back().End };
    auto const& row = rows_[index];
    std::vector<shim::ConsoleLine> lines;
    if (screen_.Copy(row.Line, row.Line + 1, lines) != row.Line || lines.empty()) return Place{ row.Line, row.Begin };
    auto text = TextOf(lines[0]);
    // Before the middle of a character is before it.
    float x = origin_.x;
    for (size_t at = std::min<size_t>(row.Begin, text.size()); at < row.End && at < text.size();) {
        size_t start = at;
        float advance = Advance(NextCharacter(text, at));
        if (point.x < x + advance * 0.5f) return Place{ row.Line, (uint32_t)start };
        x += advance;
    }
    return Place{ row.Line, row.End };
}

void ConsoleWindow::Mouse(shim::ConsoleScreen::View const& view)
{
    auto const& io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto at = At(io.MousePos);
        if (io.KeyShift) {
            caret_ = at;
        } else {
            anchor_ = caret_ = at;
        }
        selecting_ = true;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            // A word: what lies between blanks around the place.
            std::vector<shim::ConsoleLine> lines;
            if (screen_.Copy(at.Line, at.Line + 1, lines) == at.Line && !lines.empty()) {
                auto text = TextOf(lines[0]);
                size_t begin = std::min<size_t>(at.Byte, text.size()), end = begin;
                while (begin > 0) {
                    size_t before = begin - 1;
                    while (before > 0 && ((unsigned char)text[before] & 0xC0) == 0x80) before--;
                    size_t next = before;
                    if (Blank(NextCharacter(text, next))) break;
                    begin = before;
                }
                while (end < text.size()) {
                    size_t next = end;
                    if (Blank(NextCharacter(text, next))) break;
                    end = next;
                }
                anchor_ = Place{ at.Line, (uint32_t)begin };
                caret_ = Place{ at.Line, (uint32_t)end };
            }
            selecting_ = false;
        }
    } else if (selecting_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            caret_ = At(io.MousePos);
            // Dragged past the window's edge, the window scrolls after the mouse.
            auto top = ImGui::GetWindowPos().y, bottom = top + ImGui::GetWindowHeight();
            if (io.MousePos.y < top) ImGui::SetScrollY(ImGui::GetScrollY() - rowHeight_);
            else if (io.MousePos.y > bottom) ImGui::SetScrollY(ImGui::GetScrollY() + rowHeight_);
        } else {
            selecting_ = false;
            // The line has the keyboard again, as the console's input has.
            focus_.store(true);
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        // As the Windows console's right click: a selection is copied, or else
        // the clipboard is pasted.
        Place from, to;
        if (Selection(from, to)) {
            Copy();
        } else if (auto clipboard = ImGui::GetClipboardText()) {
            Paste(clipboard);
        }
        focus_.store(true);
    }
    // A selection keeps its place while lines are dropped before it, as far as it can.
    if (anchor_.Line < view.First) anchor_ = Place{ view.First, 0 };
    if (caret_.Line < view.First) caret_ = Place{ view.First, 0 };
}

void ConsoleWindow::DrawLines(shim::ConsoleScreen::View const& view)
{
    Rows(view, ImGui::GetContentRegionAvail().x);
    rowHeight_ = ImGui::GetTextLineHeight();
    origin_ = ImGui::GetCursorScreenPos();
    // The part in sight takes the mouse, so that a drag selects rather than
    // moves the window.
    auto const* window = ImGui::GetCurrentWindow();
    ImGui::SetCursorScreenPos(window->InnerRect.Min);
    ImGui::InvisibleButton("##console.mouse", ImVec2(std::max(window->InnerRect.GetWidth(), 1.0f), std::max(window->InnerRect.GetHeight(), 1.0f)),
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    Mouse(view);
    ImGui::SetCursorScreenPos(origin_);
    Place from, to;
    bool selected = Selection(from, to);
    auto* draw = ImGui::GetWindowDrawList();
    ImU32 selection = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);

    // Only the rows in sight are copied from the screen and drawn.
    ImGuiListClipper clipper;
    clipper.Begin((int)std::min<size_t>(rows_.size(), (size_t)INT32_MAX), rowHeight_);
    while (clipper.Step()) {
        if (clipper.DisplayStart >= clipper.DisplayEnd) continue;
        visibleLines_.clear();
        uint64_t firstLine = rows_[(size_t)clipper.DisplayStart].Line;
        uint64_t copied = screen_.Copy(firstLine, rows_[(size_t)clipper.DisplayEnd - 1].Line + 1, visibleLines_);
        for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; index++) {
            auto const& row = rows_[(size_t)index];
            ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(rowsWidth_, rowHeight_));
            // A line dropped since the rows were made is drawn empty.
            if (row.Line < copied || row.Line - copied >= visibleLines_.size()) continue;
            auto const& line = visibleLines_[(size_t)(row.Line - copied)];

            float x = at.x;
            float selectFrom = -1.0f, selectTo = -1.0f;
            uint32_t offset = 0;
            auto mark = [&](uint32_t byte, float position) {
                if (!selected) return;
                if (Place{ row.Line, byte } == from) selectFrom = position;
                if (Place{ row.Line, byte } == to) selectTo = position;
            };
            for (auto const& run : line) {
                uint32_t runEnd = offset + (uint32_t)run.Text.size();
                uint32_t begin = std::max(offset, row.Begin), end = std::min(runEnd, row.End);
                if (begin < end) {
                    std::string_view piece(run.Text.data() + (begin - offset), end - begin);
                    float start = x;
                    for (size_t i = 0; i < piece.size();) {
                        mark(begin + (uint32_t)i, x);
                        x += Advance(NextCharacter(piece, i));
                    }
                    if (run.Background != shim::kNoBackground) {
                        draw->AddRectFilled(ImVec2(start, at.y), ImVec2(x, at.y + rowHeight_), ColourOf(run.Background));
                    }
                }
                offset = runEnd;
            }
            mark(row.End, x);
            if (selected) {
                Place rowStart{ row.Line, row.Begin }, rowEnd{ row.Line, row.End };
                bool startsBefore = from < rowStart, endsAfter = to > rowEnd;
                if (startsBefore && selectFrom < 0.0f) selectFrom = at.x;
                // A selection that goes on past the line's end takes in its line end too.
                if (endsAfter && selectTo < 0.0f) selectTo = x + (row.End == TextOf(line).size() ? Advance(' ') : 0.0f);
                if (from <= rowEnd && to >= rowStart && selectFrom >= 0.0f && selectTo > selectFrom) {
                    draw->AddRectFilled(ImVec2(selectFrom, at.y), ImVec2(selectTo, at.y + rowHeight_), selection);
                }
            }
            x = at.x;
            offset = 0;
            for (auto const& run : line) {
                uint32_t runEnd = offset + (uint32_t)run.Text.size();
                uint32_t begin = std::max(offset, row.Begin), end = std::min(runEnd, row.End);
                if (begin < end) {
                    char const* text = run.Text.data() + (begin - offset);
                    draw->AddText(ImVec2(x, at.y), ColourOf(run.Colour), text, text + (end - begin));
                    x += Width(std::string_view(text, end - begin));
                }
                offset = runEnd;
            }
        }
    }
    clipper.End();

    // At the end, the window follows what is printed; scrolled up, it stays.
    if (!selecting_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);

    // For GetConsoleScreenBufferInfoEx: the characters there is room for.
    auto fit = [](float space, float size) { return (uint16_t)std::clamp(size > 0.0f ? space / size : 1.0f, 1.0f, 32767.0f); };
    screen_.SetSize(fit(rowsWidth_, Advance('0')), fit(ImGui::GetWindowHeight(), rowHeight_));
}

void ConsoleWindow::Draw()
{
    if (!visible_.load()) return;
    auto view = screen_.Look();

    auto const* viewport = ImGui::GetMainViewport();
    auto const& style = ImGui::GetStyle();
    // The size of a new Windows console: 120 by 30 characters, as far as the game's view has room.
    // Under the lines: the separator, which takes its thickness of the
    // layout (from ImGui 1.93 on, a thin one too), and the line to type in.
    float footer = style.ItemSpacing.y + std::max(style.SeparatorSize, 1.0f) + ImGui::GetFrameHeightWithSpacing();
    ImVec2 size(kColumns * Advance('0') + style.ScrollbarSize + style.WindowPadding.x * 2.0f,
        kRows * ImGui::GetTextLineHeight() + ImGui::GetFrameHeight() + footer + style.WindowPadding.y * 2.0f + style.ItemSpacing.y);
    size.x = std::min(size.x, viewport->Size.x * 0.9f);
    size.y = std::min(size.y, viewport->Size.y * 0.9f);
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.05f, viewport->Pos.y + viewport->Size.y * 0.05f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    bool focus = focus_.exchange(false);
    if (focus) {
        ImGui::SetNextWindowFocus();
        ImGui::SetNextWindowCollapsed(false);
    }

    // Norbyte's console names itself (SetConsoleTitleW); the id stays the same.
    std::string title = (view.Title.empty() ? std::string("Script Extender Console") : view.Title) + "###bg3mse.console";
    bool open = true;
    if (ImGui::Begin(title.c_str(), &open)) {
        ImGuiID const input = ImGui::GetID("##console.input");
        // ImGui starts a line's editing on any activation of it, a
        // controller's button included (before 1.93 only the keyboard's, or
        // a controller's button for text input, did). The console's line is
        // the keyboard's: a controller's press does not start it, and the
        // keyboard stays the game's.
        if (auto& g = *GImGui; g.NavActivateId == input && g.ActiveId != input && g.NavInputSource == ImGuiInputSource_Gamepad
            && !(g.NavActivateFlags & ImGuiActivateFlags_FromFocusApi)) {
            g.NavActivateId = 0;
        }
        enterTaken_ = false;
        Keys(ImGui::GetActiveID() == input);
        if (ImGui::BeginChild("##console.lines", ImVec2(0.0f, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, 0.0f));
            DrawLines(view);
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();
        ImGui::Separator();

        // The prompt is what Norbyte's console printed before it began to
        // read ("S >> "); without one it waits for Enter to begin.
        if (!view.Prompt.empty()) {
            DrawRuns(view.Prompt);
            ImGui::SameLine(0.0f, 0.0f);
        }
        char const* hint = !view.Prompt.empty() ? "" : view.Reading ? "Enter starts the console's input; then help lists its commands" : "running a command";
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (focus) ImGui::SetKeyboardFocusHere();
        auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways
            | ImGuiInputTextFlags_AllowTabInput;
        if (ImGui::InputTextWithHint("##console.input", hint, line_, sizeof(line_), flags, &OnInput, this)) {
            if (!enterTaken_) {
                Submit(line_);
                line_[0] = '\0';
            }
            // The line keeps the keyboard for the next command.
            ImGui::SetKeyboardFocusHere(-1);
        }
    }
    ImGui::End();
    if (!open) visible_.store(false);
}

// --- the process's window ----------------------------------------------------

namespace {

std::atomic<ConsoleWindow*> gConsoleWindow{ nullptr };

void DrawHook(ImGuiContext*, ImGuiContextHook*)
{
    if (auto window = gConsoleWindow.load()) window->Draw();
}

}

void EnableConsoleWindow()
{
    if (gConsoleWindow.load()) return;
    std::string problem;
    auto key = ParseKeyChord(kDefaultConsoleKey, problem);
    if (auto setting = ConfValue(kConsoleKeySetting)) {
        if (auto chosen = ParseKeyChord(*setting, problem)) {
            key = chosen;
        } else {
            Log(LogLevel::Warning, "Console: %s = %s in bg3mse.conf: %s; the console window's key is %s",
                kConsoleKeySetting, setting->c_str(), problem.c_str(), kDefaultConsoleKey);
        }
    }
    // Never destroyed: the console and the overlay may use it until the process ends.
    auto window = new ConsoleWindow(shim::WindowConsoleScreen(), *key, [] {
        auto const& dependencies = GameDependencies();
        if (dependencies.Drawable()) return std::string();
        return dependencies.Problem(dependencies.Usable(Part::Frame) ? Part::Events : Part::Frame);
    });
    gConsoleWindow.store(window);
    shim::SetWindowConsole(window);
}

bool ConsoleWindowTakes(CocoaEvent const& event)
{
    auto window = gConsoleWindow.load();
    return window && window->TakeEvent(event, State().Presenting.load());
}

void AddConsoleWindowHook(ImGuiContext* context)
{
    ImGuiContextHook hook;
    // After ImGui::NewFrame, where the overlay's own windows are drawn.
    hook.Type = ImGuiContextHookType_NewFramePost;
    hook.Callback = &DrawHook;
    ImGui::AddContextHook(context, &hook);
}

}
