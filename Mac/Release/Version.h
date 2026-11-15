#pragma once

// What a build of bg3mse is (Mac/Release/Configure.cmake writes the values).
// The same is in each dylib for tools to read: its LC_ID_DYLIB current
// version is kVersion, and kStamp is in the section __TEXT,__bg3mse_version,
//
//   @(#)bg3mse <version> commit=<commit> extender=<extender version> channel=<channel>
//
// which "what -s libbg3mse.dylib" prints without its "@(#)".

#include <string>

namespace bg3se::mac::version {

// bg3mse's own version, "a.b.c".
extern char const kVersion[];
// The git commit the build was configured at, 40 hexadecimal digits, or
// "unknown" when the tree is not a git checkout.
extern char const kCommit[];
// Norbyte's version of the Script Extender, RES_DLL_VERSION_STRING.
extern char const kExtenderVersion[];
// The build's flavour, BG3MSE_CHANNEL: Release, Devel or Nightly.
extern char const kChannel[];
// The line above, with its "@(#)".
extern char const kStamp[];

// The line Norbyte's ShowVersionNumber adds under the game's version number
// in the main menu (Extender/Client/ScriptExtenderClient.cpp), naming
// bg3mse. It keeps the words "Script Extender", which his code looks for so
// as not to add the line twice.
inline std::string MenuVersionLine(unsigned extenderVersion, char const* buildDate)
{
    return std::string("\r\nbg3mse ") + kVersion + " loaded (Script Extender v" + std::to_string(extenderVersion)
        + ", built on " + buildDate + ").";
}

}
