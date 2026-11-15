// The updater's tests (Mac/Updater/Updater.h), all offline: a server on
// 127.0.0.1 (Server.h) serves manifests and zips signed with a key made here
// and thrown away. Each case checks which build is left in use.
//
// Part one drives the updater as the loader does, in this process. Part two
// starts a stand-in game with the tests' loader inserted, as the game loads
// the real one, and checks what it loads, and that its main() is not held
// up by a slow or absent server.

#include "Server.h"

#include <Installer/Installer.h>
#include <Platform/Log.h>
#include <Updater/Files.h>
#include <Updater/Signature.h>
#include <Updater/Stamp.h>
#include <Updater/Updater.h>
#include <Updater/Zip.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <map>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <thread>
#include <cstring>
#include <climits>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

using namespace bg3se::mac::updater;
using namespace bg3se::mac::updater::test;
using Clock = std::chrono::steady_clock;

extern char** environ;

namespace {

int gFailures = 0;
int gChecks = 0;
std::string gCase;

#define CHECK(cond, ...) do { \
    gChecks++; \
    if (!(cond)) { \
        gFailures++; \
        fprintf(stderr, "FAIL [%s] %s:%d: %s: ", gCase.c_str(), __FILE__, __LINE__, #cond); \
        fprintf(stderr, " " __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

struct Args
{
    std::string Self, Scratch, Loader, OfflineLoader, StandIn, Launcher, CMake, ZipScript, ApiTests;
    std::map<std::string, std::string> Fakes;
} gArgs;

Server gServer;
PublicKey gPublic;
PrivateKey gPrivate;
PublicKey gOtherPublic;
PrivateKey gOtherPrivate;

constexpr char kGame[] = "4.1.1.7631656";

std::span<uint8_t const> Bytes(std::string const& s)
{
    return { reinterpret_cast<uint8_t const*>(s.data()), s.size() };
}

std::string ReadAll(std::string const& path)
{
    std::string text;
    ReadFile(path, text, 1ull << 32);
    return text;
}

void WriteAll(std::string const& path, std::string const& text)
{
    std::string why;
    MakeDirectories(path.substr(0, path.rfind('/')), why);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        exit(2);
    }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

bool Exists(std::string const& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

std::string Signed(std::string const& json, PrivateKey const& key = gPrivate)
{
    std::vector<uint8_t> out;
    if (!Sign(Bytes(json), key, out)) {
        fprintf(stderr, "cannot sign\n");
        exit(2);
    }
    return std::string(out.begin(), out.end());
}

// A release as the release target makes it: bg3mse-<v>/ with the
// libraries, a licence, and manifest.json; zipped by Mac/Release/Zip.cmake.
struct Built
{
    std::string Zip;
    std::string ManifestJson;
    Release Entry;
};

std::map<std::string, Built> gReleases;

Built const& MakeRelease(std::string const& version)
{
    if (auto it = gReleases.find(version); it != gReleases.end()) return it->second;
    std::string root = gArgs.Scratch + "/releases";
    std::string dir = root + "/bg3mse-" + version;
    RemoveTree(dir);
    std::string why;
    MakeDirectories(dir + "/camera", why);
    MakeDirectories(dir + "/licenses", why);
    WriteAll(dir + "/libbg3mse.dylib", ReadAll(gArgs.Fakes.at(version)));
    WriteAll(dir + "/camera/libbg3mse-camera.dylib", "camera module " + version + "\n");
    WriteAll(dir + "/licenses/bg3mse.txt", "licence\n");
    // A name with a space, and an executable that is no image.
    WriteAll(dir + "/Install bg3mse.command", "#!/bin/sh\n");
    chmod((dir + "/Install bg3mse.command").c_str(), 0755);
    std::string files;
    for (std::string path : { "Install bg3mse.command", "camera/libbg3mse-camera.dylib", "libbg3mse.dylib", "licenses/bg3mse.txt" }) {
        Digest d;
        uint64_t size = 0;
        Sha256File(dir + "/" + path, d, &size);
        if (!files.empty()) files += ",";
        files += "{\"path\":\"" + path + "\",\"size\":" + std::to_string(size) + ",\"sha256\":\"" + Hex(d) + "\"}";
    }
    std::string manifest = "{\"name\":\"bg3mse-" + version + "\",\"version\":\"" + version
        + "\",\"commit\":\"0000000000000000000000000000000000000000\",\"script_extender_version\":\"34.0.0.0\","
          "\"channel\":\"Release\",\"game_build_checked\":\"" + kGame + "\",\"macos_minimum\":\"11.0\","
          "\"architecture\":\"arm64\",\"files\":[" + files + "]}\n";
    WriteAll(dir + "/manifest.json", manifest);
    std::string zip = root + "/bg3mse-" + version + ".zip";
    std::string command = "'" + gArgs.CMake + "' -DROOT='" + root + "' -DENTRY=bg3mse-" + version + " -DOUTPUT='" + zip
        + "' -P '" + gArgs.ZipScript + "'";
    if (system(command.c_str()) != 0) {
        fprintf(stderr, "Zip.cmake failed: %s\n", command.c_str());
        exit(2);
    }
    Built b;
    b.Zip = ReadAll(zip);
    b.ManifestJson = manifest;
    b.Entry.ReleaseVersion = *Version::Parse(version);
    b.Entry.Commit = "0000000000000000000000000000000000000000";
    b.Entry.URL = "../v" + version + "/bg3mse-" + version + ".zip";
    b.Entry.Size = b.Zip.size();
    b.Entry.Digest = Hex(Sha256(Bytes(b.Zip)));
    b.Entry.ManifestDigest = Hex(Sha256(Bytes(manifest)));
    b.Entry.GameBuildChecked = kGame;
    b.Entry.MacOSMinimum = *Version::Parse("11.0");
    b.Entry.Architecture = "arm64";
    b.Entry.BuildDate = 1762900000;
    return gReleases[version] = std::move(b);
}

std::string ManifestJson(std::string const& channel, uint64_t sequence, std::vector<Release> const& releases)
{
    ChannelManifest m;
    m.Channel = channel;
    m.Sequence = sequence;
    m.Releases = releases;
    return StringifyChannelManifest(m);
}

std::string ManifestPath(std::string const& channel = "Release")
{
    return "/channel-" + channel + "/Manifest.json";
}

void Serve(std::string const& version, Route::Mode how = Route::Mode::Normal)
{
    Built const& b = MakeRelease(version);
    Route r;
    r.How = how;
    r.Body = b.Zip;
    gServer.Set("/v" + version + "/bg3mse-" + version + ".zip", r);
}

void ServeManifest(std::string const& body, std::string const& channel = "Release")
{
    Route r;
    r.Body = body;
    gServer.Set(ManifestPath(channel), r);
}

// A case's own directory: the cache, and the "bundle" with the build beside the loader (0.1.0).
Environment MakeEnvironment(std::string const& name)
{
    gCase = name;
    gServer.Clear();
    std::string dir = gArgs.Scratch + "/cases/" + name;
    RemoveTree(dir);
    WriteAll(dir + "/MacOS/libbg3mse.dylib", ReadAll(gArgs.Fakes.at("0.1.0")));
    Environment env;
    env.Settings.ManifestURL = gServer.URL("/channel-");
    env.Settings.CachePath = dir + "/Cache";
    env.Key = gPublic;
    env.Here.Game = *Version::Parse(kGame);
    env.Here.MacOS = *Version::Parse("15.0");
    env.BundledLibrary = dir + "/MacOS/libbg3mse.dylib";
    env.ManifestLimits = { 2.0, 1.0, 4u << 20 };
    env.PackageLimits = { 5.0, 1.0, 0 };
    return env;
}

std::string CachedManifest(Environment const& env)
{
    return ReadAll(ManifestCachePath(env.Settings));
}

bool Cached(Environment const& env, std::string const& version)
{
    return Exists(BuildsDirectory(env.Settings) + "/" + MakeRelease(version).Entry.CacheName());
}

// What the loader would load now.
std::string Chosen(Environment const& env)
{
    Choice c = ChooseBuild(env);
    Stamp s;
    std::string why;
    if (!ReadStamp(c.Path, s, why)) return "unreadable: " + why;
    return s.Version + (c.Cached ? " cached" : " bundled");
}

// --- Part one: the updater in this process ------------------------------------------

void Basics()
{
    gCase = "basics";
    CHECK(Version::Parse("0.2.0")->Compare(*Version::Parse("0.10.0")) < 0);
    CHECK(*Version::Parse("4.1.1") == *Version::Parse("4.1.1.0"));
    CHECK(!Version::Parse("4.1.x"));
    CHECK(!Version::Parse(""));
    CHECK(!Version::Parse("1..2"));

    std::string text = "{\"x\":1}";
    std::string s = Signed(text);
    std::span<uint8_t const> payload;
    std::string why;
    CHECK(Verify(Bytes(s), gPublic, payload, why), "%s", why.c_str());
    CHECK(std::string(payload.begin(), payload.end()) == text);
    CHECK(!Verify(Bytes(s), gOtherPublic, payload, why));
    for (size_t at : { (size_t)0, s.size() - 260, s.size() - 200, s.size() - 1 }) {
        std::string t = s;
        t[at] ^= 1;
        CHECK(!Verify(Bytes(t), gPublic, payload, why), "a byte changed at %zu still verifies", at);
    }
    CHECK(!Verify(Bytes(text), gPublic, payload, why));
    CHECK(!Verify(Bytes(s.substr(1)), gPublic, payload, why));

    // The footer is Norbyte's PackageSignature: 'BGS1', then the signature.
    CHECK(s.size() == text.size() + 260);
    CHECK(memcmp(s.data() + text.size(), "1SGB", 4) == 0);
    CHECK((uint8_t)s[text.size() + 68] == 1);

    Stamp stamp;
    CHECK(ReadStamp(gArgs.Fakes.at("0.2.0"), stamp, why) && stamp.Version == "0.2.0", "%s", why.c_str());
    CHECK(!ReadStamp(gArgs.StandIn, stamp, why));

    CHECK(ResolveURL("https://github.com/o/r/releases/download/channel-Release/Manifest.json", "../v0.2.0/bg3mse-0.2.0.zip")
        == "https://github.com/o/r/releases/download/v0.2.0/bg3mse-0.2.0.zip");
    CHECK(ResolveURL("https://h/a/b", "file:///etc/passwd").empty());
    CHECK(ResolveURL("https://h/a/b", "https://other/x") == "https://other/x");

    // Norbyte's ConfigGet: a key of another type changes nothing, without a word.
    Config c;
    CHECK(ApplyConfig("{\"UpdateChannel\":\"Nightly\",\"TargetVersion\":\"0.2.0\",\"DisableUpdates\":true,\"IPv4Only\":7,\"Debug\":\"yes\"}", c));
    CHECK(c.UpdateChannel == "Nightly" && c.TargetVersion && c.DisableUpdates && !c.IPv4Only && !c.Debug);
    CHECK(ApplyConfig("{\"IPv4Only\":true,\"TargetVersion\":\"x\"}", c) && c.IPv4Only && !c.TargetVersion);
    // What is not a JSON object is refused whole, nothing applied.
    for (char const* text : { "", "{", "{\"UpdateChannel\":\"Devel\",}x", "[1]", "nonsense" }) {
        Config d;
        CHECK(!ApplyConfig(text, d) && d.UpdateChannel == "Release", "'%s' is taken", text);
    }
    // LoadConfig: no file is the defaults; a damaged one is Fail()'s text.
    std::string dir = gArgs.Scratch + "/cases/config";
    RemoveTree(dir);
    std::string failure;
    Config defaults = LoadConfig(dir, "https://x/channel-", failure);
    CHECK(failure.empty() && defaults.ManifestURL == "https://x/channel-" && defaults.CachePath == dir + "/Cache");
    WriteAll(dir + "/ScriptExtenderUpdaterConfig.json", "{ \"UpdateChannel\": \"Devel\", ");
    LoadConfig(dir, "https://x/channel-", failure);
    CHECK(failure == "Failed to load configuration file '" + dir + "/ScriptExtenderUpdaterConfig.json'", "%s", failure.c_str());
}

void Good()
{
    Environment env = MakeEnvironment("good");
    CHECK(Chosen(env) == "0.1.0 bundled", "%s", Chosen(env).c_str());
    Serve("0.2.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
    Outcome o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::None && o.Staged == "0.2.0", "%d %s", (int)o.What, o.Message.c_str());
    CHECK(Chosen(env) == "0.2.0 cached", "%s", Chosen(env).c_str());
    // The camera module is beside the library, where libbg3mse.dylib looks for it.
    std::string dir = BuildsDirectory(env.Settings) + "/" + MakeRelease("0.2.0").Entry.CacheName();
    CHECK(ReadAll(dir + "/libbg3mse-camera.dylib") == "camera module 0.2.0\n");
    CHECK(!Exists(env.Settings.CachePath + "/Staging") || RemoveTree(env.Settings.CachePath + "/Staging"));
    // A build fetched is no news: Norbyte's updater says nothing of it.
    CHECK(DisplayText(o, ChooseBuild(env)).empty(), "%s", DisplayText(o, ChooseBuild(env)).c_str());

    // Run again: nothing is fetched but the manifest.
    int zipHits = gServer.Hits("/v0.2.0/bg3mse-0.2.0.zip");
    o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::None && o.Staged.empty(), "%s", o.Message.c_str());
    CHECK(gServer.Hits("/v0.2.0/bg3mse-0.2.0.zip") == zipHits);

    // A later release with a notice, through a redirect, as GitHub serves assets.
    Built const& b3 = MakeRelease("0.3.0");
    Route redirect;
    redirect.How = Route::Mode::Redirect;
    redirect.Location = gServer.URL("/assets/0.3.0.zip");
    gServer.Set("/v0.3.0/bg3mse-0.3.0.zip", redirect);
    Route asset;
    asset.Body = b3.Zip;
    gServer.Set("/assets/0.3.0.zip", asset);
    Release r3 = b3.Entry;
    r3.Notice = "Notice of 0.3.0";
    ServeManifest(Signed(ManifestJson("Release", 2, { MakeRelease("0.2.0").Entry, r3 })));
    o = Update(env, ChooseBuild(env));
    CHECK(o.Staged == "0.3.0", "%s", o.Message.c_str());
    Choice c = ChooseBuild(env);
    CHECK(Chosen(env) == "0.3.0 cached" && c.Notice == "Notice of 0.3.0", "%s", Chosen(env).c_str());

    // 0.2.0 dropped from the manifest is removed from the cache; the loaded one stays.
    ServeManifest(Signed(ManifestJson("Release", 3, { r3 })));
    o = Update(env, c);
    CHECK(o.What == Category::None, "%s", o.Message.c_str());
    CHECK(!Cached(env, "0.2.0") && Cached(env, "0.3.0"));
}

void Refusals()
{
    struct Case
    {
        char const* Name;
        std::function<void(Environment&)> Arrange;
        Category Expected;
    };
    std::vector<Case> cases = {
        { "tampered-zip", [](Environment&) {
              Built const& b = MakeRelease("0.2.0");
              Route r;
              r.Body = b.Zip;
              r.Body[r.Body.size() / 2] ^= 0x40;
              gServer.Set("/v0.2.0/bg3mse-0.2.0.zip", r);
              ServeManifest(Signed(ManifestJson("Release", 1, { b.Entry })));
          }, Category::UpdateDownload },
        { "truncated-zip", [](Environment&) {
              Built const& b = MakeRelease("0.2.0");
              Route r;
              r.Body = b.Zip.substr(0, b.Zip.size() / 2);
              gServer.Set("/v0.2.0/bg3mse-0.2.0.zip", r);
              ServeManifest(Signed(ManifestJson("Release", 1, { b.Entry })));
          }, Category::UpdateDownload },
        { "connection-cut", [](Environment&) {
              Built const& b = MakeRelease("0.2.0");
              Route r;
              r.Body = b.Zip.substr(0, b.Zip.size() / 2);
              r.Length = (long long)b.Zip.size();     // announces all of it, sends half
              gServer.Set("/v0.2.0/bg3mse-0.2.0.zip", r);
              ServeManifest(Signed(ManifestJson("Release", 1, { b.Entry })));
          }, Category::UpdateDownload },
        { "oversized-zip", [](Environment&) {
              Built const& b = MakeRelease("0.2.0");
              Route r;
              r.Body = b.Zip + std::string(4096, 'x');
              gServer.Set("/v0.2.0/bg3mse-0.2.0.zip", r);
              ServeManifest(Signed(ManifestJson("Release", 1, { b.Entry })));
          }, Category::UpdateDownload },
        { "unsigned-manifest", [](Environment&) {
              Serve("0.2.0");
              ServeManifest(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry }));
          }, Category::ManifestFetch },
        { "tampered-manifest", [](Environment&) {
              Serve("0.2.0");
              std::string s = Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry }));
              size_t at = s.find("\"Sequence\": 1");
              s[at + 12] = '9';
              ServeManifest(s);
          }, Category::ManifestFetch },
        { "other-key", [](Environment&) {
              Serve("0.2.0");
              ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry }), gOtherPrivate));
          }, Category::ManifestFetch },
        { "nightly-as-release", [](Environment&) {
              Serve("0.2.0");
              ServeManifest(Signed(ManifestJson("Nightly", 1, { MakeRelease("0.2.0").Entry })));
          }, Category::ManifestFetch },
        { "wrong-game", [](Environment&) {
              Serve("0.2.0");
              Release r = MakeRelease("0.2.0").Entry;
              r.MinGameVersion = Version::Parse("4.1.2.0");
              ServeManifest(Signed(ManifestJson("Release", 1, { r })));
          }, Category::NoMatchingVersion },
        { "game-too-new", [](Environment&) {
              Serve("0.2.0");
              Release r = MakeRelease("0.2.0").Entry;
              r.MaxGameVersion = Version::Parse("4.1.1.7000000");
              ServeManifest(Signed(ManifestJson("Release", 1, { r })));
          }, Category::NoMatchingVersion },
        { "macos-too-old", [](Environment&) {
              Serve("0.2.0");
              Release r = MakeRelease("0.2.0").Entry;
              r.MacOSMinimum = *Version::Parse("26.0");
              ServeManifest(Signed(ManifestJson("Release", 1, { r })));
          }, Category::NoMatchingVersion },
        { "revoked", [](Environment&) {
              Serve("0.2.0");
              Release r = MakeRelease("0.2.0").Entry;
              r.Revoked = true;
              ServeManifest(Signed(ManifestJson("Release", 1, { r })));
          }, Category::NoMatchingVersion },
        { "loader-too-old", [](Environment&) {
              std::string json = ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry });
              json.replace(json.find("\"ManifestVersion\": 1"), 20, "\"ManifestVersion\": 2");
              ServeManifest(Signed(json));
          }, Category::UpdateRequired },
        { "manifest-404", [](Environment&) {}, Category::ManifestFetch },
        { "zip-404", [](Environment&) {
              ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
          }, Category::UpdateDownload },
        { "network-down", [](Environment& env) {
              // A port nothing listens on: refused at once.
              int fd = socket(AF_INET, SOCK_STREAM, 0);
              sockaddr_in addr{};
              addr.sin_family = AF_INET;
              addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
              bind(fd, (sockaddr*)&addr, sizeof(addr));
              socklen_t size = sizeof(addr);
              getsockname(fd, (sockaddr*)&addr, &size);
              close(fd);
              env.Settings.ManifestURL = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port)) + "/channel-";
          }, Category::ManifestFetch },
        { "server-closes", [](Environment&) {
              Route r;
              r.How = Route::Mode::Close;
              gServer.Set(ManifestPath(), r);
          }, Category::ManifestFetch },
        { "slow-manifest", [](Environment&) {
              // One byte every 0.2 s: each byte comes in time, the whole does not.
              Route r;
              r.How = Route::Mode::Drip;
              r.Body = Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry }));
              gServer.Set(ManifestPath(), r);
          }, Category::ManifestFetch },
        { "stalled-zip", [](Environment&) {
              Serve("0.2.0", Route::Mode::Stall);
              ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
          }, Category::UpdateDownload },
    };
    for (auto const& c : cases) {
        Environment env = MakeEnvironment(c.Name);
        c.Arrange(env);
        auto start = Clock::now();
        Outcome o = Update(env, ChooseBuild(env));
        double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        CHECK(o.What == c.Expected, "outcome %d (%s), expected %d", (int)o.What, o.Message.c_str(), (int)c.Expected);
        CHECK(o.Staged.empty());
        CHECK(Chosen(env) == "0.1.0 bundled", "%s", Chosen(env).c_str());
        // Within the limits: the manifest's 2 s, or the package's 5 s, and a little.
        CHECK(seconds < 7.5, "took %.1f s", seconds);
        CHECK(!Exists(env.Settings.CachePath + "/Staging/" + MakeRelease("0.2.0").Entry.CacheName()));
        CHECK(!DisplayText(o, ChooseBuild(env)).empty());
        bool manifestRefused = c.Expected == Category::ManifestFetch || c.Expected == Category::UpdateRequired;
        CHECK(!manifestRefused || CachedManifest(env).empty(), "a refused manifest was kept");
        if (std::string(c.Name) == "slow-manifest") CHECK(seconds < 3.5, "the 2 s limit took %.1f s", seconds);
        if (std::string(c.Name) == "network-down") CHECK(o.InternetIssue && seconds < 1.9, "%.1f s", seconds);
    }
}

