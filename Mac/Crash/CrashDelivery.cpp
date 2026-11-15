// What happens to a crash report once it is written
// (crash::LaunchCrashReporter, Crash/CrashReporter.h), for Norbyte's
// extender: by the owner's decision it stays on disk and the log says where.
// No program is started and nothing is sent. The report's path is noted
// beside it, and the next start of the game tells the user what Windows shows
// in CrashReporter.exe's dialog (TakeCrashNotice).
//
// LaunchCrashReporter runs under the rules of the time of a crash: no lock,
// no allocation; it calls only what may be called from a signal handler.

#include <Crash/CrashReporter.h>
#include <Platform/Log.h>

#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace bg3se::mac::crash {

namespace {

// Notes the report's path in its directory, for the next start.
void NoteForNextStart(char const* reportPath)
{
    char const* slash = strrchr(reportPath, '/');
    if (!slash) return;
    size_t directory = (size_t)(slash - reportPath);
    char note[PATH_MAX];
    if (directory + 1 + sizeof(kCrashNoticeFile) > sizeof(note)) return;
    memcpy(note, reportPath, directory);
    note[directory] = '/';
    memcpy(note + directory + 1, kCrashNoticeFile, sizeof(kCrashNoticeFile));

    int fd = open(note, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    size_t left = strlen(reportPath);
    char const* at = reportPath;
    while (left > 0) {
        ssize_t wrote = write(fd, at, left);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            break;
        }
        at += wrote;
        left -= (size_t)wrote;
    }
    close(fd);
}

bool ReadFile(std::string const& path, std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream all;
    all << in.rdbuf();
    text = all.str();
    return true;
}

}

void LaunchCrashReporter(char const* reportPath)
{
    constexpr char kSaid[] = "crash report written to ";
    char line[1024];
    size_t length = sizeof(kSaid) - 1;
    memcpy(line, kSaid, length);
    size_t path = strlen(reportPath);
    if (path > sizeof(line) - 1 - length) path = sizeof(line) - 1 - length;
    memcpy(line + length, reportPath, path);
    line[length + path] = '\0';
    LogFromFaultHandler(LogLevel::Error, line);
    NoteForNextStart(reportPath);
}

std::optional<std::string> TakeCrashNotice(std::string const& directory)
{
    std::string note = directory + "/" + kCrashNoticeFile;
    std::string report;
    if (!ReadFile(note, report)) return {};
    // Told once, whatever follows.
    unlink(note.c_str());

    struct stat info{};
    if (report.empty() || stat(report.c_str(), &info) != 0) return {};
    // The report is not sent.
    std::string notice = "The game crashed last time. Crash report:\n";
    notice += report;

    // The backtrace CrashReporter.exe shows, from the report's .bt beside it.
    std::string backtrace;
    std::string_view suffix = kReportSuffix;
    if (report.size() > suffix.size() && report.compare(report.size() - suffix.size(), suffix.size(), suffix) == 0
        && ReadFile(report.substr(0, report.size() - suffix.size()) + kBacktraceSuffix, backtrace)) {
        // Only the crashing thread's frames, and few of them: the .bt goes on
        // with the Lua backtrace and every other thread, and an alert that
        // long runs off the screen with its button.
        constexpr size_t kLines = 12;
        size_t end = 0, lines = 0;
        while (end < backtrace.size() && lines < kLines) {
            size_t next = backtrace.find('\n', end);
            size_t stop = next == std::string::npos ? backtrace.size() : next;
            size_t length = stop - end;
            if (length != 0 && backtrace[stop - 1] == '\r') length--;
            if (length == 0) break;
            end = next == std::string::npos ? backtrace.size() : next + 1;
            lines++;
        }
        backtrace.resize(end);
        while (!backtrace.empty() && (backtrace.back() == '\n' || backtrace.back() == '\r')) backtrace.pop_back();
        if (!backtrace.empty()) notice += "\n\nBacktrace:\n" + backtrace;
    }
    return notice;
}

}
