// libbg3mse-loader.dylib: what the game loads (Mac/Updater/Updater.h), the
// Mac form of Norbyte's DWrite.dll (BG3Updater/dllmain.cpp). Its initializer
// runs before the game's main(). It runs the update first, as Norbyte's
// holds the game while it runs, so that a build it fetches is the one
// loaded; then it picks a build and loads it, still before main(), so the
// extender starts when it did when the game loaded libbg3mse.dylib itself. While the update runs it shows his progress window
// (Notice.h), and when it ends with something to say that the extender
// cannot say in the game, his box.
//
// It exports Norbyte's updater API (BG3Updater/Exports.def, API.cpp): the
// extender finds this image by name and shows its message in the main menu
// (BG3Extender/Extender/Shared/UpdaterAPI.inl), and a tool outside the game
// can load it and run an update with it (SEUpdaterInitialize, SEUpdate, ...).

#include <Platform/Config.h>
#include <Platform/Log.h>
#include <Platform/Process.h>
#include <Release/Version.h>
#include <Updater/Notice.h>
#include <Updater/Updater.h>

#if !BG3MSE_SELF_UPDATE
// A release that does not update itself (BG3MSE_SELF_UPDATE=OFF): no key and
// no channel, and its Fetch (FetchNone.cpp) opens nothing.
#define BG3MSE_UPDATER_URL ""
#elif BG3MSE_UPDATER_TEST_KEY
// The tests' loader (Mac/Updater/Updater.cmake): the key of the test that runs it.
#define BG3MSE_UPDATER_URL ""
#else
// BG3MSE_UPDATER_KEY (128 hexadecimal digits, or empty) and BG3MSE_UPDATER_URL.
#include BG3MSE_UPDATER_KEY_HEADER
#endif

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <cstring>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mutex>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#if BG3MSE_UPDATER_TEST_KEY
#include <cstdio>
#endif

using namespace bg3se::mac;
using namespace bg3se::mac::updater;

namespace bg3se::mac::updater {
void StartLoader() noexcept;
}

namespace {

// Norbyte's UpdaterAPICapability and UpdaterAPIErrorFlags (BG3Updater/ExtenderAPI.h).
constexpr uint32_t kCapErrorReporting = 1u << 0;
constexpr uint32_t kErrShowError = 1u << 0, kErrIsCritical = 1u << 1;

struct State
{
    std::mutex Lock;
    Environment Env;
    Choice Loaded;
    std::optional<Outcome> Result;
    std::string Text;
    std::string LoadError;      // no build could be loaded: Norbyte's critical error
    uint32_t Capabilities{ 0 };
    std::atomic<bool> Cancel{ false };
    // Made by SEUpdaterInitialize in a process that is not the game: no
    // thread of its own, nothing loaded; SEUpdate runs the update.
    bool Tool{ false };
    std::optional<Version> GameVersionOverride;
};

State* gState = nullptr;

// The extender this loader loaded, for native modules: Norbyte's
// SEUpdaterGetModule and SEUpdaterNotifyWhenLoaded (BG3Updater/Updater.h,
// GetLoadedExtenderModule and AddCompletionCallback). What a module is given
// is what it gives dlsym, a handle of dlopen, as on Windows it is the HMODULE
// it gives GetProcAddress; the extender's own two functions of these names
// give the same (Platform/Process.h, NativeModuleHandle).
//
// Apart from the loader's state, and made when first asked for: a module
// loaded before the loader may ask from its own initializer, before the
// loader's has run, and is then told when the extender is loaded.
using LoadCompletionCallbackProc = void(void* module, void* userData);

struct LoadedModule
{
    std::mutex Lock;
    void* Handle{ nullptr };
    std::vector<std::pair<LoadCompletionCallbackProc*, void*>> Waiting;
};

LoadedModule& TheLoadedModule()
{
    static auto* module = new LoadedModule;
    return *module;
}

// The extender is loaded, and its initializers have run: the modules that
// asked are told, each once, with no lock held.
void ExtenderLoaded(void* handle)
{
    if (!handle) return;
    auto& module = TheLoadedModule();
    std::vector<std::pair<LoadCompletionCallbackProc*, void*>> waiting;
    {
        std::lock_guard guard(module.Lock);
        module.Handle = handle;
        waiting.swap(module.Waiting);
    }
    for (auto const& [callback, userData] : waiting) callback(handle, userData);
}

// SEUpdaterSetLogCallback's function, and SEUpdaterGetLog's text: every
// line the updater logs, as Norbyte's UpdaterConsole::Print collects it.
using LogCallbackProc = void(int type, char const* message);
// Never destroyed: threads of the process may log while it exits.
std::atomic<LogCallbackProc*> gLogCallback{ nullptr };
std::mutex& gLogTextLock = *new std::mutex;
std::string& gLogText = *new std::string;

void CollectLine(LogLevel level, char const* text)
{
    {
        std::lock_guard guard(gLogTextLock);
        gLogText += text;
        gLogText += "\r\n";
    }
    // LogLevel has DebugMessageType's values.
    if (auto callback = gLogCallback.load()) callback(static_cast<int>(level), text);
}

// --- what the player sees -------------------------------------------------------

#if BG3MSE_UPDATER_TEST_KEY
// The tests' loader shows nothing. With BG3MSE_UPDATER_TEST_UI it writes,
// one to a line, what it would show to that file, and takes the file
// <that file>.cancel appearing for the player's Cancel.
class Recorded : public NoticeDisplay
{
public:
    explicit Recorded(std::string path) : path_(std::move(path)) {}