// What was accepted stays: an older or changed manifest does not replace it,
// nor bring back what it revoked.
void Replay()
{
    Environment env = MakeEnvironment("replay");
    Serve("0.2.0");
    Serve("0.3.0");
    std::string first = Signed(ManifestJson("Release", 5, { MakeRelease("0.2.0").Entry }));
    ServeManifest(first);
    CHECK(Update(env, ChooseBuild(env)).Staged == "0.2.0");

    Release revoked = MakeRelease("0.2.0").Entry;
    revoked.Revoked = true;
    std::string second = Signed(ManifestJson("Release", 6, { revoked }));
    ServeManifest(second);
    // The revoked build is in use while this runs: it is no longer chosen,
    // and is removed by the next update, which runs with another build loaded.
    Outcome o = Update(env, ChooseBuild(env));
    CHECK(Chosen(env) == "0.1.0 bundled", "%s", Chosen(env).c_str());
    o = Update(env, ChooseBuild(env));
    CHECK(!Cached(env, "0.2.0"));

    ServeManifest(first);     // the older one again
    o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::ManifestFetch && o.Message.find("older") != std::string::npos, "%s", o.Message.c_str());
    CHECK(CachedManifest(env) == second);
    CHECK(Chosen(env) == "0.1.0 bundled");
    CHECK(!Cached(env, "0.2.0"));

    // Another manifest under sequence 6.
    ServeManifest(Signed(ManifestJson("Release", 6, { MakeRelease("0.3.0").Entry })));
    o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::ManifestFetch && o.Message.find("same sequence") != std::string::npos, "%s", o.Message.c_str());
    CHECK(!Cached(env, "0.3.0"));

    // The same manifest again is fine.
    ServeManifest(second);
    o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::NoMatchingVersion, "%s", o.Message.c_str());
}

