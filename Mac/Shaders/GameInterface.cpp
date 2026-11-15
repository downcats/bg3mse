#include <stdafx.h>
#include <Platform/CodeSearch.h>
#include <Shaders/GameInterface.h>
#include <Shaders/PakFile.h>
#include <Shaders/SpirvIdentity.h>

#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bg3se::mac {

namespace {

constexpr char kCreateShader[] = "__ZN2rf9GPUDevice12CreateShaderEONS_4bshd6ShaderE";
constexpr char kNewFunctionWithName[] = "_objc_msgSend$newFunctionWithName:";
// 2: with how many textures an argument of several holds.
constexpr char kCacheHeader[] = "bg3mse Mac interface 2";

}

ShaderCreationCheck CheckShaderCreation(GameImage const& image)
{
    ShaderCreationCheck check;
    uint64_t create = image.FindVA(kCreateShader), ask = image.FindVA(kNewFunctionWithName);
    if (!create) { check.Problem = "the game has no rf::GPUDevice::CreateShader"; return check; }
    if (!ask) { check.Problem = "the game asks no Metal library for a function by name"; return check; }
    uint64_t end = image.NextSymbolVA(create);
    check.Requests = end > create ? (uint32_t)CallsTo(image, create, end, ask).size() : 0;
    // A vertex and a fragment function, each asked of one library at least.
    if (check.Requests < 2) check.Problem = "rf::GPUDevice::CreateShader does not ask a Metal library for the functions a stub names";
    return check;
}

namespace {

constexpr char kSetupForwardLightingConstants[] = "__ZN2ls29SetupForwardLightingConstantsERN2rf2sb24ForwardLightingConstantsEPKNS_14GameRenderViewEPKNS_25CascadedShadowBufferStageEPKNS_23CascadedShadowMaskStageEPKNS_8DirLightEPKNS_9CullStageEPKNS_9SSAOStageEPKNS_12SSAOSettingsEPKNS_21LocalLightShadowStageE";

// The instructions of ls::SetupForwardLightingConstants the relations rest
// on. x19 is the block, x22 and x23 the two shadow stages. None of them names
// an address; the two loops end in a branch to their own beginning.
struct Stretch
{
    char const* What;
    std::span<uint32_t const> Words;
};

// g_ShadowEnabled = shadows are on ? 1 : 0, at +0xc.
constexpr uint32_t kEnabled[] = { 0x7100013f, 0x1e201c20, 0xbd000e60 };
// The count of cascades, a whole number, at +0x170.
constexpr uint32_t kCount[] = { 0xbd666ac0, 0x5e21d800, 0x1e380008, 0xb9017268 };
// One over the fade at +0x18, the far planes at +0x20, one over the widths at +0x30.
constexpr uint32_t kFadeFarWidths[] = { 0xbd614ae0, 0x1e2e1001, 0x1e201821, 0xbd001a61, 0x3dc91ec1, 0x3d800a61, 0x3dc922c1, 0x4f03f602,
    0x6e21fc41, 0x3d800e61 };
// For each cascade: far / (fade * width / 2) at +0x140, (bias / far) * mix(width / width0, far / far0, 0.75)
// at +0x150, (2 / (width / width0)) / the map's size at +0x160.
constexpr uint32_t kPerCascade[] = { 0x92401d6f, 0xd37ef5ef, 0xbc6f6930, 0xbc6f6951, 0x1e250a31, 0x1e310811, 0x1e311a11, 0xbc2f6991,
    0xbc6f6951, 0x1e221a31, 0x1e211a12, 0x1e313a52, 0x1f064652, 0x1e301870, 0x1e320a10, 0xbc2f69b0, 0x1e3118f0, 0x1e241a10, 0x1100056b,
    0x6b2b011f, 0xbc2f69d0, 0x54fffd6c };
// The same for four cascades at once.
constexpr uint32_t kFourCascades[] = { 0x3dc009d4, 0x3cc105b5, 0x6e25feb6, 0x6e26fe97, 0x4eb6d6f7, 0x6e36fe78, 0x4e37ce56, 0x6e31deb5,
    0x4f8092b5, 0x6e35fe95, 0x6e34fcf4, 0x6e36de94, 0xad0a51d5, 0x6e30ff14, 0x3d8059d4, 0x910041ce, 0xf100116b, 0x54fffde1 };
// With shadows off: no cascade, and a hundred-thousandth for the fade and for each cascade's three numbers.
constexpr uint32_t kOffCountAndFade[] = { 0xb901727f, 0xf9000a7f, 0x5298b598, 0x72a6e4f8, 0xb9001a78 };
constexpr uint32_t kOffPerCascade[] = { 0x4e040f00, 0xad0a0260, 0x3d805a60 };

constexpr Stretch kSunShadowStretches[] = {
    { "whether shadows are on", kEnabled },
    { "the count of cascades", kCount },
    { "the fade, the far planes and the widths", kFadeFarWidths },
    { "each cascade's bias and fade", kPerCascade },
    { "four cascades' bias and fade at once", kFourCascades },
    { "the count and the fade with shadows off", kOffCountAndFade },
    { "each cascade's numbers with shadows off", kOffPerCascade },
};

}

