// Tests of libbg3mse.dylib as a file that is loaded: what the image does by
// being loaded, and what its start-up does with the Script Extender's setting
// off and on. They load the dylib that was built, in processes of their own.
//
//   bg3mse_dylib_tests <libbg3mse.dylib> <stand-in executable> <extender linked: 0|1>
//
// 1. Loaded into a process that is not the game, the image must do nothing:
//    its initializers (those of every file linked into it, Norbyte's code
//    included) start no thread, open no file, write nothing to the standard
//    streams and leave nothing in the home directory.
// 2. Loaded into a process named like the game (the stand-in, which has none
//    of the game's code), with a home directory of the test's:
//      - with no settings file, and with the setting off, the log holds the
//        lines bg3mse writes without the extender, and not one of the
//        extender's;
//      - with the setting on and the extender linked, the start-up sequence
//        runs and refuses (there is no game to read), says so in the log,
//        asks for the message box Windows shows for a game it does not
//        support, and the process goes on and ends normally.
//    Every run has the alert recorder (Mac/Tests/AlertRecorder.c) in place of
//    the system's alerts: nothing is put on the screen.
//      - the log takes DEBUG lines when bg3mse.conf switches log_debug on,
//        and only then.
// 3. Loaded into the game's crash reporter (the game's executable started
//    with the argument -crash), the image does nothing, whatever the settings
//    say: no log, no file, no line on the standard streams, no alert.
// 4. Named in DYLD_INSERT_LIBRARIES, as a launch option names it: it starts
//    once, says so, and a second copy of it in the process does nothing.
// 5. With the extender linked it exports Norbyte's native API, which a
//    process without an extender object can call to no effect.
// 6. The line the main menu shows under the game's version names this build,
//    and keeps the words Norbyte's code looks for so it is added once.

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <mach/mach.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <Release/Version.h>

namespace {

int gFailures = 0;
char const* gTest = "";
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)

unsigned ThreadCount()
{
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return 0;
    for (mach_msg_type_number_t i = 0; i < count; i++) mach_port_deallocate(mach_task_self(), threads[i]);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
    return count;
}

std::vector<int> OpenDescriptors()
{
    std::vector<int> open;
    for (int fd = 0; fd < 1024; fd++) {
        if (fcntl(fd, F_GETFD) != -1) open.push_back(fd);
    }
    return open;
}

std::vector<std::string> FilesUnder(std::string const& dir)
{
    std::vector<std::string> files;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            std::string path = dir + "/" + name;
            struct stat st{};
            if (lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                auto below = FilesUnder(path);
                files.insert(files.end(), below.begin(), below.end());
            } else {
                files.push_back(path);
            }
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string ReadAll(std::string const& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string TempDir(char const* what)
{
    char const* tmp = getenv("TMPDIR");
    std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/bg3mse-dylib-" + what + ".XXXXXX";
    return mkdtemp(pattern.data()) ? pattern : std::string();
}

void RemoveTree(std::string const& dir)
{
    if (dir.empty()) return;
    for (auto const& file : FilesUnder(dir)) unlink(file.c_str());
    // Directories, deepest first: few, so by trying until none is left.
    for (int pass = 0; pass < 8; pass++) {
        std::vector<std::string> dirs{ dir };
        for (size_t i = 0; i < dirs.size(); i++) {
            if (DIR* d = opendir(dirs[i].c_str())) {
                while (dirent* e = readdir(d)) {
                    std::string name = e->d_name;
                    if (name != "." && name != "..") dirs.push_back(dirs[i] + "/" + name);
                }
                closedir(d);
            }
        }
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) rmdir(it->c_str());
    }
}

// --- 1: loaded into a process that is not the game ------------------------------

// In a child, so that what the image leaves behind is not this test's own.
void loading_the_image_does_nothing(char const* dylib)
{
    gTest = "loading_the_image_does_nothing";
    std::string home = TempDir("home");
    CHECK(!home.empty());
    std::string out = home + ".out";
    std::string alerts = home + ".alerts";
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setenv("HOME", home.c_str(), 1);
        int capture = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(capture, STDOUT_FILENO);
        dup2(capture, STDERR_FILENO);
        close(capture);

        unsigned threads = ThreadCount();
        auto descriptors = OpenDescriptors();
        void* image = dlopen(dylib, RTLD_NOW | RTLD_LOCAL);
        if (!image) _exit(10);
        if (ThreadCount() != threads) _exit(11);
        if (OpenDescriptors() != descriptors) _exit(12);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 10) printf("  the image did not load\n");
        if (code == 11) printf("  loading the image started or ended a thread\n");
        if (code == 12) printf("  loading the image opened or closed a file\n");
        CHECK(code == 0);
    } else {
        printf("  the process loading the image was ended by signal %d\n", WTERMSIG(status));
    }
    std::string said = ReadAll(out);
    if (!said.empty()) printf("  the image wrote to the standard streams:\n%s\n", said.c_str());
    CHECK(said.empty());
    auto left = FilesUnder(home);
    for (auto const& file : left) printf("  the image left %s\n", file.c_str());
    CHECK(left.empty());
    unlink(out.c_str());
    RemoveTree(home);
}

