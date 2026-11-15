#include <Binding/GameVersion.h>

#include <CoreFoundation/CoreFoundation.h>

namespace bg3se::mac::binding {

std::optional<GameVersion> ParseGameVersion(std::string_view text)
{
    uint32_t parts[4]{};
    size_t count = 0;
    while (true) {
        if (count == std::size(parts) || text.empty() || text.front() < '0' || text.front() > '9') return std::nullopt;
        uint64_t value = 0;
        while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
            value = value * 10 + static_cast<uint64_t>(text.front() - '0');
            if (value > UINT32_MAX) return std::nullopt;
            text.remove_prefix(1);
        }
        parts[count++] = static_cast<uint32_t>(value);
        if (text.empty()) break;
        if (text.front() != '.') return std::nullopt;
        text.remove_prefix(1);
    }
    return GameVersion{ parts[0], parts[1], parts[2], parts[3] };
}

std::string GameVersionString()
{
    CFBundleRef bundle = CFBundleGetMainBundle();
    if (!bundle) return {};
    for (CFStringRef key : { CFSTR("CFBundleShortVersionString"), kCFBundleVersionKey }) {
        CFTypeRef found = CFBundleGetValueForInfoDictionaryKey(bundle, key);
        if (!found || CFGetTypeID(found) != CFStringGetTypeID()) continue;
        char text[128];
        if (CFStringGetCString(static_cast<CFStringRef>(found), text, sizeof(text), kCFStringEncodingUTF8) && text[0]) return text;
    }
    return {};
}

std::optional<GameVersion> ReadGameVersion()
{
    return ParseGameVersion(GameVersionString());
}

}
