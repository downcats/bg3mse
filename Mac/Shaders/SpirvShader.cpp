// See ShaderInterface.h. Not built with the precompiled header: SPIRV-Cross's
// own headers are all this file needs.

#include <Shaders/ShaderInterface.h>
#include <Shaders/SpirvIdentity.h>

#include <spirv_msl.hpp>
#include <spirv_parser.hpp>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

namespace bg3se::mac {

using namespace SPIRV_CROSS_NAMESPACE;

namespace {

std::string TypeOf(SPIRType const& type)
{
    std::string out;
    switch (type.basetype) {
    case SPIRType::Float: out = "f"; break;
    case SPIRType::Half: out = "h"; break;
    case SPIRType::Int: out = "i"; break;
    case SPIRType::UInt: out = "u"; break;
    case SPIRType::Boolean: out = "b"; break;
    case SPIRType::Struct: out = "s"; break;
    default: out = "?"; break;
    }
    out += std::to_string(type.vecsize * type.columns);
    // The outermost dimension is the last of the array.
    for (size_t i = type.array.size(); i--;) {
        out += type.array_size_literal[i] && type.array[i] ? "[" + std::to_string(type.array[i]) + "]" : "[]";
    }
    return out;
}

std::string TextureTypeOf(SPIRType const& type)
{
    std::string out;
    switch (type.image.dim) {
    case spv::Dim1D: out = "1d"; break;
    case spv::Dim2D: out = "2d"; break;
    case spv::Dim3D: out = "3d"; break;
    case spv::DimCube: out = "cube"; break;
    default: out = "?"; break;
    }
    if (type.image.arrayed) out += "array";
    if (type.image.ms) out += "ms";
    if (!type.array.empty()) out += "[]";
    return out;
}

}

namespace {

// Rewrites the module so that the built matrices the rule picks are used transposed.
void TransposeBuiltMatrices(std::vector<uint32_t>& words, MatrixRule const& rule)
{
    constexpr uint16_t kTypeBool = 20, kTypeInt = 21, kTypeFloat = 22, kTypeVector = 23, kTypeMatrix = 24, kCompositeConstruct = 80,
        kTranspose = 84, kVectorTimesMatrix = 144, kMatrixTimesVector = 145, kMatrixTimesMatrix = 146;
    uint32_t bound = words[3];
    std::vector<uint32_t> vectorSize(bound, 0), typeOf(bound, 0), matrixSize(bound, 0);
    std::vector<bool> scalarType(bound, false), builtVector(bound, false), builtFromScalars(bound, false);
    auto each = [&](auto&& visit) {
        for (size_t at = 5; at < words.size();) {
            uint32_t count = words[at] >> 16, op = words[at] & 0xffff;
            if (!count || at + count > words.size()) return;
            visit(at, count, op);
            at += count;
        }
    };
    each([&](size_t at, uint32_t count, uint32_t op) {
        if ((op == kTypeFloat || op == kTypeInt || op == kTypeBool) && count >= 2 && words[at + 1] < bound) scalarType[words[at + 1]] = true;
        if (op == kTypeVector && count == 4 && words[at + 1] < bound) vectorSize[words[at + 1]] = words[at + 3];
        if (op == kTypeMatrix && count == 4 && words[at + 1] < bound && words[at + 2] < bound && vectorSize[words[at + 2]] == words[at + 3]) {
            matrixSize[words[at + 1]] = words[at + 3];
        }
    });
    // An instruction whose first operand is a scalar, vector or matrix type has its result second.
    each([&](size_t at, uint32_t count, uint32_t) {
        if (count >= 3 && words[at + 1] < bound && words[at + 2] < bound
            && (scalarType[words[at + 1]] || vectorSize[words[at + 1]] || matrixSize[words[at + 1]])) {
            typeOf[words[at + 2]] = words[at + 1];
        }
    });
    each([&](size_t at, uint32_t count, uint32_t op) {
        if (op != kCompositeConstruct || count < 3 || words[at + 1] >= bound || !vectorSize[words[at + 1]] || words[at + 2] >= bound) return;
        builtVector[words[at + 2]] = true;
        bool scalars = true;
        for (uint32_t i = 3; i < count; i++) scalars = scalars && words[at + i] < bound && scalarType[typeOf[words[at + i]]];
        builtFromScalars[words[at + 2]] = scalars;
    });

    // The built matrices, in the order the module builds them, and how each is used.
    std::vector<uint32_t> ids;
    std::vector<BuiltMatrix> matrices;
    std::vector<uint32_t> indexOf(bound, UINT32_MAX);
    each([&](size_t at, uint32_t count, uint32_t op) {
        if (op != kCompositeConstruct || count < 3 || words[at + 1] >= bound || !matrixSize[words[at + 1]] || words[at + 2] >= bound) return;
        BuiltMatrix m;
        m.Size = matrixSize[words[at + 1]];
        m.ColumnsFromScalars = m.ColumnsBuilt = true;
        for (uint32_t i = 3; i < count; i++) {
            uint32_t column = words[at + i];
            m.ColumnsBuilt = m.ColumnsBuilt && column < bound && builtVector[column];
            m.ColumnsFromScalars = m.ColumnsFromScalars && column < bound && builtFromScalars[column];
        }
        indexOf[words[at + 2]] = (uint32_t)matrices.size();
        ids.push_back(words[at + 2]);
        matrices.push_back(m);
    });
    if (matrices.empty()) return;
    each([&](size_t at, uint32_t count, uint32_t op) {
        if (op == kCompositeConstruct && count >= 3 && words[at + 2] < bound && indexOf[words[at + 2]] != UINT32_MAX) return;      // its own definition
        for (uint32_t i = 1; i < count; i++) {
            uint32_t id = words[at + i];
            if (id >= bound || indexOf[id] == UINT32_MAX) continue;
            auto& m = matrices[indexOf[id]];
            if (op == kVectorTimesMatrix && i == 4) m.TimesVectorLeft = true;
            else if (op == kMatrixTimesVector && i == 3) m.TimesVectorRight = true;
            else if (op == kMatrixTimesMatrix && i == 3) m.TimesMatrixLeft = true;
            else if (op == kMatrixTimesMatrix && i == 4) m.TimesMatrixRight = true;
            else if (i >= 3) m.OtherUse = true;     // past a result type and a result: an operand
        }
    });

    std::vector<uint32_t> out(words.begin(), words.begin() + 5);
    each([&](size_t at, uint32_t count, uint32_t op) {
        size_t begin = out.size();
        out.insert(out.end(), words.begin() + at, words.begin() + at + count);
        if (op != kCompositeConstruct || count < 3 || words[at + 2] >= bound) return;
        uint32_t index = indexOf[words[at + 2]];
        if (index == UINT32_MAX || !rule(matrices[index], index)) return;
        // The matrix as built gets a new id; the id the shader uses is its transpose.
        uint32_t built = out[3]++, used = words[at + 2];
        out[begin + 2] = built;
        out.insert(out.end(), { (4u << 16) | kTranspose, words[at + 1], used, built });
    });
    words = std::move(out);
}

}

namespace {

// A member worked out from what the Mac block has: the name it was given, what a read of it becomes, and whether
// the shader is known to read it.
struct DerivedRead { std::string Name, Read; bool IsRead; };

}

struct SpirvShader::Impl
{
    std::unique_ptr<CompilerMSL> Compiler;
    spv::ExecutionModel Model{ spv::ExecutionModelMax };
    std::string Entry;
    std::vector<WindowsResource> Resources;
    std::vector<WindowsInput> Inputs;
    std::vector<bool> Bound;
    std::vector<bool> Placed;
    std::vector<DerivedRead> DerivedMembers;

