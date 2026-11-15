#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the updater reads.
//
// The channel manifest, Manifest.json of a channel, is the Mac form of
// Norbyte's Channels/<channel>/Manifest.json (BG3Updater/Manifest.h): the
// builds of one channel, each with the game versions it is for, signed as a
// whole (Updater/Signature.h). It is a release asset of the tag
// "channel-<Channel>"; each build is the zip the release target makes
// (Mac/Release/Assemble.cmake), an asset of its own release, and the
// manifest.json inside that zip lists every file with its size and SHA-256.
//
//   {
//     "ManifestVersion": 1,             a reader refuses a later version: it is too old
//     "Product": "bg3mse",
//     "Channel": "Release",             must be the channel that was asked for
//     "Sequence": 12,                   raised by one for every manifest of the channel;
//                                       a reader never accepts a lower one again
//     "Notice": "...",                  optional, shown in the main menu
//     "NoMatchingVersionNotice": "...", optional, shown when no build is for this game
//     "Releases": [{
//       "Version": "0.2.0",             bg3mse's version
//       "Commit": "<40 hex>", "ScriptExtenderVersion": "34.0.0.0",
//       "URL": "../v0.2.0/bg3mse-0.2.0.zip",   absolute, or relative to the manifest's URL
//       "Size": 31457280, "Digest": "<SHA-256 of the zip>",
//       "ManifestDigest": "<SHA-256 of the zip's manifest.json>",
//       "MinGameVersion": "4.1.1.0",    optional, as Norbyte's; the game's CFBundleVersion
//       "MaxGameVersion": "4.1.1.9999999",    optional
//       "GameBuildChecked": "4.1.1.7631656",  the game the release's checks ran on
//       "MacOSMinimum": "11.0", "Architecture": "arm64",
//       "BuildDate": 1762900000,        seconds since 1970
//       "Revoked": false,               a revoked build is never loaded, and removed from the cache
//       "Notice": "..."                 optional, shown in the main menu while it is loaded
//     }]
//   }

namespace bg3se::mac::updater {

// A version of dotted numbers ("0.2.0", "4.1.1.7631656"); missing parts are 0.
struct Version
{
    std::vector<uint32_t> Parts;

    static std::optional<Version> Parse(std::string_view text);
    std::string ToString() const;
    int Compare(Version const& other) const;
    bool operator<(Version const& o) const { return Compare(o) < 0; }
    bool operator>(Version const& o) const { return Compare(o) > 0; }
    bool operator==(Version const& o) const { return Compare(o) == 0; }
    bool operator!=(Version const& o) const { return Compare(o) != 0; }
    bool operator<=(Version const& o) const { return Compare(o) <= 0; }
    bool operator>=(Version const& o) const { return Compare(o) >= 0; }
};

inline constexpr int32_t kManifestVersion = 1;
inline constexpr char kProduct[] = "bg3mse";
inline constexpr char kArchitecture[] = "arm64";

struct Release
{
    Version ReleaseVersion;
    std::string Commit;
    std::string ScriptExtenderVersion;
    std::string URL;
    uint64_t Size{ 0 };
    std::string Digest;              // 64 lower-case hex
    std::string ManifestDigest;      // 64 lower-case hex
    std::optional<Version> MinGameVersion, MaxGameVersion;
    std::string GameBuildChecked;
    Version MacOSMinimum;
    std::string Architecture;
    uint64_t BuildDate{ 0 };
    bool Revoked{ false };
    std::string Notice;

    // The name of its directory in the cache: "<version>_<digest>", as Norbyte's.
    std::string CacheName() const;
};

struct ChannelManifest
{
    int32_t ManifestVersion{ kManifestVersion };
    std::string Product{ kProduct };
    std::string Channel;
    uint64_t Sequence{ 0 };
    std::string Notice;
    std::string NoMatchingVersionNotice;
    std::vector<Release> Releases;
};

enum class ParseResult
{
    Ok,
    Invalid,
    TooNew,     // a later ManifestVersion: this reader must be updated
};

ParseResult ParseChannelManifest(std::string_view json, ChannelManifest& manifest, std::string& why);
std::string StringifyChannelManifest(ChannelManifest const& manifest);

// What a build is for.
struct Platform
{
    Version Game;        // the game's CFBundleVersion
    Version MacOS;       // the running system's
    std::string Architecture{ kArchitecture };
};

// Why `release` cannot be loaded here; empty when it can.
std::string Unsuitable(Release const& release, Platform const& platform);

// The newest release in `manifest` that can be loaded here, by version, then
// by build date (Norbyte's ordering puts the game range first; here every
// candidate is for this game). `version` and `digest`, when set, are the
// TargetVersion and TargetResourceDigest pins.
Release const* Choose(ChannelManifest const& manifest, Platform const& platform,
    std::optional<Version> const& version, std::string const& digest);

// The manifest.json of a release (Mac/Release/Assemble.cmake).
struct ReleaseFile
{
    std::string Path;
    uint64_t Size{ 0 };
    std::string Sha256;
};

struct ReleaseManifest
{
    std::string Name;           // bg3mse-<version>
    Version ReleaseVersion;
    std::string Commit;
    std::string Channel;
    std::string ScriptExtenderVersion;
    std::string GameBuildChecked;
    std::string MacOSMinimum;
    std::string Architecture;
    std::vector<ReleaseFile> Files;
};

bool ParseReleaseManifest(std::string_view json, ReleaseManifest& manifest, std::string& why);

}
