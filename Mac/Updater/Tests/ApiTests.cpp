// The loader's API as the extender binds it (BG3Extender/Extender/Shared/
// UpdaterAPI.inl and BG3Updater/ExtenderAPI.inl): the loader is found by
// its file name through the shim's GetModuleHandleW, and its functions by
// GetProcAddress, as Norbyte's code does on macOS.
//
//   bg3mse_updater_api_tests <libbg3mse-loader-test.dylib> <scratch directory>
//
// And, for bg3mse_updater_tests (UpdaterTests.cpp, ToolApi), a tool outside
// the game that runs an update through the API, as a tool does with
// Norbyte's DWrite.dll:
//
//   bg3mse_updater_api_tests --tool --loader= --home= --bin= --url= --key=

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <string>
#include <sys/stat.h>

namespace {

using InitializeLibraryProc = void();
using SetLogCallbackProc = void(void (*)(int, char const*));
using InitializeProc = void(char const*);
using ShowConsoleProc = void();
using SetGameVersionProc = void(int32_t, int32_t, int32_t, int32_t);
using UpdateProc = bool();
using GetTextProc = void(char*, uint32_t*);
using GetErrorFlagsProc = uint32_t();
using GetResourceVersionProc = bool(int32_t*, int32_t*, int32_t*, int32_t*);
using ShutdownProc = void();

std::atomic<int> gToolLines{ 0 };

int Tool(std::map<std::string, std::string> const& o)
{
    setenv("HOME", o.at("home").c_str(), 1);
    setenv("BG3MSE_UPDATER_TEST_KEY", o.at("key").c_str(), 1);
    void* h = dlopen(o.at("loader").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) return 100;
    auto sym = [&](char const* name) { return dlsym(h, name); };
    auto initLibrary = (InitializeLibraryProc*)sym("SEUpdaterInitializeLibrary");
    auto setLogCallback = (SetLogCallbackProc*)sym("SEUpdaterSetLogCallback");
    auto initialize = (InitializeProc*)sym("SEUpdaterInitialize");
    auto showConsole = (ShowConsoleProc*)sym("SEUpdaterShowConsole");
    auto setGameVersion = (SetGameVersionProc*)sym("SESetGameVersion");
    auto update = (UpdateProc*)sym("SEUpdate");
    auto getError = (GetTextProc*)sym("SEUpdaterGetError");
    auto getLog = (GetTextProc*)sym("SEUpdaterGetLog");
    auto getErrorFlags = (GetErrorFlagsProc*)sym("SEUpdaterGetErrorFlags");
    auto getResourceVersion = (GetResourceVersionProc*)sym("SEUpdaterGetResourceVersion");
    auto shutdownUpdater = (ShutdownProc*)sym("SEUpdaterShutdown");
    auto test = (ShutdownProc*)sym("SEUpdaterTest");
    if (!initLibrary || !setLogCallback || !initialize || !showConsole || !setGameVersion || !update || !getError || !getLog
        || !getErrorFlags || !getResourceVersion || !shutdownUpdater || !test) {
        return 101;
    }
    int failures = 0;
    auto check = [&](bool ok, char const* what) {
        if (!ok) {
            printf("tool: FAIL %s\n", what);
            failures++;
        }
    };
    auto text = [](GetTextProc* get) {
        uint32_t length = 0;
        get(nullptr, &length);
        std::string t(length, '\0');
        get(t.data(), &length);
        return t;
    };
    auto version = [&] {
        int32_t v[4] = { -1, -1, -1, -1 };
        if (!getResourceVersion(&v[0], &v[1], &v[2], &v[3])) return std::string("none");
        return std::to_string(v[0]) + "." + std::to_string(v[1]) + "." + std::to_string(v[2]) + "." + std::to_string(v[3]);
    };
    std::string config = o.at("home") + "/Library/Application Support/bg3mse/ScriptExtenderUpdaterConfig.json";
    auto writeConfig = [&](std::string const& url) {
        std::ofstream(config, std::ios::trunc) << "{ \"ManifestURL\": \"" << url << "\" }\n";
    };

    check(!update() && getErrorFlags() == 0 && version() == "none", "before SEUpdaterInitialize nothing is there");
    initLibrary();
    setLogCallback([](int, char const*) { gToolLines++; });
    writeConfig(o.at("url"));
    initialize(o.at("bin").c_str());
    check(version() == "0.1.0.0", "the build beside the bundle's loader would be loaded");
    check(!update(), "SEUpdate: nothing to show");
    check(getErrorFlags() == 0 && text(getError).empty(), "no error");
    check(version() == "0.2.0.0", "the build fetched would be loaded");
    std::string log = text(getLog);
    check(log.find("update: Script Extender 0.2.0 downloaded and checked") != std::string::npos, "SEUpdaterGetLog has the update");
    check(log.find("\r\n") != std::string::npos && gToolLines > 0, "the callback is given the lines");
    check(log.find("fetch: GET") == std::string::npos, "no trace without the console");
    showConsole();
    check(!update(), "SEUpdate again: nothing to show");
    check(text(getLog).find("fetch: GET " + o.at("url") + "Release/Manifest.json") != std::string::npos, "the console's Debug traces");
    shutdownUpdater();
    check(getErrorFlags() == 0 && version() == "none", "SEUpdaterShutdown ends it");

    // A game version no build is for (SESetGameVersion).
    initialize(o.at("bin").c_str());
    setGameVersion(4, 0, 0, 0);
    check(update(), "SEUpdate: an error to show");
    check(getErrorFlags() == 1, "an error, not critical: a build is there");
    check(text(getError).find("supports this game version yet (v4.0.0.0)") != std::string::npos, "the error says why");
    shutdownUpdater();

    // The network down.
    writeConfig("http://127.0.0.1:1/channel-");
    initialize(o.at("bin").c_str());
    check(update() && getErrorFlags() == 1, "SEUpdate: a failure to show");
    check(text(getError).starts_with("bg3mse update failed. Check your internet connection.\nUnable to fetch manifest"), "the failure, told");
    shutdownUpdater();

    // SEUpdaterTest: his sequence, here with the game's version unknown.
    test();
    check(getErrorFlags() == 0, "SEUpdaterTest leaves nothing behind");
    return failures;
}

}

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--tool") == 0) {
        std::map<std::string, std::string> o;
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            size_t eq = a.find('=');
            if (a.starts_with("--") && eq != std::string::npos) o[a.substr(2, eq - 2)] = a.substr(eq + 1);
        }
        for (char const* key : { "loader", "home", "bin", "url", "key" }) {
            if (!o.count(key)) return 2;
        }
        return Tool(o);
    }
    if (argc != 3) return 2;
    // The name UpdaterAPI.inl looks for.
    mkdir(argv[2], 0755);
    std::string path = std::string(argv[2]) + "/libbg3mse-loader.dylib";
    {
        std::ifstream in(argv[1], std::ios::binary);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << in.rdbuf();
    }
    int failures = 0;
    auto check = [&](bool ok, char const* what) {
        if (!ok) {
            printf("FAIL %s\n", what);
            failures++;
        }
    };
    check(GetModuleHandleW(L"libbg3mse-loader.dylib") == nullptr, "nothing is found before the loader is loaded");
    // Outside the game its initializer does nothing; its API answers all the same.
    check(dlopen(path.c_str(), RTLD_NOW) != nullptr, "the loader loads");
    HMODULE module = GetModuleHandleW(L"libbg3mse-loader.dylib");
    check(module != nullptr, "GetModuleHandleW finds the loader by its file name");
    using GetVersionProc = uint32_t();
    using GetErrorFlagsProc = uint32_t();
    using GetErrorProc = void(char*, uint32_t*);
    using SetCapabilitiesProc = void(uint32_t);
    auto getVersion = (GetVersionProc*)(void*)GetProcAddress(module, "SEUpdaterGetVersion");
    auto getErrorFlags = (GetErrorFlagsProc*)(void*)GetProcAddress(module, "SEUpdaterGetErrorFlags");
    auto getError = (GetErrorProc*)(void*)GetProcAddress(module, "SEUpdaterGetError");
    auto setCapabilities = (SetCapabilitiesProc*)(void*)GetProcAddress(module, "SEUpdaterSetCapabilities");
    check(getVersion && getErrorFlags && getError && setCapabilities, "GetProcAddress finds the API");
    check(GetProcAddress(module, "StartLoader") == nullptr, "nothing else is exported");
    if (getVersion) check(getVersion() == 7, "SEUpdaterGetVersion is VerInitialAPI (7)");
    if (setCapabilities) setCapabilities(1);
    if (getErrorFlags) check(getErrorFlags() == 0, "no error to show");
    if (getError) {
        uint32_t length = 99;
        getError(nullptr, &length);
        check(length == 0, "an empty error text");
    }
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
