// Tests of bg3mse-install and bg3mse-launch (Mac/Installer): a stand-in game
// built with the tests, started through the launcher, and the launch
// option's install, its extras and its uninstall.
//
// Nothing outside the build directory is changed: the settings go to a home
// directory made here.

#include <Installer/Extras.h>
#include <Installer/Installer.h>
#include <Installer/Launch.h>
#include <Installer/Setup.h>
#include <Platform/Config.h>
#include <Updater/Stamp.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <mach-o/dyld.h>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <climits>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>
#include <vector>

extern char** environ;

using namespace bg3se::mac::installer;
namespace fs = std::filesystem;

namespace {

int gFailures = 0;
char const* gTest = "";

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; name(); } static void name()

using Bytes = std::vector<uint8_t>;

bool Contains(std::string const& text, std::string const& part)
{
    return text.find(part) != std::string::npos;
}

// ---- A stand-in game, built with the tests --------------------------------

std::string const kFixtures = BG3MSE_INSTALLER_FIXTURES;
std::string const kWork = BG3MSE_INSTALLER_WORK;
std::string const kExecutableName = "Baldur's Gate 3";

Bytes ReadFile(std::string const& path)
{
    Bytes b;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END);
    b.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
    fclose(f);
    return b;
}

// Replaced as Steam replaces a file: a new one, renamed into place.
void WriteFile(std::string const& path, Bytes const& b, mode_t mode = 0755)
{
    std::string temporary = path + ".writing";
    FILE* f = fopen(temporary.c_str(), "wb");
    if (!f) return;
    fwrite(b.data(), 1, b.size(), f);
    fclose(f);
    chmod(temporary.c_str(), mode);
    rename(temporary.c_str(), path.c_str());
}

void WriteText(std::string const& path, std::string const& text)
{
    WriteFile(path, Bytes(text.begin(), text.end()), 0644);
}

std::string ReadText(std::string const& path)
{
    Bytes b = ReadFile(path);
    return std::string(b.begin(), b.end());
}

ino_t Inode(std::string const& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0 ? st.st_ino : 0;
}

bool Exists(std::string const& path)
{
    struct stat st{};
    return lstat(path.c_str(), &st) == 0;
}

// A program's exit status and output.
struct Ran
{
    int Status{ -1 };
    std::string Output;
};

// With no terminal: standard input is /dev/null, and the program runs in a
// session of its own, so that it cannot open the terminal the tests were
// started from (/dev/tty) either.
Ran RunProgram(std::vector<std::string> const& args, std::vector<std::string> const& extraEnvironment = {})
{
    Ran ran;
    int pipe[2];
    if (::pipe(pipe) != 0) return ran;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe[0]);
    std::vector<char*> argv, envp;
    for (auto const& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char** e = environ; *e; e++) {
        std::string entry = *e;
        bool replaced = false;
        for (auto const& extra : extraEnvironment) replaced = replaced || entry.starts_with(extra.substr(0, extra.find('=') + 1));
        if (!replaced) environment.push_back(entry);
    }
    for (auto const& extra : extraEnvironment) environment.push_back(extra);
    for (auto& e : environment) envp.push_back(e.data());
    envp.push_back(nullptr);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    pid_t pid = 0;
    int spawned = posix_spawn(&pid, args[0].c_str(), &actions, &attributes, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(pipe[1]);
    if (spawned != 0) {
        close(pipe[0]);
        return ran;
    }
    char buffer[4096];
    ssize_t n;
    while ((n = read(pipe[0], buffer, sizeof(buffer))) > 0) ran.Output.append(buffer, (size_t)n);
    close(pipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    ran.Status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return ran;
}

std::string PlistOf(std::string const& identifier, std::string const& version)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\"><dict>\n"
           "<key>CFBundleIdentifier</key><string>" + identifier + "</string>\n"
           "<key>CFBundleExecutable</key><string>" + kExecutableName + "</string>\n"
           "<key>CFBundleShortVersionString</key><string>" + version + "</string>\n"
           "</dict></plist>\n";
}

// A bundle in the work directory with the given executable, an Info.plist
// that names the game, and a libOsiris.dylib (the stand-in library).
struct FakeGame
{
    std::string Root, App, MacOS, Executable, Data, From;

    explicit FakeGame(std::string const& name, std::string const& executable, std::string const& identifier = "com.larian.bg3")
    {
        Root = kWork + "/" + name;
        fs::remove_all(Root);
        App = Root + "/Baldur's Gate 3.app";
        MacOS = App + "/Contents/MacOS";
        fs::create_directories(MacOS);
        fs::create_directories(App + "/Contents/Frameworks");
        WriteText(App + "/Contents/Info.plist", PlistOf(identifier, "1.0.0"));
        Executable = MacOS + "/" + kExecutableName;
        WriteFile(Executable, ReadFile(executable));
        WriteFile(App + "/Contents/Frameworks/libOsiris.dylib", ReadFile(kFixtures + "/standin/" + kLibrary));
        Data = Root + "/home/Library/Application Support/bg3mse";
        fs::create_directories(Root + "/home/Library/Application Support");
        From = kFixtures + "/standin";
    }
};

struct Result
{
    int Status{ -1 };
    std::string Output;
};

// What the verbs print, read back.
std::string Printed(FILE* out)
{
    std::string text;
    fflush(out);
    fseek(out, 0, SEEK_END);
    text.resize((size_t)ftell(out));
    fseek(out, 0, SEEK_SET);
    if (fread(text.data(), 1, text.size(), out) != text.size()) text.clear();
    fclose(out);
    return text;
}

std::string const kFixture = kFixtures + "/game";

// ---- Starting the game through the launcher ---------------------------------

TEST(the_launcher_names_the_dylib_only_when_it_can_be_loaded)
{
    std::string dylib = "/x/libbg3mse.dylib";
    Bytes library = ReadFile(kFixtures + "/standin/" + kLibrary), executable = ReadFile(kFixture);
    CHECK(!library.empty() && !executable.empty());
    auto plan = PlanLaunch(dylib, std::span<uint8_t const>(library), nullptr);
    CHECK(plan.Insert == dylib && plan.Why.empty());
    // After what the variable names already, and only once.
    CHECK(PlanLaunch(dylib, std::span<uint8_t const>(library), "/a.dylib").Insert == "/a.dylib:" + dylib);
    CHECK(PlanLaunch(dylib, std::span<uint8_t const>(library), ("/a.dylib:" + dylib).c_str()).Insert == "/a.dylib:" + dylib);
    // dyld would end the game: a file that is not there, not a library, not Mach-O.
    CHECK(PlanLaunch(dylib, std::nullopt, nullptr).Insert.empty() && Contains(PlanLaunch(dylib, std::nullopt, nullptr).Why, "is not there"));
    CHECK(PlanLaunch(dylib, std::span<uint8_t const>(executable), nullptr).Insert.empty());
    Bytes text = { 'n', 'o', 't', ' ', 'a', ' ', 'l', 'i', 'b' };
    CHECK(PlanLaunch(dylib, std::span<uint8_t const>(text), nullptr).Insert.empty());
    CHECK(LaunchOption("/Users/a b/bg3mse/bg3mse-launch") == "\"/Users/a b/bg3mse/bg3mse-launch\" %command%");
    CHECK(LaunchedLibrary("/d", false) == "/d/libbg3mse.dylib" && LaunchedLibrary("/d", true) == "/d/libbg3mse-loader.dylib");
}

TEST(the_launcher_starts_the_game_with_and_without_the_dylib)
{
    // The launcher in a directory of its own, with a stand-in dylib beside it,
    // starts the stand-in game (signed ad hoc, as the game is) as Steam would.
    std::string dir = kWork + "/launcher with spaces";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string launcher = dir + "/" + kLauncher;
    WriteFile(launcher, ReadFile(BG3MSE_LAUNCHER));
    // A home of its own: the launcher writes its log there.
    std::string home = "HOME=" + dir + "/home";
    auto ran = RunProgram({ launcher, kFixture }, { home });
    CHECK(ran.Status == 0 && Contains(ran.Output, "starts without bg3mse") && Contains(ran.Output, "fixture build 1"));
    CHECK(!Contains(ran.Output, "stand-in libbg3mse.dylib loaded"));

    WriteFile(dir + "/" + kLibrary, ReadFile(kFixtures + "/standin/" + kLibrary));
    ran = RunProgram({ launcher, kFixture, "-arg" }, { home });
    CHECK(ran.Status == 0 && Contains(ran.Output, "stand-in libbg3mse.dylib loaded") && Contains(ran.Output, "fixture build 1"));
    CHECK(!Contains(ran.Output, "starts without"));

    // Steam on macOS names the application bundle, not its executable.
    {
        FakeGame g("launched", kFixture);
        ran = RunProgram({ launcher, g.App, "-arg" }, { home });
        CHECK(ran.Status == 0 && Contains(ran.Output, "stand-in libbg3mse.dylib loaded") && Contains(ran.Output, "fixture build 1"));
        ran = RunProgram({ launcher, dir }, { home });
        CHECK(ran.Status == 127 && Contains(ran.Output, "is not an application bundle with an executable"));
    }

    // bg3mse-install prints the option for that directory, and changes nothing.
    ran = RunProgram({ BG3MSE_INSTALL_TOOL, "launch-option", "--from", dir });
    CHECK(ran.Status == 0 && Contains(ran.Output, LaunchOption(launcher)));
    unlink((dir + "/" + kLibrary).c_str());
    ran = RunProgram({ BG3MSE_INSTALL_TOOL, "launch-option", "--from", dir });
    CHECK(ran.Status == 1 && Contains(ran.Output, "is not there"));
    fs::remove_all(dir);
}

}

// ---- The launch option's install --------------------------------------------

namespace {

// A home whose path has spaces and letters outside ASCII, as a user's name gives it.
std::string const kHomeName = "José Müller ✓";

// The camera module's default settings file, as the build makes it
// (bg3mse-camera defaults), or a stand-in for it in a build without the module.
std::string CameraDefaults()
{
#ifdef BG3MSE_CAMERA_DEFAULTS
    return ReadText(BG3MSE_CAMERA_DEFAULTS);
#else
    return "# The camera module's settings (a stand-in for its default file).\n"
           "\n"
           "[General]\n"
           "# Master switch: the camera hooks are installed only when\n"
           "# \"Enabled = true\" is in this section when the game starts.\n"
           "UnlockedPitchInitialValue = 25.0\n"
           "\n"
           "[Mouse]\n"
           "MousePitchMult = 0.25\n";
#endif
}

// A release folder as the release target lays it out: the two tools, the
// libraries (stand-ins), the camera module with its licence texts, the manifest.
std::string MakeReleaseFolder(std::string const& dir, bool loader = true, bool camera = true, std::string const& library = kLibrary)
{
    fs::remove_all(dir);
    fs::create_directories(dir + "/camera");
    std::string standin = kFixtures + "/standin/";
    WriteFile(dir + "/" + kLauncher, ReadFile(BG3MSE_LAUNCHER));
    WriteFile(dir + "/" + kInstallTool, ReadFile(BG3MSE_INSTALL_TOOL));
    WriteFile(dir + "/" + kLibrary, ReadFile(standin + library));
    if (loader) WriteFile(dir + "/" + kLoaderLibrary, ReadFile(standin + kLoaderLibrary));
    if (camera) {
        WriteFile(dir + "/camera/" + kCameraLibrary, ReadFile(standin + kCameraLibrary));
        for (auto licence : kCameraLicences) WriteText(dir + "/camera/" + licence, std::string("licence text ") + licence + "\n");
        WriteText(dir + "/camera/" + kCameraDefaultSettings, CameraDefaults());
    }
    WriteText(dir + "/" + kReleaseManifest, "{ \"name\": \"bg3mse-test\" }\n");
    return dir;
}

struct LaunchHome
{
    std::string Root, Home, Data, Bin, Previous;
    std::vector<std::string> Running;
    std::vector<std::string> Clipboard;
    bool ClipboardFails{ false };
    std::string MacOS{ "15.2" };
    std::function<void(Host&)> AdjustHost;