char const* SunShadowFunctionName() { return kSetupForwardLightingConstants; }

std::vector<std::span<uint32_t const>> SunShadowStretches()
{
    std::vector<std::span<uint32_t const>> out;
    for (auto const& stretch : kSunShadowStretches) out.push_back(stretch.Words);
    return out;
}

SunShadowCheck CheckSunShadowBridge(GameImage const& image)
{
    SunShadowCheck check;
    uint64_t begin = image.FindVA(kSetupForwardLightingConstants);
    if (!begin) { check.Problem = "the game has no ls::SetupForwardLightingConstants with the arguments this was read from"; return check; }
    uint64_t end = image.NextSymbolVA(begin);
    if (end <= begin) { check.Problem = "where ls::SetupForwardLightingConstants ends is not known"; return check; }
    for (auto const& stretch : kSunShadowStretches) {
        std::vector<MaskedWord> pattern;
        for (uint32_t word : stretch.Words) pattern.push_back({ word });
        size_t found = FindPattern(image, begin, end, pattern).size();
        if (found != 1) {
            check.Problem = std::string("ls::SetupForwardLightingConstants does not fill ") + stretch.What + " as it did when the bridge was read from it ("
                + (found ? "found " + std::to_string(found) + " times" : "not found") + ")";
            return check;
        }
    }
    return check;
}

bool SunShadowMembersWhereTheEngineWritesThem(MacInterface const& mac, std::string& why)
{
    auto block = mac.Resource("ForwardLightingConstants", ResourceKind::Buffer);
    if (!block || !mac.HasFixedLayout(*block)) { why = "the game's functions have no one layout for ForwardLightingConstants"; return false; }
    struct Where { char const* Name; uint32_t Offset; char const* Type; };
    static constexpr Where kWhere[] = {
        { "g_ShadowEnabled", 0xc, "f1" }, { "g_ShadowFadeAmount", 0x18, "f1" }, { "g_ShadowFarPlanes", 0x20, "f4" },
        { "g_ShadowInvFrustumWidths", 0x30, "f4" }, { "g_ShadowInvFadeWorldSize", 0x140, "f4" }, { "g_ShadowCascadeBias", 0x150, "f4" },
        { "g_NumActiveCascades", 0x170, "u1" },
    };
    for (auto const& where : kWhere) {
        bool found = std::any_of(block->Members.begin(), block->Members.end(),
            [&](auto const& m) { return m.Name == where.Name && m.Offset == where.Offset && m.Type == where.Type; });
        if (!found) {
            why = std::string("the game's functions do not have ") + where.Name + " where ls::SetupForwardLightingConstants writes it";
            return false;
        }
    }
    return true;
}

