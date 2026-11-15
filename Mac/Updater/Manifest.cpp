#include <Updater/Manifest.h>

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <charconv>

namespace bg3se::mac::updater {

using rapidjson::Value;

std::optional<Version> Version::Parse(std::string_view text)
{
    Version v;
    if (text.empty()) return {};
    size_t at = 0;
    while (true) {
        uint32_t part = 0;
        auto [end, ec] = std::from_chars(text.data() + at, text.data() + text.size(), part);
        if (ec != std::errc() || end == text.data() + at) return {};
        v.Parts.push_back(part);
        at = (size_t)(end - text.data());
        if (at == text.size()) break;
        if (text[at] != '.' || v.Parts.size() >= 8) return {};
        at++;
    }
    return v;
}

std::string Version::ToString() const
{
    std::string s;
    for (size_t i = 0; i < Parts.size(); i++) {
        if (i) s += '.';
        s += std::to_string(Parts[i]);
    }
    return s;
}

int Version::Compare(Version const& o) const
{
    size_t n = std::max(Parts.size(), o.Parts.size());
    for (size_t i = 0; i < n; i++) {
        uint32_t a = i < Parts.size() ? Parts[i] : 0, b = i < o.Parts.size() ? o.Parts[i] : 0;
        if (a != b) return a < b ? -1 : 1;
    }
    return 0;
}

std::string Release::CacheName() const
{
    return ReleaseVersion.ToString() + "_" + Digest;
}

namespace {

bool IsDigest(std::string const& s)
{
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// Members are read strictly: a member that is there with the wrong type makes
// the manifest invalid, so that a mistake is not read as "not set".
struct Reader
{
    Value const& Node;
    std::string& Why;
    bool Ok{ true };

    Value const* Find(char const* key)
    {
        auto it = Node.FindMember(key);
        return it == Node.MemberEnd() ? nullptr : &it->value;
    }
    void Fail(char const* key, char const* what)
    {
        if (Ok) Why = std::string("'") + key + "' " + what;
        Ok = false;
    }
    std::string String(char const* key, bool required)
    {
        Value const* v = Find(key);
        if (!v) {
            if (required) Fail(key, "is missing");
            return {};
        }
        if (!v->IsString()) {
            Fail(key, "is not a string");
            return {};
        }
        return std::string(v->GetString(), v->GetStringLength());
    }
    uint64_t Unsigned(char const* key, bool required)
    {
        Value const* v = Find(key);
        if (!v) {
            if (required) Fail(key, "is missing");
            return 0;
        }
        if (!v->IsUint64()) {
            Fail(key, "is not a whole number of 0 or more");
            return 0;
        }
        return v->GetUint64();
    }
    bool Bool(char const* key)
    {
        Value const* v = Find(key);
        if (!v) return false;
        if (!v->IsBool()) {
            Fail(key, "is not true or false");
            return false;
        }
        return v->GetBool();
    }
    std::optional<Version> VersionOf(char const* key, bool required)
    {
        std::string text = String(key, required);
        if (text.empty()) {
            if (required && Ok) Fail(key, "is empty");
            return {};
        }
        auto v = Version::Parse(text);
        if (!v) Fail(key, "is not a version");
        return v;
    }
};

bool ParseRelease(Value const& node, Release& r, std::string& why)
{
    if (!node.IsObject()) {
        why = "a release is not an object";
        return false;
    }
    Reader in{ node, why };
    auto version = in.VersionOf("Version", true);
    r.Commit = in.String("Commit", false);
    r.ScriptExtenderVersion = in.String("ScriptExtenderVersion", false);
    r.URL = in.String("URL", true);
    r.Size = in.Unsigned("Size", true);
    r.Digest = in.String("Digest", true);
    r.ManifestDigest = in.String("ManifestDigest", true);
    r.MinGameVersion = in.VersionOf("MinGameVersion", false);
    r.MaxGameVersion = in.VersionOf("MaxGameVersion", false);
    r.GameBuildChecked = in.String("GameBuildChecked", false);
    auto macos = in.VersionOf("MacOSMinimum", true);
    r.Architecture = in.String("Architecture", true);
    r.BuildDate = in.Unsigned("BuildDate", false);
    r.Revoked = in.Bool("Revoked");
    r.Notice = in.String("Notice", false);
    if (!in.Ok) return false;
    r.ReleaseVersion = *version;
    r.MacOSMinimum = *macos;
    if (!IsDigest(r.Digest) || !IsDigest(r.ManifestDigest)) {
        why = "release " + r.ReleaseVersion.ToString() + ": a digest is not 64 lower-case hexadecimal digits";
        return false;
    }
    if (r.URL.empty() || r.Size == 0) {
        why = "release " + r.ReleaseVersion.ToString() + " has no URL or no size";
        return false;
    }
    return true;
}

}

ParseResult ParseChannelManifest(std::string_view json, ChannelManifest& m, std::string& why)
{
    rapidjson::Document root;
    root.Parse<rapidjson::kParseFullPrecisionFlag>(json.data(), json.size());
    if (root.HasParseError() || !root.IsObject()) {
        why = "it is not a JSON object";
        return ParseResult::Invalid;
    }
    Reader in{ root, why };
    uint64_t version = in.Unsigned("ManifestVersion", true);
    if (!in.Ok) return ParseResult::Invalid;
    if (version > (uint64_t)kManifestVersion) {
        why = "its format (ManifestVersion " + std::to_string(version) + ") is newer than this loader reads";
        return ParseResult::TooNew;
    }
    if (version != (uint64_t)kManifestVersion) {
        why = "its format (ManifestVersion " + std::to_string(version) + ") is not one this loader reads";
        return ParseResult::Invalid;
    }
    m = ChannelManifest{};
    m.ManifestVersion = (int32_t)version;
    m.Product = in.String("Product", true);
    m.Channel = in.String("Channel", true);
    m.Sequence = in.Unsigned("Sequence", true);
    m.Notice = in.String("Notice", false);
    m.NoMatchingVersionNotice = in.String("NoMatchingVersionNotice", false);
    if (!in.Ok) return ParseResult::Invalid;
    if (m.Product != kProduct) {
        why = "it is for '" + m.Product + "', not " + kProduct;
        return ParseResult::Invalid;
    }
    Value const* releases = in.Find("Releases");
    if (!releases || !releases->IsArray()) {
        why = "'Releases' is missing or not a list";
        return ParseResult::Invalid;
    }
    for (auto const& node : releases->GetArray()) {
        Release r;
        if (!ParseRelease(node, r, why)) return ParseResult::Invalid;
        for (auto const& other : m.Releases) {
            if (other.Digest == r.Digest || other.ReleaseVersion == r.ReleaseVersion) {
                why = "release " + r.ReleaseVersion.ToString() + " is listed twice";
                return ParseResult::Invalid;
            }
        }
        m.Releases.push_back(std::move(r));
    }
    return ParseResult::Ok;
}

std::string StringifyChannelManifest(ChannelManifest const& m)
{
    rapidjson::Document doc(rapidjson::kObjectType);
    auto& a = doc.GetAllocator();
    auto str = [&](std::string const& s) { return Value(s.c_str(), (rapidjson::SizeType)s.size(), a); };
    doc.AddMember("ManifestVersion", m.ManifestVersion, a);
    doc.AddMember("Product", str(m.Product), a);
    doc.AddMember("Channel", str(m.Channel), a);
    doc.AddMember("Sequence", (uint64_t)m.Sequence, a);
    if (!m.Notice.empty()) doc.AddMember("Notice", str(m.Notice), a);
    if (!m.NoMatchingVersionNotice.empty()) doc.AddMember("NoMatchingVersionNotice", str(m.NoMatchingVersionNotice), a);
    Value releases(rapidjson::kArrayType);
    for (auto const& r : m.Releases) {
        Value v(rapidjson::kObjectType);
        v.AddMember("Version", str(r.ReleaseVersion.ToString()), a);
        if (!r.Commit.empty()) v.AddMember("Commit", str(r.Commit), a);
        if (!r.ScriptExtenderVersion.empty()) v.AddMember("ScriptExtenderVersion", str(r.ScriptExtenderVersion), a);
        v.AddMember("URL", str(r.URL), a);
        v.AddMember("Size", (uint64_t)r.Size, a);
        v.AddMember("Digest", str(r.Digest), a);
        v.AddMember("ManifestDigest", str(r.ManifestDigest), a);
        if (r.MinGameVersion) v.AddMember("MinGameVersion", str(r.MinGameVersion->ToString()), a);
        if (r.MaxGameVersion) v.AddMember("MaxGameVersion", str(r.MaxGameVersion->ToString()), a);
        if (!r.GameBuildChecked.empty()) v.AddMember("GameBuildChecked", str(r.GameBuildChecked), a);
        v.AddMember("MacOSMinimum", str(r.MacOSMinimum.ToString()), a);
        v.AddMember("Architecture", str(r.Architecture), a);
        v.AddMember("BuildDate", (uint64_t)r.BuildDate, a);
        if (r.Revoked) v.AddMember("Revoked", true, a);
        if (!r.Notice.empty()) v.AddMember("Notice", str(r.Notice), a);
        releases.PushBack(v, a);
    }
    doc.AddMember("Releases", releases, a);
    rapidjson::StringBuffer sb;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
    doc.Accept(writer);
    return std::string(sb.GetString(), sb.GetSize()) + "\n";
}

std::string Unsuitable(Release const& r, Platform const& p)
{
    if (r.Revoked) return "it is revoked";
    if (r.Architecture != p.Architecture) return "it is for " + r.Architecture + ", not " + p.Architecture;
    if (r.MacOSMinimum > p.MacOS) return "it needs macOS " + r.MacOSMinimum.ToString() + " or later";
    if (r.MinGameVersion && *r.MinGameVersion > p.Game) return "it needs game version " + r.MinGameVersion->ToString() + " or later";
    if (r.MaxGameVersion && *r.MaxGameVersion < p.Game) return "it is for game versions up to " + r.MaxGameVersion->ToString();
    return {};
}

Release const* Choose(ChannelManifest const& m, Platform const& p, std::optional<Version> const& version, std::string const& digest)
{
    Release const* best = nullptr;
    for (auto const& r : m.Releases) {
        if (!digest.empty()) {
            // Norbyte's TargetResourceDigest picks that build whatever the game is.
            if (r.Digest == digest && !r.Revoked) return &r;
            continue;
        }
        if (!Unsuitable(r, p).empty()) continue;
        if (version && r.ReleaseVersion != *version) continue;
        if (!best || r.ReleaseVersion > best->ReleaseVersion
            || (r.ReleaseVersion == best->ReleaseVersion && r.BuildDate > best->BuildDate)) {
            best = &r;
        }
    }
    return best;
}

bool ParseReleaseManifest(std::string_view json, ReleaseManifest& m, std::string& why)
{
    rapidjson::Document root;
    root.Parse(json.data(), json.size());
    if (root.HasParseError() || !root.IsObject()) {
        why = "manifest.json is not a JSON object";
        return false;
    }
    Reader in{ root, why };
    m = ReleaseManifest{};
    m.Name = in.String("name", true);
    auto version = in.VersionOf("version", true);
    m.Commit = in.String("commit", true);
    m.Channel = in.String("channel", true);
    m.Architecture = in.String("architecture", true);
    m.ScriptExtenderVersion = in.String("script_extender_version", false);
    m.GameBuildChecked = in.String("game_build_checked", false);
    m.MacOSMinimum = in.String("macos_minimum", false);
    if (!in.Ok) {
        why = "manifest.json: " + why;
        return false;
    }
    m.ReleaseVersion = *version;
    Value const* files = in.Find("files");
    if (!files || !files->IsArray()) {
        why = "manifest.json has no list of files";
        return false;
    }
    for (auto const& node : files->GetArray()) {
        if (!node.IsObject()) {
            why = "manifest.json: a file is not an object";
            return false;
        }
        Reader f{ node, why };
        ReleaseFile file;
        file.Path = f.String("path", true);
        file.Size = f.Unsigned("size", true);
        file.Sha256 = f.String("sha256", true);
        if (!f.Ok || !IsDigest(file.Sha256)) {
            why = "manifest.json: a file entry is not valid" + (f.Ok ? std::string() : ": " + why);
            return false;
        }
        m.Files.push_back(std::move(file));
    }
    return true;
}

}
