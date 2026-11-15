#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// bg3mse-install's verbs (Mac/Installer/InstallTool.cpp). bg3mse's files go
// to ~/Library/Application Support/bg3mse/bin, and the game is started
// through bg3mse-launch (Mac/Installer/Launch.h), set once as its launch
// option in Steam. Nothing of the game's is changed, so a game update or
// Steam's "Verify integrity of game files" leaves bg3mse in.
//
// install    copies bg3mse-launch, bg3mse-install, the libraries, the
//            release's manifest and the camera module's licence texts into
//            bin/, all at once: they are staged beside it, checked, and
//            swapped in by one rename, and the files they replace are kept in
//            bin.previous. Creates the settings files when they are missing.
//            Puts the launch option on the clipboard and says what to do in
//            Steam. When a mod has shaders to translate (and
//            shader_translation does not switch that off) and the game is
//            known (given, last started, or where Steam puts it), reads the
//            game's own Metal functions for that, unless they are kept for
//            this version of the game already.
// status     what is in bin/, the optional extras (Extras.h), the last start
//            through bg3mse-launch, and the game's version (given, or the one
//            last started).
// uninstall  removes bin/, what bg3mse kept of the game's and the mods'
//            shaders, and the builds the updater fetched; the settings and
//            the updater's manifests stay, unless purged. The launch option
//            has to be cleared in Steam.
// options    turns the optional extras on or off (Extras.h).
//
// install settles the optional extras too, before anything is changed or
// read: each one not chosen before is asked about on a terminal, or answered
// by a flag.
//
// Each verb works out every change first, from what is there, and refuses
// before changing anything when one of them cannot be made; a dry run prints
// the changes and makes none. Each is safe to repeat: a change that is
// already made is not made again. Files are replaced by renaming, never
// rewritten in place.

namespace bg3se::mac::installer {

enum class Verb
{
    Install,
    Status,
    Uninstall,
    Options,
};

struct Options
{
    Verb What{ Verb::Status };
    std::string App;
    std::string From;          // the directory that holds the libraries to install
    bool DryRun{ false };
    bool Purge{ false };       // uninstall: remove the settings files too
    // The optional extras (Extras.h): --camera=on|off, --upscaling=on|off,
    // and --defaults, which answers every question not otherwise answered with its default.
    std::optional<bool> Camera;
    std::optional<bool> Upscaling;
    bool Defaults{ false };
};

// What the verbs need of the machine: given by the tool, and by tests.
struct Host
{
    bool AppleSilicon{ false };
    // Whether bg3mse supports the game whose executable (its arm64 slice is
    // read) and libOsiris.dylib these are; why not, when it does not.
    std::function<bool(std::span<uint8_t const> executable, std::span<uint8_t const> osiris, std::string& why)> Supports;
    // ~/Library/Application Support/bg3mse; empty when there is no home directory.
    std::string DataDirectory;
    FILE* Out{ stdout };
    // Puts the launch option on the clipboard; none: it is only printed.
    std::function<bool(std::string const& text, std::string& problem)> Clipboard;
    // The executables of the copies of the game that are running with this
    // data directory's bg3mse (RunningGames below).
    std::function<std::vector<std::string>()> RunningGames;
    // Where the game is when Steam installed it in its own folder; checked
    // for when no game is given and none was started through bg3mse-launch.
    std::string DefaultGame;
    // The game's own Metal functions, which a mod's shaders are translated
    // against (Mac/Shaders/GameInterface.h). Reading them takes a minute or
    // two for each version of the game; done by the installer, the game
    // does not have to when it starts. ShaderInterfaceKept: whether there
    // is nothing to read: `file` holds them for the game at `app` as it is
    // now (or is a later build's to keep), or the install has no mod
    // material to translate. ReadShaderInterface: reads them and keeps them
    // there. None: the installer leaves them to the game.
    std::function<bool(std::string const& app, std::string const& file)> ShaderInterfaceKept;
    std::function<bool(std::string const& app, std::string const& file, std::string& problem)> ReadShaderInterface;
    // Where the answers to the installer's questions are read from: a
    // terminal, in the foreground, that Out is shown on too. -1 when there is
    // none to ask on, and nothing is asked.
    int Terminal{ -1 };
    // This Mac's macOS version ("15.2"); empty when it is not known.
    std::string MacOSVersion;
};

// 0 when done (or nothing was to be done), 1 when refused or failed,
// kInstalledWithoutExtras when bg3mse was installed but an optional feature
// asked for was not set.
inline constexpr int kInstalledWithoutExtras = 2;
int Run(Options const& options, Host const& host);

// The names the installer gives what it puts in the data directory.
inline constexpr char kTemporarySuffix[] = ".bg3mse-new";
inline constexpr char kLibrary[] = "libbg3mse.dylib";
inline constexpr char kCameraLibrary[] = "libbg3mse-camera.dylib";
// The updater's loader (Mac/Updater/Loader.cpp): what bg3mse-launch has the game load.
inline constexpr char kLoaderLibrary[] = "libbg3mse-loader.dylib";
inline constexpr char kConfFile[] = "bg3mse.conf";
// What the launch option's install keeps in the data directory.
inline constexpr char kBinDirectory[] = "bin";
inline constexpr char kPreviousDirectory[] = "bin.previous";
inline constexpr char kStagingPrefix[] = "bin.staging-";
inline constexpr char kInstallTool[] = "bg3mse-install";
inline constexpr char kReleaseManifest[] = "manifest.json";
inline constexpr char kCameraDirectory[] = "camera";
inline constexpr char const* kCameraLicences[] = { "LICENSE", "COPYING", "EXCEPTIONS" };
inline constexpr char kExtenderSettingsFile[] = "ScriptExtenderSettings.json";
// The game's Metal functions as the installer or the game read them, in the
// data directory (Mac/Shaders/GameInterface.h: kMacInterfaceFile).
inline constexpr char kShaderInterfaceFile[] = "shader-interface.txt";
// And the mods' materials it has translated (Mac/Shaders/TranslationCache.h).
inline constexpr char kShaderCacheDirectory[] = "shader-cache";
// The builds and the channel's manifest the updater fetched, where it keeps
// them unless its settings say otherwise (Mac/Updater/Config.h: CachePath).
inline constexpr char kUpdaterCacheDirectory[] = "Cache";
// The camera module's settings file in the data directory (Mac/Camera/camera_settings.h),
// and the one it writes when there is none, as the release ships it in camera/.
inline constexpr char kCameraSettingsFile[] = "NativeCameraTweaks.toml";
inline constexpr char kCameraDefaultSettings[] = "NativeCameraTweaks.default.toml";

// The text of the settings files the installer creates when they are missing.
extern char const kDefaultConf[];
extern char const kDefaultExtenderSettings[];

// Whether this Mac has an Apple processor (also when this runs under Rosetta).
bool IsAppleSilicon();

// This Mac's macOS version ("15.2"), as the system gives it; empty when it does not.
std::string MacOSVersion();

// The executables of the running processes that are Baldur's Gate 3 (in an
// application bundle whose identifier is the game's) and have a file of
// `dataDirectory` mapped: started through its bin/bg3mse-launch, they run
// its loader, and the build it loaded, from there. A copy of the game that
// runs without that bg3mse, or with another data directory's, is not one;
// one whose memory cannot be read is.
std::vector<std::string> RunningGames(std::string const& dataDirectory);

// Puts `text` on the clipboard through `program` (/usr/bin/pbcopy), as UTF-8.
bool CopyToClipboard(std::string const& program, std::string const& text, std::string& problem);

}