    explicit LaunchHome(std::string const& name)
    {
        Root = kWork + "/" + name;
        fs::remove_all(Root);
        Home = Root + "/Users/" + kHomeName;
        fs::create_directories(Home + "/Library/Application Support");
        Data = Home + "/Library/Application Support/bg3mse";
        Bin = Data + "/" + kBinDirectory;
        Previous = Data + "/" + kPreviousDirectory;
    }

    Result Do(Verb what, std::string const& from, std::function<void(Options&)> adjust = {})
    {
        FILE* out = tmpfile();
        Host host;
        host.AppleSilicon = true;
        host.Supports = [](std::span<uint8_t const>, std::span<uint8_t const>, std::string&) { return true; };
        host.DataDirectory = Data;
        host.Out = out;
        host.Clipboard = [this](std::string const& text, std::string& problem) {
            if (ClipboardFails) {
                problem = "no pasteboard";
                return false;
            }
            Clipboard.push_back(text);
            return true;
        };
        host.RunningGames = [this] { return Running; };
        host.DefaultGame = Root + "/no game here.app";
        host.MacOSVersion = MacOS;
        if (AdjustHost) AdjustHost(host);
        Options options;
        options.What = what;
        options.From = from;
        if (adjust) adjust(options);
        Result r;
        r.Status = Run(options, host);
        r.Output = Printed(out);
        return r;
    }

    std::string Line() const
    {
        char resolved[PATH_MAX];
        return LaunchOption(std::string(realpath(Data.c_str(), resolved) ? resolved : Data.c_str()) + "/bin/" + kLauncher);
    }
};

bool HasQuarantine(std::string const& path)
{
    return getxattr(path.c_str(), "com.apple.quarantine", nullptr, 0, 0, XATTR_NOFOLLOW) >= 0;
}

}

TEST(setup_puts_the_release_in_bin_and_the_launch_option_on_the_clipboard)
{
    LaunchHome h("setup");
    std::string release = MakeReleaseFolder(h.Root + "/bg3mse-0.1.0 release");
    // As a browser leaves a download.
    std::string mark = "0081;00000000;Safari;";
    CHECK(setxattr((release + "/" + kLibrary).c_str(), "com.apple.quarantine", mark.data(), mark.size(), 0, XATTR_NOFOLLOW) == 0);

    auto r = h.Do(Verb::Install, release, [](Options& o) { o.DryRun = true; });
    CHECK(r.Status == 0 && Contains(r.Output, "Dry run; nothing is changed") && Contains(r.Output, h.Line()));
    CHECK(!Exists(h.Data) && h.Clipboard.empty());

    r = h.Do(Verb::Install, release);
    if (r.Status != 0) printf("%s", r.Output.c_str());
    CHECK(r.Status == 0 && Contains(r.Output, "bg3mse is installed in " + h.Bin));
    CHECK(h.Clipboard.size() == 1 && h.Clipboard[0] == h.Line() && Contains(h.Clipboard[0], kHomeName));
    CHECK(h.Line() == "\"" + h.Bin + "/" + kLauncher + "\" %command%");
    CHECK(Contains(r.Output, "Copied.") && Contains(r.Output, "    " + h.Line()));
    CHECK(Contains(r.Output, "Launch Options under Properties > General in Steam, then press Play."));
    for (auto name : { kLauncher, kInstallTool, kLibrary, kLoaderLibrary, kReleaseManifest }) {
        CHECK(ReadFile(h.Bin + "/" + name) == ReadFile(release + "/" + name));
    }
    CHECK(ReadFile(h.Bin + "/" + kCameraLibrary) == ReadFile(release + "/camera/" + kCameraLibrary));
    for (auto licence : kCameraLicences) CHECK(ReadFile(h.Bin + "/camera/" + licence) == ReadFile(release + "/camera/" + licence));
    CHECK(ReadText(h.Bin + "/camera/" + kCameraDefaultSettings) == CameraDefaults());
    // No terminal and no flag: the extras are not chosen, and nothing is written for them.
    CHECK(!Exists(h.Data + "/" + kCameraSettingsFile) && Contains(r.Output, "off for now (there was no terminal to ask on)"));
    struct stat st{};
    CHECK(stat((h.Bin + "/" + kLauncher).c_str(), &st) == 0 && (st.st_mode & 0777) == 0755);
    CHECK(stat((h.Bin + "/" + kReleaseManifest).c_str(), &st) == 0 && (st.st_mode & 0777) == 0644);
    CHECK(!HasQuarantine(h.Bin + "/" + kLibrary));
    CHECK(ReadText(h.Data + "/" + kConfFile) == kDefaultConf && ReadText(h.Data + "/" + kExtenderSettingsFile) == kDefaultExtenderSettings);
    CHECK(!Exists(h.Previous));

    // Again: nothing replaced; the option is put on the clipboard again.
    ino_t library = Inode(h.Bin + "/" + kLibrary), bin = Inode(h.Bin);
    WriteText(h.Data + "/" + kConfFile, "script_extender = 0\n");
    r = h.Do(Verb::Install, release);
    CHECK(r.Status == 0 && Contains(r.Output, "Nothing to do, already installed.") && Contains(r.Output, "is already installed in"));
    CHECK(Inode(h.Bin + "/" + kLibrary) == library && Inode(h.Bin) == bin && h.Clipboard.size() == 2);
    CHECK(ReadText(h.Data + "/" + kConfFile) == "script_extender = 0\n");
    // Run from bin/ itself, as a player who installed it does.
    r = h.Do(Verb::Install, h.Bin);
    CHECK(r.Status == 0 && Contains(r.Output, "Nothing to do") && Inode(h.Bin) == bin);

    // A clipboard that cannot be used fails nothing: the option is printed.
    h.ClipboardFails = true;
    r = h.Do(Verb::Install, release);
    CHECK(r.Status == 0 && Contains(r.Output, "Couldn't copy it (no pasteboard). Copy it from above."));
    h.ClipboardFails = false;

    // A newer release, without the loader or the camera module: bin/ holds
    // what it has, the files it replaces stay in bin.previous, and a file of
    // the user's in bin/ stays.
    WriteText(h.Bin + "/my notes.txt", "mine\n");
    std::string newer = MakeReleaseFolder(h.Root + "/newer", false, false, kCameraLibrary);
    r = h.Do(Verb::Install, newer);
    CHECK(r.Status == 0 && Contains(r.Output, "leave out libbg3mse-loader.dylib, libbg3mse-camera.dylib"));
    CHECK(ReadFile(h.Bin + "/" + kLibrary) == ReadFile(newer + "/" + kLibrary));
    CHECK(!Exists(h.Bin + "/" + kLoaderLibrary) && !Exists(h.Bin + "/" + kCameraLibrary) && !Exists(h.Bin + "/camera/LICENSE"));
    CHECK(ReadText(h.Bin + "/my notes.txt") == "mine\n");
    CHECK(ReadFile(h.Previous + "/" + kLibrary) == ReadFile(release + "/" + kLibrary) && Exists(h.Previous + "/" + kLoaderLibrary));
    CHECK(Inode(h.Previous + "/" + kLibrary) == library);

    // A copy that fails leaves bin/ as it was.
    ino_t now = Inode(h.Bin + "/" + kLibrary);
    chmod(h.Data.c_str(), 0555);
    r = h.Do(Verb::Install, release);
    chmod(h.Data.c_str(), 0755);
    CHECK(r.Status == 1 && Contains(r.Output, h.Bin + " is as it was") && Inode(h.Bin + "/" + kLibrary) == now);
    CHECK(ReadFile(h.Bin + "/" + kLibrary) == ReadFile(newer + "/" + kLibrary));

    // Refused while the game runs, and from a folder that is not a release.
    h.Running = { "/Games/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3" };
    r = h.Do(Verb::Install, release);
    CHECK(r.Status == 1 && Contains(r.Output, "the game is running") && Inode(h.Bin + "/" + kLibrary) == now);
    r = h.Do(Verb::Uninstall, release);
    CHECK(r.Status == 1 && Contains(r.Output, "the game is running") && Exists(h.Bin + "/" + kLibrary));
    h.Running.clear();
    r = h.Do(Verb::Install, kFixtures + "/standin");
    CHECK(r.Status == 1 && Contains(r.Output, "bg3mse-launch is not in") && Inode(h.Bin + "/" + kLibrary) == now);
    std::string broken = MakeReleaseFolder(h.Root + "/broken");
    WriteText(broken + "/" + kLibrary, "not a library");
    r = h.Do(Verb::Install, broken);
    CHECK(r.Status == 1 && Contains(r.Output, "Nothing was changed") && Inode(h.Bin + "/" + kLibrary) == now);

    // Back to the full release; then what status says.
    CHECK(h.Do(Verb::Install, release).Status == 0);
    r = h.Do(Verb::Status, release);
    CHECK(r.Status == 0 && Contains(r.Output, "bg3mse's launch option install: " + h.Bin));
    CHECK(Contains(r.Output, std::string("  ") + kLauncher + ": bg3mse "));
    CHECK(Contains(r.Output, std::string("  ") + kLibrary + ": there, without bg3mse's version stamp"));
    CHECK(Contains(r.Output, "the launch option for Steam: " + h.Line()));
    CHECK(Contains(r.Output, "can't see Steam's settings") && Contains(r.Output, "Check Launch Options"));
    CHECK(Contains(r.Output, "last start through bg3mse-launch: none recorded"));
    CHECK(Contains(r.Output, "other files in it, not bg3mse-install's (left as they are): \"my notes.txt\""));
    CHECK(Contains(r.Output, "bg3mse is installed."));
    CHECK(Contains(r.Output, "No game was given"));

    // Uninstall: bin/ goes, the user's file and the settings stay; the option is to be cleared.
    r = h.Do(Verb::Uninstall, release, [](Options& o) { o.DryRun = true; });
    CHECK(r.Status == 0 && Contains(r.Output, "Dry run") && Exists(h.Bin + "/" + kLibrary));
    r = h.Do(Verb::Uninstall, release);
    CHECK(r.Status == 0 && Contains(r.Output, "bg3mse is uninstalled, settings kept.") && Contains(r.Output, "Remember to clear the game's launch options"));
    for (auto const& name : { std::string(kLauncher), std::string(kLibrary), std::string(kLoaderLibrary), std::string("camera/LICENSE") }) {
        CHECK(!Exists(h.Bin + "/" + name));
    }
    CHECK(!Exists(h.Previous) && ReadText(h.Bin + "/my notes.txt") == "mine\n" && Exists(h.Data + "/" + kConfFile));
    unlink((h.Bin + "/my notes.txt").c_str());
    fs::create_directories(h.Data + "/" + kUpdaterCacheDirectory);
    WriteText(h.Data + "/" + kUpdaterCacheDirectory + "/Manifest-Release.json", "the last manifest accepted\n");
    CHECK(Exists(h.Data + "/" + kUpdaterCacheDirectory + "/Manifest-Release.json"));
    r = h.Do(Verb::Uninstall, release, [](Options& o) { o.Purge = true; });
    CHECK(r.Status == 0 && !Exists(h.Bin) && !Exists(h.Data + "/" + kConfFile) && !Exists(h.Data + "/" + kExtenderSettingsFile)
        && !Exists(h.Data + "/" + kUpdaterCacheDirectory));
    r = h.Do(Verb::Uninstall, release);
    CHECK(r.Status == 0 && Contains(r.Output, "bg3mse isn't installed."));
    r = h.Do(Verb::Status, release);
    CHECK(Contains(r.Output, "bg3mse isn't installed."));
    CHECK(Contains(r.Output, "can't see Steam's settings") && Contains(r.Output, "until you clear the launch option or install bg3mse again"));
}

