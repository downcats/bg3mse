#pragma once

#include <cstdlib>
#include <string>
#include <string_view>

// What a player is told to do when bg3mse does not support their game. A
// release records the game version it was checked against in its
// manifest.json (game_build_checked, Mac/Release/Assemble.cmake), which is
// kept beside libbg3mse.dylib in bin/ and in the updater's cache: a game
// older than that is updated in Steam; for a newer one, a release for it is
// fetched when the game starts, once there is one, or, from a release that
// does not update itself (self_update false), downloaded. For bg3mse-install
// and the extender's start-up notice alike.

namespace bg3se::mac {

// The game version a release's manifest.json says it was checked against; empty when it says none.
inline std::string CheckedGameVersion(std::string_view manifest)
{
    size_t at = manifest.find("\"game_build_checked\"");
    if (at == std::string_view::npos) return {};
    at = manifest.find(':', at);
    size_t open = at == std::string_view::npos ? at : manifest.find('"', at);
    size_t close = open == std::string_view::npos ? open : manifest.find('"', open + 1);
    if (close == std::string_view::npos) return {};
    return std::string(manifest.substr(open + 1, close - open - 1));
}

// Whether a release's manifest.json says it updates itself: all but one that says "self_update": false.
inline bool SelfUpdating(std::string_view manifest)
{
    size_t at = manifest.find("\"self_update\"");
    if (at == std::string_view::npos) return true;
    at = manifest.find(':', at);
    if (at == std::string_view::npos) return true;
    at = manifest.find_first_not_of(" \t\r\n", at + 1);
    return at == std::string_view::npos || manifest.substr(at, 5) != "false";
}

// Whether the updater's settings (ScriptExtenderUpdaterConfig.json, Norbyte's) say "DisableUpdates": true.
inline bool UpdatesDisabled(std::string_view settings)
{
    size_t at = settings.find("\"DisableUpdates\"");
    if (at == std::string_view::npos) return false;
    at = settings.find(':', at);
    if (at == std::string_view::npos) return false;
    at = settings.find_first_not_of(" \t\r\n", at + 1);
    return at != std::string_view::npos && settings.substr(at, 4) == "true";
}

// -1, 0 or 1 as the dotted version a is before, the same as, or after b.
inline int CompareGameVersions(std::string_view a, std::string_view b)
{
    while (!a.empty() || !b.empty()) {
        auto part = [](std::string_view& v) {
            size_t dot = v.find('.');
            unsigned long n = strtoul(std::string(v.substr(0, dot)).c_str(), nullptr, 10);
            v = dot == std::string_view::npos ? std::string_view() : v.substr(dot + 1);
            return n;
        };
        unsigned long x = part(a), y = part(b);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// What to do, for the game version `game` and a release checked against `checked`, which updates itself or not
// (a release that does, with DisableUpdates set, does not).
inline std::string UnsupportedGameAdvice(std::string const& game, std::string const& checked, bool selfUpdating = true)
{
    if (!game.empty() && !checked.empty() && CompareGameVersions(game, checked) < 0) {
        return "Update the game in Steam.";
    }
    if (!game.empty() && !checked.empty() && CompareGameVersions(game, checked) > 0) {
        return selfUpdating ? "It'll update itself once a new version is out." : "Download the new bg3mse once it's out.";
    }
    return "Check for a newer bg3mse.";
}

}
