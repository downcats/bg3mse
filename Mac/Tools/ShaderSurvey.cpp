#include <stdafx.h>
#include <Shaders/GameInterface.h>
#include <Shaders/PakFile.h>
#include <Shaders/ShaderMapping.h>
#include <Shaders/SpirvIdentity.h>
#include <Shaders/Translation.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <tuple>

// bg3mse-shaders interface / verify: every vanilla pass that both platforms
// ship, read from a Windows Materials.pak and from the Mac game.
//
//   interface   derives the interface table (Mac/Shaders/InterfaceData.cpp) from them
//   verify      translates every Windows pass with the table this build has and
//               compares the result with the function the Mac game ships for it:
//               what each takes and where, and with --draw what each draws
//               (BG3MSE_SHADERS_SHOW=1 prints the source and the pixels of each pass drawn)

namespace bg3se::mac {

namespace {

constexpr std::string_view kMetalExt = "_Metal.bshd";
constexpr std::string_view kPassTags[] = { "FOR", "DEF", "DEP", "DEPS", "DEPST", "VEL", "EMI", "BAKE" };

struct Pass
{
    std::string Stem;               // Public/.../Material_ST_FOR
    std::string Tag;                // ST_FOR
    std::string Kind;               // FOR
    std::string VertexName, FragmentName;
    std::vector<uint8_t> Vulkan;    // the Windows container
    ContainerParts Parts;
    std::vector<InputSemantic> Inputs;
    std::vector<MaterialSlot> Material;
    // For what the material's .lsf gives no Vulkan place (PlaceShader): what it names for the pass without one,
    // and what the pass's DirectX 11 stages declare, vertex then fragment, when they can be read.
    std::vector<MaterialSlot> Unplaced;
    std::optional<StageRegisters> Declares[2];
    PassRegisters Registers(int stage) const { return { Declares[stage] ? &*Declares[stage] : nullptr, Unplaced, {} }; }
    MacPass Mac;
    // Where the material's two constant blocks have each value, on each platform.
    std::map<std::string, int64_t> WindowsOffsets, MacOffsets;
    std::map<std::string, std::vector<float>> MacValues;        // what the material sets each of its parameters to
    // What the .lsf that would be served for the material says for this pass, when the pass is drawn as served:
    // where each stage takes the material's blocks, textures and samplers, and where the blocks have each value.
    bool Served{ false };
    std::vector<MaterialSlot> ServedSlots;
    std::map<std::string, int64_t> ServedOffsets;
    std::string Problem;            // why it cannot be surveyed
};

// Where a material puts each parameter and engine value in its two constant blocks, for one pass.
std::map<std::string, int64_t> BlockOffsets(LsfDoc const& doc, std::string_view passNode)
{
    std::map<std::string, int64_t> offsets;
    std::string description = "ShaderDesc_" + std::string(passNode), pass(passNode);
    for (uint32_t i = 0; i < doc.node_count; i++) {
        auto const& node = doc.nodes[i];
        bool inDescription = description == node.name || (node.parent >= 0 && description == doc.nodes[node.parent].name);
        if (!strcmp(node.name, "EngineCBBinding") || !strcmp(node.name, "MaterialCBSize")) continue;
        char uniform[256] = "";
        auto name = lsf_attr(&doc, i, "UniformName");
        if (name) lsf_attr_text(name, uniform, sizeof(uniform));
        for (uint32_t a = 0; a < node.attr_count; a++) {
            auto const& attr = doc.attrs[node.attr_begin + a];
            int64_t value = 0;
            if (!lsf_attr_int(&attr, &value)) continue;
            if (inDescription) offsets[std::string(node.name) + "." + attr.name] = value;
            else if (uniform[0] && pass == attr.name) offsets[uniform] = value;
        }
    }
    return offsets;
}

// The numbers a material gives its own parameters, by the name its shaders know each by.
std::map<std::string, std::vector<float>> ParameterValues(LsfDoc const& doc)
{
    std::map<std::string, std::vector<float>> values;
    for (uint32_t i = 0; i < doc.node_count; i++) {
        auto name = lsf_attr(&doc, i, "UniformName");
        auto value = lsf_attr(&doc, i, "Value");
        char uniform[256] = "";
        if (!name || !value || !lsf_attr_text(name, uniform, sizeof(uniform)) || !uniform[0]) continue;
        // A number, or two to four of them.
        bool numbers = value->type == LSF_T_FLOAT || (value->type >= 11 && value->type <= 13);
        if (!numbers || value->size % 4 || value->size > 16) continue;
        std::vector<float> floats(value->size / 4);
        memcpy(floats.data(), value->value, value->size);
        values[uniform] = std::move(floats);
    }
    return values;
}

struct ParsedMaterial
{
    LsfDoc Doc{};
    bool Ok{ false };
    std::vector<uint8_t> Bytes;
    ~ParsedMaterial() { if (Ok) lsf_free(&Doc); }
};

class Survey
{
public:
    // `reference`, when given, is a package of Metal stubs and Mac materials
    // for the Windows package's paths, in place of the game's own.
    bool Open(std::string const& windowsPak, std::string const& app, std::string const& reference = {})
    {
        std::string error;
        windows_ = PakFile::Open(windowsPak, &error);
        if (!windows_) { fprintf(stderr, "cannot read %s: %s\n", windowsPak.c_str(), error.c_str()); return false; }
        mac_ = PakFile::Open(reference.empty() ? app + "/Contents/Data/Materials.pak" : reference, &error);
        if (!mac_) { fprintf(stderr, "cannot read the package of Metal stubs: %s\n", error.c_str()); return false; }
        game_ = MetalFunctions::OpenGame(app, error);
        if (!game_) { fprintf(stderr, "%s\n", error.c_str()); return false; }
        for (auto const& entry : mac_->Entries()) {
            if (std::string_view(entry.Name).ends_with(kMetalExt)) stubs_.push_back(&entry);
        }
        std::sort(stubs_.begin(), stubs_.end(), [](auto a, auto b) { return a->Name < b->Name; });
        return true;
    }

    size_t Count() const { return stubs_.size(); }
    MetalFunctions const& Game() const { return *game_; }
    // The Windows package as the translator reads a mod's.
    MaterialSource WindowsSource() const
    {
        MaterialSource source;
        source.Label = windows_->FileName();
        for (auto const& entry : windows_->Entries()) source.Names.insert(entry.Name);
        source.Read = [this](std::string const& name) -> std::optional<std::vector<uint8_t>> {
            auto entry = windows_->Find(name);
            return entry ? windows_->Read(*entry) : std::nullopt;
        };
        return source;
    }

    // The pass of stub `index`, read from both sides.
    Pass Load(size_t index)
    {
        Pass pass;
        auto const& entry = *stubs_[index];
        pass.Stem = entry.Name.substr(0, entry.Name.size() - kMetalExt.size());
        for (auto tag : kPassTags) {
            for (auto prefix : { "_ST_", "_STI_" }) {
                std::string suffix = std::string(prefix) + std::string(tag);
                if (pass.Stem.ends_with(suffix)) {
                    pass.Tag = suffix.substr(1);
                    pass.Kind = tag;
                }
            }
        }
        if (pass.Tag.empty()) { pass.Problem = "not a pass this code knows"; return pass; }

        auto stub = mac_->Read(entry);
        ContainerParts parts;
        if (!stub || !ParseContainer(*stub, parts) || parts.Version != 2) { pass.Problem = "the Mac stub is not a version 2 container"; return pass; }
        auto name = [](std::span<uint8_t const> blob) {
            std::string s((char const*)blob.data(), blob.size());
            while (!s.empty() && (s.back() == '\n' || s.back() == 0)) s.pop_back();
            return s;
        };
        pass.VertexName = name(parts.Stages[0]);
        pass.FragmentName = name(parts.Stages[4]);

        auto read = [&](std::string const& path) -> std::optional<std::vector<uint8_t>> {
            auto e = windows_->Find(path);
            return e ? windows_->Read(*e) : std::nullopt;
        };
        auto vulkan = read(pass.Stem + "_Vulkan.bshd");
        if (!vulkan) { pass.Problem = "Windows has no Vulkan container for it"; return pass; }
        pass.Vulkan = std::move(*vulkan);
        if (!ParseContainer(pass.Vulkan, pass.Parts)) { pass.Problem = "the Vulkan container does not parse"; return pass; }
        if (VulkanTableVersion(pass.Parts) != kVulkanTableVersion) { pass.Problem = "the Vulkan container is not of the version translation takes"; return pass; }

        auto dx11 = read(pass.Stem + "_DX11.bshd");
        ContainerParts dxParts;
        std::string why;
        if (!dx11 || !ParseContainer(*dx11, dxParts) || !ReadInputSignature(dxParts.Stages[0], pass.Inputs, why)) {
            pass.Problem = "no DirectX 11 input signature: " + why;
            return pass;
        }
        for (int s = 0; s < 2; s++) {
            StageRegisters declares;
            if (ReadStageRegisters(dxParts.Stages[s ? 4 : 0], s == 0, declares, why)) pass.Declares[s] = std::move(declares);
        }

        std::string materialPath = pass.Stem.substr(0, pass.Stem.size() - pass.Tag.size() - 1) + ".lsf";
        auto& material = materials_[materialPath];
        if (!material) {
            material = std::make_unique<ParsedMaterial>();
            if (auto bytes = read(materialPath)) {
                material->Bytes = std::move(*bytes);
                char err[256] = "";
                material->Ok = lsf_parse(material->Bytes.data(), material->Bytes.size(), &material->Doc, err, sizeof(err));
            }
        }
        if (!material->Ok) { pass.Problem = "the Windows material does not parse"; return pass; }
        pass.Material = ReadMaterialSlots(material->Doc, pass.Tag);
        pass.Unplaced = ReadUnplacedSlots(material->Doc, pass.Tag);

        auto& macMaterial = macMaterials_[materialPath];
        if (!macMaterial) {
            macMaterial = std::make_unique<ParsedMaterial>();
            auto e = mac_->Find(materialPath);
            if (auto bytes = e ? mac_->Read(*e) : std::nullopt) {
                macMaterial->Bytes = std::move(*bytes);
                char err[256] = "";
                macMaterial->Ok = lsf_parse(macMaterial->Bytes.data(), macMaterial->Bytes.size(), &macMaterial->Doc, err, sizeof(err));
            }
        }
        auto node = PassNodeName(pass.Tag);
        pass.WindowsOffsets = BlockOffsets(material->Doc, node);
        if (macMaterial->Ok) pass.MacOffsets = BlockOffsets(macMaterial->Doc, node);
        if (macMaterial->Ok) pass.MacValues = ParameterValues(macMaterial->Doc);

        if (!game_->Reflect(pass.VertexName, pass.FragmentName, pass.Mac, why)) pass.Problem = "the Mac functions: " + why;
        return pass;
    }

    // The DirectX 12 fragment stage of a pass, for the names in its bitcode.
    std::optional<std::vector<uint8_t>> Dx12Fragment(std::string const& stem)
    {
        auto e = windows_->Find(stem + "_DX12.bshd");
        auto data = e ? windows_->Read(*e) : std::nullopt;
        ContainerParts parts;
        if (!data || !ParseContainer(*data, parts)) return {};
        return std::vector<uint8_t>(parts.Stages[4].begin(), parts.Stages[4].end());
    }

private:
    std::unique_ptr<PakFile> windows_, mac_;
    std::unique_ptr<MetalFunctions> game_;
    std::vector<PakFile::Entry const*> stubs_;
    std::map<std::string, std::unique_ptr<ParsedMaterial>> materials_, macMaterials_;
};

// The stage's first module (of its two, the one every driver takes), or nothing.
std::span<uint8_t const> ModuleOf(std::span<uint8_t const> stage)
{
    auto modules = VulkanStageModules(stage);
    return modules.empty() ? std::span<uint8_t const>{} : modules[0];
}

bool IsMaterialSlot(Pass const& pass, WindowsResource const& r)
{
    return std::any_of(pass.Material.begin(), pass.Material.end(), [&](auto const& s) { return s.Set == r.Set && s.Binding == r.Binding; });
}

bool IsMaterialName(Pass const& pass, std::string const& name)
{
    return std::any_of(pass.Material.begin(), pass.Material.end(), [&](auto const& s) { return s.Name == name; });
}

// --- the strings of a DXIL module's metadata, in the order they are stored ---------

class BitReader
{
public:
    BitReader(uint8_t const* data, size_t size) : data_(data), bits_(size * 8) {}

