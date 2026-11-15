#include <stdafx.h>
#if BG3MSE_EXTENDER
#include <Binding/Startup.h>
#endif
#if BG3MSE_GFX
#include <Gfx/Gfx.h>
#include <Release/GfxMark.h>
#endif
#include <Platform/Config.h>
#include <Platform/Log.h>
#include <Platform/Process.h>
#include <Release/Version.h>
#include <Shaders/ShaderProvision.h>

#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <sys/stat.h>

// What the logging macros name. Not `using namespace bg3se;`: at namespace
// scope that would make "std" name two namespaces (Layout/StdVariant.h).
using bg3se::Debug;
using bg3se::DebugMessageType;

#if BG3MSE_GFX
// This image starts the graphics module (StartInGame, below), and says so to
// bg3mse-install, which reads this section from the file (Release/GfxMark.h).
__attribute__((used, section(BG3MSE_GFX_SEGMENT "," BG3MSE_GFX_SECTION))) static char const kGfxMark[] = BG3MSE_GFX_MARK;
#endif

namespace bg3se::mac {

// Called by the image's initializer (Mac/Entry.cpp).
void StartInGame() noexcept;

}

// What libbg3mse.dylib does when the game loads it. The game loads it through
// the updater's loader (Mac/Updater/Loader.cpp), or directly; the game loads
// either through a load command the installer adds, or because a launch
// option names it in DYLD_INSERT_LIBRARIES (Platform/Process.h). Either way
// this runs before the game's main(). It is called from the image's
// initializer (Mac/Entry.cpp), after every other initializer of the image.

namespace {

// Loads a separately built module that sits next to this dylib, for its own
// constructor. A module that is not installed is skipped without a word.
void LoadModule(char const* file)
{
    Dl_info self{};
    if (!dladdr(reinterpret_cast<void const*>(&LoadModule), &self) || !self.dli_fname) {
        ERR("cannot find where libbg3mse.dylib was loaded from; %s not loaded", file);
        return;
    }
    std::string path = self.dli_fname;
    size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        ERR("libbg3mse.dylib was loaded as '%s', not by its path; %s not loaded", self.dli_fname, file);
        return;
    }
    path.replace(slash + 1, std::string::npos, file);

    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        if (errno != ENOENT) ERR("%s: %s", path.c_str(), strerror(errno));
        return;
    }
    if (!dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL)) {
        char const* why = dlerror();
        ERR("%s did not load: %s", path.c_str(), why ? why : "unknown error");
        return;
    }
    INFO("loaded %s", path.c_str());
}

void Initialize()
{
    if (getenv("BG3MSE_DISABLE") || !bg3se::mac::IsGameProcess()) return;
    // A second copy of this library in the process (the load command and
    // the variable naming two files) leaves everything to the first.
    if (!bg3se::mac::FirstCopyOfThisLibrary(reinterpret_cast<void const*>(&LoadModule))) return;
    Dl_info self{};
    bool inserted = dladdr(reinterpret_cast<void const*>(&LoadModule), &self) && self.dli_fname
        && bg3se::mac::InsertedByEnvironment(self.dli_fname);
    // Inserted, or opened by an image that was, this image is initialized
    // before the game's own libraries: they are initialized first, as they
    // are when it is the game's last. Where they already are, nothing runs.
    unsigned initialized = bg3se::mac::InitializeLibrariesLoadedBefore(reinterpret_cast<void const*>(&LoadModule));

    // Without a home directory there is no log file: the log stays on stderr.
    std::string support = bg3se::mac::DataDirectory();
    bg3se::mac::SetLogDebug(bg3se::mac::LogDebugWanted());
    if (!support.empty()) {
        mkdir(support.c_str(), 0755);
        bg3se::mac::OpenLogFile(support + "/logs");
    }

    namespace version = bg3se::mac::version;
    INFO("bg3mse %s loaded (commit %.12s, extender %s, %s)", version::kVersion, version::kCommit,
        version::kExtenderVersion, version::kChannel);
    if (inserted) {
        INFO("loaded through DYLD_INSERT_LIBRARIES from %s; the %u libraries loaded with the game were initialized first",
            self.dli_fname, initialized);
    }
    // So that a DEBUG line missing from the log means it was not written.
    DEBUG("the log takes DEBUG lines (log_debug in bg3mse.conf, or BG3MSE_LOG_DEBUG)");
#if BG3MSE_EXTENDER
    // Norbyte's Script Extender, only when bg3mse.conf turns it on. What it
    // needs of the game is read from the game's code as it was shipped, so
    // that comes first, before anything below puts a hook on the game; it
    // changes nothing. A refusal is in the log, and the rest goes on without
    // the extender.
    namespace binding = bg3se::mac::binding;
    bool extender = bg3se::mac::FeatureEnabled(bg3se::mac::kScriptExtenderSetting);
    auto refusal = binding::StartRefusal::None;
    if (extender) {
        INFO("Script Extender: turned on in bg3mse.conf (%s)", bg3se::mac::kScriptExtenderSetting);
        refusal = binding::PrepareScriptExtender();
        extender = refusal == binding::StartRefusal::None;
    }
#endif
    bg3se::mac::StartShaderProvisioning();
    LoadModule("libbg3mse-camera.dylib");     // built with BG3MSE_CAMERA=ON; it logs to logs/camera_*.log
#if BG3MSE_GFX
    bg3se::mac::gfx::StartGfx();     // bg3mse.conf's gfx_* keys decide the rest
#endif
#if BG3MSE_EXTENDER
    // The rest of the extender's start-up: this layer's hooks on the game,
    // then Norbyte's SetupScriptExtender. It must be done before the game's
    // CoreLibSDM::Init runs, whose return is where Norbyte's start-up goes
    // on; here the game's main() has not begun.
    if (extender) refusal = binding::StartScriptExtender();
    if (extender && refusal == binding::StartRefusal::None) {
        INFO("Script Extender: started; it goes on when the game's core library is initialised");
    }
    // A refusal that concerns the player is told in a message box, as Windows
    // tells a game it does not support; the game goes on without the extender.
    binding::ShowRefusalNotice(refusal);
#endif
}

}

// An exception leaving an initializer of the image would end the game before it starts.
void bg3se::mac::StartInGame() noexcept
{
    try {
        Initialize();
    } catch (std::exception const& e) {
        ERR("start-up failed: %s", e.what());
    } catch (...) {
        ERR("start-up failed");
    }
}