    void AddBuffer(Resource const& r, bool storage)
    {
        auto& c = *Compiler;
        WindowsResource out;
        out.Kind = ResourceKind::Buffer;
        out.Set = c.get_decoration(r.id, spv::DecorationDescriptorSet);
        out.Binding = c.get_decoration(r.id, spv::DecorationBinding);
        out.Id = r.id;

        // A block of members, or a block whose one member is an array of them.
        SPIRType const* block = &c.get_type(r.base_type_id);
        uint32_t blockId = r.base_type_id;
        out.Type = "block";
        if (storage && block->member_types.size() == 1) {
            auto const& element = c.get_type(block->member_types[0]);
            if (!element.array.empty()) {
                out.Type = "array";
                out.Size = c.type_struct_member_array_stride(*block, 0);
                if (element.basetype == SPIRType::Struct) {
                    blockId = element.parent_type ? element.parent_type : block->member_types[0];
                    block = &c.get_type(blockId);
                    while (!block->array.empty() && block->parent_type) {
                        blockId = block->parent_type;
                        block = &c.get_type(blockId);
                    }
                } else {
                    out.Members.push_back({ {}, 0, TypeOf(element) });
                    block = nullptr;
                }
            }
        }
        out.TypeId = block ? blockId : 0;
        if (block) {
            if (out.Type == "block") out.Size = (uint32_t)c.get_declared_struct_size(*block);
            for (uint32_t i = 0; i < block->member_types.size(); i++) {
                out.Members.push_back({ {}, c.type_struct_member_offset(*block, i), TypeOf(c.get_type(block->member_types[i])) });
            }
        }
        if (out.Type == "block") {
            // Which members the shader reads: by the byte ranges it is found to use.
            out.ReadKnown = true;
            for (auto const& range : c.get_active_buffer_ranges(r.id)) out.Read.push_back(range.index);
        }
        Resources.push_back(std::move(out));
    }

