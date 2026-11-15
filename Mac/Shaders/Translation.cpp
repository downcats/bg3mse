#include <stdafx.h>
#include <Shaders/SpirvIdentity.h>
#include <Shaders/Translation.h>

extern "C" {
#include <Shaders/lsf_write.h>
}

namespace bg3se::mac {

namespace {

// The passes the Mac game has stubs for. A mod's ST_BAKE has none there.
constexpr std::string_view kMacPasses[] = {
    "ST_FOR", "STI_FOR", "ST_DEF", "STI_DEF", "ST_DEP", "STI_DEP", "ST_DEPS", "STI_DEPS",
    "ST_DEPST", "STI_DEPST", "ST_VEL", "STI_VEL", "ST_EMI", "STI_EMI",
};
constexpr std::string_view kMaterialExt = ".lsf";

void Put32(std::vector<uint8_t>& out, uint32_t value)
{
    out.insert(out.end(), (uint8_t*)&value, (uint8_t*)&value + sizeof(value));
}

struct Document
{
    LsfDoc Doc{};
    bool Ok{ false };
    Document(std::vector<uint8_t> const& bytes, std::string& why)
    {
        char err[256] = "";
        Ok = lsf_parse(bytes.data(), bytes.size(), &Doc, err, sizeof(err));
        if (!Ok) why = std::string("its .lsf does not parse: ") + err;
    }
    ~Document() { if (Ok) lsf_free(&Doc); }
};

// The .lsf with the two constant blocks at the Mac game's indices for the
// translated passes, and MetalReady set. Everything else is the mod's.
std::optional<std::vector<uint8_t>> MacReadyLsf(LsfDoc const& doc, std::vector<std::string> const& passes, uint32_t engineIndex,
    uint32_t materialIndex, std::string& why)
{
    char err[256] = "";
    LsfNames names;
    if (!lsf_names_init(&names, &doc, err, sizeof(err))) { why = err; return {}; }

    std::vector<LsfOutNode> nodes(doc.node_count);
    for (uint32_t i = 0; i < doc.node_count; i++) nodes[i] = { doc.nodes[i].name_ref, doc.nodes[i].parent };

    // Which block's index an attribute holds, when it holds one.
    auto blockIndex = [&](uint32_t node) -> std::optional<uint32_t> {
        auto const& n = doc.nodes[node];
        if (n.parent < 0) return {};
        bool engine = !strcmp(n.name, "EngineCBBinding"), constants = !strcmp(n.name, "MaterialCBSize");
        if (!engine && !constants) return {};
        std::string_view description = doc.nodes[n.parent].name;
        for (auto const& pass : passes) {
            if (description == "ShaderDesc_" + std::string(PassNodeName(pass))) return engine ? engineIndex : materialIndex;
        }
        return {};
    };

    // The attributes in the order the file stores them, which is the order it is written back in.
    struct Stored { LsfAttr const* Attr; uint32_t Node; };
    std::vector<Stored> stored;
    for (uint32_t n = 0; n < doc.node_count; n++) {
        for (uint32_t a = 0; a < doc.nodes[n].attr_count; a++) stored.push_back({ &doc.attrs[doc.nodes[n].attr_begin + a], n });
    }
    std::sort(stored.begin(), stored.end(), [](Stored const& a, Stored const& b) { return a.Attr->index < b.Attr->index; });

    std::vector<LsfOutAttr> attrs;
    std::vector<std::unique_ptr<int64_t>> replaced;
    static const uint8_t kTrue = 1;
    bool ready = false, ok = true;
    for (auto const& s : stored) {
        LsfOutAttr out{ s.Node, s.Attr->name_ref, s.Attr->type, s.Attr->value, s.Attr->size };
        bool dx = !strcmp(s.Attr->name, "DxVsIndex") || !strcmp(s.Attr->name, "DxPsIndex");
        int64_t value = 0;
        if (dx && lsf_attr_int(s.Attr, &value) && value >= 0) {
            // A stage that takes the block takes it where the Mac game binds it:
            // written as the whole number the attribute is, of whatever width
            // (a material of the game's has it in one byte).
            if (auto index = blockIndex(s.Node)) {
                bool fits = s.Attr->size == 8 || (s.Attr->size >= 1 && s.Attr->size <= 4 && *index < (1ull << (8 * s.Attr->size - 1)));
                if (!fits) { lsf_names_free(&names); why = "a constant block's index in its .lsf is not a whole number that can hold the Mac game's"; return {}; }
                replaced.push_back(std::make_unique<int64_t>((int64_t)*index));
                out.value = (uint8_t const*)replaced.back().get();     // little-endian: the low bytes are the number
            }
        } else if (dx && blockIndex(s.Node) && !lsf_attr_int(s.Attr, &value)) {
            lsf_names_free(&names);
            why = "a constant block's index in its .lsf is not a whole number";
            return {};
        }
        if (s.Node == 0 && !strcmp(s.Attr->name, "MetalReady")) {
            ready = true;
            out.value = &kTrue;
            out.size = 1;
            out.type = LSF_T_BOOL;
        }
        attrs.push_back(out);
    }
    if (!ready) {
        uint32_t ref = 0;
        ok = lsf_names_ref(&names, "MetalReady", &ref, err, sizeof(err));
        attrs.push_back({ 0, ref, LSF_T_BOOL, &kTrue, 1 });
    }

    uint8_t* bytes = nullptr;
    size_t size = 0;
    ok = ok && lsf_write(&doc, &names, nodes.data(), (uint32_t)nodes.size(), attrs.data(), (uint32_t)attrs.size(), &bytes, &size, err, sizeof(err));
    lsf_names_free(&names);
    if (!ok) { why = std::string("its .lsf cannot be rewritten: ") + err; return {}; }
    std::vector<uint8_t> out(bytes, bytes + size);
    free(bytes);

    // What is served must read back as the document it was made from, with the one attribute more.
    LsfDoc back{};
    if (!lsf_parse(out.data(), out.size(), &back, err, sizeof(err))) { why = std::string("its rewritten .lsf does not parse: ") + err; return {}; }
    bool same = back.node_count == doc.node_count && back.attr_count == doc.attr_count + (ready ? 0 : 1);
    for (uint32_t i = 0; same && i < doc.node_count; i++) {
        same = !strcmp(back.nodes[i].name, doc.nodes[i].name) && back.nodes[i].parent == doc.nodes[i].parent;
    }
    // And it must say what the translated functions take: a block of a translated
    // pass at the Mac game's index in every stage that takes it. The engine binds
    // a material's blocks where its .lsf says, and a function that reads another
    // place reads whatever is there.
    bool placed = true;
    for (uint32_t i = 0; same && i < back.node_count; i++) {
        auto const& n = back.nodes[i];
        if (n.parent < 0) continue;
        bool engine = !strcmp(n.name, "EngineCBBinding"), constants = !strcmp(n.name, "MaterialCBSize");
        if (!engine && !constants) continue;
        std::string_view description = back.nodes[n.parent].name;
        bool translated = std::any_of(passes.begin(), passes.end(), [&](auto const& pass) {
            return description == "ShaderDesc_" + std::string(PassNodeName(pass));
        });
        if (!translated) continue;
        for (char const* stage : { "DxVsIndex", "DxPsIndex" }) {
            int64_t index = -1;
            auto attr = lsf_attr(&back, i, stage);
            if (attr && (!lsf_attr_int(attr, &index) || (index >= 0 && index != (int64_t)(engine ? engineIndex : materialIndex)))) placed = false;
        }
    }
    lsf_free(&back);
    if (!same) { why = "its rewritten .lsf is not the document it was made from"; return {}; }
    if (!placed) { why = "its rewritten .lsf does not put a constant block where the translated functions take it"; return {}; }
    return out;
}

}

std::vector<uint8_t> BuildMetalStub(std::string_view name, std::string_view vertexFunction, std::string_view fragmentFunction)
{
    std::vector<uint8_t> out = { 'B', 'S', 'H', 'D' };
    Put32(out, 2);
    uint32_t padded = (uint32_t)((name.size() + 4) & ~(size_t)3);
    Put32(out, padded);
    Put32(out, (uint32_t)name.size());
    out.insert(out.end(), name.begin(), name.end());
    out.resize(out.size() + padded - name.size());
    // No reflection table; the vertex stage first, the fragment stage fifth.
    uint32_t sizes[7] = { 0, (uint32_t)vertexFunction.size(), 0, 0, 0, (uint32_t)fragmentFunction.size(), 0 };
    for (uint32_t size : sizes) Put32(out, size);
    out.insert(out.end(), vertexFunction.begin(), vertexFunction.end());
    out.insert(out.end(), fragmentFunction.begin(), fragmentFunction.end());
    return out;
}

std::string TranslatedFunctionName(std::span<uint8_t const> vulkanContainer, bool vertex)
{
    auto hash = ContainerHash(vulkanContainer);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string name(kTranslatedPrefix);
    for (size_t i = 0; i < 16; i++) {
        name.push_back(kHex[hash[i] >> 4]);
        name.push_back(kHex[hash[i] & 15]);
    }
    return name + (vertex ? "_vertexMain" : "_fragmentMain");
}

MatrixRule const& MacMatrices()
{
    static const MatrixRule rule = [](BuiltMatrix const& matrix, uint32_t) { return matrix.Size == 2 && matrix.ColumnsFromScalars; };
    return rule;
}

bool TranslateStage(std::span<uint8_t const> stage, std::string const& entry, std::span<InputSemantic const> inputs,
    std::span<MaterialSlot const> material, InterfaceTable const& table, MacInterface const& mac,
    std::vector<PlacedSlot>& placed, std::string& source, std::string& why, MatrixRule const& matrices,
    std::vector<std::string>* byOrderOnly, PassRegisters const* registers)
{
    auto modules = VulkanStageModules(stage);
    if (modules.empty()) {
        why = "a stage is not a chunk of one or two SPIR-V modules";
        return false;
    }
    for (size_t i = 0; i < modules.size(); i++) {
        std::string reason, translated;
        std::vector<PlacedSlot> slots;
        auto shader = SpirvShader::Parse(modules[i], reason, matrices, table.Textures ? &*table.Textures : nullptr);
        if (!shader || !PlaceShader(*shader, inputs, material, table, mac, slots, reason, i == 0 ? byOrderOnly : nullptr, registers)
            || !shader->Translate(entry, translated, reason)) {
            why = reason;
            return false;
        }
        if (i == 0) {
            source = std::move(translated);
            placed.insert(placed.end(), slots.begin(), slots.end());
        } else if (translated != source) {
            why = "the stage's two shaders do not come to the same Metal function";
            return false;
        }
    }
    return true;
}

uint32_t VulkanTableVersion(ContainerParts const& parts)
{
    uint32_t version = 0;
    if (parts.Reflection.size() >= 4) memcpy(&version, parts.Reflection.data(), 4);
    return version;
}

std::optional<TranslatedMaterial> TranslateMaterial(MaterialSource const& mod, std::string const& material, InterfaceTable const& table,
    MacInterface const& mac, std::string& why, std::vector<std::string>* everyReason)
{
    if (!material.ends_with(kMaterialExt)) { why = "not a material"; return {}; }
    std::string stem = material.substr(0, material.size() - kMaterialExt.size());
    auto baseName = [](std::string const& path) { return path.substr(path.rfind('/') + 1); };

    auto lsfBytes = mod.Read(material);
    if (!lsfBytes) { why = "cannot read " + material; return {}; }
    Document document(*lsfBytes, why);
    if (!document.Ok) return {};

    // A material with shader descriptions of its own for skinned meshes (ShaderDesc_Skinned...) was made for a game
    // that drew those with shaders that take the bones. This game has no such description: it draws a skinned mesh
    // through the Static ones and gives their vertex stage the positions it has moved already, at an offset the
    // description has a place for (TransformedVerticesOffsetCurr). The static stages of such a material take a
    // mesh as it was modelled and have no such place: translated, a skinned mesh would not follow the body.
    for (uint32_t i = 0; i < document.Doc.node_count; i++) {
        if (strncmp(document.Doc.nodes[i].name, "ShaderDesc_Skinned", strlen("ShaderDesc_Skinned")) != 0) continue;
        why = std::string("it has shaders of its own for skinned meshes (") + document.Doc.nodes[i].name + "), which this game does not have: it was "
            "built for an earlier game, and its other shaders do not take the positions this game has moved a skinned mesh's vertices to, so "
            "such a mesh would not follow the body";
        if (everyReason) everyReason->push_back(why);
        return {};
    }

    auto engine = mac.Resource("CB_EngineParams", ResourceKind::Buffer), constants = mac.Resource("CB_MaterialConstants", ResourceKind::Buffer);
    if (!engine || !constants) { why = "the Mac game has no fixed place for a material's constant blocks"; return {}; }

    TranslatedMaterial out;
    std::vector<std::string> passes;
    bool refused = false;
    uint32_t ownVertexStages = 0;
    for (auto tag : kMacPasses) {
        std::string pass(tag), passStem = stem + "_" + pass;
        if (!mod.Names.contains(passStem + "_Vulkan.bshd")) continue;
        std::string first;
        auto fail = [&](std::string const& reason) {
            if (first.empty()) first = pass + ": " + reason;
            if (everyReason) everyReason->push_back(pass + ": " + reason);
            return false;
        };
        // True when the pass is translated. A refusal ends the pass, except
        // that with every reason wanted the vertex stage's proof and each
        // stage's own refusal are all looked at.
        auto translate = [&]() -> bool {

            auto vulkan = mod.Read(passStem + "_Vulkan.bshd");
            auto dx11 = mod.Read(passStem + "_DX11.bshd");
            ContainerParts parts, dxParts;
            if (!vulkan || !ParseContainer(*vulkan, parts)) return fail("its Vulkan container cannot be read");
            if (!dx11 || !ParseContainer(*dx11, dxParts)) return fail("its DirectX 11 container, which names the vertex inputs, cannot be read");
            for (int stage : { 1, 2, 3, 5 }) {
                if (!parts.Stages[stage].empty()) return fail("it has a stage that is neither vertex nor fragment");
            }
            if (parts.Stages[0].empty()) return fail("it has no vertex stage");
            if (auto version = VulkanTableVersion(parts); version != kVulkanTableVersion) {
                return fail("its container is of version " + std::to_string(version) + " where the game's own are of version "
                    + std::to_string(kVulkanTableVersion) + ": it was built for another version of the game, and where that one's engine kept what it supplies is not known");
            }

            // A stage of the game's own that is known to draw elsewhere translated, or a pair of its
            // stages known to draw another picture, is refused by the fault's name. A stage that is not
            // the game's is translated: what stands behind that is every stage of the game's that is
            // drawn both ways, not a likeness to one of them.
            {
                SpirvHash vertex{}, fragment{};
                std::string unread;
                auto modules = VulkanStageModules(parts.Stages[0]);
                if (modules.empty() || !SpirvModuleHash(modules[0], vertex, unread)) return fail("its vertex stage cannot be read");
                if (!table.IsGameVertexStage(vertex)) ownVertexStages++;
                if (auto fault = table.FaultOfVertexStage(vertex)) {
                    fail("its vertex stage is one of the game's own that draws elsewhere translated: " + InterfaceTable::Said(*fault));
                    if (!everyReason) return false;
                }
                auto fragments = VulkanStageModules(parts.Stages[4]);
                if (!fragments.empty() && SpirvModuleHash(fragments[0], fragment, unread)) {
                    if (auto fault = table.FaultOfPair(vertex, fragment)) {
                        fail("its two stages are those of a pass of the game's that draws another picture translated: " + InterfaceTable::Said(*fault));
                        if (!everyReason) return false;
                    }
                }
            }

            std::vector<InputSemantic> inputs;
            std::string reason;
            if (!ReadInputSignature(dxParts.Stages[0], inputs, reason)) return fail(reason);
            auto slots = ReadMaterialSlots(document.Doc, pass);
            // For what the .lsf names for the pass and gives no Vulkan place, or does not name at all.
            auto unplaced = ReadUnplacedSlots(document.Doc, pass);

            TranslatedPass translated;
            translated.Pass = pass;
            std::vector<PlacedSlot> placed;
            for (int s = 0; s < 2; s++) {
                auto stage = parts.Stages[s ? 4 : 0];
                if (stage.empty()) continue;
                auto name = TranslatedFunctionName(*vulkan, s == 0);
                std::string source, unread;
                StageRegisters declares;
                PassRegisters registers{ ReadStageRegisters(dxParts.Stages[s ? 4 : 0], s == 0, declares, unread) ? &declares : nullptr, unplaced, unread };
                if (!TranslateStage(stage, name, inputs, slots, table, mac, placed, source, reason, MacMatrices(), &out.ByOrderOnly, &registers)) {
                    fail(reason);
                    if (!everyReason) return false;
                    continue;
                }
                (s ? translated.FragmentName : translated.VertexName) = name;
                (s ? translated.FragmentSource : translated.VertexSource) = std::move(source);
            }
            if (!first.empty()) return false;
            translated.Stub = BuildMetalStub(baseName(passStem) + "_Metal", translated.VertexName, translated.FragmentName);
            out.Passes.push_back(std::move(translated));
            passes.push_back(pass);
            return true;
        };
        if (!translate()) {
            if (!refused) why = first;
            refused = true;
            if (!everyReason) return {};
        }
    }
    if (refused) return {};
    if (out.Passes.empty()) { why = "it has no pass the Mac game draws"; return {}; }

    auto lsf = MacReadyLsf(document.Doc, passes, engine->Index, constants->Index, why);
    if (!lsf) return {};
    out.Lsf = std::move(*lsf);
    out.OwnVertexStages = ownVertexStages;
    std::sort(out.ByOrderOnly.begin(), out.ByOrderOnly.end());
    out.ByOrderOnly.erase(std::unique(out.ByOrderOnly.begin(), out.ByOrderOnly.end()), out.ByOrderOnly.end());
    return out;
}

}
