#pragma once

#include <Updater/Manifest.h>

#include <optional>
#include <string>

// The updater's settings: Norbyte's ScriptExtenderUpdaterConfig.json
// (BG3Updater/Config.h), same file name and keys, read from bg3mse's data
// directory (~/Library/Application Support/bg3mse) instead of the game's
// directory, as ScriptExtenderSettings.json is.
//
//   ManifestURL            prefix of the channel manifest's URL; the manifest is
//                          ManifestURL + UpdateChannel + "/" + ManifestName
//   ManifestName           "Manifest.json"
//   UpdateChannel          Release, Devel or Nightly
//   TargetVersion          load (and fetch) only this bg3mse version
//   TargetResourceDigest   load (and fetch) only the build whose zip has this SHA-256
//   CachePath              where builds are kept; <data directory>/Cache by default
//   Debug                  DEBUG lines in the updater's log, the log on stderr as well
//                          (his console), and every transfer traced; holding Shift
//                          when the game starts sets it too
//   DebugLoadSE            load the libbg3mse.dylib beside the loader, never a cached one
//   IPv4Only               connect over IPv4 only (Fetch.h)
//   DisableUpdates         fetch nothing; cached builds are still loaded
//
// Norbyte's ValidateSignature is read by his debug builds only; this
// updater has no switch that turns its checks off.
//
// As his ConfigGet does, a key that is missing or of another type leaves
// the setting as it was, without a word; a TargetVersion that is no version
// is no pin. A file that is not JSON ends the process with a box, as his
// ApplyConfigFile does through Fail() (CoreLib/ConfigUtils.inl).

namespace bg3se::mac::updater {

inline constexpr char kConfigFile[] = "ScriptExtenderUpdaterConfig.json";

struct Config
{
    std::string ManifestURL;
    std::string ManifestName{ "Manifest.json" };
    std::string UpdateChannel{ "Release" };
    std::optional<Version> TargetVersion;
    std::string TargetResourceDigest;
    std::string CachePath;
    bool Debug{ false };
    bool DebugLoadSE{ false };
    bool IPv4Only{ false };
    bool DisableUpdates{ false };

    std::string ManifestLocation() const { return ManifestURL + UpdateChannel + "/" + ManifestName; }
};

// Applies the settings in `json` over `config`. False, and nothing applied,
// when the text is not a JSON object.
bool ApplyConfig(std::string_view json, Config& config);

// The defaults (the built-in URL, <dataDirectory>/Cache), then the file
// <dataDirectory>/ScriptExtenderUpdaterConfig.json when it is there. When
// the file is there and is not JSON, `failure` is Fail()'s text for it.
Config LoadConfig(std::string const& dataDirectory, std::string const& defaultURL, std::string& failure);

}