// What the installer does of the read the game would otherwise do when it starts (Host::ReadShaderInterface).
TEST(install_reads_the_games_metal_functions_when_mod_shaders_are_translated)
{
    LaunchHome h("shader-interface");
    std::string release = MakeReleaseFolder(h.Root + "/release");
    // A game where Steam puts it: known without being given.
    std::string app = h.Root + "/Steam/Baldur's Gate 3.app";
    fs::create_directories(app + "/Contents");
    WriteText(app + "/Contents/Info.plist", PlistOf("com.larian.bg3", "1.0.0"));
    // kept: there is nothing to read (what is kept is the game's, or no mod has shaders to translate).
    bool kept = true, fails = false;
    std::vector<std::pair<std::string, std::string>> asked, read;
    h.AdjustHost = [&](Host& host) {
        host.DefaultGame = app;
        host.ShaderInterfaceKept = [&](std::string const& game, std::string const& file) {
            asked.emplace_back(game, file);
            return kept;
        };
        host.ReadShaderInterface = [&](std::string const& game, std::string const& file, std::string& problem) {
            read.emplace_back(game, file);
            if (fails) problem = "no Metal device";
            else kept = true;
            return !fails;
        };
    };
    std::string file = h.Data + "/" + kShaderInterfaceFile;
    char const* reading = "Loading the game's shaders for mods that have their own";

    // Nothing to read: asked whether there is, and nothing read.
    auto r = h.Do(Verb::Install, release);
    CHECK(r.Status == 0 && asked.size() == 1 && asked[0].first == app && asked[0].second == file && read.empty() && !Contains(r.Output, reading));

    // Something to read, with the settings as the installer makes them, which have no key for it: mods'
    // shaders are translated. Read once, after everything else, and not again while there is nothing to read.
    kept = false;
    CHECK(!Contains(ReadText(h.Data + "/" + kConfFile), "shader_translation"));
    r = h.Do(Verb::Install, release, [](Options& o) { o.DryRun = true; });
    CHECK(r.Status == 0 && asked.size() == 1 && read.empty());
    r = h.Do(Verb::Install, release);
    CHECK(r.Status == 0 && read.size() == 1 && read[0].first == app && read[0].second == file);
    CHECK(Contains(r.Output, reading) && Contains(r.Output, "Done, in ") && r.Output.find(reading) > r.Output.find("then press Play."));
    r = h.Do(Verb::Install, release);
    CHECK(r.Status == 0 && read.size() == 1 && asked.size() == 3 && !Contains(r.Output, reading));

    // The game updated: what is kept is not of it any more, and install reads again. With the key on, and at
    // "all", the same.
    kept = false;
    WriteText(h.Data + "/" + kConfFile, "script_extender = 1\nshader_translation = 1\n");
    r = h.Do(Verb::Install, release);
    CHECK(r.Status == 0 && read.size() == 2 && Contains(r.Output, reading));
    kept = false;
    WriteText(h.Data + "/" + kConfFile, "shader_translation = all\n");
    r = h.Do(Verb::Install, release);
    CHECK(r.Status == 0 && read.size() == 3 && Contains(r.Output, reading));

    // On a terminal the optional features are asked about first: the read comes after the answers.
    kept = false;
    int answers[2] = { -1, -1 };
    CHECK(pipe(answers) == 0 && write(answers[1], "n\nn\n", 4) == 4);
    close(answers[1]);
    auto adjust = h.AdjustHost;
    h.AdjustHost = [&](Host& host) {
        adjust(host);
        host.Terminal = answers[0];
    };
    r = h.Do(Verb::Install, release);
    close(answers[0]);
    h.AdjustHost = adjust;
    size_t lastQuestion = r.Output.rfind("? [y/N] "), summary = r.Output.find("Optional features:");
    CHECK(r.Status == 0 && read.size() == 4 && Contains(r.Output, "Turn on Native Camera Tweaks? [y/N] "));
    CHECK(lastQuestion != std::string::npos && summary != std::string::npos && lastQuestion < summary && summary < r.Output.find(reading));

    // Not being able to read fails nothing: the game does it then.
    kept = false;
    fails = true;
    r = h.Do(Verb::Install, release);
    CHECK(r.Status == 0 && read.size() == 5 && Contains(r.Output, "Couldn't load them (no Metal device)") &&
          Contains(r.Output, "the game loads them when it starts"));
    fails = false;

    // Switched off, or no game known: not read.
    WriteText(h.Data + "/" + kConfFile, "shader_translation = 0\n");
    size_t askedBefore = asked.size();
    r = h.Do(Verb::Install, release);
    CHECK(r.Status == 0 && read.size() == 5 && asked.size() == askedBefore);
    WriteText(h.Data + "/" + kConfFile, "script_extender = 1\n");
    fs::remove_all(app);
    r = h.Do(Verb::Install, release);
    CHECK(r.Status == 0 && read.size() == 5 && asked.size() == askedBefore);

    // Uninstall reads nothing, and takes what was kept of shaders, and the builds the updater fetched, with it;
    // the settings stay.
    WriteText(file, "an interface\n");
    WriteText(file + ".4321.partial", "half an interface\n");
    fs::create_directories(h.Data + "/" + kShaderCacheDirectory);
    WriteText(h.Data + "/" + kShaderCacheDirectory + "/0123.translated", "an entry\n");
    std::string cache = h.Data + "/" + kUpdaterCacheDirectory;
    fs::create_directories(cache + "/Builds/0.2.0_00");
    WriteText(cache + "/Builds/0.2.0_00/libbg3mse.dylib", "a build\n");
    WriteText(cache + "/Failed-0.3.0_00", "");
    WriteText(cache + "/Manifest-Release.json", "the last manifest accepted\n");
    r = h.Do(Verb::Uninstall, release);
    CHECK(r.Status == 0 && read.size() == 5 && !Exists(file) && !Exists(file + ".4321.partial"));
    CHECK(!Exists(h.Data + "/" + kShaderCacheDirectory) && !Exists(cache + "/Builds") && !Exists(cache + "/Failed-0.3.0_00")
        && Exists(cache + "/Manifest-Release.json") && Exists(h.Data + "/" + kConfFile));

}

// --- the game's shaders, read before the game starts --------------------------------

// What the notice would show, and a player who presses its button after so many looks.
struct RecordedNotice : ReadNotice
{
    int Shown{ 0 }, Hidden{ 0 }, Looks{ 0 }, PressAfter{ -1 };

    void Show() override { Shown++; }
    bool StartNowPressed() override
    {
        usleep(20 * 1000);
        return PressAfter >= 0 && ++Looks > PressAfter;
    }
    void Hide() override { Hidden++; }
};

std::string ThisProgram()
{
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return {};
    char resolved[PATH_MAX];
    return realpath(path, resolved) ? resolved : path;
}

// bg3mse-install's stand-in, which this program is when it is given the verb (main): leaves what it was
// started with in the file BG3MSE_TEST_REPORT names, and ends as BG3MSE_TEST_READ says.
int ReadStandIn(int argc, char** argv)
{
    std::string how = getenv("BG3MSE_TEST_READ") ? getenv("BG3MSE_TEST_READ") : "";
    std::string report = "pid " + std::to_string(getpid()) + "\narguments";
    for (int i = 1; i < argc; i++) report += std::string(" ") + argv[i];
    report += "\n";
    for (char** e = environ; e && *e; e++) {
        if (strncmp(*e, "DYLD_", 5) == 0) report += std::string("environment ") + *e + "\n";
    }
    // A file the launcher had open is not this program's to have.
    if (fcntl(17, F_GETFD) != -1) report += "descriptor 17 is open\n";
    if (char const* to = getenv("BG3MSE_TEST_REPORT")) WriteText(to, report);
    if (how == "fail") return 1;
    if (how.starts_with("slow ")) usleep((useconds_t)(atof(how.c_str() + 5) * 1e6));
    if (how.starts_with("deaf ")) {
        // Does not end when asked to.
        signal(SIGTERM, SIG_IGN);
        for (double left = atof(how.c_str() + 5); left > 0; left -= 0.1) usleep(100 * 1000);
    }
    return 0;
}

