#include <Installer/Installer.h>

#include <Installer/Extras.h>
#include <Installer/Launch.h>
#include <Installer/Setup.h>
#include <Installer/Support.h>
#include <Platform/Config.h>
#include <Release/GameAdvice.h>

#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <climits>
#include <cstring>
#include <ctime>
#include <libproc.h>
#include <optional>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <vector>

namespace bg3se::mac::installer {

char const kDefaultConf[] =
    "# bg3mse's settings: one \"key = value\" per line.\n"
    "\n"
    "# Norbyte's Script Extender, started in the game.\n"
    "script_extender = 1\n";

// Every setting of Norbyte's at its default, as no file at all would leave them.
char const kDefaultExtenderSettings[] = "{\n}\n";

bool IsAppleSilicon()
{
    int arm64 = 0;
    size_t size = sizeof(arm64);
    return sysctlbyname("hw.optional.arm64", &arm64, &size, nullptr, 0) == 0 && arm64 == 1;
}

std::string MacOSVersion()
{
    char version[64] = {};
    size_t size = sizeof(version) - 1;
    if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) != 0) return {};
    return version;
}

namespace {

std::string StringOf(CFDictionaryRef dict, CFStringRef key)
{
    CFTypeRef value = CFDictionaryGetValue(dict, key);
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    char text[1024];
    if (!CFStringGetCString(static_cast<CFStringRef>(value), text, sizeof(text), kCFStringEncodingUTF8)) return {};
    return text;
}

struct Game
{
    std::string App, MacOS, ExecutableName, Executable, Osiris;
    std::string Identifier, Version;
};

constexpr char kGameIdentifier[] = "com.larian.bg3";

std::optional<Game> OpenGame(std::string app, std::string& problem)
{
    while (app.size() > 1 && app.back() == '/') app.pop_back();
    Game game;
    char resolved[PATH_MAX];
    if (!realpath(app.c_str(), resolved)) {
        problem = app + ": " + Errno();
        return std::nullopt;
    }
    game.App = resolved;
    std::string plist = game.App + "/Contents/Info.plist";
    Mapped file;
    if (!file.Open(plist, problem)) return std::nullopt;
    if (!file.Present()) {
        problem = game.App + " is not an application bundle (it has no Contents/Info.plist)";
        return std::nullopt;
    }
    CFDataRef data = CFDataCreate(nullptr, file.Bytes().data(), (CFIndex)file.Bytes().size());
    CFPropertyListRef list = data ? CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr) : nullptr;
    if (data) CFRelease(data);
    if (!list || CFGetTypeID(list) != CFDictionaryGetTypeID()) {
        if (list) CFRelease(list);
        problem = plist + " cannot be read";
        return std::nullopt;
    }
    auto dict = static_cast<CFDictionaryRef>(list);
    game.Identifier = StringOf(dict, CFSTR("CFBundleIdentifier"));
    game.ExecutableName = StringOf(dict, CFSTR("CFBundleExecutable"));
    game.Version = StringOf(dict, CFSTR("CFBundleShortVersionString"));
    if (game.Version.empty()) game.Version = StringOf(dict, CFSTR("CFBundleVersion"));
    CFRelease(list);
    if (game.Identifier != kGameIdentifier) {
        problem = game.App + " is not Baldur's Gate 3 (its bundle identifier is " + Quoted(game.Identifier) + ", not " + kGameIdentifier + ")";
        return std::nullopt;
    }
    if (game.ExecutableName.empty() || game.ExecutableName.find('/') != std::string::npos || game.ExecutableName[0] == '.') {
        problem = plist + " names no executable";
        return std::nullopt;
    }
    game.MacOS = game.App + "/Contents/MacOS";
    game.Executable = game.MacOS + "/" + game.ExecutableName;
    game.Osiris = game.App + "/Contents/Frameworks/libOsiris.dylib";
    struct stat st{};
    if (lstat(game.Executable.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        problem = game.Executable + " is not there, or is not a file";
        return std::nullopt;
    }
    return game;
}

class Verbs
{
public:
    Verbs(Options const& o, Host const& h) : o_(o), h_(h) {}