    bool Failed() const { return failed_; }
    bool AtEnd() const { return failed_ || at_ >= bits_; }
    uint64_t Read(unsigned n)
    {
        uint64_t value = 0;
        if (n > 64 || at_ + n > bits_) { failed_ = true; return 0; }
        for (unsigned i = 0; i < n; i++, at_++) value |= (uint64_t)((data_[at_ >> 3] >> (at_ & 7)) & 1) << i;
        return value;
    }
    uint64_t Vbr(unsigned n)
    {
        uint64_t value = 0, high = 1ull << (n - 1);
        for (unsigned shift = 0; shift < 64; shift += n - 1) {
            uint64_t piece = Read(n);
            value |= (piece & (high - 1)) << shift;
            if (!(piece & high) || failed_) return value;
        }
        failed_ = true;
        return 0;
    }
    void Align32() { at_ = (at_ + 31) & ~(size_t)31; }
    void Skip(uint64_t bits) { if (at_ + bits > bits_) failed_ = true; else at_ += bits; }

private:
    uint8_t const* data_;
    size_t bits_, at_{ 0 };
    bool failed_{ false };
};

struct AbbrevOp { enum Kind { Literal, Fixed, Vbr, Array, Char6, Blob } K; uint64_t Value; };
using Abbrev = std::vector<AbbrevOp>;

uint32_t U32At(uint8_t const* p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

class MetadataStrings
{
public:
    // Every METADATA_STRING of the module, in order. Empty when the bitcode cannot be read.
    static std::vector<std::string> Read(std::span<uint8_t const> bitcode)
    {
        // A module may come wrapped: a header that says where in the file it is.
        if (bitcode.size() >= 20 && U32At(bitcode.data()) == 0x0b17c0de) {
            uint32_t offset = U32At(bitcode.data() + 8), size = U32At(bitcode.data() + 12);
            if ((uint64_t)offset + size > bitcode.size()) return {};
            bitcode = bitcode.subspan(offset, size);
        }
        MetadataStrings reader(bitcode);
        if (bitcode.size() < 8 || reader.in_.Read(32) != 0xdec04342) return {};
        reader.Block(UINT32_MAX, 2);
        return reader.in_.Failed() ? std::vector<std::string>{} : std::move(reader.strings_);
    }

private:
    explicit MetadataStrings(std::span<uint8_t const> bitcode) : in_(bitcode.data(), bitcode.size()) {}

    static constexpr uint32_t kBlockInfo = 0, kModule = 8, kMetadata = 15;

    Abbrev DefineAbbrev()
    {
        Abbrev abbrev;
        uint64_t count = in_.Vbr(5);
        for (uint64_t i = 0; i < count && !in_.Failed(); i++) {
            if (in_.Read(1)) { abbrev.push_back({ AbbrevOp::Literal, in_.Vbr(8) }); continue; }
            switch (in_.Read(3)) {
            case 1: abbrev.push_back({ AbbrevOp::Fixed, in_.Vbr(5) }); break;
            case 2: abbrev.push_back({ AbbrevOp::Vbr, in_.Vbr(5) }); break;
            case 3: abbrev.push_back({ AbbrevOp::Array, 0 }); break;
            case 4: abbrev.push_back({ AbbrevOp::Char6, 0 }); break;
            case 5: abbrev.push_back({ AbbrevOp::Blob, 0 }); break;
            default: in_.Skip(UINT64_MAX); break;
            }
        }
        return abbrev;
    }

    uint64_t Scalar(AbbrevOp const& op)
    {
        switch (op.K) {
        case AbbrevOp::Literal: return op.Value;
        case AbbrevOp::Fixed: return op.Value ? in_.Read((unsigned)op.Value) : 0;
        case AbbrevOp::Vbr: return op.Value ? in_.Vbr((unsigned)op.Value) : 0;
        case AbbrevOp::Char6: return (uint64_t)"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._"[in_.Read(6)];
        default: in_.Skip(UINT64_MAX); return 0;
        }
    }

    std::vector<uint64_t> Record(Abbrev const& abbrev)
    {
        std::vector<uint64_t> values;
        for (size_t i = 0; i < abbrev.size() && !in_.Failed(); i++) {
            auto const& op = abbrev[i];
            if (op.K == AbbrevOp::Array) {
                if (i + 1 >= abbrev.size()) { in_.Skip(UINT64_MAX); break; }
                uint64_t count = in_.Vbr(6);
                for (uint64_t n = 0; n < count && !in_.Failed(); n++) values.push_back(Scalar(abbrev[i + 1]));
                i++;
            } else if (op.K == AbbrevOp::Blob) {
                uint64_t count = in_.Vbr(6);
                in_.Align32();
                for (uint64_t n = 0; n < count && !in_.Failed(); n++) values.push_back(in_.Read(8));
                in_.Align32();
            } else {
                values.push_back(Scalar(op));
            }
        }
        return values;
    }

    void BlockInfo(unsigned width)
    {
        uint32_t current = UINT32_MAX;
        while (!in_.AtEnd()) {
            uint64_t id = in_.Read(width);
            if (id == 0) { in_.Align32(); return; }
            if (id == 1) { in_.Skip(UINT64_MAX); return; }      // no block inside the block info
            if (id == 2) { blockInfo_[current].push_back(DefineAbbrev()); continue; }
            if (id == 3) {
                uint64_t code = in_.Vbr(6), count = in_.Vbr(6);
                std::vector<uint64_t> ops;
                for (uint64_t i = 0; i < count && !in_.Failed(); i++) ops.push_back(in_.Vbr(6));
                if (code == 1 && !ops.empty()) current = (uint32_t)ops[0];      // SETBID
                continue;
            }
            in_.Skip(UINT64_MAX);       // an abbreviated record here would need abbreviations of the block info itself
        }
    }

    void Block(uint32_t blockId, unsigned width)
    {
        std::vector<Abbrev> abbrevs = blockInfo_[blockId];
        while (!in_.AtEnd()) {
            uint64_t id = in_.Read(width);
            if (id == 0) { in_.Align32(); return; }
            if (id == 1) {
                uint32_t inner = (uint32_t)in_.Vbr(8);
                unsigned innerWidth = (unsigned)in_.Vbr(4);
                in_.Align32();
                uint64_t words = in_.Read(32);
                if (inner == kBlockInfo) BlockInfo(innerWidth);
                else if (inner == kModule || (inner == kMetadata && blockId == kModule)) Block(inner, innerWidth);
                else in_.Skip(words * 32);
                continue;
            }
            if (id == 2) { abbrevs.push_back(DefineAbbrev()); continue; }
            uint64_t code;
            std::vector<uint64_t> ops;
            if (id == 3) {
                code = in_.Vbr(6);
                uint64_t count = in_.Vbr(6);
                for (uint64_t i = 0; i < count && !in_.Failed(); i++) ops.push_back(in_.Vbr(6));
            } else {
                if (id - 4 >= abbrevs.size()) { in_.Skip(UINT64_MAX); return; }
                ops = Record(abbrevs[id - 4]);
                if (ops.empty()) { in_.Skip(UINT64_MAX); return; }
                code = ops[0];
                ops.erase(ops.begin());
            }
            if (blockId == kMetadata && code == 1) {        // METADATA_STRING
                std::string s;
                for (uint64_t c : ops) s.push_back((char)c);
                strings_.push_back(std::move(s));
            }
            if (blockId == kMetadata && code == 35 && ops.size() >= 2) {
                // METADATA_STRINGS: how many, where the characters begin in the blob, and the blob: every
                // string's length as a number of six-bit pieces, then all the characters.
                uint64_t count = ops[0], chars = ops[1];
                std::vector<uint8_t> blob;
                for (size_t i = 2; i < ops.size(); i++) blob.push_back((uint8_t)ops[i]);
                if (chars > blob.size()) { in_.Skip(UINT64_MAX); return; }
                BitReader lengths(blob.data(), (size_t)chars);
                size_t at = (size_t)chars;
                for (uint64_t n = 0; n < count; n++) {
                    uint64_t length = lengths.Vbr(6);
                    if (lengths.Failed() || at + length > blob.size()) { in_.Skip(UINT64_MAX); return; }
                    strings_.emplace_back((char const*)blob.data() + at, (size_t)length);
                    at += (size_t)length;
                }
            }
        }
    }

    BitReader in_;
    std::map<uint32_t, std::vector<Abbrev>> blockInfo_;
    std::vector<std::string> strings_;
};

uint32_t U32(uint8_t const* p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

// One of the Mac game's Metal libraries as a file: for what Metal's own reflection does not tell of a function,
// which is what a vertex function hands the fragment function, by name and type.
class MetalLibraryFile
{
public:
    bool Open(std::string const& path)
    {
        int fd = open(path.c_str(), O_RDONLY);
        struct stat st{};
        if (fd < 0 || fstat(fd, &st) != 0) { if (fd >= 0) close(fd); return false; }
        void* map = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (map == MAP_FAILED) return false;
        data_ = { (uint8_t const*)map, (size_t)st.st_size };
        if (data_.size() < 88 || memcmp(data_.data(), "MTLB", 4) != 0) return false;
        uint64_t list = U64(24), bitcode = U64(72);
        if (list + 4 > data_.size()) return false;
        uint32_t count = U32At(data_.data() + list);
        uint64_t at = list + 4;
        for (uint32_t i = 0; i < count && at + 4 <= data_.size(); i++) {
            uint32_t size = U32At(data_.data() + at);
            uint64_t tag = at + 4, end = at + size;
            if (size < 4 || end > data_.size()) return false;
            std::string name;
            uint64_t offset = 0, length = 0;
            while (tag + 6 <= end && memcmp(data_.data() + tag, "ENDT", 4) != 0) {
                uint16_t n;
                memcpy(&n, data_.data() + tag + 4, 2);
                uint8_t const* body = data_.data() + tag + 6;
                if (tag + 6 + n > end) return false;
                if (!memcmp(data_.data() + tag, "NAME", 4)) name.assign((char const*)body, strnlen((char const*)body, n));
                if (!memcmp(data_.data() + tag, "OFFT", 4) && n >= 24) memcpy(&offset, body + 16, 8);
                if (!memcmp(data_.data() + tag, "MDSZ", 4) && n >= 8) memcpy(&length, body, 8);
                tag += 6 + n;
            }
            if (!name.empty() && bitcode + offset + length <= data_.size()) functions_[name] = { bitcode + offset, length };
            at = end;
        }
        return !functions_.empty();
    }

    struct Varying { std::string Name, Type; };

    // What the function hands on (a vertex function) or takes (a fragment function) besides the position, in the
    // order its module names them. Metal names one "generated(<length><name><type>)" when the source gave it no
    // name of its own, the type as C++ mangles it. False when the function is not there or one is not read.
    bool Varyings(std::string const& function, std::vector<Varying>& varyings) const
    {
        auto it = functions_.find(function);
        if (it == functions_.end()) return false;
        for (auto const& s : MetadataStrings::Read(data_.subspan(it->second.first, it->second.second))) {
            static constexpr std::string_view kGenerated = "generated(";
            if (!s.starts_with(kGenerated) || !s.ends_with(")")) {
                if (s.starts_with("user(")) return false;       // named in the source: another rule
                continue;
            }
            std::string_view inner(s.data() + kGenerated.size(), s.size() - kGenerated.size() - 1);
            size_t digits = 0, length = 0;
            while (digits < inner.size() && isdigit((unsigned char)inner[digits])) length = length * 10 + (size_t)(inner[digits++] - '0');
            if (!digits || digits + length > inner.size()) return false;
            std::string code(inner.substr(digits + length)), type;
            if (code == "f") type = "float";
            else if (code == "i") type = "int";
            else if (code == "j") type = "uint";
            else if (code.size() == 5 && code.starts_with("Dv") && code[3] == '_' && code[2] >= '2' && code[2] <= '4') {
                type = std::string(code[4] == 'f' ? "float" : code[4] == 'i' ? "int" : code[4] == 'j' ? "uint" : "?") + code[2];
            }
            if (type.empty() || type[0] == '?') return false;
            varyings.push_back({ std::string(inner.substr(digits, length)), type });
        }
        return true;
    }

private:
    uint64_t U64(size_t at) const { uint64_t v; memcpy(&v, data_.data() + at, 8); return v; }
    std::span<uint8_t const> data_;
    std::map<std::string, std::pair<uint64_t, uint64_t>> functions_;
};

// The metadata strings of a DirectX 12 stage: of its STAT part (the module
// kept for reflection), else of its DXIL part.
std::vector<std::string> Dx12Strings(std::span<uint8_t const> stage)
{
    constexpr size_t kHeader = 18;
    if (stage.size() < kHeader + 32 || memcmp(stage.data() + kHeader, "DXBC", 4) != 0) return {};
    auto dxbc = stage.subspan(kHeader);
    uint32_t chunks = U32(dxbc.data() + 28);
    if (chunks > 64 || 32 + (uint64_t)chunks * 4 > dxbc.size()) return {};
    for (char const* wanted : { "STAT", "DXIL" }) {
        for (uint32_t i = 0; i < chunks; i++) {
            uint64_t at = U32(dxbc.data() + 32 + i * 4);
            if (at + 8 > dxbc.size() || memcmp(dxbc.data() + at, wanted, 4) != 0) continue;
            uint64_t size = U32(dxbc.data() + at + 4);
            if (at + 8 + size > dxbc.size() || size < 24) continue;
            auto part = dxbc.subspan(at + 8, size);
            // Program version, size; then "DXIL", version, the bitcode's offset from here, its size.
            if (memcmp(part.data() + 8, "DXIL", 4) != 0) continue;
            uint64_t offset = 8 + (uint64_t)U32(part.data() + 16), length = U32(part.data() + 20);
            if (offset + length > part.size()) continue;
            auto strings = MetadataStrings::Read(part.subspan(offset, length));
            if (!strings.empty()) return strings;
        }
    }
    return {};
}

}

// Whether the installed game still has everything the interface table names,
// where a fixed place and layout are needed: what translating a shader for
// this game relies on.
int InterfaceCheckCommand(std::string const& app)
{
    std::string error;
    auto pak = PakFile::Open(app + "/Contents/Data/Materials.pak", &error);
    auto game = pak ? MetalFunctions::OpenGame(app, error) : nullptr;
    if (!game) {
        fprintf(stderr, "cannot read the game: %s\n", error.c_str());
        return 1;
    }
    MacInterface mac;
    uint32_t stubs = 0, unread = 0;
    for (auto const& entry : pak->Entries()) {
        if (!std::string_view(entry.Name).ends_with(kMetalExt)) continue;
        stubs++;
        auto stub = pak->Read(entry);
        ContainerParts parts;
        MacPass pass;
        auto name = [](std::span<uint8_t const> blob) {
            std::string s((char const*)blob.data(), blob.size());
            while (!s.empty() && (s.back() == '\n' || s.back() == 0)) s.pop_back();
            return s;
        };
        if (!stub || !ParseContainer(*stub, parts) || parts.Version != 2
            || !game->Reflect(name(parts.Stages[0]), name(parts.Stages[4]), pass, error)) {
            unread++;
            continue;
        }
        mac.Add(pass);
    }

    auto const& table = BuiltInInterface();
    uint32_t problems = 0;
    auto problem = [&](std::string const& what) {
        printf("  NOT as the table has it: %s\n", what.c_str());
        problems++;
    };
    for (auto const& slot : table.Slots) {
        auto resource = mac.Resource(slot.Name, slot.Kind);
        if (!resource) { problem(slot.Name + " has no fixed place in the game's functions"); continue; }
        if (slot.Kind == ResourceKind::Texture && resource->Type != slot.Type) problem(slot.Name + " is " + resource->Type + ", not " + slot.Type);
    }
    for (auto const& block : table.Blocks) {
        auto resource = mac.Resource(block.Name, ResourceKind::Buffer);
        if (!resource || !mac.HasFixedLayout(*resource)) { problem(block.Name + " has no fixed layout in the game's functions"); continue; }
        for (size_t i = 0; i < block.Members.size(); i++) {
            if (block.Members[i].empty()) continue;
            bool found = std::any_of(resource->Members.begin(), resource->Members.end(),
                [&](auto const& m) { return m.Name == block.Members[i] && m.Type == block.Types[i]; });
            if (!found) problem(block.Name + "." + block.Members[i] + " (" + block.Types[i] + ") is not in the game's block");
        }
    }
    for (auto const& attribute : table.Attributes) {
        bool found = false;
        for (char scalar : { 'f', 'i', 'u' }) found = found || mac.Attribute(attribute.Name, std::string(1, scalar));
        if (!found) problem("the attribute " + attribute.Name + " has no fixed index in the game's functions");
    }
    printf("%u function pairs read (%u could not be); %zu slots, %zu blocks and %zu attributes of the interface table checked: %s\n",
        stubs - unread, unread, table.Slots.size(), table.Blocks.size(), table.Attributes.size(),
        problems ? "NOT as expected" : "as expected");
    return problems || mac.Passes() == 0 || table.Slots.empty() ? 1 : 0;
}

// Translates one material of a package as the game would, compiles the
// result, and with `out` writes what would be served and the Metal source.
int TranslateCommand(std::string const& package, std::string const& material, std::string const& app, std::string const& out)
{
    std::string error;
    auto pak = PakFile::Open(package, &error);
    if (!pak) { fprintf(stderr, "cannot read %s: %s\n", package.c_str(), error.c_str()); return 1; }
    MaterialSource source;
    source.Label = pak->FileName();
    for (auto const& entry : pak->Entries()) source.Names.insert(entry.Name);
    source.Read = [&](std::string const& name) -> std::optional<std::vector<uint8_t>> {
        auto entry = pak->Find(name);
        return entry ? pak->Read(*entry) : std::nullopt;
    };
    MacInterface mac;
    if (!SurveyMacInterface(app, mac, error)) { fprintf(stderr, "cannot read the game: %s\n", error.c_str()); return 1; }

    std::string why;
    std::vector<std::string> reasons;
    // The table as the game itself would have it (the same check on this game's binary).
    BridgeProblems problems;
    InterfaceTable table = InterfaceForBundle(app, mac, problems);
    if (!problems.SunShadow.empty()) printf("the sun's shadow constants are not worked out for this game: %s\n", problems.SunShadow.c_str());
    if (!problems.TextureTable.empty()) printf("the table of textures is not bridged for this game: %s\n", problems.TextureTable.c_str());
    auto translated = TranslateMaterial(source, material, table, mac, why, getenv("BG3MSE_SHADERS_REASONS") ? &reasons : nullptr);
    if (!translated) {
        printf("refused: %s\n", why.c_str());
        // BG3MSE_SHADERS_REASONS: every reason that refuses a pass or a stage of it, not only the first.
        for (auto const& reason : reasons) printf("  also: %s\n", reason.c_str());
        return 1;
    }
    for (auto const& pass : translated->Passes) {
        std::vector<std::string> sources = { pass.VertexSource };
        if (!pass.FragmentSource.empty()) sources.push_back(pass.FragmentSource);
        auto compiled = MetalFunctions::Compile(sources, why);
        MacPass taken;
        bool ok = compiled && compiled->Reflect(pass.VertexName, pass.FragmentName, taken, why);
        printf("%-10s %s%s%s  %s%s\n", pass.Pass.c_str(), pass.VertexName.c_str(), pass.FragmentName.empty() ? "" : " + ", pass.FragmentName.c_str(),
            ok ? "compiles" : "DOES NOT COMPILE: ", ok ? "" : why.c_str());
        if (!ok) return 1;
        if (out.empty()) continue;
        auto write = [&](std::string const& name, void const* data, size_t size) {
            FILE* f = fopen((out + "/" + name).c_str(), "wb");
            if (f) { fwrite(data, 1, size, f); fclose(f); }
        };
        std::string stem = material.substr(material.rfind('/') + 1);
        stem = stem.substr(0, stem.size() - 4) + "_" + pass.Pass;
        write(stem + "_Metal.bshd", pass.Stub.data(), pass.Stub.size());
        write(stem + ".vertex.metal", pass.VertexSource.data(), pass.VertexSource.size());
        if (!pass.FragmentSource.empty()) write(stem + ".fragment.metal", pass.FragmentSource.data(), pass.FragmentSource.size());
    }
    if (!out.empty()) {
        FILE* f = fopen((out + "/" + material.substr(material.rfind('/') + 1)).c_str(), "wb");
        if (f) { fwrite(translated->Lsf.data(), 1, translated->Lsf.size(), f); fclose(f); }
    }
    printf("%zu passes translated; the .lsf is %zu bytes", translated->Passes.size(), translated->Lsf.size());
    if (translated->OwnVertexStages) printf("; its vertex stage is the mod's own (%u of %zu passes)", translated->OwnVertexStages, translated->Passes.size());
    for (size_t i = 0; i < translated->ByOrderOnly.size(); i++) printf("%s%s", i ? ", " : "; taken by a pairing made by order only: ", translated->ByOrderOnly[i].c_str());
    printf("\n");
    return 0;
}

// The vertex stages of a package's containers as Metal source, one file per stage that differs (named by what the
// stage is), with a list of the containers that have each: for seeing what two stages differ in. Nothing is placed:
// the source is the shader's own, with the toolkit's names for everything.
// Which members of the engine's blocks each stage of a package's containers reads, by the names the interface table
// has for them: one line per container, stage and block. For counting what a mapping by name rests on.
int ReadsCommand(std::string const& package)
{
    std::string error;
    auto pak = PakFile::Open(package, &error);
    if (!pak) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
    auto const& table = BuiltInInterface();
    for (auto const& entry : pak->Entries()) {
        if (!entry.Name.ends_with("_Vulkan.bshd")) continue;
        auto data = pak->Read(entry);
        ContainerParts parts;
        if (!data || !ParseContainer(*data, parts)) continue;
        for (int s : { 0, 4 }) {
            auto modules = VulkanStageModules(parts.Stages[s]);
            if (modules.empty()) continue;
            std::string why;
            auto shader = SpirvShader::Parse(modules[0], why);
            if (!shader) continue;
            for (auto const& r : shader->Resources()) {
                if (r.Kind != ResourceKind::Buffer) continue;
                EngineBlock const* block = nullptr;
                for (auto const& slot : table.Slots) {
                    if (slot.Set != r.Set || slot.Binding != r.Binding || slot.Kind != ResourceKind::Buffer) continue;
                    for (auto const& b : table.Blocks) if (b.Name == slot.Name) block = &b;
                }
                if (!block) continue;
                std::string names;
                if (!r.ReadKnown) names = "?";
                for (uint32_t read : r.Read) {
                    auto absent = block->Absent.find(read);
                    names += (names.empty() ? "" : ",") + (read < block->Members.size() && !block->Members[read].empty() ? block->Members[read]
                        : absent != block->Absent.end() ? "-" + absent->second : "#" + std::to_string(read));
                }
                printf("%s\t%d\t%s\t%s\n", entry.Name.substr(0, entry.Name.size() - 12).c_str(), s, block->Name.c_str(), names.c_str());
            }
        }
    }
    return 0;
}

int StageSourcesCommand(std::string const& package, std::string const& part, std::string const& outDir)
{
    std::string error;
    auto pak = PakFile::Open(package, &error);
    if (!pak) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
    mkdir(outDir.c_str(), 0755);
    FILE* index = fopen((outDir + "/index.tsv").c_str(), "w");
    if (!index) { fprintf(stderr, "cannot write into %s\n", outDir.c_str()); return 1; }
    std::set<std::string> written;
    size_t containers = 0, failed = 0;
    for (auto const& entry : pak->Entries()) {
        if (!entry.Name.ends_with("_Vulkan.bshd") || (!part.empty() && entry.Name.find(part) == std::string::npos)) continue;
        auto data = pak->Read(entry);
        ContainerParts parts;
        if (!data || !ParseContainer(*data, parts)) continue;
        auto modules = VulkanStageModules(parts.Stages[0]);
        SpirvHash stage{};
        std::string why, hex;
        if (modules.empty() || !SpirvModuleHash(modules[0], stage, why)) { failed++; continue; }
        for (uint8_t b : stage) { char two[3]; snprintf(two, sizeof(two), "%02x", b); hex += two; }
        containers++;
        fprintf(index, "%s\t%s\n", hex.c_str(), entry.Name.c_str());
        if (!written.insert(hex).second) continue;
        std::string source;
        auto shader = SpirvShader::Parse(modules[0], why);
        if (shader) {
            // Everything by the place the shader itself gives it, so that two stages that take the same things say so.
            uint32_t next = 0;
            for (auto const& r : shader->Resources()) {
                shader->Bind(r, next++, "set" + std::to_string(r.Set) + "_binding" + std::to_string(r.Binding));
            }
            for (auto const& input : shader->Inputs()) shader->PlaceInput(input, input.Location, "location" + std::to_string(input.Location));
        }
        if (!shader || !shader->Translate("vertexStage", source, why)) { failed++; source = "// " + why + "\n"; }
        if (FILE* f = fopen((outDir + "/" + hex + ".metal").c_str(), "w")) {
            fwrite(source.data(), 1, source.size(), f);
            fclose(f);
        }
    }
    fclose(index);
    printf("%zu containers, %zu vertex stages written, %zu could not be read or translated\n", containers, written.size(), failed);
    return 0;
}

// What the stages of a Vulkan container of a package take, as the shader says it.
int InputsCommand(std::string const& package, std::string const& container, std::string const& app)
{
    if (!app.empty()) {
        // Beside it, what the Mac game's functions for the pass of that name take.
        static constexpr std::string_view kVulkanExt = "_Vulkan.bshd";
        std::string why, stem = container.ends_with(kVulkanExt) ? container.substr(0, container.size() - kVulkanExt.size()) : container;
        auto mac = PakFile::Open(app + "/Contents/Data/Materials.pak", &why);
        auto game = mac ? MetalFunctions::OpenGame(app, why) : nullptr;
        auto stubEntry = mac ? mac->Find(stem + std::string(kMetalExt)) : nullptr;
        auto stub = stubEntry ? mac->Read(*stubEntry) : std::nullopt;
        ContainerParts parts;
        MacPass pass;
        auto name = [](std::span<uint8_t const> blob) {
            std::string n((char const*)blob.data(), blob.size());
            while (!n.empty() && (n.back() == '\n' || n.back() == 0)) n.pop_back();
            return n;
        };
        if (game && stub && ParseContainer(*stub, parts) && game->Reflect(name(parts.Stages[0]), name(parts.Stages[4]), pass, why)) {
            printf("the Mac game's functions take:");
            for (auto const& a : pass.Attributes) printf(" %s@%u", a.Name.c_str(), a.Index);
            for (auto const* stage : { &pass.Vertex, &pass.Fragment }) {
                for (auto const& r : *stage) if (r.Used) printf(" %s:%s@%u", stage == &pass.Vertex ? "v" : "f", r.Name.c_str(), r.Index);
            }
            printf("\n");
            if (getenv("BG3MSE_SHADERS_SHOW")) {
                for (auto const& r : pass.Fragment) {
                    if (!r.Used || r.Members.empty()) continue;
                    printf("  %s, %u bytes:", r.Name.c_str(), r.Size);
                    for (auto const& m : r.Members) printf(" %s:%s@%u", m.Name.c_str(), m.Type.c_str(), m.Offset);
                    printf("\n");
                }
            }
        } else {
            printf("the Mac game has no functions for it (%s)\n", why.c_str());
        }
    }
    std::string error;
    auto pak = PakFile::Open(package, &error);
    auto entry = pak ? pak->Find(container) : nullptr;
    if (pak && !entry) {
        // Not a whole path: the first container whose path has it.
        for (auto const& e : pak->Entries()) {
            if (e.Name.find(container) != std::string::npos && e.Name.ends_with("_Vulkan.bshd")) { entry = &e; break; }
        }
        if (entry) printf("%s\n", entry->Name.c_str());
    }
    auto data = entry ? pak->Read(*entry) : std::nullopt;
    ContainerParts parts;
    if (!data || !ParseContainer(*data, parts)) {
        fprintf(stderr, "cannot read %s\n", container.c_str());
        return 1;
    }
    if (getenv("BG3MSE_SHADERS_SHOW")) {
        printf("reflection table, %zu bytes:", parts.Reflection.size());
        for (size_t i = 0; i < parts.Reflection.size(); i++) printf("%s%02x", i % 32 ? " " : "\n  ", parts.Reflection[i]);
        printf("\n");
    }
    for (int s = 0; s < 6; s++) {
        auto stage = parts.Stages[s];
        if (stage.empty()) continue;
        auto modules = VulkanStageModules(stage);
        printf("stage %d: %zu bytes, %zu modules%s\n", s, stage.size(), modules.size(), modules.empty() ? " (not a chunk this code reads)" : "");
        if (modules.empty()) {
            printf("  begins:");
            for (size_t i = 0; i < 56 && i < stage.size(); i++) printf(" %02x", stage[i]);
            printf("\n");
            continue;
        }
        if (char const* folder = getenv("BG3MSE_SHADERS_MODULES")) {
            // For reading a stage's own instructions: its first module, as it is in the container.
            std::string path = std::string(folder) + "/stage" + std::to_string(s) + ".spv";
            if (FILE* file = fopen(path.c_str(), "wb")) {
                fwrite(modules[0].data(), 1, modules[0].size(), file);
                fclose(file);
                printf("  written to %s\n", path.c_str());
            }
        }
        std::string why;
        SpirvHash identity{};
        if (SpirvModuleHash(modules[0], identity, why)) {
            // What the stage is, as the table of proven vertex stages has it.
            printf("  is ");
            for (uint8_t b : identity) printf("%02x", b);
            printf("\n");
        }
        auto shader = SpirvShader::Parse(modules[0], why);
        if (!shader) { printf("  %s\n", why.c_str()); continue; }
        for (auto const& input : shader->Inputs()) printf("  input at location %u: %s\n", input.Location, input.Type.c_str());
        for (auto const& r : shader->Resources()) {
            printf("  (%u, %u) %s %s", r.Set, r.Binding, r.Kind == ResourceKind::Buffer ? "buffer" : r.Kind == ResourceKind::Texture ? "texture" : "sampler",
                r.Type.c_str());
            if (r.Kind == ResourceKind::Buffer) {
                printf(" size %u:", r.Size);
                for (size_t m = 0; m < r.Members.size(); m++) {
                    bool read = std::find(r.Read.begin(), r.Read.end(), (uint32_t)m) != r.Read.end();
                    printf(" %s%s@%u", read ? "*" : "", r.Members[m].Type.c_str(), r.Members[m].Offset);
                }
            }
            printf("\n");
        }
    }
    return 0;
}

// The names a pass's DirectX 12 stage carries, in the order its bitcode stores them.
int NamesCommand(std::string const& windowsPak, std::string const& stem, int stage)
{
    std::string error;
    auto pak = PakFile::Open(windowsPak, &error);
    auto entry = pak ? pak->Find(stem + "_DX12.bshd") : nullptr;
    auto data = entry ? pak->Read(*entry) : std::nullopt;
    ContainerParts parts;
    if (!data || !ParseContainer(*data, parts) || stage < 0 || stage > 5) {
        fprintf(stderr, "cannot read %s_DX12.bshd\n", stem.c_str());
        return 1;
    }
    for (auto const& s : Dx12Strings(parts.Stages[stage])) printf("%s\n", s.c_str());
    return 0;
}

namespace {

// --- interface ----------------------------------------------------------------------

// See BG3MSE_SHADERS_SUN below.
std::string SunShadowDraws();

struct Tally
{
    uint32_t Uses{ 0 };
    std::map<std::string, uint32_t> With;       // the Mac name -> passes where both are there
    WindowsResource Example;
    std::string ExamplePass;
    std::string Type;
    std::map<uint32_t, uint32_t> ReadBy;        // a block's member -> the stages that read it
};

// How well a Windows thing and a Mac name go together: the passes that have
// both, over the passes that have either one more often.
double Score(Tally const& tally, std::string const& name, std::map<std::string, uint32_t> const& macUses)
{
    auto with = tally.With.find(name);
    if (with == tally.With.end()) return 0;
    return (double)with->second / std::max(tally.Uses, macUses.at(name));
}

// The Mac names that go with it: there nine times in ten that either is, and
// as often as the best of them. One name is a pairing; several are a group
// whose members nothing here tells apart.
std::vector<std::string> Partners(Tally const& tally, std::map<std::string, uint32_t> const& macUses)
{
    double best = 0;
    for (auto const& [name, count] : tally.With) best = std::max(best, Score(tally, name, macUses));
    std::vector<std::string> out;
    if (best < 0.9) return out;
    for (auto const& [name, count] : tally.With) {
        if (best - Score(tally, name, macUses) < 0.02) out.push_back(name);
    }
    return out;
}

// Pairs every key with a Mac name. A key with one partner gets it; keys that
// share the same partners, as many as there are of them, are paired in order
// (the keys' order with the names' Mac index order) and marked inferred.
template <class Key>
std::map<Key, std::pair<std::string, bool>> Pair(std::map<Key, Tally> const& tallies, std::map<std::string, uint32_t> const& macUses,
    std::map<std::string, uint32_t> const& macIndex, std::vector<std::string>& notes, std::function<std::string(Key const&)> const& text)
{
    std::map<Key, std::pair<std::string, bool>> out;
    std::map<std::vector<std::string>, std::vector<Key>> groups;
    for (auto const& [key, tally] : tallies) {
        auto partners = Partners(tally, macUses);
        if (partners.empty()) {
            notes.push_back(text(key) + ", used by " + std::to_string(tally.Uses) + " passes: no Mac name goes with it");
            continue;
        }
        std::sort(partners.begin(), partners.end(), [&](auto const& a, auto const& b) { return macIndex.at(a) < macIndex.at(b); });
        groups[partners].push_back(key);
    }
    std::map<std::string, Key> owner;
    for (auto const& [partners, keys] : groups) {
        if (partners.size() != keys.size()) {
            std::string names;
            for (auto const& p : partners) names += " " + p;
            for (auto const& key : keys) notes.push_back(text(key) + ": one of" + names + ", which nothing tells apart");
            continue;
        }
        for (size_t i = 0; i < keys.size(); i++) {
            if (auto [it, first] = owner.try_emplace(partners[i], keys[i]); !first) {
                notes.push_back(text(keys[i]) + " and " + text(it->second) + " both go with " + partners[i]);
                out.erase(it->second);
                continue;
            }
            out[keys[i]] = { partners[i], keys.size() > 1 };
        }
    }
    return out;
}

std::string Quoted(std::string const& s)
{
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out + "\"";
}

}

int InterfaceCommand(std::string const& windowsPak, std::string const& app, std::string const& build, std::string const& provenPath,
    std::string const& faultsPath)
{
    // The open faults: what a translated shader is known to draw wrong and is not put right yet, and how a shader is
    // known to touch each. A file of lines:
    //   fault <id> <the sentence a refusal says>
    //   resource <id> <the Mac name of a thing of the engine's>
    //   stage <id> <a vertex stage of the game's, by what it is>
    //   pass <id> <a pass of the game's, by its path without the extension: both its stages>
    struct FaultLine { std::string Id, What; };
    std::vector<FaultLine> faults;
    std::vector<std::pair<std::string, std::string>> faultyResources, faultyStages, faultyPasses;       // id, what
    if (!faultsPath.empty()) {
        std::ifstream in(faultsPath);
        if (!in) { fprintf(stderr, "cannot read %s\n", faultsPath.c_str()); return 1; }
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto first = line.find(' '), second = line.find(' ', first + 1);
            if (first == std::string::npos || second == std::string::npos) { fprintf(stderr, "%s: not a line of faults: %s\n", faultsPath.c_str(), line.c_str()); return 1; }
            std::string kind = line.substr(0, first), id = line.substr(first + 1, second - first - 1), what = line.substr(second + 1);
            if (kind == "fault") faults.push_back({ id, what });
            else if (kind == "resource") faultyResources.push_back({ id, what });
            else if (kind == "stage") faultyStages.push_back({ id, what });
            else if (kind == "pass") faultyPasses.push_back({ id, what });
            else { fprintf(stderr, "%s: not a line of faults: %s\n", faultsPath.c_str(), line.c_str()); return 1; }
        }
    }
    auto faultIndex = [&](std::string const& id) -> int {
        for (size_t i = 0; i < faults.size(); i++) if (faults[i].Id == id) return (int)i;
        return -1;
    };
    for (auto const* list : { &faultyResources, &faultyStages, &faultyPasses }) {
        for (auto const& [id, what] : *list) {
            if (faultIndex(id) < 0) { fprintf(stderr, "%s: %s is said of fault %s, which has no sentence\n", faultsPath.c_str(), what.c_str(), id.c_str()); return 1; }
        }
    }
    // Every vertex stage of the game's, and for the passes named both stages: found as the passes are gone through.
    std::set<std::string> gameStages;
    std::map<std::string, std::pair<std::string, std::string>> stagesOfPass;
    for (auto const& [id, pass] : faultyPasses) stagesOfPass[pass];
    auto hexOf = [](std::span<uint8_t const> stage) {
        std::string hex;
        auto modules = VulkanStageModules(stage);
        SpirvHash hash{};
        std::string unread;
        if (modules.empty() || !SpirvModuleHash(modules[0], hash, unread)) return hex;
        for (uint8_t b : hash) { char two[3]; snprintf(two, sizeof(two), "%02x", b); hex += two; }
        return hex;
    };
    // What `verify --prove` found: a pairing made by order that at least twenty passes bear out is no longer only inferred.
    std::map<std::string, uint32_t> proven;
    // And for a slot of a group that always appears together: the name the pictures gave it, pass by pass.
    std::map<std::pair<uint32_t, uint32_t>, std::map<std::string, uint32_t>> drawn;
    if (!provenPath.empty()) {
        FILE* f = fopen(provenPath.c_str(), "r");
        if (!f) { fprintf(stderr, "cannot read %s\n", provenPath.c_str()); return 1; }
        char kind[16], name[256];
        unsigned a = 0, b = 0;
        while (fscanf(f, "%15s", kind) == 1) {
            if (!strcmp(kind, "name") && fscanf(f, "%255s %u %u", name, &a, &b) == 3) proven[name] = a;
            else if (!strcmp(kind, "slot") && fscanf(f, "%u %u %255s", &a, &b, name) == 3) {
                unsigned count = 0;
                if (fscanf(f, "%u", &count) == 1) drawn[{ a, b }][name] = count;
            } else break;
        }
        fclose(f);
    }
    constexpr uint32_t kEnough = 20;
    Survey survey;
    if (!survey.Open(windowsPak, app)) return 1;

    std::map<std::pair<uint32_t, uint32_t>, Tally> slots;
    std::map<std::pair<std::string, uint32_t>, Tally> semantics;
    std::map<std::string, uint32_t> macUses, macAttributeUses, macIndex, macAttributeIndex;
    std::map<std::string, MacResource> macBlocks;
    uint32_t surveyed = 0, skipped = 0;
    // The names materials give each slot they declare. A slot of many names
    // is the material's own (its textures); a slot that always has the same
    // name is one of the engine's that a material may declare (a sampler).
    std::map<std::pair<uint32_t, uint32_t>, std::set<std::string>> materialSlots;

    for (size_t i = 0; i < survey.Count(); i++) {
        auto pass = survey.Load(i);
        if (auto vertex = hexOf(pass.Parts.Stages[0]); !vertex.empty()) {
            gameStages.insert(vertex);
            if (auto named = stagesOfPass.find(pass.Stem); named != stagesOfPass.end()) named->second = { vertex, hexOf(pass.Parts.Stages[4]) };
        }
        if (!pass.Problem.empty()) { skipped++; continue; }
        std::string why;
        std::unique_ptr<SpirvShader> stages[2] = {
            SpirvShader::Parse(ModuleOf(pass.Parts.Stages[0]), why),
            pass.Parts.Stages[4].empty() ? nullptr : SpirvShader::Parse(ModuleOf(pass.Parts.Stages[4]), why),
        };
        if (!stages[0] || (!pass.Parts.Stages[4].empty() && !stages[1])) { skipped++; continue; }
        surveyed++;

        std::set<std::string> counted;
        for (int s = 0; s < 2; s++) {
            if (!stages[s]) continue;
            auto const& mac = s ? pass.Mac.Fragment : pass.Mac.Vertex;
            for (auto const& m : mac) {
                if (!m.Used || IsMaterialName(pass, m.Name)) continue;
                if (counted.insert(std::to_string((int)m.Kind) + m.Name).second) macUses[m.Name]++;
                macIndex[m.Name] = m.Index;
                if (m.Kind == ResourceKind::Buffer && m.Type == "block") macBlocks.try_emplace(m.Name, m);
            }
        }
        for (auto const& slot : pass.Material) materialSlots[{ slot.Set, slot.Binding }].insert(slot.Name);
        std::set<std::pair<uint32_t, uint32_t>> seen;
        for (int s = 0; s < 2; s++) {
            if (!stages[s]) continue;
            auto const& mac = s ? pass.Mac.Fragment : pass.Mac.Vertex;
            for (auto const& r : stages[s]->Resources()) {
                if (IsMaterialSlot(pass, r)) continue;
                if (r.Kind == ResourceKind::Buffer) for (uint32_t read : r.Read) slots[{ r.Set, r.Binding }].ReadBy[read]++;
                if (!seen.insert({ r.Set, r.Binding }).second) continue;
                auto& tally = slots[{ r.Set, r.Binding }];
                if (!tally.Uses++) { tally.Example = r; tally.ExamplePass = pass.Stem; }
                std::set<std::string> here;
                // In either stage: a block both stages read is one argument of each function.
                for (auto const* side : { &pass.Mac.Vertex, &pass.Mac.Fragment }) {
                    for (auto const& m : *side) {
                        if (!m.Used || m.Kind != r.Kind || IsMaterialName(pass, m.Name)) continue;
                        if (r.Kind == ResourceKind::Texture && m.Type != r.Type) continue;
                        here.insert(m.Name);
                    }
                }
                (void)mac;
                for (auto const& name : here) tally.With[name]++;
            }
        }
        for (auto const& a : pass.Mac.Attributes) {
            macAttributeUses[a.Name]++;
            macAttributeIndex[a.Name] = a.Index;
        }
        for (auto const& input : stages[0]->Inputs()) {
            if (input.Location >= pass.Inputs.size()) continue;
            auto const& semantic = pass.Inputs[input.Location];
            auto& tally = semantics[{ semantic.Name, semantic.Index }];
            tally.Uses++;
            tally.Type = input.Type;
            for (auto const& a : pass.Mac.Attributes) {
                if (a.Type[0] == input.Type[0]) tally.With[a.Name]++;
            }
        }
        if (i % 1000 == 0) fprintf(stderr, "  %zu of %zu passes\n", i, survey.Count());
    }

    InterfaceTable table;
    table.Build = build;
    std::vector<std::string> notes;
    for (auto& [slot, tally] : slots) {
        // The vertex buffers are arguments of every function, and nothing a shader declares.
        std::erase_if(tally.With, [&](auto const& with) { return with.first.starts_with("vertexBuffer."); });
    }

    using SlotKey = std::pair<uint32_t, uint32_t>;
    auto slotPairs = Pair<SlotKey>(slots, macUses, macIndex, notes, [&](SlotKey const& k) {
        return "(" + std::to_string(k.first) + ", " + std::to_string(k.second) + ") " + slots.at(k).Example.Type;
    });
    for (auto const& [slot, tally] : slots) {
        auto paired = slotPairs.find(slot);
        if (paired == slotPairs.end()) continue;
        std::string name = paired->second.first;
        if (auto declared = materialSlots.find(slot); declared != materialSlots.end()) {
            // Where materials declare it too, they must all call it what the Mac game does.
            bool agrees = declared->second.size() == 1 && *declared->second.begin() == name && tally.Example.Kind == ResourceKind::Sampler;
            if (!agrees) {
                if (tally.Uses >= 20) {
                    notes.push_back("(" + std::to_string(slot.first) + ", " + std::to_string(slot.second) + "), which materials declare, goes with "
                        + name + " in " + std::to_string(tally.Uses) + " passes that do not declare it: left out");
                }
                continue;
            }
        }
        bool inferredOnly = paired->second.second;
        // Five passes that each allow one assignment only, and none that says otherwise, settle a slot.
        constexpr uint32_t kEnoughPictures = 5;
        if (auto pictures = drawn.find(slot); inferredOnly && pictures != drawn.end() && pictures->second.size() == 1
            && pictures->second.begin()->second >= kEnoughPictures) {
            // Every pass whose picture depends on the group and settles it gives this slot the same name.
            name = pictures->second.begin()->first;
            inferredOnly = false;
            notes.push_back("(" + std::to_string(slot.first) + ", " + std::to_string(slot.second) + ") is " + name + ": in "
                + std::to_string(pictures->second.begin()->second) + " passes no other pairing of its group draws what the game's function draws");
        }
        if (inferredOnly && proven[name] >= kEnough) {
            inferredOnly = false;
            notes.push_back(name + ": paired by order within its group; " + std::to_string(proven[name]) + " passes draw the same and depend on it");
        }
        table.Slots.push_back({ slot.first, slot.second, tally.Example.Kind, tally.Example.Type, name, inferredOnly });

        if (tally.Example.Kind != ResourceKind::Buffer || tally.Example.Type != "block") continue;
        auto mac = macBlocks.find(name);
        if (mac == macBlocks.end()) continue;
        auto const& windows = tally.Example.Members;
        auto const& members = mac->second.Members;
        EngineBlock block;
        block.Name = name;
        for (auto const& m : windows) {
            block.Types.push_back(m.Type);
            block.Offsets.push_back(m.Offset);
        }
        block.Members.resize(windows.size());
        // A member no stage of the game reads is paired by its name and type alone.
        auto untested = [&] {
            std::string names;
            for (uint32_t m = 0; m < block.Members.size(); m++) {
                if (block.Members[m].empty() || tally.ReadBy.contains(m)) continue;
                block.Untested.push_back(m);
                names += " " + block.Members[m];
            }
            if (!names.empty()) notes.push_back(name + ": " + std::to_string(block.Untested.size()) + " members no shader of the game reads:" + names);
        };

        // The same block, or the same with members added at its end, is named by position.
        size_t common = 0;
        while (common < windows.size() && common < members.size() && windows[common].Offset == members[common].Offset
            && windows[common].Type == members[common].Type) common++;
        if (common == members.size() && common <= windows.size()) {
            for (size_t m = 0; m < common; m++) block.Members[m] = members[m].Name;
            if (common < windows.size()) notes.push_back(name + ": Windows has " + std::to_string(windows.size() - common) + " more members at the end");
            untested();
            table.Blocks.push_back(std::move(block));
            continue;
        }

        // A block that differs is named by the shader's own DirectX 12 build of it.
        auto dx12 = survey.Dx12Fragment(tally.ExamplePass);
        auto strings = dx12 ? Dx12Strings(*dx12) : std::vector<std::string>{};
        std::map<std::string, BlockMember const*> byName;
        for (auto const& m : members) byName[m.Name] = &m;
        size_t bestAt = 0, bestCount = 0, bestTimes = 0;
        for (size_t at2 = 0; at2 + windows.size() <= strings.size(); at2++) {
            size_t count = 0;
            uint32_t last = 0;
            bool ok = true;
            for (size_t m = 0; m < windows.size() && ok; m++) {
                auto it = byName.find(strings[at2 + m]);
                // A member of that name and another type is another member.
                if (it == byName.end() || it->second->Type != windows[m].Type) continue;
                ok = count == 0 || it->second->Offset > last;
                last = it->second->Offset;
                count++;
            }
            if (!ok) continue;
            if (count > bestCount) { bestCount = count; bestAt = at2; bestTimes = 1; }
            else if (count == bestCount) bestTimes++;
        }
        if (bestCount < 2 || bestTimes != 1) {
            std::string sample;
            for (size_t k = 0; k < strings.size() && k < 60; k++) sample += " " + strings[k];
            notes.push_back(name + ": differs from the Mac block and its members' names were not found (" + std::to_string(strings.size())
                + " metadata strings in " + tally.ExamplePass + "):" + sample);
            continue;
        }
        std::string windowsOnly;
        for (size_t m = 0; m < windows.size(); m++) {
            auto const& s = strings[bestAt + m];
            auto it = byName.find(s);
            // A member the Mac block has under the same name and type and fills with another thing is not that
            // member. These are known from the engine's code, not from the two interfaces, which agree on them.
            static const std::map<std::pair<std::string, std::string>, std::string> kOtherMeaning = {
                // ls::SetupForwardLightingConstants writes one over the fade amount, and the game's functions
                // multiply by it; the Windows shaders divide by the member of that name.
                { { "ForwardLightingConstants", "g_ShadowFadeAmount" }, "its reciprocal" },
            };
            if (auto other = kOtherMeaning.find({ name, s }); other != kOtherMeaning.end()) {
                block.Absent[(uint32_t)m] = s;
                block.OtherMeaning[(uint32_t)m] = other->second;
                windowsOnly += " " + s + " (the Mac's holds " + other->second + ")";
                continue;
            }
            if (it != byName.end() && it->second->Type == windows[m].Type) { block.Members[m] = s; continue; }
            block.Absent[(uint32_t)m] = s;
            windowsOnly += " " + s + (it != byName.end() ? " (" + windows[m].Type + ", the Mac's " + it->second->Type + ")" : "");
        }
        notes.push_back(name + ": named by its DirectX 12 build; " + std::to_string(bestCount) + " of " + std::to_string(windows.size())
            + " members are in the Mac block; not in it:" + windowsOnly);
        // Members the Mac block lacks that are worked out from members it has. Written by hand from the engine's
        // code (ls::SetupForwardLightingConstants, which the game that runs is checked against before any of
        // these is used); the two interfaces only say that the Mac block has what each is worked out from.
        struct DerivedEntry { char const* Block; char const* Member; EngineBlock::DerivedMember How; };
        static const DerivedEntry kDerived[] = {
            // The function writes one over the fade amount.
            { "ForwardLightingConstants", "g_ShadowFadeAmount", { "g_ShadowFadeAmount", "f1", 0, "(1.0 / $)" } },
            // One over each cascade's width.
            { "ForwardLightingConstants", "g_ShadowFrustumWidths", { "g_ShadowInvFrustumWidths", "f4", 0, "(1.0 / $)" } },
            // For each cascade (bias / far) * mix(width / width0, far / far0, 0.75), and the mix is 1 for the first.
            { "ForwardLightingConstants", "g_ShadowBias", { "g_ShadowCascadeBias", "f4", 0, "($ * {g_ShadowFarPlanes}.x)" } },
            // The count as a whole number, where the Windows shaders have it as a float.
            { "ForwardLightingConstants", "g_NumActiveCascades", { "g_NumActiveCascades", "u1", 0, "float(as_type<uint>($))" } },
        };
        std::string worked;
        for (auto const& entry : kDerived) {
            if (name != entry.Block) continue;
            auto from = byName.find(entry.How.From);
            bool put = false;
            for (auto const& [m, absent] : block.Absent) {
                if (absent != entry.Member || from == byName.end() || from->second->Type != entry.How.FromType) continue;
                block.Derived[m] = entry.How;
                put = true;
            }
            worked += std::string(" ") + entry.Member + (put ? " (from " + entry.How.From + ")" : " (NOT: the Mac block has no " + entry.How.From + " of that type)");
        }
        if (!worked.empty()) notes.push_back(name + ": worked out from what the Mac block has:" + worked);
        untested();
        table.Blocks.push_back(std::move(block));
    }
    // The table of textures (ShaderInterface.h: TextureTableRewrite). Written by hand from the two platforms'
    // functions and from the engine's code (ls::ClusteredAssignmentStage, which the game that runs is checked
    // against before this is used): which members hold a probe's and a light's places, how many arrays the Mac
    // game keeps the specular cubes and the cookies in, and under which names. What the interfaces say of it is
    // only that both sides have the things named.
    {
        constexpr uint32_t kTableSet = 2, kTableBinding = 32, kNewSet = 15;
        struct MacTexture { char const* Name; char const* Type; };
        static constexpr MacTexture kMacTextures[5] = { { "LightProbeDiffRGB", "cubearray" }, { "LightProbeDiffAlpha", "cubearray" },
            { "LightProbeSpecRGB", "cubearray[]" }, { "LightProbeSpecAlpha", "cubearray[]" }, { "LightCookie", "2darray[]" } };
        EngineSlot const* probes = nullptr;
        EngineSlot const* lights = nullptr;
        for (auto const& slot : table.Slots) {
            if (slot.Name == "LightProbeDataBuffer") probes = &slot;
            if (slot.Name == "DirectionLightDataBuffer") lights = &slot;
        }
        bool theirs = slots.contains({ kTableSet, kTableBinding }) && probes && lights, ours = true;
        for (auto const& texture : kMacTextures) ours = ours && macIndex.contains(texture.Name);
        if (theirs && ours) {
            TextureTableRewrite textures;
            textures.Set = kTableSet;
            textures.Binding = kTableBinding;
            textures.ProbesSet = probes->Set;
            textures.ProbesBinding = probes->Binding;
            textures.DiffuseMember = 5;         // ProbeShapeType_Diff
            textures.SpecularMember = 6;        // Spec
            textures.LightsSet = lights->Set;
            textures.LightsBinding = lights->Binding;
            textures.CookieMember = 1;          // LightCookiePreExpose
            textures.SpecularArrays = 4;
            textures.CookieArrays = 6;
            textures.NewSet = kNewSet;
            for (uint32_t i = 0; i < 5; i++) {
                textures.NewBindings[i] = i;
                table.Slots.push_back({ kNewSet, i, ResourceKind::Texture, kMacTextures[i].Type, kMacTextures[i].Name, false });
            }
            table.Textures = textures;
            notes.push_back("(2, 32) is the table of textures: its cubes and cookies are taken from the Mac's LightProbeDiffRGB, LightProbeDiffAlpha, "
                "LightProbeSpecRGB, LightProbeSpecAlpha and LightCookie");
        } else if (slots.contains({ kTableSet, kTableBinding })) {
            notes.push_back("(2, 32), the table of textures: NOT bridged (the two buffers or the Mac's five textures were not found)");
        }
    }
    notes.push_back("vertex stages of the game's: " + std::to_string(gameStages.size()));
    for (size_t f = 0; f < faults.size(); f++) {
        size_t resources = 0, stages = 0, passes = 0;
        for (auto const& [id, what] : faultyResources) resources += id == faults[f].Id;
        for (auto const& [id, what] : faultyStages) stages += id == faults[f].Id;
        for (auto const& [id, what] : faultyPasses) passes += id == faults[f].Id;
        notes.push_back("open fault " + faults[f].Id + " (" + faults[f].What + "): " + std::to_string(resources) + " things of the engine's, "
            + std::to_string(stages) + " vertex stages, " + std::to_string(passes) + " passes");
    }
    for (auto const& [id, what] : faultyStages) {
        if (!gameStages.contains(what)) { fprintf(stderr, "%s: %s is not a vertex stage of this game's\n", faultsPath.c_str(), what.c_str()); return 1; }
    }
    for (auto const& [id, pass] : faultyPasses) {
        auto const& found = stagesOfPass[pass];
        if (found.first.empty() || found.second.empty()) { fprintf(stderr, "%s: %s is not a pass of this game's with two stages\n", faultsPath.c_str(), pass.c_str()); return 1; }
    }
    using SemanticKey = std::pair<std::string, uint32_t>;
    auto attributePairs = Pair<SemanticKey>(semantics, macAttributeUses, macAttributeIndex, notes, [](SemanticKey const& k) {
        return k.first + std::to_string(k.second);
    });
    for (auto const& [semantic, paired] : attributePairs) {
        bool inferredOnly = paired.second;
        if (inferredOnly && proven[paired.first] >= kEnough) {
            inferredOnly = false;
            notes.push_back(paired.first + ": paired by order within its group; " + std::to_string(proven[paired.first]) + " passes draw the same and depend on it");
        }
        table.Attributes.push_back({ semantic.first, semantic.second, paired.first, inferredOnly });
    }

    printf("// The interface table, shipped data: what the engine gives a Windows shader at each Vulkan\n"
           "// binding and vertex semantic, by the name the Mac game has for it. Derived from every pass\n"
           "// both platforms ship (%u) of %s.\n// Written by `bg3mse-shaders interface`. Do not edit.\n//\n", surveyed, build.c_str());
    for (auto const& note : notes) printf("// %s\n", note.c_str());
    printf("\n#include <stdafx.h>\n#include <Shaders/ShaderMapping.h>\n\nnamespace bg3se::mac {\n\n"
           "InterfaceTable const& BuiltInInterface()\n{\n    static const InterfaceTable table{\n        %s,\n        {\n", Quoted(build).c_str());
    for (auto const& s : table.Slots) {
        printf("            { %u, %u, ResourceKind::%s, %s, %s, %s },\n", s.Set, s.Binding,
            s.Kind == ResourceKind::Buffer ? "Buffer" : s.Kind == ResourceKind::Texture ? "Texture" : "Sampler", Quoted(s.Type).c_str(),
            Quoted(s.Name).c_str(), s.Inferred ? "true" : "false");
    }
    printf("        },\n        {\n");
    for (auto const& b : table.Blocks) {
        printf("            { %s,\n              {", Quoted(b.Name).c_str());
        for (auto const& m : b.Members) printf(" %s,", Quoted(m).c_str());
        printf(" },\n              {");
        for (auto const& t : b.Types) printf(" %s,", Quoted(t).c_str());
        printf(" },\n              {");
        for (uint32_t offset : b.Offsets) printf(" %u,", offset);
        printf(" },\n              {");
        for (auto const& [member, absent] : b.Absent) printf(" { %u, %s },", member, Quoted(absent).c_str());
        printf(" },\n              {");
        for (auto const& [member, other] : b.OtherMeaning) printf(" { %u, %s },", member, Quoted(other).c_str());
        printf(" },\n              {");
        for (uint32_t member : b.Untested) printf(" %u,", member);
        printf(" },\n              {");
        for (auto const& [member, how] : b.Derived) {
            printf(" { %u, { %s, %s, %u, %s } },", member, Quoted(how.From).c_str(), Quoted(how.FromType).c_str(), how.Number, Quoted(how.Read).c_str());
        }
        printf(" } },\n");
    }
    printf("        },\n        {\n");
    for (auto const& a : table.Attributes) {
        printf("            { %s, %u, %s, %s },\n", Quoted(a.Semantic).c_str(), a.Index, Quoted(a.Name).c_str(), a.Inferred ? "true" : "false");
    }
    printf("        },\n        {\n");
    auto bytes = [](std::string const& hash) {
        std::string out = "{";
        for (size_t at = 0; at + 1 < hash.size(); at += 2) out += std::string(" 0x") + hash[at] + hash[at + 1] + ",";
        return out + " }";
    };
    for (auto const& fault : faults) printf("            { %s, %s },\n", Quoted(fault.Id).c_str(), Quoted(fault.What).c_str());
    printf("        },\n        {\n");
    for (auto const& [id, name] : faultyResources) printf("            { %s, %d },\n", Quoted(name).c_str(), faultIndex(id));
    printf("        },\n        {\n");
    std::sort(faultyStages.begin(), faultyStages.end(), [](auto const& a, auto const& b) { return a.second < b.second; });
    for (auto const& [id, stage] : faultyStages) printf("            { %s, %d },\n", bytes(stage).c_str(), faultIndex(id));
    printf("        },\n        {\n");
    std::vector<std::tuple<std::string, std::string, int>> pairs;
    for (auto const& [id, pass] : faultyPasses) pairs.emplace_back(stagesOfPass[pass].first, stagesOfPass[pass].second, faultIndex(id));
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end(), [](auto const& a, auto const& b) { return std::get<0>(a) == std::get<0>(b) && std::get<1>(a) == std::get<1>(b); }), pairs.end());
    for (auto const& [vertex, fragment, fault] : pairs) printf("            { %s, %s, %d },\n", bytes(vertex).c_str(), bytes(fragment).c_str(), fault);
    printf("        },\n        {\n");
    for (auto const& stage : gameStages) printf("            %s,\n", bytes(stage).c_str());
    printf("        },\n");
    if (table.Textures) {
        auto const& t = *table.Textures;
        printf("        TextureTableRewrite{ %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, %u, { %u, %u, %u, %u, %u } },\n", t.Set, t.Binding, t.ProbesSet,
            t.ProbesBinding, t.DiffuseMember, t.SpecularMember, t.LightsSet, t.LightsBinding, t.CookieMember, t.SpecularArrays, t.CookieArrays,
            t.NewSet, t.NewBindings[0], t.NewBindings[1], t.NewBindings[2], t.NewBindings[3], t.NewBindings[4]);
    }
    printf("    };\n    return table;\n}\n\n}\n");