TEST(the_games_shaders_are_read_before_the_game_starts)
{
    for (auto on : { "1", "true", "ON", "yes", "all", "ALL", "All" }) CHECK(TranslatesShaders(on));
    for (auto off : { "0", "", "no", "off", "always", "al", "alll" }) CHECK(!TranslatesShaders(off));

    std::string dir = kWork + "/shader-read", report = dir + "/report", tool = ThisProgram(), app = dir + "/Baldur's Gate 3.app";
    fs::remove_all(dir);
    fs::create_directories(dir);
    setenv("BG3MSE_TEST_REPORT", report.c_str(), 1);
    // As the launcher's environment may have it, and a file the launcher has open.
    setenv("DYLD_INSERT_LIBRARIES_OF_A_TEST", "/some/library.dylib", 1);
    int open17 = dup2(STDOUT_FILENO, 17);
    auto run = [&](char const* how, RecordedNotice& notice, double noticeAfter = 0.3, double timeout = 30) {
        setenv("BG3MSE_TEST_READ", how, 1);
        unlink(report.c_str());
        return ReadShadersBeforeLaunch(tool, app, &notice, noticeAfter, timeout);
    };
    auto pidOf = [&] {
        std::string text = ReadText(report);
        return text.size() > 4 ? atoi(text.c_str() + 4) : 0;
    };

    // What is kept is the game's: over at once, nothing shown, nothing to say.
    RecordedNotice quiet;
    auto read = run("", quiet);
    CHECK(read.How == ShaderRead::Ended::AtOnce && quiet.Shown == 0 && quiet.Hidden == 0 && Describe(read).empty());
    CHECK(Contains(ReadText(report), std::string("\narguments ") + kShaderInterfaceVerb + " " + app + "\n"));
    CHECK(!Contains(ReadText(report), "environment") && !Contains(ReadText(report), "descriptor"));

    // A read: the notice is shown while it lasts and taken away after, and the log says how long it took.
    RecordedNotice shown;
    read = run("slow 0.8", shown);
    CHECK(read.How == ShaderRead::Ended::Read && shown.Shown == 1 && shown.Hidden == 1 && read.Seconds >= 0.8);
    CHECK(Describe(read).starts_with("the game's Metal functions were read for mods' shaders in ") && Describe(read).ends_with(" seconds"));
    ShaderRead timed;
    timed.How = ShaderRead::Ended::Read;
    timed.Seconds = 83.6;
    CHECK(Describe(timed) == "the game's Metal functions were read for mods' shaders in 84 seconds");

    // It fails: the game starts all the same, and reads them itself.
    RecordedNotice failed;
    read = run("fail", failed);
    CHECK(read.How == ShaderRead::Ended::Failed && failed.Shown == 0);
    CHECK(Describe(read) == "the game's Metal functions were not read before the start (shader-interface ended with 1); the game reads them itself");

    // The player asks for the game at once: the read is stopped, and the wait is over.
    RecordedNotice pressed;
    pressed.PressAfter = 3;
    read = run("slow 60", pressed);
    CHECK(read.How == ShaderRead::Ended::StartedWithout && read.Seconds < 10 && pressed.Shown == 1 && pressed.Hidden == 1);
    CHECK(kill(pidOf(), 0) != 0 && Contains(Describe(read), "(the game was asked for at once)"));

    // One that does not end when asked to is ended: the game does not wait for it.
    RecordedNotice deaf;
    deaf.PressAfter = 3;
    read = run("deaf 60", deaf);
    CHECK(read.How == ShaderRead::Ended::StartedWithout && read.Seconds > 1.5 && read.Seconds < 10 && deaf.Hidden == 1 && kill(pidOf(), 0) != 0);

    // One that never ends is stopped too.
    RecordedNotice late;
    read = run("slow 60", late, 0.3, 1);
    CHECK(read.How == ShaderRead::Ended::TimedOut && read.Seconds < 10 && late.Hidden == 1 && kill(pidOf(), 0) != 0);
    CHECK(Contains(Describe(read), "(not done after 1 seconds)"));

    // No tool beside the launcher: nothing is waited for.
    RecordedNotice none;
    read = ReadShadersBeforeLaunch(dir + "/bg3mse-install", app, &none);
    CHECK(read.How == ShaderRead::Ended::NotStarted && Contains(Describe(read), "bg3mse-install is not there"));
    if (open17 == 17) close(17);
    unsetenv("DYLD_INSERT_LIBRARIES_OF_A_TEST");
    unsetenv("BG3MSE_TEST_READ");
    unsetenv("BG3MSE_TEST_REPORT");

    // The launcher itself: unless the translation of mods' shaders is switched off it has the tool beside it
    // read for the game it is given, before the game starts; switched off, or when it starts the game
    // without bg3mse, it does not.
    LaunchHome h("shader-read-launch");
    std::string release = MakeReleaseFolder(h.Root + "/release");
    CHECK(h.Do(Verb::Install, release).Status == 0);
    std::string launcher = h.Bin + "/" + kLauncher, log = h.Data + "/logs/" + kLaunchLog, home = "HOME=" + h.Home, asked = h.Home + "/asked";
    auto standIn = [&](int status) {
        WriteText(h.Bin + "/" + kInstallTool, "#!/bin/sh\necho \"$@\" >> \"$HOME/asked\"\nexit " + std::to_string(status) + "\n");
        chmod((h.Bin + "/" + kInstallTool).c_str(), 0755);
    };
    standIn(0);
    FakeGame g("shader-read-game", kFixture);
    // Switched off: the tool is not run.
    WriteText(h.Data + "/" + kConfFile, "script_extender = 1\nshader_translation = 0\n");
    auto ran = RunProgram({ launcher, g.App }, { home, "DYLD_INSERT_LIBRARIES=" });
    CHECK(ran.Status == 0 && Contains(ran.Output, "fixture build 1") && !Exists(asked));

    // With the settings as the installer makes them, which have no key for it, and with the key on: the tool
    // is run, and finds out whether there is anything to read.
    for (auto settings : { "script_extender = 1\n", "script_extender = 1\nshader_translation = 1\n" }) {
        fs::remove(asked);
        WriteText(h.Data + "/" + kConfFile, settings);
        ran = RunProgram({ launcher, g.App }, { home, "DYLD_INSERT_LIBRARIES=" });
        CHECK(ran.Status == 0 && Contains(ran.Output, "stand-in libbg3mse.dylib loaded") && Contains(ran.Output, "fixture build 1"));
        CHECK(ReadText(asked) == std::string(kShaderInterfaceVerb) + " " + g.App + "\n");
        CHECK(LastLaunch(log).size() == 1);
    }

    standIn(3);
    ran = RunProgram({ launcher, g.Executable }, { home, "DYLD_INSERT_LIBRARIES=" });
    CHECK(ran.Status == 0 && Contains(ran.Output, "fixture build 1"));
    auto last = LastLaunch(log);
    CHECK(last.size() == 2 && Contains(last[1], "the game's Metal functions were not read before the start (shader-interface ended with 3); "
        "the game reads them itself") && LaunchedGame(last) == g.Executable);

    // A game that starts without bg3mse has nothing read for it.
    fs::remove(asked);
    fs::rename(h.Bin + "/" + kLibrary, h.Root + "/aside");
    fs::rename(h.Bin + "/" + kLoaderLibrary, h.Root + "/aside-loader");
    ran = RunProgram({ launcher, g.App }, { home });
    CHECK(ran.Status == 0 && Contains(ran.Output, "fixture build 1") && !Exists(asked));
}

TEST(the_launcher_says_in_its_log_what_it_did)
{
    LaunchHome h("launch-log");
    std::string release = MakeReleaseFolder(h.Root + "/release");
    CHECK(h.Do(Verb::Install, release).Status == 0);
    std::string launcher = h.Bin + "/" + kLauncher, log = h.Data + "/logs/" + kLaunchLog, home = "HOME=" + h.Home;
    FakeGame g("launch-log-game", kFixture);

    // Through the bundle, as Steam gives it: the loader beside the launcher is inserted.
    auto ran = RunProgram({ launcher, g.App, "-arg" }, { home, "DYLD_INSERT_LIBRARIES=" });
    CHECK(ran.Status == 0 && Contains(ran.Output, "stand-in libbg3mse-loader.dylib loaded\nstand-in libbg3mse.dylib loaded"));
    CHECK(Contains(ran.Output, "fixture build 1"));
    auto last = LastLaunch(log);
    CHECK(last.size() == 1 && Contains(last[0], "] given \"" + g.App + "\" and 1 more argument; game \"" + g.Executable + "\"; inserted \"" +
        h.Bin + "/" + kLoaderLibrary + "\""));
    CHECK(LaunchedGame(last) == g.Executable);

    // What status shows of it, and of the game it started.
    auto r = h.Do(Verb::Status, release);
    CHECK(Contains(r.Output, "last start through bg3mse-launch (" + log + "):") && Contains(r.Output, last[0]));
    CHECK(Contains(r.Output, "Baldur's Gate 3: " + g.App) && Contains(r.Output, "  version 1.0.0"));

    // Without the libraries the game starts, without bg3mse, and the log says why.
    fs::rename(h.Bin + "/" + kLibrary, h.Root + "/aside");
    fs::rename(h.Bin + "/" + kLoaderLibrary, h.Root + "/aside-loader");
    ran = RunProgram({ launcher, g.App }, { home });
    CHECK(ran.Status == 0 && Contains(ran.Output, "fixture build 1") && !Contains(ran.Output, "stand-in"));
    last = LastLaunch(log);
    CHECK(last.size() == 1 && Contains(last[0], "not inserted: " + h.Bin + "/" + kLibrary + " is not there; the game starts without bg3mse"));
    fs::rename(h.Root + "/aside", h.Bin + "/" + kLibrary);
    fs::rename(h.Root + "/aside-loader", h.Bin + "/" + kLoaderLibrary);

    // What cannot be started: Steam shows nothing, the log says it.
    ran = RunProgram({ launcher, h.Root }, { home });
    CHECK(ran.Status == 127);
    last = LastLaunch(log);
    CHECK(last.size() == 2 && Contains(last[1], "cannot start the game: " + h.Root + " is not an application bundle with an executable"));
    ran = RunProgram({ launcher }, { home });
    CHECK(ran.Status == 2);
    last = LastLaunch(log);
    CHECK(last.size() == 1 && Contains(last[0], "given nothing to start: the launch option has no %command%"));
    ran = RunProgram({ launcher, h.Root + "/no such game" }, { home });
    CHECK(ran.Status == 127 && Contains(LastLaunch(log).back(), "cannot start the game: " + h.Root + "/no such game: No such file or directory"));

    // Kept small: a full log becomes launcher.log.1.
    std::string filler(1000, 'x');
    for (size_t i = 0; i < kLaunchLogLimit / 1000 + 2; i++) AppendLaunchLog(log, "filler " + filler);
    ran = RunProgram({ launcher, g.App }, { home });
    struct stat st{};
    CHECK(stat(log.c_str(), &st) == 0 && (size_t)st.st_size < kLaunchLogLimit && Exists(log + ".1"));
    CHECK(stat((log + ".1").c_str(), &st) == 0 && (size_t)st.st_size < kLaunchLogLimit + 2000);
    CHECK(Contains(LastLaunch(log).front(), "inserted"));

    // No data directory: no log, and the game still starts.
    LaunchHome bare("launch-log-bare");
    fs::create_directories(bare.Root + "/bin");
    WriteFile(bare.Root + "/bin/" + kLauncher, ReadFile(BG3MSE_LAUNCHER));
    ran = RunProgram({ bare.Root + "/bin/" + kLauncher, g.App }, { "HOME=" + bare.Home });
    CHECK(ran.Status == 0 && Contains(ran.Output, "fixture build 1") && !Exists(bare.Data));
}

TEST(the_tool_installs_for_the_launch_option)
{
    LaunchHome h("tool-launch");
    std::string release = MakeReleaseFolder(h.Root + "/release");
    std::string home = "HOME=" + h.Home;
    // The changes are tried as a dry run: a game the user is playing on this Mac refuses a real one.
    auto ran = RunProgram({ release + "/" + kInstallTool, "install", "--dry-run" }, { home });
    CHECK(ran.Status == 0 && Contains(ran.Output, "copied from " + release) && Contains(ran.Output, h.Line()) && !Exists(h.Data));
    ran = RunProgram({ release + "/" + kInstallTool, "status" }, { home });
    CHECK(ran.Status == 0 && Contains(ran.Output, "bg3mse isn't installed."));
    ran = RunProgram({ release + "/" + kInstallTool, "--version" });
    CHECK(ran.Status == 0 && Contains(ran.Output, "bg3mse-install "));
    for (auto args : std::vector<std::vector<std::string>>{ { "repair" }, { "uninstall", h.Root },
             { "status", "--dry-run" }, { "uninstall", "--from", release } }) {
        args.insert(args.begin(), release + "/" + kInstallTool);
        CHECK(RunProgram(args, { home }).Status == 2);
    }
    // The clipboard by a program, as pbcopy is given the text: here one that writes it to a file.
    std::string program = h.Root + "/copy", copied = h.Root + "/copied";
    WriteText(program, "#!/bin/sh\ncat > '" + copied + "'\n");
    chmod(program.c_str(), 0755);
    std::string problem;
    CHECK(CopyToClipboard(program, h.Line(), problem) && ReadText(copied) == h.Line());
    WriteText(program, "#!/bin/sh\necho cannot >&2\nexit 3\n");
    chmod(program.c_str(), 0755);
    CHECK(!CopyToClipboard(program, h.Line(), problem) && Contains(problem, "failed: cannot"));
}

// ---- The optional extras (Installer/Extras.h) -------------------------------------