    // The launch option's install (Setup.h).
    int LaunchInstall()
    {
        std::string problem;
        bool game = !o_.App.empty();
        if (game && !Open(o_.App, problem)) return Refuse(problem);
        if (!o_.DryRun && h_.RunningGames) {
            auto running = h_.RunningGames();
            if (!running.empty()) return Refuse("the game is running (" + running.front() + "); quit it first");
        }

        Setup setup(o_, h_);
        if (!setup.Plan(problem)) return Refuse(problem);
        if (game) {
            WarnWhenUnsupported();
        } else if (std::string found = FoundGame(); !found.empty()) {
            // The game where Steam puts it, or the one last started: said of as well.
            std::string ignored;
            if (Open(found, ignored)) WarnWhenUnsupported();
        }
        size_t told = setup.Notes().size();
        for (auto const& note : setup.Notes()) Say(note);
        // The optional extras: asked about only now that nothing refuses the install.
        Extras extras(o_, h_, setup.Data(), o_.From, setup.CameraMissing(), kDefaultConf);
        extras.SettleForInstall();
        auto changes = setup.Changes();
        auto extraChanges = extras.Changes();
        auto summary = [&] {
            for (auto const& line : extras.Summary()) Say(line);
            Say("");
        };
        if (changes.empty() && extraChanges.empty()) {
            Say("Nothing to do, already installed.");
            summary();
            setup.Tell(false);
            int status = Installed(extras);
            if (!o_.DryRun) ReadShaderInterface(game ? g_.App : FoundGame());
            return status;
        }
        if (o_.DryRun) {
            Say("Dry run; nothing is changed. bg3mse-install " + std::string(VerbName()) + " would:");
            for (auto const& c : changes) Say("  " + c);
            for (auto const& c : extraChanges) Say("  " + c);
            Say("");
            summary();
            setup.Tell(false);
            return extras.Refused() ? 1 : 0;
        }
        for (auto const& c : changes) Say("  " + c);
        if (!setup.Execute(problem)) {
            Say("bg3mse-install: " + problem);
            Say("Nothing in the game was changed.");
            return 1;
        }
        for (size_t i = told; i < setup.Notes().size(); i++) Say(setup.Notes()[i]);
        // bg3mse is in place; an answer that cannot be written is said, and
        // the install goes on: bg3mse-install options can write it later,
        // and the install ends with a failure that says so.
        if (!extraChanges.empty()) {
            for (auto const& c : extraChanges) Say("  " + c);
            DataLock lock(setup.Data());
            if (!lock.Held()) {
                Say("bg3mse-install: another bg3mse-install is working in " + setup.Data() + "; the optional features are as they were");
                extras.Unapplied();
            } else {
                extras.Apply();
            }
        }
        extras.Refresh();
        Say("");
        summary();
        setup.Tell(true);
        int status = Installed(extras);
        ReadShaderInterface(game ? g_.App : FoundGame());
        return status;
    }

    int OptionsHere()
    {
        return OptionsVerb(o_, h_, LaunchOptionPlace(ResolvedPath(h_.DataDirectory)));
    }

