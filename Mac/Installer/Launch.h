#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// bg3mse-launch: starts the game with libbg3mse.dylib named in
// DYLD_INSERT_LIBRARIES, so that the game's executable stays as Steam
// installed it and a Steam update or "Verify integrity" does not take bg3mse
// out (Mac/Installer/LaunchTool.cpp).
//
// Set once as the game's launch option in Steam, which bg3mse-install
// launch-option prints:
//
//   "<directory>/bg3mse-launch" %command%
//
// Steam puts the game's command where %command% is. The launcher names the
// dylib beside it in the variable and replaces itself with that command. The
// variable must be set by the program that starts the game: dyld takes it
// from the environment of the process it starts, and macOS clears it in any
// process of a protected system program (/bin/sh, /usr/bin/env), so it does
// not reach the game through one. dyld ends a process whose inserted library
// is missing or is not arm64; the launcher names the dylib only when it is
// there and is an arm64 library, and otherwise starts the game without it.
//
// What the game's binary allows: its arm64 slice is signed ad hoc, without
// the hardened runtime (CodeDirectory flags 0x2), with no entitlements and no
// __RESTRICT segment, so dyld honours the variable (as read on build
// 4.1.1.7631656).

namespace bg3se::mac::installer {

inline constexpr char kLauncher[] = "bg3mse-launch";

struct LaunchPlan
{
    // The value DYLD_INSERT_LIBRARIES is to have; empty when the game starts
    // without bg3mse.
    std::string Insert;
    // Why it starts without, when it does.
    std::string Why;
};

// `dylib`: the path of libbg3mse.dylib beside the launcher; `file`: its
// contents, or nothing when it is not there; `current`: the variable as it
// is now, or nullptr. The dylib is added after what the variable already
// names, once.

// The file the launcher names: the updater's loader when it is beside the
// launcher, else libbg3mse.dylib. `loaderThere`: whether it is.
std::string LaunchedLibrary(std::string const& directory, bool loaderThere);
LaunchPlan PlanLaunch(std::string const& dylib, std::optional<std::span<uint8_t const>> file, char const* current);

// The executable of the application bundle at `app`, as its Info.plist names
// it: Steam on macOS puts the bundle, not the executable, where %command% is.
// Empty, with the reason in `problem`, when `app` is not a bundle with one.
std::string BundleExecutable(std::string const& app, std::string& problem);

// The launch option for a launcher at `launcher`. Steam does not run it
// through a shell: the path is whole, from the root, and quoted.
std::string LaunchOption(std::string const& launcher);

// The game's own Metal functions, read before the game starts.
//
// Unless shader_translation switches it off, bg3mse translates a mod's
// shaders to fit the game's own Metal functions, which have to have been read
// once for each version of the game (Mac/Shaders/GameInterface.h). The read
// has the system compile every one of them: a minute and a half with the
// machine to itself, five to six minutes while the game runs (as measured, in
// the game and in a program beside it alike). So it is done before the game
// is there: the launcher runs bg3mse-install's shader-interface verb from its
// own directory and waits for it. That ends at once when no mod has a
// material to translate, or when what is kept is the game's, which is every
// start but the first after a game update (bg3mse-install reads when it
// installs). While it takes longer a notice says what is being done, with a
// button that starts the game at once: the read is stopped then, and the game
// does it itself while it runs, as it does whenever the launcher could not
// have it done.

inline constexpr char kShaderInterfaceVerb[] = "shader-interface";

// Whether a value of shader_translation in bg3mse.conf has mods' shaders
// translated: switched on, or "all". Without the key they are.
bool TranslatesShaders(std::string_view value);

// The notice shown while the read takes long: the system's in the launcher,
// a record of what would be shown in tests.
class ReadNotice
{
public:
    virtual ~ReadNotice() = default;
    virtual void Show() = 0;
    // Whether the player has asked for the game to start at once. Returns
    // within a twentieth of a second.
    virtual bool StartNowPressed() = 0;
    virtual void Hide() = 0;
};
inline constexpr char kReadNoticeHeading[] = "bg3mse is loading shaders..";
inline constexpr char kReadNoticeText[] = "";
inline constexpr char kReadNoticeButton[] = "Start now";

struct ShaderRead
{
    enum class Ended
    {
        NotStarted,     // the tool is not there, or could not be started
        AtOnce,         // what is kept is the game's: nothing was read
        Read,
        Failed,
        StartedWithout, // the player asked for the game at once
        TimedOut,
    };
    Ended How{ Ended::NotStarted };
    double Seconds{ 0 };
    std::string Why;    // when it was not read
};

// Runs `tool shader-interface <app>` and waits for it to end, at most
// `timeoutSeconds`; `notice` is shown once it has run for `noticeAfterSeconds`
// and hidden when the wait is over. The tool is started with nothing of
// dyld's in its environment (the launcher's names the library for the game)
// and none of the launcher's files.
ShaderRead ReadShadersBeforeLaunch(std::string const& tool, std::string const& app, ReadNotice* notice, double noticeAfterSeconds = 1.5,
    double timeoutSeconds = 900);

// What the launcher's log says of it; empty for a read that was not needed.
std::string Describe(ShaderRead const& read);

// The launcher's log, <data directory>/logs/launcher.log: one line for each
// start, one more when the game's shaders were read before it or could not
// be, and one when it cannot start the game, for Steam shows nothing of a
// launcher that fails. Kept under kLaunchLogLimit: a larger log becomes
// launcher.log.1, replacing the one before.
//
//   <local time> [<pid>] given "<what Steam gave>" (and <n> more arguments); game "<executable>"; inserted "<library>"
//   <local time> [<pid>] given ...; game ...; not inserted: <why>; the game starts without bg3mse
//   <local time> [<pid>] the game's Metal functions were read for mods' shaders in <n> seconds
//   <local time> [<pid>] the game's Metal functions were not read before the start (<why>); the game reads them itself
//   <local time> [<pid>] cannot start the game: <why>
inline constexpr char kLaunchLog[] = "launcher.log";
inline constexpr size_t kLaunchLogLimit = 256 * 1024;

std::string LaunchLogPath(std::string const& dataDirectory);
// Appends `text` as a line, with the time and this process's id. Failing
// changes nothing for the caller: the game starts whether or not it is written.
void AppendLaunchLog(std::string const& path, std::string const& text);
// The lines of the last start in the log (those with its process id), oldest first.
std::vector<std::string> LastLaunch(std::string const& path);
// The game's executable a line names, or empty.
std::string LaunchedGame(std::vector<std::string> const& lines);

}
