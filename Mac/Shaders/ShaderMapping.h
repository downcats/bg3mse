#pragma once

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string_view>

#include <Shaders/ShaderInterface.h>

extern "C" {
#include <Shaders/lsf.h>
}

// Where each thing a Windows shader takes goes in a Mac shader.
//
// The names come from three places. What the material itself declares (its
// two constant blocks, its textures and samplers) is named by the material's
// own .lsf, per pass, with the Vulkan binding beside the name. A vertex input
// is named by the input signature of the pass's DirectX 11 container, whose
// elements are in location order. What the engine supplies has no name in
// anything a mod ships: that is the interface table, written by
// `bg3mse-shaders interface` from a Windows Materials.pak and the Mac game
// (InterfaceData.cpp), and checked against the shader before it is used.
//
// The Mac side is never taken from a table: the index of every engine
// resource and attribute, and the layout of every engine constant block, is
// read from the functions of the installed game (MacInterface).

namespace bg3se::mac {

struct InputSemantic
{
    std::string Name;       // "TEXCOORD"
    uint32_t Index{ 0 };
};

// The vertex inputs of a DirectX 11 vertex stage (a container's stage 0), in
// location order: the elements of its input signature that are not system
// values. False when the stage is not one this code reads.
bool ReadInputSignature(std::span<uint8_t const> stage, std::vector<InputSemantic>& inputs, std::string& why);

// A resource a material declares for one pass.
struct MaterialSlot
{
    uint32_t Set{ 0 };
    uint32_t Binding{ 0 };
    std::string Name;       // the UniformName, or CB_EngineParams / CB_MaterialConstants
    int32_t DxVs{ -1 };
    int32_t DxPs{ -1 };
    char Kind{ 0 };         // 'b' a constant block, 't' a texture, 's' a sampler; 0 for a parameter of another sort
};

// The .lsf's name for a pass ("ST_FOR" -> "StaticForward"), or empty.
std::string_view PassNodeName(std::string_view pass);

// Every resource a Windows-packed material declares for `pass`.
std::vector<MaterialSlot> ReadMaterialSlots(LsfDoc const& material, std::string_view pass);

// The resources the material names for `pass` with a DirectX register and no Vulkan place (-1 for the set or the
// binding): a sampler of the engine's the Vulkan stages do not take, or a material whose .lsf was written without
// the Vulkan places of a pass that has Vulkan shaders all the same. Their Set and Binding say nothing.
std::vector<MaterialSlot> ReadUnplacedSlots(LsfDoc const& material, std::string_view pass);

// What a DirectX 11 stage declares of textures and samplers, each list by register. A texture's type is as a
// Windows resource's ("2d", "3d", "cube", "2darray", ...; "?" for a kind this code has no name for).
struct StageRegisters
{
    struct Texture { uint32_t Register{ 0 }; std::string Type; };
    std::vector<Texture> Textures;
    std::vector<uint32_t> Samplers;
};

// Reads them from a stage of a DirectX 11 container (its stage 0 with `vertex`, its stage 4 without). False,
// with `why`, when the stage is not one this code reads or is not of that kind.
bool ReadStageRegisters(std::span<uint8_t const> stage, bool vertex, StageRegisters& registers, std::string& why);

// What else says where a material's own things are, for a pass whose .lsf does not say it of all of them.
struct PassRegisters
{
    StageRegisters const* DirectX{ nullptr };       // of the same stage of the same pass
    std::span<MaterialSlot const> Unplaced;         // ReadUnplacedSlots
    std::string_view Unread;                        // why not, when that stage could not be read
};

struct EngineSlot
{
    uint32_t Set{ 0 };
    uint32_t Binding{ 0 };
    ResourceKind Kind{ ResourceKind::Buffer };
    std::string Type;       // as WindowsResource::Type
    std::string Name;       // the Mac argument
    // Paired by order within a group that always appears together (the four
    // light-probe textures, say), not by anything that tells its members apart.
    bool Inferred{ false };
};

struct EngineBlock
{
    std::string Name;
    std::vector<std::string> Members;       // in the Windows block's order; empty where the Mac block has no such member
    std::vector<std::string> Types;         // the Windows members' types and offsets: a shader's block must have
    std::vector<uint32_t> Offsets;          // these, or the first of them (a shader built before members were added at the end)
    // The names of the members the Mac block has no member for, as far as
    // they are known, by their place in the block: what a refusal says.
    std::map<uint32_t, std::string> Absent;
    // Members the Mac block has a member of the same name and type for that
    // holds another thing, by their place, with what that thing is: such a
    // member is not the Mac member, and a shader that reads it is refused.
    std::map<uint32_t, std::string> OtherMeaning;
    // Members that are paired with a Mac member by name and type alone: no
    // shader of the game reads them, so nothing was ever drawn that could
    // have shown the pairing wrong. A shader that reads one gets that member.
    std::vector<uint32_t> Untested;
    // Members the Mac block has no member for that can be worked out from
    // members it has, by their place. Such a member is put over a Mac member
    // (or over some of its numbers), and every read of it in the translated
    // source becomes an expression of what is there. The expressions rest on
    // how the engine fills the block, which is checked in the game that runs
    // (GameInterface.h); where that does not hold, the table is used without
    // them (InterfaceTable::WithoutDerived) and a shader that reads such a
    // member is refused as for any other member the Mac block lacks.
    struct DerivedMember
    {
        std::string From;       // the Mac member it is put over
        std::string FromType;   // and that member's type
        uint32_t Number{ 0 };   // the first of From's numbers it lies over
        // What a read becomes: $ is the member where it was put, {X} the
        // member of the same block the Mac game calls X.
        std::string Read;
    };
    std::map<uint32_t, DerivedMember> Derived;
};

struct AttributeName
{
    std::string Semantic;
    uint32_t Index{ 0 };
    std::string Name;       // the Mac attribute
    bool Inferred{ false }; // as EngineSlot::Inferred
};

struct InterfaceTable
{
    std::string Build;
    std::vector<EngineSlot> Slots;
    std::vector<EngineBlock> Blocks;
    std::vector<AttributeName> Attributes;
    // What a translated shader is known to draw wrong and is not put right
    // yet. A pass that touches one is refused, by name: Id for the log and a
    // report, What for the sentence that says it.
    struct OpenFault
    {
        std::string Id, What;
    };
    std::vector<OpenFault> Faults;
    // How a shader is known to touch a fault (Fault is an index into Faults):
    //  - it takes a thing of the engine's that the fault is about, by the
    //    Mac name the table has for it;
    //  - its vertex stage is one of the game's own (by SpirvModuleHash) that
    //    draws elsewhere translated, for a cause not found yet;
    //  - its two stages are those of a pass of the game's that draws another
    //    picture translated, for a cause not found yet.
    // The last two say nothing of a stage that is not the game's: such a
    // stage is translated. Both lists are sorted.
    struct FaultyResource { std::string Name; uint16_t Fault; };
    struct FaultyStage { std::array<uint8_t, 32> Stage; uint16_t Fault; };
    struct FaultyPair { std::array<uint8_t, 32> Vertex, Fragment; uint16_t Fault; };
    std::vector<FaultyResource> FaultyResources;
    std::vector<FaultyStage> FaultyVertexStages;
    std::vector<FaultyPair> FaultyPairs;
    // Every vertex stage of the game's own shaders, sorted: whether a stage
    // a mod ships is one of them is said when its material is translated.
    std::vector<std::array<uint8_t, 32>> GameVertexStages;