    // What is installed for the launch option, and the game: the game given,
    // or the one bg3mse-launch last started, or the one where Steam puts it.
    int LaunchStatusVerb()
    {
        std::string last;
        for (auto const& line : LaunchStatus(o_, h_, LaunchOptionPlace(ResolvedPath(h_.DataDirectory)), last)) Say(line);
        std::string app = !o_.App.empty() ? o_.App : !last.empty() ? last : FoundGame();
        if (app.empty()) {
            Say("No game was given, and none was started through bg3mse-launch: bg3mse-install status <Game.app> says which "
                "version of the game it is too.");
            return 0;
        }
        Say("");
        std::string problem;
        if (!Open(app, problem)) {
            Say("bg3mse-install: " + problem);
            return o_.App.empty() ? 0 : 1;
        }
        Say("Baldur's Gate 3: " + g_.App);
        Say("  version " + (g_.Version.empty() ? std::string("(none in Info.plist)") : g_.Version));
        return 0;
    }

private:
    bool Open(std::string const& app, std::string& problem)
    {
        auto game = OpenGame(app, problem);
        if (!game) return false;
        g_ = *game;
        return true;
    }

    // The game's own Metal functions, read for the game (Host::ReadShaderInterface) unless the settings
    // switch the translation of mods' shaders off, or there is nothing to read (Host::ShaderInterfaceKept).
    // The last thing a verb does, and nothing it fails by: without them the game reads them itself when
    // it needs them.
    void ReadShaderInterface(std::string const& app)
    {
        if (app.empty() || h_.DataDirectory.empty() || !h_.ShaderInterfaceKept || !h_.ReadShaderInterface) return;
        // Translated unless the key is there and says otherwise: no file and no key are "on".
        Mapped conf;
        std::string problem;
        std::optional<std::string> wanted;
        if (conf.Open(h_.DataDirectory + "/" + kConfFile, problem) && conf.Present()) {
            wanted = ConfLookup({ (char const*)conf.Bytes().data(), conf.Bytes().size() }, kShaderTranslationSetting);
        }
        if (wanted && !TranslatesShaders(*wanted)) return;
        std::string file = h_.DataDirectory + "/" + kShaderInterfaceFile;
        if (h_.ShaderInterfaceKept(app, file)) return;
        Say("");
        Say("Loading the game's shaders for mods that have their own. Takes a minute or two...");
        fflush(h_.Out);
        time_t start = time(nullptr);
        if (h_.ReadShaderInterface(app, file, problem)) {
            Say("Done, in " + std::to_string((long)(time(nullptr) - start)) + " seconds.");
        } else {
            Say("Couldn't load them (" + problem + "). That's fine, the game loads them when it starts.");
        }
    }

    // The end of an install that put bg3mse in place, or would on a dry run:
    // a failure when an optional extra asked for was not (or would not be) set.
    int Installed(Extras const& extras)
    {
        if (!extras.Refused()) return 0;
        if (o_.DryRun) return 1;
        Say("bg3mse itself is installed; only " + extras.NotSet() + ".");
        return kInstalledWithoutExtras;
    }

    // The game bg3mse-launch last started, else the one where Steam puts it.
    std::string FoundGame() const
    {
        if (h_.DataDirectory.empty()) return {};
        std::string last = BundleOf(LaunchedGame(LastLaunch(LaunchLogPath(h_.DataDirectory))));
        if (!last.empty() && Exists(last)) return last;
        if (!h_.DefaultGame.empty() && Exists(h_.DefaultGame + "/Contents/Info.plist")) return h_.DefaultGame;
        return {};
    }

    void WarnWhenUnsupported()
    {
        if (!h_.Supports) return;
        Mapped executable, osiris;
        std::string why;
        if (!executable.Open(g_.Executable, why) || !executable.Present() || !osiris.Open(g_.Osiris, why) || !osiris.Present()) return;
        if (!h_.Supports(executable.Bytes(), osiris.Bytes(), why)) {
            std::string checked;
            bool selfUpdating = true;
            if (FILE* f = fopen((o_.From + "/" + kReleaseManifest).c_str(), "rb")) {
                char text[64 * 1024];
                size_t n = fread(text, 1, sizeof(text), f);
                fclose(f);
                checked = CheckedGameVersion(std::string_view(text, n));
                selfUpdating = SelfUpdating(std::string_view(text, n));
            }
            if (FILE* f = fopen((h_.DataDirectory + "/ScriptExtenderUpdaterConfig.json").c_str(), "rb")) {
                char text[64 * 1024];
                size_t n = fread(text, 1, sizeof(text), f);
                fclose(f);
                if (UpdatesDisabled(std::string_view(text, n))) selfUpdating = false;
            }
            Say("Warning: this bg3mse doesn't support your game version (" + g_.Version + "). Installed anyway.");
            Say(UnsupportedGameAdvice(g_.Version, checked, selfUpdating));
        }
    }