    fprintf(stderr, "%u passes surveyed, %u skipped; %zu slots, %zu blocks, %zu attributes\n", surveyed, skipped, table.Slots.size(),
        table.Blocks.size(), table.Attributes.size());
    for (auto const& note : notes) fprintf(stderr, "  %s\n", note.c_str());
    return 0;
}

// --- verify -------------------------------------------------------------------------

namespace {

// A name as SPIRV-Cross writes it into Metal source, where two underscores in a row are reserved.
std::string AsWritten(std::string name)
{
    for (size_t at; (at = name.find("__")) != std::string::npos;) name.erase(at, 1);
    return name;
}

// Whether the translated functions take everything where the game's own
// functions for the pass take it. What the material declares is where its
// .lsf says, on either platform: its index is not compared, and `extra`
// counts the textures and samplers the game's function does without.
// A thing of the material's that its .lsf does not name is named after its place in a translated function
// (PlaceShader); the game's own function has it at that place under the name its shader gave it.
bool NamedAfterItsPlace(MacResource const& r)
{
    return r.Name.starts_with("bg3mse_texture_at_") || r.Name.starts_with("bg3mse_sampler_at_");
}

std::string CompareInterfaces(MacPass const& ours, MacPass const& game, std::span<MaterialSlot const> material, uint32_t& extra)
{
    for (auto const& a : ours.Attributes) {
        auto it = std::find_if(game.Attributes.begin(), game.Attributes.end(), [&](auto const& g) { return g.Name == a.Name; });
        if (it == game.Attributes.end()) return "attribute " + a.Name + " is not one of the game's function";
        if (it->Index != a.Index || it->Type != a.Type) return "attribute " + a.Name + " differs in index or type";
    }
    for (int s = 0; s < 2; s++) {
        auto const& mine = s ? ours.Fragment : ours.Vertex;
        auto const& theirs = s ? game.Fragment : game.Vertex;
        for (auto const& r : mine) {
            if (!r.Used) continue;
            bool declared = r.Kind != ResourceKind::Buffer
                && std::any_of(material.begin(), material.end(), [&](auto const& m) { return AsWritten(m.Name) == r.Name; });
            auto it = std::find_if(theirs.begin(), theirs.end(), [&](auto const& g) {
                return g.Kind == r.Kind && (NamedAfterItsPlace(r) ? g.Index == r.Index : AsWritten(g.Name) == r.Name);
            });
            if (it == theirs.end()) {
                if (declared) { extra++; continue; }
                return r.Name + " is not an argument of the game's function";
            }
            if (!declared && it->Index != r.Index) return r.Name + " is at " + std::to_string(r.Index) + ", the game's at " + std::to_string(it->Index);
            if (r.Kind == ResourceKind::Texture && it->Type != r.Type) return r.Name + " is " + r.Type + ", the game's " + it->Type;
            // What the game makes the texture for: one it only writes to need not be readable.
            if (r.Kind == ResourceKind::Texture && (it->Written != r.Written || it->Read != r.Read)) {
                return r.Name + " is not read or written to as the game's function does";
            }
            if (r.Kind != ResourceKind::Buffer) continue;
            for (auto const& m : r.Members) {
                auto member = std::find_if(it->Members.begin(), it->Members.end(), [&](auto const& g) { return g.Name == m.Name; });
                if (member == it->Members.end()) continue;      // not named after the game's: the material's own block
                if (member->Offset != m.Offset || member->Type != m.Type) return r.Name + "." + m.Name + " differs in offset or type";
            }
        }
    }
    return {};
}

}

