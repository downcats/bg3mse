#include <stdafx.h>
#include <Shaders/ShaderMapping.h>

#include <algorithm>
#include <cstring>

namespace bg3se::mac {

namespace {

uint32_t ReadU32(uint8_t const* p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

constexpr std::pair<std::string_view, std::string_view> kPassNodes[] = {
    { "FOR", "Forward" }, { "DEF", "Deferred" }, { "DEP", "Depth" }, { "DEPS", "DepthShadow" },
    { "DEPST", "DepthShadowTetrahedron" }, { "VEL", "Velocity" }, { "EMI", "Emissive" }, { "BAKE", "Bake" },
};
// One string per pass, built once: PassNodeName hands out views of them.
std::vector<std::pair<std::string, std::string>> const& PassNodes()
{
    static const auto nodes = [] {
        std::vector<std::pair<std::string, std::string>> out;
        for (auto const& [tag, name] : kPassNodes) {
            out.emplace_back("ST_" + std::string(tag), "Static" + std::string(name));
            out.emplace_back("STI_" + std::string(tag), "StaticInstanced" + std::string(name));
        }
        return out;
    }();
    return nodes;
}

std::optional<int64_t> IntAttr(LsfDoc const& doc, uint32_t node, char const* name)
{
    int64_t value = 0;
    auto attr = lsf_attr(&doc, node, name);
    if (!attr || !lsf_attr_int(attr, &value)) return {};
    return value;
}

std::string TextAttr(LsfDoc const& doc, uint32_t node, char const* name)
{
    char buffer[512] = "";
    auto attr = lsf_attr(&doc, node, name);
    if (!attr || !lsf_attr_text(attr, buffer, sizeof(buffer))) return {};
    return buffer;
}

// The size and alignment of a member by its type string, for placing a member the Mac block lacks.
uint32_t ScalarCount(std::string const& type)
{
    uint32_t n = 0;
    for (size_t i = 1; i < type.size() && isdigit((unsigned char)type[i]); i++) n = n * 10 + (uint32_t)(type[i] - '0');
    return n;
}

}

bool ReadInputSignature(std::span<uint8_t const> stage, std::vector<InputSemantic>& inputs, std::string& why)
{
    inputs.clear();
    // "SHD2", three words of the toolkit's, a hash; then the DXBC container.
    constexpr size_t kHeader = 18;
    if (stage.size() < kHeader + 32 || memcmp(stage.data(), "SHD2", 4) != 0 || memcmp(stage.data() + kHeader, "DXBC", 4) != 0) {
        why = "the DirectX 11 stage is not a SHD2 chunk";
        return false;
    }
    auto dxbc = stage.subspan(kHeader);
    uint32_t total = ReadU32(dxbc.data() + 24), chunks = ReadU32(dxbc.data() + 28);
    if (total != dxbc.size() || chunks > 64 || 32 + (uint64_t)chunks * 4 > dxbc.size()) {
        why = "the DirectX 11 container is malformed";
        return false;
    }
    for (uint32_t i = 0; i < chunks; i++) {
        uint64_t at = ReadU32(dxbc.data() + 32 + i * 4);
        if (at + 8 > dxbc.size()) break;
        if (memcmp(dxbc.data() + at, "ISGN", 4) != 0) continue;
        uint64_t size = ReadU32(dxbc.data() + at + 4);
        if (at + 8 + size > dxbc.size() || size < 8) break;
        auto chunk = dxbc.subspan(at + 8, size);
        uint32_t count = ReadU32(chunk.data());
        if (count > 64 || 8 + (uint64_t)count * 24 > chunk.size()) break;
        for (uint32_t e = 0; e < count; e++) {
            auto element = chunk.data() + 8 + e * 24;
            uint32_t name = ReadU32(element), index = ReadU32(element + 4), systemValue = ReadU32(element + 8);
            if (name >= chunk.size()) { why = "the input signature is malformed"; return false; }
            auto end = (uint8_t const*)memchr(chunk.data() + name, 0, chunk.size() - name);
            if (!end) { why = "the input signature is malformed"; return false; }
            if (systemValue) continue;      // SV_VertexID, SV_InstanceID: not vertex data
            inputs.push_back({ std::string((char const*)chunk.data() + name, (char const*)end), index });
        }
        return true;
    }
    why = "the DirectX 11 container has no input signature";
    return false;
}

std::string_view PassNodeName(std::string_view pass)
{
    for (auto const& [tag, name] : PassNodes()) {
        if (tag == pass) return name;
    }
    return {};
}

namespace {

// A pass's nodes of a material: the two constant blocks of its description, a parameter's node for the pass, and
// the several of a virtual texture's. With `placed` those that have a Vulkan place, without those that have a
// DirectX register and none.
std::vector<MaterialSlot> PassSlots(LsfDoc const& doc, std::string_view pass, bool placed)
{
    std::vector<MaterialSlot> slots;
    auto passNode = PassNodeName(pass);
    if (passNode.empty()) return slots;
    std::string description = "ShaderDesc_" + std::string(passNode);

    auto slotOf = [&](uint32_t node, std::string name, char kind) {
        auto set = IntAttr(doc, node, "VkDescriptorSet"), binding = IntAttr(doc, node, "VkBindingIndex");
        if (name.empty()) return;
        MaterialSlot slot;
        slot.Name = std::move(name);
        slot.DxVs = (int32_t)IntAttr(doc, node, "DxVsIndex").value_or(-1);
        slot.DxPs = (int32_t)IntAttr(doc, node, "DxPsIndex").value_or(-1);
        slot.Kind = kind;
        bool has = set && binding && *set >= 0 && *binding >= 0;
        if (has != placed || (!placed && slot.DxVs < 0 && slot.DxPs < 0)) return;
        slot.Set = has ? (uint32_t)*set : UINT32_MAX;
        slot.Binding = has ? (uint32_t)*binding : UINT32_MAX;
        slots.push_back(std::move(slot));
    };
    // What sort of thing a parameter is, by the list it is in ("Texture2DParameters", "SamplerStateParameters").
    auto kindOf = [&](int32_t parameter) -> char {
        std::string_view list = doc.nodes[parameter].name;
        return list.find("Sampler") != std::string_view::npos ? 's' : list.find("Texture") != std::string_view::npos ? 't' : 0;
    };

    for (uint32_t i = 0; i < doc.node_count; i++) {
        auto const& node = doc.nodes[i];
        if (node.parent < 0) continue;
        auto const& parent = doc.nodes[node.parent];
        if (description == parent.name) {
            // The pass's two constant blocks.
            if (!strcmp(node.name, "EngineCBBinding")) slotOf(i, "CB_EngineParams", 'b');
            else if (!strcmp(node.name, "MaterialCBSize")) slotOf(i, "CB_MaterialConstants", 'b');
        } else if (passNode == node.name) {
            // A parameter's binding for this pass: the parameter is the parent.
            slotOf(i, TextAttr(doc, (uint32_t)node.parent, "UniformName"), kindOf(node.parent));
        } else if (passNode == parent.name && parent.parent >= 0) {
            // A virtual texture has several textures per pass: its page table and its caches.
            auto name = TextAttr(doc, (uint32_t)parent.parent, "UniformName");
            if (!name.empty()) slotOf(i, name + "_" + (strcmp(node.name, "TranslationTable") ? node.name : "Pagetable"), 't');
        }
    }
    return slots;
}

uint32_t ReadWord(std::span<uint8_t const> bytes, size_t word)
{
    return ReadU32(bytes.data() + word * 4);
}

}

std::vector<MaterialSlot> ReadMaterialSlots(LsfDoc const& doc, std::string_view pass)
{
    return PassSlots(doc, pass, true);
}

std::vector<MaterialSlot> ReadUnplacedSlots(LsfDoc const& doc, std::string_view pass)
{
    return PassSlots(doc, pass, false);
}

bool ReadStageRegisters(std::span<uint8_t const> stage, bool vertex, StageRegisters& registers, std::string& why)
{
    registers = {};
    // As ReadInputSignature: "SHD2", three words of the toolkit's and, in a vertex stage, a hash of eight bytes;
    // then the DXBC container.
    size_t header = 0;
    for (size_t at : { (size_t)10, (size_t)18 }) {
        if (!header && stage.size() >= at + 32 && memcmp(stage.data() + at, "DXBC", 4) == 0) header = at;
    }
    if (!header || memcmp(stage.data(), "SHD2", 4) != 0) {
        why = "the DirectX 11 stage is not a SHD2 chunk";
        return false;
    }
    auto dxbc = stage.subspan(header);
    uint32_t total = ReadU32(dxbc.data() + 24), chunks = ReadU32(dxbc.data() + 28);
    if (total != dxbc.size() || chunks > 64 || 32 + (uint64_t)chunks * 4 > dxbc.size()) {
        why = "the DirectX 11 container is malformed";
        return false;
    }
    for (uint32_t i = 0; i < chunks; i++) {
        uint64_t at = ReadU32(dxbc.data() + 32 + i * 4);
        if (at + 8 > dxbc.size()) break;
        if (memcmp(dxbc.data() + at, "SHEX", 4) != 0 && memcmp(dxbc.data() + at, "SHDR", 4) != 0) continue;
        uint64_t size = ReadU32(dxbc.data() + at + 4);
        if (at + 8 + size > dxbc.size() || size < 8) break;
        // The program: its version and kind, its length in words, then one instruction after another, each
        // with its opcode in the low eleven bits of its first word and its length in words in bits 24 to 30.
        auto program = dxbc.subspan(at + 8, size);
        size_t words = ReadWord(program, 1);
        if (words * 4 > program.size() || words < 2) break;
        constexpr uint32_t kPixel = 0, kVertex = 1, kCustomData = 53, kResource = 88, kSampler = 90;
        if (ReadWord(program, 0) >> 16 != (vertex ? kVertex : kPixel)) {
            why = std::string("the DirectX 11 stage is not a ") + (vertex ? "vertex" : "pixel") + " shader";
            return false;
        }
        for (size_t word = 2; word < words;) {
            uint32_t first = ReadWord(program, word), opcode = first & 0x7ff, length = (first >> 24) & 0x7f;
            if (opcode == kCustomData) length = word + 1 < words ? ReadWord(program, word + 1) : 0;
            if (!length || word + length > words) { why = "the DirectX 11 stage's program is malformed"; return false; }
            if (opcode == kResource || opcode == kSampler) {
                // The opcode (and the words that extend it), the operand (and the words that extend that), and
                // the register as a plain number: one index, said outright, as a shader of model 5.0 has it.
                size_t operand = word + 1;
                for (uint32_t extended = first; (extended >> 31) && operand < word + length; operand++) extended = ReadWord(program, operand);
                size_t index = operand + 1;
                for (uint32_t extended = operand < word + length ? ReadWord(program, operand) : 0; (extended >> 31) && index < word + length; index++) {
                    extended = ReadWord(program, index);
                }
                uint32_t described = operand < word + length ? ReadWord(program, operand) : 0;
                if (index >= word + length || ((described >> 20) & 3) != 1 || ((described >> 22) & 7) != 0) {
                    why = "the DirectX 11 stage declares a texture or a sampler in a way this code does not read";
                    return false;
                }
                uint32_t number = ReadWord(program, index);
                if (opcode == kSampler) {
                    registers.Samplers.push_back(number);
                } else {
                    static constexpr char const* kTypes[] = { "?", "?", "1d", "2d", "2dms", "3d", "cube", "1darray", "2darray", "2darrayms", "cubearray" };
                    uint32_t dimension = (first >> 11) & 0x1f;
                    registers.Textures.push_back({ number, dimension < std::size(kTypes) ? kTypes[dimension] : "?" });
                }
            }
            word += length;
        }
        std::sort(registers.Textures.begin(), registers.Textures.end(), [](auto const& a, auto const& b) { return a.Register < b.Register; });
        std::sort(registers.Samplers.begin(), registers.Samplers.end());
        return true;
    }
    why = "the DirectX 11 container has no program";
    return false;
}

InterfaceTable::OpenFault const* InterfaceTable::FaultOfResource(std::string_view name) const
{
    for (auto const& r : FaultyResources) if (r.Name == name && r.Fault < Faults.size()) return &Faults[r.Fault];
    return nullptr;
}

InterfaceTable::OpenFault const* InterfaceTable::FaultOfVertexStage(std::array<uint8_t, 32> const& stage) const
{
    auto at = std::lower_bound(FaultyVertexStages.begin(), FaultyVertexStages.end(), stage, [](FaultyStage const& s, auto const& hash) { return s.Stage < hash; });
    return at != FaultyVertexStages.end() && at->Stage == stage && at->Fault < Faults.size() ? &Faults[at->Fault] : nullptr;
}

InterfaceTable::OpenFault const* InterfaceTable::FaultOfPair(std::array<uint8_t, 32> const& vertex, std::array<uint8_t, 32> const& fragment) const
{
    for (auto const& pair : FaultyPairs) if (pair.Vertex == vertex && pair.Fragment == fragment && pair.Fault < Faults.size()) return &Faults[pair.Fault];
    return nullptr;
}

InterfaceTable InterfaceTable::WithoutDerived() const
{
    InterfaceTable out = *this;
    for (auto& block : out.Blocks) block.Derived.clear();
    return out;
}

InterfaceTable InterfaceTable::WithoutTextureTable() const
{
    InterfaceTable out = *this;
    if (out.Textures) {
        std::erase_if(out.Slots, [&](EngineSlot const& slot) { return slot.Set == out.Textures->NewSet; });
        out.Textures.reset();
    }
    return out;
}

EngineSlot const* InterfaceTable::Slot(uint32_t set, uint32_t binding) const
{
    for (auto const& slot : Slots) {
        if (slot.Set == set && slot.Binding == binding) return &slot;
    }
    return nullptr;
}

EngineBlock const* InterfaceTable::Block(std::string_view name) const
{
    for (auto const& block : Blocks) {
        if (block.Name == name) return &block;
    }
    return nullptr;
}

AttributeName const* InterfaceTable::Attribute(std::string_view semantic, uint32_t index) const
{
    for (auto const& attribute : Attributes) {
        if (attribute.Index == index && attribute.Semantic == semantic) return &attribute;
    }
    return nullptr;
}

void MacInterface::Add(MacPass const& pass)
{
    passes_++;
    for (auto const* stage : { &pass.Vertex, &pass.Fragment }) {
        for (auto const& r : *stage) {
            auto [it, first] = resources_.try_emplace(std::pair(r.Name, r.Kind));
            auto& seen = it->second;
            if (first) {
                seen.Value = r;
                continue;
            }
            if (seen.Value.Index != r.Index || (r.Kind == ResourceKind::Texture && seen.Value.Type != r.Type)) seen.Fixed = false;
            bool same = seen.Value.Type == r.Type && seen.Value.Size == r.Size && seen.Value.Members.size() == r.Members.size();
            for (size_t m = 0; same && m < r.Members.size(); m++) {
                same = seen.Value.Members[m].Name == r.Members[m].Name && seen.Value.Members[m].Offset == r.Members[m].Offset
                    && seen.Value.Members[m].Type == r.Members[m].Type;
            }
            if (!same) seen.FixedLayout = false;
        }
    }
    for (auto const& a : pass.Attributes) {
        auto [it, first] = attributes_.try_emplace(a.Name);
        auto& seen = it->second;
        if (first) seen.Value = a;
        else if (seen.Value.Index != a.Index) seen.Fixed = false;
        if (!a.Type.empty() && seen.Scalars.find(a.Type[0]) == std::string::npos) seen.Scalars.push_back(a.Type[0]);
    }
}

MacResource const* MacInterface::Resource(std::string_view name, ResourceKind kind) const
{
    auto it = resources_.find(std::pair(std::string(name), kind));
    return it != resources_.end() && it->second.Fixed ? &it->second.Value : nullptr;
}

bool MacInterface::HasFixedLayout(MacResource const& resource) const
{
    auto it = resources_.find(std::pair(resource.Name, resource.Kind));
    return it != resources_.end() && it->second.FixedLayout;
}

MacAttribute const* MacInterface::Attribute(std::string_view name, std::string_view type) const
{
    auto it = attributes_.find(name);
    if (it == attributes_.end() || !it->second.Fixed || type.empty()) return nullptr;
    return it->second.Scalars.find(type[0]) != std::string::npos ? &it->second.Value : nullptr;
}

std::string MacInterface::Serialize() const
{
    std::string out = "P\t" + std::to_string(passes_) + "\n";
    for (auto const& [key, seen] : resources_) {
        auto const& r = seen.Value;
        out += "R\t" + std::to_string((int)r.Kind) + "\t" + r.Name + "\t" + std::to_string(r.Index) + "\t" + r.Type + "\t" + std::to_string(r.Size)
            + "\t" + (seen.Fixed ? "1" : "0") + "\t" + (seen.FixedLayout ? "1" : "0") + "\t" + (r.Depth ? "1" : "0") + "\n";
        for (auto const& m : r.Members) out += "M\t" + m.Name + "\t" + std::to_string(m.Offset) + "\t" + m.Type + "\n";
    }
    for (auto const& [name, seen] : attributes_) {
        out += "A\t" + name + "\t" + std::to_string(seen.Value.Index) + "\t" + seen.Value.Type + "\t" + (seen.Fixed ? "1" : "0") + "\t"
            + seen.Scalars + "\n";
    }
    return out + "E\n";
}

std::optional<MacInterface> MacInterface::Parse(std::string_view text)
{
    MacInterface out;
    Seen* last = nullptr;
    bool ended = false;
    auto number = [](std::string_view field, uint32_t& value) {
        if (field.empty() || field.size() > 9) return false;
        value = 0;
        for (char c : field) {
            if (c < '0' || c > '9') return false;
            value = value * 10 + (uint32_t)(c - '0');
        }
        return true;
    };
    while (!text.empty() && !ended) {
        auto end = text.find('\n');
        if (end == std::string_view::npos) return {};
        auto line = text.substr(0, end);
        text.remove_prefix(end + 1);
        std::vector<std::string_view> f;
        for (size_t at = 0;;) {
            auto tab = line.find('\t', at);
            f.push_back(line.substr(at, tab == std::string_view::npos ? tab : tab - at));
            if (tab == std::string_view::npos) break;
            at = tab + 1;
        }
        uint32_t a = 0, b = 0;
        if (f[0] == "P" && f.size() == 2 && number(f[1], a)) {
            out.passes_ = a;
        } else if (f[0] == "R" && f.size() == 9 && number(f[1], a) && a <= 2 && number(f[3], b)) {
            Seen seen;
            seen.Value.Kind = (ResourceKind)a;
            seen.Value.Name = f[2];
            seen.Value.Index = b;
            seen.Value.Type = f[4];
            if (!number(f[5], seen.Value.Size)) return {};
            seen.Fixed = f[6] == "1";
            seen.FixedLayout = f[7] == "1";
            seen.Value.Depth = f[8] == "1";
            seen.Value.Used = true;
            last = &out.resources_[std::pair(seen.Value.Name, seen.Value.Kind)];
            *last = std::move(seen);
        } else if (f[0] == "M" && f.size() == 4 && last && number(f[2], a)) {
            last->Value.Members.push_back({ std::string(f[1]), a, std::string(f[3]) });
        } else if (f[0] == "A" && f.size() == 6 && number(f[2], a)) {
            auto& seen = out.attributes_[std::string(f[1])];
            seen.Value = { std::string(f[1]), a, std::string(f[3]) };
            seen.Fixed = f[4] == "1";
            seen.Scalars = f[5];
        } else if (f[0] == "E" && f.size() == 1) {
            ended = true;
        } else {
            return {};
        }
    }
    // Cut short or with something after its end, it is not what was written.
    if (!ended || !text.empty() || !out.passes_) return {};
    return out;
}

namespace {

char const* KindText(ResourceKind kind)
{
    return kind == ResourceKind::Buffer ? "buffer" : kind == ResourceKind::Texture ? "texture" : "sampler";
}

std::string SlotText(WindowsResource const& r)
{
    return "set " + std::to_string(r.Set) + ", binding " + std::to_string(r.Binding);
}

// The members of an engine block the shader reads go where the Mac block has
// the member of that name; the others only have to stay out of their way.
bool PlaceBlock(SpirvShader& shader, WindowsResource const& r, EngineBlock const& block, MacResource const& mac, std::string& why)
{
    // The shader's block is the table's, or its beginning: the engine has only
    // ever added members at the end of a block, and a shader built before it
    // did reads the first of what today's game binds.
    if (r.Members.size() > block.Members.size() || block.Types.size() != block.Members.size() || block.Offsets.size() != block.Members.size()) {
        why = mac.Name + " has " + std::to_string(r.Members.size()) + " members in the shader, " + std::to_string(block.Members.size())
            + " in the interface table";
        return false;
    }
    for (size_t i = 0; i < r.Members.size(); i++) {
        if (block.Types[i] != r.Members[i].Type || block.Offsets[i] != r.Members[i].Offset) {
            why = mac.Name + "'s member " + std::to_string(i) + " is " + r.Members[i].Type + " at " + std::to_string(r.Members[i].Offset)
                + " in the shader, " + block.Types[i] + " at " + std::to_string(block.Offsets[i]) + " in the interface table";
            return false;
        }
    }
    if (!r.ReadKnown) {
        why = "which members of " + mac.Name + " the shader reads is not known";
        return false;
    }

    std::vector<BlockMember const*> target(r.Members.size(), nullptr);
    for (size_t i = 0; i < r.Members.size(); i++) {
        if (block.Members[i].empty()) continue;
        for (auto const& member : mac.Members) {
            if (member.Name == block.Members[i] && member.Type == r.Members[i].Type) target[i] = &member;
        }
    }
    // A vector's size is in its type; an array's or a matrix's is the
    // shader's own distance to the next member.
    auto sizeOf = [&](uint32_t i) {
        uint32_t scalars = ScalarCount(r.Members[i].Type);
        bool plain = scalars <= 4 && r.Members[i].Type.find('[') == std::string::npos;
        if (plain || i + 1 == r.Members.size()) return std::max<uint32_t>(4, scalars * 4);
        return r.Members[i + 1].Offset - r.Members[i].Offset;
    };

    // The members worked out from what the Mac block has: each where the
    // thing it is worked out from is, when the Mac block has that thing.
    struct Put { uint32_t Offset; EngineBlock::DerivedMember const* How; };
    std::vector<std::optional<Put>> derived(r.Members.size());
    for (auto const& [member, how] : block.Derived) {
        if (member >= r.Members.size() || target[member]) continue;
        for (auto const& from : mac.Members) {
            if (from.Name != how.From || from.Type != how.FromType) continue;
            if (4 * how.Number + sizeOf(member) > 4 * ScalarCount(from.Type)) continue;
            derived[member] = Put{ from.Offset + 4 * how.Number, &how };
        }
    }
    for (uint32_t read : r.Read) {
        if (read < derived.size() && derived[read]) {
            // What it is worked out with has to be in the shader's block too, under the Mac game's name.
            std::string const& text = derived[read]->How->Read;
            for (size_t open = text.find('{'); open != std::string::npos; open = text.find('{', open + 1)) {
                size_t close = text.find('}', open);
                std::string name = close == std::string::npos ? std::string() : text.substr(open + 1, close - open - 1);
                bool has = false;
                for (size_t i = 0; i < target.size(); i++) has = has || (target[i] && target[i]->Name == name);
                if (!has) {
                    auto absent = block.Absent.find(read);
                    why = "the shader reads " + (absent != block.Absent.end() ? absent->second : "member " + std::to_string(read)) + " of " + mac.Name
                        + ", which is worked out with " + (name.empty() ? "something not named" : name) + ", and the shader's block has no such member";
                    return false;
                }
            }
            continue;
        }
        if (auto other = block.OtherMeaning.find(read); other != block.OtherMeaning.end()) {
            auto absent = block.Absent.find(read);
            why = "the shader reads " + (absent != block.Absent.end() ? absent->second : "member " + std::to_string(read)) + " of " + mac.Name
                + ", and the Mac game's member of that name holds " + other->second;
            return false;
        }
        if (read >= target.size() || !target[read]) {
            // Named when the table knows the member's name.
            auto absent = block.Absent.find(read);
            why = "the shader reads " + (read < block.Members.size() && !block.Members[read].empty() ? block.Members[read]
                    : absent != block.Absent.end() ? absent->second : "member " + std::to_string(read))
                + " of " + mac.Name + ", which has no counterpart of its type in the Mac game's " + mac.Name;
            return false;
        }
    }

    // In order: a member with a counterpart at the counterpart's offset, one
    // without right after the member before it. A member that is worked out
    // goes where the thing it is worked out from is, wherever that is, and
    // the others are placed as if it were not there; in the end no two may
    // lie over each other.
    std::vector<std::pair<uint32_t, uint32_t>> taken;
    uint32_t end = 0;
    for (uint32_t i = 0; i < r.Members.size(); i++) {
        uint32_t offset;
        if (derived[i]) {
            auto const& how = *derived[i]->How;
            shader.DeriveMember(r, i, derived[i]->Offset, "bg3mse_from_" + how.From + (how.Number ? "_" + std::to_string(how.Number) : ""), how.Read,
                std::find(r.Read.begin(), r.Read.end(), i) != r.Read.end());
            taken.push_back({ derived[i]->Offset, derived[i]->Offset + sizeOf(i) });
            continue;
        }
        if (target[i]) {
            offset = target[i]->Offset;
            if (offset < end) {
                why = mac.Name + "'s members are in another order in the Mac game";
                return false;
            }
        } else {
            offset = (end + 3) & ~3u;
            // It must end before the next member that has a place.
            for (uint32_t next = i + 1; next < r.Members.size(); next++) {
                if (!target[next]) continue;
                if (offset + sizeOf(i) > target[next]->Offset) {
                    why = mac.Name + " has no room for a member the Mac game lacks";
                    return false;
                }
                break;
            }
        }
        shader.MoveMember(r, i, offset, target[i] ? target[i]->Name : std::string());
        end = offset + sizeOf(i);
        taken.push_back({ offset, end });
    }
    std::sort(taken.begin(), taken.end());
    for (size_t i = 1; i < taken.size(); i++) {
        if (taken[i].first < taken[i - 1].second) {
            why = mac.Name + " has no room for a member that is worked out from the Mac game's";
            return false;
        }
    }
    return true;
}

}

bool PlaceShader(SpirvShader& shader, std::span<InputSemantic const> inputs, std::span<MaterialSlot const> material,
    InterfaceTable const& table, MacInterface const& mac, std::vector<PlacedSlot>& placed, std::string& why,
    std::vector<std::string>* byOrderOnly, PassRegisters const* registers)
{
    bool vertex = shader.IsVertex();
    auto declaredAt = [&](WindowsResource const& r) {
        return std::find_if(material.begin(), material.end(), [&](auto const& s) { return s.Set == r.Set && s.Binding == r.Binding; });
    };
    // A thing of the material's own that its .lsf gives no Vulkan place: a texture the shader samples through a
    // second sampler (the toolkit makes two textures of it and writes one parameter), or a whole pass written
    // with -1 for every Vulkan place. Where it is, the pass's DirectX 11 stage says, and the Mac game's own
    // function for such a pass takes it there: every material of the game has its own things in this set, and
    // those of one kind, in the order of their bindings, at the DirectX stage's lowest registers in order.
    constexpr uint32_t kMaterialSet = 1;
    auto byOrder = [&](WindowsResource const& r, uint32_t& index, std::string& name) {
        std::string what = std::string(KindText(r.Kind)) + " at " + SlotText(r);
        if (!registers || r.Set != kMaterialSet) { why = "nothing is known to be at " + SlotText(r); return false; }
        char kind = r.Kind == ResourceKind::Buffer ? 'b' : r.Kind == ResourceKind::Texture ? 't' : 's';
        int32_t MaterialSlot::* dx = vertex ? &MaterialSlot::DxVs : &MaterialSlot::DxPs;
        std::vector<WindowsResource const*> mine;
        for (auto const& other : shader.Resources()) {
            if (other.Set != r.Set || other.Kind != r.Kind) continue;
            // Of the blocks only those the .lsf gives no place: they are paired with its nodes, not with registers.
            if (kind == 'b' && (other.Type != "block" || declaredAt(other) != material.end())) continue;
            if (other.Type.ends_with("[]")) { why = "the material's .lsf gives no place for the " + what + ", and the stage has several textures at one place beside it"; return false; }
            mine.push_back(&other);
        }
        std::sort(mine.begin(), mine.end(), [](auto a, auto b) { return a->Binding < b->Binding; });
        size_t rank = (size_t)(std::find(mine.begin(), mine.end(), &r) - mine.begin());
        if (rank >= mine.size()) { why = "nothing is known to be at " + SlotText(r); return false; }

        if (kind == 'b') {
            // The .lsf's nodes of the pass's blocks that this stage takes, by register.
            std::vector<MaterialSlot const*> nodes;
            for (auto const& slot : registers->Unplaced) if (slot.Kind == 'b' && slot.*dx >= 0) nodes.push_back(&slot);
            std::sort(nodes.begin(), nodes.end(), [&](auto a, auto b) { return a->*dx < b->*dx; });
            if (nodes.size() != mine.size()) {
                why = "the material's .lsf gives no place for the " + what + ": of the material's blocks the stage has " + std::to_string(mine.size())
                    + " without one, and the .lsf names " + std::to_string(nodes.size()) + " for it that have a register and no place";
                return false;
            }
            name = nodes[rank]->Name;
            return true;
        }

        if (!registers->DirectX) {
            why = "the material's .lsf gives no place for the " + what + ", and the pass's DirectX 11 stage, which would say it, cannot be read"
                + (registers->Unread.empty() ? std::string() : " (" + std::string(registers->Unread) + ")");
            return false;
        }
        size_t count = kind == 't' ? registers->DirectX->Textures.size() : registers->DirectX->Samplers.size();
        auto at = [&](size_t i) { return kind == 't' ? registers->DirectX->Textures[i].Register : registers->DirectX->Samplers[i]; };
        if (count < mine.size()) {
            why = "the material's .lsf gives no place for the " + what + ": the stage has " + std::to_string(mine.size())
                + " of the material's and the pass's DirectX 11 stage declares " + std::to_string(count) + " in all";
            return false;
        }
        // The material's own are the stage's first registers, from 0 on without a gap, in every stage of the
        // game's (16,110 with textures, 7,017 with samplers). A stage that has them otherwise did not keep the
        // things the Vulkan stage kept, and which register is whose is then not known.
        for (size_t i = 0; i < mine.size(); i++) {
            if (at(i) != i) {
                why = "the material's .lsf gives no place for the " + what + ", and the pass's DirectX 11 stage does not have the material's "
                    + std::to_string(mine.size()) + " at its first registers (it has none at " + std::to_string(i) + ")";
                return false;
            }
        }
        // The rule has to give those the .lsf does place the registers the .lsf gives them.
        for (size_t i = 0; i < mine.size(); i++) {
            auto slot = declaredAt(*mine[i]);
            if (slot != material.end() && (*slot).*dx >= 0 && (uint32_t)((*slot).*dx) != at(i)) {
                why = "the material's .lsf gives no place for the " + what + ", and the pass's DirectX 11 stage has " + slot->Name
                    + " at another register than the order of the Vulkan bindings gives it";
                return false;
            }
        }
        index = at(rank);
        if (kind == 't' && registers->DirectX->Textures[rank].Type != r.Type) {
            why = "the " + what + " (" + r.Type + ") is " + registers->DirectX->Textures[rank].Type + " at register " + std::to_string(index)
                + " of the pass's DirectX 11 stage, where the order of the Vulkan bindings puts it";
            return false;
        }
        // Named as the .lsf names what it has at that register for this pass, when it names one thing there; a
        // texture it does not name at all has no name but its place.
        MaterialSlot const* named = nullptr;
        uint32_t candidates = 0;
        for (auto const& slot : registers->Unplaced) {
            if (slot.Kind == kind && slot.*dx == (int32_t)index) { named = &slot; candidates++; }
        }
        name = candidates == 1 ? named->Name : std::string(kind == 't' ? "bg3mse_texture_at_" : "bg3mse_sampler_at_") + std::to_string(index);
        return true;
    };
    // Two things of one kind cannot share an index.
    std::map<std::pair<ResourceKind, uint32_t>, std::string> taken;
    auto take = [&](ResourceKind kind, uint32_t index, std::string const& name) {
        auto [it, first] = taken.try_emplace({ kind, index }, name);
        if (!first) why = name + " and " + it->second + " would share index " + std::to_string(index);
        return first;
    };
    for (auto const& r : shader.Resources()) {
        // Several of a thing at one place: only textures of the engine's, where the Mac game has as many.
        bool several = r.Type.ends_with("[]");
        auto declared = std::find_if(material.begin(), material.end(), [&](auto const& s) { return s.Set == r.Set && s.Binding == r.Binding; });
        if (r.Type.find('?') != std::string::npos || (several && (r.Kind != ResourceKind::Texture || declared != material.end()))) {
            why = "the resource at " + SlotText(r) + " is of a type this code does not place (" + r.Type + ")";
            return false;
        }

        // What the material declares.
        bool engines = false;
        if (declared != material.end()) {
            uint32_t index = 0;
            if (r.Kind == ResourceKind::Buffer) {
                // Its own two blocks keep their layout; the Mac game has one place for each.
                auto block = mac.Resource(declared->Name, ResourceKind::Buffer);
                if (!block) {
                    why = "the Mac game has no fixed place for " + declared->Name;
                    return false;
                }
                index = block->Index;
            } else {
                // A texture or a sampler is where its DirectX register says.
                int32_t dx = vertex ? declared->DxVs : declared->DxPs;
                // A material may name one of the engine's own samplers and give it no register for a stage
                // that takes it all the same: the stage then has the engine's, where the engine keeps it.
                auto slot = dx < 0 ? table.Slot(r.Set, r.Binding) : nullptr;
                engines = slot && slot->Kind == r.Kind && slot->Name == declared->Name;
                if (dx < 0 && !engines) {
                    why = declared->Name + " has no DirectX register for this stage";
                    return false;
                }
                index = (uint32_t)std::max(dx, 0);
            }
            if (!engines) {
                if (!take(r.Kind, index, declared->Name)) return false;
                shader.Bind(r, index, declared->Name);
                placed.push_back({ declared->Name, vertex, index });
                continue;
            }
        }

        // What the engine supplies.
        auto slot = table.Slot(r.Set, r.Binding);
        if (!slot) {
            // Or a thing of the material's that its .lsf does not place.
            uint32_t index = 0;
            std::string name;
            if (!byOrder(r, index, name)) return false;
            if (r.Kind == ResourceKind::Buffer) {
                auto block = mac.Resource(name, ResourceKind::Buffer);
                if (!block) {
                    why = "the Mac game has no fixed place for " + name;
                    return false;
                }
                index = block->Index;
            }
            if (!take(r.Kind, index, name)) return false;
            shader.Bind(r, index, name);
            placed.push_back({ name, vertex, index });
            continue;
        }
        if (slot->Inferred && byOrderOnly) byOrderOnly->push_back(slot->Name);
        if (auto fault = table.FaultOfResource(slot->Name)) {
            why = "it takes " + slot->Name + ": " + InterfaceTable::Said(*fault);
            return false;
        }
        if (slot->Kind != r.Kind) {
            // Not a difference in detail: the shader was built for an engine
            // that kept something else at this place, and what it expects
            // there (and where it expects what is here) is not known.
            why = "the shader has a " + std::string(KindText(r.Kind)) + " at " + SlotText(r) + ", where the game has a "
                + KindText(slot->Kind) + " (" + slot->Name + "): it was built for another version of the game";
            return false;
        }
        if (slot->Type != r.Type) {
            why = slot->Name + " at " + SlotText(r) + " is not of the type the interface table has (" + r.Type + ", not " + slot->Type + ")";
            return false;
        }
        auto resource = mac.Resource(slot->Name, r.Kind);
        if (!resource) {
            why = "the Mac game has no fixed place for " + slot->Name;
            return false;
        }
        if (r.Kind == ResourceKind::Texture && resource->Type != r.Type) {
            why = slot->Name + " is " + resource->Type + " in the Mac game, " + r.Type + " in the shader";
            return false;
        }
        if (several && (r.Size < 2 || resource->Size != r.Size)) {
            why = slot->Name + " is " + std::to_string(resource->Size) + " textures in the Mac game, " + std::to_string(r.Size) + " in the shader";
            return false;
        }
        if (r.Kind == ResourceKind::Buffer) {
            if (r.Type == "block") {
                auto block = table.Block(slot->Name);
                if (!block || resource->Type != "block" || !mac.HasFixedLayout(*resource)) {
                    why = "the layout of " + slot->Name + " cannot be compared with the Mac game's";
                    return false;
                }
                if (!PlaceBlock(shader, r, *block, *resource, why)) return false;
            } else if (resource->Type == "block") {
                // An array of structures: the Mac function's is the same structure, an element as large, or
                // it is refused. The shader's may end in members the Mac's does not list (what fills an
                // element up to its size is members in the one and not in the other) when the shader is known
                // to read none of them.
                bool same = mac.HasFixedLayout(*resource) && resource->Members.size() <= r.Members.size() && resource->Size == r.Size;
                for (size_t m = 0; same && m < resource->Members.size(); m++) {
                    same = resource->Members[m].Offset == r.Members[m].Offset && resource->Members[m].Type == r.Members[m].Type;
                }
                if (same && resource->Members.size() < r.Members.size()) {
                    same = r.ReadKnown;
                    for (uint32_t read : r.Read) same = same && read < resource->Members.size();
                }
                if (!same) {
                    why = slot->Name + "'s elements are laid out differently in the Mac game";
                    return false;
                }
            } else if (r.Members.size() != 1 || r.Members[0].Type.find('s') == 0) {
                // The Mac function takes it as bytes. An array of words is the
                // same on both sides; an array of structures cannot be compared.
                why = "the layout of " + slot->Name + "'s elements cannot be compared with the Mac game's";
                return false;
            }
        }
        if (!take(r.Kind, resource->Index, slot->Name)) return false;
        shader.Bind(r, resource->Index, slot->Name);
    }

    for (auto const& input : shader.Inputs()) {
        if (input.Location >= inputs.size()) {
            why = "the vertex input at location " + std::to_string(input.Location) + " is not in the DirectX input signature";
            return false;
        }
        auto const& semantic = inputs[input.Location];
        auto name = table.Attribute(semantic.Name, semantic.Index);
        if (name && name->Inferred && byOrderOnly) byOrderOnly->push_back(name->Name);
        auto attribute = name ? mac.Attribute(name->Name, input.Type) : nullptr;
        if (!attribute) {
            why = "the vertex input " + semantic.Name + std::to_string(semantic.Index) + " (" + input.Type + ") has no Mac attribute of its kind";
            return false;
        }
        shader.PlaceInput(input, attribute->Index, attribute->Name);
    }
    return true;
}

}
