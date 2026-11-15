// bg3mse-install: installs bg3mse for Baldur's Gate 3, and takes it out
// again (Mac/Installer/Installer.h).
//
//   bg3mse-install install [<Game.app>] [--from <dir>] [--dry-run] [--no-clipboard] [<features>]
//   bg3mse-install status [<Game.app>]
//   bg3mse-install uninstall [--purge] [--dry-run]
//   bg3mse-install options [--camera=on|off] [--upscaling=on|off] [--dry-run]
//   bg3mse-install launch-option [--from <dir>]
//   bg3mse-install shader-interface <Game.app> [--to <file>]
//
// <features> is --camera=on|off, --upscaling=on|off, --defaults
//
// install, run from the folder of a release, is all a player does: it puts
// bg3mse in ~/Library/Application Support/bg3mse/bin, puts the game's launch
// option on the clipboard, and says where it goes in Steam.
// --no-clipboard leaves the clipboard alone; the option is printed either way.
//
// install asks, on a terminal, whether to enable each optional
// feature (Native Camera Tweaks, MetalFX Temporal) not chosen before
// (Installer/Extras.h). --camera and --upscaling answer their question;
// --defaults answers every question not otherwise answered with its default,
// off. Nothing is asked without a terminal (standard input and output both
// one, in the foreground) or on a dry run. options asks again, the current
// value the default, or sets what its flags say; without either, it says
// what each extra is, of the install in bin/. A flag that cannot be applied
// ends install and options alike with a failure.
//
// launch-option prints the launch option that starts the game through
// bg3mse-launch in --from's directory (Mac/Installer/Launch.h). It changes nothing.
//
// shader-interface reads the game's own Metal functions, which a mod's
// shaders are translated to fit, and keeps them in shader-interface.txt of
// the settings folder (or in <file>), unless what is kept there is of this
// version of the game already (Mac/Shaders/GameInterface.h). Without --to
// it reads only for an install that has something to translate: a mod with
// a material whose shaders are not the game's own, and shader_translation
// not switched off (Mac/Shaders/ShaderProvision.h, TranslationHasWork).
// install does the same, and bg3mse-launch runs this verb before it starts
// the game (Mac/Installer/Launch.h).
//
// --from names the directory that holds the release's files (bg3mse-launch,
// bg3mse-install, libbg3mse.dylib, libbg3mse-loader.dylib, the camera
// module); without it, the directory of this tool. The settings are kept in
// ~/Library/Application Support/bg3mse.
//
// Whether bg3mse supports the game is what `bg3mse-engine check` says of it:
// every entry of the engine table is as the table expects on the game's
// arm64 executable and libOsiris.dylib, both read from disk. install says
// when it does not.

#include <Installer/Installer.h>
#include <Installer/Launch.h>
#include <Installer/ToolHost.h>
#include <Platform/Config.h>
#include <Platform/FunctionExtents.h>
#include <Platform/Log.h>
#include <Platform/Symbols.h>
#include <Release/Version.h>
#include <Shaders/GameInterface.h>
#include <Shaders/ShaderProvision.h>
#include <Shaders/TranslationCache.h>

#include <climits>
#include <cstdlib>
#include <mach-o/dyld.h>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

using namespace bg3se::mac;
namespace installer = bg3se::mac::installer;

namespace {

// One file, named on both sides: the installer's library knows nothing of shaders.
static_assert(std::string_view(installer::kShaderInterfaceFile) == kMacInterfaceFile);
static_assert(std::string_view(installer::kShaderCacheDirectory) == kTranslationCacheDirectory);

std::string ToolDirectory()
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return ".";
    char resolved[PATH_MAX];
    if (!realpath(path.c_str(), resolved)) return ".";
    std::string dir = resolved;
    return dir.substr(0, dir.rfind('/'));
}

int Usage();

// The launch option for the launcher in --from's directory, or this tool's.
int LaunchOption(std::vector<std::string> const& args)
{
    std::string from;
    if (args.size() == 3 && args[1] == "--from") {
        char resolved[PATH_MAX];
        if (!realpath(args[2].c_str(), resolved)) {
            fprintf(stderr, "bg3mse-install: %s: no such directory\n", args[2].c_str());
            return 2;
        }
        from = resolved;
    } else if (args.size() != 1) {
        return Usage();
    } else {
        from = ToolDirectory();
    }
    std::string launcher = from + "/" + installer::kLauncher;
    for (auto const& file : { launcher, from + "/" + installer::kLibrary }) {
        if (access(file.c_str(), F_OK) != 0) {
            fprintf(stderr, "bg3mse-install: %s is not there\n", file.c_str());
            return 1;
        }
    }
    printf("%s\n", installer::LaunchOption(launcher).c_str());
    fprintf(stderr, "Paste it into Launch Options under Properties > General in Steam.\n");
    return 0;
}

