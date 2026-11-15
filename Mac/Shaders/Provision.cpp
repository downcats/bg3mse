#include <stdafx.h>
#include <Shaders/Provision.h>

#include <algorithm>

extern "C" {
#include <Shaders/identity.h>
#include <Shaders/lsf.h>
#include <Shaders/lsf_relayout.h>
}

namespace bg3se::mac {

namespace {

constexpr std::string_view kPasses[] = {
    "ST_FOR", "STI_FOR", "ST_DEF", "STI_DEF", "ST_DEP", "STI_DEP", "ST_DEPS", "STI_DEPS",
    "ST_DEPST", "STI_DEPST", "ST_VEL", "STI_VEL", "ST_EMI", "STI_EMI", "ST_BAKE",
};
constexpr size_t kPassCount = std::size(kPasses);

constexpr std::string_view kMaterialExt = ".lsf";
constexpr std::string_view kVulkanExt = "_Vulkan.bshd";
constexpr std::string_view kMetalExt = "_Metal.bshd";

std::string PassFile(std::string_view stem, size_t pass, std::string_view ext)
{
    std::string name;
    name.reserve(stem.size() + 1 + kPasses[pass].size() + ext.size());
    name.append(stem).append("_").append(kPasses[pass]).append(ext);
    return name;
}

std::string_view BaseName(std::string_view path)
{
    auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

struct ParsedLsf
{
    LsfDoc Doc{};
    bool Ok{ false };

    ParsedLsf(std::vector<uint8_t> const& bytes, std::string& error)
    {
        char err[256] = "";
        Ok = lsf_parse(bytes.data(), bytes.size(), &Doc, err, sizeof(err));
        if (!Ok) error = err;
    }
    ~ParsedLsf() { if (Ok) lsf_free(&Doc); }
    ParsedLsf(ParsedLsf const&) = delete;
    ParsedLsf& operator = (ParsedLsf const&) = delete;
};

// The mod's .lsf with the vanilla Mac material's structure and the mod's values.
std::optional<std::vector<uint8_t>> Relayout(std::vector<uint8_t> const& mod, std::vector<uint8_t> const& mac, std::string& error)
{
    ParsedLsf modDoc(mod, error);
    if (!modDoc.Ok) { error = "its .lsf does not parse: " + error; return {}; }
    ParsedLsf macDoc(mac, error);
    if (!macDoc.Ok) { error = "the vanilla Mac .lsf does not parse: " + error; return {}; }

    uint8_t* out = nullptr;
    size_t outSize = 0;
    LsfRelayoutStats stats{};
    char err[256] = "";
    if (!lsf_relayout(&modDoc.Doc, &macDoc.Doc, &out, &outSize, &stats, err, sizeof(err))) {
        error = std::string("the .lsf rewrite refused: ") + err;
        return {};
    }
    std::vector<uint8_t> result(out, out + outSize);
    free(out);

    // What is served must at least read back.
    ParsedLsf check(result, error);
    if (!check.Ok) { error = "the rewritten .lsf does not parse: " + error; return {}; }
    return result;
}

struct Builder
{
    MaterialSource const& Vanilla;
    std::span<MaterialSource const> Mods;
    ContainerIdentity const& Identity;
    ProvisionTranslator const& Translate;
    bool TranslateFirst;
    ProvisionMap& Map;

    Builder(MaterialSource const& vanilla, std::span<MaterialSource const> mods, ContainerIdentity const& identity,
        ProvisionTranslator const& translator, bool translateFirst, ProvisionMap& map)
        : Vanilla(vanilla), Mods(mods), Identity(identity), Translate(translator), TranslateFirst(translateFirst), Map(map)
    {}

    // hash of "<vanilla material stem>_<PASS>" -> (vanilla .lsf, pass)
    std::unordered_map<uint64_t, std::pair<std::string const*, size_t>> stems_;
    // every _Metal.bshd a mod ships -> the package that ships it (the last one)
    std::unordered_map<std::string_view, std::string const*> modStubs_;
    // material path -> its winning decision so far
    std::unordered_map<std::string_view, uint32_t> winner_;

    void Index()
    {
        for (auto const& name : Vanilla.Names) {
            if (!name.ends_with(kMaterialExt)) continue;
            std::string_view stem(name.data(), name.size() - kMaterialExt.size());
            for (size_t p = 0; p < kPassCount; p++) {
                stems_.try_emplace(PassStemHash(PassFile(stem, p, {})), &name, p);
            }
        }
        for (auto const& mod : Mods) {
            for (auto const& name : mod.Names) {
                if (name.ends_with(kMetalExt)) modStubs_[name] = &mod.Label;
            }
        }
    }

    ProvisionDecision& Decide(MaterialSource const& mod, std::string const& material, uint32_t passes)
    {
        uint32_t index = (uint32_t)Map.Decisions.size();
        auto& d = Map.Decisions.emplace_back();
        d.Material = material;
        d.Package = mod.Label;
        d.Passes = passes;

        // A later package's copy of a material path replaces an earlier one's, whatever was decided for it.
        auto [it, first] = winner_.try_emplace(Map.Decisions[index].Material, index);
        if (!first) {
            auto& earlier = Map.Decisions[it->second];
            earlier.Kind = ProvisionKind::Superseded;
            earlier.Reason = d.Package + " ships the same material and loads later";
            it->second = index;
        }
        return d;
    }

    // What to serve for a twin. Collected per decision and only kept for the winners.
    std::unordered_map<uint32_t, std::vector<ServedFile>> pending_;

    // Hands the material to the translator; on success it is decided.
    bool Translated(MaterialSource const& mod, std::string const& material, std::string_view stem, ProvisionDecision& d, uint32_t decision,
        std::string& why)
    {
        auto translated = Translate(mod, material, why);
        if (!translated) return false;
        std::vector<ServedFile> files;
        files.push_back({ material, std::move(translated->Lsf), decision, true });
        d.Stubs = 0;
        for (auto& [pass, stub] : translated->Stubs) {
            files.push_back({ std::string(stem) + "_" + pass + std::string(kMetalExt), std::move(stub), decision, false });
            d.Stubs++;
        }
        d.Reason = std::move(translated->Note);
        d.Kind = ProvisionKind::Translated;
        pending_.emplace(decision, std::move(files));
        return true;
    }

    void DecideMaterial(MaterialSource const& mod, std::string const& material)
    {
        std::string_view stem(material.data(), material.size() - kMaterialExt.size());

        bool present[kPassCount];
        uint32_t passes = 0;
        for (size_t p = 0; p < kPassCount; p++) {
            present[p] = mod.Names.contains(PassFile(stem, p, kVulkanExt));
            if (present[p]) passes++;
        }
        if (!passes) {
            // Not a shader material, unless it replaces a vanilla material's .lsf:
            // that file is packed for Windows and would need the same rewrite.
            if (Vanilla.Names.contains(material)) {
                auto& d = Decide(mod, material, 0);
                d.Reason = "it replaces a vanilla material's .lsf without shipping shaders, which is not handled yet";
            }
            return;
        }

        auto& d = Decide(mod, material, passes);
        uint32_t decision = (uint32_t)(&d - Map.Decisions.data());

        for (size_t p = 0; p < kPassCount; p++) {
            auto stub = PassFile(stem, p, kMetalExt);
            if (auto by = modStubs_.find(stub); by != modStubs_.end()) {
                d.Kind = ProvisionKind::Skipped;
                d.Reason = *by->second + " ships its Metal shaders (" + std::string(BaseName(stub)) + ")";
                return;
            }
        }

        if (Translate && TranslateFirst) {
            std::string why;
            if (Translated(mod, material, stem, d, decision, why)) return;
        }

        // Every container must be a vanilla container of one and the same vanilla material.
        std::vector<std::string const*> candidates;
        bool first = true;
        uint32_t missed = 0;
        for (size_t p = 0; p < kPassCount; p++) {
            if (!present[p]) continue;
            auto name = PassFile(stem, p, kVulkanExt);
            auto container = mod.Read(name);
            if (!container) { d.Reason = "cannot read " + name; return; }

            std::vector<std::string const*> here;
            for (uint64_t stemHash : Identity(*container)) {
                auto it = stems_.find(stemHash);
                if (it != stems_.end() && it->second.second == p) here.push_back(it->second.first);
            }
            if (here.empty()) { missed++; continue; }
            d.Matched++;

            if (first) {
                candidates = std::move(here);
                first = false;
            } else {
                std::erase_if(candidates, [&](auto c) { return std::find(here.begin(), here.end(), c) == here.end(); });
            }
            if (candidates.empty()) {
                d.Reason = "its containers are vanilla containers of different materials (" + std::string(kPasses[p]) + " is not of the one the others are)";
                return;
            }
        }
        if (missed) {
            d.Reason = std::to_string(missed) + " of " + std::to_string(passes) + " containers are not vanilla shaders, as shipped or compiled again";
            if (!Translate) {
                d.Reason += "; the game has no Metal function for them";
                return;
            }
            // Its own shaders, then, translated: all of the material or none of it.
            std::string why;
            if (!Translated(mod, material, stem, d, decision, why)) d.Reason += "; not translated: " + why;
            return;
        }
        if (candidates.size() != 1) {
            d.Reason = "its containers are the shaders of " + std::to_string(candidates.size()) + " vanilla materials";
            return;
        }

        std::string const& vanillaMaterial = *candidates[0];
        std::string_view vanillaStem(vanillaMaterial.data(), vanillaMaterial.size() - kMaterialExt.size());
        d.Vanilla = vanillaMaterial;

        auto modLsf = mod.Read(material);
        auto macLsf = Vanilla.Read(vanillaMaterial);
        if (!modLsf || !macLsf) { d.Reason = "cannot read " + (modLsf ? vanillaMaterial : material); return; }

        std::string error;
        auto rewritten = Relayout(*modLsf, *macLsf, error);
        if (!rewritten) { d.Reason = error; return; }

        std::vector<ServedFile> files;
        files.push_back({ material, std::move(*rewritten), decision, true });
        for (size_t p = 0; p < kPassCount; p++) {
            // A pass the Mac game has no stub for (ST_BAKE) is one its .lsf has no description for either.
            auto vanillaStub = PassFile(vanillaStem, p, kMetalExt);
            if (!Vanilla.Names.contains(vanillaStub)) continue;
            auto stub = Vanilla.Read(vanillaStub);
            if (!stub) { d.Reason = "cannot read " + vanillaStub; return; }
            files.push_back({ PassFile(stem, p, kMetalExt), std::move(*stub), decision, false });
            d.Stubs++;
        }

        d.Kind = ProvisionKind::Twin;
        pending_.emplace(decision, std::move(files));
    }

    void Run()
    {
        Index();
        // Decisions must not move while winner_ holds views of their Material strings.
        size_t materials = 0;
        for (auto const& mod : Mods) {
            for (auto const& name : mod.Names) if (name.ends_with(kMaterialExt)) materials++;
        }
        Map.Decisions.reserve(materials);

        for (auto const& mod : Mods) {
            // A set has no order; decide in a stable one so the log and the tests are reproducible.
            std::vector<std::string const*> names;
            for (auto const& name : mod.Names) if (name.ends_with(kMaterialExt)) names.push_back(&name);
            std::sort(names.begin(), names.end(), [](auto a, auto b) { return *a < *b; });
            for (auto name : names) DecideMaterial(mod, *name);
        }

        for (uint32_t i = 0; i < Map.Decisions.size(); i++) {
            if (Map.Decisions[i].Kind != ProvisionKind::Twin && Map.Decisions[i].Kind != ProvisionKind::Translated) continue;
            auto it = pending_.find(i);
            if (it == pending_.end()) continue;
            for (auto& file : it->second) Map.Files.push_back(std::move(file));
        }
    }
};

}

uint64_t PassStemHash(std::string_view stem)
{
    return identity_stem_hash(stem.data(), stem.size());
}

ContainerIdentity VanillaContainerIdentity()
{
    return [](std::span<uint8_t const> container) {
        std::vector<uint64_t> stems;
        IdentityEntry const* first = nullptr;
        uint32_t count = 0;
        if (identity_lookup(container.data(), container.size(), &first, &count)) {
            for (uint32_t i = 0; i < count; i++) stems.push_back(first[i].stem);
        }
        return stems;
    };
}

uint32_t ProvisionMap::Count(ProvisionKind kind) const
{
    return (uint32_t)std::count_if(Decisions.begin(), Decisions.end(), [&](auto const& d) { return d.Kind == kind; });
}

namespace {

// The game prefixes a root: a package path matches as a suffix at a '/' boundary.
bool IsPathOf(std::string_view enginePath, std::string_view packagePath)
{
    if (enginePath.size() < packagePath.size() || !enginePath.ends_with(packagePath)) return false;
    return enginePath.size() == packagePath.size() || enginePath[enginePath.size() - packagePath.size() - 1] == '/';
}

}

ServedFile const* ProvisionMap::Find(std::string_view path) const
{
    auto it = byBaseName_.find(BaseName(path));
    if (it == byBaseName_.end()) return nullptr;
    for (uint32_t index : it->second) {
        if (IsPathOf(path, Files[index].Path)) return &Files[index];
    }
    return nullptr;
}

ProvisionDecision const* ProvisionMap::FindRefused(std::string_view path) const
{
    auto it = refusedByBaseName_.find(BaseName(path));
    if (it == refusedByBaseName_.end()) return nullptr;
    for (uint32_t index : it->second) {
        if (IsPathOf(path, Decisions[index].Material)) return &Decisions[index];
    }
    return nullptr;
}

std::unique_ptr<ProvisionMap> BuildProvisionMap(MaterialSource const& vanilla, std::span<MaterialSource const> mods,
    ContainerIdentity const& identity, ProvisionTranslator const& translator, bool translateFirst)
{
    auto map = std::make_unique<ProvisionMap>();
    Builder(vanilla, mods, identity, translator, translateFirst, *map).Run();

    // Files no longer changes, so views of its paths stay valid.
    for (uint32_t i = 0; i < map->Files.size(); i++) {
        map->byBaseName_[BaseName(map->Files[i].Path)].push_back(i);
    }
    for (uint32_t i = 0; i < map->Decisions.size(); i++) {
        if (map->Decisions[i].Kind == ProvisionKind::Refused) {
            map->refusedByBaseName_[BaseName(map->Decisions[i].Material)].push_back(i);
        }
    }
    return map;
}

}