namespace {

// What the camera module reads from a settings file, by bg3mse-camera
// settings (its own parser); true without the module in the build.
bool CameraReadsAs(std::string const& path, std::string const& expected)
{
#ifdef BG3MSE_CAMERA_TOOL
    std::string got = RunProgram({ BG3MSE_CAMERA_TOOL, "settings", path }).Output;
    if (got != expected) printf("  the camera module reads %s as:\n%s", path.c_str(), got.c_str());
    return got == expected;
#else
    (void)path;
    (void)expected;
    return true;
#endif
}

bool CameraTextReadsAs(std::string const& text, std::string const& expected)
{
    std::string path = kWork + "/camera-read.toml";
    fs::create_directories(kWork);
    WriteText(path, text);
    return CameraReadsAs(path, expected);
}

// The camera's default file with "Enabled = <value>" where bg3mse-install
// puts it: under [General], after the comments that open it.
std::string WithEnabled(std::string text, std::string const& value)
{
    size_t at = text.find("[General]\n");
    if (at == std::string::npos) return {};
    at += strlen("[General]\n");
    while (at < text.size() && text[at] == '#') at = text.find('\n', at) + 1;
    text.insert(at, "Enabled = " + value + "\n");
    return text;
}

std::string Self()
{
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return {};
    char resolved[PATH_MAX];
    return realpath(path, resolved) ? resolved : path;
}

// A program run as a player runs it: on a terminal of its own, or with none.
struct Session
{
    int Status{ -1 };
    std::string Output;       // as shown, without the terminal's \r
    size_t Questions{ 0 };    // questions shown
    bool AskedMore{ false };  // a question shown after the answers ran out (it was given the end of the input)
    bool Hung{ false };       // still running after a minute: stopped
};

size_t CountQuestions(std::string const& text)
{
    size_t n = 0;
    for (char const* q : { "[y/N] ", "[Y/n] " }) {
        for (size_t at = text.find(q); at != std::string::npos; at = text.find(q, at + 1)) n++;
    }
    return n;
}

// Runs a program on a pseudo-terminal that is its controlling terminal, its
// standard input and output, typing each answer once a question is shown
// ("\x04" is the end of the input); or, with `terminal` false, with
// /dev/null for its input, in a session of its own (no terminal at all).
Session Converse(std::vector<std::string> const& args, std::vector<std::string> const& answers, bool terminal = true,
    std::vector<std::string> const& extraEnvironment = {})
{
    Session session;
    std::vector<std::string> environment;
    for (char** e = environ; *e; e++) {
        std::string entry = *e;
        bool replaced = false;
        for (auto const& extra : extraEnvironment) replaced = replaced || entry.starts_with(extra.substr(0, extra.find('=') + 1));
        if (!replaced) environment.push_back(entry);
    }
    for (auto const& extra : extraEnvironment) environment.push_back(extra);
    std::vector<char*> argv, envp;
    for (auto const& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    for (auto& e : environment) envp.push_back(e.data());
    envp.push_back(nullptr);

    int fd = -1;
    pid_t pid = -1;
    if (terminal) {
        winsize size{};
        size.ws_row = 40;
        size.ws_col = 200;
        pid = forkpty(&fd, nullptr, nullptr, &size);
        if (pid == 0) {
            execve(argv[0], argv.data(), envp.data());
            _exit(127);
        }
    } else {
        int out[2];
        if (pipe(out) != 0) return session;
        pid = fork();
        if (pid == 0) {
            setsid();
            int null = open("/dev/null", O_RDONLY);
            dup2(null, STDIN_FILENO);
            dup2(out[1], STDOUT_FILENO);
            dup2(out[1], STDERR_FILENO);
            close(out[0]);
            execve(argv[0], argv.data(), envp.data());
            _exit(127);
        }
        close(out[1]);
        fd = out[0];
    }
    if (pid < 0) return session;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    size_t sent = 0;
    bool open = true;
    while (open) {
        if (std::chrono::steady_clock::now() > deadline) {
            session.Hung = true;
            break;
        }
        pollfd p{ fd, POLLIN, 0 };
        if (poll(&p, 1, 100) <= 0) continue;
        char buffer[4096];
        ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            open = false;
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buffer[i] != '\r') session.Output += buffer[i];
        }
        if (!terminal) continue;
        for (size_t shown = CountQuestions(session.Output); sent < shown; sent++) {
            std::string answer = sent < answers.size() ? answers[sent] : "\x04";
            if (sent >= answers.size()) session.AskedMore = true;
            if (write(fd, answer.data(), answer.size()) != (ssize_t)answer.size()) open = false;
        }
    }
    int status = 0;
    bool killed = false;
    for (;;) {
        if (session.Hung && !killed) {
            kill(pid, SIGKILL);
            killed = true;
        }
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0) {
            status = -1;
            break;
        }
        if (std::chrono::steady_clock::now() > deadline + std::chrono::seconds(5)) session.Hung = true;
        usleep(10000);
    }
    close(fd);
    session.Questions = CountQuestions(session.Output);
    session.Status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (session.Hung) printf("  %s did not end; its output:\n%s\n", args[0].c_str(), session.Output.c_str());
    return session;
}

// A home with a release beside it; bg3mse-install's verbs run on it by this
// program in a child (extras-child below), with a stand-in host: no game is
// looked for, the clipboard is not used, and the macOS version is given.
struct ExtrasHome
{
    std::string Root, Home, Data, Bin, Release, Conf, Camera;

    explicit ExtrasHome(std::string const& name, bool camera = true)
    {
        Root = kWork + "/" + name;
        fs::remove_all(Root);
        Home = Root + "/Users/" + kHomeName;
        fs::create_directories(Home + "/Library/Application Support");
        Data = Home + "/Library/Application Support/bg3mse";
        Bin = Data + "/" + kBinDirectory;
        Conf = Data + "/" + kConfFile;
        Camera = Data + "/" + kCameraSettingsFile;
        Release = MakeReleaseFolder(Root + "/release", true, camera);
    }

    Session Run(std::string const& verb, std::vector<std::string> const& answers, std::vector<std::string> const& flags = {},
        bool terminal = true, std::string const& macos = "15.2")
    {
        std::vector<std::string> args = { Self(), "extras-child", verb, Data, Release, macos };
        args.insert(args.end(), flags.begin(), flags.end());
        auto session = Converse(args, answers, terminal);
        if (session.Status != 0 && session.Status != 1) printf("  %s: %s\n", verb.c_str(), session.Output.c_str());
        return session;
    }

    // The real bg3mse-install, as installed in bin/.
    Session Tool(std::vector<std::string> args, std::vector<std::string> const& answers = {}, bool terminal = false)
    {
        args.insert(args.begin(), Bin + "/" + kInstallTool);
        return Converse(args, answers, terminal, { "HOME=" + Home });
    }

    std::string ConfValue() const
    {
        auto value = bg3se::mac::ConfLookup(ReadText(Conf), "gfx_temporal");
        return value ? *value : "(none)";
    }
};

}

// bg3mse_installer_tests extras-child <verb> <data directory> <release> <macOS version, or -> [flags]:
// a verb run as bg3mse-install runs it, on a stand-in host.
int ExtrasChild(std::vector<std::string> const& a)
{
    if (a.size() < 4) return 2;
    Options options;
    if (a[0] == "install") options.What = Verb::Install;
    else if (a[0] == "options") options.What = Verb::Options;
    else if (a[0] == "status") options.What = Verb::Status;
    else if (a[0] == "uninstall") options.What = Verb::Uninstall;
    else return 2;
    options.From = a[2];
    bool running = false;
    for (size_t i = 4; i < a.size(); i++) {
        if (a[i] == "--running") running = true;
        else if (a[i] == "--camera=on") options.Camera = true;
        else if (a[i] == "--camera=off") options.Camera = false;
        else if (a[i] == "--upscaling=on") options.Upscaling = true;
        else if (a[i] == "--upscaling=off") options.Upscaling = false;
        else if (a[i] == "--defaults") options.Defaults = true;
        else if (a[i] == "--dry-run") options.DryRun = true;
        else if (a[i] == "--purge") options.Purge = true;
        else return 2;
    }
    Host host;
    host.AppleSilicon = true;
    host.Supports = [](std::span<uint8_t const>, std::span<uint8_t const>, std::string&) { return true; };
    host.DataDirectory = a[1];
    host.Out = stdout;
    host.MacOSVersion = a[3] == "-" ? "" : a[3];
    host.RunningGames = [running] {
        return running ? std::vector<std::string>{ "/Games/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3" } : std::vector<std::string>{};
    };
    host.DefaultGame = a[1] + "/no game here.app";
    // As bg3mse-install finds a terminal to ask on.
    if (isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && tcgetpgrp(STDIN_FILENO) == getpgrp()) host.Terminal = STDIN_FILENO;
    int status = Run(options, host);
    fflush(stdout);
    return status;
}

TEST(extras_files_are_changed_in_the_one_key)
{
    std::string problem;
    // bg3mse.conf as a player keeps it: CRLF, comments of both kinds, keys
    // of other things, the key twice in two cases, no newline at the end.
    std::string conf = "# mine\r\n"
                       "; an old comment\r\n"
                       "\r\n"
                       "script_extender = 0\r\n"
                       "my_own = a # not a comment here\r\n"
                       "GFX_TEMPORAL = 0\r\n"
                       "  gfx_temporal=yes\r\n"
                       "log_debug=1";
    auto found = FindConfKey(conf, "gfx_temporal");
    CHECK(found.Problem.empty() && found.Present && found.Value == "yes" && found.Line == 7);
    auto set = SetConfKey(conf, "gfx_temporal", "0", problem);
    CHECK(set && *set == "# mine\r\n; an old comment\r\n\r\nscript_extender = 0\r\nmy_own = a # not a comment here\r\nGFX_TEMPORAL = 0\r\n"
                         "  gfx_temporal=0\r\nlog_debug=1");
    CHECK(set && bg3se::mac::ConfLookup(*set, "gfx_temporal") == "0" && bg3se::mac::ConfLookup(*set, "script_extender") == "0");
    set = SetConfKey("script_extender = 1", "gfx_temporal", "1", problem);
    CHECK(set && *set == "script_extender = 1\ngfx_temporal = 1\n" && bg3se::mac::ConfLookup(*set, "gfx_temporal") == "1");
    set = SetConfKey("a = 1\r\n", "gfx_temporal", "1", problem);
    CHECK(set && *set == "a = 1\r\ngfx_temporal = 1\r\n");
    set = SetConfKey("", "gfx_temporal", "1", problem);
    CHECK(set && *set == "gfx_temporal = 1\n");
    set = SetConfKey("gfx_temporal =\n", "gfx_temporal", "1", problem);
    CHECK(set && *set == "gfx_temporal = 1\n");

    // The camera's file: the value alone changes; the comment after it, a
    // quoted #, other sections and keys it does not know stay.
    std::string camera = "# my camera\n"
                         "[General]\n"
                         "# a note\n"
                         "Enabled = false   # I chose this\n"
                         "Unknown = \"a # b\"\n"
                         "[Mouse]\n"
                         "Enabled = true\n"
                         "MousePitchMult = 0.5\n";
    found = FindTomlKey(camera, "General", "Enabled");
    CHECK(found.Problem.empty() && found.Present && found.Value == "false" && found.Line == 4);
    set = SetTomlKey(camera, "General", "Enabled", "true", problem);
    std::string expected = camera;
    expected.replace(expected.find("Enabled = false"), strlen("Enabled = false"), "Enabled = true");
    CHECK(set && *set == expected);
    CHECK(set && CameraTextReadsAs(*set, "Enabled: true\nignored: 2 (first at line 5)\n"));
    // Not there: under [General], after its opening comments; in [Mouse] it is another key.
    std::string unset = "[General]\n# one\n# two\nA = 1\n[Mouse]\nEnabled = true\n";
    set = SetTomlKey(unset, "General", "Enabled", "false", problem);
    CHECK(set && *set == "[General]\n# one\n# two\nEnabled = false\nA = 1\n[Mouse]\nEnabled = true\n");
    set = SetTomlKey("[Mouse]\r\nX = 1", "General", "Enabled", "true", problem);
    CHECK(set && *set == "[Mouse]\r\nX = 1\r\n\r\n[General]\r\nEnabled = true\r\n");
    set = SetTomlKey("[General]\n# only a comment", "General", "Enabled", "true", problem);
    CHECK(set && *set == "[General]\n# only a comment\nEnabled = true\n");
    // Twice: the last one is what the module reads, and what is changed.
    set = SetTomlKey("[General]\nEnabled = true\n[General]\nEnabled = true\n", "General", "Enabled", "false", problem);
    CHECK(set && *set == "[General]\nEnabled = true\n[General]\nEnabled = false\n");
    CHECK(set && CameraTextReadsAs(*set, "Enabled: false\nignored: 0\n"));
    // The module's own default file holds no choice, and takes one where it says.
    std::string defaults = CameraDefaults();
    CHECK(!FindTomlKey(defaults, "General", "Enabled").Present && FindTomlKey(defaults, "General", "Enabled").Problem.empty());
    set = SetTomlKey(defaults, "General", "Enabled", "true", problem);
    CHECK(set && *set == WithEnabled(defaults, "true"));
    CHECK(set && CameraTextReadsAs(*set, "Enabled: true\nignored: 0\n"));
    CHECK(CameraTextReadsAs(defaults, "Enabled: false\nignored: 0\n"));

    // What its reader does not read as a whole is not changed.
    struct Bad
    {
        bool Toml;
        std::string Text, Said;
    };
    std::string longLine = "# " + std::string(600, 'x') + "\n";
    std::string big = "a = 1\n";
    while (big.size() <= 64 * 1024) big += "# a comment line\n";
    std::vector<Bad> bad = {
        { false, "a = 1\nthis is not a setting\n", "line 2 is not a comment or a \"key = value\" line, and bg3mse skips it" },
        { false, "= 1\n", "line 1 has no key before its =" },
        { false, "a = 1\n" + longLine, "line 2 is 512 characters or more, and bg3mse skips it" },
        { false, std::string("a = 1\n\0b = 2\n", 13), "it has a NUL byte (at byte 6), where bg3mse stops reading" },
        { false, big, "it is larger than 64 KB, and bg3mse reads only the first 64 KB" },
        { true, "[General\nEnabled = true\n", "line 1 is not a [section] line the camera module reads" },
        { true, "[General] x\n", "line 1 is not a [section] line the camera module reads" },
        { true, "[General]\nEnabled true\n", "line 2 is not a comment, a [section] or a \"key = value\" line" },
        { true, "[General]\n" + longLine, "line 2 is 512 characters or more, and the camera module skips it" },
        { true, std::string("[General]\n\0", 11), "it has a NUL byte (at byte 10), where the camera module stops reading" },
    };
    for (auto const& b : bad) {
        auto key = b.Toml ? FindTomlKey(b.Text, "General", "Enabled") : FindConfKey(b.Text, "gfx_temporal");
        auto made = b.Toml ? SetTomlKey(b.Text, "General", "Enabled", "true", problem) : SetConfKey(b.Text, "gfx_temporal", "1", problem);
        CHECK(Contains(key.Problem, b.Said) && !made && problem == key.Problem);
        if (!Contains(key.Problem, b.Said)) printf("  said: %s\n", key.Problem.c_str());
    }
}