// An older release than the build beside the loader is not fetched; a newer
// one that the manifest says is not for this game is not loaded.
void Older()
{
    Environment env = MakeEnvironment("older");
    // Beside the loader: 0.3.0.
    WriteAll(env.BundledLibrary, ReadAll(gArgs.Fakes.at("0.3.0")));
    Serve("0.2.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry, MakeRelease("0.3.0").Entry })));
    Outcome o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::None && o.Staged.empty(), "%s", o.Message.c_str());
    CHECK(gServer.Hits("/v0.2.0/bg3mse-0.2.0.zip") == 0);
    CHECK(Chosen(env) == "0.3.0 bundled");

    // The manifest revokes 0.3.0: the older 0.2.0 is fetched and loaded instead.
    Release r3 = MakeRelease("0.3.0").Entry;
    r3.Revoked = true;
    ServeManifest(Signed(ManifestJson("Release", 2, { MakeRelease("0.2.0").Entry, r3 })));
    o = Update(env, ChooseBuild(env));
    CHECK(o.Staged == "0.2.0", "%s", o.Message.c_str());
    CHECK(Chosen(env) == "0.2.0 cached", "%s", Chosen(env).c_str());
}

// A cached build that no longer matches is not loaded; a manifest in the
// cache that does not verify counts as none.
void DamagedCache()
{
    Environment env = MakeEnvironment("damaged-cache");
    Serve("0.2.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
    CHECK(Update(env, ChooseBuild(env)).Staged == "0.2.0");
    std::string library = BuildsDirectory(env.Settings) + "/" + MakeRelease("0.2.0").Entry.CacheName() + "/libbg3mse.dylib";
    std::string good = ReadAll(library);
    std::string bad = good;
    bad[100] ^= 1;
    WriteAll(library, bad);
    Choice c = ChooseBuild(env);
    CHECK(Chosen(env) == "0.1.0 bundled" && c.Note.find("not the file") != std::string::npos, "%s / %s", Chosen(env).c_str(), c.Note.c_str());
    WriteAll(library, good);
    CHECK(Chosen(env) == "0.2.0 cached");

    // Left damaged, it is fetched again by the next update.
    WriteAll(library, bad);
    Outcome o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::None && o.Staged == "0.2.0", "%d %s", (int)o.What, o.Message.c_str());
    CHECK(Chosen(env) == "0.2.0 cached", "%s", Chosen(env).c_str());

    std::string manifest = CachedManifest(env);
    std::string changed = manifest;
    changed[10] ^= 1;
    WriteAll(ManifestCachePath(env.Settings), changed);
    CHECK(Chosen(env) == "0.1.0 bundled");
    WriteAll(ManifestCachePath(env.Settings), manifest);

    // What a killed update leaves in Staging is cleared by the next one,
    // and the build in use stays.
    WriteAll(env.Settings.CachePath + "/Staging/" + MakeRelease("0.3.0").Entry.CacheName() + "/libbg3mse.dylib", "half a file");
    CHECK(Chosen(env) == "0.2.0 cached");
    ServeManifest(Signed(ManifestJson("Release", 2, { MakeRelease("0.2.0").Entry })));
    Update(env, ChooseBuild(env));
    CHECK(!Exists(env.Settings.CachePath + "/Staging/" + MakeRelease("0.3.0").Entry.CacheName()));

    // No key: nothing cached is trusted, nothing is fetched, and that is no failure to show.
    env.Key.reset();
    CHECK(Chosen(env) == "0.1.0 bundled");
    o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::None && DisplayText(o, ChooseBuild(env)).empty(), "%s", o.Message.c_str());
    // DebugLoadSE: the build beside the loader.
    env.Key = gPublic;
    env.Settings.DebugLoadSE = true;
    CHECK(Chosen(env) == "0.1.0 bundled");
}

// The pins of ScriptExtenderUpdaterConfig.json.
void Pins()
{
    Environment env = MakeEnvironment("pins");
    Serve("0.2.0");
    Serve("0.3.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry, MakeRelease("0.3.0").Entry })));
    env.Settings.TargetVersion = Version::Parse("0.2.0");
    CHECK(Update(env, ChooseBuild(env)).Staged == "0.2.0");
    CHECK(Chosen(env) == "0.2.0 cached");
    env.Settings.TargetVersion.reset();
    env.Settings.TargetResourceDigest = MakeRelease("0.3.0").Entry.Digest;
    CHECK(Update(env, ChooseBuild(env)).Staged == "0.3.0");
    CHECK(Chosen(env) == "0.3.0 cached");
    env.Settings.TargetResourceDigest = std::string(64, 'a');
    Outcome o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::NoMatchingVersion);
    Choice c = ChooseBuild(env);
    CHECK(Chosen(env) == "0.1.0 bundled" && !c.Problem.empty());
    // DisableUpdates fetches nothing.
    env.Settings.TargetResourceDigest.clear();
    env.Settings.DisableUpdates = true;
    int hits = gServer.Hits(ManifestPath());
    o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::None && gServer.Hits(ManifestPath()) == hits);
}

// A download that does not succeed is not tried at every start, as the game
// waits for it: the same build again a day after, another build at once.
void FailedDownload()
{
    Environment env = MakeEnvironment("failed-download");
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
    std::string zip = "/v0.2.0/bg3mse-0.2.0.zip", mark = env.Settings.CachePath + "/Failed-" + MakeRelease("0.2.0").Entry.CacheName();
    Outcome o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::UpdateDownload && gServer.Hits(zip) == 1 && Exists(mark), "%s", o.Message.c_str());
    o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::None && o.Message.find("it is tried again a day after") != std::string::npos && gServer.Hits(zip) == 1,
        "%s", o.Message.c_str());
    // A day after: tried again; and now served.
    timeval aDayAgo[2] = { { time(nullptr) - 25 * 3600, 0 }, { time(nullptr) - 25 * 3600, 0 } };
    utimes(mark.c_str(), aDayAgo);
    Serve("0.2.0");
    o = Update(env, ChooseBuild(env));
    CHECK(o.Staged == "0.2.0" && gServer.Hits(zip) == 2 && !Exists(mark), "%s", o.Message.c_str());
    // Another build, failing at once after this one did: fetched at once all the same.
    ServeManifest(Signed(ManifestJson("Release", 2, { MakeRelease("0.2.0").Entry, MakeRelease("0.3.0").Entry })));
    o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::UpdateDownload && gServer.Hits("/v0.3.0/bg3mse-0.3.0.zip") == 1, "%s", o.Message.c_str());
}