    bool OpenProgress(std::string const& status) override
    {
        Write("open " + status);
        return true;
    }
    void UpdateProgress(std::string const& status) override { Write("status " + status); }
    bool WaitProgress() override
    {
        struct stat st;
        for (;;) {
            if (closed_) return false;
            if (stat((path_ + ".cancel").c_str(), &st) == 0) {
                Write("cancel");
                return true;
            }
            usleep(20000);
        }
    }
    void CloseProgress() override
    {
        closed_ = true;
        Write("close");
    }
    void Box(std::string const& title, std::string const& text, bool warning) override
    {
        std::string line = text;
        for (char& c : line) {
            if (c == '\n') c = '|';
        }
        Write(std::string(warning ? "warning " : "error ") + title + ": " + line);
    }

private:
    void Write(std::string const& line)
    {
        std::lock_guard guard(lock_);
        if (FILE* f = fopen(path_.c_str(), "a")) {
            fprintf(f, "%s\n", line.c_str());
            fclose(f);
        }
    }

    std::string path_;
    std::mutex lock_;
    std::atomic<bool> closed_{ false };
};

class Nothing : public NoticeDisplay
{
public:
    bool OpenProgress(std::string const&) override { return false; }
    void UpdateProgress(std::string const&) override {}
    bool WaitProgress() override { return false; }
    void CloseProgress() override {}
    void Box(std::string const&, std::string const&, bool) override {}
};
#endif

// Never destroyed: a box's thread may still be showing it while the process exits.
NoticeDisplay& Display()
{
    static NoticeDisplay* display = [] () -> NoticeDisplay* {
#if BG3MSE_UPDATER_TEST_KEY
        if (char const* path = getenv("BG3MSE_UPDATER_TEST_UI")) return new Recorded(path);
        return new Nothing;
#else
        return SystemDisplay().release();
#endif
    }();
    return *display;
}

// Norbyte's GameHelpers::ShowError: a box, from a thread of its own.
void ShowErrorBox(std::string text, bool warning)
{
    std::thread([text = std::move(text), warning] { Display().Box(kErrorBoxTitle, text, warning); }).detach();
}

// Norbyte's Fail() (CoreLib/Utils.cpp): the error in the log, a box, and
// the process ends, without its exit handlers (TerminateProcess).
[[noreturn]] void Fail(std::string const& reason)
{
    Log(LogLevel::Error, "%s", reason.c_str());
    Display().Box(kFailBoxTitle, reason, false);
    _exit(1);
}

// Shift held when the game starts: Norbyte's updater sets Debug
// (GetAsyncKeyState(VK_SHIFT) in ScriptExtenderUpdater::LoadConfig). The
// state of the login session's keyboard, as the shim's GetAsyncKeyState has it.
bool ShiftHeld()
{
#if BG3MSE_UPDATER_TEST_KEY
    if (getenv("BG3MSE_UPDATER_TEST_SHIFT")) return true;
#endif
    return (CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState) & kCGEventFlagMaskShift) != 0;
}

