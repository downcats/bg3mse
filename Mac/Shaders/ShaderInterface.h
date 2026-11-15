#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <map>
#include <string>
#include <vector>

// What a shader takes from the engine, as each platform's build of it says.
//
// A Windows shader (SPIR-V, compiled by the toolkit) names nothing: a
// resource is a descriptor set and binding, a vertex input a location. A Mac
// shader (a function of the libraries the game ships) names everything: a
// resource is an argument with a name and an index, a vertex input an
// attribute with a name and an index. The two are read here into one
// vocabulary so they can be compared and one mapped onto the other.
//
// A type is written the same way for both: the scalar's letter (f, i, u, b,
// h) and how many of it (f3, a matrix by its scalar count: f12), then one
// "[n]" per array dimension ("[]" when its length is not known). A texture is
// "1d", "2d", "3d", "cube", with "array" and "ms" appended.

namespace bg3se::mac {

enum class ResourceKind : uint8_t { Buffer, Texture, Sampler };

struct BlockMember
{
    std::string Name;       // empty in a Windows shader
    uint32_t Offset{ 0 };
    std::string Type;
};

struct WindowsResource
{
    uint32_t Set{ 0 };
    uint32_t Binding{ 0 };
    ResourceKind Kind{ ResourceKind::Buffer };
    std::string Type;                       // a texture's type; "block" or "array" for a buffer
    uint32_t Size{ 0 };                     // a block's size, an array's element size; how many textures, for several at one place
    std::vector<BlockMember> Members;       // of the block, or of the array's element
    std::vector<uint32_t> Read;             // the members the shader reads, when that is known
    bool ReadKnown{ false };
    uint32_t Id{ 0 };                       // the module's id of the variable
    uint32_t TypeId{ 0 };                   // and of the type its members belong to
};

struct WindowsInput
{
    uint32_t Location{ 0 };
    std::string Type;
    uint32_t Id{ 0 };
};

struct MacResource
{
    std::string Name;
    ResourceKind Kind{ ResourceKind::Buffer };
    uint32_t Index{ 0 };
    std::string Type;                       // a texture's type; "block" for a buffer with members, else "bytes"
    uint32_t Size{ 0 };
    std::vector<BlockMember> Members;
    bool Used{ false };
    bool Depth{ false };                    // a depth texture
    bool Written{ false };                  // a texture the function may write to
    bool Read{ true };                      // and one it may read
};

struct MacAttribute
{
    std::string Name;
    uint32_t Index{ 0 };
    std::string Type;
};

struct MacPass
{
    std::vector<MacAttribute> Attributes;
    std::vector<MacResource> Vertex;
    std::vector<MacResource> Fragment;
};

// A square matrix a shader builds itself (not one it reads from a constant
// block or takes from vertex attributes), and how.
struct BuiltMatrix
{
    uint32_t Size{ 0 };
    bool ColumnsFromScalars{ false };       // every column is a vector built in the shader from scalars
    bool ColumnsBuilt{ false };             // every column is a vector built in the shader
    bool TimesVectorLeft{ false };          // used as  vector * matrix
    bool TimesVectorRight{ false };         // used as  matrix * vector
    bool TimesMatrixLeft{ false };          // used as  this * matrix
    bool TimesMatrixRight{ false };         // used as  matrix * this
    bool OtherUse{ false };
};
// Whether the `index`-th built matrix of a module is to be used transposed.
using MatrixRule = std::function<bool(BuiltMatrix const& matrix, uint32_t index)>;

// The table of textures. A Windows shader that lights by clusters takes the
// light probes' cubes and the lights' cookies out of one unsized array of
// textures (declared twice, as cubes and as flat textures), at a place a
// probe's or a light's whole numbers name. The Mac game has no such table:
// it keeps the cubes as layers of cube arrays (one for the diffuse cubes,
// several for the specular ones) and the cookies as layers of several flat
// arrays, and its engine writes the same whole numbers as an array's number
// and a layer. RewriteTextureTable changes a module to take them the Mac's
// way; SpirvTextureTable.cpp says what it recognises. The new textures are
// declared at NewSet and NewBindings: the diffuse colour, the diffuse alpha,
// the specular colour, the specular alpha, the cookies.
struct TextureTableRewrite
{
    uint32_t Set{ 0 }, Binding{ 0 };                                            // the table
    uint32_t ProbesSet{ 0 }, ProbesBinding{ 0 }, DiffuseMember{ 0 }, SpecularMember{ 0 };
    uint32_t LightsSet{ 0 }, LightsBinding{ 0 }, CookieMember{ 0 };
    uint32_t SpecularArrays{ 0 }, CookieArrays{ 0 };
    uint32_t NewSet{ 0 }, NewBindings[5]{};
};
// NotThere: the module does not take the table. Refused, with `why`: it takes
// it in a way that is not one of those recognised, and is left as it was.
enum class TextureTableResult { NotThere, Done, Refused };
TextureTableResult RewriteTextureTable(std::vector<uint32_t>& words, TextureTableRewrite const& how, std::string& why);

// A SPIR-V module of one stage: what it takes, and its translation to Metal
// once every resource and input has been given its Mac place.
class SpirvShader
{
public:
    // nullptr, with `why`, when the module is not one this code translates.
    // With `textures`, a module that takes the table of textures is rewritten to take the Mac's (or refused).
    static std::unique_ptr<SpirvShader> Parse(std::span<uint8_t const> module, std::string& why, MatrixRule const& matrices = {},
        TextureTableRewrite const* textures = nullptr);
    ~SpirvShader();