namespace {

constexpr char kAssignmentKernel[] = "__ZN2ls24ClusteredAssignmentStage15ExecuteWTKernelEv";
constexpr char kAssignDiffuse[] = "__ZN2ls24ClusteredAssignmentStage23AssignDiffuseLightProbeEONS0_22DiffuseProbeAssignment6AssignE";

// A diffuse cube's layer into a probe's first number: at 3 + 9 * (first or second), nine bits, the layer plus one.
//   add w, i, i, lsl #3; add w, w, #3; mov m, #0x1ff; lsl m, m, w; ldr v, [p]; bic m, v, m; add l, l, #1; lsl l, l, w; orr; str
constexpr uint32_t kDiffuseLayer[] = { 0x0b090d29, 0x11000d29, 0x52803feb, 0x1ac9216b, 0xb940010c, 0x0a2b018b, 0x1100054a, 0x1ac92149, 0x2a090169,
    0xb9000109 };
// The same once more, with other registers (and once in the function that assigns one on its own).
constexpr uint32_t kDiffuseLayerAgain[] = { 0x0b0a0d4a, 0x11000d4a, 0x52803feb, 0x1aca216b, 0xb940012c, 0x0a2b018b, 0x11000508, 0x1aca2108, 0x2a080168,
    0xb9000128 };
// A specular cube into a probe's second number: at 11 * (first or second), eleven bits: the array's number, and the
// layer plus one two bits above it.
//   mov t, #11; mul w, i, t; mov m, #0x7ff; lsl m, m, w; ldr v, [p]; bic m, v, m; lsl a, a, w; add l, l, #1; add w, w, #2; lsl l, l, w; orr; orr; str
constexpr uint32_t kSpecularArrayAndLayer[] = { 0x5280016d, 0x1b0d7d6b, 0x5280ffed, 0x1acb21ad, 0xb940018e, 0x0a2d01cd, 0x1acb2129, 0x1100054a,
    0x1100096b, 0x1acb214a, 0x2a090149, 0x2a0d0129, 0xb9000189 };
constexpr uint32_t kSpecularArrayAndLayerAgain[] = { 0x5280016b, 0x1b0b7d4a, 0x5280ffeb, 0x1aca216b, 0xb940012c, 0x0a2b018b, 0x1aca238c, 0x110006ed,
    0x1100094a, 0x1aca21aa, 0x2a0c014a, 0x2a0b014a, 0xb900012a };
// A cookie into a light's number: the layer plus one from bit 4 (lsl l, l, #4; add l, l, #0x10), with the array's
// number times two, into what is there.
constexpr uint32_t kCookieLayer[] = { 0x531c6f09, 0x11004129, 0xb94113ea, 0x2a0a0129, 0xb940010a, 0x2a0a0129, 0xb9000109 };
constexpr uint32_t kCookieLayerAgain[] = { 0x531c6d4a, 0x1100414a, 0x2a090149, 0xb940016a, 0x2a0a0129, 0xb9000169 };
// The array's number times two: kept for the first of those (lsl w, a, #1; str), counted up by two an array for the second.
constexpr uint32_t kCookieArray[] = { 0x531f7ac8, 0xb90113e8 };
constexpr uint32_t kCookieArrayAgain[] = { 0x52800009, 0xd280000b, 0xb86b790a, 0x3100055f, 0x54000441, 0x9100056b, 0x11000929, 0xeb0b029f, 0x54ffff41 };

struct TextureTableStretch
{
    char const* Function;
    char const* What;
    std::span<uint32_t const> Words;
};
constexpr TextureTableStretch kTextureTableStretches[] = {
    { kAssignmentKernel, "a diffuse cube's layer", kDiffuseLayer },
    { kAssignmentKernel, "a diffuse cube's layer (the second place)", kDiffuseLayerAgain },
    { kAssignmentKernel, "a specular cube's array and layer", kSpecularArrayAndLayer },
    { kAssignmentKernel, "a specular cube's array and layer (the second place)", kSpecularArrayAndLayerAgain },
    { kAssignmentKernel, "a cookie's layer", kCookieLayer },
    { kAssignmentKernel, "a cookie's layer (the second place)", kCookieLayerAgain },
    { kAssignmentKernel, "a cookie's array", kCookieArray },
    { kAssignmentKernel, "a cookie's array (the second place)", kCookieArrayAgain },
    { kAssignDiffuse, "a diffuse cube's layer", kDiffuseLayerAgain },
};

}

std::vector<TextureTableCode> TextureTableFunctions()
{
    std::vector<TextureTableCode> out;
    for (auto const& stretch : kTextureTableStretches) {
        if (out.empty() || std::string_view(out.back().Function) != stretch.Function) out.push_back({ stretch.Function, {} });
        out.back().Stretches.push_back(stretch.Words);
    }
    return out;
}

