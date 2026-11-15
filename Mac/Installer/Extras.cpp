#include <Installer/Extras.h>

#include <Installer/Support.h>
#include <Platform/Config.h>
#include <Updater/Stamp.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bg3se::mac::installer {

namespace {

// What both readers read of a file: the first 64 KB, up to a NUL, in lines
// of fewer than 512 characters (Platform/Config.cpp, camera_tweaks.c).
constexpr size_t kMaxFile = 64 * 1024;
constexpr size_t kMaxLine = 512;

constexpr char kUpscalingKey[] = "gfx_temporal";
constexpr char kCameraSection[] = "General";
constexpr char kCameraKey[] = "Enabled";
constexpr int kUpscalingMacOS = 13;

struct TextLine
{
    size_t Start, End;   // without its '\n'
    bool Newline;
};

std::vector<TextLine> Lines(std::string_view text)
{
    std::vector<TextLine> lines;
    size_t at = 0;
    while (at < text.size()) {
        size_t nl = text.find('\n', at);
        if (nl == std::string_view::npos) {
            lines.push_back({ at, text.size(), false });
            break;
        }
        lines.push_back({ at, nl, true });
        at = nl + 1;
    }
    return lines;
}

// The line ending a line added to the text takes: the first one's.
std::string LineEnding(std::string_view text)
{
    size_t nl = text.find('\n');
    return nl != std::string_view::npos && nl > 0 && text[nl - 1] == '\r' ? "\r\n" : "\n";
}

std::string Number(size_t n)
{
    return std::to_string(n);
}

// What both readers stop at, or skip, before the lines are read.
bool WholeText(std::string_view text, char const* reader, SettingKey& found)
{
    if (text.size() > kMaxFile) {
        found.Problem = "it is larger than 64 KB, and " + std::string(reader) + " reads only the first 64 KB";
        return false;
    }
    if (size_t nul = text.find('\0'); nul != std::string_view::npos) {
        found.Problem = "it has a NUL byte (at byte " + Number(nul) + "), where " + reader + " stops reading";
        return false;
    }
    auto lines = Lines(text);
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].End - lines[i].Start >= kMaxLine) {
            found.Problem = "line " + Number(i + 1) + " is 512 characters or more, and " + reader + " skips it";
            return false;
        }
    }
    return true;
}

// ---- bg3mse.conf, as Platform/Config.cpp's ConfLookup reads it ----

std::string_view ConfTrimStart(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    return s;
}

std::string_view ConfTrimEnd(std::string_view s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool EqualNoCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && strncasecmp(a.data(), b.data(), a.size()) == 0;
}

// ---- NativeCameraTweaks.toml, as camera_settings_parse reads it ----

bool TomlSpace(char c)
{
    return isspace((unsigned char)c) != 0;
}

std::string_view TomlTrim(std::string_view s)
{
    while (!s.empty() && TomlSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && TomlSpace(s.back())) s.remove_suffix(1);
    return s;
}

// Where the line's comment starts: the first # outside "quotes" (strip_comment).
size_t TomlCommentAt(std::string_view line)
{
    bool quoted = false;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '"') quoted = !quoted;
        else if (line[i] == '#' && !quoted) return i;
    }
    return line.size();
}

// A [section] line the module accepts: the name between the brackets, else none.
std::optional<std::string_view> TomlSection(std::string_view t)
{
    size_t close = t.find(']');
    if (close == std::string_view::npos || close + 1 != t.size() || close - 1 >= 64) return std::nullopt;
    return TomlTrim(t.substr(1, close - 1));
}

// Each extra's name in what is said.
char const* NameOf(Extra which)
{
    return which == Extra::Camera ? "Native Camera Tweaks" : "MetalFX Temporal";
}

// "Native Camera Tweaks is", "MetalFX Temporal is": what is said of each.
std::string Subject(Extra which)
{
    return std::string(NameOf(which)) + " is";
}

std::string ValueText(Extra which, bool on)
{
    if (which == Extra::Camera) return on ? "true" : "false";
    return on ? "1" : "0";
}

std::string KeyText(Extra which, std::string const& value)
{
    if (which == Extra::Camera) return std::string(kCameraKey) + " = " + value + " under [" + kCameraSection + "]";
    return std::string(kUpscalingKey) + " = " + value;
}