    OpenFault const* FaultOfResource(std::string_view name) const;
    OpenFault const* FaultOfVertexStage(std::array<uint8_t, 32> const& stage) const;
    OpenFault const* FaultOfPair(std::array<uint8_t, 32> const& vertex, std::array<uint8_t, 32> const& fragment) const;
    bool IsGameVertexStage(std::array<uint8_t, 32> const& stage) const
    {
        return std::binary_search(GameVertexStages.begin(), GameVertexStages.end(), stage);
    }
    // "open fault F1: ..." as a refusal says it.
    static std::string Said(OpenFault const& fault) { return "open fault " + fault.Id + ": " + fault.What; }

    // The table with no member worked out from others (EngineBlock::Derived).
    InterfaceTable WithoutDerived() const;

    // The table of textures the Windows shaders that light by clusters take a
    // probe's cubes and a light's cookie out of, and what stands for it on the
    // Mac (ShaderInterface.h: TextureTableRewrite). The Mac's five textures
    // are slots of their own in Slots, at the set and bindings named here,
    // which no shader has. It rests on how the Mac game's engine packs a
    // probe's and a light's whole numbers, which is checked in the game that
    // runs (GameInterface.h); where that does not hold, the table is used
    // without it and a shader that takes the table of textures is refused.
    std::optional<TextureTableRewrite> Textures;
    InterfaceTable WithoutTextureTable() const;

