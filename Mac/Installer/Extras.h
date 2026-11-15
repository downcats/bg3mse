#pragma once

#include <Installer/Installer.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// bg3mse's optional extras ("Optional features" to the player), each off
// until the player turns it on:
//
//   Native Camera Tweaks  libbg3mse-camera.dylib (Mac/Camera), installed with
//                         bg3mse whenever the release has it, with its
//                         licence texts, and switched by "Enabled = true"
//                         under [General] in NativeCameraTweaks.toml.
//                         Declining leaves it installed and switched off: the
//                         updater puts the module beside every
//                         libbg3mse.dylib it downloads
//                         (Mac/Updater/Updater.cpp), and libbg3mse.dylib loads
//                         it from its own folder (Mac/Main.cpp), so leaving
//                         it out of bin/ would not keep it out; and a later
//                         yes needs no download.
//   MetalFX Temporal      gfx_temporal = 1 in bg3mse.conf (Mac/Gfx/Gfx.h), on
//                         macOS 13 or later only, and only with a
//                         libbg3mse.dylib that says it has the graphics
//                         module (Release/GfxMark.h): the one being
//                         installed, or the one installed. It takes the place
//                         of the game's TAA and FSR 1.0 only while FSR is on
//                         in the game's video settings.
//
// Both keys are read when the game starts, and only then: a change made
// while the game runs is safe, and takes effect the next time it starts.
//
// A choice is a key written in its file, on a line the file's reader reads
// (Platform/Config.h for bg3mse.conf, camera_settings_parse for the
// camera's). It stands: install does not ask about it again. A key that is
// not written, or a file that is not there, is the default (off) and not
// chosen yet; the camera module's own default file leaves Enabled out for
// that reason. An answer, Return for the default included, is written; no
// answer (no terminal, a dry run, the end of the input) writes nothing, so
// that a later install on a terminal asks.
//
// A file is changed in that one key only: every other line, comment and line
// ending stays as it is. It is written whole to a temporary file beside it,
// with its mode, and renamed into place. A file its reader does not read as a
// whole (a line it skips or rejects, a NUL, more than 64 KB) is not changed.

namespace bg3se::mac::installer {

// A key in the text of a settings file, as the file's reader finds it.
struct SettingKey
{
    std::string Problem;     // not empty: the reader does not read the text as a whole, and it is not changed
    bool Present{ false };   // the key is written; the last time, when it is more than once
    std::string Value;       // as written
    size_t Line{ 0 };        // from 1
};

// bg3mse.conf (Platform/Config.h): "key = value" lines, keys without regard
// to case, comments that start a line with # or ;, the last one wins.
SettingKey FindConfKey(std::string_view text, std::string_view key);
// The text with the key set to `value`: in the line that sets it last, or in a
// line added at the end.
std::optional<std::string> SetConfKey(std::string_view text, std::string_view key, std::string_view value, std::string& problem);

// NativeCameraTweaks.toml (Mac/Camera/camera_settings.c): [Section] lines,
// "Key = value" lines, keys as written, # comments outside "quotes", the
// last one wins.
SettingKey FindTomlKey(std::string_view text, std::string_view section, std::string_view key);
// The text with the key set to `value`: in the line that sets it last
// (only the value; a comment after it stays), or in a line added to the
// section's first header, after the comments that open it; or, with no such
// section, in the section added at the end.
std::optional<std::string> SetTomlKey(std::string_view text, std::string_view section, std::string_view key, std::string_view value,
    std::string& problem);

enum class Extra
{
    Camera,
    Upscaling,
};

// The install whose extras options and status are about: the folder that
// holds the libbg3mse.dylib the game loads and the camera module beside it,
// bin/.
struct ExtrasPlace
{
    std::string Library;
    std::string Options;    // the command that changes them, as said to the player
};
// bin/, the launch option's.
ExtrasPlace LaunchOptionPlace(std::string const& data);
// Why the camera module cannot be used from `library`; empty when it can.
std::string CameraMissingIn(std::string const& library);
// Why MetalFX Temporal cannot be turned on for the libbg3mse.dylib in
// `library` on a Mac with macOS `macos` ("15.2"); empty when it can.
std::string UpscalingUnavailable(std::string const& library, std::string const& macos);

// The extras of the data directory, and what an install or `options` does to them.
class Extras
{
public:
    // `library`: the folder that holds the libbg3mse.dylib the game is to
    // load, and the camera module: the one the install copies from, or the
    // install's (ExtrasPlace). Its camera/NativeCameraTweaks.default.toml is
    // what the camera's settings file is made from, else bin/'s; MetalFX
    // upscaling is offered only when its libbg3mse.dylib has the graphics
    // module. `cameraMissing`: why the camera module is not (or is not
    // being) installed; empty when it is. `confBase`: the text bg3mse.conf
    // is made from when it is not there (the install's default settings);
    // none: it is not made here, as the install's default settings, which
    // turn the Script Extender on, are only the install's to write.
    Extras(Options const& options, Host const& host, std::string data, std::string library, std::string cameraMissing,
        std::optional<std::string> confBase);