    void AddOther(Resource const& r, ResourceKind kind)
    {
        auto& c = *Compiler;
        WindowsResource out;
        out.Kind = kind;
        out.Set = c.get_decoration(r.id, spv::DecorationDescriptorSet);
        out.Binding = c.get_decoration(r.id, spv::DecorationBinding);
        out.Id = r.id;
        auto const& type = c.get_type(r.type_id);
        out.Type = kind == ResourceKind::Texture ? TextureTypeOf(type) : (type.array.empty() ? "" : "[]");
        // How many, for several textures at one place.
        if (kind == ResourceKind::Texture && !type.array.empty() && type.array_size_literal[0]) out.Size = type.array[0];
        Resources.push_back(std::move(out));
    }
};

SpirvShader::SpirvShader() : impl_(std::make_unique<Impl>()) {}
SpirvShader::~SpirvShader() = default;

std::unique_ptr<SpirvShader> SpirvShader::Parse(std::span<uint8_t const> module, std::string& why, MatrixRule const& matrices,
    TextureTableRewrite const* textures)
{
    if (module.size() < 20 || module.size() % 4) {
        why = "the stage is not one SPIR-V module";
        return nullptr;
    }
    std::vector<uint32_t> words(module.size() / 4);
    memcpy(words.data(), module.data(), module.size());
    if (textures && RewriteTextureTable(words, *textures, why) == TextureTableResult::Refused) return nullptr;
    if (matrices && words.size() > 5 && words[3] < (1u << 22)) TransposeBuiltMatrices(words, matrices);

    std::unique_ptr<SpirvShader> shader(new SpirvShader());
    auto& impl = *shader->impl_;
    try {
        Parser parser(std::move(words));
        parser.parse();
        impl.Compiler = std::make_unique<CompilerMSL>(std::move(parser.get_parsed_ir()));
        auto& c = *impl.Compiler;

        auto entries = c.get_entry_points_and_stages();
        if (entries.size() != 1) {
            why = "the module has " + std::to_string(entries.size()) + " entry points";
            return nullptr;
        }
        impl.Model = entries[0].execution_model;
        impl.Entry = entries[0].name;
        if (impl.Model != spv::ExecutionModelVertex && impl.Model != spv::ExecutionModelFragment) {
            why = "the module is neither a vertex nor a fragment shader";
            return nullptr;
        }

        // Only what the entry point reaches: the rest is not in the Metal function either.
        auto active = c.get_active_interface_variables();
        auto resources = c.get_shader_resources(active);
        c.set_enabled_interface_variables(std::move(active));

        for (auto const& r : resources.uniform_buffers) impl.AddBuffer(r, false);
        for (auto const& r : resources.storage_buffers) impl.AddBuffer(r, true);
        // An array of structures: which members of an element are read, by the module's own instructions.
        for (auto& r : impl.Resources) {
            if (r.Kind == ResourceKind::Buffer && r.Type == "array" && r.TypeId) r.ReadKnown = SpirvElementMembersRead(module, r.Id, r.Read);
        }
        for (auto const& r : resources.separate_images) impl.AddOther(r, ResourceKind::Texture);
        // A texture the shader may also write (the engine's linear depth, read where it is taken): a texture all the same.
        for (auto const& r : resources.storage_images) impl.AddOther(r, ResourceKind::Texture);
        for (auto const& r : resources.separate_samplers) impl.AddOther(r, ResourceKind::Sampler);
        for (auto const& r : resources.stage_inputs) {
            if (impl.Model != spv::ExecutionModelVertex) break;
            impl.Inputs.push_back({ c.get_decoration(r.id, spv::DecorationLocation), TypeOf(c.get_type(r.type_id)), r.id });
        }

        // The Windows shadow passes for point lights clip by the face of the
        // tetrahedron they draw. The Mac game's functions for the same passes
        // do not (they draw the same whatever the face is), so neither do
        // these: the clip distances become outputs nothing reads.
        if (impl.Model == spv::ExecutionModelVertex) {
            uint32_t next = 0;
            for (auto const& r : resources.stage_outputs) next = std::max(next, c.get_decoration(r.id, spv::DecorationLocation) + 1);
            for (auto const& builtin : resources.builtin_outputs) {
                if (builtin.builtin != spv::BuiltInClipDistance && builtin.builtin != spv::BuiltInCullDistance) continue;
                auto const& type = c.get_type(builtin.resource.type_id);
                c.unset_decoration(builtin.resource.id, spv::DecorationBuiltIn);
                c.set_decoration(builtin.resource.id, spv::DecorationLocation, next);
                next += type.array.empty() ? 1 : type.array[0];
            }
        }

        std::string has;
        auto unsupported = [&](char const* what, size_t count) {
            if (count && has.empty()) has = what;
        };
        unsupported("combined image samplers", resources.sampled_images.size());
        unsupported("subpass inputs", resources.subpass_inputs.size());
        unsupported("push constants", resources.push_constant_buffers.size());
        unsupported("atomic counters", resources.atomic_counters.size());
        unsupported("acceleration structures", resources.acceleration_structures.size());
        if (!has.empty()) {
            why = "the shader has " + has;
            return nullptr;
        }
    } catch (std::exception const& e) {
        why = std::string("SPIRV-Cross: ") + e.what();
        return nullptr;
    }
    impl.Bound.assign(impl.Resources.size(), false);
    impl.Placed.assign(impl.Inputs.size(), false);
    return shader;
}

bool SpirvShader::IsVertex() const { return impl_->Model == spv::ExecutionModelVertex; }
std::vector<WindowsResource> const& SpirvShader::Resources() const { return impl_->Resources; }
std::vector<WindowsInput> const& SpirvShader::Inputs() const { return impl_->Inputs; }

void SpirvShader::Bind(WindowsResource const& resource, uint32_t index, std::string const& name)
{
    auto& c = *impl_->Compiler;
    MSLResourceBinding binding;
    binding.stage = impl_->Model;
    binding.desc_set = resource.Set;
    binding.binding = resource.Binding;
    binding.count = 1;
    binding.msl_buffer = binding.msl_texture = binding.msl_sampler = index;
    c.add_msl_resource_binding(binding);
    if (!name.empty()) c.set_name(resource.Id, name);
    impl_->Bound[(size_t)(&resource - impl_->Resources.data())] = true;
}

void SpirvShader::MoveMember(WindowsResource const& resource, uint32_t member, uint32_t offset, std::string const& name)
{
    auto& c = *impl_->Compiler;
    c.set_member_decoration(resource.TypeId, member, spv::DecorationOffset, offset);
    if (!name.empty()) c.set_member_name(resource.TypeId, member, name);
}

void SpirvShader::DeriveMember(WindowsResource const& resource, uint32_t member, uint32_t offset, std::string const& name, std::string const& read, bool isRead)
{
    auto& c = *impl_->Compiler;
    c.set_member_decoration(resource.TypeId, member, spv::DecorationOffset, offset);
    c.set_member_name(resource.TypeId, member, name);
    impl_->DerivedMembers.push_back({ name, read, isRead });
}

void SpirvShader::PlaceInput(WindowsInput const& input, uint32_t attribute, std::string const& name)
{
    auto& c = *impl_->Compiler;
    c.set_decoration(input.Id, spv::DecorationLocation, attribute);
    if (!name.empty()) c.set_name(input.Id, name);
    impl_->Placed[(size_t)(&input - impl_->Inputs.data())] = true;
}

namespace {

// A product the shader says must not be contracted (a position's, which
// every pass of a mesh has to work out to the same last bit) is written as a
// call of a helper, and the helper for a matrix and a vector adds the terms
// last to first. The Mac game's own functions add them first to last: a
// translated vertex function then puts a vertex a last bit away from where
// the game's function for the same shader puts it, which shows where that bit
// is multiplied up (a virtual texture's page table) and where two functions
// must agree on a depth. The helpers are made to add in the game's order.
// False when a helper is there and is not written as expected.
bool SumAsTheGameDoes(std::string& source, std::string& why)
{
    struct Helper { char const* Declaration; char const* Loop; char const* InOrder; };
    static constexpr Helper kHelpers[] = {
        { "spvFMulVectorMatrix(vec<T, Rows> v, matrix<T, Cols, Rows> m)", "for (uint i = Rows; i > 0; --i)", "for (uint i = 1; i <= Rows; ++i)" },
        { "spvFMulMatrixVector(matrix<T, Cols, Rows> m, vec<T, Cols> v)", "for (uint i = Cols; i > 0; --i)", "for (uint i = 1; i <= Cols; ++i)" },
    };
    for (auto const& helper : kHelpers) {
        bool called = source.find(std::string(helper.Declaration).substr(0, strlen("spvFMulVectorMatrix(")) ) != std::string::npos;
        auto declared = source.find(helper.Declaration);
        if (declared == std::string::npos) {
            if (called) { why = "a product helper of the translated source is not written as expected"; return false; }
            continue;
        }
        auto end = source.find("\n}\n", declared);
        auto loop = source.find(helper.Loop, declared);
        if (end == std::string::npos || loop == std::string::npos || loop > end
            || source.find("[i - 1]", loop) == std::string::npos || source.find("[i - 1]", loop) > end) {
            why = "a product helper of the translated source is not written as expected";
            return false;
        }
        source.replace(loop, strlen(helper.Loop), helper.InOrder);
    }
    return true;
}

// A product added to something, where the Windows compiler fused the two into one operation that rounds once
// (the module's Fma), is written as the product and the sum, as it was for the game's own Mac functions, compiled
// from the same shaders. The Windows compiler fused wherever it could; what the game's functions fuse is not that.
//
// A fragment function of the game's is compiled with fast math, where Metal's compiler fuses as it sees fit, and
// the two compilers do not choose alike. In a noise's remainder, x * y - floor(x * y / 289) * 289, the Windows
// module fuses x * y into the subtraction and the game's function fuses the other product into it; the remainders
// differ in a last bit, the noise's cell is another, and a translated shader moves a particle elsewhere than the
// game's own does. There the product and the sum are one expression, and which to fuse is left to Metal's compiler.
//
// A vertex function of the game's has a position that must come out the same in every pass, and is compiled so
// that it does (see PositionAsTheGameHasIt): Metal then fuses only what the source writes as one expression, and
// the game's source has a material's product and its sum as two statements. Where a vertex is moved by a number of
// chance made of them, fract((x * 165843.2 + time) * 0.3), fusing them moves it elsewhere than the game's own
// function does. There the product and the sum are two statements, which are never fused. Not so what the engine's
// own code of every vertex shader has as one expression, which the game's function fuses as the Windows module
// does, and which stays fused here:
//   - the helpers of a product the shader says must not be contracted (a matrix and a vector's, see
//     SumAsTheGameDoes), where each term is fused into the sum;
//   - the turn a vertex's quaternion stands for (its normal and tangents): sums of products of the components of
//     a normalized vector of four. Unfused, a normal handed on is a last bit off the game's own, and a pass whose
//     fragment stage looks something up by it draws other texels.
void MultiplyThenAddAsTheGameDoes(std::string& source, bool vertex)
{
    static constexpr std::string_view kFused = "fma(", kPlain = "bg3mse_multiply_add(", kUsing = "using namespace metal;\n";
    static constexpr std::string_view kHelper = "[[clang::optnone]]", kHelperEnd = "\n}\n";
    auto inName = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == ':'; };
    auto inHelper = [&](size_t at) {
        auto helper = source.rfind(kHelper, at);
        if (helper == std::string::npos) return false;
        auto end = source.find(kHelperEnd, helper);
        return end == std::string::npos || at < end;
    };
    // The quaternions (float4 q = normalize(...)) and the numbers made of one's components alone: a component
    // (float a = q.y) and the product of two (float b = a * a). The translation names each once, before its use.
    std::vector<std::string_view> quaternions, components;
    auto among = [](std::vector<std::string_view> const& names, std::string_view name) { return std::find(names.begin(), names.end(), name) != names.end(); };
    auto isComponent = [&](std::string_view text) {
        if (among(components, text)) return true;
        return text.size() > 2 && text[text.size() - 2] == '.' && std::string_view("xyzw").find(text.back()) != std::string_view::npos
            && among(quaternions, text.substr(0, text.size() - 2));
    };
    for (size_t line = 0; vertex && line < source.size();) {
        auto end = source.find('\n', line);
        if (end == std::string::npos) end = source.size();
        std::string_view text(source.data() + line, end - line);
        line = end + 1;
        if (auto first = text.find_first_not_of(' '); first != std::string_view::npos) text.remove_prefix(first);
        bool four = text.starts_with("float4 "), one = text.starts_with("float ");
        auto equals = text.find(" = ");
        if ((!four && !one) || equals == std::string_view::npos || text.empty() || text.back() != ';') continue;
        std::string_view name = text.substr(four ? 7 : 6, equals - (four ? 7 : 6)), value = text.substr(equals + 3, text.size() - equals - 4);
        if (four) {
            if (value.starts_with("fast::normalize(") || value.starts_with("normalize(")) quaternions.push_back(name);
        } else if (auto times = value.find(" * "); isComponent(value)
            || (times != std::string_view::npos && isComponent(value.substr(0, times)) && isComponent(value.substr(times + 3)))) {
            components.push_back(name);
        }
    }
    // Is everything this fused product and sum is made of a quaternion's component, or a constant?
    auto ofQuaternion = [&](size_t at) {
        size_t open = at + kFused.size() - 1, close = open;
        for (int depth = 0; close < source.size(); close++) {
            if (source[close] == '(') depth++;
            else if (source[close] == ')' && --depth == 0) break;
        }
        if (close >= source.size()) return false;
        std::string_view made(source.data() + open + 1, close - open - 1);
        bool any = false;
        for (size_t i = 0; i < made.size();) {
            char c = made[i];
            if (c >= '0' && c <= '9') {
                while (i < made.size() && ((made[i] >= '0' && made[i] <= '9') || made[i] == '.' || made[i] == 'e'
                    || ((made[i] == '-' || made[i] == '+') && made[i - 1] == 'e'))) i++;
            } else if (inName(c)) {
                size_t from = i;
                while (i < made.size() && (inName(made[i]) || made[i] == '.')) i++;
                std::string_view word = made.substr(from, i - from);
                if (word == "fma") continue;
                if (!isComponent(word)) return false;
                any = true;
            } else if (std::string_view(" *+-(),").find(c) != std::string_view::npos) {
                i++;
            } else {
                return false;
            }
        }
        return any;
    };
    bool any = false;
    for (size_t at = source.find(kFused); at != std::string::npos; at = source.find(kFused, at + 1)) {
        if (at && inName(source[at - 1])) continue;
        if (vertex && (inHelper(at) || ofQuaternion(at))) continue;
        source.replace(at, kFused.size(), kPlain);
        any = true;
    }
    auto top = source.find(kUsing);
    if (!any || top == std::string::npos) return;
    source.insert(top + kUsing.size(), std::string(
        "\ntemplate<typename A, typename B, typename C>\n"
        "static inline __attribute__((always_inline)) auto bg3mse_multiply_add(A a, B b, C c) { ")
        + (vertex ? "auto product = a * b; return product + c;" : "return (a * b) + c;") + " }\n");
}

// The position a vertex function hands on is marked invariant, as it is in every vertex function of the game's
// own. Compiled with the invariance preserved (which both the game's functions and the translated ones are, see
// where they are compiled), Metal's compiler then neither fuses nor reorders the function's arithmetic beyond what
// its source writes, which a translated function has to share with the game's to put a vertex where the game does.
void PositionAsTheGameHasIt(std::string& source)
{
    static constexpr std::string_view kPosition = "gl_Position [[position]]", kInvariant = "gl_Position [[position, invariant]]";
    if (auto at = source.find(kPosition); at != std::string::npos) source.replace(at, kPosition.size(), kInvariant);
}

// Every read of a member that is worked out becomes what it is worked out as. The member has a name nothing else
// has, so each place it is written is either its declaration in the block (once, a type before it and a
// semicolon after) or a read of it as a member of something with a name (`block.member`); the read, with the
// name it is a member of, is replaced. Anything else there, and the source is not what this knows how to rewrite.
bool ReadDerivedMembers(std::string& source, std::span<DerivedRead const> members, std::string& why)
{
    auto inName = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
    auto unexpected = [&](std::string const& name) {
        why = "the translated source does not read " + name + " the way it is rewritten";
        return false;
    };
    for (auto const& member : members) {
        size_t declared = 0, read = 0;
        for (size_t at = source.find(member.Name); at != std::string::npos; at = source.find(member.Name, at)) {
            size_t end = at + member.Name.size();
            if (!at || (end < source.size() && inName(source[end]))) return unexpected(member.Name);
            if (source[at - 1] == ' ') {
                if (end >= source.size() || source[end] != ';') return unexpected(member.Name);
                declared++;
                at = end;
                continue;
            }
            // A member of one named thing: not of what a call or an index gives.
            if (source[at - 1] != '.') return unexpected(member.Name);
            size_t begin = at - 1;
            while (begin && inName(source[begin - 1])) begin--;
            if (begin == at - 1 || (source[begin] >= '0' && source[begin] <= '9')) return unexpected(member.Name);
            if (begin && (source[begin - 1] == '.' || source[begin - 1] == ')' || source[begin - 1] == ']' || source[begin - 1] == '>')) return unexpected(member.Name);
            std::string of = source.substr(begin, at - 1 - begin), text;
            for (size_t i = 0; i < member.Read.size(); i++) {
                char c = member.Read[i];
                if (c == '$') {
                    text += of + "." + member.Name;
                } else if (c == '{') {
                    size_t close = member.Read.find('}', i);
                    if (close == std::string::npos) return unexpected(member.Name);
                    std::string other = member.Read.substr(i + 1, close - i - 1);
                    // The block declares it under that very name.
                    size_t has = source.find(" " + other + ";");
                    if (other.empty() || has == std::string::npos) return unexpected(member.Name);
                    text += of + "." + other;
                    i = close;
                } else {
                    text += c;
                }
            }
            source.replace(begin, end - begin, text);
            at = begin + text.size();
            read++;
        }
        if (declared != 1 || (member.IsRead && !read)) return unexpected(member.Name);
    }
    return true;
}

}

