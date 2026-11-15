#include <Installer/Launch.h>

#include <Installer/MachO.h>
#include <Platform/Config.h>

#include <CoreFoundation/CoreFoundation.h>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <string_view>
#include <strings.h>
#include <vector>
#include <unistd.h>

extern char** environ;

namespace bg3se::mac::installer {

namespace {

constexpr char kLoaderFile[] = "libbg3mse-loader.dylib";

}

std::string LaunchedLibrary(std::string const& directory, bool loaderThere)
{
    return directory + "/" + (loaderThere ? kLoaderFile : "libbg3mse.dylib");
}

std::string BundleExecutable(std::string const& app, std::string& problem)
{
    std::string executable;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<UInt8 const*>(app.data()),
        (CFIndex)app.size(), true);
    // CFBundle takes any directory for a bundle; an application has an Info.plist.
    bool application = access((app + "/Contents/Info.plist").c_str(), F_OK) == 0;
    CFBundleRef bundle = url && application ? CFBundleCreate(nullptr, url) : nullptr;
    CFURLRef found = bundle ? CFBundleCopyExecutableURL(bundle) : nullptr;
    char path[PATH_MAX];
    if (found && CFURLGetFileSystemRepresentation(found, true, reinterpret_cast<UInt8*>(path), sizeof(path))) executable = path;
    if (found) CFRelease(found);
    if (bundle) CFRelease(bundle);
    if (url) CFRelease(url);
    if (executable.empty()) problem = app + " is not an application bundle with an executable";
    return executable;
}

LaunchPlan PlanLaunch(std::string const& dylib, std::optional<std::span<uint8_t const>> file, char const* current)
{
    LaunchPlan plan;
    if (!file) {
        plan.Why = dylib + " is not there";
        return plan;
    }
    std::string problem;
    auto macho = ReadMachO(*file, problem);
    if (!macho) {
        plan.Why = dylib + " is not a Mach-O file: " + problem;
        return plan;
    }
    auto arm64 = macho->Find(CPU_TYPE_ARM64);
    if (!arm64) {
        plan.Why = dylib + " has no arm64 slice";
        return plan;
    }
    if (arm64->FileType != MH_DYLIB) {
        plan.Why = dylib + " is not a library";
        return plan;
    }
    std::string_view now = current ? current : "";
    std::string_view rest = now;
    while (!rest.empty()) {
        size_t colon = rest.find(':');
        if (rest.substr(0, colon) == dylib) {
            plan.Insert = std::string(now);
            return plan;
        }
        if (colon == std::string_view::npos) break;
        rest.remove_prefix(colon + 1);
    }
    plan.Insert = now.empty() ? dylib : std::string(now) + ":" + dylib;
    return plan;
}

bool TranslatesShaders(std::string_view value)
{
    if (FlagValue(value)) return true;
    return value.size() == 3 && strncasecmp(value.data(), "all", 3) == 0;
}

ShaderRead ReadShadersBeforeLaunch(std::string const& tool, std::string const& app, ReadNotice* notice, double noticeAfterSeconds,
    double timeoutSeconds)
{
    ShaderRead read;
    if (access(tool.c_str(), X_OK) != 0) {
        read.Why = tool + " is not there";
        return read;
    }
    std::vector<char*> argv = { const_cast<char*>(tool.c_str()), const_cast<char*>(kShaderInterfaceVerb), const_cast<char*>(app.c_str()), nullptr };
    std::vector<char*> env;
    for (char** e = environ; e && *e; e++) {
        if (strncmp(*e, "DYLD_", 5) != 0) env.push_back(*e);
    }
    env.push_back(nullptr);

    // What it says goes nowhere: the file it leaves is what the game reads. Nothing else of the launcher's
    // goes with it.
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT);
    pid_t pid = 0;
    int error = posix_spawn(&pid, tool.c_str(), &actions, &attributes, argv.data(), env.data());
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0) {
        read.Why = tool + " cannot be started: " + strerror(error);
        return read;
    }

    auto start = std::chrono::steady_clock::now();
    auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    bool shown = false;
    // Asked to end, and ended when it does not within two seconds; not waited for beyond two more. Nothing of
    // it may keep the game from starting.
    auto stop = [&](ShaderRead::Ended how, std::string why) {
        bool gone = false;
        for (int with : { SIGTERM, SIGKILL }) {
            kill(pid, with);
            for (int i = 0; i < 100 && !gone; i++) {
                int status = 0;
                pid_t done = waitpid(pid, &status, WNOHANG);
                gone = done == pid || (done < 0 && errno != EINTR);
                if (!gone) usleep(20 * 1000);
            }
            if (gone) break;
        }
        read.How = how;
        read.Why = std::move(why);
    };
    for (;;) {
        int status = 0;
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                // One that ended before the notice was due read nothing: the kept file was the game's.
                read.How = shown ? ShaderRead::Ended::Read : ShaderRead::Ended::AtOnce;
            } else {
                read.How = ShaderRead::Ended::Failed;
                read.Why = WIFEXITED(status) ? std::string(kShaderInterfaceVerb) + " ended with " + std::to_string(WEXITSTATUS(status))
                                             : std::string(kShaderInterfaceVerb) + " was ended by signal " + std::to_string(WTERMSIG(status));
            }
            break;
        }
        if (done < 0 && errno != EINTR) {
            read.How = ShaderRead::Ended::Failed;
            read.Why = std::string("waiting for it: ") + strerror(errno);
            break;
        }
        if (seconds() >= timeoutSeconds) {
            stop(ShaderRead::Ended::TimedOut, "not done after " + std::to_string((long)timeoutSeconds) + " seconds");
            break;
        }
        if (!shown && seconds() >= noticeAfterSeconds) {
            shown = true;
            if (notice) notice->Show();
        }
        if (shown && notice) {
            if (notice->StartNowPressed()) {
                stop(ShaderRead::Ended::StartedWithout, "the game was asked for at once");
                break;
            }
        } else {
            usleep(20 * 1000);
        }
    }
    if (shown && notice) notice->Hide();
    read.Seconds = seconds();
    return read;
}