    bool IsVertex() const;
    std::vector<WindowsResource> const& Resources() const;
    std::vector<WindowsInput> const& Inputs() const;

    void Bind(WindowsResource const& resource, uint32_t index, std::string const& name);
    void MoveMember(WindowsResource const& resource, uint32_t member, uint32_t offset, std::string const& name);
    // A member that is worked out from what the Mac block has at `offset`: it is put there under `name`, which
    // has to be a name nothing else in the shader has, and in the translated source every read of it becomes
    // `read`, where $ stands for the member and {X} for the member of the same block that is named X. With
    // `isRead`, a translation in which no read of it is found is refused.
    void DeriveMember(WindowsResource const& resource, uint32_t member, uint32_t offset, std::string const& name, std::string const& read, bool isRead);
    void PlaceInput(WindowsInput const& input, uint32_t attribute, std::string const& name);

    // The Metal source of the stage, its function named `entry`. False, with
    // `why`, when something was left unplaced or cannot be expressed.
    bool Translate(std::string const& entry, std::string& source, std::string& why);

private:
    SpirvShader();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// One draw, for comparing two pairs of functions: what is bound at each index
// is the same whichever pair draws.
struct DrawSetup
{
    // A stage's indices are its own: each thing is bound for the vertex or for the fragment function.
    struct Buffer { uint32_t Index; std::vector<uint8_t> Bytes; bool Vertex; };
    // Noise: which noise fills it. Written: as large as the targets, for what a function writes to it by pixel to land.
    // LowBits: when not negative, only bytes that read as a float whose lowest four bits are no more than this.
    // AtLeast: no byte is less than this. Sparse: three texels in four are zero.
    // Levels: it has smaller levels too, each with numbers of its own, so that the level a function samples at shows.
    // Quarters: each quarter of it holds one number, the one of that quarter's first texel.
    static constexpr uint32_t kTextureAcross = 8;       // how many texels across a texture drawn with is, unless it is written to
    struct Texture { uint32_t Index; std::string Type; bool Depth; uint32_t Noise; bool Vertex; bool Written{ false }; int LowBits{ -1 }; int AtLeast{ 0 }; bool Sparse{ false }; bool Flat{ false }; bool Levels{ false }; bool Quarters{ false }; };
    // Flat: every texel is the first one, so that where a function looks a thing up makes no difference to what it finds.
    // Which: one of four ways of sampling, so that a function that samples through another sampler's place draws otherwise.
    struct Sampler { uint32_t Index; bool Vertex; uint32_t Which{ 0 }; };
    struct Attribute { uint32_t Index; char Scalar; uint32_t Noise{ 0 }; };     // 'f', 'i' or 'u'; Noise: which values it gets

    // The three attributes that are the rows of an instance's matrix, when they are to be a turn, one scale for all
    // three axes and a move, as an object's matrix in the game mostly is, instead of twelve numbers of chance.
    int32_t MatrixRows[3]{ -1, -1, -1 };
    std::vector<Buffer> Buffers;
    std::vector<Texture> Textures;
    std::vector<Sampler> Samplers;
    std::vector<Attribute> Attributes;      // each vertex has four random values per attribute
    uint32_t VertexBuffer{ 30 };            // where the functions take their vertices
    uint32_t Vertices{ 384 };
    uint32_t Seed{ 1 };
};

struct DrawResult
{
    static constexpr uint32_t kSize = 64;
    static constexpr uint32_t kTargets = 8;
    std::vector<float> Color[kTargets];     // kSize * kSize * 4 each
    std::vector<float> Depth;               // kSize * kSize
    // What each plain 2D texture held before the draw and holds after it, by
    // its noise: a function may write to one.
    struct Texture { std::vector<uint8_t> Before, After; };
    std::map<uint32_t, Texture> Textures;
};

// Metal functions and what they take: the game's libraries, or source compiled here.
class MetalFunctions
{
public:
    static std::unique_ptr<MetalFunctions> OpenGame(std::string const& appBundle, std::string& why);
    static std::unique_ptr<MetalFunctions> Compile(std::span<std::string const> sources, std::string& why);
    ~MetalFunctions();

    // Whether one of the libraries answers for a function of that name.
    bool Has(std::string const& function) const;

    // `fragment` may be empty (a depth pass).
    bool Reflect(std::string const& vertex, std::string const& fragment, MacPass& pass, std::string& why) const;

    // Draws `setup` with the two functions into float targets cleared to
    // DrawResult's sentinel values, and reads them back.
    // `fragmentFrom`, when given, is where the fragment function is looked up
    // instead: one pair's vertex function then draws with the other's fragment
    // function, which tells in which stage two pairs come apart.
    bool Draw(std::string const& vertex, std::string const& fragment, DrawSetup const& setup, DrawResult& result, std::string& why,
        MetalFunctions const* fragmentFrom = nullptr) const;
    static constexpr float kClearColor = -7.0f;

private:
    MetalFunctions();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