bool SpirvShader::Translate(std::string const& entry, std::string& source, std::string& why)
{
    for (size_t i = 0; i < impl_->Bound.size(); i++) {
        if (!impl_->Bound[i]) {
            why = "the resource at set " + std::to_string(impl_->Resources[i].Set) + ", binding "
                + std::to_string(impl_->Resources[i].Binding) + " has no place in the Mac shader";
            return false;
        }
    }
    for (size_t i = 0; i < impl_->Placed.size(); i++) {
        if (!impl_->Placed[i]) {
            why = "the vertex input at location " + std::to_string(impl_->Inputs[i].Location) + " has no Mac attribute";
            return false;
        }
    }

    auto& c = *impl_->Compiler;
    try {
        CompilerMSL::Options msl;
        msl.platform = CompilerMSL::Options::macOS;
        msl.set_msl_version(2, 3);              // what the game's minimum macOS (11) compiles
        c.set_msl_options(msl);

        auto common = c.get_common_options();
        // The toolkit's Vulkan vertex shaders end by negating y, for Vulkan's
        // clip space. Metal's is Direct3D's: negated once more, y is as the
        // shader's source has it.
        common.vertex.flip_vert_y = impl_->Model == spv::ExecutionModelVertex;
        c.set_common_options(common);

        c.rename_entry_point(impl_->Entry, entry, impl_->Model);
        source = c.compile();

        if (c.needs_swizzle_buffer() || c.needs_buffer_size_buffer() || c.needs_output_buffer() || c.needs_patch_output_buffer()
            || c.needs_view_mask_buffer() || c.needs_dispatch_base_buffer()) {
            why = "the Metal shader would need a buffer the game does not bind";
            return false;
        }
    } catch (std::exception const& e) {
        why = std::string("SPIRV-Cross: ") + e.what();
        return false;
    }
    bool vertex = impl_->Model == spv::ExecutionModelVertex;
    MultiplyThenAddAsTheGameDoes(source, vertex);
    if (vertex) PositionAsTheGameHasIt(source);
    return SumAsTheGameDoes(source, why) && ReadDerivedMembers(source, impl_->DerivedMembers, why);
}

}