namespace {

uint32_t NameNoise(std::string const& name, uint32_t seed)
{
    uint32_t h = 2166136261u ^ seed;
    for (char c : AsWritten(name)) h = (h ^ (uint8_t)c) * 16777619u;
    return h;
}

// One of the material's two blocks as the Windows shader has it laid out,
// from the block as the Mac game has it: every value moved from its Mac
// offset to its Windows offset. `engine` picks the pass description's
// values (CB_EngineParams), else the parameters (CB_MaterialConstants).
// Nothing when the two materials do not list the same values.
std::optional<std::vector<uint8_t>> AsWindowsBlock(Pass const& pass, std::vector<uint8_t> const& macBytes, bool engine)
{
    // Where the block has each value: as the served .lsf says when the pass is drawn as served.
    auto const& windowsOffsets = pass.Served ? pass.ServedOffsets : pass.WindowsOffsets;
    struct Value { int64_t Mac, Windows; };
    std::vector<Value> values;
    int64_t macSize = -1, windowsSize = -1;
    auto isEngine = [](std::string const& key) { return key.starts_with("ShaderDesc_") || key.find('.') != std::string::npos; };
    for (auto const& [key, macOffset] : pass.MacOffsets) {
        if (isEngine(key) != engine) continue;
        auto windows = windowsOffsets.find(key);
        if (key.ends_with(engine ? ".EngineCBSize" : ".MaterialCBSize")) continue;
        if (key.ends_with(".EngineCBSize") || key.ends_with(".MaterialCBSize") || key.ends_with(".EngineParamFlags")) continue;
        if (windows == windowsOffsets.end()) return {};
        if ((macOffset < 0) != (windows->second < 0)) return {};
        if (macOffset >= 0) values.push_back({ macOffset, windows->second });
    }
    for (auto const& [key, offset] : windowsOffsets) {
        if (isEngine(key) == engine && !pass.MacOffsets.contains(key)) return {};
    }
    for (auto const* offsets : { &pass.MacOffsets, &windowsOffsets }) {
        for (auto const& [key, value] : *offsets) {
            if (key.ends_with(engine ? ".EngineCBSize" : ".MaterialCBSize")) (offsets == &pass.MacOffsets ? macSize : windowsSize) = value;
        }
    }
    if (macSize < 0 || windowsSize < 0) return {};

    std::vector<uint8_t> out((size_t)windowsSize + 32);
    for (auto const& value : values) {
        // A value reaches to the next one of its block, on each platform.
        int64_t macEnd = macSize, windowsEnd = windowsSize;
        for (auto const& other : values) {
            if (other.Mac > value.Mac) macEnd = std::min(macEnd, other.Mac);
            if (other.Windows > value.Windows) windowsEnd = std::min(windowsEnd, other.Windows);
        }
        int64_t size = std::min(macEnd - value.Mac, windowsEnd - value.Windows);
        if (size <= 0 || value.Mac + size > (int64_t)macBytes.size() || value.Windows + size > (int64_t)out.size()) return {};
        memcpy(out.data() + value.Windows, macBytes.data() + value.Mac, (size_t)size);
    }
    return out;
}

// The sun's shadow. The Mac block of the lighting constants has other members for it than the Windows shaders
// read, and a translated shader works the Windows ones out from them, which is right for what
// ls::SetupForwardLightingConstants writes and for nothing else. So the block is made the way that function makes it,
// from numbers of chance for the fade, the far planes, the widths and the count of cascades, and both pairs of
// functions are given the same bytes. What a pass is judged by:
//   - shadows on with one to four cascades and shadows off; the draw a pass is judged by first has two cascades,
//     and the one with whole numbers set has three, so that the cascade after the first is asked in both;
//   - never shadows on with no cascade: the game's own function takes its first cascade before it looks at the
//     count, so what it draws then is not something to be the same as;
//   - a bias of nothing. The game's own function adds the SQUARE of a cascade's bias to the depth it compares,
//     where the Windows shader adds the bias, and a translated shader is to do what the Windows shader does: with
//     a bias the two are meant to differ, and with none both add nothing.
// BG3MSE_SHADERS_SUN chooses otherwise:
//   states       also draws the pair in each of the eight states and says for each whether it is the same
//   bias         a bias, and a large one (the game's own are far smaller, and change no pixel of a picture of
//                chance), and the states: what the two ways of adding it differ by
//   on-none      shadows on with no cascade wherever they are on, and the states
//   shadows-off  off in every draw, for what a pair differs by with no shadow in it at all, and the states
//   chance       numbers of chance in every member, as before any of this
std::string SunShadowDraws()
{
    static const std::string how = getenv("BG3MSE_SHADERS_SUN") ? getenv("BG3MSE_SHADERS_SUN") : "";
    return how;
}

int64_t MemberAt(MacResource const& block, char const* name)
{
    for (auto const& m : block.Members) if (m.Name == name) return m.Offset;
    return -1;
}

// The game's block for one draw, the shadow members as the engine's function writes them. `kind` is the draw's turn
// among the eight: the count of cascades and whether shadows are on go by it.
void FillSunShadowAsTheEngineDoes(MacResource const& block, uint32_t kind, uint64_t state, std::vector<uint8_t>& bytes)
{
    auto unit = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return (float)((state >> 40) & 0xffff) / 65536.0f;
    };
    auto put = [&](int64_t at, float value) { if (at >= 0 && (size_t)at + 4 <= bytes.size()) memcpy(bytes.data() + at, &value, 4); };
    int64_t enabledAt = MemberAt(block, "g_ShadowEnabled"), fadeAt = MemberAt(block, "g_ShadowFadeAmount"), farAt = MemberAt(block, "g_ShadowFarPlanes"),
        widthsAt = MemberAt(block, "g_ShadowInvFrustumWidths"), matricesAt = MemberAt(block, "g_ShadowViewProjMatrices"),
        fadeSizeAt = MemberAt(block, "g_ShadowInvFadeWorldSize"), biasAt = MemberAt(block, "g_ShadowCascadeBias"),
        radiusAt = MemberAt(block, "g_ShadowMinFilterRadius"), countAt = MemberAt(block, "g_NumActiveCascades");
    static constexpr uint32_t kCascades[8] = { 2, 1, 3, 4, 3, 0, 4, 2 };
    bool on = kind != 6 && SunShadowDraws() != "shadows-off";
    uint32_t count = SunShadowDraws() == "on-none" ? 0 : kCascades[(kind - 1) % 8];
    // Of a size at which each of them shows: a fade that is neither all nor nothing over what is drawn, and a bias
    // that moves the depth compared by a part of the map's range (the game's own are far smaller, and with those a
    // wrong bias or width changes no pixel of a picture of chance).
    float fade = 1.0f + 3.0f * unit(), resolution = unit() < 0.5f ? 1024.0f : 2048.0f;
    float far[4], width[4];
    far[0] = 5.0f + 10.0f * unit();
    width[0] = 5.0f + 10.0f * unit();
    float bias = far[0] * (0.05f + 0.25f * unit());
    if (SunShadowDraws() != "bias") bias = 0.0f;
    for (int i = 1; i < 4; i++) {
        far[i] = far[i - 1] * (1.5f + 1.5f * unit());
        width[i] = width[i - 1] * (1.5f + 1.5f * unit());
    }
    // A cascade's matrix puts what is drawn into its map, each cascade a wider stretch of it than the one before:
    // the first only some of what is drawn, so that the next is asked as well.
    for (int i = 0; i < 4 && matricesAt >= 0; i++) {
        float scale = 0.5f / (float)(1 << i) * (0.8f + 0.4f * unit());
        float matrix[16] = { scale, 0, 0, 0,  0, scale, 0, 0,  0, 0, scale, 0,  0.5f, 0.5f, 0.5f, 1.0f };
        if ((size_t)matricesAt + 64 * (i + 1) <= bytes.size()) memcpy(bytes.data() + matricesAt + 64 * i, matrix, 64);
    }
    put(enabledAt, on ? 1.0f : 0.0f);
    uint32_t written = on ? count : 0;
    if (countAt >= 0 && (size_t)countAt + 4 <= bytes.size()) memcpy(bytes.data() + countAt, &written, 4);
    if (!on) {
        // The other branch of the engine's function: a hundred-thousandth everywhere.
        put(fadeAt, 1e-5f);
        for (int i = 0; i < 4; i++) {
            put(farAt < 0 ? -1 : farAt + 4 * i, 1e-5f);
            put(widthsAt < 0 ? -1 : widthsAt + 4 * i, 1e-5f);
            put(fadeSizeAt < 0 ? -1 : fadeSizeAt + 4 * i, 1e-5f);
            put(biasAt < 0 ? -1 : biasAt + 4 * i, 1e-5f);
            put(radiusAt < 0 ? -1 : radiusAt + 4 * i, 1e-5f);
        }
        return;
    }
    put(fadeAt, 1.0f / fade);
    for (int i = 0; i < 4; i++) {
        put(farAt < 0 ? -1 : farAt + 4 * i, far[i]);
        put(widthsAt < 0 ? -1 : widthsAt + 4 * i, 1.0f / width[i]);
    }
    // Only the cascades there are; the rest of the three vectors stays what it was (numbers of chance), as the
    // engine leaves it.
    for (uint32_t i = 0; i < count; i++) {
        float ratio = width[i] / width[0];
        put(fadeSizeAt < 0 ? -1 : fadeSizeAt + 4 * i, far[i] / (fade * (width[i] * 0.5f)));
        put(biasAt < 0 ? -1 : biasAt + 4 * i, (bias / far[i]) * ((far[i] / far[0] - ratio) * 0.75f + ratio));
        put(radiusAt < 0 ? -1 : radiusAt + 4 * i, (2.0f / ratio) / resolution);
    }
}

// The lights of a cluster. A forward pass finds its lights through ClusterInfoBuffer: the picture is cut into tiles
// and the depth into slices, and the entry of the cluster a pixel is in says where its lights' indices begin in
// ClusterIndexListsBuffer and how many there are of each kind (read from the game's own functions: x the beginning;
// y two counts of 12 bits, then one of 8 for the light probes; z a count of 10 bits; the stretches of indices
// follow each other from x as first count, second count, z's count, the probes'). With numbers of chance a tile has
// no size and a cluster no light, and nothing of the lighting buffers shows in any picture. So:
//   - the grid is one the engine could have: tiles of 16 pixels, four by four of them, four slices;
//   - every cluster has two lights of the first kind, one of the second, one of the third and one probe, and
//     the indices are 0 1 2 3 0 1 2 3 ...;
//   - every buffer the table has as an array and the game's function describes by one element's members holds
//     eight elements, each with numbers of its own: an index that is not zero reads something that is bound;
//   - a light's channels are all of them (a member named ...ChannelFlags), or the object's channel would have to
//     be the light's by chance.
//   - a probe's cubes and a light's cookie are layers of the arrays the Mac game keeps them in, packed into the
//     probe's and the light's whole numbers the way ls::ClusteredAssignmentStage packs them (ProbeShapeType_Diff,
//     Spec, LightCookiePreExpose), other layers and arrays in other draws;
//   - a light casts a shadow and has shadow data of its own (a member named ShadowDataIndex...: the lowest bit, and
//     the index above the two lowest), or the shadow data would show in no picture;
//   - a light is strong, reaches everything drawn (a point light's reach, a spot's cone, a direction light's box)
//     and a direction light comes from somewhere and fades one of its four ways; a light's shadow has a tile of
//     the shadow map and a depth the map can hold. Each is said where it is written, with what the game's own
//     function does with it.
// Draws two and four have no light in any cluster, as before. BG3MSE_SHADERS_NO_LIGHTS: none in any draw.
// Eight elements, of which the lists name the first four: a point light's shadow data is the element of its own
// number and the three after it (one for each face of its tetrahedron).
constexpr uint32_t kElementsBound = 8, kLightsListed = 4;

bool ClustersHaveLights(uint32_t kind)
{
    return kind != 2 && kind != 4 && !getenv("BG3MSE_SHADERS_NO_LIGHTS");
}

// For looking into one pass: BG3MSE_SHADERS_LIGHTS names what a cluster has, by letters: p two point lights, s a
// spot, d a direction light, r a probe, w the lights cast shadows. Unset: all of it ("psdrw").
bool ClustersHave(char what)
{
    auto asked = getenv("BG3MSE_SHADERS_LIGHTS");
    return !asked || strchr(asked, what);
}

void GiveClustersLights(MacResource const& r, uint32_t kind, uint64_t state, std::vector<uint8_t>& bytes)
{
    auto putWhole = [&](size_t at, uint32_t value) { if (at + 4 <= bytes.size()) memcpy(bytes.data() + at, &value, 4); };
    if (r.Name == "ForwardLightingConstants") {
        struct Grid { char const* Name; uint32_t Value; };
        static constexpr Grid kGrid[] = { { "g_ClusteredTileSize", 16 }, { "g_ClusteredGridSizeX", 4 }, { "g_ClusteredGridSizeY", 4 }, { "g_ClusteredLastSlice", 3 } };
        for (auto const& grid : kGrid) {
            for (auto const& m : r.Members) {
                if (m.Name != grid.Name || m.Type.empty()) continue;
                float number = (float)grid.Value;
                if (m.Type[0] == 'f' && m.Offset + 4 <= bytes.size()) memcpy(bytes.data() + m.Offset, &number, 4);
                else putWhole(m.Offset, grid.Value);
            }
        }
        // One over how many texels across the lights' shadow map is: the shadow's filter steps by it.
        for (auto const& m : r.Members) {
            if (m.Name != "g_ShadowTileMapSizeInv") continue;
            float step = 1.0f / (float)DrawSetup::kTextureAcross;
            for (size_t at = m.Offset; at < (size_t)m.Offset + 8 && at + 4 <= bytes.size(); at += 4) memcpy(bytes.data() + at, &step, 4);
        }
        return;
    }
    if (r.Name == "ClusterInfoBuffer") {
        if (!ClustersHaveLights(kind)) return;
        for (size_t at = 0; at + 16 <= bytes.size(); at += 16) {
            putWhole(at, 0);
            putWhole(at + 4, (ClustersHave('p') ? 2u : 0u) | ((ClustersHave('s') ? 1u : 0u) << 12) | ((ClustersHave('r') ? 1u : 0u) << 24));
            putWhole(at + 8, ClustersHave('d') ? 1 : 0);
            putWhole(at + 12, 0);
        }
        return;
    }
    if (r.Name == "ClusterIndexListsBuffer") {
        for (size_t at = 0; at + 4 <= bytes.size(); at += 4) putWhole(at, (uint32_t)(at / 4) % kLightsListed);
        return;
    }
    // An array the game's function describes by one element.
    bool array = false;
    for (auto const& slot : BuiltInInterface().Slots) array = array || (slot.Kind == ResourceKind::Buffer && slot.Type == "array" && slot.Name == r.Name);
    if (!array || r.Type != "block" || !r.Size || r.Members.empty() || r.Name == "VirtualTexture_TilesetDataBuffer" || r.Name == "DiffusionProfileBuffer") return;
    bytes.resize(((size_t)r.Size * kElementsBound + 15) & ~(size_t)15);
    auto unit = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return 0.1f + 0.9f * (float)((state >> 40) & 0xffff) / 65536.0f;
    };
    for (uint32_t element = 0; element < kElementsBound; element++) {
        float fadeNear = 0.5f, fadeFar = 0.8f;
        for (size_t m = 0; m < r.Members.size(); m++) {
            auto const& member = r.Members[m];
            if (member.Type.empty()) continue;
            size_t begin = (size_t)r.Size * element + member.Offset, end = (size_t)r.Size * element + (m + 1 < r.Members.size() ? r.Members[m + 1].Offset : r.Size);
            bool whole = member.Type[0] == 'i' || member.Type[0] == 'u';
            for (size_t at = begin; at + 4 <= end && at + 4 <= bytes.size(); at += 4) {
                state = state * 6364136223846793005ull + 1442695040888963407ull;
                uint32_t chance = (uint32_t)(state >> 33);
                // A layer of four, as the engine writes it (one more than the layer; nothing is "none").
                auto layer = [&](uint32_t shift, bool mayBeNone) { return (mayBeNone && (chance >> (shift + 7)) % 3 == 0 ? 0u : 1u + ((chance >> shift) & 3)); };
                if (whole && member.Name == "ProbeShapeType_Diff") {
                    // A ball or a box; it has diffuse and specular cubes; the two diffuse layers, the second one there or not.
                    putWhole(at, (chance & 1) | 2u | 4u | (layer(1, false) << 3) | (layer(3, true) << 12));
                } else if (whole && member.Name == "Spec") {
                    // For each of the two: which of the four arrays, and the layer.
                    putWhole(at, ((chance >> 5) & 3) | (layer(7, false) << 2) | (((chance >> 9) & 3) << 11) | (layer(11, true) << 13));
                } else if (whole && member.Name == "LightCookiePreExpose") {
                    // Pre-exposed or not; which of the six arrays; the layer, or no cookie.
                    putWhole(at, ((chance >> 13) & 1) | (((chance >> 14) % 6) << 1) | (layer(17, true) << 4));
                } else if (whole && member.Name.find("ChannelFlags") != std::string::npos) {
                    // Every channel; above them a direction light keeps which of its four ways of fading it uses.
                    putWhole(at, 0x00ffffff | (member.Name.find("AttenuationFunction") != std::string::npos ? (element & 3) << 24 : 0));
                } else if (whole && member.Name.find("ShadowDataIndex") != std::string::npos) {
                    // It casts a shadow (the lowest bit), and its shadow's data is the element of its own number.
                    putWhole(at, (element << 2) | (ClustersHave('w') ? 1u : 0u));
                } else if (member.Type[0] == 'f' && (member.Name == "WorldOffsetRadius" || member.Name == "SizeTransition")) {
                    // A light reaches, and a probe's box holds, everything that is drawn: with a reach or a size
                    // of chance nothing drawn is inside either and neither shows in any picture. A light's fourth
                    // number is one over its reach (the game's function lets a point in when its distance squared
                    // is below one over that number squared); a probe's first three are half its box.
                    bool light = member.Name == "WorldOffsetRadius", reach = light ? at == begin + 12 : at < begin + 12;
                    float value = !reach ? unit() : light ? 1.0f / (1000.0f + 1000.0f * unit()) : 1000.0f + 1000.0f * unit();
                    memcpy(bytes.data() + at, &value, 4);
                } else if (member.Type[0] == 'f' && (member.Name == "ColorOuterAngle" || member.Name == "Color")) {
                    // A light's colour is its colour times how strong it is: with a colour below one, what a
                    // light adds is below what two pictures may be apart. A spot's cone holds everything drawn
                    // (the fourth number: the cosine of its outer angle).
                    float value = member.Name == "ColorOuterAngle" && at == begin + 12 ? -1.0f + 0.2f * unit() : 20.0f + 80.0f * unit();
                    memcpy(bytes.data() + at, &value, 4);
                } else if (member.Type[0] == 'f' && member.Name == "DirectionInnerAngle" && at == begin + 12) {
                    float value = -0.5f + 0.5f * unit();        // the cosine of a spot's inner angle, above its outer one
                    memcpy(bytes.data() + at, &value, 4);
                } else if (member.Type[0] == 'f' && member.Name == "WorldOffsetToLight") {
                    // A direction light lights what is in its box: the matrix takes a point into it, and in it
                    // is between nought and one each way. So that everything drawn is: a small step each way
                    // from the middle. Three rows of four.
                    float value = (at - begin) % 16 == 12 ? 0.5f : (unit() - 0.55f) / 64.0f;
                    memcpy(bytes.data() + at, &value, 4);
                } else if (member.Type[0] == 'f' && member.Name == "Direction") {
                    // Where the light comes from, of length one.
                    static constexpr float kDirections[4][3] = { { 0.6f, 0.64f, 0.48f }, { -0.36f, 0.8f, 0.48f }, { 0.28f, 0.96f, 0.0f }, { 0.0f, 0.6f, -0.8f } };
                    size_t which = (at - begin) / 4;
                    if (which < 3) memcpy(bytes.data() + at, &kDirections[element & 3][which], 4);
                } else if (member.Type[0] == 'f' && (member.Name == "AttenuationParamsZ" || member.Name == "AttenuationParamsXY")) {
                    // How a direction light fades towards the ends and the sides of its box. Along it: full
                    // between a near and a far place, and the two numbers that bring it to nothing at the ends
                    // (one over the near place, one over what is left after the far one). Across it: how fast
                    // it rises from the sides. The near place is about the middle, where what is drawn is, so
                    // that some of it is in the fading part and some is not.
                    size_t which = (at - begin) / 4;
                    float value = unit();
                    if (member.Name == "AttenuationParamsXY") value = 1.5f + 2.0f * value;
                    else if (which == 0) value = fadeNear = 0.45f + 0.1f * value;
                    else if (which == 1) value = fadeFar = 0.7f + 0.2f * value;
                    else value = which == 2 ? 1.0f / fadeNear : 1.0f / (1.0f - fadeFar);
                    memcpy(bytes.data() + at, &value, 4);
                } else if (r.Name == "ShadowDataBuffer" && member.Type[0] == 'f') {
                    // A light's shadow: a tile of the shadow map (a quarter of it, another for each element),
                    // a matrix that takes what is drawn into that tile at a depth of about 0.7, and the three
                    // numbers a depth is made of, so that the depth a point light, a spot and a direction
                    // light each compare with what the map holds is between nought and one (for the first
                    // two: the second over the third times what the depth is above the first; for the last:
                    // what the depth is above the second, over the first times the third). With numbers of
                    // chance the place is outside the map or the depth beyond anything it holds, and the map
                    // shows in no picture.
                    float tile[2] = { 0.5f * (float)(element & 1), 0.5f * (float)((element >> 1) & 1) };
                    size_t which = (at - begin) / 4;
                    float value = unit();
                    if (member.Name == "ShadowMatrix") {
                        size_t row = which / 4, column = which % 4;
                        if (row == 3) value = column == 3 ? 1.0f : 0.0f;
                        else if (column < 3) value = (value - 0.55f) / 64.0f;
                        else value = row == 0 ? 2.0f * (tile[0] + 0.25f) - 1.0f : row == 1 ? 1.0f - 2.0f * (tile[1] + 0.25f) : 0.7f;
                    } else if (member.Name == "UVOffset") {
                        value = tile[which & 1];
                    } else if (member.Name == "UVScale") {
                        value = 0.5f;
                    } else if (member.Name == "Resolution") {
                        value = 0.5f * (float)DrawSetup::kTextureAcross;
                    } else if (member.Name == "WorldTexelSize") {
                        value = 0.01f * value;
                    } else if (member.Name == "ProjectionInfo") {
                        value = which == 0 ? 0.3f : 0.3f + 0.2f * value;
                    } else if (member.Name == "NormalizationFactor") {
                        value = 2.0f;
                    }
                    memcpy(bytes.data() + at, &value, 4);
                } else if (!element) {
                    continue;       // the first element is as any block's members are
                } else if (whole) {
                    state = state * 6364136223846793005ull + 1442695040888963407ull;
                    putWhole(at, kind > 4 ? (uint32_t)((state >> 40) % 3) : 0);
                } else if (member.Type[0] == 'f') {
                    float value = unit();
                    memcpy(bytes.data() + at, &value, 4);
                }
            }
        }
    }
}

