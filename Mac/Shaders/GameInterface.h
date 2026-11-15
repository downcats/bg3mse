#pragma once

#include <Platform/Symbols.h>
#include <Shaders/ShaderMapping.h>

// What translating a shader needs of the game it runs in: that the game
// makes shaders the way the stubs rely on, and where its own Metal functions
// take everything (MacInterface), read from them once and kept between runs.

namespace bg3se::mac {

// rf::GPUDevice::CreateShader asks the game's Metal libraries for the
// functions a stub names (-newFunctionWithName:, for the vertex and for the
// fragment function, each of two libraries). A translated shader is served
// through exactly that; without it, nothing is.
struct ShaderCreationCheck
{
    uint32_t Requests{ 0 };     // the places the function asks a library for a function by name
    std::string Problem;        // empty when translated functions will be found

    bool Ok() const { return Problem.empty(); }
};
ShaderCreationCheck CheckShaderCreation(GameImage const& image);

// The sun's shadow. The Windows shaders read members of the lighting
// constants that the Mac block does not have; they are worked out from
// members it has, by relations that hold because of how the game's
// ls::SetupForwardLightingConstants fills the block: one over the fade
// amount, one over the frustum widths, a bias per cascade that is the
// Windows bias over the cascade's far plane times a mix that is 1 for the
// first, the count of cascades as a whole number. Those relations are this
// function's arithmetic, so the function is looked up by its name in the
// game that is running and the instructions that do it are compared: each
// of the stretches has to be in the function exactly once. Anything else
// (no such function, a stretch missing or twice) and the members stay
// without a counterpart, with the reason in Problem.
struct SunShadowCheck
{
    std::string Problem;

    bool Ok() const { return Problem.empty(); }
};
SunShadowCheck CheckSunShadowBridge(GameImage const& image);
// The function's name and the stretches looked for, in the order they are looked
// for: what a made-up game needs to be the game the bridge was read from.
char const* SunShadowFunctionName();
std::vector<std::span<uint32_t const>> SunShadowStretches();
// And where that function writes them is where the game's own Metal
// functions read them: the block's members at the offsets the function uses.
bool SunShadowMembersWhereTheEngineWritesThem(MacInterface const& mac, std::string& why);

// The table of textures (ShaderInterface.h: TextureTableRewrite). A translated
// shader reads a light probe's and a light's whole numbers as an array's
// number and a layer, which is right because of how the game's
// ls::ClusteredAssignmentStage packs them when it gives a probe's cube or a
// light's cookie its place: the layer plus one, nine bits at 3 and at 12 of
// a probe's first number (the diffuse cubes); the array's number and, two
// bits above it, the layer plus one, at 0 and at 11 of its second (the
// specular cubes); and in a light's, the array's number times two with the
// layer plus one from bit 4 (the cookie). The two functions that do it are
// looked up by their names in the game that is running and those
// instructions compared: each stretch has to be in its function exactly
// once. Anything else, and the table of textures is not bridged, with the
// reason in Problem.
struct TextureTableCheck
{
    std::string Problem;

    bool Ok() const { return Problem.empty(); }
};
TextureTableCheck CheckTextureTableBridge(GameImage const& image);
// What a made-up game needs to be that game: each function's name and the
// stretches looked for in it.
struct TextureTableCode
{
    char const* Function;
    std::vector<std::span<uint32_t const>> Stretches;
};
std::vector<TextureTableCode> TextureTableFunctions();
// And the game's own Metal functions take the probes, the lights and the
// textures as that was read: the two buffers' elements with the members
// where the table has them, the cubes as arrays (one for the diffuse ones,
// as many as the table says for the specular ones) and the cookies as as
// many flat arrays as it says.
bool TextureTableAsRead(MacInterface const& mac, InterfaceTable const& table, std::string& why);

// What of the interface table rests on the game's engine and is not shown
// for a game: empty where it stands.
struct BridgeProblems
{
    std::string SunShadow;          // the members of the lighting constants that are worked out
    std::string TextureTable;       // the table of textures
};
// The interface table for one game: the table this build was made with,
// without the members that are worked out from others (EngineBlock::Derived)
// and without the table of textures unless what each rests on is shown for
// that game, by the checks above. `image` is the game's binary and `mac`
// what its own Metal functions agree on.
InterfaceTable InterfaceFor(GameImage const& image, MacInterface const& mac, BridgeProblems& problems);
// The same for a game that is not the one running: its binary is read from the bundle.
InterfaceTable InterfaceForBundle(std::string const& appBundle, MacInterface const& mac, BridgeProblems& problems);

// Reads every function pair the game's Materials.pak has a stub for. Minutes
// the first time on a machine (the system compiles each for reflection),
// seconds afterwards: not for a thread of the game's.
bool SurveyMacInterface(std::string const& appBundle, MacInterface& mac, std::string& why);

// What a kept interface was read from: the game's two Metal libraries and its
// Materials.pak, by size and modification time. Empty when one is missing.
std::string MacInterfaceStamp(std::string const& appBundle);

// The file the interface is kept in, in bg3mse's data directory.
inline constexpr char kMacInterfaceFile[] = "shader-interface.txt";

// The interface kept in `file`, when it was read from what `stamp` describes
// and written by a build that writes the file as this one does. Any other
// file (another game's or game version's, an earlier build's, one that is cut
// short or does not read back) is not an interface: nothing is returned, and
// whoever asked reads the game again and writes the file anew.
std::optional<MacInterface> LoadMacInterface(std::string const& file, std::string const& stamp);
// Written beside the file under a name of this process's own
// (<file>.<pid>.partial) and moved into place: two writers leave one whole
// file, and a run that ends in the middle none. What such a run left beside
// the file is removed once it is an hour old, here and by EnsureMacInterface.
bool SaveMacInterface(std::string const& file, std::string const& stamp, MacInterface const& mac);

// What a tool outside the game does of the read, so that the game does not
// have to (bg3mse-install shader-interface, Mac/Installer/InstallTool.cpp).
// Not for a thread of the game's.
//
// Kept: `file` holds the interface of the game at `appBundle` as it is now,
// as this build writes it; nothing is read. Read: it did not, and the game's
// functions were read now (SurveyMacInterface) and kept there.
// OfNewerBuild: the file was written by a later build of bg3mse than this
// one, which writes it another way. It is that build's to keep and is left
// as it is: the tool beside an installed bg3mse may be older than the build
// the updater has the game load, and each writing the file its own way over
// the other's would have both read the game at every start. NotMade, with
// `why`: the game cannot be read, or the file not written.
struct EnsuredMacInterface
{
    enum class How { Kept, Read, OfNewerBuild, NotMade };
    How What{ How::NotMade };
    std::optional<MacInterface> Interface;      // Kept, Read

    bool Ok() const { return What != How::NotMade; }
};
EnsuredMacInterface EnsureMacInterface(std::string const& appBundle, std::string const& file, std::string& why);
// Whether EnsureMacInterface would read the game.
bool MacInterfaceToBeRead(std::string const& appBundle, std::string const& file);

}
