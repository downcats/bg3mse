#pragma once

#include <Installer/Installer.h>

#include <string>
#include <vector>

// The launch option's install in bg3mse's data directory (Installer.h): what
// bin/ is to hold, read from the directory given with --from, and the change
// that makes it so.

namespace bg3se::mac::installer {

class Setup
{
public:
    Setup(Options const& options, Host const& host);

    // Reads --from's directory and bin/; false, with the reason, when the
    // install cannot be made. Changes nothing.
    bool Plan(std::string& refusal);
    // The changes Execute makes, in words; none when everything is in place.
    std::vector<std::string> Changes() const;
    std::vector<std::string> const& Notes() const { return notes_; }
    // Makes the changes. When it fails, bin/ is as it was.
    bool Execute(std::string& problem);
    // The launch option on the clipboard (not on a dry run), and what to do in Steam.
    void Tell(bool changed) const;

    std::string const& Data() const { return data_; }
    std::string const& Bin() const { return bin_; }
    std::string const& LaunchLine() const { return line_; }
    // Why the camera module is not installed; empty when it is.
    std::string const& CameraMissing() const { return cameraMissing_; }

private:
    struct File
    {
        std::string Name;     // in bin/
        std::string Source;
        mode_t Mode;
        bool Changed;
    };

    void Say(std::string const& text) const;

    Options const& o_;
    Host const& h_;
    std::string data_, bin_, previous_, line_, cameraMissing_;
    std::vector<File> files_;
    std::vector<std::string> stale_;       // this installer's files in bin/ that the release does not have
    std::vector<std::string> carried_;     // files in bin/ that are not this installer's: kept
    std::vector<std::string> leftovers_;   // staging directories of an interrupted run
    std::vector<std::string> settings_;    // settings files to create
    bool createData_{ false };
    bool swap_{ false };
    std::vector<std::string> notes_;
};

// The names this installer gives what it puts in bin/.
std::vector<std::string> OwnBinFiles();

struct ExtrasPlace;

// What is in bin/ and what the launcher's log says of the last start, as
// lines, with the optional extras of the install at `extras` (bin/'s); and
// the game it last started, as its application bundle (empty when there is
// none).
std::vector<std::string> LaunchStatus(Options const& options, Host const& host, ExtrasPlace const& extras, std::string& lastGame);

// Removes bin/ (and bin.previous) and what bg3mse kept of the game's and the
// mods' shaders; with --purge, the settings files too (bg3mse.conf,
// ScriptExtenderSettings.json, NativeCameraTweaks.toml).
int LaunchUninstall(Options const& options, Host const& host);

// The application bundle an executable is in, or empty.
std::string BundleOf(std::string const& executable);

}