// A cached build chosen where the manifest says the one beside the loader
// is not for this game: should it not load, the loader loads nothing.
void NoFallback()
{
    Environment env = MakeEnvironment("no-fallback");
    Release bundled = MakeRelease("0.1.0").Entry;
    bundled.MinGameVersion = Version::Parse("5.0");
    Serve("0.2.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { bundled, MakeRelease("0.2.0").Entry })));
    CHECK(Update(env, ChooseBuild(env)).Staged == "0.2.0");
    Choice c = ChooseBuild(env);
    CHECK(c.Cached && c.NoFallback.starts_with("No extender version found for game version v"), "%s", c.NoFallback.c_str());
    // The one beside the loader fits: it is what is loaded then.
    ServeManifest(Signed(ManifestJson("Release", 2, { MakeRelease("0.1.0").Entry, MakeRelease("0.2.0").Entry })));
    Update(env, ChooseBuild(env));
    c = ChooseBuild(env);
    CHECK(c.Cached && c.NoFallback.empty(), "%s", c.NoFallback.c_str());
}

// A zip with stored entries, written here so that it can hold what the
// release target never makes.
std::string StoredZip(std::vector<std::tuple<std::string, std::string, uint32_t>> const& entries)
{
    std::string out, central;
    auto u16 = [](std::string& s, uint16_t v) { s += (char)(v & 255); s += (char)(v >> 8); };
    auto u32 = [&](std::string& s, uint32_t v) { u16(s, (uint16_t)(v & 0xffff)); u16(s, (uint16_t)(v >> 16)); };
    for (auto const& [name, data, mode] : entries) {
        uint32_t crc = (uint32_t)crc32(0, reinterpret_cast<Bytef const*>(data.data()), (uInt)data.size());
        uint32_t offset = (uint32_t)out.size();
        u32(out, 0x04034b50); u16(out, 10); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0);
        u32(out, crc); u32(out, (uint32_t)data.size()); u32(out, (uint32_t)data.size());
        u16(out, (uint16_t)name.size()); u16(out, 0);
        out += name;
        out += data;
        u32(central, 0x02014b50); u16(central, 3 << 8 | 30); u16(central, 10); u16(central, 0); u16(central, 0);
        u16(central, 0); u16(central, 0);
        u32(central, crc); u32(central, (uint32_t)data.size()); u32(central, (uint32_t)data.size());
        u16(central, (uint16_t)name.size()); u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0);
        u32(central, mode << 16); u32(central, offset);
        central += name;
    }
    uint32_t at = (uint32_t)out.size();
    out += central;
    u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, (uint16_t)entries.size()); u16(out, (uint16_t)entries.size());
    u32(out, (uint32_t)central.size()); u32(out, at); u16(out, 0);
    return out;
}

// Signed, but not what a release is: each is refused while unpacking.
void BadZips()
{
    Built const& b = MakeRelease("0.2.0");
    std::string library = ReadAll(gArgs.Fakes.at("0.2.0"));
    std::string camera = "camera module 0.2.0\n";
    std::string licence = "licence\n";
    std::string p = "bg3mse-0.2.0/";
    using E = std::vector<std::tuple<std::string, std::string, uint32_t>>;
    E good = { { p + "manifest.json", b.ManifestJson, 0100644 }, { p + "libbg3mse.dylib", library, 0100755 },
        { p + "camera/libbg3mse-camera.dylib", camera, 0100644 }, { p + "licenses/bg3mse.txt", licence, 0100644 },
        { p + "Install bg3mse.command", "#!/bin/sh\n", 0100755 } };
    struct Case
    {
        char const* Name;
        E Entries;
        bool Accepted;
    };
    E traversal = good;
    traversal.push_back({ p + "../escaped.txt", "x", 0100644 });
    E extra = good;
    extra.push_back({ p + "unlisted.txt", "x", 0100644 });
    E link = good;
    std::get<2>(link[3]) = 0120777;
    E outside = good;
    std::get<0>(outside[3]) = "elsewhere/licenses/bg3mse.txt";
    E missing = good;
    missing.pop_back();
    E swapped = good;
    std::get<1>(swapped[2]) = "camera module 0.3.0\n";
    E absolute = good;
    absolute.push_back({ "/tmp/bg3mse-absolute.txt", "x", 0100644 });
    E twice = good;
    twice.push_back(good[1]);
    std::vector<Case> cases = { { "zip-stored-good", good, true }, { "zip-traversal", traversal, false }, { "zip-unlisted", extra, false },
        { "zip-symlink", link, false }, { "zip-outside", outside, false }, { "zip-missing", missing, false },
        { "zip-changed-file", swapped, false }, { "zip-absolute", absolute, false }, { "zip-twice", twice, false } };
    for (auto const& c : cases) {
        Environment env = MakeEnvironment(c.Name);
        std::string zip = StoredZip(c.Entries);
        Release r = b.Entry;
        r.Size = zip.size();
        r.Digest = Hex(Sha256(Bytes(zip)));
        Route route;
        route.Body = zip;
        gServer.Set("/v0.2.0/bg3mse-0.2.0.zip", route);
        ServeManifest(Signed(ManifestJson("Release", 1, { r })));
        Outcome o = Update(env, ChooseBuild(env));
        if (c.Accepted) {
            CHECK(o.Staged == "0.2.0", "%s", o.Message.c_str());
            CHECK(Chosen(env) == "0.2.0 cached");
        } else {
            CHECK(o.What == Category::LocalUpdate, "%d %s", (int)o.What, o.Message.c_str());
            CHECK(Chosen(env) == "0.1.0 bundled");
            CHECK(!Exists(env.Settings.CachePath + "/Staging"));
        }
        CHECK(!Exists(gArgs.Scratch + "/cases/escaped.txt") && !Exists("/tmp/bg3mse-absolute.txt"));
    }
}

// The cases that end a process: in a child started afresh (a forked child
// may not use Foundation once its parent has), which runs one update on the
// case's cache and exits with its outcome's category.
int RunChild(Environment const& env, std::string const& mode)
{
    std::string exe = gArgs.Self;
    std::vector<std::string> args = { exe, "--child=" + mode, "--cache=" + env.Settings.CachePath,
        "--url=" + env.Settings.ManifestURL, "--bundled=" + env.BundledLibrary, "--key=" + Hex(gPublic),
        "--release=" + MakeRelease("0.2.0").Entry.CacheName() };
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) != 0) return -1;
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int ChildMain(std::map<std::string, std::string> const& o)
{
    Environment env;
    env.Settings.ManifestURL = o.at("url");
    env.Settings.CachePath = o.at("cache");
    PublicKey key;
    FromHex(o.at("key"), key);
    env.Key = key;
    env.Here.Game = *Version::Parse(kGame);
    env.Here.MacOS = *Version::Parse("15.0");
    env.BundledLibrary = o.at("bundled");
    env.ManifestLimits = { 2.0, 1.0, 4u << 20 };
    env.PackageLimits = { 5.0, 1.0, 0 };
    if (o.at("child") == "full-disk") {
        signal(SIGXFSZ, SIG_IGN);
        rlimit limit{ 8 * 1024, 8 * 1024 };
        setrlimit(RLIMIT_FSIZE, &limit);
    } else if (o.at("child") == "killed") {
        // Killed as soon as unpacking has begun.
        std::string staging = env.Settings.CachePath + "/Staging/" + o.at("release") + "/manifest.json";
        std::thread([staging] {
            for (;;) {
                if (Exists(staging)) kill(getpid(), SIGKILL);
                usleep(200);
            }
        }).detach();
    }
    Outcome outcome = Update(env, ChooseBuild(env));
    return (int)outcome.What;
}

// A full disk: the child's files may not grow past 8 KiB, less than a library.
void FullDisk()
{
    Environment env = MakeEnvironment("full-disk");
    Serve("0.2.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
    int code = RunChild(env, "full-disk");
    CHECK(code == (int)Category::LocalUpdate, "child ended with %d", code);
    CHECK(Chosen(env) == "0.1.0 bundled");
    CHECK(!Cached(env, "0.2.0"));
    // With room again, the same update goes through.
    CHECK(Update(env, ChooseBuild(env)).Staged == "0.2.0");
}

// Killed while unpacking: what is loaded stays whole, and the next update
// starts again from nothing.
void Killed()
{
    Environment env = MakeEnvironment("killed");
    Serve("0.2.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
    int code = RunChild(env, "killed");
    CHECK(code == 1000 + SIGKILL, "child ended with %d", code);
    CHECK(Exists(env.Settings.CachePath + "/Staging"));
    CHECK(Chosen(env) == "0.1.0 bundled", "%s", Chosen(env).c_str());
    CHECK(Update(env, ChooseBuild(env)).Staged == "0.2.0");
    CHECK(Chosen(env) == "0.2.0 cached");
    CHECK(!Exists(env.Settings.CachePath + "/Staging"));
}

// --- Part two: the loader in a stand-in game --------------------------------------------

struct Run
{
    std::vector<std::pair<double, std::string>> Lines;
    int Status{ -1 };
    double Main{ -1 };
    std::string Extender;      // "<version> <path>"
};

struct Game
{
    std::string Dir, MacOS, Home, Data;
};

Game MakeGame(std::string const& name, std::string const& bundled = "0.1.0")
{
    gCase = name;
    gServer.Clear();
    Game g;
    g.Dir = gArgs.Scratch + "/games/" + name;
    RemoveTree(g.Dir);
    g.MacOS = g.Dir + "/Baldur's Gate 3.app/Contents/MacOS";
    g.Home = g.Dir + "/home";
    g.Data = g.Home + "/Library/Application Support/bg3mse";
    WriteAll(g.Dir + "/Baldur's Gate 3.app/Contents/Info.plist",
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict>"
        "<key>CFBundleExecutable</key><string>Baldur's Gate 3</string>"
        "<key>CFBundleIdentifier</key><string>com.larian.bg3</string>"
        "<key>CFBundleVersion</key><string>" + std::string(kGame) + "</string>"
        "<key>CFBundlePackageType</key><string>APPL</string></dict></plist>\n");
    WriteAll(g.MacOS + "/Baldur's Gate 3", ReadAll(gArgs.StandIn));
    chmod((g.MacOS + "/Baldur's Gate 3").c_str(), 0755);
    WriteAll(g.MacOS + "/libbg3mse-loader.dylib", ReadAll(gArgs.Loader));
    WriteAll(g.MacOS + "/libbg3mse.dylib", ReadAll(gArgs.Fakes.at(bundled)));
    WriteAll(g.Data + "/ScriptExtenderUpdaterConfig.json",
        "{ \"ManifestURL\": \"" + gServer.URL("/channel-") + "\" }\n");
    return g;
}

// The stand-in game started with the loader beside it inserted, or, given
// `launcher`, through bg3mse-launch, given the bundle as Steam gives it.
Run Start(Game const& g, std::vector<std::string> args = {}, double timeout = 20, std::string launcher = {},
    std::vector<std::string> extraEnv = {}, int expectStatus = 0)
{
    std::string exe = g.MacOS + "/Baldur's Gate 3";
    std::string app = g.Dir + "/Baldur's Gate 3.app";
    std::vector<std::string> env = { "HOME=" + g.Home, "DYLD_INSERT_LIBRARIES=" + g.MacOS + "/libbg3mse-loader.dylib",
        "BG3MSE_UPDATER_TEST_KEY=" + Hex(gPublic), "PATH=/usr/bin:/bin" };
    if (!launcher.empty()) env.erase(env.begin() + 1);
    env.insert(env.end(), extraEnv.begin(), extraEnv.end());
    std::vector<char*> envp, argv;
    for (auto& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);
    if (!launcher.empty()) {
        exe = launcher;
        argv.push_back(exe.data());
        argv.push_back(app.data());
    } else {
        argv.push_back(exe.data());
    }
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    int out[2];
    pipe(out);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    Run run;
    auto start = Clock::now();
    pid_t pid;
    if (posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), envp.data()) != 0) {
        CHECK(false, "cannot start the stand-in");
        return run;
    }
    close(out[1]);
    std::string pending;
    for (;;) {
        double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        if (elapsed > timeout) {
            kill(pid, SIGKILL);
            CHECK(false, "the stand-in did not end in %.0f s", timeout);
            break;
        }
        pollfd p{ out[0], POLLIN, 0 };
        if (poll(&p, 1, 100) <= 0) continue;
        char buffer[4096];
        ssize_t n = read(out[0], buffer, sizeof(buffer));
        if (n <= 0) break;
        pending.append(buffer, (size_t)n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            double at = std::chrono::duration<double>(Clock::now() - start).count();
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            if (line == "main" && run.Main < 0) run.Main = at;
            if (line.starts_with("extender ")) run.Extender = line.substr(9);
            run.Lines.push_back({ at, line });
        }
    }
    close(out[0]);
    waitpid(pid, &run.Status, 0);
    posix_spawn_file_actions_destroy(&actions);
    // The game ends as it would without the loader: nothing aborts at exit.
    CHECK(WIFEXITED(run.Status) && WEXITSTATUS(run.Status) == expectStatus, "the stand-in ended with status %d", run.Status);
    return run;
}

