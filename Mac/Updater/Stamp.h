#pragma once

#include <optional>
#include <string>
#include <string_view>

// The version stamp of an image of bg3mse (Mac/Release/Version.h), read from
// the file without loading it: the section __TEXT,__bg3mse_version of its
// arm64 code, "@(#)bg3mse <version> commit=<commit> extender=<version> channel=<channel>".

namespace bg3se::mac::updater {

struct Stamp
{
    std::string Version;
    std::string Commit;
    std::string Extender;
    std::string Channel;
};

bool ReadStamp(std::string const& path, Stamp& stamp, std::string& why);
bool ParseStamp(std::string_view text, Stamp& stamp);

// Whether the image carries the graphics module's mark (Release/GfxMark.h)
// in its arm64 code: `marked`, and true. False, with why, when that cannot
// be told: the file is not arm64 Mach-O code that can be read whole, or the
// section holds something other than the mark.
bool ReadGfxMark(std::string const& path, bool& marked, std::string& why);

// The bytes of a section of the file's arm64 code (at most 4 KB), or none
// when it has no such section; false, with why, when the file cannot be read
// as arm64 Mach-O code.
bool ReadSection(std::string const& path, char const* segment, char const* section, std::optional<std::string>& bytes, std::string& why);

}
