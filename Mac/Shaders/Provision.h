#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Mac shaders for mod materials.
//
// A mod built with the Windows toolkit ships a material as an .lsf packed for
// Windows plus one DirectX and one Vulkan shader container per pass. The Mac
// game wants a _Metal.bshd stub per pass and an .lsf packed for Mac. A stub
// holds no shader: it names two functions of the Metal libraries the game
// ships (rf::GPUDevice::CreateShader asks those libraries for them by name),
// so the Mac game has Metal functions for vanilla shaders only. When every
// Vulkan container of a mod material is a vanilla Windows container of one
// vanilla material, as shipped or compiled again (SpirvIdentity.h), the mod's
// shader is that vanilla shader: the material gets the vanilla Mac stubs, and
// its .lsf is rewritten with the Mac material's structure and the mod's
// values. Anything else is refused by name and left as the game would have it.

namespace bg3se::mac {

// The files of one package that matter here, and how to read them.
struct MaterialSource
{
    std::string Label;                                   // for the log: the package's file name
    std::unordered_set<std::string> Names;               // every path under Assets/Materials/
    std::function<std::optional<std::vector<uint8_t>>(std::string const&)> Read;
};

// Which vanilla pass stems (hashes of "<material>_<PASS>") a container is the shader of.
using ContainerIdentity = std::function<std::vector<uint64_t>(std::span<uint8_t const> container)>;
ContainerIdentity VanillaContainerIdentity();
uint64_t PassStemHash(std::string_view stem);

// What a translator makes of a material whose shaders are not vanilla ones:
// its Mac-ready .lsf and a stub per pass, or nothing and why. The functions
// the stubs name are the translator's to keep where the game finds them.
struct TranslatedFiles
{
    std::vector<uint8_t> Lsf;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> Stubs;        // by pass, "ST_FOR"
    std::string Note;           // for the log: how it was done
};
using ProvisionTranslator = std::function<std::optional<TranslatedFiles>(MaterialSource const& mod, std::string const& material, std::string& why)>;

enum class ProvisionKind
{
    Twin,           // served
    Translated,     // served: its own shaders, translated to Metal
    Skipped,        // a mod ships Metal stubs for it
    Refused,
    Superseded,     // a later package ships the same material path
};

struct ProvisionDecision
{
    ProvisionKind Kind{ ProvisionKind::Refused };
    std::string Material;       // the mod's .lsf path
    std::string Package;
    std::string Vanilla;        // Twin: the vanilla material's .lsf path
    std::string Reason;
    uint32_t Passes{ 0 };       // Vulkan containers the mod ships for it
    uint32_t Matched{ 0 };      // of those, vanilla containers
    uint32_t Stubs{ 0 };        // Twin, Translated: Mac stubs served
};

struct ServedFile
{
    std::string Path;           // package-relative path
    std::vector<uint8_t> Data;
    uint32_t Decision{ 0 };     // index into Decisions
    bool IsMaterial{ false };   // the .lsf, as opposed to a stub
};

class ProvisionMap
{
public:
    std::vector<ProvisionDecision> Decisions;
    std::vector<ServedFile> Files;

    uint32_t Count(ProvisionKind kind) const;

    // The file to serve for a path as the game asks for it: the game prefixes
    // a root, so a served path matches as a suffix at a '/' boundary.
    ServedFile const* Find(std::string_view enginePath) const;

    // The refused material whose .lsf a path as the game asks for it is, or nullptr.
    ProvisionDecision const* FindRefused(std::string_view enginePath) const;

private:
    friend std::unique_ptr<ProvisionMap> BuildProvisionMap(MaterialSource const&, std::span<MaterialSource const>, ContainerIdentity const&,
        ProvisionTranslator const&, bool);
    std::unordered_map<std::string_view, std::vector<uint32_t>> byBaseName_;
    std::unordered_map<std::string_view, std::vector<uint32_t>> refusedByBaseName_;
};

// `mods` in load order: for a material path two packages ship, the later one wins.
// With a translator, a material whose shaders are not vanilla ones is handed
// to it instead of being refused outright. With `translateFirst`, so is one
// whose shaders are vanilla ones, and it is a twin only when that fails: for
// seeing translation at work on a material that needs none.
std::unique_ptr<ProvisionMap> BuildProvisionMap(MaterialSource const& vanilla, std::span<MaterialSource const> mods,
    ContainerIdentity const& identity, ProvisionTranslator const& translator = {}, bool translateFirst = false);

}