std::string Describe(ShaderRead const& read)
{
    switch (read.How) {
    case ShaderRead::Ended::AtOnce:
        return {};
    case ShaderRead::Ended::Read:
        return "the game's Metal functions were read for mods' shaders in " + std::to_string((long)(read.Seconds + 0.5)) + " seconds";
    default:
        return "the game's Metal functions were not read before the start (" + read.Why + "); the game reads them itself";
    }
}

std::string LaunchOption(std::string const& launcher)
{
    return "\"" + launcher + "\" %command%";
}

std::string LaunchLogPath(std::string const& dataDirectory)
{
    return dataDirectory.empty() ? std::string() : dataDirectory + "/logs/" + kLaunchLog;
}

void AppendLaunchLog(std::string const& path, std::string const& text)
{
    if (path.empty()) return;
    std::string dir = path.substr(0, path.rfind('/'));
    // The data directory is there once bg3mse is installed; logs/ is made when missing.
    std::string data = dir.substr(0, dir.rfind('/'));
    struct stat st{};
    if (stat(data.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return;
    mkdir(dir.c_str(), 0755);
    if (stat(path.c_str(), &st) == 0 && (size_t)st.st_size >= kLaunchLogLimit) rename(path.c_str(), (path + ".1").c_str());

    time_t now = time(nullptr);
    struct tm local{};
    localtime_r(&now, &local);
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S %z", &local);
    std::string line = std::string(stamp) + " [" + std::to_string(getpid()) + "] " + text;
    for (auto& c : line) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    line += "\n";
    int fd = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    // One write: lines of two starts at once do not interleave.
    ssize_t written;
    do {
        written = write(fd, line.data(), line.size());
    } while (written < 0 && errno == EINTR);
    close(fd);
}

std::vector<std::string> LastLaunch(std::string const& path)
{
    std::vector<std::string> lines;
    FILE* f = path.empty() ? nullptr : fopen(path.c_str(), "r");
    if (!f) return lines;
    std::vector<std::string> all;
    char buffer[4096];
    std::string line;
    while (fgets(buffer, sizeof(buffer), f)) {
        line += buffer;
        if (line.ends_with("\n")) {
            line.pop_back();
            all.push_back(line);
            line.clear();
        }
    }
    if (!line.empty()) all.push_back(line);
    fclose(f);
    auto pid = [](std::string const& l) {
        size_t open = l.find(" ["), close = l.find("] ");
        return open == std::string::npos || close == std::string::npos || close < open ? std::string() : l.substr(open, close - open);
    };
    // The last start is the last line that says what it was given, with what followed it.
    size_t start = all.size();
    for (size_t i = all.size(); i-- > 0;) {
        if (all[i].find("] given ") != std::string::npos) {
            start = i;
            break;
        }
    }
    if (start == all.size()) {
        if (!all.empty()) lines.push_back(all.back());
        return lines;
    }
    std::string id = pid(all[start]);
    for (size_t i = start; i < all.size(); i++) {
        if (pid(all[i]) == id) lines.push_back(all[i]);
    }
    return lines;
}

std::string LaunchedGame(std::vector<std::string> const& lines)
{
    for (auto const& l : lines) {
        size_t at = l.find("; game \"");
        if (at == std::string::npos) continue;
        at += 8;
        size_t end = l.find("\"; ", at);
        if (end == std::string::npos) end = l.rfind('"');
        if (end != std::string::npos && end > at) return l.substr(at, end - at);
    }
    return {};
}

}
