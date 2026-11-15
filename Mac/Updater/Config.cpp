#include <Updater/Config.h>
#include <Updater/Files.h>

#include <rapidjson/document.h>

namespace bg3se::mac::updater {

bool ApplyConfig(std::string_view json, Config& c)
{
    rapidjson::Document root;
    root.Parse(json.data(), json.size());
    if (root.HasParseError() || !root.IsObject()) return false;
    auto get = [&](char const* key, auto& out) {
        auto it = root.FindMember(key);
        if (it == root.MemberEnd()) return;
        using T = std::decay_t<decltype(out)>;
        if constexpr (std::is_same_v<T, bool>) {
            if (it->value.IsBool()) out = it->value.GetBool();
        } else {
            if (it->value.IsString()) out = std::string(it->value.GetString(), it->value.GetStringLength());
        }
    };
    get("ManifestURL", c.ManifestURL);
    get("ManifestName", c.ManifestName);
    get("UpdateChannel", c.UpdateChannel);
    std::string target;
    get("TargetVersion", target);
    if (!target.empty()) c.TargetVersion = Version::Parse(target);
    get("TargetResourceDigest", c.TargetResourceDigest);
    for (char& ch : c.TargetResourceDigest) ch = (char)tolower((unsigned char)ch);
    get("CachePath", c.CachePath);
    get("Debug", c.Debug);
    get("DebugLoadSE", c.DebugLoadSE);
    get("IPv4Only", c.IPv4Only);
    get("DisableUpdates", c.DisableUpdates);
    return true;
}

Config LoadConfig(std::string const& dataDirectory, std::string const& defaultURL, std::string& failure)
{
    Config c;
    c.ManifestURL = defaultURL;
    if (dataDirectory.empty()) return c;
    c.CachePath = dataDirectory + "/Cache";
    std::string path = dataDirectory + "/" + kConfigFile;
    std::string text;
    // As Norbyte's ReadConfigFile: a file that cannot be opened is no file.
    // He reads the whole file whatever its size; one this large is not taken.
    switch (ReadFile(path, text, 16u << 20)) {
    case ReadResult::Ok:
        if (!ApplyConfig(text, c)) failure = "Failed to load configuration file '" + path + "'";
        break;
    case ReadResult::TooLarge:
        failure = "Failed to load configuration file '" + path + "'";
        break;
    case ReadResult::Missing:
    case ReadResult::Failed:
        break;
    }
    return c;
}

}
