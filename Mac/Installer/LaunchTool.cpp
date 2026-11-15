// bg3mse-launch <game> [arguments]...: what Steam runs when the game's launch
// option is the one bg3mse-install launch-option prints
// (Mac/Installer/Launch.h). The game is its executable or, as Steam on macOS
// gives it, its application bundle, whose executable Info.plist names. Names libbg3mse.dylib, from the launcher's own
// directory, in DYLD_INSERT_LIBRARIES when it can be loaded, and replaces
// itself with the game. It changes no file but its log, launcher.log in
// bg3mse's logs folder, which says for every start what it was given and
// what it inserted, or why not: Steam shows nothing of a launcher that fails.
// Unless bg3mse.conf switches the translation of mods' shaders off
// (shader_translation), it first has bg3mse-install read the game's own
// Metal functions when a mod needs them and the ones kept are not this
// game's, and shows a notice while that takes long.

#include <Installer/Installer.h>
#include <Installer/Launch.h>
#include <Installer/Setup.h>
#include <Platform/Config.h>
#include <Release/Version.h>

#include <CoreFoundation/CoreFoundation.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace bg3se::mac::installer;

namespace {

// The notice, shown by the system on the launcher's behalf: the launcher has no window and no run loop.
class SystemNotice : public ReadNotice
{
public:
    ~SystemNotice() override { Hide(); }

    void Show() override
    {
        CFStringRef heading = CFStringCreateWithCString(nullptr, kReadNoticeHeading, kCFStringEncodingUTF8);
        CFStringRef text = CFStringCreateWithCString(nullptr, kReadNoticeText, kCFStringEncodingUTF8);
        CFStringRef button = CFStringCreateWithCString(nullptr, kReadNoticeButton, kCFStringEncodingUTF8);
        void const* keys[] = { kCFUserNotificationAlertHeaderKey, kCFUserNotificationAlertMessageKey,
            kCFUserNotificationProgressIndicatorValueKey, kCFUserNotificationDefaultButtonTitleKey };
        void const* values[] = { heading, text, kCFBooleanTrue, button };
        CFDictionaryRef dictionary = CFDictionaryCreate(nullptr, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        SInt32 error = 0;
        note_ = CFUserNotificationCreate(nullptr, 0, kCFUserNotificationPlainAlertLevel, &error, dictionary);
        CFRelease(dictionary);
        CFRelease(heading);
        CFRelease(text);
        CFRelease(button);
    }

    bool StartNowPressed() override
    {
        if (!note_) {
            usleep(50 * 1000);
            return false;
        }
        CFOptionFlags response = 0;
        if (CFUserNotificationReceiveResponse(note_, 0.05, &response) != 0) return false;
        return (response & 0x3) == kCFUserNotificationDefaultResponse;
    }

    void Hide() override
    {
        if (!note_) return;
        CFUserNotificationCancel(note_);
        CFRelease(note_);
        note_ = nullptr;
    }

private:
    CFUserNotificationRef note_{ nullptr };
};

std::string LauncherDirectory()
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    char resolved[PATH_MAX];
    if (!realpath(path.c_str(), resolved)) return {};
    std::string dir = resolved;
    return dir.substr(0, dir.rfind('/'));
}

}

int main(int argc, char** argv)
{
    std::string log = LaunchLogPath(bg3se::mac::DataDirectory());
    std::string self = std::string(" (") + kLauncher + " " + bg3se::mac::version::kVersion + ")";
    if (argc < 2) {
        AppendLaunchLog(log, std::string("given nothing to start: the launch option has no %command% after the launcher") + self);
        fprintf(stderr, "usage: %s <game executable or application> [arguments]...\n"
                        "As the game's launch option in Steam: \"<path of %s>\" %%command%%\n", kLauncher, kLauncher);
        return 2;
    }
    std::string given = std::string("given \"") + argv[1] + "\"";
    if (argc > 2) given += " and " + std::to_string(argc - 2) + (argc == 3 ? " more argument" : " more arguments");

    // Steam on macOS puts the application bundle where %command% is.
    std::string executable = argv[1];
    struct stat named{};
    if (stat(argv[1], &named) == 0 && S_ISDIR(named.st_mode)) {
        std::string problem;
        executable = BundleExecutable(argv[1], problem);
        if (executable.empty()) {
            AppendLaunchLog(log, given + self);
            AppendLaunchLog(log, "cannot start the game: " + problem);
            fprintf(stderr, "%s: %s\n", kLauncher, problem.c_str());
            return 127;
        }
    }
    argv[1] = executable.data();
    given += "; game \"" + executable + "\"";

    std::string dir = LauncherDirectory();
    // The updater's loader when it is there, which opens the newest build of bg3mse itself.
    std::string loader = LaunchedLibrary(dir, true);
    std::string dylib = LaunchedLibrary(dir, access(loader.c_str(), F_OK) == 0);
    std::optional<std::span<uint8_t const>> file;
    void* mapped = MAP_FAILED;
    size_t size = 0;
    int fd = dir.empty() ? -1 : open(dylib.c_str(), O_RDONLY);
    struct stat st{};
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0) {
        size = (size_t)st.st_size;
        mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped != MAP_FAILED) file = std::span<uint8_t const>(static_cast<uint8_t const*>(mapped), size);
    }
    if (fd >= 0) close(fd);

    auto plan = PlanLaunch(dylib, file, getenv("DYLD_INSERT_LIBRARIES"));
    if (mapped != MAP_FAILED) munmap(mapped, size);
    if (!plan.Insert.empty()) {
        setenv("DYLD_INSERT_LIBRARIES", plan.Insert.c_str(), 1);
        AppendLaunchLog(log, given + "; inserted \"" + dylib + "\"" + self);
    } else {
        AppendLaunchLog(log, given + "; not inserted: " + plan.Why + "; the game starts without bg3mse" + self);
        fprintf(stderr, "%s: %s; the game starts without bg3mse\n", kLauncher, plan.Why.c_str());
    }

    // The game's own Metal functions, for mods' shaders: read now, while the game is not there to slow the
    // read down, when a mod needs them and what is kept is not this game's (Launch.h): the tool finds that
    // out, and ends at once otherwise. Not with the key there and off. Only for a game that starts with
    // bg3mse, with the library inserted.
    auto translation = bg3se::mac::ConfValue(bg3se::mac::kShaderTranslationSetting);
    std::string app = BundleOf(executable);
    if (!plan.Insert.empty() && !app.empty() && (!translation || TranslatesShaders(*translation))) {
        SystemNotice notice;
        auto read = ReadShadersBeforeLaunch(dir + "/" + kInstallTool, app, &notice);
        if (auto said = Describe(read); !said.empty()) AppendLaunchLog(log, said);
    }

    execv(argv[1], argv + 1);
    int error = errno;
    AppendLaunchLog(log, std::string("cannot start the game: ") + argv[1] + ": " + strerror(error));
    fprintf(stderr, "%s: %s: %s\n", kLauncher, argv[1], strerror(error));
    return 127;
}