std::string MacOSWhy(std::string const& version)
{
    if (version.empty()) return "requires macOS 13 or later (can't read this Mac's version)";
    int major = atoi(version.c_str());
    if (major >= kUpscalingMacOS) return {};
    return "requires macOS 13 or later (this Mac has " + version + ")";
}

// Why MetalFX Temporal is not in the libbg3mse.dylib of `library`; empty
// when it is. It is part of the graphics module, which a library built with
// it says it has (Release/GfxMark.h); what cannot be told is taken as not.
std::string GfxWhy(std::string const& library)
{
    std::string path = library + "/" + kLibrary, why;
    if (!Exists(path)) return "it is part of bg3mse's graphics module, and there is no " + std::string(kLibrary) + " in " + library;
    bool marked = false;
    if (!updater::ReadGfxMark(path, marked, why)) {
        return "it is part of bg3mse's graphics module, and whether " + path + " has that module cannot be told: " + why;
    }
    if (!marked) return "it is part of bg3mse's graphics module, and " + path + " does not say it has that module";
    return {};
}

}

SettingKey FindConfKey(std::string_view text, std::string_view key)
{
    SettingKey found;
    if (!WholeText(text, "bg3mse", found)) return found;
    auto lines = Lines(text);
    for (size_t i = 0; i < lines.size(); i++) {
        std::string_view raw = text.substr(lines[i].Start, lines[i].End - lines[i].Start);
        std::string_view line = ConfTrimStart(raw);
        if (ConfTrimEnd(line).empty() || line.front() == '#' || line.front() == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            found.Problem = "line " + Number(i + 1) + " is not a comment or a \"key = value\" line, and bg3mse skips it";
            return found;
        }
        std::string_view name = ConfTrimEnd(line.substr(0, eq));
        if (name.empty()) {
            found.Problem = "line " + Number(i + 1) + " has no key before its =, and bg3mse skips it";
            return found;
        }
        if (!EqualNoCase(name, key)) continue;
        found.Present = true;
        found.Value = std::string(ConfTrimEnd(ConfTrimStart(line.substr(eq + 1))));
        found.Line = i + 1;
    }
    return found;
}

std::optional<std::string> SetConfKey(std::string_view text, std::string_view key, std::string_view value, std::string& problem)
{
    SettingKey found = FindConfKey(text, key);
    if (!found.Problem.empty()) {
        problem = found.Problem;
        return std::nullopt;
    }
    std::string eol = LineEnding(text);
    std::string result;
    if (found.Present) {
        auto line = Lines(text)[found.Line - 1];
        std::string_view raw = text.substr(line.Start, line.End - line.Start);
        size_t eq = raw.find('=');
        size_t start = eq + 1;
        while (start < raw.size() && (raw[start] == ' ' || raw[start] == '\t')) start++;
        bool cr = !raw.empty() && raw.back() == '\r';
        std::string replaced = std::string(raw.substr(0, start));
        // "key =" with nothing after it: the value takes the space the key has before its =.
        if (start == eq + 1 && eq > 0 && (raw[eq - 1] == ' ' || raw[eq - 1] == '\t')) replaced += ' ';
        replaced += value;
        if (cr) replaced += '\r';
        result = std::string(text.substr(0, line.Start)) + replaced + std::string(text.substr(line.End));
    } else {
        result = std::string(text);
        if (!result.empty() && result.back() != '\n') result += eol;
        result += std::string(key) + " = " + std::string(value) + eol;
    }
    // Read back as the extender reads it.
    auto check = FindConfKey(result, key);
    auto read = ConfLookup(result, key);
    if (!check.Problem.empty() || !check.Present || check.Value != value || !read || *read != value) {
        problem = "the text made does not read back with " + std::string(key) + " = " + std::string(value);
        return std::nullopt;
    }
    return result;
}