// --- 5: the native API -----------------------------------------------------------

// What a native module is given and gives back (Norbyte's
// Extender/Shared/APIExports.h, as a module without his headers declares it).
using NativeLoaded = void(void* module, void* userData);
using NativeStateChange = void(uint32_t context, uint32_t event, void* L, void* userData);
using NativeCallback = void(void* userData);

void* gToldModule = nullptr;
int gCalledBack = 0;

// With the extender linked, the dylib exports the seven functions of
// Norbyte's native API, by the names his BG3ScriptExtender.dll exports them
// under. In a process that is not the game there is no extender object: the
// module is still the dylib's, the version is still told, and the functions
// that need the extender do nothing. Without the extender the dylib exports
// none of them.
void the_native_api_is_exported_and_does_nothing_without_an_extender(char const* dylib, bool extenderLinked)
{
    gTest = "the_native_api_is_exported_and_does_nothing_without_an_extender";
    static char const* const kNames[] = { "SEUpdaterGetModule", "SEUpdaterNotifyWhenLoaded", "SEGetAPIVersion",
        "SERegisterLuaStateChangeCallback", "SEScheduleCallbackOnThread", "SEEnterLuaVM", "SEExitLuaVM" };
    std::string home = TempDir("home");
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setenv("HOME", home.c_str(), 1);
        void* image = dlopen(dylib, RTLD_NOW | RTLD_LOCAL);
        if (!image) _exit(10);
        void* found[std::size(kNames)];
        for (size_t i = 0; i < std::size(kNames); i++) {
            found[i] = dlsym(image, kNames[i]);
            if ((found[i] != nullptr) != extenderLinked) _exit(20 + (int)i);
        }
        if (!extenderLinked) _exit(0);

        auto getModule = reinterpret_cast<void* (*)()>(found[0]);
        auto notifyWhenLoaded = reinterpret_cast<void (*)(NativeLoaded*, void*)>(found[1]);
        auto getVersion = reinterpret_cast<uint32_t (*)()>(found[2]);
        auto registerCallback = reinterpret_cast<void (*)(NativeStateChange*, void*)>(found[3]);
        auto schedule = reinterpret_cast<void (*)(uint32_t, NativeCallback*, void*)>(found[4]);
        auto enter = reinterpret_cast<void* (*)(uint32_t)>(found[5]);
        auto leave = reinterpret_cast<void (*)(void*)>(found[6]);

        // Version 1.0, as APIVersion::VerInitial has it.
        if (getVersion() != (1u << 16)) _exit(30);
        // The module is what dlsym takes, and leads to the same functions.
        void* module = getModule();
        if (!module) _exit(31);
        for (size_t i = 0; i < std::size(kNames); i++) {
            if (dlsym(module, kNames[i]) != found[i]) _exit(32);
        }
        if (getModule() != module) _exit(33);
        // Loaded directly, the extender tells a module at once.
        notifyWhenLoaded([](void* told, void* userData) { gToldModule = userData == &gCalledBack ? told : nullptr; }, &gCalledBack);
        if (gToldModule != module) _exit(34);

        // No extender object in this process: nothing is entered, nothing
        // is called back, and no thread is started for it.
        unsigned threads = ThreadCount();
        registerCallback([](uint32_t, uint32_t, void*, void*) { gCalledBack++; }, nullptr);
        schedule(0, [](void*) { gCalledBack++; }, nullptr);
        schedule(1, [](void*) { gCalledBack++; }, nullptr);
        if (enter(0) || enter(1)) _exit(35);
        leave(nullptr);
        usleep(200000);
        if (gCalledBack != 0) _exit(36);
        if (ThreadCount() != threads) _exit(37);
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 10) printf("  the image did not load\n");
        if (code >= 20 && code < 30) printf("  %s is %s\n", kNames[code - 20], extenderLinked ? "not exported" : "exported without the extender");
        if (code == 30) printf("  SEGetAPIVersion is not 1.0\n");
        if (code >= 31 && code <= 33) printf("  SEUpdaterGetModule's module does not lead dlsym to the dylib's functions (%d)\n", code);
        if (code == 34) printf("  SEUpdaterNotifyWhenLoaded did not call back at once with the module\n");
        if (code == 35) printf("  a Lua state was entered without an extender\n");
        if (code == 36) printf("  a callback ran without an extender\n");
        if (code == 37) printf("  a function of the API started or ended a thread\n");
        CHECK(code == 0);
    } else {
        printf("  the process calling the API was ended by signal %d\n", WTERMSIG(status));
    }
    RemoveTree(home);
}