    void Say(std::string const& text) { fprintf(h_.Out, "%s\n", text.c_str()); }

    int Refuse(std::string const& why)
    {
        Say("bg3mse-install: " + why);
        Say("Nothing was changed.");
        return 1;
    }

    char const* VerbName() const
    {
        switch (o_.What) {
        case Verb::Install: return "install";
        case Verb::Uninstall: return "uninstall";
        case Verb::Status: return "status";
        case Verb::Options: return "options";
        }
        return "?";
    }

    Options const& o_;
    Host const& h_;
    Game g_;
};

}

int Run(Options const& options, Host const& host)
{
    Verbs verbs(options, host);
    switch (options.What) {
    case Verb::Status: return verbs.LaunchStatusVerb();
    case Verb::Install: return verbs.LaunchInstall();
    case Verb::Uninstall: return LaunchUninstall(options, host);
    case Verb::Options: return verbs.OptionsHere();
    }
    return 1;
}

// Whether the process has a file under `prefix` mapped: one of its regions, in address order, is backed by it.
// A process whose regions cannot be read at all counts as one that has: what is not known is not taken as no.
static bool MapsFileUnder(pid_t pid, std::string const& prefix)
{
    proc_regionwithpathinfo region{};
    uint64_t address = 0;
    bool read = false;
    while (proc_pidinfo(pid, PROC_PIDREGIONPATHINFO, address, &region, sizeof(region)) == (int)sizeof(region)) {
        read = true;
        if (std::string_view(region.prp_vip.vip_path).starts_with(prefix)) return true;
        uint64_t next = region.prp_prinfo.pri_address + region.prp_prinfo.pri_size;
        if (next <= address) break;
        address = next;
    }
    return !read;
}

std::vector<std::string> RunningGames(std::string const& dataDirectory)
{
    std::vector<std::string> games;
    if (dataDirectory.empty()) return games;
    char real[PATH_MAX];
    std::string prefix = (realpath(dataDirectory.c_str(), real) ? std::string(real) : dataDirectory) + "/";
    int count = proc_listallpids(nullptr, 0);
    if (count <= 0) return games;
    std::vector<pid_t> pids((size_t)count + 64);
    count = proc_listallpids(pids.data(), (int)(pids.size() * sizeof(pid_t)));
    for (int i = 0; i < count; i++) {
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (pids[(size_t)i] <= 0 || proc_pidpath(pids[(size_t)i], path, sizeof(path)) <= 0) continue;
        std::string bundle = BundleOf(path), problem;
        if (bundle.empty()) continue;
        auto game = OpenGame(bundle, problem);
        char resolved[PATH_MAX];
        if (game && realpath(game->Executable.c_str(), resolved) && strcmp(resolved, path) == 0 && MapsFileUnder(pids[(size_t)i], prefix)) {
            games.push_back(path);
        }
    }
    return games;
}

bool CopyToClipboard(std::string const& program, std::string const& text, std::string& problem)
{
    std::string output;
    // pbcopy reads its input in the encoding of the locale: UTF-8, whatever the caller's.
    int status = RunProgram({ program }, output, &text, { "LANG=en_US.UTF-8", "LC_ALL=en_US.UTF-8" });
    if (status != 0) {
        problem = program + (status < 0 ? " did not run" : " failed") + (output.empty() ? std::string() : ": " + output);
        while (!problem.empty() && problem.back() == '\n') problem.pop_back();
        return false;
    }
    return true;
}

}