SettingKey FindTomlKey(std::string_view text, std::string_view section, std::string_view key)
{
    SettingKey found;
    if (!WholeText(text, "the camera module", found)) return found;
    auto lines = Lines(text);
    std::string current;
    for (size_t i = 0; i < lines.size(); i++) {
        std::string_view raw = text.substr(lines[i].Start, lines[i].End - lines[i].Start);
        std::string_view t = TomlTrim(raw.substr(0, TomlCommentAt(raw)));
        if (t.empty()) continue;
        if (t.front() == '[') {
            auto name = TomlSection(t);
            if (!name) {
                found.Problem = "line " + Number(i + 1) + " is not a [section] line the camera module reads";
                return found;
            }
            current = std::string(*name);
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string_view::npos) {
            found.Problem = "line " + Number(i + 1) + " is not a comment, a [section] or a \"key = value\" line, and the camera module skips it";
            return found;
        }
        std::string_view name = TomlTrim(t.substr(0, eq));
        if (name.empty()) {
            found.Problem = "line " + Number(i + 1) + " has no key before its =, and the camera module skips it";
            return found;
        }
        if (current != section || name != key) continue;
        found.Present = true;
        found.Value = std::string(TomlTrim(t.substr(eq + 1)));
        found.Line = i + 1;
    }
    return found;
}

std::optional<std::string> SetTomlKey(std::string_view text, std::string_view section, std::string_view key, std::string_view value,
    std::string& problem)
{
    SettingKey found = FindTomlKey(text, section, key);
    if (!found.Problem.empty()) {
        problem = found.Problem;
        return std::nullopt;
    }
    std::string eol = LineEnding(text);
    auto lines = Lines(text);
    std::string result;
    if (found.Present) {
        // Only the value: what is before it and a comment after it stay.
        auto line = lines[found.Line - 1];
        std::string_view raw = text.substr(line.Start, line.End - line.Start);
        size_t cut = TomlCommentAt(raw);
        size_t start = raw.find('=') + 1;
        while (start < cut && TomlSpace(raw[start])) start++;
        size_t end = cut;
        while (end > start && TomlSpace(raw[end - 1])) end--;
        std::string replaced = std::string(raw.substr(0, start)) + std::string(value) + std::string(raw.substr(end));
        result = std::string(text.substr(0, line.Start)) + replaced + std::string(text.substr(line.End));
    } else {
        // After the section's first header and the comments right under it.
        std::optional<size_t> at;
        for (size_t i = 0; i < lines.size() && !at; i++) {
            std::string_view raw = text.substr(lines[i].Start, lines[i].End - lines[i].Start);
            std::string_view t = TomlTrim(raw.substr(0, TomlCommentAt(raw)));
            if (t.empty() || t.front() != '[' || TomlSection(t) != section) continue;
            size_t next = i + 1;
            while (next < lines.size()) {
                std::string_view t2 = TomlTrim(text.substr(lines[next].Start, lines[next].End - lines[next].Start));
                if (t2.empty() || t2.front() != '#') break;
                next++;
            }
            at = next;
        }
        std::string added = std::string(key) + " = " + std::string(value) + eol;
        if (at && *at < lines.size()) {
            result = std::string(text.substr(0, lines[*at].Start)) + added + std::string(text.substr(lines[*at].Start));
        } else {
            result = std::string(text);
            if (!result.empty() && result.back() != '\n') result += eol;
            if (!at) {
                bool blank = result.empty() || (result.size() >= eol.size() * 2 && result.ends_with(eol + eol));
                if (!blank) result += eol;
                result += "[" + std::string(section) + "]" + eol;
            }
            result += added;
        }
    }
    auto check = FindTomlKey(result, section, key);
    if (!check.Problem.empty() || !check.Present || check.Value != value) {
        problem = "the text made does not read back with " + std::string(key) + " = " + std::string(value);
        return std::nullopt;
    }
    return result;
}

Extras::Extras(Options const& options, Host const& host, std::string data, std::string library, std::string cameraMissing,
    std::optional<std::string> confBase)
    : o_(options), h_(host), data_(std::move(data)), library_(std::move(library)), confBase_(std::move(confBase))
{
    State camera;
    camera.Which = Extra::Camera;
    camera.File = data_ + "/" + kCameraSettingsFile;
    camera.Unavailable = std::move(cameraMissing);
    State upscaling;
    upscaling.Which = Extra::Upscaling;
    upscaling.File = data_ + "/" + kConfFile;
    upscaling.Unavailable = UpscalingUnavailable(library_, h_.MacOSVersion);
    states_ = { std::move(camera), std::move(upscaling) };
    for (auto& s : states_) Read(s);
}

void Extras::Say(std::string const& text) const
{
    fprintf(h_.Out, "%s\n", text.c_str());
}

