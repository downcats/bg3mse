#include <ConsoleScreen.h>

#include <algorithm>
#include <charconv>

static_assert(sizeof(wchar_t) == sizeof(char32_t), "a console character is a UTF-32 code point here");

namespace bg3se::shim {

namespace {

// The attributes of text nobody has coloured: grey on black.
constexpr uint16_t kDefaultAttributes = 0x7;
// What can wait to be read, in characters: far more than anyone types.
constexpr size_t kMostTyped = 64 * 1024;
// An escape sequence's parameters longer than this are no sequence anybody sends.
constexpr size_t kMostParameters = 64;

// The Windows console's default palette ("Campbell"), by attribute: blue,
// green, red and intensity bits.
constexpr uint32_t kPalette[16] = {
    0x0C0C0C, 0x0037DA, 0x13A10E, 0x3A96DD, 0xC50F1F, 0x881798, 0xC19C00, 0xCCCCCC,
    0x767676, 0x3B78FF, 0x16C60C, 0x61D6D6, 0xE74856, 0xB4009E, 0xF9F1A5, 0xF2F2F2,
};

// An SGR colour number (black, red, green, yellow, blue, magenta, cyan, white)
// as an index of the palette.
constexpr int kAnsiToPalette[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

// The colours of xterm's 256-colour table.
uint32_t ExtendedColour(int n)
{
    if (n < 8) return kPalette[kAnsiToPalette[n]];
    if (n < 16) return kPalette[kAnsiToPalette[n - 8] + 8];
    if (n < 232) {
        int index = n - 16;
        auto level = [](int v) { return (uint32_t)(v ? 55 + v * 40 : 0); };
        return level(index / 36) << 16 | level(index / 6 % 6) << 8 | level(index % 6);
    }
    uint32_t grey = (uint32_t)(8 + 10 * (n - 232));
    return grey << 16 | grey << 8 | grey;
}

void AppendUtf8(std::string& out, char32_t c)
{
    if (c < 0x80) {
        out += (char)c;
    } else if (c < 0x800) {
        out += (char)(0xC0 | c >> 6);
        out += (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += (char)(0xE0 | c >> 12);
        out += (char)(0x80 | (c >> 6 & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    } else {
        out += (char)(0xF0 | c >> 18);
        out += (char)(0x80 | (c >> 12 & 0x3F));
        out += (char)(0x80 | (c >> 6 & 0x3F));
        out += (char)(0x80 | (c & 0x3F));
    }
}

// The code points of UTF-8 text; a byte that starts no valid sequence is U+FFFD.
std::u32string DecodeUtf8(std::string_view text)
{
    std::u32string out;
    for (size_t i = 0; i < text.size();) {
        auto c = (unsigned char)text[i];
        size_t length = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        char32_t value = length == 1 ? c : length == 2 ? (c & 0x1F) : length == 3 ? (c & 0x0F) : (c & 0x07);
        bool valid = length > 0 && i + length <= text.size();
        for (size_t k = 1; valid && k < length; k++) {
            auto next = (unsigned char)text[i + k];
            if ((next & 0xC0) != 0x80) valid = false;
            value = value << 6 | (next & 0x3F);
        }
        // Overlong forms, surrogates and what is past Unicode are not characters.
        static constexpr char32_t kLeast[5] = { 0, 0, 0x80, 0x800, 0x10000 };
        if (valid && (value < kLeast[length] || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))) valid = false;
        if (!valid) {
            out += U'\xFFFD';
            i++;
            continue;
        }
        out += value;
        i += length;
    }
    return out;
}

size_t BytesOf(ConsoleLine const& line)
{
    size_t bytes = 0;
    for (auto const& run : line) bytes += run.Text.size();
    return bytes;
}

void AppendRun(ConsoleLine& line, uint32_t colour, uint32_t background, std::string_view text)
{
    if (text.empty()) return;
    if (!line.empty() && line.back().Colour == colour && line.back().Background == background) {
        line.back().Text += text;
    } else {
        line.push_back(ConsoleRun{ colour, std::string(text), background });
    }
}

// The background of a palette index: black is the console's own.
uint32_t BackgroundColour(int index)
{
    return index == 0 ? kNoBackground : kPalette[index];
}

// COMMON_LVB_REVERSE_VIDEO: the attributes' colour and background swapped.
constexpr uint16_t kReverseVideo = 0x4000;

}

uint32_t ConsoleColourOf(uint16_t attributes)
{
    return kPalette[attributes & 0xF];
}

uint32_t ConsoleBackgroundOf(uint16_t attributes)
{
    return BackgroundColour(attributes >> 4 & 0xF);
}

ConsoleScreen::Writer& ConsoleScreen::WriterOf(std::thread::id writer)
{
    auto it = writers_.find(writer);
    if (it != writers_.end()) return it->second;
    Writer fresh;
    fresh.Attributes = kDefaultAttributes;
    return writers_.emplace(writer, std::move(fresh)).first->second;
}

// A writer whose state is the one every writer starts with needs no entry:
// threads come and go, and their entries would stay.
void ConsoleScreen::Forget(std::thread::id writer)
{
    auto it = writers_.find(writer);
    if (it == writers_.end()) return;
    auto const& w = it->second;
    if (w.Attributes == kDefaultAttributes && !w.HasColour && !w.Bold && w.Basic < 0 && !w.HasBackground && w.BasicBackground < 0
        && !w.Reverse && w.State == Escape::None && w.Line.empty()) {
        writers_.erase(it);
    }
}

// The colour and the background before SGR 7 or the attribute
// COMMON_LVB_REVERSE_VIDEO swaps them.
uint32_t ConsoleScreen::ForegroundOf(Writer const& writer)
{
    if (writer.HasColour) return writer.Colour;
    if (writer.Basic >= 0) return kPalette[kAnsiToPalette[writer.Basic] + (writer.Bold ? 8 : 0)];
    // Bold brightens the attributes' colour as it does a basic one.
    return kPalette[(writer.Attributes & 0xF) | (writer.Bold ? 0x8 : 0)];
}

uint32_t ConsoleScreen::UnderOf(Writer const& writer)
{
    if (writer.HasBackground) return writer.Background;
    if (writer.BasicBackground >= 0) return BackgroundColour(kAnsiToPalette[writer.BasicBackground]);
    return BackgroundColour(writer.Attributes >> 4 & 0xF);
}

bool ConsoleScreen::Reversed(Writer const& writer)
{
    return writer.Reverse != ((writer.Attributes & kReverseVideo) != 0);
}

uint32_t ConsoleScreen::ColourOf(Writer const& writer) const
{
    if (!Reversed(writer)) return ForegroundOf(writer);
    // Swapped, text on the console's own background is drawn in its black.
    auto under = UnderOf(writer);
    return under == kNoBackground ? kPalette[0] : under;
}

uint32_t ConsoleScreen::BackgroundOf(Writer const& writer) const
{
    return Reversed(writer) ? ForegroundOf(writer) : UnderOf(writer);
}

void ConsoleScreen::Commit(ConsoleLine line)
{
    bytes_ += BytesOf(line);
    lines_.push_back(std::move(line));
    while (lines_.size() > 1 && (lines_.size() > kMostLines || bytes_ > kMostBytes)) {
        bytes_ -= BytesOf(lines_.front());
        lines_.pop_front();
        first_++;
    }
}

void ConsoleScreen::Commit(Writer& writer)
{
    Commit(std::move(writer.Line));
    writer.Line.clear();
    writer.LineBytes = 0;
}

void ConsoleScreen::Append(Writer& writer, char const* text, size_t length)
{
    auto colour = ColourOf(writer);
    auto background = BackgroundOf(writer);
    size_t start = 0;
    for (size_t i = 0; i < length; i++) {
        // A line goes on in the next one before the character that would not fit.
        auto c = (unsigned char)text[i];
        if ((c & 0xC0) == 0x80) continue;
        size_t bytes = c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (writer.LineBytes + (i - start) + bytes > kMostLineBytes) {
            AppendRun(writer.Line, colour, background, std::string_view(text + start, i - start));
            Commit(writer);
            start = i;
        }
    }
    writer.LineBytes += length - start;
    AppendRun(writer.Line, colour, background, std::string_view(text + start, length - start));
}

// SGR: the colours, backgrounds, boldness and reversal of what follows, as
// the Windows console takes them (Console Virtual Terminal Sequences, "Text
// Formatting").
void ConsoleScreen::Select(Writer& writer)
{
    std::vector<int> values;
    std::string_view parameters = writer.Parameters;
    while (true) {
        auto end = parameters.find_first_of(";:");
        auto part = parameters.substr(0, end);
        int value = 0;
        std::from_chars(part.data(), part.data() + part.size(), value);
        values.push_back(value);
        if (end == std::string_view::npos) break;
        parameters.remove_prefix(end + 1);
    }

    for (size_t i = 0; i < values.size(); i++) {
        int v = values[i];
        if (v == 0) {
            writer.Attributes = kDefaultAttributes;
            writer.HasColour = false;
            writer.Bold = false;
            writer.Basic = -1;
            writer.HasBackground = false;
            writer.BasicBackground = -1;
            writer.Reverse = false;
        } else if (v == 7) {
            writer.Reverse = true;
        } else if (v == 27) {
            writer.Reverse = false;
        } else if (v >= 40 && v <= 47) {
            writer.HasBackground = false;
            writer.BasicBackground = v - 40;
        } else if (v == 49) {
            writer.HasBackground = false;
            writer.BasicBackground = -1;
            writer.Attributes &= (uint16_t)~0xF0;
        } else if (v >= 100 && v <= 107) {
            writer.HasBackground = true;
            writer.Background = kPalette[kAnsiToPalette[v - 100] + 8];
            writer.BasicBackground = -1;
        } else if (v == 1) {
            writer.Bold = true;
        } else if (v == 22) {
            writer.Bold = false;
        } else if (v >= 30 && v <= 37) {
            writer.HasColour = false;
            writer.Basic = v - 30;
        } else if (v == 39) {
            writer.HasColour = false;
            writer.Basic = -1;
        } else if (v >= 90 && v <= 97) {
            writer.HasColour = true;
            writer.Colour = kPalette[kAnsiToPalette[v - 90] + 8];
            writer.Basic = -1;
        } else if (v == 38 || v == 48) {
            // 5;n or 2;r;g;b
            uint32_t colour = 0;
            bool valid = false;
            if (i + 2 < values.size() && values[i + 1] == 5) {
                colour = ExtendedColour(std::clamp(values[i + 2], 0, 255));
                valid = true;
                i += 2;
            } else if (i + 4 < values.size() && values[i + 1] == 2) {
                auto c = [&](size_t k) { return (uint32_t)std::clamp(values[i + k], 0, 255); };
                colour = c(2) << 16 | c(3) << 8 | c(4);
                valid = true;
                i += 4;
            } else {
                // What follows is not a colour this knows: the rest of the sequence with it.
                i = values.size();
            }
            if (valid && v == 38) {
                writer.HasColour = true;
                writer.Colour = colour;
                writer.Basic = -1;
            } else if (valid) {
                writer.HasBackground = true;
                writer.Background = colour;
                writer.BasicBackground = -1;
            }
        }
    }
}

// The end of a control sequence.
void ConsoleScreen::EndOf(Writer& writer, char final)
{
    if (final == 'm') {
        Select(writer);
    } else if (final == 'J' && (writer.Parameters == "2" || writer.Parameters == "3")) {
        // Erasing the display, or its scroll-back: either empties the screen.
        ClearLines();
    }
    // Moving the cursor and every other sequence have nothing to act on here.
}

void ConsoleScreen::Write(std::thread::id id, std::string_view text)
{
    std::lock_guard lock(mutex_);
    auto& writer = WriterOf(id);
    size_t plain = 0, plainLength = 0;
    auto flush = [&] {
        if (plainLength) Append(writer, text.data() + plain, plainLength);
        plainLength = 0;
    };
    for (size_t i = 0; i < text.size(); i++) {
        auto c = (unsigned char)text[i];
        switch (writer.State) {
        case Escape::None:
            if (c >= 0x20 && c != 0x7F) {
                if (!plainLength) plain = i;
                plainLength++;
                continue;
            }
            flush();
            if (c == 0x1B) {
                writer.State = Escape::Start;
            } else if (c == '\n') {
                Commit(writer);
            } else if (c == '\t') {
                // To the next of the console's tab stops, every eight columns.
                std::string spaces(8 - writer.LineBytes % 8, ' ');
                Append(writer, spaces.data(), spaces.size());
            }
            // A carriage return, a backspace and every other control
            // character move a cursor this does not have.
            break;

        case Escape::Start:
            if (c == '[') {
                writer.State = Escape::Csi;
                writer.Parameters.clear();
            } else if (c == ']') {
                writer.State = Escape::Osc;
            } else {
                writer.State = Escape::None;
            }
            break;

        case Escape::Csi:
            if (c >= 0x20 && c <= 0x3F) {
                if (writer.Parameters.size() >= kMostParameters) {
                    writer.State = Escape::None;
                } else {
                    writer.Parameters += (char)c;
                }
            } else if (c >= 0x40 && c <= 0x7E) {
                writer.State = Escape::None;
                EndOf(writer, (char)c);
            } else {
                writer.State = Escape::None;
            }
            break;

        case Escape::Osc:
            // An operating system command (a title, a link) ends with BEL or ESC \.
            if (c == 0x07) writer.State = Escape::None;
            else if (c == 0x1B) writer.State = Escape::OscEnd;
            break;

        case Escape::OscEnd:
            writer.State = Escape::None;
            break;
        }
    }
    flush();
    Forget(id);
}

void ConsoleScreen::SetAttributes(std::thread::id id, uint16_t attributes)
{
    std::lock_guard lock(mutex_);
    auto& writer = WriterOf(id);
    writer.Attributes = attributes;
    writer.HasColour = false;
    writer.Bold = false;
    writer.Basic = -1;
    writer.HasBackground = false;
    writer.BasicBackground = -1;
    writer.Reverse = false;
    Forget(id);
}

void ConsoleScreen::Clear()
{
    std::lock_guard lock(mutex_);
    ClearLines();
}

// Line numbers go on from where they were: a window that asks for one it saw
// before gets nothing rather than another line.
void ConsoleScreen::ClearLines()
{
    first_ += lines_.size();
    lines_.clear();
    bytes_ = 0;
    cleared_++;
}

void ConsoleScreen::SetTitle(std::string title)
{
    std::lock_guard lock(mutex_);
    title_ = std::move(title);
}

bool ConsoleScreen::Read(wchar_t* out, size_t count, size_t& taken)
{
    taken = 0;
    std::unique_lock lock(mutex_);
    // What the reader wrote and did not end is its prompt.
    auto it = writers_.find(std::this_thread::get_id());
    if (it != writers_.end() && !it->second.Line.empty()) {
        for (auto& run : it->second.Line) AppendRun(prompt_, run.Colour, run.Background, run.Text);
        it->second.Line.clear();
        it->second.LineBytes = 0;
        Forget(it->first);
    }

    reading_ = true;
    typed_.wait(lock, [&] { return !input_.empty() || !open_; });
    reading_ = false;
    if (!open_) return false;

    while (taken < count && !input_.empty()) {
        wchar_t c = input_.front();
        input_.pop_front();
        out[taken++] = c;
        if (c == L'\r') {
            // The line read, after its prompt, as the console echoed it.
            ConsoleLine line = std::move(prompt_);
            std::string text;
            for (char32_t e : echo_) {
                // A tab typed is shown as the console shows one: to its next tab stop.
                if (e == U'\t') text.append(8 - (BytesOf(line) + text.size()) % 8, ' ');
                else AppendUtf8(text, e);
            }
            AppendRun(line, kPalette[kDefaultAttributes], kNoBackground, text);
            Commit(std::move(line));
            prompt_.clear();
            echo_.clear();
        } else if (c != L'\n') {
            echo_ += (char32_t)c;
        }
    }
    return true;
}

void ConsoleScreen::Type(std::string_view line)
{
    {
        std::lock_guard lock(mutex_);
        if (!open_) return;
        auto characters = DecodeUtf8(line);
        // A line end typed inside the line would end it: control characters
        // are not typed, except a tab, which the Windows console's line keeps.
        std::erase_if(characters, [](char32_t c) { return (c < 0x20 && c != U'\t') || c == 0x7F; });
        if (input_.size() + characters.size() + 2 > kMostTyped) return;
        for (char32_t c : characters) input_.push_back((wchar_t)c);
        input_.push_back(L'\r');
        input_.push_back(L'\n');
    }
    typed_.notify_all();
}

ConsoleScreen::View ConsoleScreen::Look()
{
    std::lock_guard lock(mutex_);
    View view;
    view.First = first_;
    view.End = first_ + lines_.size();
    view.Cleared = cleared_;
    view.Prompt = prompt_;
    view.Reading = reading_;
    view.Title = title_;
    return view;
}

uint64_t ConsoleScreen::Copy(uint64_t from, uint64_t to, std::vector<ConsoleLine>& out)
{
    std::lock_guard lock(mutex_);
    uint64_t end = first_ + lines_.size();
    from = std::max(from, first_);
    to = std::min(to, end);
    for (uint64_t i = from; i < to; i++) out.push_back(lines_[(size_t)(i - first_)]);
    return from;
}

void ConsoleScreen::SetSize(uint16_t columns, uint16_t rows)
{
    std::lock_guard lock(mutex_);
    columns_ = std::max<uint16_t>(columns, 1);
    rows_ = std::max<uint16_t>(rows, 1);
}

void ConsoleScreen::Size(uint16_t& columns, uint16_t& rows)
{
    std::lock_guard lock(mutex_);
    columns = columns_;
    rows = rows_;
}

void ConsoleScreen::Open()
{
    std::lock_guard lock(mutex_);
    open_ = true;
    input_.clear();
    prompt_.clear();
    echo_.clear();
}

void ConsoleScreen::Close()
{
    {
        std::lock_guard lock(mutex_);
        open_ = false;
    }
    typed_.notify_all();
}

bool ConsoleScreen::IsOpen()
{
    std::lock_guard lock(mutex_);
    return open_;
}

size_t ConsoleScreen::Bytes()
{
    std::lock_guard lock(mutex_);
    return bytes_;
}

size_t ConsoleScreen::Writers()
{
    std::lock_guard lock(mutex_);
    return writers_.size();
}

ConsoleScreen& WindowConsoleScreen()
{
    // Never destroyed: threads may still write while the process exits.
    static auto* screen = new ConsoleScreen();
    return *screen;
}

}