TEST(extras_are_asked_about_on_a_terminal)
{
    std::string const camera = "Turn on Native Camera Tweaks? [y/N] ", upscaling = "Turn on MetalFX Temporal? [y/N] ";
    {
        // No to both: both written, so that they are not asked again.
        ExtrasHome h("extras-no");
        auto r = h.Run("install", { "n\n", "n\n" });
        CHECK(r.Status == 0 && !r.Hung && r.Questions == 2 && !r.AskedMore);
        // The whole exchange (the terminal shows each answer as it is typed).
        CHECK(Contains(r.Output, "\nOptional features (change later with \"bg3mse-install options\"):\n\n" + camera + "n\n" + upscaling + "n\n\n"));
        CHECK(ReadText(h.Camera) == WithEnabled(CameraDefaults(), "false"));
        CHECK(ReadText(h.Conf) == std::string(kDefaultConf) + "gfx_temporal = 0\n");
        CHECK(CameraReadsAs(h.Camera, "Enabled: false\nignored: 0\n"));
        CHECK(Contains(r.Output, "  create " + h.Camera + " from " + h.Release + "/camera/" + kCameraDefaultSettings +
            ", with Enabled = false under [General]"));
        CHECK(Contains(r.Output, "  add gfx_temporal = 0 to " + h.Conf));
        CHECK(Contains(r.Output, "Native Camera Tweaks: off (Enabled = false under [General] in " + h.Camera + ", line "));
        CHECK(Contains(r.Output, "MetalFX Temporal: off (gfx_temporal = 0 in " + h.Conf + ", line 5)"));
        CHECK(Contains(r.Output, "Change them with \"" + h.Bin + "/bg3mse-install\" options."));
        // Again, as an update does: nothing is asked, and nothing changes.
        Bytes before = ReadFile(h.Camera), conf = ReadFile(h.Conf);
        r = h.Run("install", {});
        CHECK(r.Status == 0 && r.Questions == 0 && Contains(r.Output, "Nothing to do"));
        CHECK(ReadFile(h.Camera) == before && ReadFile(h.Conf) == conf);
    }
    {
        // Yes to both, as y and as yes.
        ExtrasHome h("extras-yes");
        auto r = h.Run("install", { "y\n", "YES\n" });
        CHECK(r.Status == 0 && r.Questions == 2 && ReadText(h.Camera) == WithEnabled(CameraDefaults(), "true") && h.ConfValue() == "1");
        CHECK(CameraReadsAs(h.Camera, "Enabled: true\nignored: 0\n"));
        CHECK(Contains(r.Output, "Native Camera Tweaks: on (Enabled = true under [General]"));
        CHECK(Contains(r.Output, "MetalFX Temporal: on (gfx_temporal = 1 in " + h.Conf + ", line 5); turn on FSR in game"));
        r = h.Run("install", {});
        CHECK(r.Status == 0 && r.Questions == 0 && ReadText(h.Camera) == WithEnabled(CameraDefaults(), "true") && h.ConfValue() == "1");
    }
    {
        // Return alone: the default, written.
        ExtrasHome h("extras-return");
        auto r = h.Run("install", { "\n", "\n" });
        CHECK(r.Status == 0 && r.Questions == 2 && ReadText(h.Camera) == WithEnabled(CameraDefaults(), "false") && h.ConfValue() == "0");
    }
    {
        // A wrong answer, then a right one; three wrong answers are no answer.
        ExtrasHome h("extras-wrong");
        auto r = h.Run("install", { "maybe\n", "y\n", "\n" });
        CHECK(r.Status == 0 && r.Questions == 3 && Contains(r.Output, "Type y or n (Return means n).\n" + camera));
        CHECK(ReadText(h.Camera) == WithEnabled(CameraDefaults(), "true") && h.ConfValue() == "0");
        ExtrasHome w("extras-wrong-thrice");
        r = w.Run("install", { "a\n", "b\n", "c\n", "y\n" });
        CHECK(r.Status == 0 && r.Questions == 4 && !r.AskedMore && Contains(r.Output, "No answer, leaving it as it is."));
        CHECK(!Exists(w.Camera) && w.ConfValue() == "1");
        CHECK(Contains(r.Output, "Native Camera Tweaks is off for now (no answer). Turn it on with"));
    }
    {
        // The end of the input in the first question: nothing more is asked or written; bg3mse is installed.
        ExtrasHome h("extras-end");
        auto r = h.Run("install", { "\x04" });
        CHECK(r.Status == 0 && !r.Hung && r.Questions == 1 && !r.AskedMore && Contains(r.Output, "(Input ended, skipping the rest.)\n\n  "));
        CHECK(!Exists(h.Camera) && h.ConfValue() == "(none)" && Exists(h.Bin + "/" + kLibrary));
        CHECK(Contains(r.Output, "Native Camera Tweaks and MetalFX Temporal are off for now (the input ended). Turn them on with \"" + h.Bin +
            "/bg3mse-install\" options."));
        // Typed without Return, then the end: no answer either. The next install asks again.
        r = h.Run("install", { "y\n", "y\x04\x04" });
        CHECK(r.Status == 0 && r.Questions == 2 && ReadText(h.Camera) == WithEnabled(CameraDefaults(), "true") && h.ConfValue() == "(none)");
        r = h.Run("install", { "y\n" });
        CHECK(r.Status == 0 && r.Questions == 1 && Contains(r.Output, upscaling) && !Contains(r.Output, camera) && h.ConfValue() == "1");
    }
    {
        // macOS before 13: the upscaling is not offered, and why is said.
        ExtrasHome h("extras-old-macos");
        auto r = h.Run("install", { "\n" }, {}, true, "12.7.4");
        CHECK(r.Status == 0 && r.Questions == 1 && Contains(r.Output, camera) && !Contains(r.Output, upscaling));
        CHECK(Contains(r.Output, "MetalFX Temporal is not offered: requires macOS 13 or later (this Mac has 12.7.4).\n"));
        CHECK(Contains(r.Output, "Optional features (change later with \"bg3mse-install options\"):"));
        CHECK(h.ConfValue() == "(none)" && ReadText(h.Camera) == WithEnabled(CameraDefaults(), "false"));
        CHECK(Contains(r.Output, "MetalFX Temporal: unavailable: requires macOS 13 or later (this Mac has 12.7.4)"));
        // A flag that cannot be applied: bg3mse is installed, and the install fails, saying so.
        r = h.Run("install", {}, { "--upscaling=on" }, true, "12.7.4");
        CHECK(r.Status == kInstalledWithoutExtras && r.Questions == 0 && h.ConfValue() == "(none)" &&
            Contains(r.Output, "MetalFX Temporal is not turned on: requires macOS 13 or later (this Mac has 12.7.4).\n"));
        CHECK(r.Output.ends_with("bg3mse itself is installed; only MetalFX Temporal was not set, as said above.\n"));
        // Off where it cannot be used is no failure.
        r = h.Run("install", {}, { "--upscaling=off" }, true, "12.7.4");
        CHECK(r.Status == 0 && h.ConfValue() == "(none)" && !Contains(r.Output, "not set"));
        r = h.Run("install", {}, {}, true, "-");
        CHECK(Contains(r.Output, "MetalFX Temporal is not offered: requires macOS 13 or later (can't read this Mac's version)."));
        r = h.Run("install", { "\n" }, {}, true, "13.0");
        CHECK(r.Questions == 1 && Contains(r.Output, upscaling) && h.ConfValue() == "0");
    }
    {
        // A refusal comes before any question.
        ExtrasHome h("extras-refused");
        auto r = h.Run("install", {}, { "--running" });
        CHECK(r.Status == 1 && r.Questions == 0 && Contains(r.Output, "the game is running") && !Exists(h.Data));
    }
}