// Debug (ScriptExtenderUpdater::InitConsole and the fetcher's DebugLogging):
// DEBUG lines, the log on stderr as well (the console), every transfer traced.
void ApplyDebug(Environment& env)
{
    SetLogDebug(env.Settings.Debug || LogDebugWanted());
    SetLogEcho(env.Settings.Debug);
    env.Fetching.Trace = env.Settings.Debug;
    env.Fetching.IPv4Only = env.Settings.IPv4Only;
}

// --- the loader -------------------------------------------------------------------

std::optional<Version> BundleVersion(CFBundleRef bundle)
{
    if (!bundle) return {};
    CFTypeRef value = CFBundleGetValueForInfoDictionaryKey(bundle, kCFBundleVersionKey);
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return {};
    char text[64];
    if (!CFStringGetCString((CFStringRef)value, text, sizeof(text), kCFStringEncodingUTF8)) return {};
    return Version::Parse(text);
}

std::optional<Version> GameVersion()
{
    return BundleVersion(CFBundleGetMainBundle());
}

// The version of the bundle that holds `path` (the bundle, or a directory in it).
std::optional<Version> GameVersionAt(std::string path)
{
    while (!path.empty()) {
        if (path.ends_with(".app")) {
            CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, reinterpret_cast<UInt8 const*>(path.data()),
                static_cast<CFIndex>(path.size()), true);
            if (!url) return {};
            CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, url);
            CFRelease(url);
            auto version = BundleVersion(bundle);
            if (bundle) CFRelease(bundle);
            return version;
        }
        size_t slash = path.rfind('/');
        if (slash == std::string::npos) break;
        path.resize(slash);
    }
    return {};
}

std::optional<Version> SystemVersion()
{
    char text[64] = {};
    size_t size = sizeof(text) - 1;
    if (sysctlbyname("kern.osproductversion", text, &size, nullptr, 0) != 0) return {};
    return Version::Parse(text);
}

std::string OwnDirectory()
{
    Dl_info self{};
    if (!dladdr(reinterpret_cast<void const*>(&OwnDirectory), &self) || !self.dli_fname) return {};
    std::string path = self.dli_fname;
    size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// An image other than this one that carries bg3mse's version stamp: the
// extender (the camera module, which carries one too, is only ever loaded by it).
std::string LoadedExtender()
{
    Dl_info self{};
    if (!dladdr(reinterpret_cast<void const*>(&LoadedExtender), &self)) return {};
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        auto const* header = reinterpret_cast<mach_header_64 const*>(_dyld_get_image_header(i));
        if (!header || header == self.dli_fbase) continue;
        unsigned long size = 0;
        if (getsectiondata(header, "__TEXT", "__bg3mse_version", &size) && size) {
            char const* name = _dyld_get_image_name(i);
            return name ? name : "an image";
        }
    }
    return {};
}

PublicKey const* BuiltInKey()
{
    static std::optional<PublicKey> key = [] () -> std::optional<PublicKey> {
#if !BG3MSE_SELF_UPDATE
        char const* text = "";
#elif BG3MSE_UPDATER_TEST_KEY
        char const* text = getenv("BG3MSE_UPDATER_TEST_KEY");
        if (!text) text = "";
#else
        char const* text = BG3MSE_UPDATER_KEY;
#endif
        PublicKey parsed;
        if (*text && FromHex(text, parsed)) return parsed;
        return {};
    }();
    return key ? &*key : nullptr;
}