// The game's own Metal functions into the kept file, unless it is of this game already.
int ShaderInterface(std::vector<std::string> const& args)
{
    std::string app, file;
    for (size_t i = 1; i < args.size(); i++) {
        if (args[i] == "--to" && i + 1 < args.size() && file.empty()) file = args[++i];
        else if (!args[i].starts_with("--") && app.empty()) app = args[i];
        else return Usage();
    }
    if (app.empty()) return Usage();
    while (app.size() > 1 && app.back() == '/') app.pop_back();
    if (file.empty()) {
        std::string data = DataDirectory();
        if (data.empty()) {
            fprintf(stderr, "bg3mse-install: there is no home directory to keep the file in\n");
            return 1;
        }
        mkdir(data.c_str(), 0755);
        file = data + "/" + kMacInterfaceFile;
        // The player's own install: nothing is read for one that has nothing to translate, a game whose shader
        // files are not there among them. What is kept for the game is found without planning.
        std::string whyNot;
        if ((MacInterfaceStamp(app).empty() || MacInterfaceToBeRead(app, file)) &&
            !TranslationHasWork(InstallPaths::ForGame(app), TranslationSettingOf(ConfValue(kShaderTranslationSetting)), whyNot)) {
            printf("the game's Metal functions are not read: %s\n", whyNot.c_str());
            return 0;
        }
    }
    std::string why;
    auto ensured = EnsureMacInterface(app, file, why);
    switch (ensured.What) {
    case EnsuredMacInterface::How::Kept:
        printf("%zu function pairs are kept in %s for this version of the game already\n", ensured.Interface->Passes(), file.c_str());
        return 0;
    case EnsuredMacInterface::How::Read:
        printf("%zu function pairs read; kept in %s\n", ensured.Interface->Passes(), file.c_str());
        return 0;
    case EnsuredMacInterface::How::OfNewerBuild:
        printf("%s was written by a later build of bg3mse than this one: left as it is\n", file.c_str());
        return 0;
    case EnsuredMacInterface::How::NotMade:
        break;
    }
    fprintf(stderr, "bg3mse-install: the game's Metal functions were not read: %s\n", why.c_str());
    return 1;
}

int Usage()
{
    fprintf(stderr, "usage: bg3mse-install install [<Game.app>] [--from <dir>] [--dry-run] [--no-clipboard] [<features>]\n"
                    "       bg3mse-install status [<Game.app>]\n"
                    "       bg3mse-install uninstall [--purge] [--dry-run]\n"
                    "       bg3mse-install options [--camera=on|off] [--upscaling=on|off] [--dry-run]\n"
                    "       bg3mse-install launch-option [--from <dir>]\n"
                    "       bg3mse-install shader-interface <Game.app> [--to <file>]\n"
                    "       bg3mse-install --version\n"
                    "<features>: --camera=on|off (Native Camera Tweaks), --upscaling=on|off (MetalFX Temporal), --defaults\n"
                    "            (off, for each one not chosen before); without them, a terminal is asked\n");
    return 2;
}

}

int main(int argc, char** argv)
{
    SetLogStream(nullptr);
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 1 && args[0] == "--version") {
        printf("bg3mse-install %s (commit %s, %s)\n", version::kVersion, version::kCommit, version::kChannel);
        return 0;
    }
    if (!args.empty() && args[0] == "launch-option") return LaunchOption(args);
    if (!args.empty() && args[0] == installer::kShaderInterfaceVerb) return ShaderInterface(args);
    if (args.empty()) return Usage();
    installer::Options options;
    if (args[0] == "install") options.What = installer::Verb::Install;
    else if (args[0] == "status") options.What = installer::Verb::Status;
    else if (args[0] == "uninstall") options.What = installer::Verb::Uninstall;
    else if (args[0] == "options") options.What = installer::Verb::Options;
    else return Usage();
    bool clipboard = true;
    bool optionsVerb = options.What == installer::Verb::Options;
    bool changes = options.What != installer::Verb::Status;
    bool installs = options.What == installer::Verb::Install;
    bool extras = installs || optionsVerb;
    // --camera=on|off and --upscaling=on|off: 1 when `arg` is the flag, 0
    // when it is not, -1 when its value is neither.
    auto onOff = [](std::string const& arg, char const* flag, std::optional<bool>& to) {
        std::string prefix = std::string(flag) + "=";
        if (!arg.starts_with(prefix)) return 0;
        std::string value = arg.substr(prefix.size());
        if (value != "on" && value != "off") {
            fprintf(stderr, "bg3mse-install: %s takes on or off, not \"%s\"\n", flag, value.c_str());
            return -1;
        }
        to = value == "on";
        return 1;
    };
    for (size_t i = 1; i < args.size(); i++) {
        int camera = extras ? onOff(args[i], "--camera", options.Camera) : 0;
        int upscaling = extras ? onOff(args[i], "--upscaling", options.Upscaling) : 0;
        if (camera < 0 || upscaling < 0) return 2;
        if (camera || upscaling) continue;
        if (args[i] == "--defaults" && installs) {
            options.Defaults = true;
        } else if (!args[i].starts_with("--") && options.App.empty()) {
            options.App = args[i];
        } else if (args[i] == "--dry-run" && changes) {
            options.DryRun = true;
        } else if (args[i] == "--purge" && options.What == installer::Verb::Uninstall) {
            options.Purge = true;
        } else if (args[i] == "--no-clipboard" && changes && !optionsVerb) {
            clipboard = false;
        } else if (args[i] == "--from" && i + 1 < args.size() && options.What != installer::Verb::Uninstall && !optionsVerb) {
            char resolved[PATH_MAX];
            if (!realpath(args[++i].c_str(), resolved)) {
                fprintf(stderr, "bg3mse-install: %s: no such directory\n", args[i].c_str());
                return 2;
            }
            options.From = resolved;
        } else {
            return Usage();
        }
    }
    // uninstall and options are of bin/: they take no game.
    if ((optionsVerb || options.What == installer::Verb::Uninstall) && !options.App.empty()) return Usage();
    if (options.From.empty()) options.From = ToolDirectory();

    installer::Host host = installer::ToolHost();
    if (clipboard) {
        host.Clipboard = [](std::string const& text, std::string& problem) { return installer::CopyToClipboard("/usr/bin/pbcopy", text, problem); };
    }
    // A terminal to ask on: the questions are read from standard input and
    // shown on standard output, and a process in the background would be
    // stopped by reading its terminal.
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && tcgetpgrp(STDIN_FILENO) == getpgrp()) host.Terminal = STDIN_FILENO;
    return installer::Run(options, host);
}