TEST(extras_ask_nothing_without_a_terminal_or_with_flags)
{
    {
        // No terminal: nothing asked, nothing written for them, and it ends.
        ExtrasHome h("extras-none");
        auto r = h.Run("install", {}, {}, false);
        CHECK(r.Status == 0 && !r.Hung && r.Questions == 0 && Exists(h.Bin + "/" + kLibrary));
        CHECK(!Exists(h.Camera) && h.ConfValue() == "(none)");
        CHECK(Contains(r.Output, "Native Camera Tweaks: off, not chosen yet (" + h.Camera + " is not there)"));
        CHECK(Contains(r.Output, "MetalFX Temporal: off, not chosen yet (" + h.Conf + " has no gfx_temporal line)"));
        CHECK(Contains(r.Output, "Native Camera Tweaks and MetalFX Temporal are off for now (there was no terminal to ask on)"));
        // Later, on a terminal: asked.
        r = h.Run("install", { "y\n", "n\n" });
        CHECK(r.Questions == 2 && ReadText(h.Camera) == WithEnabled(CameraDefaults(), "true") && h.ConfValue() == "0");
    }
    {
        // A dry run asks nothing and writes nothing; with a flag it says what it would write.
        ExtrasHome h("extras-dry");
        auto r = h.Run("install", {}, { "--dry-run" });
        CHECK(r.Status == 0 && r.Questions == 0 && !Exists(h.Data) && Contains(r.Output, "off for now (a dry run asks nothing)"));
        r = h.Run("install", {}, { "--dry-run", "--camera=on", "--upscaling=on" });
        CHECK(r.Status == 0 && r.Questions == 0 && !Exists(h.Data));
        CHECK(Contains(r.Output, "  create " + h.Camera + " from " + h.Release + "/camera/" + kCameraDefaultSettings + ", with Enabled = true"));
        CHECK(Contains(r.Output, "  add gfx_temporal = 1 to " + h.Conf));
    }
    {
        // Both flags: nothing asked on the terminal.
        ExtrasHome h("extras-flags");
        auto r = h.Run("install", {}, { "--camera=on", "--upscaling=off" });
        CHECK(r.Status == 0 && r.Questions == 0 && ReadText(h.Camera) == WithEnabled(CameraDefaults(), "true") && h.ConfValue() == "0");
        // A flag over an earlier choice: the flag is what is written.
        r = h.Run("install", {}, { "--camera=off" });
        CHECK(r.Status == 0 && r.Questions == 0 && ReadText(h.Camera) == WithEnabled(CameraDefaults(), "false") && h.ConfValue() == "0");
        CHECK(Contains(r.Output, "  set Enabled = false under [General] in " + h.Camera + ", line "));
        // Without a terminal too.
        r = h.Run("install", {}, { "--upscaling=on" }, false);
        CHECK(r.Status == 0 && h.ConfValue() == "1");
    }
    {
        // One flag: only the other question.
        ExtrasHome h("extras-one-flag");
        auto r = h.Run("install", { "y\n" }, { "--upscaling=on" });
        CHECK(r.Status == 0 && r.Questions == 1 && Contains(r.Output, "Turn on Native Camera Tweaks?") && !Contains(r.Output, "Turn on MetalFX"));
        CHECK(Contains(r.Output, "Optional features (change later with \"bg3mse-install options\"):"));
        CHECK(ReadText(h.Camera) == WithEnabled(CameraDefaults(), "true") && h.ConfValue() == "1");
    }
    {
        // --defaults: off for each, written, nothing asked; an earlier choice stands.
        ExtrasHome h("extras-defaults");
        CHECK(h.Run("install", {}, { "--upscaling=on" }, false).Status == 0);
        auto r = h.Run("install", {}, { "--defaults" });
        CHECK(r.Status == 0 && r.Questions == 0 && ReadText(h.Camera) == WithEnabled(CameraDefaults(), "false") && h.ConfValue() == "1");
    }
    {
        // A release without the camera module, or without its licence texts: not offered, and said why.
        ExtrasHome h("extras-no-camera", false);
        auto r = h.Run("install", { "\n" });
        CHECK(r.Status == 0 && r.Questions == 1 && !Contains(r.Output, "Turn on Native Camera Tweaks?"));
        CHECK(Contains(r.Output, "Native Camera Tweaks is not offered: the release in " + h.Release + " has no camera module (libbg3mse-camera.dylib)."));
        CHECK(!Exists(h.Bin + "/" + kCameraLibrary) && !Exists(h.Camera));
        ExtrasHome l("extras-no-licence");
        unlink((l.Release + "/camera/COPYING").c_str());
        r = l.Run("install", {}, { "--camera=on" });
        CHECK(r.Status == kInstalledWithoutExtras && Exists(l.Bin + "/" + kLibrary) && !Exists(l.Bin + "/" + kCameraLibrary) && !Exists(l.Camera));
        CHECK(Contains(r.Output, "libbg3mse-camera.dylib is left out: its licence texts are not all in " + l.Release + "/camera (missing: COPYING)"));
        CHECK(Contains(r.Output, "Native Camera Tweaks is not turned on: libbg3mse-camera.dylib is not installed without its licence texts."));
        CHECK(r.Output.ends_with("bg3mse itself is installed; only Native Camera Tweaks was not set, as said above.\n"));
    }
}