// What the two pairs of functions are given. The game's own description of
// each argument says what to put there (numbers where it takes numbers,
// nothing where it takes counts and offsets); ours get the same under the
// same names, the material's two blocks in their own layout. False when the
// two cannot be given the same.
bool SetupsFor(Pass const& pass, MacPass const& ours, uint32_t seed, DrawSetup& forGame, DrawSetup& forOurs, std::string& why)
{
    std::map<std::string, std::vector<uint8_t>> buffers;
    forGame = {};
    forOurs = {};
    // BG3MSE_SHADERS_SEEDS=n draws with another set of numbers: what a draw is (whole numbers set or not, a mask or
    // noise) stays by its turn among the eight, the numbers themselves are others.
    const uint32_t kind = seed;
    if (char const* set = getenv("BG3MSE_SHADERS_SEEDS")) seed += 8 * (uint32_t)atoi(set);
    forGame.Seed = forOurs.Seed = seed;

    auto fill = [&](DrawSetup& setup, MacPass const& side, bool game) {
        // Drawn as served, a translated function is given what the game would give it, where the game would: the
        // vertex data in the layout the game has for the pass (its own function's), what the material declares at
        // the index the served .lsf has for the stage (none when it says the stage takes none), and what the
        // engine supplies where the game's function for the pass takes it. A function that takes something
        // elsewhere then reads what is not there, and the pictures show it.
        bool asServed = !game && pass.Served;
        auto where = [&](MacResource const& r, bool vertex) -> int64_t {
            if (!asServed) return r.Index;
            for (auto const& slot : pass.ServedSlots) {
                if (AsWritten(slot.Name) == r.Name || slot.Name == r.Name) return vertex ? slot.DxVs : slot.DxPs;
            }
            for (auto const& g : vertex ? pass.Mac.Vertex : pass.Mac.Fragment) {
                if (g.Kind == r.Kind && AsWritten(g.Name) == AsWritten(r.Name)) return g.Index;
            }
            return r.Index;
        };
        for (auto const& a : asServed ? pass.Mac.Attributes : side.Attributes) setup.Attributes.push_back({ a.Index, a.Type.empty() ? 'f' : a.Type[0] });
        // BG3MSE_SHADERS_RIGID: an instance's matrix is a turn, one scale and a move (see DrawSetup::MatrixRows).
        if (getenv("BG3MSE_SHADERS_RIGID")) {
            for (auto const& a : pass.Mac.Attributes) {
                for (int row = 0; row < 3; row++) if (a.Name == "InstanceMatrix" + std::to_string(row + 1)) setup.MatrixRows[row] = (int32_t)a.Index;
            }
        }
        for (auto const* stage : { &side.Vertex, &side.Fragment }) {
            bool vertex = stage == &side.Vertex;
            for (auto const& found : *stage) {
                if (found.Name.starts_with("vertexBuffer.")) {
                    // The buffer the vertex data is in: the game's layout's when drawn as served.
                    uint32_t buffer = found.Index;
                    for (auto const& g : pass.Mac.Vertex) if (asServed && g.Name.starts_with("vertexBuffer.")) buffer = g.Index;
                    setup.VertexBuffer = buffer;
                    continue;
                }
                int64_t at = where(found, vertex);
                if (at < 0) continue;
                MacResource r = found;
                r.Index = (uint32_t)at;
                // What has no name but its place is given what the game's own function is given there.
                for (auto const& g : vertex ? pass.Mac.Vertex : pass.Mac.Fragment) {
                    if (!game && NamedAfterItsPlace(r) && g.Kind == r.Kind && g.Index == r.Index) { r.Name = g.Name; break; }
                }
                if (r.Kind == ResourceKind::Sampler) { setup.Samplers.push_back({ r.Index, vertex, NameNoise(r.Name, 0) >> 7 }); continue; }
                if (r.Kind == ResourceKind::Texture) {
                    // A virtual texture's page table gives, in the lowest four bits of what is read from it, the power of
                    // two the coordinate is multiplied by before its fraction is taken. With noise there that is up to
                    // 2^15, and the game's function and a translated one then sample values a few percent apart in some
                    // pixels: in none with powers up to 2^6, in more the higher the power. Which powers the game itself
                    // writes there is not known; up to 2^6 is what is given. BG3MSE_SHADERS_PAGETABLE=15 gives any.
                    char const* power = getenv("BG3MSE_SHADERS_PAGETABLE");
                    int lowBits = r.Name == "VirtualTexture_Pagetable" ? (power ? atoi(power) : 6) : -1;
                    // The scene's depth behind what is drawn, as a part of the far plane's: far, as it is behind an
                    // effect in the game, or a soft particle fades to nothing against a scene in front of it.
                    int atLeast = r.Name == "_LinearDepth" ? 200 : 0;
                    // A texture the vertex stage reads is a mask or holds positions. A character's vertices are
                    // drawn where its mask is zero and folded away where it is not, so with noise all over it
                    // nothing is drawn; a model whose vertices are moved by a texture of positions draws nothing
                    // when most of that is zero. Three texels in four are zero in every other draw, and none in
                    // the draws between, so that each kind draws in one of them.
                    // BG3MSE_SHADERS_FLAT, for looking into one pass: what the fragment stage looks up is the same
                    // wherever it looks, so that a picture that differs does not differ for where a texel's edge is.
                    // Several textures at one place (the light probes' specular arrays, the cookies'): each its own,
                    // one index after the other. A light probe's cubes have smaller levels with numbers of their
                    // own: the level one is sampled at then shows.
                    bool several = r.Type.ends_with("[]") && r.Size > 1, levels = r.Name.starts_with("LightProbe");
                    // A texture is what the engine binds, whichever function reads it: where the game's own
                    // function takes one as depths, ours gets the same depths, though it may declare a plain
                    // texture there (the Windows shader gathers from the lights' shadow map without comparing).
                    bool depth = r.Depth;
                    for (auto const* stage : { &pass.Mac.Vertex, &pass.Mac.Fragment }) {
                        for (auto const& g : *stage) depth = depth || (!game && g.Kind == ResourceKind::Texture && g.Depth && AsWritten(g.Name) == AsWritten(r.Name));
                    }
                    // The lights' shadow map holds one depth in each tile. The game's own Mac function and the
                    // Windows shader filter a light's shadow over twelve taps on a spiral, and the spirals are not
                    // the same: the Mac's is a table whose tap i lies (i + 0.5) * 0.2887 from the middle, the
                    // Windows shader works out sqrt(i + 0.5) * 0.2887 (both times 2.5 texels, turned by the same
                    // angle each tap). A translated shader keeps the Windows spiral, so over a map of noise the
                    // two draw a shadow's edge otherwise, and with one depth in a tile they do not. Everything
                    // else of the filter is the same arithmetic: with the Mac's radius put into the translation
                    // the two draw the same over noise. BG3MSE_SHADERS_LIGHT_SHADOWS=noise: a map of noise.
                    auto shadows = getenv("BG3MSE_SHADERS_LIGHT_SHADOWS");
                    bool quarters = r.Name == "ShadowTileMap" && !(shadows && !strcmp(shadows, "noise"));
                    // BG3MSE_SHADERS_FLAT=1: every texture the fragment stage reads; BG3MSE_SHADERS_FLAT=<part of a
                    // name>: only those named so, for finding which lookup it is that two functions make elsewhere.
                    auto flatten = getenv("BG3MSE_SHADERS_FLAT");
                    bool flat = !vertex && flatten && (!strcmp(flatten, "1") || r.Name.find(flatten) != std::string::npos);
                    for (uint32_t element = 0; element < (several ? r.Size : 1); element++) {
                        setup.Textures.push_back({ r.Index + element, several ? r.Type.substr(0, r.Type.size() - 2) : r.Type, depth,
                            NameNoise(r.Name, seed) + 7919 * element, vertex, r.Written, lowBits, atLeast,
                            vertex && kind % 2 == 1, flat, levels, quarters });
                        if (getenv("BG3MSE_SHADERS_SHOW") && several) {
                            printf("        draw %u gives %s %s[%u] at %u, a %s of noise %u\n", seed, game ? "the game's" : "ours", r.Name.c_str(), element,
                                setup.Textures.back().Index, setup.Textures.back().Type.c_str(), setup.Textures.back().Noise);
                        }
                    }
                    continue;
                }

                auto name = AsWritten(r.Name);
                if (game && buffers.contains(name)) {
                    // Both stages take it: the same bytes.
                    setup.Buffers.push_back({ r.Index, buffers[name], vertex });
                    continue;
                }
                if (game) {
                    std::vector<uint8_t> bytes(r.Type == "block" ? ((r.Size + 31) & ~15u) : 65536);
                    // A material's index into the virtual textures' records may be other than zero: four records.
                    if (r.Name == "VirtualTexture_TilesetDataBuffer" && bytes.size() < 512) bytes.resize(512);
                    // And an object's index into the diffusion profiles: four profiles of 64 bytes.
                    if (r.Name == "DiffusionProfileBuffer" && bytes.size() < 256 && !getenv("BG3MSE_SHADERS_ONE_PROFILE")) bytes.resize(256);
                    if (r.Type != "block" && name.starts_with("TransformedVertices")) {
                        // Vertices the engine has moved already (a skinned mesh's), read when the object's offset into
                        // them is not zero: positions as numbers of either sign in the bits of whole numbers.
                        uint64_t state = (uint64_t)NameNoise(r.Name, seed) + 11;
                        for (size_t at = 0; at + 4 <= bytes.size(); at += 4) {
                            state = state * 6364136223846793005ull + 1442695040888963407ull;
                            float value = 2.0f * (float)((state >> 40) & 0xffff) / 65536.0f - 1.0f;
                            memcpy(bytes.data() + at, &value, 4);
                        }
                    } else if (r.Type != "block") {
                        // The game's function takes it as bytes. Where the Windows shader reads numbers from it
                        // (the exposure), it gets numbers; where it reads counts or offsets, nothing.
                        for (auto const* mine : { &ours.Vertex, &ours.Fragment }) {
                            for (auto const& o : *mine) {
                                bool numbers = AsWritten(o.Name) == name && o.Kind == ResourceKind::Buffer && o.Members.size() == 1
                                    && !o.Members[0].Type.empty() && o.Members[0].Type[0] == 'f';
                                if (!numbers) continue;
                                uint64_t state = (uint64_t)NameNoise(r.Name, seed) + 7;
                                for (size_t at = 0; at + 4 <= bytes.size(); at += 4) {
                                    state = state * 6364136223846793005ull + 1442695040888963407ull;
                                    float value = 0.1f + 0.9f * (float)((state >> 40) & 0xffff) / 65536.0f;
                                    memcpy(bytes.data() + at, &value, 4);
                                }
                            }
                        }
                    }
                    uint64_t state = (uint64_t)NameNoise(r.Name, seed) + 1;
                    auto unit = [&] {
                        state = state * 6364136223846793005ull + 1442695040888963407ull;
                        return 0.1f + 0.9f * (float)((state >> 40) & 0xffff) / 65536.0f;
                    };
                    char const* zero = getenv("BG3MSE_SHADERS_ZERO");       // for looking into one pass: a member left at zero
                    for (size_t m = 0; m < r.Members.size(); m++) {
                        if (r.Members[m].Type.empty()) continue;
                        if (zero && r.Members[m].Name == zero) continue;
                        size_t end = m + 1 < r.Members.size() ? r.Members[m + 1].Offset : r.Size;
                        // BG3MSE_SHADERS_FACE=n, for the point light's shadow pass: the face of the tetrahedron that is
                        // drawn is that one in every draw, as the engine sets it for each of the four, not a number of chance.
                        if (char const* face = getenv("BG3MSE_SHADERS_FACE"); face && r.Members[m].Name == "g_TetrahedronFaceIndex"
                            && r.Members[m].Offset + 4 <= bytes.size()) {
                            int32_t value = atoi(face);
                            memcpy(bytes.data() + r.Members[m].Offset, &value, 4);
                            continue;
                        }
                        if (r.Members[m].Type[0] == 'i' || r.Members[m].Type[0] == 'u') {
                            // A whole number is a count, an index or a switch: zero in the first four draws, and
                            // a small one in the others, so that what it selects is drawn as well and an index
                            // stays inside what is bound.
                            for (size_t at = r.Members[m].Offset; kind > 4 && at + 4 <= end && at + 4 <= bytes.size(); at += 4) {
                                state = state * 6364136223846793005ull + 1442695040888963407ull;
                                uint32_t value = (uint32_t)((state >> 40) % 3);
                                memcpy(bytes.data() + at, &value, 4);
                            }
                            continue;
                        }
                        if (r.Members[m].Type[0] != 'f') continue;
                        // An object's own matrix turns and mirrors as an instance's does: of either sign, or a
                        // pass that is not instanced puts all it draws along one direction and draws nothing.
                        // A decal's is the inverse, into the box the decal fills: small, or nothing lands in the box.
                        // What a thing was a frame before is a thing of the same kind.
                        std::string member = r.Members[m].Name;
                        if (member.ends_with("_prev")) member.resize(member.size() - 5);
                        bool eitherSign = member == "WorldOffsetMatrix", small = member == "InverseWorldOffsetMatrix";
                        for (size_t at = r.Members[m].Offset; at + 4 <= end && at + 4 <= bytes.size(); at += 4) {
                            // The object's: near the one that leaves everything where it is, so that what is drawn
                            // does not hang on the chance of one matrix as an instanced pass's does not.
                            size_t element = (at - r.Members[m].Offset) / 4;
                            // The camera's: of either sign. With every number of it above zero all that is drawn
                            // lands along one edge of the picture unless each vertex has a matrix of its own, as
                            // an instanced pass's has; a pass that is not instanced then drew in one draw of eight.
                            // It looks at what is drawn: near a matrix that keeps x and y, puts z between a fifth
                            // and four fifths of the depth there is and leaves w at one.
                            bool camera = member == "g_OffsetViewProjectionMat";
                            static constexpr float kLooksAt[16] = { 0.9f, 0, 0, 0,  0, 0.9f, 0, 0,  0, 0, 0.3f, 0,  0, 0, 0.5f, 1.0f };
                            // And it turns, scales and moves, no more: its last row is 0 0 0 1, which a function
                            // may count on (one of the game's takes the length of a whole column where the
                            // Windows shader takes that of its first three numbers).
                            // BG3MSE_SHADERS_CHANCE_ROW, for showing what that does to a stage: the last row is numbers
                            // of chance like the rest, as it was before.
                            if (eitherSign && element % 4 == 3 && !getenv("BG3MSE_SHADERS_CHANCE_ROW")) {
                                float last = element == 15 ? 1.0f : 0.0f;
                                unit();
                                memcpy(bytes.data() + at, &last, 4);
                                continue;
                            }
                            float value = eitherSign ? (element % 5 == 0 ? 1.0f : 0.0f) + 0.8f * unit() - 0.45f
                                : small ? 0.2f * unit() - 0.11f : camera ? kLooksAt[element % 16] + 0.2f * unit() - 0.11f : unit();
                            memcpy(bytes.data() + at, &value, 4);
                        }
                        // A decal's NormalMatrix as ls::DecalObject::PrepareDraw writes it: the first three numbers of
                        // each of its first three vectors made a vector of length one (nothing, when they are next to
                        // nothing), every fourth number 0, and 0 0 0 1 last. One of the game's functions makes the
                        // second vector's length one from all four numbers where the Windows shader takes three, which
                        // is the same thing only with that 0. BG3MSE_SHADERS_CHANCE_ROW: sixteen numbers of chance, as before.
                        if (member == "NormalMatrix" && r.Members[m].Type == "f16" && r.Members[m].Offset + 64 <= bytes.size()
                            && !getenv("BG3MSE_SHADERS_CHANCE_ROW")) {
                            float matrix[16];
                            memcpy(matrix, bytes.data() + r.Members[m].Offset, 64);
                            for (int v = 0; v < 3; v++) {
                                float* at = matrix + 4 * v;
                                float length = sqrtf(at[0] * at[0] + at[1] * at[1] + at[2] * at[2]);
                                for (int i = 0; i < 3; i++) at[i] = length > 1e-7f ? at[i] / length : 0.0f;
                                at[3] = 0.0f;
                            }
                            matrix[12] = matrix[13] = matrix[14] = 0.0f;
                            matrix[15] = 1.0f;
                            memcpy(bytes.data() + r.Members[m].Offset, matrix, 64);
                        }
                        if (eitherSign && game && getenv("BG3MSE_SHADERS_SHOW")) {
                            // For looking into one pass: the object's matrix as it is given, sixteen numbers in the order they lie.
                            printf("        draw %u is given %s:", seed, r.Members[m].Name.c_str());
                            for (size_t at = r.Members[m].Offset; at + 4 <= end && at + 4 <= bytes.size(); at += 4) {
                                float value;
                                memcpy(&value, bytes.data() + at, 4);
                                printf(" %.6g", value);
                            }
                            printf("\n");
                        }
                    }
                    // The cloud and coverage maps are looked up where a ray from the surface meets a sphere whose
                    // radius is the third of four parameters: above the scene, or the root in the lookup is of a
                    // negative number and nothing depends on the maps.
                    for (auto const& m : r.Members) {
                        if (m.Type != "f4" || !m.Name.ends_with("CoverageParams") || m.Offset + 16 > bytes.size()) continue;
                        float radius = 1000.0f;
                        memcpy(bytes.data() + m.Offset + 8, &radius, 4);
                    }
                    // A virtual texture's record says it is one by the number
                    // 2202 in its first matrix (the Windows shaders draw red
                    // without it): the draw is of a material whose texture is there.
                    if (r.Name == "VirtualTexture_TilesetDataBuffer" && bytes.size() >= 60) {
                        float valid = 2202.0f;
                        memcpy(bytes.data() + 56, &valid, 4);
                        // The records after the first, which a material's index into them may name: numbers of
                        // their own, and valid as well. A record is two matrices.
                        for (size_t record = 128; record + 128 <= bytes.size() && record < 128 * 4; record += 128) {
                            for (size_t at = record; at < record + 128; at += 4) {
                                float value = unit();
                                memcpy(bytes.data() + at, &value, 4);
                            }
                            memcpy(bytes.data() + record + 56, &valid, 4);
                        }
                    }
                    // The diffusion profiles after the first, which an object's index into them may name: numbers
                    // of their own, where there was nothing bound at all and both functions read whatever lay there.
                    // BG3MSE_SHADERS_ONE_PROFILE: as before, one profile and nothing after it.
                    if (r.Name == "DiffusionProfileBuffer" && !getenv("BG3MSE_SHADERS_ONE_PROFILE")) {
                        for (size_t at = 64; at + 4 <= bytes.size() && at < 256; at += 4) {
                            float value = unit();
                            memcpy(bytes.data() + at, &value, 4);
                        }
                    }
                    if (r.Name == "ForwardLightingConstants" && SunShadowDraws() != "chance") {
                        FillSunShadowAsTheEngineDoes(r, kind, (uint64_t)NameNoise(r.Name, seed) + 99, bytes);
                    }
                    GiveClustersLights(r, kind, (uint64_t)NameNoise(r.Name, seed) + 131, bytes);
                    // A material's own parameters are what the material sets them to in every other draw: a
                    // threshold it tests against, a fade, a count of frames are then like the game's, and a
                    // pass that draws nothing with numbers of chance draws.
                    if (r.Name == "CB_MaterialConstants" && kind % 2 == 1) {
                        for (auto const& [uniform, floats] : pass.MacValues) {
                            auto at = pass.MacOffsets.find(uniform);
                            if (at == pass.MacOffsets.end() || at->second < 0 || (size_t)at->second + floats.size() * 4 > bytes.size()) continue;
                            memcpy(bytes.data() + at->second, floats.data(), floats.size() * 4);
                        }
                    }
                    buffers[name] = bytes;
                    setup.Buffers.push_back({ r.Index, std::move(bytes), vertex });
                    continue;
                }
                auto have = buffers.find(name);
                if (have == buffers.end()) {
                    // The game's functions do not take it: nothing to give ours that means the same.
                    why = r.Name + " is taken by our functions only";
                    return false;
                }
                bool engine = r.Name == "CB_EngineParams";
                if (engine || r.Name == "CB_MaterialConstants") {
                    auto converted = AsWindowsBlock(pass, have->second, engine);
                    if (!converted) {
                        why = "the two platforms' materials do not list the same values for " + r.Name;
                        return false;
                    }
                    setup.Buffers.push_back({ r.Index, std::move(*converted), vertex });
                } else {
                    setup.Buffers.push_back({ r.Index, have->second, vertex });
                }
            }
        }
        return true;
    };
    return fill(forGame, pass.Mac, true) && fill(forOurs, ours, false);
}

struct Pictures
{
    uint32_t Drawn{ 0 }, Different{ 0 };
    uint32_t DepthDifferent{ 0 }, CoverageDifferent{ 0 };       // of Different: in depth; drawn by one pair only
    uint32_t Written{ 0 }, WrittenDifferent{ 0 };               // texels of the textures given that either pair wrote to
    uint32_t DepthBits{ 0 };                                    // pixels both drew whose depths are not the same to the last bit
    uint32_t DepthApart{ 0 };                                   // pixels both drew whose depths are apart: the vertex stage's doing
    float DepthMost{ 0 };                                       // the most the depths of a pixel both drew differ by
    uint32_t OnlyFirst{ 0 }, OnlySecond{ 0 }, BothDrew{ 0 };    // of the pixels: drawn by the first pair only, by the second only, by both
};

// The pixels either pair drew to, and those where the two disagree.
Pictures ComparePictures(DrawResult const& a, DrawResult const& b, float tolerance = 1e-2f)
{
    Pictures out;
    auto close = [](float x, float y, float tolerance) {
        if (std::isnan(x) || std::isnan(y)) return std::isnan(x) && std::isnan(y);
        if (std::isinf(x) || std::isinf(y)) return x == y;
        return std::fabs(x - y) <= tolerance * (1 + std::max(std::fabs(x), std::fabs(y)));
    };
    for (uint32_t p = 0; p < DrawResult::kSize * DrawResult::kSize; p++) {
        bool depthSame = close(a.Depth[p], b.Depth[p], 1e-3f), same = depthSame;
        bool drawnA = a.Depth[p] != 1.0f, drawnB = b.Depth[p] != 1.0f;
        for (uint32_t t = 0; t < DrawResult::kTargets; t++) {
            for (int c = 0; c < 4; c++) {
                float x = a.Color[t][p * 4 + c], y = b.Color[t][p * 4 + c];
                drawnA = drawnA || x != MetalFunctions::kClearColor;
                drawnB = drawnB || y != MetalFunctions::kClearColor;
                same = same && close(x, y, tolerance);
            }
        }
        bool drawn = drawnA || drawnB;
        out.Drawn += drawn;
        out.Different += drawn && !same;
        out.DepthDifferent += drawn && !depthSame;
        out.CoverageDifferent += drawnA != drawnB;
        out.OnlyFirst += drawnA && !drawnB;
        out.OnlySecond += drawnB && !drawnA;
        out.BothDrew += drawnA && drawnB;
        out.DepthBits += drawnA && drawnB && memcmp(&a.Depth[p], &b.Depth[p], 4) != 0;
        out.DepthApart += a.Depth[p] != 1.0f && b.Depth[p] != 1.0f && !depthSame;
        if (a.Depth[p] != 1.0f && b.Depth[p] != 1.0f && !std::isnan(a.Depth[p]) && !std::isnan(b.Depth[p])) out.DepthMost = std::max(out.DepthMost, std::fabs(a.Depth[p] - b.Depth[p]));
    }
    // What either pair wrote into a texture it was given counts as drawn. Only that: a texture is kept by its
    // numbers, and one that both stages are given is made twice, the vertex stage's with most of it zero in every
    // other draw. The one kept is the fragment stage's where that stage is given it and the vertex stage's where
    // it is not, so where only one pair's fragment stage is given it the two kept are not the same texture, though
    // neither was written to.
    for (auto const& [noise, mine] : a.Textures) {
        auto other = b.Textures.find(noise);
        if (other == b.Textures.end() || other->second.After.size() != mine.After.size()) continue;
        auto const& theirs = other->second;
        for (size_t texel = 0; texel + 4 <= mine.After.size(); texel += 4) {
            bool wrote = memcmp(&mine.Before[texel], &mine.After[texel], 4) != 0 || memcmp(&theirs.Before[texel], &theirs.After[texel], 4) != 0;
            bool same = memcmp(&mine.After[texel], &theirs.After[texel], 4) == 0;
            out.Written += wrote;
            out.WrittenDifferent += wrote && !same;
            out.Drawn += wrote;
            out.Different += wrote && !same;
        }
    }
    return out;
}

}