// The settings, the key and the platform, for the game or for a tool.
void Prepare(State& state, std::string const& data)
{
    state.Env.Cancel = &state.Cancel;
    std::string failure;
    state.Env.Settings = LoadConfig(data, BG3MSE_UPDATER_URL, failure);
    if (!failure.empty()) Fail(failure);
    if (ShiftHeld()) state.Env.Settings.Debug = true;
    ApplyDebug(state.Env);
    if (auto key = BuiltInKey()) state.Env.Key = *key;
    Log(LogLevel::Debug, "Update channel: %s; Debug %s, IPv4 only %s", state.Env.Settings.UpdateChannel.c_str(),
        state.Env.Settings.Debug ? "on" : "off", state.Env.Settings.IPv4Only ? "on" : "off");
#if BG3MSE_SELF_UPDATE
    if (!state.Env.Key) Log(LogLevel::Info, "this build has no update key: updates are off");
#else
    Log(LogLevel::Info, "this bg3mse does not update itself: the build beside the loader is loaded, nothing is fetched");
#endif
}

void LogOutcome(Outcome const& outcome, Choice const& loaded, Environment const& env)
{
    if (!outcome.Staged.empty()) {
        Log(LogLevel::Info, "update: Script Extender %s downloaded and checked",
            outcome.Staged.c_str());
    } else if (outcome.Cancelled) {
        Log(LogLevel::Info, "update: cancelled; %s is kept", loaded.Path.c_str());
    } else if (outcome.What != Category::None) {
        Log(LogLevel::Error, "update failed, %s is kept: %s", loaded.Path.c_str(), outcome.Message.c_str());
    } else if (!outcome.Message.empty()) {
        Log(LogLevel::Info, "update: %s", outcome.Message.c_str());
    } else {
        Log(LogLevel::Info, "update: %s", env.Settings.DisableUpdates ? "DisableUpdates is set" : "the loaded build is the newest for this game");
    }
}

#if BG3MSE_SELF_UPDATE
// Norbyte's updater holds the game at SDL_Init while it fetches, and then
// loads the build it chose: an update is used by the start that fetched it.
// The extender has to be loaded before the game's main() (Mac/Main.cpp), so
// here the update runs in the loader's initializer, before anything is
// loaded: the game waits at most the fetch limits (Updater.h), and Cancel in
// the progress window ends the wait.
Outcome UpdateBeforeLoad(State* state)
{
    ProgressNotice notice(Display(), [state] {
        Log(LogLevel::Info, "update: cancelled by the player");
        state->Cancel = true;
    });
    Environment env = state->Env;
    env.Status = [&notice](std::string const& status) { notice.SetStatus(status); };
    notice.Show("Initializing");
    Outcome outcome = Update(env, state->Loaded);
    notice.Hide();
    LogOutcome(outcome, state->Loaded, env);
    return outcome;
}
#endif

// Norbyte's ScriptExtenderUpdater::Run, once the build is loaded or none
// could be: an error the extender cannot show in the game (critical, or no
// extender that reports errors) is a box.
void ReportUpdate(State* state, Outcome const& outcome)
{
    std::string box;
    bool critical = false;
    {
        std::lock_guard guard(state->Lock);
        state->Result = outcome;
        state->Text = DisplayText(outcome, state->Loaded);
        critical = !state->LoadError.empty();
        box = critical ? state->LoadError : state->Text;
        if (!box.empty() && !critical && (state->Capabilities & kCapErrorReporting)) box.clear();
    }
    if (!box.empty()) ShowErrorBox(box, !critical);
}

