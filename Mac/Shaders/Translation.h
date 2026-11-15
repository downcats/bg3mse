#pragma once

#include <optional>

#include <Shaders/Provision.h>
#include <Shaders/ShaderMapping.h>
#include <Shaders/SpirvIdentity.h>

// A mod material whose shaders are not vanilla ones, made into what the Mac
// game loads: for every pass a stub naming two Metal functions and the
// source of those functions, and the material's .lsf made ready for the Mac.
//
// The functions are the mod's own Vulkan shaders translated (SpirvShader),
// with everything they take placed where the Mac game has it (PlaceShader).
// The material's two constant blocks keep the layout the mod's shader has,
// so the offsets its .lsf lists stay true: the .lsf only learns where the Mac
// game binds the two blocks, and that it is ready for Metal. Any pass that
// cannot be placed with certainty refuses the whole material.

namespace bg3se::mac {

// A Vulkan container's own table of what its shader takes begins with the
// version of the toolkit's format. Every container of the game the interface
// table was learnt from has this one; a material built for an earlier game
// has an earlier one, and where that game's engine kept what it supplies
// (its lighting block, the cloud maps) is not something the table knows.
constexpr uint32_t kVulkanTableVersion = 3;
// 0 when the container has no table.
uint32_t VulkanTableVersion(ContainerParts const& parts);

struct TranslatedPass
{
    std::string Pass;                   // ST_FOR
    std::string VertexName, FragmentName;       // the functions the stub names; no fragment for a depth pass without one
    std::string VertexSource, FragmentSource;
    std::vector<uint8_t> Stub;          // the _Metal.bshd
};

struct TranslatedMaterial
{
    std::vector<TranslatedPass> Passes;
    std::vector<uint8_t> Lsf;
    // How many of its passes have a vertex stage that is not one of the
    // game's own: said when the material is served, so that a report of a
    // wrong picture can be traced to a stage nothing of the game's was
    // drawn beside.
    uint32_t OwnVertexStages{ 0 };
    // The Mac names its shaders take by a pairing the interface table made
    // by order alone, nothing drawn having borne it out: said with it too.
    std::vector<std::string> ByOrderOnly;
};

// A _Metal.bshd as the game ships them: no shader, the names of two functions.
std::vector<uint8_t> BuildMetalStub(std::string_view name, std::string_view vertexFunction, std::string_view fragmentFunction);

// The name of the Metal function of a stage of a pass: of what the Vulkan
// container holds, so the same shader has the same name wherever it ships.
std::string TranslatedFunctionName(std::span<uint8_t const> vulkanContainer, bool vertex);
constexpr std::string_view kTranslatedPrefix = "bg3mse_";

// Which matrices a shader builds are the other way round in the Mac game's
// own functions: a 2x2 matrix built from scalars (a rotation of texture
// coordinates, in every case found) is the transpose there of what the
// Windows shader builds. Measured on every vanilla pass that both platforms
// ship and that draws differently without it: 68 such matrices have to be
// transposed for the pictures to agree, none has to be left; of the 3x3 and
// 4x4 matrices shaders build, none has to be transposed (bg3mse-shaders
// verify with BG3MSE_SHADERS_MATRIX_SEARCH).
MatrixRule const& MacMatrices();

// One stage of a pass (a container's stage blob) as the source of a Metal
// function named `entry`. A stage of two modules is translated twice and
// taken when both come to the same source. False, with `why`, otherwise.
bool TranslateStage(std::span<uint8_t const> stage, std::string const& entry, std::span<InputSemantic const> inputs,
    std::span<MaterialSlot const> material, InterfaceTable const& table, MacInterface const& mac,
    std::vector<PlacedSlot>& placed, std::string& source, std::string& why, MatrixRule const& matrices = MacMatrices(),
    std::vector<std::string>* byOrderOnly = nullptr, PassRegisters const* registers = nullptr);

// `material` is the mod's .lsf path.
// Every pass is translated unless a translation cannot be made of it (a
// container that cannot be read, a thing with nothing known at its place) or
// it touches one of the table's open faults; whether its stages are the
// game's own decides nothing, and neither does a pairing made by order alone.
// With `everyReason`, a refused material is gone through to the end and every
// reason that refuses a pass or a stage of it is added there, not only the
// first, which `why` has.
std::optional<TranslatedMaterial> TranslateMaterial(MaterialSource const& mod, std::string const& material, InterfaceTable const& table,
    MacInterface const& mac, std::string& why, std::vector<std::string>* everyReason = nullptr);

}