TEST(extras_keep_what_else_the_files_hold)
{
    ExtrasHome h("extras-user");
    fs::create_directories(h.Data);
    std::string conf = "# mine\r\nscript_extender = 0\r\n; old\r\nmy_key = 3\r\nlog_debug=1";
    std::string camera = "# my camera\n[General]\n# a note\nWatchForConfigChanges = false   # keep\nUnknownThing = \"a # b\"\n[Mouse]\n"
                         "Enabled = true\nMousePitchMult = 0.5";
    WriteText(h.Conf, conf);
    chmod(h.Conf.c_str(), 0600);
    // The camera's file is kept elsewhere, and linked: the file it names is changed, the link stays.
    std::string elsewhere = h.Root + "/dotfiles/camera.toml";
    fs::create_directories(h.Root + "/dotfiles");
    WriteText(elsewhere, camera);
    fs::create_symlink(elsewhere, h.Camera);
    auto r = h.Run("install", { "y\n", "y\n" });
    CHECK(r.Status == 0 && r.Questions == 2);
    CHECK(ReadText(h.Conf) == conf + "\r\ngfx_temporal = 1\r\n");
    struct stat st{};
    CHECK(stat(h.Conf.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(fs::is_symlink(h.Camera) && fs::read_symlink(h.Camera) == elsewhere);
    std::string on = "# my camera\n[General]\n# a note\nEnabled = true\nWatchForConfigChanges = false   # keep\nUnknownThing = \"a # b\"\n"
                     "[Mouse]\nEnabled = true\nMousePitchMult = 0.5";
    CHECK(ReadText(elsewhere) == on);
    CHECK(CameraReadsAs(elsewhere, "Enabled: true\nignored: 2 (first at line 6)\n"));
    CHECK(bg3se::mac::ConfLookup(ReadText(h.Conf), "script_extender") == "0" && h.ConfValue() == "1");
    // options: off again, in the value alone.
    auto t = h.Tool({ "options", "--camera=off", "--upscaling=off" });
    CHECK(t.Status == 0);
    std::string off = on;
    off.replace(off.find("Enabled = true"), strlen("Enabled = true"), "Enabled = false");
    CHECK(ReadText(elsewhere) == off && ReadText(h.Conf) == conf + "\r\ngfx_temporal = 0\r\n");
}

TEST(extras_leave_a_file_their_reader_would_not_read_as_it_is)
{
    ExtrasHome h("extras-malformed");
    fs::create_directories(h.Data);
    std::string camera = "[General\nEnabled = false\n", conf = "script_extender = 1\nthis line has no equals sign\ngfx_temporal = 1\n";
    WriteText(h.Camera, camera);
    WriteText(h.Conf, conf);
    auto r = h.Run("install", { "y\n", "y\n" });
    CHECK(r.Status == 0 && r.Questions == 0 && ReadText(h.Camera) == camera && ReadText(h.Conf) == conf);
    CHECK(Contains(r.Output, "Native Camera Tweaks is not asked about: " + h.Camera + " is left as it is: line 1 is not a [section] line the "
                             "camera module reads."));
    CHECK(Contains(r.Output, "MetalFX Temporal is not asked about: " + h.Conf + " is left as it is: line 2 is not a comment or a \"key = value\" "
                             "line, and bg3mse skips it."));
    CHECK(Contains(r.Output, "Native Camera Tweaks: cannot tell: " + h.Camera + " is not read as a whole by the camera module"));
    CHECK(Contains(r.Output, "MetalFX Temporal: on as bg3mse reads " + h.Conf + ", which bg3mse-install leaves as it is"));
    r = h.Run("install", {}, { "--camera=on", "--upscaling=off" });
    CHECK(r.Status == kInstalledWithoutExtras && ReadText(h.Camera) == camera && ReadText(h.Conf) == conf && Contains(r.Output, "Native Camera Tweaks is not set: "));
    CHECK(r.Output.ends_with("bg3mse itself is installed; only Native Camera Tweaks and MetalFX Temporal were not set, as said above.\n"));
    // A dry run with it fails too, and does not say bg3mse is installed.
    r = h.Run("install", {}, { "--camera=on", "--dry-run" });
    CHECK(r.Status == 1 && !Contains(r.Output, "bg3mse itself is installed"));
    auto t = h.Tool({ "options", "--camera=on" });
    CHECK(t.Status == 1 && ReadText(h.Camera) == camera && Contains(t.Output, "Native Camera Tweaks is not set: " + h.Camera + " is left as it is"));
    // A value the module does not read.
    WriteText(h.Camera, "[General]\nEnabled = yes\n");
    t = h.Tool({ "options", "--camera=off" });
    CHECK(t.Status == 1 && ReadText(h.Camera) == "[General]\nEnabled = yes\n" &&
        Contains(t.Output, "line 2 has Enabled = yes, which is not true or false, and the camera module skips it"));
    // A link to a file that is not there: not followed, and not replaced.
    std::string nowhere = h.Root + "/nowhere/camera.toml";
    fs::remove(h.Camera);
    fs::create_symlink(nowhere, h.Camera);
    t = h.Tool({ "options", "--camera=on" });
    CHECK(t.Status == 1 && fs::is_symlink(h.Camera) && fs::read_symlink(h.Camera) == nowhere && !Exists(nowhere));
    CHECK(Contains(t.Output, "Native Camera Tweaks is not set: " + h.Camera + " is left as it is: it is a link that cannot be followed: "));
}

TEST(options_turns_the_extras_on_and_off)
{
    {
        // Before an install: bg3mse.conf is not made by options, so that the
        // install makes it with the default settings (the Script Extender on).
        ExtrasHome h("extras-options-first");
        auto r = h.Run("options", {}, { "--upscaling=on", "--camera=on" });
        CHECK(r.Status == 1 && !Exists(h.Data));
        CHECK(Contains(r.Output, "MetalFX Temporal is not turned on: it is part of bg3mse's graphics module, and there is no libbg3mse.dylib in " +
                                 h.Bin + "."));
        CHECK(Contains(r.Output, "Native Camera Tweaks is not turned on: libbg3mse-camera.dylib is not installed in " + h.Bin + "."));
        r = h.Run("options", { "\n" });
        CHECK(r.Status == 0 && r.Questions == 0 && !Exists(h.Data));
        CHECK(h.Run("install", {}, { "--upscaling=on" }, false).Status == 0);
        CHECK(bg3se::mac::ConfLookup(ReadText(h.Conf), "script_extender") == "1" && h.ConfValue() == "1");
    }
    ExtrasHome h("extras-options");
    CHECK(h.Run("install", {}, {}, false).Status == 0);
    // No terminal and no flag: what they are, and how to change them.
    auto t = h.Tool({ "options" });
    CHECK(t.Status == 0 && !t.Hung && t.Questions == 0 && Contains(t.Output, "Optional features:\n"));
    CHECK(Contains(t.Output, "  Native Camera Tweaks: off, not chosen yet (" + h.Camera + " is not there)\n"));
    CHECK(Contains(t.Output, "There is no terminal to ask on. To change them: \"" + h.Bin + "/bg3mse-install\" options --camera=on|off "
                             "--upscaling=on|off\n"));
    CHECK(!Exists(h.Camera));
    // Flags.
    t = h.Tool({ "options", "--camera=on" });
    CHECK(t.Status == 0 && ReadText(h.Camera) == WithEnabled(CameraDefaults(), "true") && h.ConfValue() == "(none)");
    CHECK(Contains(t.Output, "  create " + h.Camera + " from " + h.Bin + "/camera/" + kCameraDefaultSettings + ", with Enabled = true"));
    CHECK(Contains(t.Output, "Takes effect next time the game starts."));
    t = h.Tool({ "options", "--camera=on" });
    CHECK(t.Status == 0 && Contains(t.Output, "Nothing to change.") && Contains(t.Output, "Native Camera Tweaks: on (Enabled = true"));
    // On a terminal: both asked, the values now the defaults.
    t = h.Tool({ "options" }, { "\n", "y\n" }, true);
    CHECK(t.Status == 0 && t.Questions == 2 && Contains(t.Output, "Turn on Native Camera Tweaks? [Y/n] ") &&
        Contains(t.Output, "Turn on MetalFX Temporal? [y/N] "));
    CHECK(Contains(t.Output, "Optional features (Return keeps the current setting):\n\nTurn on Native Camera Tweaks? [Y/n] \n"
                             "Turn on MetalFX Temporal? [y/N] y\n\n"));
    CHECK(ReadText(h.Camera) == WithEnabled(CameraDefaults(), "true") && h.ConfValue() == "1");
    t = h.Tool({ "options" }, { "n\n", "\n" }, true);
    CHECK(t.Status == 0 && Contains(t.Output, "Turn on MetalFX Temporal? [Y/n] ") && h.ConfValue() == "1");
    CHECK(ReadText(h.Camera) == WithEnabled(CameraDefaults(), "false"));
    t = h.Tool({ "options" }, { "\x04" }, true);
    CHECK(t.Status == 0 && t.Questions == 1 && !t.Hung && ReadText(h.Camera) == WithEnabled(CameraDefaults(), "false"));
    // A dry run.
    t = h.Tool({ "options", "--dry-run", "--upscaling=off" });
    CHECK(t.Status == 0 && Contains(t.Output, "Dry run; nothing is changed. bg3mse-install options would:\n  set gfx_temporal = 0 in " + h.Conf) &&
        h.ConfValue() == "1");
    // While the game runs: changed, for its next start.
    auto r = h.Run("options", {}, { "--upscaling=off", "--running" });
    CHECK(r.Status == 0 && h.ConfValue() == "0" && Contains(r.Output, "Takes effect next time the game starts."));
    // macOS before 13: not turned on; off can be written over a choice of on.
    r = h.Run("options", {}, { "--upscaling=on" }, true, "12.7");
    CHECK(r.Status == 1 && h.ConfValue() == "0" && Contains(r.Output, "MetalFX Temporal is not turned on: requires macOS 13 or later "
                                                                      "(this Mac has 12.7)."));
    CHECK(h.Run("options", {}, { "--upscaling=on" }).Status == 0 && h.ConfValue() == "1");
    r = h.Run("options", {}, { "--upscaling=off" }, false, "12.7");
    CHECK(r.Status == 0 && h.ConfValue() == "0");
    r = h.Run("options", { "\n" }, {}, true, "12.7");
    CHECK(r.Questions == 1 && Contains(r.Output, "MetalFX Temporal is not offered: requires macOS 13 or later"));
    // Without the camera module in bin/.
    fs::rename(h.Bin + "/" + kCameraLibrary, h.Root + "/camera aside");
    r = h.Run("options", {}, { "--camera=on" });
    CHECK(r.Status == 1 && Contains(r.Output, "Native Camera Tweaks is not turned on: libbg3mse-camera.dylib is not installed in " + h.Bin + "."));
    r = h.Run("status", {}, {}, false);
    CHECK(Contains(r.Output, "  Native Camera Tweaks: unavailable: libbg3mse-camera.dylib is not installed in " + h.Bin + "\n"));
    fs::rename(h.Root + "/camera aside", h.Bin + "/" + kCameraLibrary);
    // What is not right on the command line.
    for (auto args : std::vector<std::vector<std::string>>{ { "options", "--camera=maybe" }, { "options", "--defaults" },
             { "options", "--purge" }, { "status", "--camera=on" }, { "uninstall", "--upscaling=off" }, { "install", "--camera" },
             { "options", h.Root } }) {
        t = h.Tool(args);
        CHECK(t.Status == 2);
    }
    CHECK(Contains(h.Tool({ "options", "--camera=maybe" }).Output, "bg3mse-install: --camera takes on or off, not \"maybe\""));
}

TEST(status_says_what_each_extra_is)
{
    ExtrasHome h("extras-status");
    CHECK(h.Run("install", {}, { "--camera=on", "--upscaling=off" }, false).Status == 0);
    auto r = h.Run("status", {}, {}, false);
    CHECK(r.Status == 0 && Contains(r.Output, "  Native Camera Tweaks: on (Enabled = true under [General] in " + h.Camera + ", line "));
    CHECK(Contains(r.Output, "  MetalFX Temporal: off (gfx_temporal = 0 in " + h.Conf + ", line 5)\n"));
    r = h.Run("status", {}, {}, false, "12.0");
    CHECK(Contains(r.Output, "  MetalFX Temporal: unavailable: requires macOS 13 or later (this Mac has 12.0)\n"));
    CHECK(h.Run("options", {}, { "--upscaling=on" }, false).Status == 0);
    r = h.Run("status", {}, {}, false, "12.0");
    CHECK(Contains(r.Output, "  MetalFX Temporal: unavailable: requires macOS 13 or later (this Mac has 12.0) (" + h.Conf +
        " has gfx_temporal = 1)\n"));
    // --purge takes the camera's settings too.
    r = h.Run("uninstall", {}, { "--purge" }, false);
    CHECK(r.Status == 0 && !Exists(h.Camera) && !Exists(h.Conf));
}

TEST(upscaling_is_offered_only_with_the_graphics_module)
{
    using bg3se::mac::updater::ReadGfxMark;
    std::string const upscaling = "Turn on MetalFX Temporal?";
    // What the library says: the stand-in with the mark, the one without
    // it, and a file that is not a library, where it cannot be told.
    bool marked = false;
    std::string why;
    CHECK(ReadGfxMark(kFixtures + "/standin/" + kLibrary, marked, why) && marked);
    CHECK(ReadGfxMark(kFixtures + "/standin-plain/" + kLibrary, marked, why) && !marked);
    CHECK(!ReadGfxMark(kFixtures + "/standin/camera/LICENSE", marked, why) && !marked && Contains(why, "is not arm64 Mach-O code"));
#ifdef BG3MSE_GFX_DYLIB
    // This build's libbg3mse.dylib, built with the graphics module.
    CHECK(ReadGfxMark(BG3MSE_GFX_DYLIB, marked, why) && marked);
#endif

    // The libbg3mse.dylib being installed does not have it: not offered, and why is said.
    ExtrasHome h("extras-no-gfx");
    WriteFile(h.Release + "/" + kLibrary, ReadFile(kFixtures + "/standin-plain/" + kLibrary));
    std::string release = "it is part of bg3mse's graphics module, and " + h.Release + "/" + kLibrary + " does not say it has that module";
    auto r = h.Run("install", { "\n" });
    CHECK(r.Status == 0 && r.Questions == 1 && !Contains(r.Output, upscaling) && h.ConfValue() == "(none)");
    CHECK(Contains(r.Output, "MetalFX Temporal is not offered: " + release + ".\n"));
    // Asked for by a flag: not turned on, and the install fails once bg3mse is in place.
    r = h.Run("install", {}, { "--upscaling=on" });
    CHECK(r.Status == kInstalledWithoutExtras && h.ConfValue() == "(none)" && Contains(r.Output, "MetalFX Temporal is not turned on: " + release + ".\n"));
    CHECK(Contains(r.Output, "installed in " + h.Bin));
    CHECK(r.Output.ends_with("bg3mse itself is installed; only MetalFX Temporal was not set, as said above.\n"));
    // status and options: as the installed library says.
    std::string installed = "it is part of bg3mse's graphics module, and " + h.Bin + "/" + kLibrary + " does not say it has that module";
    r = h.Run("status", {}, {}, false);
    CHECK(r.Status == 0 && Contains(r.Output, "  MetalFX Temporal: unavailable: " + installed + "\n"));
    auto t = h.Tool({ "options", "--upscaling=on" });
    CHECK(t.Status == 1 && h.ConfValue() == "(none)" && Contains(t.Output, "MetalFX Temporal is not turned on: " + installed + ".\n"));
    t = h.Tool({ "options" }, { "\n" }, true);
    CHECK(t.Status == 0 && t.Questions == 1 && !Contains(t.Output, upscaling) &&
        Contains(t.Output, "MetalFX Temporal is not offered: " + installed + ".\n"));
    // A choice of on made before stays, said as it is, until off is asked for.
    WriteText(h.Conf, ReadText(h.Conf) + "gfx_temporal = 1\n");
    r = h.Run("status", {}, {}, false);
    CHECK(Contains(r.Output, "  MetalFX Temporal: unavailable: " + installed + " (" + h.Conf + " has gfx_temporal = 1)\n"));
    t = h.Tool({ "options", "--upscaling=off" });
    CHECK(t.Status == 0 && h.ConfValue() == "0");
    // A library of which it cannot be told: not offered either.
    WriteText(h.Bin + "/" + kLibrary, "not a library");
    t = h.Tool({ "options", "--upscaling=on" });
    CHECK(t.Status == 1 && h.ConfValue() == "0" &&
        Contains(t.Output, "MetalFX Temporal is not turned on: it is part of bg3mse's graphics module, and whether " + h.Bin + "/" + kLibrary +
                               " has that module cannot be told: " + h.Bin + "/" + kLibrary + " is not arm64 Mach-O code.\n"));
}

int main(int argc, char** argv)
{
    if (argc == 3 && std::string(argv[1]) == kShaderInterfaceVerb) return ReadStandIn(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "extras-child") return ExtrasChild(std::vector<std::string>(argv + 2, argv + argc));
    if (argc != 1) {
        fprintf(stderr, "usage: bg3mse_installer_tests\n");
        return 2;
    }
    void (*tests[])() = {
        the_launcher_names_the_dylib_only_when_it_can_be_loaded_run,
        the_launcher_starts_the_game_with_and_without_the_dylib_run,
        setup_puts_the_release_in_bin_and_the_launch_option_on_the_clipboard_run,
        install_reads_the_games_metal_functions_when_mod_shaders_are_translated_run,
        the_games_shaders_are_read_before_the_game_starts_run,
        the_launcher_says_in_its_log_what_it_did_run,
        the_tool_installs_for_the_launch_option_run,
        extras_files_are_changed_in_the_one_key_run,
        extras_are_asked_about_on_a_terminal_run,
        extras_ask_nothing_without_a_terminal_or_with_flags_run,
        extras_keep_what_else_the_files_hold_run,
        extras_leave_a_file_their_reader_would_not_read_as_it_is_run,
        options_turns_the_extras_on_and_off_run,
        status_says_what_each_extra_is_run,
        upscaling_is_offered_only_with_the_graphics_module_run,
    };
    for (auto test : tests) test();
    fs::remove_all(kWork);
    printf("%d failure(s)\n", gFailures);
    return gFailures ? 1 : 0;
}