// --- 2: loaded into a process named like the game ----------------------------------

struct StandInRun
{
    bool Exited{ false };
    int Code{ -1 };
    std::string Said;                   // the stand-in's standard output and error
    std::vector<std::string> Log;       // the lines of bg3mse's log, without their time
    std::vector<std::string> Files;     // everything left in the home directory, relative to it
    std::vector<std::string> Alerts;    // "<caption>\t<text>\t<button>" for each alert asked for
};

// The alert recorder (Mac/Tests/AlertRecorder.c), which is built next to the stand-in.
std::string gAlertRecorder;

// Runs the stand-in, which loads the dylib, with a home directory holding
// `conf` as bg3mse.conf (nullptr: no settings file). `arguments` follow the
// dylib's path on its command line. `inserted`: dyld also loads the dylib
// itself, before the stand-in's main(), as the game's load command has it
// loaded.
// When set, an inserted run names this file in DYLD_INSERT_LIBRARIES, and the
// stand-in still loads the dylib it is given: two copies in one process.
std::string gInsertInstead;

StandInRun RunStandIn(char const* standIn, char const* dylib, char const* conf, std::vector<char const*> arguments = {},
    bool inserted = false, bool alertExpected = false)
{
    StandInRun run;
    std::string home = TempDir("game-home");
    if (home.empty()) return run;
    std::string support = home + "/Library/Application Support/bg3mse";
    if (conf) {
        mkdir((home + "/Library").c_str(), 0700);
        mkdir((home + "/Library/Application Support").c_str(), 0700);
        mkdir(support.c_str(), 0700);
        std::ofstream(support + "/bg3mse.conf") << conf;
    } else {
        // The dylib makes its own directory, not its parents.
        mkdir((home + "/Library").c_str(), 0700);
        mkdir((home + "/Library/Application Support").c_str(), 0700);
    }
    std::string out = home + ".out";
    std::string alerts = home + ".alerts";
    // The dylib knows the game's process by the name of its executable.
    std::string gameDir = home + ".game";
    std::string game = gameDir + "/Baldur's Gate 3";
    mkdir(gameDir.c_str(), 0700);
    if (symlink(standIn, game.c_str()) != 0) return run;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setenv("HOME", home.c_str(), 1);
        unsetenv("BG3MSE_DISABLE");
        unsetenv("BG3MSE_LOG_DEBUG");
        // Every run has the recorder in place of the system's alerts.
        std::string insert = gAlertRecorder + (inserted ? std::string(":") + (gInsertInstead.empty() ? std::string(dylib) : gInsertInstead) : std::string());
        setenv("DYLD_INSERT_LIBRARIES", insert.c_str(), 1);
        setenv("BG3MSE_TEST_ALERTS", alerts.c_str(), 1);
        if (alertExpected) {
            setenv("BG3MSE_TEST_ALERT_WAIT", "1", 1);
        } else {
            unsetenv("BG3MSE_TEST_ALERT_WAIT");
        }
        int capture = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(capture, STDOUT_FILENO);
        dup2(capture, STDERR_FILENO);
        close(capture);
        int null = open("/dev/null", O_RDONLY);
        dup2(null, STDIN_FILENO);
        std::vector<char*> argv{ game.data(), const_cast<char*>(dylib) };
        for (char const* argument : arguments) argv.push_back(const_cast<char*>(argument));
        argv.push_back(nullptr);
        execv(game.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    run.Exited = WIFEXITED(status);
    run.Code = run.Exited ? WEXITSTATUS(status) : WTERMSIG(status);
    run.Said = ReadAll(out);
    unlink(out.c_str());
    {
        std::ifstream f(alerts);
        for (std::string line; std::getline(f, line);) run.Alerts.push_back(line);
    }
    unlink(alerts.c_str());
    unlink(game.c_str());
    rmdir(gameDir.c_str());

    for (auto const& file : FilesUnder(home)) {
        std::string relative = file.substr(home.size() + 1);
        run.Files.push_back(relative);
        // The main log: logs/bg3mse_<time>.log (latest.log is a link to it).
        if (relative.find("/logs/bg3mse_") == std::string::npos) continue;
        std::ifstream f(file);
        for (std::string line; std::getline(f, line);) {
            // "[HH:MM:SS.mmm] [LEVEL] text"
            run.Log.push_back(line.size() > 15 ? line.substr(15) : line);
        }
    }
    RemoveTree(home);
    return run;
}

// The files a run left, without the settings file the test put there and
// with the time taken out of a log's name: what two runs can be compared by.
std::vector<std::string> Left(StandInRun const& run)
{
    std::vector<std::string> files;
    for (auto file : run.Files) {
        if (file == "Library/Application Support/bg3mse/bg3mse.conf") continue;
        // <prefix>_2026-11-10_21-30-05.log
        size_t at = file.rfind(".log");
        if (at != std::string::npos && at + 4 == file.size() && at >= 20 && file[at - 20] == '_' && file[at - 9] == '_') {
            file.replace(at - 19, 19, "<time>");
        }
        files.push_back(std::move(file));
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool AnyHas(std::vector<std::string> const& lines, char const* part)
{
    for (auto const& line : lines) {
        if (line.find(part) != std::string::npos) return true;
    }
    return false;
}

void Show(StandInRun const& run)
{
    printf("  exited %d, code %d\n", run.Exited, run.Code);
    for (auto const& file : run.Files) printf("  file: %s\n", file.c_str());
    for (auto const& line : run.Log) printf("  log: %s\n", line.c_str());
    if (!run.Said.empty()) printf("  said: %s\n", run.Said.c_str());
    for (auto const& alert : run.Alerts) printf("  alert: %s\n", alert.c_str());
}

// The log's first line: the dylib says which build it is, the one this test
// was built with.
std::string LoadedLine()
{
    namespace version = bg3se::mac::version;
    char line[256];
    snprintf(line, sizeof(line), "[INFO ] bg3mse %s loaded (commit %.12s, extender %s, %s)", version::kVersion,
        version::kCommit, version::kExtenderVersion, version::kChannel);
    return line;
}

// What the extender's start-up sequence and Norbyte's code say in the log.
constexpr char const* kExtenderLines[] = { "Script Extender", "StaticSymbols", "hooks on the game", "overlay:", "ScriptExtender::" };

void the_setting_decides_whether_the_extender_is_started(char const* standIn, char const* dylib, bool extenderLinked)
{
    gTest = "the_setting_decides_whether_the_extender_is_started";

    // No settings file, the setting absent, and the setting off: the same.
    auto none = RunStandIn(standIn, dylib, nullptr);
    CHECK(none.Exited && none.Code == 0 && none.Said == "stand-in: loaded\n");
    CHECK(!none.Log.empty() && none.Log.front() == LoadedLine());
    for (auto part : kExtenderLines) CHECK(!AnyHas(none.Log, part));
    CHECK(none.Alerts.empty());
    if (gFailures) Show(none);

    for (char const* conf : { "gfx_thread_idling = 0\n", "script_extender = 0\n", "script_extender = off\n", "script_extender =\n", "# script_extender = 1\n" }) {
        auto off = RunStandIn(standIn, dylib, conf);
        CHECK(off.Exited && off.Code == 0 && off.Said == "stand-in: loaded\n");
        CHECK(off.Log == none.Log && off.Alerts.empty());
        // The same files as without a settings file: the logs, and what the
        // modules that are always started leave.
        CHECK(Left(off) == Left(none));
        if (off.Log != none.Log || Left(off) != Left(none)) Show(off);
    }

    // On. The stand-in is not the game: the sequence must refuse, say so, tell
    // the player in a message box, as Windows does for a game it does not
    // support, and leave the process to go on.
    for (char const* conf : { "script_extender = 1\n", "Script_Extender = yes\n" }) {
        auto on = RunStandIn(standIn, dylib, conf, {}, false, extenderLinked);
        CHECK(on.Exited && on.Code == 0 && on.Said == "stand-in: loaded\n");
        if (extenderLinked) {
            CHECK(AnyHas(on.Log, "[ERROR] Script Extender not started: "));
            // The stand-in has none of the game's symbols: the engine table
            // resolves nothing, as on a game update that changed everything.
            CHECK(AnyHas(on.Log, "a critical member of StaticSymbols would stay null"));
            // And nothing of the extender runs after the refusal: no hook,
            // no StaticSymbols bound, no Lua library, no overlay hand-over.
            for (auto part : { "hooks on the game", "StaticSymbols: ", "[Unavailable]", "Script Extender: started", "overlay:",
                     "type indices:" }) {
                CHECK(!AnyHas(on.Log, part));
            }
            // The stand-in's executable is in no bundle: it has no version.
            constexpr char kNotice[] = "bg3mse doesn't support this game version yet. Starting without it. Check for a newer bg3mse.";
            CHECK(on.Alerts == std::vector<std::string>{ std::string("bg3mse\t") + kNotice + "\tOK" });
            CHECK(AnyHas(on.Log, (std::string("[INFO ] Script Extender: telling the player: ") + kNotice).c_str()));
            // Everything the log has without the extender, in the same order.
            std::vector<std::string> rest;
            for (auto const& line : on.Log) {
                bool extender = false;
                for (auto part : kExtenderLines) extender = extender || line.find(part) != std::string::npos;
                if (!extender) rest.push_back(line);
            }
            // A refusal's reason may run over several lines, which carry no time.
            std::vector<std::string> expected = none.Log;
            auto missing = std::find_if(expected.begin(), expected.end(), [&](std::string const& line) {
                return std::find(rest.begin(), rest.end(), line) == rest.end();
            });
            CHECK(missing == expected.end());
            // No directory for crash reports, no file of the extender's: Norbyte's code never ran.
            CHECK(Left(on) == Left(none));
        } else {
            // This build's dylib does not carry the extender: the setting is not read.
            CHECK(on.Log == none.Log && Left(on) == Left(none) && on.Alerts.empty());
        }
        if (gFailures) Show(on);
    }
}

void the_setting_decides_whether_the_log_takes_debug_lines(char const* standIn, char const* dylib)
{
    gTest = "the_setting_decides_whether_the_log_takes_debug_lines";
    constexpr char kLine[] = "[DEBUG] the log takes DEBUG lines (log_debug in bg3mse.conf, or BG3MSE_LOG_DEBUG)";

    for (char const* conf : { (char const*)nullptr, "log_debug = 0\n", "log_debug = off\n", "# log_debug = 1\n" }) {
        auto off = RunStandIn(standIn, dylib, conf);
        CHECK(off.Exited && off.Code == 0 && off.Said == "stand-in: loaded\n");
        CHECK(!off.Log.empty() && off.Log.front() == LoadedLine());
        CHECK(!AnyHas(off.Log, "[DEBUG]"));
        if (gFailures) Show(off);
    }
    for (char const* conf : { "log_debug = 1\n", "LOG_DEBUG = yes\n" }) {
        auto on = RunStandIn(standIn, dylib, conf);
        CHECK(on.Exited && on.Code == 0 && on.Said == "stand-in: loaded\n");
        CHECK(on.Log.size() >= 2 && on.Log[0] == LoadedLine() && on.Log[1] == kLine);
        if (gFailures) Show(on);
    }
}

// --- 4: named in DYLD_INSERT_LIBRARIES ---------------------------------------------

size_t Count(std::vector<std::string> const& lines, std::string const& line)
{
    return (size_t)std::count(lines.begin(), lines.end(), line);
}

// A launch option can name the dylib in DYLD_INSERT_LIBRARIES instead of the
// installer's load command (Platform/Process.h). Inserted, it starts as it
// does when loaded, says how it was loaded, and has the libraries loaded with
// the process initialized first. Two copies in one process (the load command
// and the variable naming different files) start once.
void loaded_by_the_variable_it_starts_once(char const* standIn, char const* dylib, bool extenderLinked)
{
    gTest = "loaded_by_the_variable_it_starts_once";
    auto inserted = RunStandIn(standIn, dylib, nullptr, {}, true);
    CHECK(inserted.Exited && inserted.Code == 0 && inserted.Said == "stand-in: loaded\n");
    CHECK(!inserted.Log.empty() && inserted.Log.front() == LoadedLine() && Count(inserted.Log, LoadedLine()) == 1);
    CHECK(AnyHas(inserted.Log, "[INFO ] loaded through DYLD_INSERT_LIBRARIES from "));
    if (gFailures) Show(inserted);

    // Loaded, not inserted: nothing is said of the variable.
    auto loaded = RunStandIn(standIn, dylib, nullptr, {}, false);
    CHECK(!AnyHas(loaded.Log, "DYLD_INSERT_LIBRARIES"));

    std::string dir = TempDir("copy");
    std::string copy = dir + "/libbg3mse.dylib";
    {
        std::ifstream from(dylib, std::ios::binary);
        std::ofstream to(copy, std::ios::binary);
        to << from.rdbuf();
    }
    gInsertInstead = copy;
    for (char const* conf : { (char const*)nullptr, "script_extender = 1\n" }) {
        bool on = conf && extenderLinked;
        auto twice = RunStandIn(standIn, dylib, conf, {}, true, on);
        // The Objective-C runtime says that both copies define the image's
        // classes; nothing else may be said.
        std::string said = twice.Said;
        for (size_t at; (at = said.find("objc[")) != std::string::npos;) said.erase(at, said.find('\n', at) + 1 - at);
        CHECK(twice.Exited && twice.Code == 0 && said == "stand-in: loaded\n");
        CHECK(Count(twice.Log, LoadedLine()) == 1);
        // The inserted copy is the first: it is the one that starts.
        char real[PATH_MAX];
        CHECK(realpath(copy.c_str(), real) && AnyHas(twice.Log, (std::string("loaded through DYLD_INSERT_LIBRARIES from ") + real).c_str()));
        if (on) {
            size_t refusals = 0;
            for (auto const& line : twice.Log) refusals += line.find("Script Extender not started") != std::string::npos;
            CHECK(refusals == 1 && twice.Alerts.size() == 1);
        }
        if (gFailures) Show(twice);
    }
    gInsertInstead.clear();
    unlink(copy.c_str());
    rmdir(dir.c_str());
}

// --- 3: loaded into the game's crash reporter -------------------------------------

void the_games_crash_reporter_is_left_alone(char const* standIn, char const* dylib)
{
    gTest = "the_games_crash_reporter_is_left_alone";
    constexpr char kOn[] = "script_extender = 1\n";

    // The game's own test: any argument equal to -crash, whatever its case.
    std::vector<std::vector<char const*>> reporters = { { "-crash" }, { "-CRASH" }, { "-Crash" }, { "-windowed", "-crash" }, { "-crash", "-windowed" } };
    for (auto const& arguments : reporters) {
        for (bool inserted : { false, true }) {
            auto run = RunStandIn(standIn, dylib, kOn, arguments, inserted);
            CHECK(run.Exited && run.Code == 0 && run.Said == "stand-in: loaded\n");
            CHECK(run.Log.empty());
            CHECK(Left(run).empty());
            // Nothing is shown in the crash reporter, whatever would be refused in the game.
            CHECK(run.Alerts.empty());
            if (!run.Log.empty() || !Left(run).empty() || !run.Alerts.empty()) Show(run);
        }
    }

    // Anything else is the game: what only resembles the argument, and other arguments.
    std::vector<std::vector<char const*>> games = { {}, { "-crashes" }, { "crash" }, { "--crash" }, { "-crash=1" }, { "-modded", "-stats" } };
    for (auto const& arguments : games) {
        for (bool inserted : { false, true }) {
            auto run = RunStandIn(standIn, dylib, nullptr, arguments, inserted);
            CHECK(run.Exited && run.Code == 0 && run.Said == "stand-in: loaded\n");
            CHECK(!run.Log.empty() && run.Log.front() == LoadedLine());
            if (run.Log.empty()) Show(run);
        }
    }
}

}

void the_menu_line_names_the_build()
{
    gTest = "the_menu_line_names_the_build";
    namespace version = bg3se::mac::version;
    auto line = version::MenuVersionLine(34, "Nov 15 2026 10:00:00");
    CHECK(line == std::string("\r\nbg3mse ") + version::kVersion
        + " loaded (Script Extender v34, built on Nov 15 2026 10:00:00).");
    CHECK(line.find("Script Extender") != std::string::npos);
}

int main(int argc, char** argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: bg3mse_dylib_tests <libbg3mse.dylib> <stand-in executable> <extender linked: 0|1>\n");
        return 2;
    }
    // Built next to the stand-in.
    gAlertRecorder = argv[2];
    gAlertRecorder.replace(gAlertRecorder.rfind('/') + 1, std::string::npos, "libbg3mse_alert_recorder.dylib");
    loading_the_image_does_nothing(argv[1]);
    the_setting_decides_whether_the_extender_is_started(argv[2], argv[1], strcmp(argv[3], "1") == 0);
    the_setting_decides_whether_the_log_takes_debug_lines(argv[2], argv[1]);
    the_games_crash_reporter_is_left_alone(argv[2], argv[1]);
    loaded_by_the_variable_it_starts_once(argv[2], argv[1], strcmp(argv[3], "1") == 0);
    the_native_api_is_exported_and_does_nothing_without_an_extender(argv[1], strcmp(argv[3], "1") == 0);
    the_menu_line_names_the_build();
    printf("7 tests, %d failures\n", gFailures);
    return gFailures ? 1 : 0;
}