void Extras::Read(State& s)
{
    s.There = false;
    s.Text.clear();
    s.MadeFrom.clear();
    s.NoBase.clear();
    s.Key = {};
    s.On = false;
    s.Mode = 0644;
    // A link is followed: the file it names is the one changed.
    s.Target = s.File;
    struct stat st{};
    if (lstat(s.File.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
        char resolved[PATH_MAX];
        if (!realpath(s.File.c_str(), resolved)) {
            s.Key.Problem = "it is a link that cannot be followed: " + Errno();
            return;
        }
        s.Target = resolved;
    }
    Mapped file;
    std::string problem;
    if (!file.Open(s.Target, problem)) {
        s.Key.Problem = "it cannot be read: " + problem;
        return;
    }
    if (file.Present()) {
        s.There = true;
        s.Text.assign(reinterpret_cast<char const*>(file.Bytes().data()), file.Bytes().size());
        if (stat(s.Target.c_str(), &st) == 0) s.Mode = st.st_mode & 07777;
    } else if (s.Which == Extra::Upscaling) {
        if (confBase_) s.Text = *confBase_;
    } else {
        // The camera's file is made from the one the module writes when there is none.
        std::vector<std::string> dirs;
        if (!library_.empty()) dirs.push_back(library_ + "/" + kCameraDirectory);
        std::string bin = data_ + "/" + kBinDirectory + "/" + kCameraDirectory;
        if (dirs.empty() || dirs.front() != bin) dirs.push_back(bin);
        for (auto const& dir : dirs) {
            if (Exists(dir + "/" + kCameraDefaultSettings)) {
                Mapped defaults;
                if (defaults.Open(dir + "/" + kCameraDefaultSettings, problem) && defaults.Present()) {
                    s.Text.assign(reinterpret_cast<char const*>(defaults.Bytes().data()), defaults.Bytes().size());
                    s.MadeFrom = dir + "/" + kCameraDefaultSettings;
                    break;
                }
            }
        }
    }
    if (s.Which == Extra::Upscaling) {
        s.Key = FindConfKey(s.Text, kUpscalingKey);
        // On as the extender reads it, whatever else the file has.
        auto value = ConfLookup(std::string_view(s.Text).substr(0, kMaxFile), kUpscalingKey);
        s.On = s.There && value && FlagValue(*value);
    } else {
        s.Key = FindTomlKey(s.Text, kCameraSection, kCameraKey);
        if (s.Key.Problem.empty() && s.Key.Present && s.Key.Value != "true" && s.Key.Value != "false") {
            s.Key.Problem = "line " + Number(s.Key.Line) + " has " + kCameraKey + " = " + s.Key.Value +
                ", which is not true or false, and the camera module skips it";
        }
        s.On = s.There && s.Key.Problem.empty() && s.Key.Present && s.Key.Value == "true";
    }
    if (!s.There) {
        // What it is made from holds no choice, and its problems are not the file's.
        if (s.Which == Extra::Camera && s.MadeFrom.empty()) s.Key = {};
        if (!s.Key.Problem.empty()) {
            s.NoBase = s.Key.Problem;
            s.Key = {};
        }
        s.Key.Present = false;
    }
}

std::optional<bool> Extras::Flag(Extra which) const
{
    return which == Extra::Camera ? o_.Camera : o_.Upscaling;
}

void Extras::Refresh()
{
    for (auto& s : states_) Read(s);
}

std::string Extras::CannotWrite(State const& s) const
{
    if (!s.Key.Problem.empty()) return s.File + " is left as it is: " + s.Key.Problem;
    if (!s.There && s.Which == Extra::Upscaling && !confBase_) {
        return s.File + " is not there; bg3mse-install install makes it, with bg3mse's default settings";
    }
    if (!s.There && s.Which == Extra::Camera && s.MadeFrom.empty()) {
        std::string bin = data_ + "/" + kBinDirectory;
        return s.File + " is not there, and there is no " + kCameraDirectory + "/" + kCameraDefaultSettings + " in " +
            (library_.empty() || library_ == bin ? std::string() : library_ + " or ") + bin + " to make it from";
    }
    if (!s.NoBase.empty()) return s.File + " is not there, and what it is made from cannot be used: " + s.NoBase;
    return {};
}

std::optional<std::string> Extras::NewText(State const& s, std::string& problem) const
{
    if (!s.Want) return std::nullopt;
    problem = CannotWrite(s);
    if (!problem.empty()) return std::nullopt;
    std::string value = ValueText(s.Which, *s.Want);
    return s.Which == Extra::Camera ? SetTomlKey(s.Text, kCameraSection, kCameraKey, value, problem)
                                    : SetConfKey(s.Text, kUpscalingKey, value, problem);
}

std::string Extras::Describe(State const& s) const
{
    std::string value = ValueText(s.Which, *s.Want);
    if (!s.There) {
        if (s.Which == Extra::Camera) return "create " + s.File + " from " + s.MadeFrom + ", with " + KeyText(s.Which, value);
        // The install creates it with the default settings first.
        return "add " + KeyText(s.Which, value) + " to " + s.File;
    }
    if (s.Key.Present) {
        return "set " + KeyText(s.Which, value) + " in " + s.File + ", line " + Number(s.Key.Line) + " (it was " + s.Key.Value + ")";
    }
    return "add " + KeyText(s.Which, value) + " to " + s.File;
}

std::string Extras::Line(State const& s) const
{
    std::string name = NameOf(s.Which);
    std::string fsr = s.Which == Extra::Upscaling ? "; turn on FSR in game" : "";
    if (!s.Unavailable.empty()) {
        std::string text = name + ": unavailable: " + s.Unavailable;
        if (s.On) text += " (" + s.File + " has " + KeyText(s.Which, s.Key.Value) + ")";
        return text;
    }
    if (!s.Key.Problem.empty()) {
        if (s.Which == Extra::Upscaling) {
            return name + ": " + (s.On ? "on" : "off") + " as bg3mse reads " + s.File + ", which bg3mse-install leaves as it is: " +
                s.Key.Problem + (s.On ? fsr : "");
        }
        return name + ": cannot tell: " + s.File + " is not read as a whole by the camera module, and bg3mse-install leaves it as it is: " +
            s.Key.Problem;
    }
    if (s.Key.Present) {
        return name + ": " + (s.On ? "on" : "off") + " (" + KeyText(s.Which, s.Key.Value) + " in " + s.File + ", line " + Number(s.Key.Line) +
            ")" + (s.On ? fsr : "");
    }
    std::string where = s.There ? s.File + " has no " + (s.Which == Extra::Camera ? std::string(kCameraKey) + " line under [General]"
                                                                                  : std::string(kUpscalingKey) + " line")
                                : s.File + " is not there";
    return name + ": off, not chosen yet (" + where + ")";
}

std::optional<bool> Extras::Ask(State const& s, bool defaultOn)
{
    std::string question = s.Which == Extra::Camera ? "Turn on Native Camera Tweaks?" : "Turn on MetalFX Temporal?";
    for (int wrong = 0;;) {
        fprintf(h_.Out, "%s [%s] ", question.c_str(), defaultOn ? "Y/n" : "y/N");
        fflush(h_.Out);
        // One line, read a byte at a time: nothing past it is taken from the terminal.
        std::string line;
        bool ended = false;
        for (;;) {
            char c;
            ssize_t n = read(h_.Terminal, &c, 1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                ended = true;
                break;
            }
            if (c == '\n' || c == '\r') break;
            if (line.size() < 256) line += c;
        }
        if (ended) {
            // What was typed without Return is no answer.
            Say("");
            Say("(Input ended, skipping the rest.)");
            ended_ = true;
            return std::nullopt;
        }
        std::string answer;
        for (char c : TomlTrim(line)) answer += (char)tolower((unsigned char)c);
        if (answer.empty()) return defaultOn;
        if (answer == "y" || answer == "yes") return true;
        if (answer == "n" || answer == "no") return false;
        if (++wrong == 3) {
            Say("No answer, leaving it as it is.");
            return std::nullopt;
        }
        Say(std::string("Type y or n (Return means ") + (defaultOn ? "y" : "n") + ").");
    }
}

void Extras::SettleFlag(State& s, bool on)
{
    if (!s.Unavailable.empty() && on) {
        notes_.push_back(Subject(s.Which) + " not turned on: " + s.Unavailable + ".");
        notSet_.push_back(s.Which);
        return;
    }
    // Off where it cannot be used: only a choice of on is written over.
    if (!s.Unavailable.empty() && !s.On) return;
    if (auto why = CannotWrite(s); !why.empty()) {
        notes_.push_back(Subject(s.Which) + " not set: " + why + ".");
        notSet_.push_back(s.Which);
        return;
    }
    s.Want = on;
    s.From = Source::Flag;
}

std::string Extras::NotSet() const
{
    if (notSet_.empty()) return {};
    std::string which = notSet_.size() == 1 ? std::string(NameOf(notSet_[0])) + " was" : "Native Camera Tweaks and MetalFX Temporal were";
    return which + " not set, as said above";
}

void Extras::SettleForInstall()
{
    std::vector<State*> ask;
    for (auto& s : states_) {
        if (auto flag = Flag(s.Which)) {
            SettleFlag(s, *flag);
            continue;
        }
        if (!s.Unavailable.empty()) {
            notes_.push_back(Subject(s.Which) + " not offered: " + s.Unavailable + ".");
            continue;
        }
        if (auto why = CannotWrite(s); !why.empty()) {
            notes_.push_back(Subject(s.Which) + " not asked about: " + why + ".");
            continue;
        }
        if (Chosen(s)) {
            s.From = Source::Earlier;
        } else if (o_.Defaults) {
            s.Want = false;
            s.From = Source::Defaults;
        } else if (o_.DryRun) {
            s.Unanswered = "a dry run asks nothing";
        } else if (h_.Terminal < 0) {
            s.Unanswered = "there was no terminal to ask on";
        } else {
            ask.push_back(&s);
        }
    }
    // What is not offered, and why, comes before the questions.
    for (auto const& note : notes_) Say(note);
    notes_.clear();
    if (ask.empty()) return;
    Say("");
    Say("Optional features (change later with \"bg3mse-install options\"):");
    Say("");
    for (auto* s : ask) {
        if (ended_) {
            s->Unanswered = "the input ended";
            continue;
        }
        auto answer = Ask(*s, false);
        if (answer) {
            s->Want = *answer;
            s->From = Source::Answer;
        } else {
            s->Unanswered = ended_ ? "the input ended" : "no answer";
        }
    }
    Say("");
}

bool Extras::SettleForOptions()
{
    bool flags = o_.Camera || o_.Upscaling;
    if (!flags && (o_.DryRun || h_.Terminal < 0)) return false;
    bool asked = false;
    for (auto& s : states_) {
        auto flag = Flag(s.Which);
        if (flags) {
            if (flag) SettleFlag(s, *flag);
            continue;
        }
        if (!s.Unavailable.empty()) {
            Say(Subject(s.Which) + " not offered: " + s.Unavailable + ".");
            continue;
        }
        if (auto why = CannotWrite(s); !why.empty()) {
            Say(Subject(s.Which) + " not asked about: " + why + ".");
            continue;
        }
        if (ended_) continue;
        if (!asked) {
            Say("Optional features (Return keeps the current setting):");
            Say("");
        }
        asked = true;
        auto answer = Ask(s, s.On);
        if (answer) {
            s.Want = *answer;
            s.From = Source::Answer;
        }
    }
    if (asked) Say("");
    return true;
}

std::vector<std::string> Extras::Changes() const
{
    std::vector<std::string> lines;
    for (auto const& s : states_) {
        if (!s.Want) continue;
        std::string problem;
        auto text = NewText(s, problem);
        if (!text) {
            lines.push_back("leave " + s.File + " as it is: " + problem);
            continue;
        }
        if (s.There && *text == s.Text) continue;
        lines.push_back(Describe(s));
    }
    return lines;
}

bool Extras::Apply()
{
    bool ok = true;
    for (auto& s : states_) {
        if (!s.Want) continue;
        auto want = s.Want;
        Read(s);    // as it is now: the install may have made it since
        s.Want = want;
        std::string problem;
        auto text = NewText(s, problem);
        if (!text) {
            Say("bg3mse-install: " + problem);
            NotWritten(s.Which);
            ok = false;
            continue;
        }
        if (s.There && *text == s.Text) continue;
        std::span<uint8_t const> bytes(reinterpret_cast<uint8_t const*>(text->data()), text->size());
        if (!WriteAtomically(s.Target, bytes, s.Mode, problem)) {
            Say("bg3mse-install: " + s.File + " is as it was: " + problem);
            NotWritten(s.Which);
            ok = false;
        }
    }
    return ok;
}

void Extras::NotWritten(Extra which)
{
    if (std::find(notSet_.begin(), notSet_.end(), which) == notSet_.end()) notSet_.push_back(which);
}

void Extras::Unapplied()
{
    for (auto const& s : states_) {
        if (s.Want) NotWritten(s.Which);
    }
}

std::vector<std::string> Extras::StatusLines() const
{
    std::vector<std::string> lines;
    for (auto const& s : states_) lines.push_back(Line(s));
    return lines;
}

std::vector<std::string> Extras::Summary() const
{
    std::vector<std::string> lines = { "Optional features:" };
    for (auto const& s : states_) lines.push_back("  " + Line(s));
    std::string tool = "\"" + data_ + "/" + kBinDirectory + "/" + kInstallTool + "\" options";
    // What was not chosen, by why it was not.
    std::vector<std::pair<std::string, std::vector<Extra>>> open;
    for (auto const& s : states_) {
        if (!s.Unavailable.empty() || !s.Key.Problem.empty() || s.Key.Present || s.Unanswered.empty()) continue;
        auto at = std::find_if(open.begin(), open.end(), [&](auto const& o) { return o.first == s.Unanswered; });
        if (at == open.end()) open.push_back({ s.Unanswered, { s.Which } });
        else at->second.push_back(s.Which);
    }
    for (auto const& [why, which] : open) {
        std::string subject = which.size() == 1 ? Subject(which[0]) : "Native Camera Tweaks and MetalFX Temporal are";
        lines.push_back(subject + " off for now (" + why + "). Turn " + (which.size() == 1 ? "it" : "them") + " on with " + tool + ".");
    }
    if (open.empty()) lines.push_back("Change them with " + tool + ".");
    return lines;
}

ExtrasPlace LaunchOptionPlace(std::string const& data)
{
    std::string bin = data + "/" + kBinDirectory;
    return { bin, "\"" + bin + "/" + kInstallTool + "\" options" };
}

std::string CameraMissingIn(std::string const& library)
{
    return Exists(library + "/" + kCameraLibrary) ? std::string() : std::string(kCameraLibrary) + " is not installed in " + library;
}

bool Extras::IsOn(Extra which) const
{
    for (auto const& s : states_) {
        if (s.Which == which) return s.On;
    }
    return false;
}

std::string Extras::Unavailable(Extra which) const
{
    for (auto const& s : states_) {
        if (s.Which == which) return s.Unavailable;
    }
    return {};
}

std::string UpscalingUnavailable(std::string const& library, std::string const& macos)
{
    std::string why = MacOSWhy(macos);
    return why.empty() ? GfxWhy(library) : why;
}

int OptionsVerb(Options const& options, Host const& host, ExtrasPlace const& place)
{
    auto say = [&](std::string const& text) { fprintf(host.Out, "%s\n", text.c_str()); };
    if (host.DataDirectory.empty()) {
        say("bg3mse-install: there is no home directory");
        return 1;
    }
    std::string data = ResolvedPath(host.DataDirectory);
    // bg3mse.conf is not made here: the install makes it with the default
    // settings, and does not when it is there.
    Extras extras(options, host, data, place.Library, CameraMissingIn(place.Library), std::nullopt);
    bool settled = extras.SettleForOptions();
    for (auto const& note : extras.Notes()) say(note);
    if (!settled) {
        say("Optional features:");
        for (auto const& line : extras.StatusLines()) say("  " + line);
        say(std::string(options.DryRun ? "A dry run asks nothing." : "There is no terminal to ask on.") + " To change them: " + place.Options +
            " --camera=on|off --upscaling=on|off");
        return 0;
    }
    auto changes = extras.Changes();
    if (changes.empty()) {
        say("Nothing to change.");
        for (auto const& line : extras.StatusLines()) say("  " + line);
        return extras.Refused() ? 1 : 0;
    }
    if (options.DryRun) {
        say("Dry run; nothing is changed. bg3mse-install options would:");
        for (auto const& c : changes) say("  " + c);
        return extras.Refused() ? 1 : 0;
    }
    if (!IsDirectory(data) && mkdir(data.c_str(), 0755) != 0 && errno != EEXIST) {
        say("bg3mse-install: " + data + ": " + Errno());
        say("Nothing was changed.");
        return 1;
    }
    DataLock lock(data);
    if (!lock.Held()) {
        say("bg3mse-install: another bg3mse-install is working in " + data);
        say("Nothing was changed.");
        return 1;
    }
    for (auto const& c : changes) say("  " + c);
    extras.Apply();
    extras.Refresh();
    for (auto const& line : extras.StatusLines()) say("  " + line);
    say("Takes effect next time the game starts.");
    return extras.Refused() ? 1 : 0;
}

}