TextureTableCheck CheckTextureTableBridge(GameImage const& image)
{
    TextureTableCheck check;
    for (auto const& stretch : kTextureTableStretches) {
        bool kernel = std::string_view(stretch.Function) == kAssignmentKernel;
        char const* name = kernel ? "ls::ClusteredAssignmentStage::ExecuteWTKernel" : "ls::ClusteredAssignmentStage::AssignDiffuseLightProbe";
        uint64_t begin = image.FindVA(stretch.Function);
        if (!begin) { check.Problem = std::string("the game has no ") + name + " as this was read from"; return check; }
        uint64_t end = image.NextSymbolVA(begin);
        if (end <= begin) { check.Problem = std::string("where ") + name + " ends is not known"; return check; }
        std::vector<MaskedWord> pattern;
        for (uint32_t word : stretch.Words) pattern.push_back({ word });
        size_t found = FindPattern(image, begin, end, pattern).size();
        if (found != 1) {
            check.Problem = std::string(name) + " does not pack " + stretch.What + " as it did when the table of textures was read from it ("
                + (found ? "found " + std::to_string(found) + " times" : "not found") + ")";
            return check;
        }
    }
    return check;
}

bool TextureTableAsRead(MacInterface const& mac, InterfaceTable const& table, std::string& why)
{
    if (!table.Textures) { why = "the interface table has no table of textures"; return false; }
    auto const& textures = *table.Textures;
    // The Mac names of what the table of textures is made of: the two buffers by their places, the five textures
    // by theirs.
    auto nameAt = [&](uint32_t set, uint32_t binding) {
        auto slot = table.Slot(set, binding);
        return slot ? slot->Name : std::string();
    };
    auto member = [&](std::string const& buffer, uint32_t index, char const* name, uint32_t offset, uint32_t size) {
        auto block = buffer.empty() ? nullptr : mac.Resource(buffer, ResourceKind::Buffer);
        if (!block || !mac.HasFixedLayout(*block) || block->Size != size || index >= block->Members.size() || block->Members[index].Name != name
            || block->Members[index].Type != "u1" || block->Members[index].Offset != offset) {
            why = "the game's functions do not have " + std::string(name) + " where the table of textures was read from";
            return false;
        }
        return true;
    };
    std::string probes = nameAt(textures.ProbesSet, textures.ProbesBinding), lights = nameAt(textures.LightsSet, textures.LightsBinding);
    if (!member(probes, textures.DiffuseMember, "ProbeShapeType_Diff", 136, 144) || !member(probes, textures.SpecularMember, "Spec", 140, 144)
        || !member(lights, textures.CookieMember, "LightCookiePreExpose", 12, 128)) return false;
    struct Texture { char const* Type; uint32_t Count; };
    const Texture expected[5] = { { "cubearray", 0 }, { "cubearray", 0 }, { "cubearray[]", textures.SpecularArrays },
        { "cubearray[]", textures.SpecularArrays }, { "2darray[]", textures.CookieArrays } };
    for (uint32_t i = 0; i < 5; i++) {
        std::string name = nameAt(textures.NewSet, textures.NewBindings[i]);
        auto texture = name.empty() ? nullptr : mac.Resource(name, ResourceKind::Texture);
        if (!texture || texture->Type != expected[i].Type || texture->Size != expected[i].Count) {
            why = "the game's functions do not take " + (name.empty() ? std::string("a texture of the table") : name) + " as "
                + (expected[i].Count ? std::to_string(expected[i].Count) + " arrays" : "one array");
            return false;
        }
    }
    return true;
}

InterfaceTable InterfaceFor(GameImage const& image, MacInterface const& mac, BridgeProblems& problems)
{
    problems = {};
    InterfaceTable table = BuiltInInterface();

    auto sun = CheckSunShadowBridge(image);
    if (!sun.Ok()) problems.SunShadow = sun.Problem;
    else SunShadowMembersWhereTheEngineWritesThem(mac, problems.SunShadow);
    // Those two checks are for the lighting constants. A member of another
    // block that is worked out has nothing here that shows it right.
    for (auto& block : table.Blocks) {
        if (block.Name != "ForwardLightingConstants" || !problems.SunShadow.empty()) block.Derived.clear();
    }

    if (table.Textures) {
        auto textures = CheckTextureTableBridge(image);
        if (!textures.Ok()) problems.TextureTable = textures.Problem;
        else TextureTableAsRead(mac, table, problems.TextureTable);
        if (!problems.TextureTable.empty()) table = table.WithoutTextureTable();
    }
    return table;
}

