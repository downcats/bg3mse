#include <Platform/Config.h>

#include <cstdio>
#include <cstdlib>
#include <pwd.h>
#include <strings.h>
#include <unistd.h>

namespace bg3se::mac {

namespace {

constexpr size_t kMaxFile = 64 * 1024;
constexpr size_t kMaxLine = 512;

std::string_view TrimStart(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    return s;
}

std::string_view TrimEnd(std::string_view s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool EqualNoCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && strncasecmp(a.data(), b.data(), a.size()) == 0;
}

}

std::string DataDirectory()
{
    // HOME, as every program the user starts has it. A process that was
    // started without it (or with something that is no path from the root)
    // still has a user, whose home directory the system knows; "" for a
    // home would make this the machine's /Library.
    std::string home;
    if (char const* env = getenv("HOME"); env && env[0] == '/') {
        home = env;
    } else {
        passwd entry{};
        passwd* found = nullptr;
        char buffer[4096];
        if (getpwuid_r(getuid(), &entry, buffer, sizeof(buffer), &found) == 0 && found && found->pw_dir && found->pw_dir[0] == '/') {
            home = found->pw_dir;
        }
    }
    if (home.empty()) return {};
    return home + "/Library/Application Support/bg3mse";
}

std::optional<std::string> ConfLookup(std::string_view text, std::string_view key)
{
    if (key.empty()) return {};
    // The text ends at its first NUL.
    text = text.substr(0, text.find('\0'));
    std::optional<std::string> found;
    while (!text.empty()) {
        size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
        if (line.size() >= kMaxLine) continue;

        line = TrimStart(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        if (!EqualNoCase(TrimEnd(line.substr(0, eq)), key)) continue;
        found = std::string(TrimEnd(TrimStart(line.substr(eq + 1))));    // keep scanning: the last one wins
    }
    return found;
}

std::optional<std::string> ConfValue(std::string_view key)
{
    std::string directory = DataDirectory();
    if (directory.empty()) return {};
    std::string path = directory + "/bg3mse.conf";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return {};
    std::string text(kMaxFile, '\0');
    text.resize(fread(text.data(), 1, kMaxFile, f));
    fclose(f);
    return ConfLookup(text, key);
}

bool FlagValue(std::string_view text)
{
    for (std::string_view on : { "1", "true", "yes", "on" }) {
        if (EqualNoCase(text, on)) return true;
    }
    return false;
}

bool FeatureEnabled(std::string_view key)
{
    auto value = ConfValue(key);
    return value && FlagValue(*value);
}

bool LogDebugWanted()
{
    return getenv("BG3MSE_LOG_DEBUG") != nullptr || FeatureEnabled(kLogDebugSetting);
}

}