int VerifyCommand(std::string const& windowsPak, std::string const& app, uint32_t every, std::string const& only, bool inferred, bool draw,
    std::string const& reference, std::string const& listPath, std::string const& provePath, std::string const& stagesPath)
{
    // For --stages: per vertex stage, the passes in which it drew the same and those in which it drew elsewhere:
    // a pixel both pairs drew is at another depth. (What only one pair drew may be the fragment stage's doing,
    // which discards by what it works out.)
    std::map<std::string, std::pair<uint32_t, uint32_t>> stageProofs;
    // And draw by draw: in how many of eight draws the stage, on its own and with the game's fragment function, drew
    // enough to compare and drew the same or elsewhere, over the passes that have it.
    struct SeedProof { uint32_t Passes{ 0 }, AloneSame{ 0 }, AloneApart{ 0 }, JoinedSame{ 0 }, JoinedApart{ 0 }, NotJoined{ 0 }, Handed{ 0 }, HandedDiffer{ 0 }; };
    // The draw that writes what a vertex function hands on: passes drawn, those in which everything handed on is the
    // same, the names that differ, and the passes in which one is found at another place.
    uint32_t handedPasses = 0, handedSame = 0;
    std::map<std::string, uint32_t> handedNames, handedStates;
    std::map<std::string, SeedProof> seedProofs;
    std::map<std::string, uint32_t> notJoinedReasons, unreadNames;
    MetalLibraryFile gameLibrary;
    if (!stagesPath.empty() && !gameLibrary.Open(app + "/Contents/Data/Shaders/Metal/Materials.metallib")) {
        fprintf(stderr, "cannot read the game's Materials.metallib as a file\n");
        return 1;
    }
    // For --prove: per inferred name, the passes that draw the same and whose picture depends on it, and those where it does not.
    std::map<std::string, std::pair<uint32_t, uint32_t>> proofs;
    // And per slot of a group of textures that always appear together, the names the pictures say it is.
    std::map<std::pair<uint32_t, uint32_t>, std::map<std::string, uint32_t>> votes;
    std::map<std::string, uint32_t> searched;
    // A mesh is drawn by several passes of its material, and a later one may take only the pixels an earlier one
    // left at exactly its depth. Per material: the first draw's depths of each pass, the game's functions' and ours.
    std::string depthsOf;
    std::vector<std::vector<float>> gameDepths, ourDepths;
    uint32_t depthMaterials = 0, gameApart = 0, oursApart = 0;
    FILE* list = nullptr;
    auto depthsDone = [&] {
        auto apart = [](std::vector<std::vector<float>> const& passes) {
            uint32_t pixels = 0;
            for (size_t p = 1; p < passes.size(); p++) {
                for (size_t i = 0; i < passes[p].size() && i < passes[0].size(); i++) {
                    pixels += passes[p][i] != 1.0f && passes[0][i] != 1.0f && memcmp(&passes[p][i], &passes[0][i], 4) != 0;
                }
            }
            return pixels;
        };
        if (gameDepths.size() > 1) {
            uint32_t game = apart(gameDepths), ours = apart(ourDepths);
            depthMaterials++;
            gameApart += game != 0;
            oursApart += ours != 0;
            if (list && (game || ours)) fprintf(list, "%s\tPASSES\tdepths apart\tthe game's %u pixels, ours %u\n", depthsOf.c_str(), game, ours);
        }
        gameDepths.clear();
        ourDepths.clear();
    };
    Survey survey;
    if (!survey.Open(windowsPak, app, reference)) return 1;
    list = listPath.empty() ? nullptr : fopen(listPath.c_str(), "w");
    InterfaceTable table = BuiltInInterface();
    // The game's own passes are what the faults are found and put right on: every one is drawn, whatever it is known for.
    table.FaultyResources.clear();
    table.FaultyVertexStages.clear();
    table.FaultyPairs.clear();
    if (table.Slots.empty()) {
        fprintf(stderr, "this build has no interface table: run `bg3mse-shaders interface` and rebuild\n");
        return 1;
    }

    // What the Mac game agrees on comes from all of its passes, sampled or not.
    std::vector<Pass> passes;
    MacInterface mac;
    std::string error;
    if (!reference.empty() && !SurveyMacInterface(app, mac, error)) {
        fprintf(stderr, "cannot read the game: %s\n", error.c_str());
        return 1;
    }
    for (size_t i = 0; i < survey.Count(); i++) {
        auto pass = survey.Load(i);
        if (reference.empty() && (pass.Problem.empty() || pass.Problem.rfind("the Mac functions", 0) != 0)) mac.Add(pass.Mac);
        bool wanted = (every <= 1 || i % every == 0) && (only.empty() || pass.Stem.find(only) != std::string::npos);
        if (wanted) passes.push_back(std::move(pass));
        if (i % 2000 == 0) fprintf(stderr, "  read %zu of %zu passes\n", i, survey.Count());
    }

    // The members worked out from the game's own stand here as they would in the game: only when this game's
    // binary fills them the way that was read.
    {
        BridgeProblems problems;
        InterfaceForBundle(app, mac, problems);
        if (!problems.SunShadow.empty()) {
            fprintf(stderr, "the sun's shadow constants are not worked out for this game: %s\n", problems.SunShadow.c_str());
            for (auto& block : table.Blocks) block.Derived.clear();
        }
        if (!problems.TextureTable.empty()) {
            fprintf(stderr, "the table of textures is not bridged for this game: %s\n", problems.TextureTable.c_str());
            table = table.WithoutTextureTable();
        }
    }

    struct Row { uint32_t Passes{ 0 }, Unread{ 0 }, Refused{ 0 }, NotCompiled{ 0 }, Different{ 0 }, Same{ 0 }; };
    std::map<std::string, Row> rows;
    std::map<std::string, std::pair<uint32_t, std::string>> reasons;      // with the first pass it was said of
    std::string current;
    auto note = [&](std::string reason) {
        // One line per kind of reason: the numbers in it are replaced.
        if (reason.find("nothing is known") == std::string::npos) {
            for (auto& c : reason) if (isdigit((unsigned char)c)) c = '#';
        }
        auto& entry = reasons[reason];
        if (!entry.first++) entry.second = current;
    };

    struct PictureRow { uint32_t Same{ 0 }, Different{ 0 }, Empty{ 0 }, NotComparable{ 0 }, NotDrawn{ 0 }; };
    std::map<std::string, PictureRow> pictures;
    // A material is served only when every one of its passes is.
    std::map<std::string, std::pair<uint32_t, uint32_t>> materials;
    uint32_t withExtra = 0;
    size_t done = 0;
    // A pass is drawn as served unless something asks for the stages to be translated another way: what is compiled
    // and what the draw is told are then what TranslateMaterial makes for the game, its functions and its .lsf.
    bool drawServed = !inferred && provePath.empty() && !getenv("BG3MSE_SHADERS_REPLACE") && !getenv("BG3MSE_SHADERS_MATRIX_SEARCH")
        && !getenv("BG3MSE_SHADERS_MIX");
    MaterialSource windowsSource = survey.WindowsSource();
    std::string servedOf, servedWhy;
    std::optional<TranslatedMaterial> served;
    std::unique_ptr<ParsedMaterial> servedDoc;
    for (auto& pass : passes) {
        auto& row = rows[pass.Kind.empty() ? "?" : pass.Kind];
        row.Passes++;
        current = pass.Stem;
        bool good = false;
        // One line per pass for --list: the outcome, why, and what the shaders take.
        std::string outcome = "unread", detail, uses;
        struct Listed
        {
            FILE* File;
            Pass const& P;
            std::string& Outcome;
            std::string& Detail;
            std::string& Uses;
            ~Listed() { if (File) fprintf(File, "%s\t%s\t%s\t%s\t%s\n", P.Stem.c_str(), P.Kind.c_str(), Outcome.c_str(), Detail.c_str(), Uses.c_str()); }
        } listed{ list, pass, outcome, detail, uses };
        struct Count
        {
            std::pair<uint32_t, uint32_t>& Tally;
            bool& Good;
            ~Count() { Tally.first++; if (Good) Tally.second++; }
        } count{ materials[pass.Stem.substr(0, pass.Stem.size() - std::min(pass.Stem.size(), pass.Tag.size() + 1))], good };
        if (++done % 500 == 0) fprintf(stderr, "  verified %zu of %zu\n", done, passes.size());
        // BG3MSE_SHADERS_SLICE=<i>/<n>: only the passes of one material in n, the i-th such lot (from 0), are gone
        // through and listed, a material's passes always together: every pass of the game can then be drawn in
        // pieces, and a piece that is interrupted done again without the others.
        if (char const* slice = getenv("BG3MSE_SHADERS_SLICE")) {
            unsigned which = 0, of = 0;
            std::string material = pass.Stem.substr(0, pass.Stem.size() - std::min(pass.Stem.size(), pass.Tag.size() + 1));
            if (sscanf(slice, "%u/%u", &which, &of) == 2 && of && NameNoise(material, 0) % of != which) { listed.File = nullptr; continue; }
        }
        if (!pass.Problem.empty()) { row.Unread++; note("unread: " + pass.Problem); detail = pass.Problem; continue; }
        // BG3MSE_SHADERS_ONLY_STAGES=<file of stages>: only the passes whose vertex stage is one of those are gone
        // through and listed, for drawing some stages again without drawing every pass of the game.
        if (char const* only = getenv("BG3MSE_SHADERS_ONLY_STAGES")) {
            static const std::map<std::string, bool> wanted = [only] {
                std::map<std::string, bool> out;
                if (FILE* file = fopen(only, "r")) {
                    char word[256];
                    while (fscanf(file, "%255s", word) == 1) out[std::string(word).substr(0, 64)] = true;
                    fclose(file);
                }
                return out;
            }();
            auto modules = VulkanStageModules(pass.Parts.Stages[0]);
            SpirvHash stage{};
            std::string unread, hex;
            if (!modules.empty() && SpirvModuleHash(modules[0], stage, unread)) {
                for (uint8_t b : stage) { char two[3]; snprintf(two, sizeof(two), "%02x", b); hex += two; }
            }
            if (!wanted.contains(hex)) { listed.File = nullptr; continue; }
        }

        std::string why;
        std::vector<std::string> sources;
        std::vector<PlacedSlot> placed;
        bool ok = true;
        for (int s = 0; s < 2 && ok; s++) {
            auto stage = pass.Parts.Stages[s ? 4 : 0];
            if (stage.empty()) continue;
            std::string source;
            auto registers = pass.Registers(s);
            ok = TranslateStage(stage, s ? "bg3mse_fragment" : "bg3mse_vertex", pass.Inputs, pass.Material, table, mac, placed, source, why,
                MacMatrices(), nullptr, &registers);
            // For looking into one pass: BG3MSE_SHADERS_REPLACE="this=>that;;..." changes the translated source before it is compiled.
            if (auto replace = getenv("BG3MSE_SHADERS_REPLACE"); ok && replace) {
                // Several rules are separated by ";;".
                std::string rules = replace;
                for (size_t begin = 0; begin <= rules.size();) {
                    size_t end = rules.find(";;", begin);
                    if (end == std::string::npos) end = rules.size();
                    std::string rule = rules.substr(begin, end - begin);
                    auto arrow = rule.find("=>");
                    if (arrow != std::string::npos && arrow > 0) {
                        std::string from = rule.substr(0, arrow), to = rule.substr(arrow + 2);
                        for (size_t at = 0; (at = source.find(from, at)) != std::string::npos; at += to.size()) source.replace(at, from.size(), to);
                    }
                    begin = end + 2;
                }
            }
            if (ok) sources.push_back(std::move(source));
        }
        {
            // What the pass's vertex stage is, for every pass in the list.
            auto modules = VulkanStageModules(pass.Parts.Stages[0]);
            SpirvHash stage{};
            std::string unread;
            if (!modules.empty() && SpirvModuleHash(modules[0], stage, unread)) {
                uses += "stage:";
                for (uint8_t b : stage) { char two[3]; snprintf(two, sizeof(two), "%02x", b); uses += two; }
                uses += " ";
            }
        }
        if (!stagesPath.empty() && draw && !sources.empty()) {
            // For --stages, the vertex stage by itself, in every one of eight draws. Two ways:
            //  - on its own, with no fragment function, beside the game's vertex function: the depths of what it
            //    covers. That needs neither the pass's fragment stage nor its material to be translated.
            //  - with the game's own fragment function for the pass, beside the game's own pair: everything the
            //    pass draws, so what the stage hands the fragment stage is compared as well. Metal joins the two
            //    by the names of what is handed on; the game's are its source's, a translated function's are
            //    places, so the translated function is given the game's names, each to the place of its turn.
            auto modules = VulkanStageModules(pass.Parts.Stages[0]);
            SpirvHash stage{};
            std::string whyAlone, hex;
            std::vector<std::string> alone = { sources[0] };
            // BG3MSE_SHADERS_CLIP, for the point light's shadow pass: the four distances the Windows stage works out,
            // which a translated function hands on as numbers nothing reads, are what the alone draw is clipped by.
            // This is the tool's own variant of the function, for seeing what the game's function keeps beside it.
            if (getenv("BG3MSE_SHADERS_CLIP") && pass.Kind == "DEPST") {
                static const std::regex kDistance(R"(\n    float (m_\d+)_(\d) \[\[user\(locn\d+\)\]\];)");
                std::smatch found;
                std::string source = alone[0], member;
                if (std::regex_search(source, found, kDistance)) member = found[1];
                if (!member.empty()) {
                    source = std::regex_replace(source, std::regex("\\n    float " + member + "_0 \\[\\[user\\(locn\\d+\\)\\]\\];"),
                        "\n    float bg3mse_clip [[clip_distance]] [4];");
                    source = std::regex_replace(source, std::regex("\\n    float " + member + "_[123] \\[\\[user\\(locn\\d+\\)\\]\\];"), "");
                    source = std::regex_replace(source, std::regex("out\\." + member + "_(\\d)\\b"), "out.bg3mse_clip[$1]");
                    alone[0] = source;
                }
            }
            auto vertexOnly = MetalFunctions::Compile(alone, whyAlone);
            MacPass oursAlone;
            if (!modules.empty() && SpirvModuleHash(modules[0], stage, whyAlone) && vertexOnly
                && vertexOnly->Reflect("bg3mse_vertex", "", oursAlone, whyAlone)) {
                for (uint8_t b : stage) { char two[3]; snprintf(two, sizeof(two), "%02x", b); hex += two; }
                auto& seeds = seedProofs[hex];

                // The translated function under the game's names, when the pass has a fragment function.
                std::unique_ptr<MetalFunctions> named, namedOther;
                std::string notJoined, unread;      // unread: what the game's fragment function takes and is handed a number for
                bool read = false;
                // For the draw that writes what is handed on: the game's names in their order, and the places of ours.
                std::vector<MetalLibraryFile::Varying> theirs;
                std::map<uint32_t, std::string> ourPlaces;      // place -> type
                if (!pass.FragmentName.empty()) {
                    if (!gameLibrary.Varyings(pass.VertexName, theirs)) {
                        notJoined = "what the game's vertex function hands on is not read";
                    } else {
                        struct Handed { uint32_t Location; std::string Type, Member; };
                        std::vector<Handed> mine;
                        static const std::regex kHanded(R"(\n\s+(\w+) (m_\d+) \[\[user\(locn(\d+)\)\]\];)");
                        for (std::sregex_iterator it(sources[0].begin(), sources[0].end(), kHanded), end; it != end; ++it) {
                            mine.push_back({ (uint32_t)std::stoul((*it)[3]), (*it)[1], (*it)[2] });
                        }
                        std::sort(mine.begin(), mine.end(), [](auto const& x, auto const& y) { return x.Location < y.Location; });
                        // The game's names go by place: its k-th is what a translated function hands on at
                        // place k. A translated function may lack one (the Windows shader's compiler drops what
                        // its fragment stage does not read) and may have one more (the tetrahedron shadow pass's
                        // clip distances, made an ordinary thing to hand on because the game's functions leave
                        // theirs unset). It can be joined to the game's fragment function when it has, in the
                        // game's type, every one that function takes.
                        std::vector<MetalLibraryFile::Varying> lacking;
                        std::map<uint32_t, Handed const*> byPlace;
                        for (auto const& h : mine) byPlace[h.Location] = &h;
                        for (auto const& h : mine) ourPlaces[h.Location] = h.Type;
                        std::vector<MetalLibraryFile::Varying> taken;
                        if (!gameLibrary.Varyings(pass.FragmentName, taken)) notJoined = "what the game's fragment function takes is not read";
                        for (size_t i = 0; notJoined.empty() && i < theirs.size(); i++) {
                            auto have = byPlace.find((uint32_t)i);
                            bool wanted = std::any_of(taken.begin(), taken.end(), [&](auto const& t) { return t.Name == theirs[i].Name; });
                            if (have == byPlace.end()) {
                                // Taken and not handed on. A function of the game's often takes a thing it never
                                // reads (the Windows shader's compiler dropped it from both stages, the Mac game's
                                // kept it in what the function takes): handed on with a number of no meaning, and
                                // with another, the picture then has to be the same.
                                if (wanted) lacking.push_back(theirs[i]);
                            } else if (have->second->Type != theirs[i].Type) {
                                notJoined = "the game's vertex function hands on " + theirs[i].Name + " as " + theirs[i].Type + ", the translated one a "
                                    + have->second->Type + " in its place";
                            }
                        }
                        for (auto const& t : taken) {
                            bool named = std::any_of(theirs.begin(), theirs.end(), [&](auto const& v) { return v.Name == t.Name; });
                            if (notJoined.empty() && !named) notJoined = "the game's fragment function takes " + t.Name + ", which its vertex function does not name";
                        }
                        if (notJoined.empty()) {
                            std::string source = sources[0];
                            for (auto const& h : mine) {
                                if (h.Location >= theirs.size()) continue;
                                auto const& name = theirs[h.Location].Name;
                                std::string declared = h.Member + " [[user(locn" + std::to_string(h.Location) + ")]];";
                                auto at = source.find(declared);
                                if (at != std::string::npos) source.replace(at, declared.size(), name + ";");
                                source = std::regex_replace(source, std::regex("\\bout\\." + h.Member + "\\b"), "out." + name);
                            }
                            auto withNumber = [&](char const* number) {
                                std::string filled = source;
                                for (auto const& l : lacking) {
                                    auto member = filled.find("    float4 gl_Position [[position]];");
                                    auto last = filled.rfind("    return out;");
                                    if (member == std::string::npos || last == std::string::npos) return std::unique_ptr<MetalFunctions>();
                                    filled.insert(last, "    out." + l.Name + " = " + l.Type + "(" + number + ");\n");
                                    filled.insert(member, "    " + l.Type + " " + l.Name + ";\n");
                                }
                                std::vector<std::string> one = { filled };
                                return MetalFunctions::Compile(one, whyAlone);
                            };
                            named = withNumber("0.0");
                            if (!lacking.empty()) {
                                namedOther = withNumber("7.25");
                                for (auto const& l : lacking) unread += (unread.empty() ? "" : ",") + l.Name;
                            }
                            if (!named || (!lacking.empty() && !namedOther)) {
                                named.reset();
                                notJoined = "the translated function under the game's names does not compile: " + whyAlone.substr(0, 120);
                            }
                        }
                    }
                }

                uint32_t aloneSame = 0, aloneApart = 0, mixedSame = 0, mixedApart = 0;
                for (uint32_t seed = 1; seed <= 8; seed++) {
                    DrawSetup gameSetup, ourSetup;
                    DrawResult gameDrew, weDrew;
                    if (!SetupsFor(pass, oursAlone, seed, gameSetup, ourSetup, whyAlone)
                        || !survey.Game().Draw(pass.VertexName, "", gameSetup, gameDrew, whyAlone)
                        || !vertexOnly->Draw("bg3mse_vertex", "", ourSetup, weDrew, whyAlone)) {
                        if (getenv("BG3MSE_SHADERS_SHOW")) printf("%s alone, draw %u: not drawn (%s)\n", pass.Stem.c_str(), seed, whyAlone.substr(0, 160).c_str());
                        break;
                    }
                    auto depths = ComparePictures(gameDrew, weDrew);
                    if (getenv("BG3MSE_SHADERS_SHOW")) {
                        printf("%s alone, draw %u: drawn %u, at another depth %u, covered by one only %u, depths differ by %.3g at most, in %u pixels in a bit"
                            "; by both %u, by the game's only %u, by ours only %u\n",
                            pass.Stem.c_str(), seed, depths.Drawn, depths.DepthApart, depths.CoverageDifferent, depths.DepthMost, depths.DepthBits,
                            depths.BothDrew, depths.OnlyFirst, depths.OnlySecond);
                    }
                    if (depths.Drawn >= 64) {
                        // What is covered counts as well, except in the tetrahedron shadow pass: the game's own
                        // vertex functions for it declare clip distances and leave them unset (2,272 of its 2,294),
                        // so what they cover is not defined; a translated one clips nothing.
                        bool coverage = pass.Kind != "DEPST" && depths.CoverageDifferent * 50 > depths.Drawn;
                        (depths.DepthApart || coverage ? aloneApart : aloneSame)++;
                    }
                    if (!named) continue;
                    // The vertex stage's things as the translated function takes them, the fragment stage's as the
                    // game's function does.
                    DrawSetup joined = ourSetup;
                    std::erase_if(joined.Buffers, [](auto const& x) { return !x.Vertex; });
                    std::erase_if(joined.Textures, [](auto const& x) { return !x.Vertex; });
                    std::erase_if(joined.Samplers, [](auto const& x) { return !x.Vertex; });
                    for (auto const& x : gameSetup.Buffers) if (!x.Vertex) joined.Buffers.push_back(x);
                    for (auto const& x : gameSetup.Textures) if (!x.Vertex) joined.Textures.push_back(x);
                    for (auto const& x : gameSetup.Samplers) if (!x.Vertex) joined.Samplers.push_back(x);
                    DrawResult gamePair, joinedDrew;
                    std::string whyJoined;
                    if (!survey.Game().Draw(pass.VertexName, pass.FragmentName, gameSetup, gamePair, whyJoined)) continue;
                    if (!named->Draw("bg3mse_vertex", pass.FragmentName, joined, joinedDrew, whyJoined, &survey.Game())) {
                        if (notJoined.empty()) notJoined = "the two do not make a pipeline: " + whyJoined.substr(0, 160);
                        named.reset();
                        continue;
                    }
                    if (namedOther) {
                        DrawResult other;
                        if (!namedOther->Draw("bg3mse_vertex", pass.FragmentName, joined, other, whyJoined, &survey.Game())) { named.reset(); continue; }
                        auto numbers = ComparePictures(joinedDrew, other, 0.0f);
                        if (numbers.Different) read = true;
                    }
                    auto pictures = ComparePictures(gamePair, joinedDrew);
                    if (getenv("BG3MSE_SHADERS_SHOW")) {
                        // For looking into one pass: which targets the joined pair draws otherwise, and how.
                        printf("%s joined, draw %u: drawn %u, different %u (covered by one only %u, at another depth %u);", pass.Stem.c_str(), seed,
                            pictures.Drawn, pictures.Different, pictures.CoverageDifferent, pictures.DepthApart);
                        for (uint32_t t = 0; t < DrawResult::kTargets; t++) {
                            uint32_t unequal = 0, shown = 0;
                            std::string examples;
                            auto const& a = gamePair.Color[t];
                            auto const& b = joinedDrew.Color[t];
                            for (size_t i = 0; i < a.size() && i < b.size(); i++) {
                                bool differs = std::fabs(a[i] - b[i]) > 1e-2f * (1 + std::max(std::fabs(a[i]), std::fabs(b[i])));
                                unequal += differs;
                                if (differs && shown++ < 3) {
                                    char text[64];
                                    snprintf(text, sizeof(text), " [%zu.%zu] %g|%g", i / 4, i % 4, a[i], b[i]);
                                    examples += text;
                                }
                            }
                            if (unequal) printf(" target %u: %u values%s;", t, unequal, examples.c_str());
                        }
                        printf("\n");
                    }
                    // In the tetrahedron shadow pass only what both drew is compared, as above.
                    uint32_t different = pass.Kind == "DEPST" ? pictures.Different - std::min(pictures.Different, pictures.CoverageDifferent)
                        : pictures.Different;
                    if (pictures.Drawn >= 64) (different * 50 > pictures.Drawn ? mixedApart : mixedSame)++;
                }
                // What the two vertex functions hand on, compared with no fragment stage of either side in between: a
                // fragment function of this tool's own that writes each thing it is handed to a colour target, joined once
                // to the game's vertex function and once to the translated one under the game's names. Every pass ends in
                // one of three states: the stage hands nothing on; what it hands on was compared; it could not be, and why.
                {
                    std::vector<MetalLibraryFile::Varying> names;
                    struct Place { std::string Type, Member; };
                    std::map<uint32_t, Place> places;
                    static const std::regex kPlace(R"(\n\s+(\w+) (m_\d+) \[\[user\(locn(\d+)\)\]\];)");
                    for (std::sregex_iterator it(sources[0].begin(), sources[0].end(), kPlace), end; it != end; ++it) {
                        places[(uint32_t)std::stoul((*it)[3])] = { (*it)[1], (*it)[2] };
                    }
                    std::string state, differing, onlyTheirs, onlyOurs;
                    uint32_t same = 0, compared = 0;
                    if (!gameLibrary.Varyings(pass.VertexName, names)) state = "notcompared:the-games-names-are-not-read";
                    // The tetrahedron shadow pass's clip distances are the last thing a translated function hands on,
                    // past the game's names (they were made that, for the game's functions leave theirs unset).
                    if (pass.Kind == "DEPST" && !places.empty() && std::prev(places.end())->first >= names.size()) places.erase(std::prev(places.end()));
                    // What only the game's function hands on is no doing of the translated stage: one that hands nothing on
                    // has nothing to compare, and the names of the game's are listed.
                    if (state.empty() && places.empty()) state = "none";
                    for (auto const& [place, mine] : places) {
                        if (!state.empty()) break;
                        if (place >= names.size()) onlyOurs += (onlyOurs.empty() ? "" : ",") + std::to_string(place);
                        else if (names[place].Type != mine.Type) state = "notcompared:" + names[place].Name + "-is-" + names[place].Type + "-ours-" + mine.Type;
                    }
                    std::unique_ptr<MetalFunctions> renamed;
                    if (state.empty()) {
                        std::string source = sources[0];
                        for (auto const& [place, mine] : places) {
                            if (place >= names.size()) continue;
                            std::string declared = mine.Member + " [[user(locn" + std::to_string(place) + ")]];";
                            auto at = source.find(declared);
                            if (at != std::string::npos) source.replace(at, declared.size(), names[place].Name + ";");
                            source = std::regex_replace(source, std::regex("\\bout\\." + mine.Member + "\\b"), "out." + names[place].Name);
                        }
                        std::vector<std::string> one = { source };
                        std::string whyNot;
                        renamed = MetalFunctions::Compile(one, whyNot);
                        if (!renamed) state = "notcompared:the-renamed-function-does-not-compile";
                    }
                    // The numbers among them, eight to a draw: all of the game's to its writer, those ours has to ours.
                    std::vector<uint32_t> numbers;
                    for (uint32_t i = 0; i < names.size(); i++) {
                        if (names[i].Type.starts_with("float")) numbers.push_back(i);
                        if (!places.contains(i)) onlyTheirs += (onlyTheirs.empty() ? "" : ",") + names[i].Name;
                    }
                    if (state.empty() && numbers.empty()) state = "notcompared:nothing-handed-on-is-a-number";
                    // A writer of each side for every eight of them.
                    struct Writers { size_t First, Count; std::unique_ptr<MetalFunctions> Game, Ours; };
                    std::vector<Writers> writers;
                    for (size_t first = 0; state.empty() && first < numbers.size(); first += DrawResult::kTargets) {
                        size_t count = std::min<size_t>(DrawResult::kTargets, numbers.size() - first);
                        auto writer = [&](bool ours) {
                            std::string text = "#include <metal_stdlib>\nusing namespace metal;\nstruct bg3mse_handed { float4 at [[position]];";
                            std::string body;
                            for (size_t i = 0; i < count; i++) {
                                auto const& n = names[numbers[first + i]];
                                if (ours && !places.contains(numbers[first + i])) continue;
                                text += " " + n.Type + " " + n.Name + ";";
                                std::string value = n.Type == "float" ? "float4(in." + n.Name + ", 0.0, 0.0, 1.0)" : n.Type == "float2" ? "float4(in." + n.Name + ", 0.0, 1.0)"
                                    : n.Type == "float3" ? "float4(in." + n.Name + ", 1.0)" : "in." + n.Name;
                                body += "    out.c" + std::to_string(i) + " = " + value + ";\n";
                            }
                            text += " };\nstruct bg3mse_written {";
                            for (size_t i = 0; i < DrawResult::kTargets; i++) text += " float4 c" + std::to_string(i) + " [[color(" + std::to_string(i) + ")]];";
                            text += " };\nfragment bg3mse_written bg3mse_writer(bg3mse_handed in [[stage_in]]) {\n    bg3mse_written out = {};\n" + body + "    return out;\n}\n";
                            std::vector<std::string> one = { text };
                            std::string whyNot;
                            return MetalFunctions::Compile(one, whyNot);
                        };
                        writers.push_back({ first, count, writer(false), writer(true) });
                        if (!writers.back().Game || !writers.back().Ours) state = "notcompared:the-writer-does-not-compile";
                    }
                    // Each of the eight draws that draws enough is compared: a thing handed on is the same when it is
                    // in every one of them. Of a thing that differs: in how many draws, in the draw where most pixels
                    // differ how many, by how much at most, and each kind of relation a draw showed.
                    struct Found { uint32_t Differs{ 0 }, Pixels{ 0 }, Of{ 1 }; float Most{ 0 }; std::vector<std::string> Relations; };
                    std::vector<Found> found(numbers.size());
                    uint32_t draws = 0;
                    // BG3MSE_SHADERS_CLOSE=x, for looking into one pass: how near two numbers have to be to count as the
                    // same (a hundredth of their size where nothing is said), and with it a thing differs when one pixel does.
                    static const float kClose = getenv("BG3MSE_SHADERS_CLOSE") ? (float)atof(getenv("BG3MSE_SHADERS_CLOSE")) : 1e-2f;
                    static const bool kAnyPixel = getenv("BG3MSE_SHADERS_CLOSE") != nullptr;
                    auto closeTo = [](float x, float y) {
                        if (std::isnan(x) || std::isnan(y)) return std::isnan(x) == std::isnan(y);
                        return std::fabs(x - y) <= kClose * (1 + std::max(std::fabs(x), std::fabs(y)));
                    };
                    auto width = [&](size_t i) { auto const& t = names[numbers[i]].Type; return t == "float" ? 1 : t.back() - '0'; };
                    for (uint32_t seed = 1; seed <= 8 && state.empty(); seed++) {
                        DrawSetup gameSetup, ourSetup;
                        std::string whyNot;
                        if (!SetupsFor(pass, oursAlone, seed, gameSetup, ourSetup, whyNot)) { state = "notcompared:no-inputs"; break; }
                        for (DrawSetup* setup : { &gameSetup, &ourSetup }) {
                            std::erase_if(setup->Buffers, [](auto const& x) { return !x.Vertex; });
                            std::erase_if(setup->Textures, [](auto const& x) { return !x.Vertex; });
                            std::erase_if(setup->Samplers, [](auto const& x) { return !x.Vertex; });
                        }
                        // What each side handed on under each name, four numbers to a pixel, in the pixels both drew:
                        // the same pixels for every eight, which are drawn with the same numbers.
                        struct Handed { std::vector<float> Theirs, Mine; };
                        std::vector<Handed> handed(numbers.size());
                        std::vector<uint32_t> both;
                        bool enough = true;
                        for (auto const& w : writers) {
                            DrawResult theirs, mine;
                            if (!survey.Game().Draw(pass.VertexName, "bg3mse_writer", gameSetup, theirs, whyNot, w.Game.get())
                                || !renamed->Draw("bg3mse_vertex", "bg3mse_writer", ourSetup, mine, whyNot, w.Ours.get())) {
                                state = "notcompared:not-drawn";
                                break;
                            }
                            std::vector<uint32_t> drawn;
                            for (uint32_t pixel = 0; pixel < theirs.Depth.size(); pixel++) if (theirs.Depth[pixel] != 1.0f && mine.Depth[pixel] != 1.0f) drawn.push_back(pixel);
                            if (w.First == 0) {
                                if (drawn.size() < 64) { enough = false; break; }
                                both = std::move(drawn);
                            } else if (drawn != both) {
                                state = "notcompared:not-the-same-pixels-twice";
                                break;
                            }
                            for (size_t i = 0; i < w.Count; i++) {
                                auto& h = handed[w.First + i];
                                for (uint32_t pixel : both) {
                                    for (int c = 0; c < 4; c++) {
                                        h.Theirs.push_back(theirs.Color[i][pixel * 4 + c]);
                                        h.Mine.push_back(mine.Color[i][pixel * 4 + c]);
                                    }
                                }
                            }
                        }
                        if (!state.empty() || !enough) continue;
                        draws++;
                        const size_t pixels = both.size();
                        for (size_t i = 0; i < numbers.size(); i++) {
                            if (!places.contains(numbers[i])) continue;
                            auto const& n = names[numbers[i]];
                            auto const& h = handed[i];
                            // In how many of the pixels ours is not the game's, and by how much at most.
                            float most = 0;
                            uint32_t differs = 0;
                            for (size_t pixel = 0; pixel < pixels; pixel++) {
                                bool differ = false;
                                for (int c = 0; c < width(i); c++) {
                                    float x = h.Theirs[pixel * 4 + c], y = h.Mine[pixel * 4 + c];
                                    if (!closeTo(x, y)) { differ = true; most = std::max(most, std::fabs(x - y)); }
                                }
                                differs += differ;
                            }
                            if (kAnyPixel ? differs == 0 : differs * 50 <= pixels) continue;
                            // How ours relates to the game's, by the numbers. A matrix handed on in several things,
                            // named alike with a digit: is ours the game's turned over (ours k-th the k-th number of
                            // each of the game's)? Otherwise, number by number: nothing; the game's number of the
                            // same or another place times a factor that is the same in every pixel; or, where each
                            // side hands on one number for the whole draw, the two numbers, which any factor fits.
                            std::string relation;
                            if (!n.Name.empty() && isdigit((unsigned char)n.Name.back())) {
                                std::string family = n.Name.substr(0, n.Name.size() - 1);
                                int row = n.Name.back() - '0';
                                std::map<int, size_t> columns;
                                for (size_t j = 0; j < numbers.size(); j++) {
                                    auto const& other = names[numbers[j]].Name;
                                    if (other.size() == n.Name.size() && other.starts_with(family) && isdigit((unsigned char)other.back())) columns[other.back() - '0'] = j;
                                }
                                uint32_t off = 0;
                                bool whole = !columns.empty();
                                for (int c = 0; c < width(i) && whole; c++) {
                                    auto column = columns.find(c);
                                    if (column == columns.end() || width(column->second) <= row) { whole = false; break; }
                                    for (size_t pixel = 0; pixel < pixels; pixel++) off += !closeTo(handed[column->second].Theirs[pixel * 4 + row], h.Mine[pixel * 4 + c]);
                                }
                                if (whole && off * 50 <= pixels * (uint32_t)width(i)) relation = "turned-over";
                            }
                            if (relation.empty()) {
                                bool oneNumber = true;
                                for (int c = 0; c < width(i) && oneNumber; c++) {
                                    for (size_t pixel = 1; pixel < pixels && oneNumber; pixel++) {
                                        oneNumber = std::fabs(h.Theirs[pixel * 4 + c] - h.Theirs[c]) <= 1e-4f * (1 + std::fabs(h.Theirs[c]))
                                            && std::fabs(h.Mine[pixel * 4 + c] - h.Mine[c]) <= 1e-4f * (1 + std::fabs(h.Mine[c]));
                                    }
                                }
                                if (oneNumber) {
                                    relation = "one-number-each";
                                    for (int side = 0; side < 2; side++) {
                                        relation += side ? "|" : ":";
                                        for (int c = 0; c < width(i); c++) {
                                            char text[24];
                                            snprintf(text, sizeof(text), "%s%.4g", c ? "," : "", (side ? h.Mine : h.Theirs)[c]);
                                            relation += text;
                                        }
                                    }
                                }
                            }
                            if (relation.empty()) {
                                bool every = true, asItIs = true;
                                for (int c = 0; c < width(i) && every; c++) {
                                    bool fits = false;
                                    for (int step = 0; step < width(i) && !fits; step++) {
                                        // The game's number of the same place first, then of the others. The factor:
                                        // the middle one of ours over the game's where the game's is not near zero.
                                        int d = (c + step) % width(i);
                                        std::vector<float> ratios;
                                        for (size_t pixel = 0; pixel < pixels; pixel++) {
                                            float x = h.Theirs[pixel * 4 + d], y = h.Mine[pixel * 4 + c];
                                            if (std::fabs(x) > 1e-3f) ratios.push_back(y / x);
                                        }
                                        if (ratios.size() < 32) continue;
                                        std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
                                        float factor = ratios[ratios.size() / 2];
                                        uint32_t off = 0;
                                        for (size_t pixel = 0; pixel < pixels; pixel++) off += !closeTo(h.Theirs[pixel * 4 + d] * factor, h.Mine[pixel * 4 + c]);
                                        if (off * 50 > pixels) continue;
                                        char text[48];
                                        if (std::fabs(factor) < 1e-3f) snprintf(text, sizeof(text), "%s%c=0", relation.empty() ? "" : ",", "xyzw"[c]);
                                        else snprintf(text, sizeof(text), "%s%c=%.4g*%c", relation.empty() ? "" : ",", "xyzw"[c], factor, "xyzw"[d]);
                                        relation += text;
                                        asItIs = asItIs && d == c && std::fabs(factor - 1) < 2e-3f;
                                        fits = true;
                                    }
                                    every = fits;
                                }
                                // Each number by itself the game's in nearly every pixel: what differs is some pixels.
                                if (!every) relation = "no-relation";
                                else if (asItIs) relation = "in-some-pixels";
                            }
                            auto& f = found[i];
                            if (getenv("BG3MSE_SHADERS_SHOW") && f.Differs == 0) {
                                // The numbers themselves, at three of the pixels of the first draw it differs in: the game's, then ours.
                                for (size_t pixel : { (size_t)0, pixels / 2, pixels - 1 }) {
                                    printf("        handed on %s in draw %u at pixel %u:", n.Name.c_str(), seed, both[pixel]);
                                    for (int c = 0; c < width(i); c++) printf(" %.6g", h.Theirs[pixel * 4 + c]);
                                    printf(" | ours");
                                    for (int c = 0; c < width(i); c++) printf(" %.6g", h.Mine[pixel * 4 + c]);
                                    printf("\n");
                                }
                            }
                            f.Differs++;
                            if ((uint64_t)differs * f.Of > (uint64_t)f.Pixels * pixels) { f.Pixels = differs; f.Of = (uint32_t)pixels; }
                            f.Most = std::max(f.Most, most);
                            // One of a kind is kept: the first factors, the first pair of numbers.
                            auto kind = [](std::string const& r) { return r.starts_with("one-number-each") ? std::string("one") : r.find('=') != std::string::npos ? std::string("factor") : r; };
                            if (std::none_of(f.Relations.begin(), f.Relations.end(), [&](auto const& r) { return kind(r) == kind(relation); })) f.Relations.push_back(relation);
                        }
                    }
                    if (state.empty() && !numbers.empty() && draws == 0) state = "notcompared:never-drew-enough";
                    if (state.empty()) {
                        for (size_t i = 0; i < numbers.size(); i++) {
                            if (!places.contains(numbers[i])) continue;
                            compared++;
                            auto const& f = found[i];
                            if (f.Differs == 0) { same++; continue; }
                            std::string relations;
                            for (auto const& r : f.Relations) relations += (relations.empty() ? "" : " & ") + r;
                            char text[160];
                            snprintf(text, sizeof(text), "(%u of %u draws, %u of %u pixels, up to %.3g; ", f.Differs, draws, f.Pixels, f.Of, f.Most);
                            differing += (differing.empty() ? "" : " ") + names[numbers[i]].Name + text + relations + ")";
                        }
                    }
                    if (state.empty() && compared == 0) state = "notcompared:ours-hands-on-none-of-the-numbers";
                    seeds.Handed += state.empty();
                    seeds.HandedDiffer += state.empty() && !differing.empty();
                    uses += "handed:" + (state.empty() ? std::to_string(same) + "/" + std::to_string(compared) + " draws:" + std::to_string(draws) : state)
                        + (onlyTheirs.empty() ? "" : " onlytheirs:" + onlyTheirs) + (onlyOurs.empty() ? "" : " onlyours:" + onlyOurs)
                        + (differing.empty() ? " " : " differs:[" + differing + "] ");
                    if (state.empty()) {
                        handedPasses++;
                        if (differing.empty()) handedSame++;
                        for (uint32_t i : numbers) {
                            if (differing.find(names[i].Name + "(") != std::string::npos) handedNames[std::regex_replace(names[i].Name, std::regex("[0-9]+"), "#")]++;
                        }
                    } else {
                        handedStates[std::regex_replace(state, std::regex("[0-9]+"), "#")]++;
                    }
                }
                if (read) {
                    // The picture hangs on the number: the function reads what the translated one does not hand on.
                    notJoined = "the game's fragment function reads " + unread + ", which the translated vertex function does not hand on";
                    mixedSame = mixedApart = 0;
                    unread.clear();
                }
                seeds.AloneSame += aloneSame;
                seeds.AloneApart += aloneApart;
                seeds.JoinedSame += mixedSame;
                seeds.JoinedApart += mixedApart;
                seeds.Passes++;
                if (!notJoined.empty()) { seeds.NotJoined++; notJoinedReasons[std::regex_replace(notJoined, std::regex("[0-9]+"), "#")]++; }
                auto& proof = stageProofs[hex];
                if (aloneApart) proof.second++;
                else if (aloneSame) proof.first++;
                uses += "alone:" + std::to_string(aloneSame) + "/" + std::to_string(aloneApart) + " joined:" + std::to_string(mixedSame) + "/"
                    + std::to_string(mixedApart) + (notJoined.empty() ? " " : " notjoined ") + (unread.empty() ? "" : "unread:" + unread + " ");
                if (!unread.empty()) unreadNames[std::regex_replace(unread, std::regex("[0-9]+"), "#")]++;
            }
        }
        if (!ok) { row.Refused++; note("refused: " + why); outcome = "refused"; detail = why; continue; }

        std::string vName = "bg3mse_vertex", fName = "bg3mse_fragment";
        if (drawServed) {
            std::string material = pass.Stem.substr(0, pass.Stem.size() - pass.Tag.size() - 1) + ".lsf";
            if (material != servedOf) {
                servedOf = material;
                servedWhy.clear();
                served = TranslateMaterial(windowsSource, material, table, mac, servedWhy);
                servedDoc = std::make_unique<ParsedMaterial>();
                if (served) {
                    char err[256] = "";
                    servedDoc->Bytes = served->Lsf;
                    servedDoc->Ok = lsf_parse(servedDoc->Bytes.data(), servedDoc->Bytes.size(), &servedDoc->Doc, err, sizeof(err));
                }
            }
            if (!served || !servedDoc->Ok) {
                // As in the game: a material one of whose passes is refused is refused whole.
                row.Refused++;
                note("refused: its material is refused (" + servedWhy + ")");
                outcome = "refused";
                detail = "its material is refused (" + servedWhy + ")";
                continue;
            }
            auto mine = std::find_if(served->Passes.begin(), served->Passes.end(), [&](auto const& p) { return p.Pass == pass.Tag; });
            if (mine != served->Passes.end()) {
                sources = { mine->VertexSource };
                if (!mine->FragmentSource.empty()) sources.push_back(mine->FragmentSource);
                // The functions by the names the served stub has: what the game asks its Metal libraries for.
                ContainerParts stub;
                auto text = [](std::span<uint8_t const> blob) { return std::string((char const*)blob.data(), blob.size()); };
                if (!ParseContainer(mine->Stub, stub)) { row.Refused++; note("refused: its stub does not parse"); outcome = "refused"; continue; }
                vName = text(stub.Stages[0]);
                fName = text(stub.Stages[4]);
                pass.Served = true;
                pass.ServedSlots = ReadMaterialSlots(servedDoc->Doc, pass.Tag);
                // And what it names for the pass with a register and no Vulkan place: the engine goes by the register.
                auto registersOnly = ReadUnplacedSlots(servedDoc->Doc, pass.Tag);
                pass.ServedSlots.insert(pass.ServedSlots.end(), registersOnly.begin(), registersOnly.end());
                pass.ServedOffsets = BlockOffsets(servedDoc->Doc, PassNodeName(pass.Tag));
            }
        }

        auto compiled = MetalFunctions::Compile(sources, why);
        MacPass ours;
        if (!compiled || !compiled->Reflect(vName, sources.size() > 1 ? fName : "", ours, why)) {
            row.NotCompiled++;
            note("not compiled: " + why.substr(0, 160));
            outcome = "not compiled";
            continue;
        }
        uint32_t extra = 0;
        // The material's own things are where its .lsf says, which need not be where the Mac game's own material
        // has them: those it places and those it names with a register alone.
        auto named = pass.Material;
        named.insert(named.end(), pass.Unplaced.begin(), pass.Unplaced.end());
        auto difference = CompareInterfaces(ours, pass.Mac, named, extra);
        for (auto const* stage : { &ours.Vertex, &ours.Fragment }) {
            for (auto const& r : *stage) {
                if (!r.Used) continue;
                // "(depths)": the game's function takes it as depths and ours as a plain texture; both are given depths.
                bool depths = false;
                for (auto const* theirs : { &pass.Mac.Vertex, &pass.Mac.Fragment }) {
                    for (auto const& g : *theirs) depths = depths || (r.Kind == ResourceKind::Texture && !r.Depth && g.Kind == ResourceKind::Texture && g.Depth && AsWritten(g.Name) == AsWritten(r.Name));
                }
                uses += (stage == &ours.Vertex ? "v:" : "f:") + r.Name + (depths ? "(depths) " : " ");
            }
        }
        // And the matrices each stage builds, as "m:<stage><size><how built><how used>".
        for (int s = 0; s < 2; s++) {
            auto modules = VulkanStageModules(pass.Parts.Stages[s ? 4 : 0]);
            std::string ignored;
            if (modules.empty()) continue;
            SpirvShader::Parse(modules[0], ignored, [&](BuiltMatrix const& m, uint32_t) {
                uses += std::string("m:") + (s ? "f" : "v") + std::to_string(m.Size) + (m.ColumnsFromScalars ? "s" : m.ColumnsBuilt ? "b" : "e")
                    + (m.TimesVectorLeft ? "L" : "") + (m.TimesVectorRight ? "R" : "") + (m.TimesMatrixLeft ? "l" : "")
                    + (m.TimesMatrixRight ? "r" : "") + (m.OtherUse ? "o" : "") + " ";
                return false;
            });
        }
        if (!difference.empty()) { row.Different++; note("different: " + difference); outcome = "interface differs"; detail = difference; continue; }
        row.Same++;
        outcome = "same interface";
        if (extra) withExtra++;
        good = true;
        if (!draw) continue;

        // The same draw through the game's functions and through ours.
        auto& picture = pictures[pass.Kind];
        bool settled = false;
        uint32_t littleDrawn = 0, littleDifferent = 0;
        for (uint32_t seed = 1; seed <= 8 && !settled; seed++) {
            DrawSetup forGame, forOurs;
            if (!SetupsFor(pass, ours, seed, forGame, forOurs, why)) {
                picture.NotComparable++;
                note("not comparable: " + why);
                outcome = "not comparable";
                detail = why;
                settled = true;
                break;
            }
            DrawResult theirs, mine;
            if (!survey.Game().Draw(pass.VertexName, pass.FragmentName, forGame, theirs, why)
                || !compiled->Draw(vName, sources.size() > 1 ? fName : "", forOurs, mine, why)) {
                picture.NotDrawn++;
                note("not drawn: " + why.substr(0, 120));
                outcome = "no draw";
                detail = why.substr(0, 120);
                settled = true;
                break;
            }
            auto compared = ComparePictures(theirs, mine);
            if (seed == 1) {
                // For the passes of one material: do they put a pixel at the same depth, to the last bit?
                std::string material = pass.Stem.substr(0, pass.Stem.rfind('_'));
                if (material != depthsOf) { depthsDone(); depthsOf = material; }
                gameDepths.push_back(theirs.Depth);
                ourDepths.push_back(mine.Depth);
            }
            if (getenv("BG3MSE_SHADERS_MIX") && sources.size() > 1) {
                // For looking into one pass: each pair's vertex function with the other's fragment function.
                auto mixed = [&](DrawSetup const& vertexSide, DrawSetup const& fragmentSide) {
                    DrawSetup setup = vertexSide;
                    std::erase_if(setup.Buffers, [](auto const& b) { return !b.Vertex; });
                    std::erase_if(setup.Textures, [](auto const& t) { return !t.Vertex; });
                    std::erase_if(setup.Samplers, [](auto const& s) { return !s.Vertex; });
                    for (auto const& b : fragmentSide.Buffers) if (!b.Vertex) setup.Buffers.push_back(b);
                    for (auto const& t : fragmentSide.Textures) if (!t.Vertex) setup.Textures.push_back(t);
                    for (auto const& s : fragmentSide.Samplers) if (!s.Vertex) setup.Samplers.push_back(s);
                    return setup;
                };
                DrawResult gameVertex, ourVertex;
                std::string whyMixed;
                if (survey.Game().Draw(pass.VertexName, fName, mixed(forGame, forOurs), gameVertex, whyMixed, compiled.get())) {
                    auto a = ComparePictures(theirs, gameVertex), b = ComparePictures(mine, gameVertex);
                    printf("  seed %u, the game's vertex function with our fragment function: %u of %u differ from the game's pair, %u from ours\n",
                        seed, a.Different, a.Drawn, b.Different);
                } else {
                    printf("  the game's vertex function does not draw with our fragment function: %s\n", whyMixed.substr(0, 160).c_str());
                }
                if (compiled->Draw(vName, pass.FragmentName, mixed(forOurs, forGame), ourVertex, whyMixed, &survey.Game())) {
                    auto a = ComparePictures(theirs, ourVertex), b = ComparePictures(mine, ourVertex);
                    printf("  seed %u, our vertex function with the game's fragment function: %u of %u differ from the game's pair, %u from ours\n",
                        seed, a.Different, a.Drawn, b.Different);
                } else {
                    printf("  our vertex function does not draw with the game's fragment function: %s\n", whyMixed.substr(0, 160).c_str());
                }
            }
            if (getenv("BG3MSE_SHADERS_SHOW")) {
                // For looking into one pass: what each pair drew, target by target.
                printf("%s seed %u: drawn %u, different %u\n", pass.Stem.c_str(), seed, compared.Drawn, compared.Different);
                for (uint32_t t = 0; t <= DrawResult::kTargets; t++) {
                    auto const& a = t < DrawResult::kTargets ? theirs.Color[t] : theirs.Depth;
                    auto const& b = t < DrawResult::kTargets ? mine.Color[t] : mine.Depth;
                    float clear = t < DrawResult::kTargets ? MetalFunctions::kClearColor : 1.0f;
                    uint32_t na = 0, nb = 0, shown = 0;
                    for (size_t i = 0; i < a.size(); i++) { na += a[i] != clear; nb += b[i] != clear; }
                    if (!na && !nb) continue;
                    printf("  target %u: the game's wrote %u values, ours %u;", t, na, nb);
                    // The first values that differ, or the first written when none does.
                    uint32_t unequal = 0;
                    for (size_t i = 0; i < a.size() && i < b.size(); i++) unequal += std::fabs(a[i] - b[i]) > 1e-2f * std::max({ 1.0f, std::fabs(a[i]), std::fabs(b[i]) }) || (a[i] != a[i]) != (b[i] != b[i]);
                    printf(" %u differ;", unequal);
                    for (size_t i = 0; i < a.size() && i < b.size() && shown < 8; i++) {
                        bool differs = std::fabs(a[i] - b[i]) > 1e-2f * std::max({ 1.0f, std::fabs(a[i]), std::fabs(b[i]) }) || (a[i] != a[i]) != (b[i] != b[i]);
                        if (unequal ? differs : (a[i] != clear || b[i] != clear)) { printf(" [%zu] %g|%g", i, a[i], b[i]); shown++; }
                    }
                    printf("\n");
                }
                if (seed == 1) {
                    for (MacPass const* side : { (MacPass const*)&pass.Mac, (MacPass const*)&ours }) {
                        printf("  %s take:", side == &ours ? "ours" : "the game's");
                        for (auto const& a : side->Attributes) printf(" %s@%u", a.Name.c_str(), a.Index);
                        for (auto const* stage : { &side->Vertex, &side->Fragment }) {
                            for (auto const& r : *stage) {
                                if (r.Used) printf(" %s:%s@%u", stage == &side->Vertex ? "v" : "f", r.Name.c_str(), r.Index);
                            }
                        }
                        printf("\n");
                    }
                    for (auto const& source : sources) printf("%s\n", source.c_str());
                }
            }
            if (compared.Drawn < 64) {
                // Too little to tell: other numbers.
                littleDrawn += compared.Drawn;
                littleDifferent += compared.Different;
                continue;
            }
            settled = true;
            detail = "drawn " + std::to_string(compared.Drawn) + " different " + std::to_string(compared.Different) + " depth "
                + std::to_string(compared.DepthDifferent) + " coverage " + std::to_string(compared.CoverageDifferent) + " depthbits " + std::to_string(compared.DepthBits)
                + " depthapart " + std::to_string(compared.DepthApart)
                + (compared.Written ? " written " + std::to_string(compared.Written) + " differently " + std::to_string(compared.WrittenDifferent) : "");
            outcome = compared.Different * 50 <= compared.Drawn ? "same picture" : "different picture";
            if (compared.Different * 50 > compared.Drawn && getenv("BG3MSE_SHADERS_MATRIX_SEARCH")) {
                // Which of the matrices the shaders build have to be transposed for the pictures to agree?
                std::vector<BuiltMatrix> found[2];
                for (int s = 0; s < 2; s++) {
                    auto stage = pass.Parts.Stages[s ? 4 : 0];
                    auto modules = VulkanStageModules(stage);
                    if (modules.empty()) continue;
                    std::string ignored;
                    SpirvShader::Parse(modules[0], ignored, [&](BuiltMatrix const& m, uint32_t) { found[s].push_back(m); return false; });
                }
                size_t total = found[0].size() + found[1].size();
                std::string text = " matrices " + std::to_string(found[0].size()) + "+" + std::to_string(found[1].size()) + ":";
                for (int s = 0; s < 2; s++) {
                    for (auto const& m : found[s]) {
                        text += std::string(" ") + (s ? "f" : "v") + std::to_string(m.Size) + (m.ColumnsFromScalars ? "s" : m.ColumnsBuilt ? "b" : "e")
                            + (m.TimesVectorLeft ? "L" : "") + (m.TimesVectorRight ? "R" : "") + (m.TimesMatrixLeft ? "l" : "")
                            + (m.TimesMatrixRight ? "r" : "") + (m.OtherUse ? "o" : "");
                    }
                }
                text += " agree with:";
                for (uint32_t mask = 1; total && total <= 6 && mask < (1u << total); mask++) {
                    std::vector<std::string> tried;
                    std::vector<PlacedSlot> ignoredSlots;
                    bool translated = true;
                    for (int s = 0; s < 2 && translated; s++) {
                        auto stage = pass.Parts.Stages[s ? 4 : 0];
                        if (stage.empty()) continue;
                        uint32_t shift = s ? (uint32_t)found[0].size() : 0;
                        std::string source, reason;
                        auto registers = pass.Registers(s);
                        translated = TranslateStage(stage, s ? fName : vName, pass.Inputs, pass.Material, table, mac,
                            ignoredSlots, source, reason, [&](BuiltMatrix const&, uint32_t index) { return (mask >> (index + shift)) & 1; },
                            nullptr, &registers);
                        tried.push_back(std::move(source));
                    }
                    std::string reason;
                    auto other = translated ? MetalFunctions::Compile(tried, reason) : nullptr;
                    DrawResult again;
                    if (!other || !other->Draw(vName, tried.size() > 1 ? fName : "", forOurs, again, reason)) continue;
                    auto result = ComparePictures(theirs, again);
                    if (result.Drawn >= 64 && result.Different * 50 <= result.Drawn) {
                        text += " ";
                        for (size_t bit = 0; bit < total; bit++) text.push_back((mask >> bit) & 1 ? 'T' : '-');
                    }
                }
                detail += text;
            }
            if (!provePath.empty()) {
                // Textures the table paired by order: of the ones this pass takes, which is which? Every
                // assignment of their names is translated and drawn; one that alone agrees with the game's
                // picture, in a pass whose picture depends on each of them, is a vote.
                std::map<std::string, std::vector<EngineSlot const*>> groups;
                for (int s = 0; s < 2; s++) {
                    auto modules = VulkanStageModules(pass.Parts.Stages[s ? 4 : 0]);
                    std::string ignored;
                    auto shader = modules.empty() ? nullptr : SpirvShader::Parse(modules[0], ignored);
                    if (!shader) continue;
                    for (auto const& r : shader->Resources()) {
                        auto slot = table.Slot(r.Set, r.Binding);
                        if (!slot || !slot->Inferred || slot->Kind != ResourceKind::Texture) continue;
                        auto& group = groups[slot->Type];
                        if (std::find(group.begin(), group.end(), slot) == group.end()) group.push_back(slot);
                    }
                }
                for (auto& [type, group] : groups) {
                    std::string key = type;
                    std::sort(group.begin(), group.end(), [](auto a, auto b) { return std::pair(a->Set, a->Binding) < std::pair(b->Set, b->Binding); });
                    for (auto slot : group) key += " " + std::to_string(slot->Set) + "," + std::to_string(slot->Binding);
                    if (group.size() < 2 || group.size() > 5 || searched[key] >= 200) continue;

                    // Which of them the game's picture depends on: only those can be told apart by it.
                    constexpr float kClose = 2e-5f;
                    std::set<std::string> dependsOn;
                    for (auto slot : group) {
                        DrawSetup changed = forGame;
                        bool found = false;
                        for (auto const* stage : { &pass.Mac.Vertex, &pass.Mac.Fragment }) {
                            for (auto const& r : *stage) {
                                if (r.Name != slot->Name || r.Kind != ResourceKind::Texture) continue;
                                for (auto& t : changed.Textures) {
                                    if (t.Index == r.Index && t.Vertex == (stage == &pass.Mac.Vertex)) { t.Noise ^= 0x5bd1e995; found = true; }
                                }
                            }
                        }
                        DrawResult again;
                        std::string reason;
                        if (!found || !survey.Game().Draw(pass.VertexName, pass.FragmentName, changed, again, reason)) continue;
                        // A lit surface's sunlight is a small part of what it shows: looked at closely.
                        auto result = ComparePictures(theirs, again, kClose);
                        if (result.Different * 50 > result.Drawn) dependsOn.insert(slot->Name);
                    }
                    if (getenv("BG3MSE_SHADERS_SHOW")) {
                        printf("  group %s: the picture depends on", key.c_str());
                        for (auto const& n : dependsOn) printf(" %s", n.c_str());
                        printf("\n");
                    }
                    if (dependsOn.size() < 2) continue;
                    searched[key]++;

                    std::vector<size_t> order(group.size());
                    for (size_t i = 0; i < order.size(); i++) order[i] = i;
                    std::vector<std::vector<size_t>> agreeing;
                    do {
                        InterfaceTable candidate = table;
                        for (size_t i = 0; i < group.size(); i++) {
                            for (auto& slot : candidate.Slots) {
                                if (slot.Set == group[i]->Set && slot.Binding == group[i]->Binding) slot.Name = group[order[i]]->Name;
                            }
                        }
                        std::vector<std::string> tried;
                        std::vector<PlacedSlot> ignoredSlots;
                        bool translated = true;
                        for (int s = 0; s < 2 && translated; s++) {
                            auto stage = pass.Parts.Stages[s ? 4 : 0];
                            if (stage.empty()) continue;
                            std::string source, reason;
                            auto registers = pass.Registers(s);
                            translated = TranslateStage(stage, s ? fName : vName, pass.Inputs, pass.Material, candidate, mac,
                                ignoredSlots, source, reason, MacMatrices(), nullptr, &registers);
                            tried.push_back(std::move(source));
                        }
                        std::string reason;
                        auto other = translated ? MetalFunctions::Compile(tried, reason) : nullptr;
                        MacPass taken;
                        DrawSetup gameSetup, candidateSetup;
                        DrawResult again;
                        if (!other || !other->Reflect(vName, tried.size() > 1 ? fName : "", taken, reason)
                            || !SetupsFor(pass, taken, seed, gameSetup, candidateSetup, reason)
                            || !other->Draw(vName, tried.size() > 1 ? fName : "", candidateSetup, again, reason)) continue;
                        auto result = ComparePictures(theirs, again, kClose);
                        if (result.Drawn >= 64 && result.Different * 50 <= result.Drawn) agreeing.push_back(order);
                    } while (std::next_permutation(order.begin(), order.end()));
                    if (getenv("BG3MSE_SHADERS_SHOW")) printf("  group %s: %zu assignments agree\n", key.c_str(), agreeing.size());
                    // A slot that every agreeing assignment gives the same name, one the picture depends on, has that name.
                    // The one name left when all the others of the group are told apart goes to the one slot left.
                    bool byElimination = agreeing.size() == 1 && dependsOn.size() + 1 == group.size();
                    for (size_t i = 0; i < group.size() && !agreeing.empty(); i++) {
                        auto const& name = group[agreeing[0][i]]->Name;
                        bool always = dependsOn.contains(name) || byElimination;
                        for (auto const& assignment : agreeing) always = always && group[assignment[i]]->Name == name;
                        if (always) votes[{ group[i]->Set, group[i]->Binding }][name]++;
                    }
                }
            }
            if (compared.Different * 50 <= compared.Drawn && !provePath.empty()) {
                // The pictures agree. An inferred pairing is borne out by this pass when the game's own picture
                // changes with what is given under that name: had ours taken it elsewhere, the two would differ.
                auto dependsOn = [&](DrawSetup const& changed) {
                    DrawResult again;
                    std::string reason;
                    if (!survey.Game().Draw(pass.VertexName, pass.FragmentName, changed, again, reason)) return false;
                    auto result = ComparePictures(theirs, again);
                    return result.Different * 50 > result.Drawn;
                };
                for (auto const& slot : table.Slots) {
                    if (!slot.Inferred || slot.Kind != ResourceKind::Texture) continue;
                    for (auto const* stage : { &pass.Mac.Vertex, &pass.Mac.Fragment }) {
                        for (auto const& r : *stage) {
                            if (!r.Used || r.Name != slot.Name || r.Kind != ResourceKind::Texture) continue;
                            DrawSetup changed = forGame;
                            for (auto& t : changed.Textures) {
                                if (t.Index == r.Index && t.Vertex == (stage == &pass.Mac.Vertex)) t.Noise ^= 0x5bd1e995;
                            }
                            auto& proof = proofs[slot.Name];
                            (dependsOn(changed) ? proof.first : proof.second)++;
                        }
                    }
                }
                // And a buffer the table paired by order: with nothing in it (no light in any cluster, no numbers
                // of a light), the game's picture is another when the pass's picture hangs on it.
                for (auto const& slot : table.Slots) {
                    if (!slot.Inferred || slot.Kind != ResourceKind::Buffer) continue;
                    for (auto const* stage : { &pass.Mac.Vertex, &pass.Mac.Fragment }) {
                        for (auto const& r : *stage) {
                            if (!r.Used || r.Name != slot.Name || r.Kind != ResourceKind::Buffer) continue;
                            DrawSetup changed = forGame;
                            for (auto& b : changed.Buffers) {
                                if (b.Index == r.Index && b.Vertex == (stage == &pass.Mac.Vertex)) std::fill(b.Bytes.begin(), b.Bytes.end(), (uint8_t)0);
                            }
                            auto& proof = proofs[slot.Name];
                            (dependsOn(changed) ? proof.first : proof.second)++;
                        }
                    }
                }
                for (auto const& attribute : table.Attributes) {
                    if (!attribute.Inferred) continue;
                    for (auto const& a : pass.Mac.Attributes) {
                        if (a.Name != attribute.Name) continue;
                        DrawSetup changed = forGame;
                        for (auto& c : changed.Attributes) if (c.Index == a.Index) c.Noise = 0x5bd1e995;
                        auto& proof = proofs[attribute.Name];
                        (dependsOn(changed) ? proof.first : proof.second)++;
                    }
                }
            }
            bool same = compared.Different * 50 <= compared.Drawn, whole = false;
            // The first four draws leave every whole number at zero; one of the others, which set them, has to agree too.
            for (uint32_t other = 5; same && seed <= 4 && other <= 8; other++) {
                DrawSetup gameSetup, ourSetup;
                DrawResult gameDrew, weDrew;
                std::string whyNot;
                if (!SetupsFor(pass, ours, other, gameSetup, ourSetup, whyNot)
                    || !survey.Game().Draw(pass.VertexName, pass.FragmentName, gameSetup, gameDrew, whyNot)
                    || !compiled->Draw(vName, sources.size() > 1 ? fName : "", ourSetup, weDrew, whyNot)) break;
                auto again = ComparePictures(gameDrew, weDrew);
                if (again.Drawn < 64) continue;
                detail += " whole-numbers drawn " + std::to_string(again.Drawn) + " different " + std::to_string(again.Different)
                    + " depthbits " + std::to_string(again.DepthBits);
                if (again.Different * 50 > again.Drawn) {
                    same = false;
                    whole = true;
                    outcome = "different picture";
                    compared = again;
                }
                break;
            }
            if (!SunShadowDraws().empty() && SunShadowDraws() != "chance") {
                // The pair in each of the eight states the block is made in, and for each member of the game's block
                // whether the game's own picture changes with it in any of them: a member nothing hangs on was not
                // put to the test by this pass.
                MacResource const* block = nullptr;
                for (auto const& g : pass.Mac.Fragment) if (g.Name == "ForwardLightingConstants" && g.Kind == ResourceKind::Buffer) block = &g;
                std::string states;
                std::set<std::string> hangsOn;
                for (uint32_t kind = 1; block && kind <= 8; kind++) {
                    DrawSetup gameSetup, ourSetup;
                    DrawResult gameDrew, weDrew;
                    std::string whyNot;
                    if (!SetupsFor(pass, ours, kind, gameSetup, ourSetup, whyNot)
                        || !survey.Game().Draw(pass.VertexName, pass.FragmentName, gameSetup, gameDrew, whyNot)
                        || !compiled->Draw(vName, sources.size() > 1 ? fName : "", ourSetup, weDrew, whyNot)) {
                        states += " " + std::to_string(kind) + ":not-drawn";
                        continue;
                    }
                    // To a thousandth: what the shadow adds to a pixel is a small part of it.
                    auto both = ComparePictures(gameDrew, weDrew, 1e-3f);
                    states += " " + std::to_string(kind) + (both.Drawn < 64 ? ":too-little" : both.Different * 50 <= both.Drawn ? ":same" : ":APART")
                        + "(" + std::to_string(both.Different) + "/" + std::to_string(both.Drawn) + ")";
                    if (getenv("BG3MSE_SHADERS_SHOW") && both.Different) {
                        // The pixels themselves: where, and the game's number beside ours wherever they are more than a thousandth apart.
                        uint32_t shown = 0;
                        for (uint32_t pixel = 0; pixel < DrawResult::kSize * DrawResult::kSize && shown < 12; pixel++) {
                            std::string values;
                            for (uint32_t t = 0; t < DrawResult::kTargets; t++) {
                                for (int c = 0; c < 4; c++) {
                                    float x = gameDrew.Color[t][pixel * 4 + c], y = weDrew.Color[t][pixel * 4 + c];
                                    if (std::fabs(x - y) <= 1e-3f * (1 + std::max(std::fabs(x), std::fabs(y)))) continue;
                                    char text[64];
                                    snprintf(text, sizeof(text), " [%u.%d] %.5g|%.5g", t, c, x, y);
                                    values += text;
                                }
                            }
                            if (values.empty()) continue;
                            printf("        state %u, pixel %u,%u:%s\n", kind, pixel % DrawResult::kSize, pixel / DrawResult::kSize, values.c_str());
                            shown++;
                        }
                    }
                    if (both.Drawn < 64) continue;
                    for (size_t m = 0; m < block->Members.size(); m++) {
                        auto const& member = block->Members[m];
                        if (member.Type.empty() || hangsOn.contains(member.Name)) continue;
                        size_t end = m + 1 < block->Members.size() ? block->Members[m + 1].Offset : block->Size;
                        DrawSetup changed = gameSetup;
                        for (auto& b : changed.Buffers) {
                            if (b.Vertex || b.Index != block->Index) continue;
                            for (size_t at = member.Offset; at + 4 <= end && at + 4 <= b.Bytes.size(); at += 4) {
                                if (member.Type[0] == 'f') {
                                    float value;
                                    memcpy(&value, b.Bytes.data() + at, 4);
                                    value = value * 1.7f + 0.3f;
                                    memcpy(b.Bytes.data() + at, &value, 4);
                                } else {
                                    uint32_t value;
                                    memcpy(&value, b.Bytes.data() + at, 4);
                                    value = (value + 1) % 5;
                                    memcpy(b.Bytes.data() + at, &value, 4);
                                }
                            }
                        }
                        DrawResult again;
                        if (!survey.Game().Draw(pass.VertexName, pass.FragmentName, changed, again, whyNot)) continue;
                        auto moved = ComparePictures(gameDrew, again, 1e-3f);
                        if (getenv("BG3MSE_SHADERS_SHOW") && moved.Different) printf("        draw %u, %s changed: %u of %u pixels differ\n", kind, member.Name.c_str(), moved.Different, moved.Drawn);
                        if (moved.Different * 50 > moved.Drawn) hangsOn.insert(member.Name);
                    }
                }
                std::string names;
                for (auto const& name : hangsOn) names += (names.empty() ? "" : ",") + name;
                if (block) uses += "bridge:[" + states + " ] hangs-on:[" + names + "] ";
            }
            if (!stagesPath.empty()) {
                auto modules = VulkanStageModules(pass.Parts.Stages[0]);
                SpirvHash stage{};
                std::string unread, hex;
                if (!modules.empty() && SpirvModuleHash(modules[0], stage, unread)) {
                    for (uint8_t b : stage) { char two[3]; snprintf(two, sizeof(two), "%02x", b); hex += two; }
                    auto& proof = stageProofs[hex];
                    if (compared.DepthApart) proof.second++;
                    else if (same) proof.first++;
                    uses += "vs:" + hex.substr(0, 16) + " ";
                }
            }
            if (same) picture.Same++;
            else {
                picture.Different++;
                note("picture differs (" + std::string(compared.Different * 2 > compared.Drawn ? "most" : "some") + " of what was drawn"
                    + (whole ? ", with whole numbers set)" : ")"));
            }
        }
        if (!settled) {
            picture.Empty++;
            // Eight draws with neither pair drawing a pixel is agreement of a kind: a pass this material never draws in.
            outcome = littleDrawn ? "little drawn" : "neither draws";
            detail = "drawn " + std::to_string(littleDrawn) + " different " + std::to_string(littleDifferent);
        }
    }

    depthsDone();
    if (list) fclose(list);
    if (!stagesPath.empty()) {
        // Every vertex stage of the package, the ones never drawn enough to compare as well (0 0).
        for (auto const& pass : passes) {
            auto modules = VulkanStageModules(pass.Parts.Stages[0]);
            SpirvHash stage{};
            std::string unread, hex;
            if (modules.empty() || !SpirvModuleHash(modules[0], stage, unread)) continue;
            for (uint8_t b : stage) { char two[3]; snprintf(two, sizeof(two), "%02x", b); hex += two; }
            stageProofs.try_emplace(hex);
        }
        if (FILE* f = fopen(stagesPath.c_str(), "w")) {
            for (auto const& [hash, proof] : stageProofs) fprintf(f, "%s %u %u\n", hash.c_str(), proof.first, proof.second);
            fclose(f);
        }
        if (FILE* f = fopen((stagesPath + ".draws").c_str(), "w")) {
            fprintf(f, "# stage passes alone-same alone-apart joined-same joined-apart passes-not-joined handed-on-compared handed-on-differs\n");
            for (auto const& [hash, p] : seedProofs) {
                fprintf(f, "%s %u %u %u %u %u %u %u %u\n", hash.c_str(), p.Passes, p.AloneSame, p.AloneApart, p.JoinedSame, p.JoinedApart, p.NotJoined, p.Handed,
                    p.HandedDiffer);
            }
            fclose(f);
        }
        printf("\nwhy a translated vertex function was not drawn with the game's fragment function (numbers replaced by #):\n");
        for (auto const& [reason, count] : notJoinedReasons) printf("  %6u  %s\n", count, reason.c_str());
        printf("\nwhat the two vertex functions hand on, written out by a fragment function of this tool's: compared in %u passes, everything the same "
            "in %u; the things that differ:\n", handedPasses, handedSame);
        for (auto const& [name, count] : handedNames) printf("  %6u  %s\n", count, name.c_str());
        printf("the passes in which it was not compared:\n");
        for (auto const& [state, count] : handedStates) printf("  %6u  %s\n", count, state.c_str());
        printf("\njoined with something handed on that the game's fragment function takes and does not read (the picture is the same "
            "whatever number it is given):\n");
        for (auto const& [names, count] : unreadNames) printf("  %6u  %s\n", count, names.c_str());
    }
    if (!provePath.empty()) {
        if (FILE* f = fopen(provePath.c_str(), "w")) {
            for (auto const& [name, proof] : proofs) fprintf(f, "name\t%s\t%u\t%u\n", name.c_str(), proof.first, proof.second);
            for (auto const& [slot, names] : votes) {
                for (auto const& [name, count] : names) fprintf(f, "slot\t%u\t%u\t%s\t%u\n", slot.first, slot.second, name.c_str(), count);
            }
            fclose(f);
        }
    }
    printf("%-6s %7s %7s %8s %9s %9s %7s\n", "pass", "passes", "unread", "refused", "uncompiled", "different", "same");
    Row total;
    for (auto const& [kind, row] : rows) {
        printf("%-6s %7u %7u %8u %9u %9u %7u  (%.1f%%)\n", kind.c_str(), row.Passes, row.Unread, row.Refused, row.NotCompiled, row.Different,
            row.Same, row.Passes ? 100.0 * row.Same / row.Passes : 0.0);
        total.Passes += row.Passes; total.Unread += row.Unread; total.Refused += row.Refused; total.NotCompiled += row.NotCompiled;
        total.Different += row.Different; total.Same += row.Same;
    }
    printf("%-6s %7u %7u %8u %9u %9u %7u  (%.1f%%)\n", "all", total.Passes, total.Unread, total.Refused, total.NotCompiled, total.Different,
        total.Same, total.Passes ? 100.0 * total.Same / total.Passes : 0.0);
    if (draw) {
        printf("\nthe same draw through the game's functions and ours, for the passes with the same interface:\n"
               "%-6s %9s %9s %14s %15s %9s\n", "pass", "same", "different", "nothing drawn", "not comparable", "no draw");
        for (auto const& [kind, row] : pictures) {
            printf("%-6s %9u %9u %14u %15u %9u\n", kind.c_str(), row.Same, row.Different, row.Empty, row.NotComparable, row.NotDrawn);
        }
    }
    uint32_t whole = 0;
    for (auto const& [material, tally] : materials) whole += tally.first == tally.second;
    printf("\n%u of %zu materials have every pass%s the same; in %u passes the game's function does without a texture or sampler ours takes\n",
        whole, materials.size(), every > 1 ? " that was sampled" : "", withExtra);
    if (draw && depthMaterials) {
        printf("of %u materials drawn in more than one pass, the passes do not all put a pixel at the same depth to the last bit in %u "
            "with the game's functions and in %u with ours\n", depthMaterials, gameApart, oursApart);
    }
    std::vector<std::pair<uint32_t, std::string>> sorted;
    for (auto const& [reason, entry] : reasons) sorted.emplace_back(entry.first, reason + "\n           first: " + entry.second);
    std::sort(sorted.rbegin(), sorted.rend());
    printf("\nwhy not (numbers replaced by #):\n");
    for (size_t i = 0; i < sorted.size() && i < 40; i++) printf("%7u  %s\n", sorted[i].first, sorted[i].second.c_str());
    return 0;
}

}