std::string UpdaterLog(Game const& g)
{
    return ReadAll(g.Data + "/logs/updater_latest.log");
}

// The update runs before the game's main(), as Norbyte's holds the game at
// SDL_Init: an answering server adds little to the start, one that does not
// answer at most the manifest's limits (5 s to connect, 5 s without data).
// (Loose, so that a machine busy building does not fail it.)
constexpr double kMainBudget = 3.0;
constexpr double kStalledBudget = 14.0;

void LoaderGood()
{
    Game g = MakeGame("loader-good");
    Serve("0.2.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
    std::string staged = g.Data + "/Cache/Builds/" + MakeRelease("0.2.0").Entry.CacheName();
    // The start that fetches the update loads it.
    Run first = Start(g);
    CHECK(first.Extender == "0.2.0 " + staged + "/libbg3mse.dylib", "%s", first.Extender.c_str());
    CHECK(first.Main >= 0 && first.Main < kMainBudget, "main at %.2f s", first.Main);
    CHECK(WIFEXITED(first.Status) && WEXITSTATUS(first.Status) == 0, "status %d", first.Status);
    CHECK(UpdaterLog(g).find("Script Extender 0.2.0 downloaded and checked") != std::string::npos, "%s", UpdaterLog(g).c_str());
    // The extender is loaded before main(), as when the game loads it itself.
    CHECK(!first.Lines.empty() && first.Lines[0].second.starts_with("extender "));

    Run second = Start(g);
    CHECK(second.Extender == "0.2.0 " + staged + "/libbg3mse.dylib", "%s", second.Extender.c_str());
    CHECK(second.Main >= 0 && second.Main < kMainBudget, "main at %.2f s", second.Main);
}

// The loader of a release that does not update itself (BG3MSE_SELF_UPDATE=OFF):
// with a newer build in the cache and on the channel, it loads the build
// beside it, asks the server for nothing and shows no progress window.
std::string ReadUi(std::string const& path);

void LoaderOffline()
{
    Game g = MakeGame("loader-offline");
    Serve("0.2.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
    Run updated = Start(g);
    CHECK(updated.Extender.starts_with("0.2.0 "), "%s", updated.Extender.c_str());
    WriteAll(g.MacOS + "/libbg3mse-loader.dylib", ReadAll(gArgs.OfflineLoader));
    int manifest = gServer.Hits(ManifestPath()), zip = gServer.Hits("/v0.2.0/bg3mse-0.2.0.zip");
    std::string ui = g.Dir + "/ui.txt";
    Run run = Start(g, {}, 20, {}, { "BG3MSE_UPDATER_TEST_UI=" + ui });
    CHECK(run.Extender == "0.1.0 " + g.MacOS + "/libbg3mse.dylib", "%s", run.Extender.c_str());
    CHECK(run.Main >= 0 && run.Main < kMainBudget, "main at %.2f s", run.Main);
    CHECK(gServer.Hits(ManifestPath()) == manifest && gServer.Hits("/v0.2.0/bg3mse-0.2.0.zip") == zip);
    CHECK(UpdaterLog(g).find("this bg3mse does not update itself") != std::string::npos, "%s", UpdaterLog(g).c_str());
    CHECK(ReadUi(ui).empty(), "%s", ReadUi(ui).c_str());
}

// The manifest says the build beside the loader is not for this game, and
// has none that is: nothing is loaded, as Norbyte's updater loads nothing,
// and his critical box says why; the game goes on.

void LoaderNoBuildFits()
{
    Game g = MakeGame("loader-no-build-fits");
    Release r = MakeRelease("0.1.0").Entry;
    r.MinGameVersion = Version::Parse("5.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { r })));
    std::string ui = g.Dir + "/ui.txt";
    Run run = Start(g, { "wait", g.Dir + "/never", "1" }, 20, {}, { "BG3MSE_UPDATER_TEST_UI=" + ui });
    CHECK(run.Extender.empty(), "%s", run.Extender.c_str());
    CHECK(run.Main >= 0 && run.Main < kMainBudget, "main at %.2f s", run.Main);
    for (int i = 0; i < 30 && ReadUi(ui).empty(); i++) usleep(100000);
    CHECK(ReadUi(ui).starts_with("error bg3mse: No bg3mse version supports this game version yet"),
        "%s", ReadUi(ui).c_str());
    CHECK(UpdaterLog(g).find("nothing is loaded") != std::string::npos, "%s", UpdaterLog(g).c_str());
}

void LoaderFailures()
{
    struct Case
    {
        char const* Name;
        std::function<void()> Arrange;
        char const* Logged;
    };
    std::vector<Case> cases = {
        { "loader-network-down", [] {}, "Unable to fetch manifest" },
        { "loader-stalled-manifest", [] {
              Route r;
              r.How = Route::Mode::Stall;
              gServer.Set(ManifestPath(), r);
          }, "Unable to fetch manifest" },
        { "loader-tampered-zip", [] {
              Built const& b = MakeRelease("0.2.0");
              Route r;
              r.Body = b.Zip;
              r.Body[r.Body.size() / 3] ^= 1;
              gServer.Set("/v0.2.0/bg3mse-0.2.0.zip", r);
              ServeManifest(Signed(ManifestJson("Release", 1, { b.Entry })));
          }, "SHA-256" },
        { "loader-unsigned", [] {
              Serve("0.2.0");
              ServeManifest(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry }));
          }, "not signed" },
        { "loader-wrong-game", [] {
              Serve("0.2.0");
              Release r = MakeRelease("0.2.0").Entry;
              r.MinGameVersion = Version::Parse("5.0");
              ServeManifest(Signed(ManifestJson("Release", 1, { r })));
          }, "supports this game version" },
    };
    for (auto const& c : cases) {
        Game g = MakeGame(c.Name);
        c.Arrange();
        if (std::string(c.Name) == "loader-network-down") {
            WriteAll(g.Data + "/ScriptExtenderUpdaterConfig.json", "{ \"ManifestURL\": \"http://127.0.0.1:1/channel-\" }\n");
        }
        bool stalled = std::string(c.Name) == "loader-stalled-manifest";
        Run run = Start(g, { "wait", g.Dir + "/never", "1" });
        CHECK(run.Extender == "0.1.0 " + g.MacOS + "/libbg3mse.dylib", "%s", run.Extender.c_str());
        CHECK(run.Main >= 0 && run.Main < (stalled ? kStalledBudget : kMainBudget), "main at %.2f s", run.Main);
        CHECK(WIFEXITED(run.Status) && WEXITSTATUS(run.Status) == 0, "status %d", run.Status);
        for (int i = 0; i < 80 && UpdaterLog(g).find("update failed") == std::string::npos; i++) usleep(100000);
        std::string log = UpdaterLog(g);
        if (std::string(c.Name) != "loader-stalled-manifest") {
            size_t line = log.find("update failed, ");
            CHECK(line != std::string::npos && log.find(c.Logged, line) != std::string::npos, "%s", log.c_str());
            CHECK(log.find("update failed, ", line + 1) == std::string::npos, "more than one failure line");
        }
        CHECK(!Exists(g.Data + "/Cache/Builds/" + MakeRelease("0.2.0").Entry.CacheName()));
        Run again = Start(g);
        CHECK(again.Extender == "0.1.0 " + g.MacOS + "/libbg3mse.dylib", "%s", again.Extender.c_str());
    }
}


// --- transfers: IPv4Only and Debug (Fetch.h) -------------------------------------

// A server on ::1 only, which answers one request with "ok".
struct Ipv6Server
{
    int Listen{ -1 };
    int Port{ 0 };
    std::thread Thread;

    bool Start()
    {
        Listen = socket(AF_INET6, SOCK_STREAM, 0);
        if (Listen < 0) return false;
        int one = 1;
        setsockopt(Listen, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_loopback;
        socklen_t size = sizeof(addr);
        if (bind(Listen, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(Listen, 8) != 0) return false;
        getsockname(Listen, (sockaddr*)&addr, &size);
        Port = ntohs(addr.sin6_port);
        Thread = std::thread([this] {
            for (;;) {
                int fd = accept(Listen, nullptr, nullptr);
                if (fd < 0) return;
                char buffer[4096];
                read(fd, buffer, sizeof(buffer));
                char const answer[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
                write(fd, answer, sizeof(answer) - 1);
                close(fd);
            }
        });
        return true;
    }

    ~Ipv6Server()
    {
        if (Listen >= 0) {
            shutdown(Listen, SHUT_RDWR);
            close(Listen);
        }
        if (Thread.joinable()) Thread.join();
    }
};

void Transfers()
{
    gCase = "transfers";
    gServer.Clear();
    Route r;
    r.Body = "the body";
    gServer.Set("/body", r);
    Route redirect;
    redirect.How = Route::Mode::Redirect;
    redirect.Location = "/body";
    gServer.Set("/moved", redirect);
    Route away;
    away.How = Route::Mode::Redirect;
    away.Location = "file:///etc/passwd";
    gServer.Set("/away", away);
    Route stall;
    stall.How = Route::Mode::Stall;
    stall.Length = 1000;
    gServer.Set("/stall", stall);

    FetchLimits limits{ 3.0, 1.0, 1 << 20 };
    for (bool ipv4 : { false, true }) {
        FetchOptions options;
        options.IPv4Only = ipv4;
        std::vector<uint8_t> body;
        auto fetched = Fetch(gServer.URL("/body"), limits, body, nullptr, options);
        CHECK(fetched.Failure == FetchFailure::None && std::string(body.begin(), body.end()) == "the body", "IPv4Only %d: %s", ipv4,
            fetched.Message.c_str());
        fetched = Fetch(gServer.URL("/moved"), limits, body, nullptr, options);
        CHECK(fetched.Failure == FetchFailure::None && std::string(body.begin(), body.end()) == "the body", "IPv4Only %d: %s", ipv4,
            fetched.Message.c_str());
        fetched = Fetch(gServer.URL("/missing"), limits, body, nullptr, options);
        CHECK(fetched.Failure == FetchFailure::Http && fetched.Status == 404 && body.empty(), "IPv4Only %d: %d", ipv4, (int)fetched.Failure);
        // Refused (by this code or by the system's URL loading, which has its own word for it).
        fetched = Fetch(gServer.URL("/away"), limits, body, nullptr, options);
        CHECK(fetched.Failure != FetchFailure::None && body.empty(), "IPv4Only %d: %d %s", ipv4, (int)fetched.Failure, fetched.Message.c_str());
        fetched = Fetch(gServer.URL("/body"), FetchLimits{ 3.0, 1.0, 4 }, body, nullptr, options);
        CHECK(fetched.Failure == FetchFailure::TooLarge && body.empty(), "IPv4Only %d: %d", ipv4, (int)fetched.Failure);
        auto start = Clock::now();
        fetched = Fetch(gServer.URL("/stall"), limits, body, nullptr, options);
        double took = std::chrono::duration<double>(Clock::now() - start).count();
        CHECK(fetched.Failure == FetchFailure::TimedOut && took < 2.5, "IPv4Only %d: %d after %.1f s", ipv4, (int)fetched.Failure, took);
        std::atomic<bool> cancel{ false };
        std::thread canceller([&] {
            usleep(300000);
            cancel = true;
        });
        fetched = Fetch(gServer.URL("/stall"), FetchLimits{ 10.0, 10.0, 1 << 20 }, body, &cancel, options);
        canceller.join();
        CHECK(fetched.Failure == FetchFailure::Cancelled, "IPv4Only %d: %d", ipv4, (int)fetched.Failure);
    }

    // A server that has only an IPv6 address: IPv4Only does not reach it.
    Ipv6Server v6;
    CHECK(v6.Start(), "no server on ::1");
    if (v6.Port) {
        std::string url = "http://localhost:" + std::to_string(v6.Port) + "/";
        std::vector<uint8_t> body;
        auto fetched = Fetch(url, limits, body, nullptr, FetchOptions{});
        CHECK(fetched.Failure == FetchFailure::None && std::string(body.begin(), body.end()) == "ok", "%s", fetched.Message.c_str());
        FetchOptions v4;
        v4.IPv4Only = true;
        fetched = Fetch(url, limits, body, nullptr, v4);
        CHECK(fetched.Failure == FetchFailure::Connection, "IPv4Only reached ::1: %d %s", (int)fetched.Failure, fetched.Message.c_str());
    }

    // Debug: every transfer traced in DEBUG lines.
    std::string logPath = gArgs.Scratch + "/transfers.log";
    FILE* log = fopen(logPath.c_str(), "w");
    bg3se::mac::SetLogStream(log);
    bg3se::mac::SetLogDebug(true);
    for (bool ipv4 : { false, true }) {
        FetchOptions options;
        options.IPv4Only = ipv4;
        options.Trace = true;
        std::vector<uint8_t> body;
        Fetch(gServer.URL("/moved"), limits, body, nullptr, options);
    }
    bg3se::mac::SetLogDebug(false);
    bg3se::mac::SetLogStream(nullptr);
    fclose(log);
    std::string traced = ReadAll(logPath);
    CHECK(traced.find("[DEBUG] fetch: GET " + gServer.URL("/moved") + " (3 s in all, 1 s without data, IPv4 only off)") != std::string::npos,
        "%s", traced.c_str());
    CHECK(traced.find("IPv4 only on") != std::string::npos);
    CHECK(traced.find("fetch: HTTP 302 redirects to " + gServer.URL("/body")) != std::string::npos, "%s", traced.c_str());
    CHECK(traced.find("fetch: < HTTP/1.1 302") != std::string::npos, "%s", traced.c_str());
    CHECK(traced.find("fetch: 8 bytes from") != std::string::npos);
}

// Norbyte's status texts while the update runs, and a cancelled update is
// no failure.
void StatusAndCancel()
{
    Environment env = MakeEnvironment("status-and-cancel");
    Serve("0.2.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
    std::vector<std::string> said;
    env.Status = [&](std::string const& text) { said.push_back(text); };
    Outcome o = Update(env, ChooseBuild(env));
    CHECK(o.What == Category::None && o.Staged == "0.2.0", "%s", o.Message.c_str());
    CHECK((said == std::vector<std::string>{ "Fetching manifest", "Downloading update: 0.2.0", "Unpacking update: 0.2.0" }));

    env = MakeEnvironment("status-and-cancel");
    Serve("0.2.0", Route::Mode::Stall);
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
    std::atomic<bool> cancel{ false };
    env.Cancel = &cancel;
    env.PackageLimits = { 10.0, 10.0, 0 };
    std::thread canceller([&] {
        usleep(400000);
        cancel = true;
    });
    o = Update(env, ChooseBuild(env));
    canceller.join();
    CHECK(o.Cancelled && o.What == Category::None && o.Staged.empty(), "%d %s", (int)o.What, o.Message.c_str());
    CHECK(DisplayText(o, ChooseBuild(env)).empty());
    CHECK(!Cached(env, "0.2.0"));
}

std::string ReadUi(std::string const& path)
{
    return Exists(path) ? ReadAll(path) : std::string();
}

// What the player sees while the game starts: Norbyte's progress window a
// second into an update that is still running, its Cancel, his error box,
// his Fail() box for a damaged settings file, and Shift for Debug.
void LoaderNotices()
{
    auto wait = [](std::function<bool()> done, double seconds) {
        for (int i = 0; i < seconds * 20 && !done(); i++) usleep(50000);
        return done();
    };

    {
        // A manifest that does not come: the window, then the box.
        Game g = MakeGame("loader-window");
        Route r;
        r.How = Route::Mode::Stall;
        r.Length = 1000;
        gServer.Set(ManifestPath(), r);
        std::string ui = g.Dir + "/ui.txt";
        Run run = Start(g, { "wait", g.Dir + "/never", "1" }, 30, {}, { "BG3MSE_UPDATER_TEST_UI=" + ui });
        CHECK(run.Main >= 0 && run.Main < kStalledBudget, "main at %.2f s", run.Main);
        CHECK(wait([&] { return ReadUi(ui).find("warning ") != std::string::npos; }, 3), "%s", ReadUi(ui).c_str());
        std::string seen = ReadUi(ui);
        CHECK(seen.starts_with("open Fetching manifest\nclose\n"), "%s", seen.c_str());
        CHECK(seen.find("warning bg3mse: bg3mse update failed. Check your internet connection.|Unable to fetch manifest - ") != std::string::npos,
            "%s", seen.c_str());
        CHECK(seen.find("||Using the last downloaded version (v0.1.0).\n") != std::string::npos, "%s", seen.c_str());
    }
    {
        // The same with an extender that shows errors itself: no box.
        Game g = MakeGame("loader-window-reported");
        Route r;
        r.How = Route::Mode::Stall;
        r.Length = 1000;
        gServer.Set(ManifestPath(), r);
        std::string ui = g.Dir + "/ui.txt";
        Start(g, { "wait", g.Dir + "/never", "1" }, 30, {}, { "BG3MSE_UPDATER_TEST_UI=" + ui, "FAKE_ERROR_REPORTING=1" });
        usleep(500000);
        CHECK(ReadUi(ui) == "open Fetching manifest\nclose\n", "%s", ReadUi(ui).c_str());
    }
    {
        // Cancel: the update stops, nothing is shown after, the build loaded stays.
        Game g = MakeGame("loader-window-cancel");
        Route r;
        r.How = Route::Mode::Stall;
        r.Length = 1000;
        gServer.Set(ManifestPath(), r);
        std::string ui = g.Dir + "/ui.txt";
        std::thread player([&] {
            if (wait([&] { return ReadUi(ui).find("open ") != std::string::npos; }, 5)) WriteAll(ui + ".cancel", "");
        });
        Run run = Start(g, { "wait", g.Dir + "/never", "3" }, 20, {}, { "BG3MSE_UPDATER_TEST_UI=" + ui });
        player.join();
        CHECK(run.Extender == "0.1.0 " + g.MacOS + "/libbg3mse.dylib", "%s", run.Extender.c_str());
        CHECK(wait([&] { return UpdaterLog(g).find("update: cancelled") != std::string::npos; }, 3), "%s", UpdaterLog(g).c_str());
        CHECK(UpdaterLog(g).find("update: cancelled by the player") != std::string::npos);
        CHECK(UpdaterLog(g).find("update failed") == std::string::npos);
        usleep(300000);
        CHECK(ReadUi(ui) == "open Fetching manifest\ncancel\n", "%s", ReadUi(ui).c_str());
    }
    {
        // An update that ends within the second shows nothing.
        Game g = MakeGame("loader-quick");
        Serve("0.2.0");
        ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
        std::string ui = g.Dir + "/ui.txt";
        std::string staged = g.Data + "/Cache/Builds/" + MakeRelease("0.2.0").Entry.CacheName();
        Start(g, { "wait", staged, "15" }, 20, {}, { "BG3MSE_UPDATER_TEST_UI=" + ui, "FAKE_ERROR_REPORTING=1" });
        CHECK(wait([&] { return UpdaterLog(g).find("downloaded and checked") != std::string::npos; }, 3));
        usleep(1500000);
        CHECK(ReadUi(ui).empty(), "%s", ReadUi(ui).c_str());
    }
    {
        // A native module, which the stand-in extender plays from its own
        // start (Norbyte's SEUpdaterGetModule and SEUpdaterNotifyWhenLoaded):
        // while the loader is loading the extender there is no module yet;
        // once it is loaded the module that asked is told, with a module
        // that leads dlsym to the extender's functions; one that asks then
        // is told at once.
        Game g = MakeGame("loader-native-module");
        Run run = Start(g, {}, 20, {}, { "FAKE_NATIVE_MODULE=1" });
        std::vector<std::string> native;
        for (auto const& line : run.Lines) {
            if (line.second.starts_with("native ")) native.push_back(line.second);
        }
        CHECK(native.size() == 3, "%zu lines", native.size());
        if (native.size() == 3) {
            CHECK(native[0] == "native asks: no module yet", "%s", native[0].c_str());
            CHECK(native[1] == "native told when loaded: the module gives dlsym this image's function, as SEUpdaterGetModule", "%s", native[1].c_str());
            CHECK(native[2] == "native told at once: the module gives dlsym this image's function, as SEUpdaterGetModule", "%s", native[2].c_str());
        }
        // The module was told before the game's main() began.
        CHECK(run.Lines.size() >= 2 && run.Lines.back().second == "main", "%s", run.Lines.empty() ? "" : run.Lines.back().second.c_str());
    }
    {
        // A settings file that is not JSON: Fail(), as on Windows. The game
        // ends at once with the box, and no build is loaded.
        Game g = MakeGame("loader-damaged-settings");
        WriteAll(g.Data + "/ScriptExtenderUpdaterConfig.json", "{ \"UpdateChannel\": ");
        std::string ui = g.Dir + "/ui.txt";
        Run run = Start(g, {}, 20, {}, { "BG3MSE_UPDATER_TEST_UI=" + ui }, 1);
        CHECK(run.Extender.empty() && run.Main < 0, "%s", run.Extender.c_str());
        CHECK(ReadUi(ui) == "error bg3mse: Failed to load configuration file '" + g.Data
            + "/ScriptExtenderUpdaterConfig.json'\n", "%s", ReadUi(ui).c_str());
        CHECK(UpdaterLog(g).find("[ERROR] Failed to load configuration file") != std::string::npos, "%s", UpdaterLog(g).c_str());
    }
    {
        // Shift held at the start: Debug, as if the setting were on.
        Game g = MakeGame("loader-shift");
        Serve("0.2.0");
        ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
        std::string staged = g.Data + "/Cache/Builds/" + MakeRelease("0.2.0").Entry.CacheName();
        Start(g, { "wait", staged, "15" }, 20, {}, { "BG3MSE_UPDATER_TEST_SHIFT=1" });
        CHECK(wait([&] { return UpdaterLog(g).find("downloaded and checked") != std::string::npos; }, 3));
        std::string log = UpdaterLog(g);
        CHECK(log.find("[DEBUG] Update channel: Release; Debug on, IPv4 only off") != std::string::npos, "%s", log.c_str());
        CHECK(log.find("[DEBUG] fetch: GET " + gServer.URL("/channel-Release/Manifest.json")) != std::string::npos, "%s", log.c_str());
        // And without it, none of that.
        Game h = MakeGame("loader-no-shift");
        Start(h);
        CHECK(UpdaterLog(h).find("[DEBUG]") == std::string::npos, "%s", UpdaterLog(h).c_str());
    }
}

// --- Norbyte's updater API as a tool outside the game calls it -----------------
//
// The tool is bg3mse_updater_api_tests --tool (ApiTests.cpp), a process that
// has nothing of the updater but what it loads.

void ToolApi()
{
    Game g = MakeGame("tool-api");
    Serve("0.2.0");
    Release r = MakeRelease("0.2.0").Entry;
    r.MinGameVersion = Version::Parse(kGame);
    ServeManifest(Signed(ManifestJson("Release", 1, { r })));
    std::string exe = gArgs.ApiTests;
    std::vector<std::string> args = { exe, "--tool", "--loader=" + g.MacOS + "/libbg3mse-loader.dylib", "--home=" + g.Home,
        "--bin=" + g.MacOS, "--url=" + gServer.URL("/channel-"), "--key=" + Hex(gPublic) };
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid;
    CHECK(posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv.data(), environ) == 0);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the tool's checks ended with status %d", status);
    CHECK(Exists(g.Data + "/Cache/Builds/" + MakeRelease("0.2.0").Entry.CacheName()));
}

// libbg3mse.dylib loaded another way as well (here inserted ahead of the
// loader, as the game's load command would load it): one copy only.
void LoaderSecondCopy()
{
    Game g = MakeGame("loader-second-copy");
    Serve("0.2.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
    std::string exe = g.MacOS + "/Baldur's Gate 3";
    std::string insert = "DYLD_INSERT_LIBRARIES=" + g.MacOS + "/libbg3mse.dylib:" + g.MacOS + "/libbg3mse-loader.dylib";
    std::string home = "HOME=" + g.Home, key = "BG3MSE_UPDATER_TEST_KEY=" + Hex(gPublic);
    char* envp[] = { insert.data(), home.data(), key.data(), nullptr };
    char* argv[] = { exe.data(), nullptr };
    int out[2];
    pipe(out);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    pid_t pid;
    posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv, envp);
    close(out[1]);
    std::string text;
    char buffer[256];
    ssize_t n;
    while ((n = read(out[0], buffer, sizeof(buffer))) > 0) text.append(buffer, (size_t)n);
    close(out[0]);
    int status;
    waitpid(pid, &status, 0);
    posix_spawn_file_actions_destroy(&actions);
    CHECK(text == "extender 0.1.0 " + g.MacOS + "/libbg3mse.dylib\nmain\n", "%s", text.c_str());
    CHECK(UpdaterLog(g).find("loaded already") != std::string::npos, "%s", UpdaterLog(g).c_str());
    CHECK(gServer.Hits(ManifestPath()) == 0);
}

// Outside the game, and with BG3MSE_DISABLE, the loader does nothing.
void LoaderElsewhere()
{
    Game g = MakeGame("loader-elsewhere");
    WriteAll(g.MacOS + "/Baldur's Gate 3 Tool", ReadAll(gArgs.StandIn));
    chmod((g.MacOS + "/Baldur's Gate 3 Tool").c_str(), 0755);
    std::string exe = g.MacOS + "/Baldur's Gate 3 Tool";
    std::string insert = "DYLD_INSERT_LIBRARIES=" + g.MacOS + "/libbg3mse-loader.dylib";
    std::string home = "HOME=" + g.Home;
    char* envp[] = { insert.data(), home.data(), nullptr };
    char* argv[] = { exe.data(), nullptr };
    pid_t pid;
    int out[2];
    pipe(out);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv, envp);
    close(out[1]);
    std::string text;
    char buffer[256];
    ssize_t n;
    while ((n = read(out[0], buffer, sizeof(buffer))) > 0) text.append(buffer, (size_t)n);
    close(out[0]);
    int status;
    waitpid(pid, &status, 0);
    CHECK(text == "main\n", "%s", text.c_str());
    CHECK(!Exists(g.Data + "/logs"));
}


// The launch option's install (Mac/Installer/Setup.h) and the updater, on
// one data directory. The installer owns bin/: the launcher, the loader, the
// build beside it. The updater owns Cache/. bin/ is replaced by renaming,
// never written in place, so a mapped file keeps its bytes; the cache is
// left as it is; and the loader goes on choosing between the two by version.
void LauncherSwap()
{
    namespace installer = bg3se::mac::installer;
    Game g = MakeGame("launcher-swap");
    unlink((g.MacOS + "/libbg3mse-loader.dylib").c_str());
    unlink((g.MacOS + "/libbg3mse.dylib").c_str());
    auto release = [&](std::string const& version) {
        std::string dir = g.Dir + "/release-" + version;
        WriteAll(dir + "/bg3mse-launch", ReadAll(gArgs.Launcher));
        WriteAll(dir + "/bg3mse-install", ReadAll(gArgs.StandIn));
        WriteAll(dir + "/libbg3mse-loader.dylib", ReadAll(gArgs.Loader));
        WriteAll(dir + "/libbg3mse.dylib", ReadAll(gArgs.Fakes.at(version)));
        return dir;
    };
    auto setup = [&](std::string const& from, bool realGames) {
        FILE* out = tmpfile();
        installer::Host host;
        host.AppleSilicon = true;
        host.DataDirectory = g.Data;
        host.Out = out;
        host.Clipboard = [](std::string const&, std::string&) { return true; };
        host.RunningGames = [realGames, data = g.Data] { return realGames ? installer::RunningGames(data) : std::vector<std::string>(); };
        installer::Options options;
        options.What = installer::Verb::Install;
        options.From = from;
        int status = installer::Run(options, host);
        fflush(out);
        fseek(out, 0, SEEK_END);
        std::string text((size_t)ftell(out), '\0');
        fseek(out, 0, SEEK_SET);
        if (fread(text.data(), 1, text.size(), out) != text.size()) text.clear();
        fclose(out);
        return std::make_pair(status, text);
    };
    auto inode = [](std::string const& path) {
        struct stat st{};
        return stat(path.c_str(), &st) == 0 ? st.st_ino : 0;
    };
    // Every file under the cache with its inode and bytes.
    auto tree = [&](std::string const& dir) {
        std::map<std::string, std::pair<ino_t, std::string>> files;
        std::function<void(std::string const&)> walk = [&](std::string const& d) {
            if (DIR* h = opendir(d.c_str())) {
                while (dirent* e = readdir(h)) {
                    std::string name = e->d_name, path = d + "/" + name;
                    if (name == "." || name == "..") continue;
                    struct stat st{};
                    lstat(path.c_str(), &st);
                    if (S_ISDIR(st.st_mode)) walk(path);
                    else files[path] = { st.st_ino, ReadAll(path) };
                }
                closedir(h);
            }
        };
        walk(dir);
        return files;
    };

    auto first = setup(release("0.1.0"), false);
    CHECK(first.first == 0, "%s", first.second.c_str());
    char resolved[PATH_MAX];
    std::string bin = std::string(realpath((g.Data + "/bin").c_str(), resolved) ? resolved : "") + "", launcher = bin + "/bg3mse-launch";
    std::map<std::string, ino_t> installed;
    for (auto name : { "bg3mse-launch", "libbg3mse-loader.dylib", "libbg3mse.dylib" }) installed[name] = inode(bin + "/" + name);

    // Through the launcher: bin/'s build, and the update fetched into the cache.
    Serve("0.2.0");
    ServeManifest(Signed(ManifestJson("Release", 1, { MakeRelease("0.2.0").Entry })));
    std::string staged = g.Data + "/Cache/Builds/" + MakeRelease("0.2.0").Entry.CacheName();
    Run run = Start(g, {}, 20, launcher);
    CHECK(run.Extender == "0.2.0 " + staged + "/libbg3mse.dylib", "%s", run.Extender.c_str());
    // The updater changed nothing in bin/.
    for (auto const& [name, ino] : installed) CHECK(inode(bin + "/" + name) == ino, "%s was replaced", name.c_str());
    run = Start(g, {}, 20, launcher);
    CHECK(run.Extender == "0.2.0 " + staged + "/libbg3mse.dylib", "%s", run.Extender.c_str());
    CHECK(ReadAll(g.Data + "/logs/launcher.log").find("inserted \"" + bin + "/libbg3mse-loader.dylib\"") != std::string::npos);

    // A newer release swapped into bin/ while the old build is mapped, as
    // in a game that is running: the mapping keeps its bytes, the cache is
    // left alone, and the old files are kept in bin.previous.
    std::string old = ReadAll(bin + "/libbg3mse.dylib");
    int fd = open((bin + "/libbg3mse.dylib").c_str(), O_RDONLY);
    void* mapped = fd >= 0 ? mmap(nullptr, old.size(), PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    if (fd >= 0) close(fd);
    CHECK(mapped != MAP_FAILED);
    auto cache = tree(g.Data + "/Cache");
    CHECK(!cache.empty());
    auto second = setup(release("0.3.0"), false);
    CHECK(second.first == 0, "%s", second.second.c_str());
    if (mapped != MAP_FAILED) {
        CHECK(memcmp(mapped, old.data(), old.size()) == 0, "the mapped build changed under it");
        munmap(mapped, old.size());
    }
    CHECK(tree(g.Data + "/Cache") == cache, "the installer changed the updater's cache");
    CHECK(ReadAll(bin + "/libbg3mse.dylib") == ReadAll(gArgs.Fakes.at("0.3.0")));
    CHECK(inode(bin + "/libbg3mse.dylib") != installed["libbg3mse.dylib"]);
    CHECK(inode(g.Data + "/bin.previous/libbg3mse.dylib") == installed["libbg3mse.dylib"]);
    CHECK(ReadAll(g.Data + "/bin.previous/libbg3mse.dylib") == old);

    // The loader picks bin/'s 0.3.0 over the cached 0.2.0.
    run = Start(g, {}, 20, launcher);
    CHECK(run.Extender == "0.3.0 " + bin + "/libbg3mse.dylib", "%s", run.Extender.c_str());

    // While the game runs, the installer refuses and changes nothing.
    std::string go = g.Dir + "/go";
    std::string app = g.Dir + "/Baldur's Gate 3.app", home = "HOME=" + g.Home, key = "BG3MSE_UPDATER_TEST_KEY=" + Hex(gPublic);
    std::string waitFor = go, seconds = "20", word = "wait";
    char* argv[] = { launcher.data(), app.data(), word.data(), waitFor.data(), seconds.data(), nullptr };
    char* envp[] = { home.data(), key.data(), nullptr };
    pid_t pid;
    posix_spawn_file_actions_t quiet;
    posix_spawn_file_actions_init(&quiet);
    posix_spawn_file_actions_addopen(&quiet, 1, "/dev/null", O_WRONLY, 0);
    CHECK(posix_spawn(&pid, launcher.c_str(), &quiet, nullptr, argv, envp) == 0);
    posix_spawn_file_actions_destroy(&quiet);
    std::string exe = std::string(realpath(g.MacOS.c_str(), resolved) ? resolved : "") + "/Baldur's Gate 3";
    bool seen = false;
    for (int i = 0; i < 100 && !seen; i++) {
        for (auto const& running : installer::RunningGames(g.Data)) seen = seen || running == exe;
        if (!seen) usleep(50000);
    }
    CHECK(seen, "the running stand-in is not found among the games");
    // Another data directory's install is not changed under it: there, it is not a game that runs.
    for (auto const& running : installer::RunningGames(g.Home + "/another bg3mse")) CHECK(running != exe, "%s", running.c_str());
    ino_t now = inode(bin + "/libbg3mse.dylib");
    auto refused = setup(release("0.1.0"), true);
    CHECK(refused.first == 1 && refused.second.find("the game is running") != std::string::npos, "%s", refused.second.c_str());
    CHECK(inode(bin + "/libbg3mse.dylib") == now);
    WriteAll(go, "");
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "status %d", status);
}
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    gArgs.Self = argv[0];
    std::map<std::string, std::string> child;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a.starts_with("--") && a.find('=') != std::string::npos) child[a.substr(2, a.find('=') - 2)] = a.substr(a.find('=') + 1);
    }
    if (child.count("child")) return ChildMain(child);
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto value = [&](char const* key) -> std::optional<std::string> {
            std::string k = std::string("--") + key + "=";
            if (a.starts_with(k)) return a.substr(k.size());
            return {};
        };
        if (auto v = value("scratch")) gArgs.Scratch = *v;
        else if (auto v = value("loader")) gArgs.Loader = *v;
        else if (auto v = value("offline-loader")) gArgs.OfflineLoader = *v;
        else if (auto v = value("stand-in")) gArgs.StandIn = *v;
        else if (auto v = value("launcher")) gArgs.Launcher = *v;
        else if (auto v = value("cmake")) gArgs.CMake = *v;
        else if (auto v = value("zip-script")) gArgs.ZipScript = *v;
        else if (auto v = value("api-tests")) gArgs.ApiTests = *v;
        else if (a.starts_with("--fake-")) {
            size_t eq = a.find('=');
            gArgs.Fakes[a.substr(7, eq - 7)] = a.substr(eq + 1);
        } else {
            fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    if (gArgs.Scratch.empty() || gArgs.Loader.empty() || gArgs.OfflineLoader.empty() || gArgs.StandIn.empty() || gArgs.Launcher.empty() ||
        gArgs.Fakes.size() != 3) {
        fprintf(stderr, "usage: bg3mse_updater_tests --scratch= --loader= --offline-loader= --stand-in= --launcher= --fake-<v>=... --cmake= --zip-script=\n");
        return 2;
    }
    RemoveTree(gArgs.Scratch);
    if (!gServer.Start() || !MakeKeyPair(gPublic, gPrivate) || !MakeKeyPair(gOtherPublic, gOtherPrivate)) {
        fprintf(stderr, "cannot start the server or make the test keys\n");
        return 2;
    }

    auto timed = [](char const* name, void (*test)()) {
        auto start = Clock::now();
        int before = gFailures;
        test();
        printf("%-16s %s (%.1f s)\n", name, gFailures == before ? "ok" : "FAILED",
            std::chrono::duration<double>(Clock::now() - start).count());
    };
    timed("basics", Basics);
    timed("good", Good);
    timed("refusals", Refusals);
    timed("replay", Replay);
    timed("older", Older);
    timed("damaged-cache", DamagedCache);
    timed("pins", Pins);
    timed("failed-download", FailedDownload);
    timed("no-fallback", NoFallback);
    timed("bad-zips", BadZips);
    timed("full-disk", FullDisk);
    timed("killed", Killed);
    timed("transfers", Transfers);
    timed("status-cancel", StatusAndCancel);
    timed("loader-good", LoaderGood);
    timed("loader-no-build-fits", LoaderNoBuildFits);
    timed("loader-offline", LoaderOffline);
    timed("loader-failures", LoaderFailures);
    timed("loader-notices", LoaderNotices);
    timed("tool-api", ToolApi);
    timed("loader-second", LoaderSecondCopy);
    timed("loader-elsewhere", LoaderElsewhere);
    timed("launcher-swap", LauncherSwap);
    printf("%d checks, %d failed\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
