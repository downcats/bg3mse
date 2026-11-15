#include <Updater/Updater.h>
#include <Updater/Files.h>
#include <Updater/Stamp.h>
#include <Updater/Zip.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <sys/file.h>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

namespace bg3se::mac::updater {

namespace {

constexpr uint64_t kManifestFileLimit = 4u << 20;

bool ChannelNameOk(std::string const& channel)
{
    return !channel.empty() && channel.size() <= 64
        && std::all_of(channel.begin(), channel.end(), [](char c) { return isalnum((unsigned char)c); });
}

std::span<uint8_t const> Bytes(std::string const& s)
{
    return { reinterpret_cast<uint8_t const*>(s.data()), s.size() };
}

// Verifies and reads a signed manifest; the channel must be the one asked for.
ParseResult ReadSigned(std::span<uint8_t const> file, Environment const& env, ChannelManifest& m, std::string& why)
{
    std::span<uint8_t const> payload;
    if (!Verify(file, *env.Key, payload, why)) return ParseResult::Invalid;
    auto result = ParseChannelManifest(std::string_view(reinterpret_cast<char const*>(payload.data()), payload.size()), m, why);
    if (result != ParseResult::Ok) return result;
    if (m.Channel != env.Settings.UpdateChannel) {
        why = "it is the manifest of the " + m.Channel + " channel, not of " + env.Settings.UpdateChannel;
        return ParseResult::Invalid;
    }
    return ParseResult::Ok;
}

// The last manifest accepted; false when there is none that verifies.
bool LoadCachedManifest(Environment const& env, ChannelManifest& m, std::string& bytes, std::string& why)
{
    switch (ReadFile(ManifestCachePath(env.Settings), bytes, kManifestFileLimit)) {
    case ReadResult::Ok: break;
    case ReadResult::Missing: why = "no update has been fetched yet"; return false;
    default: why = "the cached manifest cannot be read"; return false;
    }
    if (ReadSigned(Bytes(bytes), env, m, why) != ParseResult::Ok) {
        why = "the cached manifest is not valid: " + why;
        return false;
    }
    return true;
}

ReleaseFile const* FindFile(ReleaseManifest const& m, std::string const& path)
{
    for (auto const& f : m.Files) {
        if (f.Path == path) return &f;
    }
    return nullptr;
}

bool HashMatches(std::string const& path, ReleaseFile const& f, std::string& why)
{
    Digest d;
    uint64_t size = 0;
    if (!Sha256File(path, d, &size)) {
        why = "cannot read " + path;
        return false;
    }
    if (size != f.Size || Hex(d) != f.Sha256) {
        why = path + " is not the file the release lists";
        return false;
    }
    return true;
}

// The libraries of a cached build are what its manifest.json, which the
// signed channel manifest names by digest, lists.
bool VerifyBuild(std::string const& dir, Release const& r, std::string& why)
{
    std::string text;
    if (ReadFile(dir + "/manifest.json", text, kManifestFileLimit) != ReadResult::Ok) {
        why = "its manifest.json cannot be read";
        return false;
    }
    if (Hex(Sha256(Bytes(text))) != r.ManifestDigest) {
        why = "its manifest.json is not the release's";
        return false;
    }
    ReleaseManifest m;
    if (!ParseReleaseManifest(text, m, why)) return false;
    ReleaseFile const* library = FindFile(m, kLibrary);
    if (!library) {
        why = "the release has no libbg3mse.dylib";
        return false;
    }
    if (!HashMatches(dir + "/" + kLibrary, *library, why)) return false;
    if (ReleaseFile const* camera = FindFile(m, std::string("camera/") + kCameraLibrary)) {
        if (!HashMatches(dir + "/" + kCameraLibrary, *camera, why)) return false;
    }
    return true;
}

struct Lock
{
    int Fd{ -1 };
    ~Lock() { if (Fd >= 0) close(Fd); }
};

bool SafeComponent(std::string_view c)
{
    return !c.empty() && c != "." && c != ".." && c.find('\\') == std::string_view::npos && c.find(':') == std::string_view::npos;
}

// Unpacks the verified zip into `stage` and checks every file against the
// release's manifest.json, whose digest the signed manifest gives.
bool Unpack(std::span<uint8_t const> zip, Release const& r, std::string const& stage, std::string& why)
{
    ZipReader reader;
    if (!reader.Open(zip, why)) return false;
    std::string prefix = "bg3mse-" + r.ReleaseVersion.ToString() + "/";
    ZipEntry const* manifestEntry = nullptr;
    std::set<std::string> names;
    for (auto const& e : reader.Entries()) {
        if (!e.Name.starts_with(prefix)) {
            why = "the zip holds " + e.Name + ", outside " + prefix;
            return false;
        }
        std::string_view rest = std::string_view(e.Name).substr(prefix.size());
        size_t at = 0;
        while (true) {
            size_t slash = rest.find('/', at);
            if (!SafeComponent(rest.substr(at, slash == std::string_view::npos ? std::string_view::npos : slash - at))) {
                why = "the zip holds an entry with a name that is not allowed: " + e.Name;
                return false;
            }
            if (slash == std::string_view::npos) break;
            at = slash + 1;
        }
        if (e.Mode && !S_ISREG(e.Mode)) {
            why = "the zip entry " + e.Name + " is not a plain file";
            return false;
        }
        if (!names.insert(std::string(rest)).second) {
            why = "the zip holds " + e.Name + " twice";
            return false;
        }
        if (rest == "manifest.json") manifestEntry = &e;
    }
    if (!manifestEntry) {
        why = "the zip has no manifest.json";
        return false;
    }
    if (!MakeDirectories(stage, why)) return false;
    if (!reader.Extract(*manifestEntry, stage + "/manifest.json", why)) return false;
    std::string text;
    if (ReadFile(stage + "/manifest.json", text, kManifestFileLimit) != ReadResult::Ok || Hex(Sha256(Bytes(text))) != r.ManifestDigest) {
        why = "the zip's manifest.json is not the one the channel manifest names";
        return false;
    }
    ReleaseManifest m;
    if (!ParseReleaseManifest(text, m, why)) return false;
    if (m.ReleaseVersion != r.ReleaseVersion || m.Architecture != kArchitecture) {
        why = "the zip's manifest.json is of another build";
        return false;
    }
    if (m.Files.size() + 1 != names.size()) {
        why = "the zip and its manifest.json do not list the same files";
        return false;
    }
    for (auto const& e : reader.Entries()) {
        if (&e == manifestEntry) continue;
        std::string rel = e.Name.substr(prefix.size());
        ReleaseFile const* f = FindFile(m, rel);
        if (!f || f->Size != e.Size) {
            why = "the zip holds " + rel + ", which its manifest.json does not list as it is";
            return false;
        }
        std::string path = stage + "/" + rel;
        size_t slash = path.rfind('/');
        if (!MakeDirectories(path.substr(0, slash), why)) return false;
        if (!reader.Extract(e, path, why)) return false;
        if (!HashMatches(path, *f, why)) return false;
        chmod(path.c_str(), (e.Mode & 0111) ? 0755 : 0644);
    }
    if (!FindFile(m, kLibrary)) {
        why = "the release has no libbg3mse.dylib";
        return false;
    }
    // libbg3mse.dylib loads the camera module from its own directory.
    std::string camera = std::string("camera/") + kCameraLibrary;
    if (FindFile(m, camera) && link((stage + "/" + camera).c_str(), (stage + "/" + kCameraLibrary).c_str()) != 0) {
        why = "cannot place " + std::string(kCameraLibrary) + ": " + Errno();
        return false;
    }
    SyncDirectory(stage);
    return true;
}

}

std::string ManifestCachePath(Config const& c)
{
    return c.CachePath + "/Manifest-" + c.UpdateChannel + ".json";
}

std::string BuildsDirectory(Config const& c)
{
    return c.CachePath + "/Builds";
}

Choice ChooseBuild(Environment const& env)
{
    Choice bundled;
    bundled.Path = env.BundledLibrary;
    Stamp stamp;
    std::string why;
    if (ReadStamp(env.BundledLibrary, stamp, why)) {
        if (auto v = Version::Parse(stamp.Version)) bundled.Loaded = *v;
    }
    auto useBundled = [&](std::string note) {
        bundled.Note = std::move(note);
        return bundled;
    };

    Config const& c = env.Settings;
    if (c.DebugLoadSE) return useBundled("DebugLoadSE is set");
    if (!env.Key) return useBundled("this build has no update key");
    if (c.CachePath.empty()) return useBundled("there is no data directory");
    if (!ChannelNameOk(c.UpdateChannel)) return useBundled("UpdateChannel '" + c.UpdateChannel + "' is not a channel name");

    ChannelManifest m;
    std::string bytes;
    if (!LoadCachedManifest(env, m, bytes, why)) return useBundled(why);

    // The bundled build is fit unless the manifest says otherwise of its version.
    bool bundledFit = !bundled.Loaded.Parts.empty();
    bool bundledRefused = false;
    for (auto const& r : m.Releases) {
        if (bundledFit && r.ReleaseVersion == bundled.Loaded && !Unsuitable(r, env.Here).empty()) {
            bundledFit = false;
            bundledRefused = true;
            bundled.Note = "the build beside the loader (" + bundled.Loaded.ToString() + ") is not for here: " + Unsuitable(r, env.Here);
        }
        if (bundledFit && r.ReleaseVersion == bundled.Loaded) bundled.Notice = r.Notice;
    }

    // The cached candidates, best first.
    std::vector<Release const*> candidates;
    for (auto const& r : m.Releases) {
        if (!c.TargetResourceDigest.empty()) {
            if (r.Digest != c.TargetResourceDigest || r.Revoked) continue;
        } else {
            if (!Unsuitable(r, env.Here).empty()) continue;
            if (c.TargetVersion && r.ReleaseVersion != *c.TargetVersion) continue;
        }
        candidates.push_back(&r);
    }
    std::sort(candidates.begin(), candidates.end(), [](Release const* a, Release const* b) {
        return a->ReleaseVersion != b->ReleaseVersion ? a->ReleaseVersion > b->ReleaseVersion : a->BuildDate > b->BuildDate;
    });

    bool pinned = !c.TargetResourceDigest.empty() || c.TargetVersion;
    bool bundledMeetsPin = c.TargetResourceDigest.empty() && c.TargetVersion && bundled.Loaded == *c.TargetVersion;
    std::string notes;
    for (Release const* r : candidates) {
        if (!pinned && bundledFit && r->ReleaseVersion <= bundled.Loaded) break;
        if (bundledMeetsPin) break;
        std::string dir = BuildsDirectory(c) + "/" + r->CacheName();
        struct stat st{};
        if (stat(dir.c_str(), &st) != 0) continue;
        if (!VerifyBuild(dir, *r, why)) {
            notes += "cached build " + r->ReleaseVersion.ToString() + " not used: " + why + "; ";
            continue;
        }
        Choice cached;
        cached.Path = dir + "/" + kLibrary;
        cached.Loaded = r->ReleaseVersion;
        cached.Cached = true;
        cached.Digest = r->Digest;
        cached.Notice = r->Notice;
        cached.Note = notes;
        if (bundledRefused) {
            cached.NoFallback = !m.NoMatchingVersionNotice.empty() ? m.NoMatchingVersionNotice
                                                                   : "No extender version found for game version v" + env.Here.Game.ToString();
        }
        return cached;
    }
    if (pinned && !bundledMeetsPin) {
        bundled.Problem = !c.TargetResourceDigest.empty()
            ? "The requested Script Extender version ID (" + c.TargetResourceDigest + ") is not available."
            : "The requested Script Extender version (" + c.TargetVersion->ToString() + ") is not available.";
        bundled.Problem += "\nThis is likely caused by an invalid version setting in ScriptExtenderUpdaterConfig.json";
    }
    if (!notes.empty()) bundled.Note = notes + bundled.Note;
    if (bundledRefused && !pinned) {
        // Norbyte's FindLoadableResource finds nothing: nothing is loaded, and
        // UpdateErrorText's words are the player's.
        Choice none;
        none.Note = bundled.Note;
        none.Problem = !m.NoMatchingVersionNotice.empty() ? m.NoMatchingVersionNotice
                                                          : "No extender version found for game version v" + env.Here.Game.ToString();
        return none;
    }
    return bundled;
}

Outcome Update(Environment const& env, Choice const& loaded)
{
    Outcome out;
    Config const& c = env.Settings;
    auto fail = [&](Category what, std::string message) {
        out.What = what;
        out.Message = std::move(message);
        return out;
    };
    if (c.DisableUpdates) return out;
    if (!env.Key) {
        // A developer's build, or a game whose version is not known: not a failure to show.
        out.Message = "no update key: nothing is fetched";
        return out;
    }
    if (c.CachePath.empty()) return fail(Category::LocalUpdate, "there is no data directory to keep updates in");
    if (!ChannelNameOk(c.UpdateChannel)) return fail(Category::General, "UpdateChannel '" + c.UpdateChannel + "' is not a channel name");

    std::string why;
    if (!MakeDirectories(BuildsDirectory(c), why)) return fail(Category::LocalUpdate, why);
    Lock lock;
    lock.Fd = open((c.CachePath + "/.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock.Fd < 0) return fail(Category::LocalUpdate, "cannot open the cache's lock: " + Errno());
    if (flock(lock.Fd, LOCK_EX | LOCK_NB) != 0) {
        out.Message = "another game is updating the cache; this one leaves it to that one";
        return out;
    }
    std::string staging = c.CachePath + "/Staging";
    RemoveTree(staging);

    auto status = [&](std::string const& text) {
        if (env.Status) env.Status(text);
    };
    auto cancelled = [&] {
        out.Cancelled = true;
        out.Message = "cancelled";
        return out;
    };

    // The manifest.
    std::vector<uint8_t> body;
    std::string manifestURL = c.ManifestLocation();
    status("Fetching manifest");
    FetchResult fetched = Fetch(manifestURL, env.ManifestLimits, body, env.Cancel, env.Fetching);
    if (fetched.Failure == FetchFailure::Cancelled) return cancelled();
    if (fetched.Failure != FetchFailure::None) {
        out.InternetIssue = fetched.IsInternetIssue();
        return fail(Category::ManifestFetch, "Unable to fetch manifest - " + fetched.Message);
    }
    ChannelManifest m;
    switch (ReadSigned(body, env, m, why)) {
    case ParseResult::Ok: break;
    case ParseResult::TooNew: return fail(Category::UpdateRequired, "the channel manifest is newer than this loader reads: " + why);
    default: return fail(Category::ManifestFetch, "the channel manifest from " + manifestURL + " is refused: " + why);
    }

    // Not older than the last one accepted (a replay could bring back a
    // revoked build), and not another manifest under the same number.
    ChannelManifest previous;
    std::string previousBytes;
    if (LoadCachedManifest(env, previous, previousBytes, why)) {
        if (m.Sequence < previous.Sequence) {
            return fail(Category::ManifestFetch, "the channel manifest is older (sequence " + std::to_string(m.Sequence)
                + ") than the one already accepted (" + std::to_string(previous.Sequence) + "); it is refused");
        }
        if (m.Sequence == previous.Sequence && previousBytes != std::string(body.begin(), body.end())) {
            return fail(Category::ManifestFetch, "the channel manifest differs from the one accepted under the same sequence "
                + std::to_string(m.Sequence) + "; it is refused");
        }
    }
    if (previousBytes != std::string(body.begin(), body.end()) && !WriteFileAtomically(ManifestCachePath(c), body, why)) {
        return fail(Category::LocalUpdate, why);
    }
    out.Notice = m.Notice;

    // Builds the manifest no longer lists, or revokes, go (Norbyte's
    // UpdateFromManifest); the one loaded stays until the next start.
    std::set<std::string> listed;
    for (auto const& r : m.Releases) {
        if (!r.Revoked) listed.insert(r.CacheName());
    }
    if (DIR* dir = opendir(BuildsDirectory(c).c_str())) {
        std::vector<std::string> stale;
        while (dirent* e = readdir(dir)) {
            std::string name = e->d_name;
            if (name == "." || name == ".." || listed.count(name)) continue;
            if (loaded.Cached && loaded.Path.starts_with(BuildsDirectory(c) + "/" + name + "/")) continue;
            stale.push_back(name);
        }
        closedir(dir);
        for (auto const& name : stale) RemoveTree(BuildsDirectory(c) + "/" + name);
    }
    // And the marks of failed downloads of builds no longer listed.
    if (DIR* dir = opendir(c.CachePath.c_str())) {
        std::vector<std::string> stale;
        while (dirent* e = readdir(dir)) {
            std::string name = e->d_name;
            if (name.starts_with(kFailedPrefix) && !listed.count(name.substr(strlen(kFailedPrefix)))) stale.push_back(name);
        }
        closedir(dir);
        for (auto const& name : stale) unlink((c.CachePath + "/" + name).c_str());
    }

    Release const* target = Choose(m, env.Here, c.TargetVersion, c.TargetResourceDigest);
    if (!target) {
        if (!c.TargetResourceDigest.empty() || c.TargetVersion) {
            return fail(Category::NoMatchingVersion, !c.TargetResourceDigest.empty()
                ? "The requested Script Extender version ID (" + c.TargetResourceDigest + ") is not available."
                : "The requested Script Extender version (" + c.TargetVersion->ToString() + ") is not available.");
        }
        if (!m.NoMatchingVersionNotice.empty()) return fail(Category::NoMatchingVersion, m.NoMatchingVersionNotice);
        return fail(Category::NoMatchingVersion, "No bg3mse version supports this game version yet (v"
            + env.Here.Game.ToString() + ").");
    }
    std::string final = BuildsDirectory(c) + "/" + target->CacheName();
    struct stat st{};
    if (stat(final.c_str(), &st) == 0) {
        if (VerifyBuild(final, *target, why)) return out;     // already there
        // Damaged since it was put there: fetched again, unless it is in use.
        if (loaded.Cached && loaded.Digest == target->Digest) return fail(Category::LocalUpdate, "the build in use is damaged: " + why);
        if (!RemoveTree(final)) return fail(Category::LocalUpdate, "cannot remove the damaged build " + final + ": " + Errno());
    }
    bool loadedFit = !loaded.Loaded.Parts.empty();
    for (auto const& r : m.Releases) {
        if (r.ReleaseVersion == loaded.Loaded && !Unsuitable(r, env.Here).empty()) loadedFit = false;
    }
    bool pinned = !c.TargetResourceDigest.empty() || c.TargetVersion;
    if (!pinned && loadedFit && target->ReleaseVersion <= loaded.Loaded) return out;     // nothing newer
    if (pinned && loaded.Cached && loaded.Digest == target->Digest) return out;

    // The build, unless its download failed less than a day ago.
    std::string failedMark = c.CachePath + "/" + kFailedPrefix + target->CacheName();
    if (stat(failedMark.c_str(), &st) == 0 && time(nullptr) - st.st_mtime < kRetryAfterSeconds) {
        char when[64] = "";
        tm local{};
        time_t at = st.st_mtime;
        if (localtime_r(&at, &local)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &local);
        out.Message = "the download of " + target->ReleaseVersion.ToString() + " did not succeed at " + when + "; it is tried again a day after";
        return out;
    }
    auto failedDownload = [&] {
        int fd = open(failedMark.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd >= 0) close(fd);
    };
    std::string zipURL = ResolveURL(manifestURL, target->URL);
    if (zipURL.empty()) return fail(Category::UpdateDownload, "the manifest's URL for " + target->ReleaseVersion.ToString() + " is not valid");
    FetchLimits limits = env.PackageLimits;
    limits.MaxBytes = target->Size;
    status("Downloading update: " + target->ReleaseVersion.ToString());
    fetched = Fetch(zipURL, limits, body, env.Cancel, env.Fetching);
    if (fetched.Failure == FetchFailure::Cancelled) {
        failedDownload();
        return cancelled();
    }
    if (fetched.Failure != FetchFailure::None) {
        failedDownload();
        out.InternetIssue = fetched.IsInternetIssue();
        return fail(Category::UpdateDownload, "Unable to download package - " + fetched.Message);
    }
    if (body.size() != target->Size) {
        failedDownload();
        return fail(Category::UpdateDownload, "the package is " + std::to_string(body.size()) + " bytes, not the "
            + std::to_string(target->Size) + " the manifest gives: it is cut short or damaged");
    }
    if (Hex(Sha256(body)) != target->Digest) {
        failedDownload();
        return fail(Category::UpdateDownload, "the package's SHA-256 is not the one the manifest gives: it is damaged or was changed");
    }
    unlink(failedMark.c_str());

    std::string stage = staging + "/" + target->CacheName();
    status("Unpacking update: " + target->ReleaseVersion.ToString());
    if (!Unpack(body, *target, stage, why)) {
        RemoveTree(staging);
        return fail(Category::LocalUpdate, why);
    }
    if (rename(stage.c_str(), final.c_str()) != 0) {
        why = "cannot move the build into the cache: " + Errno();
        RemoveTree(staging);
        return fail(Category::LocalUpdate, why);
    }
    SyncDirectory(BuildsDirectory(c));
    RemoveTree(staging);
    out.Staged = target->ReleaseVersion.ToString();
    return out;
}

std::string DisplayText(Outcome const& o, Choice const& loaded)
{
    std::string text;
    switch (o.What) {
    case Category::None:
        break;
    case Category::UpdateDownload:
        text = o.InternetIssue ? "Couldn't download the bg3mse update. Check your internet connection.\n"
                               : "Couldn't download the bg3mse update.\n";
        text += o.Message;
        break;
    case Category::LocalUpdate:
        text = "Couldn't install the bg3mse update:\n" + o.Message;
        break;
    case Category::NoMatchingVersion:
        text = o.Message;
        break;
    case Category::UpdateRequired:
        text = "This bg3mse is too old to update itself. Download the latest release.";
        break;
    case Category::General:
    case Category::ManifestFetch:
        text = o.InternetIssue ? "bg3mse update failed. Check your internet connection.\n"
                               : "bg3mse update failed.\n";
        text += o.Message;
        break;
    }
    if (!text.empty()) {
        if ((o.What == Category::UpdateDownload || o.What == Category::ManifestFetch) && !loaded.Loaded.Parts.empty()) {
            text += "\n\nUsing the last downloaded version (v" + loaded.Loaded.ToString() + ").";
        }
        return text;
    }
    if (!loaded.Problem.empty()) return loaded.Problem;
    if (!loaded.Notice.empty()) return loaded.Notice;
    return o.Notice;
}

}