void Start()
{
    if (getenv("BG3MSE_DISABLE") || !IsGameProcess()) return;
    std::string data = DataDirectory();
    if (!data.empty()) {
        mkdir(data.c_str(), 0755);
        OpenLogFile(data + "/logs", "updater");
    }
    SetLogSink(&CollectLine);
    Log(LogLevel::Info, "bg3mse loader %s (commit %.12s)", version::kVersion, version::kCommit);

    auto* state = new State;
    Prepare(*state, data);

    // libbg3mse.dylib loaded already, by another way in (the game's own load
    // command naming it, say): a second copy is never loaded.
    if (std::string other = LoadedExtender(); !other.empty()) {
        Log(LogLevel::Warning, "%s is loaded already, not through the loader: the loader loads nothing and fetches nothing", other.c_str());
        delete state;
        // It is the extender all the same, for a module that asks the loader.
        ExtenderLoaded(dlopen(other.c_str(), RTLD_NOW | RTLD_NOLOAD));
        return;
    }

    auto game = GameVersion();
    auto system = SystemVersion();
    std::string own = OwnDirectory();
    state->Env.BundledLibrary = own + "/" + kLibrary;
    bool known = game && system && !own.empty();
    if (known) {
        state->Env.Here.Game = *game;
        state->Env.Here.MacOS = *system;
        state->Loaded = ChooseBuild(state->Env);
    } else {
        state->Loaded.Path = state->Env.BundledLibrary;
        state->Loaded.Note = "the game's version or the system's cannot be read";
        state->Env.Key.reset();     // nothing is fetched for a game that is not known
    }
#if BG3MSE_SELF_UPDATE
    Outcome outcome = UpdateBeforeLoad(state);
    if (known) state->Loaded = ChooseBuild(state->Env);
#else
    Outcome outcome;
    state->Loaded.Note.clear();     // the cache is never used: said above
#endif
    Choice& c = state->Loaded;
    if (!c.Note.empty()) Log(LogLevel::Info, "cache not used: %s", c.Note.c_str());

    // Before the extender is loaded: its initializers call the API below.
    gState = state;
    void* extender = nullptr;
    if (c.Path.empty()) {
        // No build fits this game: Norbyte's updater loads nothing then.
        std::string text = DisplayText(outcome, c);
        Log(LogLevel::Error, "no build of bg3mse is for game %s: nothing is loaded", game ? game->ToString().c_str() : "?");
        std::lock_guard guard(state->Lock);
        state->LoadError = text;
    } else {
        Log(LogLevel::Info, "loading %s (%s %s) for game %s on macOS %s", c.Path.c_str(), c.Cached ? "cached" : "beside the loader",
            c.Loaded.Parts.empty() ? "of unknown version" : c.Loaded.ToString().c_str(),
            game ? game->ToString().c_str() : "?", system ? system->ToString().c_str() : "?");
        extender = dlopen(c.Path.c_str(), RTLD_NOW);
    }
    if (!extender && !c.Path.empty()) {
        char const* why = dlerror();
        std::string error = why ? why : "unknown error";
        Log(LogLevel::Error, "%s did not load: %s", c.Path.c_str(), error.c_str());
        if (c.Cached && !c.NoFallback.empty()) {
            // The build beside the loader is not for this game either: nothing is loaded, as when none is for it.
            Log(LogLevel::Error, "the build beside the loader is not for game %s either: nothing is loaded", game ? game->ToString().c_str() : "?");
            std::lock_guard guard(state->Lock);
            state->LoadError = "bg3mse failed to load.\n" + error + "\n" + c.NoFallback;
        } else if (c.Cached) {
            Choice bundled;
            bundled.Path = state->Env.BundledLibrary;
            c = bundled;
            extender = dlopen(c.Path.c_str(), RTLD_NOW);
            if (!extender) {
                why = dlerror();
                error = why ? why : "unknown error";
                Log(LogLevel::Error, "%s did not load either: %s", c.Path.c_str(), error.c_str());
                std::lock_guard guard(state->Lock);
                state->LoadError = "bg3mse failed to load.\n" + error;
            }
        } else {
            std::lock_guard guard(state->Lock);
            state->LoadError = "bg3mse failed to load.\n" + error;
        }
    }
    ExtenderLoaded(extender);
    ReportUpdate(state, outcome);
}

}

// Called by the image's initializer (Mac/Updater/LoaderEntry.cpp). Lets nothing escape.
void bg3se::mac::updater::StartLoader() noexcept
{
    try {
        Start();
    } catch (std::exception const& e) {
        Log(LogLevel::Error, "loader failed: %s", e.what());
    } catch (...) {
        Log(LogLevel::Error, "loader failed");
    }
}