InterfaceTable InterfaceForBundle(std::string const& appBundle, MacInterface const& mac, BridgeProblems& problems)
{
    auto none = [&](std::string const& why) {
        problems.SunShadow = problems.TextureTable = why;
        return BuiltInInterface().WithoutDerived().WithoutTextureTable();
    };
    std::string binary = appBundle + "/Contents/MacOS/Baldur's Gate 3";
    int fd = open(binary.c_str(), O_RDONLY);
    struct stat st{};
    void* data = fd >= 0 && fstat(fd, &st) == 0 ? mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    if (fd >= 0) close(fd);
    if (data == MAP_FAILED) return none("the game's binary cannot be read (" + binary + ")");
    InterfaceTable table;
    {
        GameImage image;
        if (image.OpenFile({ static_cast<uint8_t const*>(data), (size_t)st.st_size })) table = InterfaceFor(image, mac, problems);
        else table = none("the game's binary has no symbol table that can be read");
    }
    munmap(data, (size_t)st.st_size);
    return table;
}

bool SurveyMacInterface(std::string const& appBundle, MacInterface& mac, std::string& why)
{
    auto pak = PakFile::Open(appBundle + "/Contents/Data/Materials.pak", &why);
    auto game = pak ? MetalFunctions::OpenGame(appBundle, why) : nullptr;
    if (!game) return false;

    // The names first, then what each pair takes: asking Metal for that is
    // most of the work (it makes a pipeline of the pair), and Metal answers
    // several threads at once. In the order of the package whatever thread
    // was first, so that the result is the same every time.
    struct Pair { std::string Vertex, Fragment; };
    std::vector<Pair> pairs;
    for (auto const& entry : pak->Entries()) {
        if (!std::string_view(entry.Name).ends_with("_Metal.bshd")) continue;
        auto stub = pak->Read(entry);
        ContainerParts parts;
        auto name = [](std::span<uint8_t const> blob) {
            std::string s((char const*)blob.data(), blob.size());
            while (!s.empty() && (s.back() == '\n' || s.back() == 0)) s.pop_back();
            return s;
        };
        if (stub && ParseContainer(*stub, parts) && parts.Version == 2) pairs.push_back({ name(parts.Stages[0]), name(parts.Stages[4]) });
    }

    constexpr size_t kThreads = 4, kBatch = 256;
    for (size_t begin = 0; begin < pairs.size(); begin += kBatch) {
        size_t count = std::min(kBatch, pairs.size() - begin);
        std::vector<MacPass> passes(count);
        std::vector<uint8_t> read(count, 0);
        std::vector<std::thread> threads;
        for (size_t t = 0; t < kThreads; t++) {
            threads.emplace_back([&, t] {
                for (size_t i = t; i < count; i += kThreads) {
                    std::string ignored;
                    read[i] = game->Reflect(pairs[begin + i].Vertex, pairs[begin + i].Fragment, passes[i], ignored);
                }
            });
        }
        for (auto& thread : threads) thread.join();
        for (size_t i = 0; i < count; i++) if (read[i]) mac.Add(passes[i]);
    }
    if (!mac.Passes()) why = "none of the game's Metal functions could be read";
    return mac.Passes() > 0;
}

std::string MacInterfaceStamp(std::string const& appBundle)
{
    std::string stamp;
    for (char const* file : { "Shaders/Metal/Core.metallib", "Shaders/Metal/Materials.metallib", "Materials.pak" }) {
        struct stat st{};
        if (stat((appBundle + "/Contents/Data/" + file).c_str(), &st) != 0) return {};
        stamp += std::string(file) + ":" + std::to_string(st.st_size) + ":" + std::to_string(st.st_mtimespec.tv_sec) + ";";
    }
    return stamp;
}

std::optional<MacInterface> LoadMacInterface(std::string const& file, std::string const& stamp)
{
    if (stamp.empty()) return {};
    std::ifstream in(file, std::ios::binary);
    std::string header, kept;
    if (!std::getline(in, header) || header != kCacheHeader || !std::getline(in, kept) || kept != stamp) return {};
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return MacInterface::Parse(text);
}