    // install: each extra a flag answers, or that is chosen in its
    // file, or that --defaults answers, is settled without a word; the others
    // are asked about on the host's terminal (not on a dry run). No answer
    // leaves the extra as it is, off and not chosen. What is not offered, and
    // why, is said first.
    void SettleForInstall();
    // options: the flags given, else every extra asked about on the
    // terminal, its value now the default. False when there is nothing to
    // ask on and no flag.
    bool SettleForOptions();

    // What Apply writes, in words; none when nothing changes.
    std::vector<std::string> Changes() const;
    // Writes the changes; false, with each problem said, when one was not made.
    bool Apply();
    // None of the changes is made (another bg3mse-install holds the data directory).
    void Unapplied();
    // Reads the files again.
    void Refresh();
    // Whether the extra is on now, as bg3mse reads its file.
    bool IsOn(Extra which) const;
    // Why the extra cannot be used here; empty when it can.
    std::string Unavailable(Extra which) const;
    // What each extra is, as status says it: on, off or unavailable, and
    // from which file.
    std::vector<std::string> StatusLines() const;
    // After an install: the status lines, and how to change them.
    std::vector<std::string> Summary() const;
    // Lines said along the way: an extra not offered and why, a flag not used, a file left as it is.
    std::vector<std::string> const& Notes() const { return notes_; }
    // A change was asked for (by a flag or an answer) that was not made:
    // it ends the verb with a failure, after the rest is done.
    bool Refused() const { return !notSet_.empty(); }
    // Which, in words ("MetalFX Temporal was not set, as said above"); empty when none.
    std::string NotSet() const;

private:
    enum class Source
    {
        None,       // not settled: not chosen, and no answer
        Earlier,    // a choice in its file
        Flag,
        Defaults,
        Answer,
    };
    struct State
    {
        Extra Which;
        std::string File;
        std::string Target;         // the file written: File, or the file it links to
        std::string Unavailable;    // why it cannot be used here; empty when it can
        bool There{ false };        // the file
        std::string Text;           // the file's, or what it is made from
        std::string MadeFrom;       // where Text comes from when the file is not there
        std::string NoBase;         // why what it would be made from cannot be used
        mode_t Mode{ 0644 };
        SettingKey Key;
        bool On{ false };           // as its reader reads it now
        std::optional<bool> Want;   // what to write
        Source From{ Source::None };
        std::string Unanswered;     // why there was no answer
    };

    void Read(State& s);
    // What a flag answers, for install and options alike: what cannot be
    // used is not turned on, and turned off only over a choice of on; a file
    // that cannot be written is not set.
    void SettleFlag(State& s, bool on);
    void NotWritten(Extra which);
    bool Chosen(State const& s) const { return s.Key.Problem.empty() && s.Key.Present; }
    // Why its file cannot be written; empty when it can.
    std::string CannotWrite(State const& s) const;
    std::optional<bool> Flag(Extra which) const;
    std::optional<std::string> NewText(State const& s, std::string& problem) const;
    std::string Describe(State const& s) const;
    std::string Line(State const& s) const;
    // Asks on the terminal; none at the end of the input or after three answers that are not one.
    std::optional<bool> Ask(State const& s, bool defaultOn);
    void Say(std::string const& text) const;

    Options const& o_;
    Host const& h_;
    std::string data_, library_;
    std::optional<std::string> confBase_;
    std::vector<State> states_;
    std::vector<std::string> notes_;
    bool ended_{ false };       // the terminal's input ended
    std::vector<Extra> notSet_; // asked for and not set
};

// bg3mse-install options: the extras of the install at `place` turned on or
// off by flags, or asked about on a terminal; what they are, when neither.
int OptionsVerb(Options const& options, Host const& host, ExtrasPlace const& place);

}