    EngineSlot const* Slot(uint32_t set, uint32_t binding) const;
    EngineBlock const* Block(std::string_view name) const;
    AttributeName const* Attribute(std::string_view semantic, uint32_t index) const;
};

// The table this build was made with.
InterfaceTable const& BuiltInInterface();

// What the installed game's functions agree on. A name that always has the
// same index is in it, the engine's own and the material's two constant
// blocks alike; one whose index differs from material to material (a
// material's texture) is not. A block whose members are the same everywhere
// has a fixed layout: the engine's do, the material's two do not.
class MacInterface
{
public:
    void Add(MacPass const& pass);

    MacResource const* Resource(std::string_view name, ResourceKind kind) const;
    bool HasFixedLayout(MacResource const& resource) const;
    // `type` is the Windows input's: the attribute must have been seen with that scalar.
    MacAttribute const* Attribute(std::string_view name, std::string_view type) const;
    size_t Passes() const { return passes_; }

    // As text, to keep between runs: reading every function of the game takes minutes the first time.
    std::string Serialize() const;
    static std::optional<MacInterface> Parse(std::string_view text);

private:
    struct Seen { MacResource Value; bool Fixed{ true }; bool FixedLayout{ true }; };
    struct SeenAttribute { MacAttribute Value; bool Fixed{ true }; std::string Scalars; };
    std::map<std::pair<std::string, ResourceKind>, Seen, std::less<>> resources_;
    std::map<std::string, SeenAttribute, std::less<>> attributes_;
    size_t passes_{ 0 };
};

// A resource of the material that was given a Mac index: what its .lsf must say.
struct PlacedSlot
{
    std::string Name;
    bool Vertex{ false };
    uint32_t Index{ 0 };
};

// Places everything `shader` takes. False, with `why`, when anything has no
// place: the shader is then not translated.
// An entry of the table that was paired by order alone is used like any
// other, and named in `byOrderOnly`: the material is translated, and what
// its translation rests on is said with it.
// A thing of the material's own that its .lsf gives no Vulkan place is put where the pass's DirectX 11 stage has
// it, when `registers` says what that stage declares: the material's textures (and its samplers) in the order of
// their Vulkan bindings are at the stage's first registers, from 0 on, in order, which holds of every texture and
// sampler of the game's own materials whose .lsf does say (50,599 and 7,559 of them in 4.1.1.7631656, none
// otherwise).
bool PlaceShader(SpirvShader& shader, std::span<InputSemantic const> inputs, std::span<MaterialSlot const> material,
    InterfaceTable const& table, MacInterface const& mac, std::vector<PlacedSlot>& placed, std::string& why,
    std::vector<std::string>* byOrderOnly = nullptr, PassRegisters const* registers = nullptr);

}