namespace {

constexpr std::string_view kPartialExt = ".partial";

// What a writer of `file` that was ended in the middle left beside it, once it is an hour old: nobody is
// still writing that.
void ClearOldPartials(std::string const& file)
{
    size_t slash = file.rfind('/');
    std::string directory = slash == std::string::npos ? "." : file.substr(0, slash);
    std::string start = (slash == std::string::npos ? file : file.substr(slash + 1)) + ".";
    DIR* dir = opendir(directory.c_str());
    if (!dir) return;
    time_t now = time(nullptr);
    while (dirent* entry = readdir(dir)) {
        std::string_view name = entry->d_name;
        if (!name.starts_with(start) || !name.ends_with(kPartialExt) || name.size() == start.size() + kPartialExt.size()) continue;
        std::string path = directory + "/" + std::string(name);
        struct stat st;
        if (lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && now - st.st_mtimespec.tv_sec > 3600) unlink(path.c_str());
    }
    closedir(dir);
}

}

bool SaveMacInterface(std::string const& file, std::string const& stamp, MacInterface const& mac)
{
    if (stamp.empty()) return false;
    ClearOldPartials(file);
    // Written beside and moved into place: a run that ends in the middle leaves no half file. Under a name
    // of this process's own: the installer and a game that starts may both be writing it.
    std::string partial = file + "." + std::to_string(getpid()) + std::string(kPartialExt);
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out << kCacheHeader << "\n" << stamp << "\n" << mac.Serialize();
        if (!out.flush()) {
            out.close();
            unlink(partial.c_str());
            return false;
        }
    }
    if (rename(partial.c_str(), file.c_str()) == 0) return true;
    unlink(partial.c_str());
    return false;
}

namespace {

// Which way of writing the file a header line names: this build's own is kCacheHeader's number. 0 for a line
// that is no header of the file's.
unsigned long HeaderVersion(std::string_view header)
{
    static constexpr std::string_view kWords = "bg3mse Mac interface ";
    if (!header.starts_with(kWords) || header.size() == kWords.size() || header.size() > kWords.size() + 6) return 0;
    unsigned long version = 0;
    for (char c : header.substr(kWords.size())) {
        if (c < '0' || c > '9') return 0;
        version = version * 10 + (unsigned long)(c - '0');
    }
    return version;
}

// A file a later build of bg3mse wrote.
bool WrittenByNewerBuild(std::string const& file)
{
    std::ifstream in(file, std::ios::binary);
    std::string header;
    return std::getline(in, header) && HeaderVersion(header) > HeaderVersion(kCacheHeader);
}

}

bool MacInterfaceToBeRead(std::string const& appBundle, std::string const& file)
{
    std::string stamp = MacInterfaceStamp(appBundle);
    return !stamp.empty() && !LoadMacInterface(file, stamp) && !WrittenByNewerBuild(file);
}

EnsuredMacInterface EnsureMacInterface(std::string const& appBundle, std::string const& file, std::string& why)
{
    EnsuredMacInterface ensured;
    // Whoever was stopped while writing is cleared up after here, where every start of the game comes by.
    ClearOldPartials(file);
    std::string stamp = MacInterfaceStamp(appBundle);
    if (stamp.empty()) {
        why = "the game's shader files are not in " + appBundle;
        return ensured;
    }
    if ((ensured.Interface = LoadMacInterface(file, stamp))) {
        ensured.What = EnsuredMacInterface::How::Kept;
        return ensured;
    }
    if (WrittenByNewerBuild(file)) {
        ensured.What = EnsuredMacInterface::How::OfNewerBuild;
        return ensured;
    }

    MacInterface mac;
    if (!SurveyMacInterface(appBundle, mac, why)) return ensured;
    // What was read is kept as the game's of before the read: a game that was updated meanwhile gets no file.
    if (MacInterfaceStamp(appBundle) != stamp) {
        why = "the game's shader files changed while they were read";
        return ensured;
    }
    if (!SaveMacInterface(file, stamp, mac)) {
        why = "cannot write " + file;
        return ensured;
    }
    ensured.What = EnsuredMacInterface::How::Read;
    ensured.Interface = std::move(mac);
    return ensured;
}

}
