#pragma once

#include <Updater/Config.h>
#include <Updater/Fetch.h>
#include <Updater/Manifest.h>
#include <Updater/Signature.h>

#include <atomic>
#include <functional>
#include <optional>
#include <string>

// The updater (the Mac form of BG3Updater/Updater.cpp and Cache.cpp).
//
// ChooseBuild: local work only, no network. It reads the last channel
// manifest that was verified, and picks the newest build in the cache that
// the manifest lists, is not revoked, is for this game version, macOS and
// processor, and whose files still match; otherwise the build beside the
// loader (libbg3mse.dylib, what the installer put there: the Mac form of
// Norbyte's embedded package), unless the manifest says that build is not
// for here either: then none, as Norbyte's updater loads none.
//
// Update fetches the channel manifest, checks its signature before reading it, refuses one
// for another channel or older than the last one accepted, and, when it
// lists a newer build for this game than the one loaded, fetches that zip,
// checks its size and SHA-256 against the manifest and every file against
// the zip's manifest.json, unpacks it beside the cache and renames it into
// place. Anything that fails leaves the cache as it was.
//
// At start, before the game's main(), the loader (Mac/Updater/Loader.cpp)
// runs Update, then ChooseBuild, and loads what that chose: a build fetched
// is loaded by the start that fetched it.
//
// The cache (CachePath, <data directory>/Cache):
//   Manifest-<channel>.json     the last manifest accepted, as signed
//   Builds/<version>_<digest>/  a build, unpacked as its release lays it out, with
//                               libbg3mse-camera.dylib also beside libbg3mse.dylib
//   Staging/                    what is being unpacked; emptied at every update
//   Failed-<version>_<digest>   a build whose download failed, was cancelled or came damaged:
//                               the game waits for a download (Loader.cpp), so the same build is
//                               not fetched again until a day after; another build is, at once
//   .lock                       held while an update runs: two games do not update at once

namespace bg3se::mac::updater {

inline constexpr char kLibrary[] = "libbg3mse.dylib";
inline constexpr char kCameraLibrary[] = "libbg3mse-camera.dylib";
// The cache's mark of a build whose download did not succeed, and how long the same build is not fetched again.
inline constexpr char kFailedPrefix[] = "Failed-";
inline constexpr long kRetryAfterSeconds = 24 * 60 * 60;

// Norbyte's ErrorCategory (BG3Updater/Result.h), for the texts he shows.
enum class Category
{
    None,
    General,
    ManifestFetch,
    UpdateDownload,
    LocalUpdate,
    NoMatchingVersion,
    UpdateRequired,
};

struct Environment
{
    Config Settings;
    std::optional<PublicKey> Key;           // none: this build cannot check updates
    Platform Here;
    std::string BundledLibrary;              // libbg3mse.dylib beside the loader
    FetchLimits ManifestLimits{ 5.0, 5.0, 4u << 20 };
    FetchLimits PackageLimits{ 120.0, 30.0, 0 };    // MaxBytes is the release's size
    FetchOptions Fetching;                   // IPv4Only and Debug
    std::atomic<bool> const* Cancel{ nullptr };
    // Told what the update is doing, in Norbyte's words for his progress
    // window: "Fetching manifest", "Downloading update: <version>",
    // "Unpacking update: <version>".
    std::function<void(std::string const&)> Status;
};

struct Choice
{
    std::string Path;            // the libbg3mse.dylib to load; empty: none is for here
    Version Loaded;              // its version; empty when not known
    bool Cached{ false };
    std::string Digest;          // a cached build's zip SHA-256
    std::string Notice;          // the release's notice, from the manifest
    std::string Note;            // why the cache was not used, when it was not; for the log
    std::string Problem;         // what the player should be told (a pin that cannot be met, no build for here)
    // A cached build's: when the manifest says the build beside the loader is not for here either, what the player
    // is told if the cached one does not load, as nothing is loaded then. Empty: the build beside the loader is loaded.
    std::string NoFallback;
};

Choice ChooseBuild(Environment const& env);

struct Outcome
{
    Category What{ Category::None };
    bool InternetIssue{ false };
    std::string Message;         // the cause, for the log and the player
    std::string Staged;          // the version that was downloaded, if one was
    std::string Notice;          // the manifest's notice
    // Cancelled (Environment::Cancel) while it fetched: as on Windows not a
    // failure, and nothing to show.
    bool Cancelled{ false };
};

// One run of the update, as described above. `loaded` is what ChooseBuild chose.
Outcome Update(Environment const& env, Choice const& loaded);

// The text the player is shown for an outcome, as Norbyte's UpdateErrorText
// words it; empty when there is nothing to show.
std::string DisplayText(Outcome const& outcome, Choice const& loaded);

// The cache's paths.
std::string ManifestCachePath(Config const& c);
std::string BuildsDirectory(Config const& c);

}