// Norbyte's updater API (BG3Updater/ExtenderAPI.h, API.cpp), all fourteen of
// his exports but DWriteCreateFactory, which on Windows is what makes the
// game load DWrite.dll.
//
// In the game the loader's state exists from its start; the extender calls
// SEUpdaterGetVersion, SetCapabilities, GetErrorFlags and GetError. A tool
// that loads this image calls SEUpdaterInitialize, then SEUpdate, as with
// Norbyte's DWrite.dll; in a tool's process the image's start does nothing,
// and SEUpdaterInitialize makes the state.
extern "C" {

__attribute__((visibility("default"))) void SEUpdaterInitializeLibrary()
{
    // Norbyte's makes his console object (GameHelpers) so that
    // SEUpdaterSetLogCallback has one to give the callback to. The log here
    // needs nothing made: lines reach the callback from the first.
    SetLogSink(&CollectLine);
}

__attribute__((visibility("default"))) uint32_t SEUpdaterGetVersion()
{
    // Called once, when the extender binds this API (UpdaterAPI::Init).
    Log(LogLevel::Info, "the extender found the updater API");
    return 7;   // UpdaterAPIVersion::VerInitialAPI
}

__attribute__((visibility("default"))) void SEUpdaterSetLogCallback(LogCallbackProc* callback)
{
    gLogCallback = callback;
    SetLogSink(&CollectLine);
}

// `binPath`: on Windows the game's bin directory, which has the settings
// file and bg3.exe; here the game's bundle or a directory in it (where
// libbg3mse.dylib is), whose version is the game's. The settings are read
// from bg3mse's data directory, where the game's loader reads them.
// Nothing (nullptr): the bundle of this process, and the loader's own
// directory.
__attribute__((visibility("default"))) void SEUpdaterInitialize(char const* binPath)
{
    if (gState) return;
    SetLogSink(&CollectLine);
    // A tool's output is its own: lines go to the callback and SEUpdaterGetLog,
    // and to stderr once SEUpdaterShowConsole asks for the console.
    SetLogStream(nullptr);
    auto* state = new State;
    state->Tool = true;
    std::string data = DataDirectory();
    if (!data.empty()) mkdir(data.c_str(), 0755);
    Prepare(*state, data);
    std::string bin = binPath && *binPath ? std::string(binPath) : OwnDirectory();
    if (bin.ends_with(".app")) bin += "/Contents/MacOS";
    state->Env.BundledLibrary = bin + "/" + kLibrary;
    auto game = binPath && *binPath ? GameVersionAt(binPath) : GameVersion();
    if (game) state->Env.Here.Game = *game;
    if (auto system = SystemVersion()) state->Env.Here.MacOS = *system;
    gState = state;
}

__attribute__((visibility("default"))) void SEUpdaterShowConsole()
{
    if (!gState) return;
    std::lock_guard guard(gState->Lock);
    gState->Env.Settings.Debug = true;
    if (gState->Tool) SetLogStream(stderr);
    ApplyDebug(gState->Env);
}

__attribute__((visibility("default"))) void SESetGameVersion(int32_t major, int32_t minor, int32_t revision, int32_t build)
{
    if (!gState) return;
    std::lock_guard guard(gState->Lock);
    Version v;
    for (int32_t part : { major, minor, revision, build }) v.Parts.push_back(static_cast<uint32_t>(part));
    gState->GameVersionOverride = v;
    gState->Env.Here.Game = v;
}

// Norbyte's: FetchUpdates (fetch, update the cache, pick the build to load)
// and whether there is something to show. Nothing is loaded.
__attribute__((visibility("default"))) bool SEUpdate()
{
    if (!gState) return false;
    State& s = *gState;
    Environment env;
    Choice before;
    {
        std::lock_guard guard(s.Lock);
        env = s.Env;
        before = s.Tool ? ChooseBuild(env) : s.Loaded;
    }
    env.Status = nullptr;
    Outcome outcome = Update(env, before);
    LogOutcome(outcome, before, env);
    Choice after = s.Tool ? ChooseBuild(env) : before;
    std::string text = DisplayText(outcome, after);
    Log(LogLevel::Debug, "Update result: %s", text.empty() ? "no error text" : text.c_str());
    std::lock_guard guard(s.Lock);
    if (s.Tool) s.Loaded = after;
    s.Result = outcome;
    s.Text = text;
    return !text.empty();
}

__attribute__((visibility("default"))) uint32_t SEUpdaterGetErrorFlags()
{
    if (!gState) return 0;
    std::lock_guard guard(gState->Lock);
    std::string text = !gState->LoadError.empty() ? gState->LoadError : gState->Result ? gState->Text : gState->Loaded.Problem;
    uint32_t flags = text.empty() ? 0 : kErrShowError;
    // Critical: no build is loaded (in the game), or none can be (a tool).
    struct stat st;
    bool critical = !gState->LoadError.empty() || (gState->Tool && stat(gState->Loaded.Path.c_str(), &st) != 0 && gState->Result);
    if (critical) flags |= kErrShowError | kErrIsCritical;
    return flags;
}

__attribute__((visibility("default"))) void SEUpdaterGetError(char* buf, uint32_t* length)
{
    if (!length) return;
    if (!gState) {
        *length = 0;
        return;
    }
    std::lock_guard guard(gState->Lock);
    std::string const& text = !gState->LoadError.empty() ? gState->LoadError : gState->Result ? gState->Text : gState->Loaded.Problem;
    if (buf) memcpy(buf, text.data(), std::min<size_t>(*length, text.size()));
    *length = (uint32_t)text.size();
}

__attribute__((visibility("default"))) void SEUpdaterSetCapabilities(uint32_t capabilities)
{
    if (!gState) return;
    std::lock_guard guard(gState->Lock);
    gState->Capabilities = capabilities;
}

__attribute__((visibility("default"))) void SEUpdaterGetLog(char* buf, uint32_t* length)
{
    if (!length) return;
    std::lock_guard guard(gLogTextLock);
    if (buf) memcpy(buf, gLogText.data(), std::min<size_t>(*length, gLogText.size()));
    *length = (uint32_t)gLogText.size();
}

// The version of the build that is loaded (in the game) or would be (a tool).
__attribute__((visibility("default"))) bool SEUpdaterGetResourceVersion(int32_t* major, int32_t* minor, int32_t* revision, int32_t* build)
{
    if (!gState) return false;
    Version v;
    {
        std::lock_guard guard(gState->Lock);
        if (gState->Tool) gState->Loaded = ChooseBuild(gState->Env);
        v = gState->Loaded.Loaded;
    }
    if (v.Parts.empty()) return false;
    auto part = [&](size_t i) { return i < v.Parts.size() ? (int32_t)v.Parts[i] : 0; };
    if (major) *major = part(0);
    if (minor) *minor = part(1);
    if (revision) *revision = part(2);
    if (build) *build = part(3);
    return true;
}

// The extender this loader loaded (or found loaded), or null: in a tool's
// process, before it is loaded, and when none could be.
__attribute__((visibility("default"))) void* SEUpdaterGetModule()
{
    auto& module = TheLoadedModule();
    std::lock_guard guard(module.Lock);
    return module.Handle;
}

// Calls `callback` with the extender's module once it is loaded: now, when
// it is. One that never loads is never told.
__attribute__((visibility("default"))) void SEUpdaterNotifyWhenLoaded(LoadCompletionCallbackProc* callback, void* userData)
{
    if (!callback) return;
    auto& module = TheLoadedModule();
    void* handle = nullptr;
    {
        std::lock_guard guard(module.Lock);
        handle = module.Handle;
        if (!handle) module.Waiting.emplace_back(callback, userData);
    }
    if (handle) callback(handle, userData);
}

__attribute__((visibility("default"))) void SEUpdaterShutdown()
{
    if (!gState) return;
    if (!gState->Tool) {
        gState->Cancel = true;
        return;
    }
    // A tool's state goes, as Norbyte's gUpdater does.
    delete gState;
    gState = nullptr;
}

// Norbyte's: an update with the console shown, for a developer. His sets a
// Windows game version; here the game's own is used.
__attribute__((visibility("default"))) void SEUpdaterTest()
{
    SEUpdaterInitialize(nullptr);
    SEUpdaterShowConsole();
    SEUpdate();
    SEUpdaterShutdown();
}

}
