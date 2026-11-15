#include <stdafx.h>
#include <Shaders/PakFile.h>
#include <Shaders/GameInterface.h>
#include <Shaders/ShaderProvision.h>
#include <Shaders/TranslatedFunctions.h>
#include <Shaders/Translation.h>
#include <Platform/Config.h>

#include <algorithm>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fstream>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach_time.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

namespace bg3se::mac {

namespace {

constexpr std::string_view kMaterialsDir = "Assets/Materials/";
constexpr std::string_view kModsPrefix = "Mods/";
constexpr std::string_view kMaterialExt = ".lsf";

struct OpenPackage
{
    std::unique_ptr<PakFile> Pak;
    MaterialSource Source;
    std::vector<std::string> ModFolders;    // every <Folder> the package has files under Mods/<Folder>/
    int Rank{ -1 };                         // position of its module in the load order
};

std::unique_ptr<OpenPackage> OpenPackageAt(std::string const& path)
{
    auto package = std::make_unique<OpenPackage>();
    package->Pak = PakFile::Open(path);
    if (!package->Pak) return nullptr;

    PakFile const* pak = package->Pak.get();
    package->Source.Label = pak->FileName();
    for (auto const& entry : pak->Entries()) {
        std::string_view name = entry.Name;
        if (name.find(kMaterialsDir) != std::string_view::npos) {
            package->Source.Names.insert(entry.Name);
        } else if (name.starts_with(kModsPrefix)) {
            auto end = name.find('/', kModsPrefix.size());
            if (end == std::string_view::npos) continue;
            std::string folder(name.substr(kModsPrefix.size(), end - kModsPrefix.size()));
            if (std::find(package->ModFolders.begin(), package->ModFolders.end(), folder) == package->ModFolders.end()) {
                package->ModFolders.push_back(std::move(folder));
            }
        }
    }
    package->Source.Read = [pak](std::string const& name) -> std::optional<std::vector<uint8_t>> {
        auto entry = pak->Find(name);
        return entry ? pak->Read(*entry) : std::nullopt;
    };
    return package;
}

std::string XmlUnescape(std::string_view s)
{
    static constexpr std::pair<std::string_view, char> entities[] = {
        { "&amp;", '&' }, { "&apos;", '\'' }, { "&quot;", '"' }, { "&lt;", '<' }, { "&gt;", '>' },
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        bool replaced = false;
        if (s[i] == '&') {
            for (auto const& [entity, ch] : entities) {
                if (s.substr(i).starts_with(entity)) {
                    out.push_back(ch);
                    i += entity.size();
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) out.push_back(s[i++]);
    }
    return out;
}

double MillisecondsSince(uint64_t start)
{
    static mach_timebase_info_data_t timebase;
    if (!timebase.denom) mach_timebase_info(&timebase);
    return (double)(mach_absolute_time() - start) * timebase.numer / timebase.denom / 1e6;
}

ProvisionPlan Plan(std::string const& materialsPak, std::vector<std::unique_ptr<OpenPackage>>& mods, ContainerIdentity const& identity,
    ProvisionTranslator const& translator = {}, bool translateFirst = false)
{
    ProvisionPlan plan;
    auto vanilla = OpenPackageAt(materialsPak);
    if (!vanilla) {
        plan.Problem = "cannot read the game's Materials.pak at " + materialsPak;
        return plan;
    }

    // The sources are built here, after the packages have their final order
    // and addresses: nothing below this point moves a package.
    std::vector<MaterialSource> sources;
    sources.reserve(mods.size());
    for (auto const& mod : mods) {
        plan.Packages.push_back(mod->Source.Label);
        sources.push_back(std::move(mod->Source));
    }
    plan.Map = BuildProvisionMap(vanilla->Source, sources, identity, translator, translateFirst);
    return plan;
}

}

InstallPaths InstallPaths::ForGame(std::string const& appBundle)
{
    InstallPaths paths;
    paths.MaterialsPak = appBundle + "/Contents/Data/Materials.pak";
    char const* home = getenv("HOME");
    std::string larian = std::string(home ? home : "") + "/Documents/Larian Studios/Baldur's Gate 3";
    paths.ModsDir = larian + "/Mods";
    paths.ModSettings = larian + "/PlayerProfiles/Public/modsettings.lsx";
    return paths;
}

InstallPaths InstallPaths::ForThisProcess()
{
    char exe[4096];
    uint32_t size = sizeof(exe);
    std::string bundle;
    if (_NSGetExecutablePath(exe, &size) == 0) {
        char resolved[4096];
        std::string path = realpath(exe, resolved) ? resolved : exe;
        // <bundle>/Contents/MacOS/<executable>
        for (int up = 0; up < 3; up++) {
            auto slash = path.rfind('/');
            if (slash == std::string::npos) break;
            path.resize(slash);
        }
        bundle = path;
    }
    return ForGame(bundle);
}

std::vector<std::string> ParseLoadOrder(std::string_view xml)
{
    // <attribute id="Folder" type="LSString" value="..."/>, one per module, in order.
    static constexpr std::string_view kAttribute = "id=\"Folder\"";
    static constexpr std::string_view kValue = "value=\"";
    std::vector<std::string> folders;
    size_t at = 0;
    while ((at = xml.find(kAttribute, at)) != std::string_view::npos) {
        at += kAttribute.size();
        auto tagEnd = xml.find('>', at);
        auto value = xml.find(kValue, at);
        if (value == std::string_view::npos || tagEnd == std::string_view::npos || value > tagEnd) continue;
        value += kValue.size();
        auto quote = xml.find('"', value);
        if (quote == std::string_view::npos) break;
        folders.push_back(XmlUnescape(xml.substr(value, quote - value)));
        at = quote;
    }
    return folders;
}

ProvisionPlan PlanFromInstall(InstallPaths const& paths, ContainerIdentity const& identity, ProvisionTranslator const& translator,
    bool translateFirst)
{
    uint64_t start = mach_absolute_time();

    std::vector<std::string> order;
    {
        std::ifstream f(paths.ModSettings, std::ios::binary);
        std::string xml((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        order = ParseLoadOrder(xml);
    }

    std::vector<std::string> unreadable, notInOrder;
    std::vector<std::unique_ptr<OpenPackage>> mods;
    if (DIR* dir = opendir(paths.ModsDir.c_str())) {
        while (dirent* e = readdir(dir)) {
            std::string name = e->d_name;
            if (name.size() < 5 || strcasecmp(name.c_str() + name.size() - 4, ".pak") != 0) continue;
            auto package = OpenPackageAt(paths.ModsDir + "/" + name);
            if (!package) {
                unreadable.push_back(name);
                continue;
            }
            for (auto const& folder : package->ModFolders) {
                auto it = std::find(order.begin(), order.end(), folder);
                if (it == order.end()) continue;
                int rank = (int)(it - order.begin());
                if (package->Rank < 0 || rank < package->Rank) package->Rank = rank;
            }
            // Mounted by the game whether its module is in the load order or not.
            if (package->Rank < 0) notInOrder.push_back(name);
            mods.push_back(std::move(package));
        }
        closedir(dir);
    }
    // The game reads a file from the package of the highest priority that has
    // it: later here is read before earlier. Within one priority the load
    // order, and a package outside it after those in it.
    std::sort(mods.begin(), mods.end(), [](auto const& a, auto const& b) {
        if (a->Pak->Priority() != b->Pak->Priority()) return a->Pak->Priority() < b->Pak->Priority();
        if ((a->Rank < 0) != (b->Rank < 0)) return b->Rank < 0;
        return a->Rank != b->Rank ? a->Rank < b->Rank : a->Source.Label < b->Source.Label;
    });

    // Where the order within one priority decides a material, say so: the
    // game's own order of two packages of one priority is not known (it sorts
    // what it collected from the folder by priority alone), whether or not
    // they are in the load order. `mods` is in the order used: the later of
    // two is the one taken.
    std::vector<std::string> ties;
    for (size_t later = 0; later < mods.size(); later++) {
        for (size_t earlier = 0; earlier < later; earlier++) {
            auto const& a = *mods[earlier];
            auto const& b = *mods[later];
            if (a.Pak->Priority() != b.Pak->Priority()) continue;
            size_t shared = 0;
            std::string example;
            for (auto const& name : b.Source.Names) {
                if (!name.ends_with(".lsf") || !a.Source.Names.contains(name)) continue;
                if (!shared++) example = name;
            }
            if (!shared) continue;
            auto label = [](OpenPackage const& p) { return p.Source.Label + (p.Rank < 0 ? " (outside the load order)" : ""); };
            ties.push_back(label(b) + " and " + label(a) + " both ship " + std::to_string(shared) + " materials (" + example
                + (shared > 1 ? ", ..." : "") + ") and have the same priority: the game mounts both, and which of the two it reads is not known; "
                "taken to be " + b.Source.Label + "'s");
        }
    }

    auto plan = Plan(paths.MaterialsPak, mods, identity, translator, translateFirst);
    plan.NotInLoadOrder = std::move(notInOrder);
    plan.Ties = std::move(ties);
    plan.Unreadable = std::move(unreadable);
    plan.Milliseconds = MillisecondsSince(start);
    return plan;
}

ProvisionPlan PlanFromPackages(std::string const& materialsPak, std::vector<std::string> const& modPackages,
    ProvisionTranslator const& translator, bool translateFirst)
{
    uint64_t start = mach_absolute_time();
    std::vector<std::string> unreadable;
    std::vector<std::unique_ptr<OpenPackage>> mods;
    for (auto const& path : modPackages) {
        if (auto package = OpenPackageAt(path)) mods.push_back(std::move(package));
        else unreadable.push_back(path);
    }
    auto plan = Plan(materialsPak, mods, VanillaContainerIdentity(), translator, translateFirst);
    plan.Unreadable = std::move(unreadable);
    plan.Milliseconds = MillisecondsSince(start);
    return plan;
}

void LogPlan(ProvisionPlan const& plan)
{
    if (!plan.Map) {
        ERR("[Shaders] %s; no shader is provisioned", plan.Problem.c_str());
        return;
    }
    for (auto const& name : plan.Unreadable) WARN("[Shaders] cannot read %s", name.c_str());

    auto const& map = *plan.Map;
    for (auto const& d : map.Decisions) {
        switch (d.Kind) {
        case ProvisionKind::Twin:
            INFO("[Shaders] %s: twin of %s (%u stubs, %u/%u containers vanilla) [%s]", d.Material.c_str(), d.Vanilla.c_str(),
                d.Stubs, d.Matched, d.Passes, d.Package.c_str());
            break;
        case ProvisionKind::Translated:
            INFO("[Shaders] %s: translated, %s (%u stubs) [%s]", d.Material.c_str(), d.Reason.c_str(), d.Stubs, d.Package.c_str());
            break;
        case ProvisionKind::Skipped:
            INFO("[Shaders] %s: skipped, %s [%s]", d.Material.c_str(), d.Reason.c_str(), d.Package.c_str());
            break;
        case ProvisionKind::Superseded:
            INFO("[Shaders] %s: superseded, %s [%s]", d.Material.c_str(), d.Reason.c_str(), d.Package.c_str());
            break;
        case ProvisionKind::Refused:
            WARN("[Shaders] %s: refused, %s [%s]", d.Material.c_str(), d.Reason.c_str(), d.Package.c_str());
            break;
        }
    }
    for (auto const& tie : plan.Ties) WARN("[Shaders] %s", tie.c_str());
    INFO("[Shaders] %zu mod packages (%zu of them outside the load order), %zu materials: %u twins, %u translated, %u skipped, %u refused, "
        "%u superseded; %zu files to serve; %.0f ms",
        plan.Packages.size(), plan.NotInLoadOrder.size(), map.Decisions.size(), map.Count(ProvisionKind::Twin),
        map.Count(ProvisionKind::Translated), map.Count(ProvisionKind::Skipped), map.Count(ProvisionKind::Refused), map.Count(ProvisionKind::Superseded),
        map.Files.size(), plan.Milliseconds);
}

void ShaderProvisioner::Remember(std::string_view path, uint8_t const* served, size_t size)
{
    std::lock_guard lock(seenMutex_);
    seen_[std::string(path)] = { served, size };
}

void ShaderProvisioner::Adopt(std::unique_ptr<ProvisionMap> map)
{
    auto plan = std::make_unique<Plan>();
    plan->Map = std::move(map);
    size_t files = plan->Map->Files.size(), decisions = plan->Map->Decisions.size();
    plan->Asked.reset(new std::atomic<uint32_t>[files]());
    plan->Served.reset(new std::atomic<uint32_t>[files]());
    plan->MaterialServed.reset(new std::atomic<bool>[decisions]());
    plan->ReadBefore.reset(new std::atomic<bool>[decisions]());
    plan->RefusedAsked.reset(new std::atomic<bool>[decisions]());

    std::lock_guard lock(adoptMutex_);
    // A material the game already holds, parsed from the .lsf the previous plan
    // served, will not be read again: if this plan serves the same bytes for it,
    // its stubs stay servable.
    if (Plan const* previous = plan_.load(std::memory_order_acquire)) {
        for (auto const& file : plan->Map->Files) {
            if (!file.IsMaterial) continue;
            auto before = previous->Map->Find(file.Path);
            if (before && before->IsMaterial && before->Data == file.Data
                && previous->MaterialServed[before->Decision].load(std::memory_order_acquire)) {
                plan->MaterialServed[file.Decision].store(true, std::memory_order_release);
            }
        }
    }

    // A material the game read before this plan could serve it: the game may
    // still hold what it read then.
    uint32_t readBefore = 0;
    std::string example;
    {
        std::lock_guard seen(seenMutex_);
        for (auto const& [path, got] : seen_) {
            auto file = plan->Map->Find(path);
            if (!file || !file->IsMaterial) continue;
            bool same = got.Data && got.Size == file->Data.size() && memcmp(got.Data, file->Data.data(), got.Size) == 0;
            if (same || plan->ReadBefore[file->Decision].exchange(true)) continue;
            if (!readBefore++) example = file->Path;
        }
    }
    if (readBefore) {
        WARN("[Shaders] the game read %u of this plan's materials before the plan was made (%s%s); it keeps what it read "
            "for as long as it uses them, and gets the plan's files when it next loads them",
            readBefore, example.c_str(), readBefore > 1 ? ", ..." : "");
    }

    plan_.store(plan.get(), std::memory_order_release);
    plans_.push_back(std::move(plan));
}

std::span<uint8_t const> ShaderProvisioner::OnFileOpened(std::string_view path)
{
    Plan const* plan = plan_.load(std::memory_order_acquire);
    auto file = plan ? plan->Map->Find(path) : nullptr;
    if (!file) {
        if (path.find(kMaterialsDir) == std::string_view::npos) return {};
        if (path.ends_with(kMaterialExt)) Remember(path, nullptr, 0);
        // The first material paths the game asks for show the shape of its
        // paths (root, separators, case), should nothing ever match.
        if (pathsLogged_.load(std::memory_order_relaxed) < 3 && pathsLogged_.fetch_add(1, std::memory_order_relaxed) < 3) {
            INFO("[Shaders] a material path as the game asks for it: %.*s", (int)path.size(), path.data());
        }
        if (auto refused = plan ? plan->Map->FindRefused(path) : nullptr) {
            size_t index = (size_t)(refused - plan->Map->Decisions.data());
            if (!plan->RefusedAsked[index].exchange(true)) {
                WARN("[Shaders] the game asked for %s, which is refused (%s): it gets the mod's own file", refused->Material.c_str(),
                    refused->Reason.c_str());
            }
        }
        return {};
    }

    size_t index = (size_t)(file - plan->Map->Files.data());
    plan->Asked[index].fetch_add(1, std::memory_order_relaxed);

    // A stub is the vanilla Mac shader: it is only right for the rewritten .lsf.
    // If the game got the mod's own .lsf, it must not get the stub either.
    if (file->IsMaterial) {
        plan->ReadBefore[file->Decision].store(false, std::memory_order_release);
        if (!plan->MaterialServed[file->Decision].exchange(true)) {
            auto const& d = plan->Map->Decisions[file->Decision];
            if (d.Kind == ProvisionKind::Translated) INFO("[Shaders] the game asked for %s: served, with its shaders translated", d.Material.c_str());
            else INFO("[Shaders] the game asked for %s: served, as a twin of %s", d.Material.c_str(), d.Vanilla.c_str());
        }
        Remember(path, file->Data.data(), file->Data.size());
    } else if (!plan->MaterialServed[file->Decision].load(std::memory_order_acquire)) {
        plan->Withheld.fetch_add(1, std::memory_order_relaxed);
        if (held_.fetch_add(1, std::memory_order_relaxed) < 8) {
            WARN("[Shaders] %s was asked for before its material's .lsf was served; left as the game found it", file->Path.c_str());
        }
        return {};
    }

    if (plan->Served[index].fetch_add(1, std::memory_order_relaxed) == 0) {
        DEBUG("[Shaders] serving %s (%zu bytes) for %.*s", file->Path.c_str(), file->Data.size(), (int)path.size(), path.data());
    }
    return file->Data;
}

ProvisionProgress ShaderProvisioner::Progress() const
{
    ProvisionProgress p;
    Plan const* plan = plan_.load(std::memory_order_acquire);
    if (!plan) return p;
    auto const& map = *plan->Map;
    p.Files = (uint32_t)map.Files.size();
    for (size_t i = 0; i < map.Files.size(); i++) {
        uint32_t asked = plan->Asked[i].load(std::memory_order_relaxed);
        p.Requests += asked;
        if (asked) p.FilesAsked++;
        if (plan->Served[i].load(std::memory_order_relaxed)) p.FilesServed++;
    }
    p.Withheld = plan->Withheld.load(std::memory_order_relaxed);
    for (size_t i = 0; i < map.Decisions.size(); i++) {
        switch (map.Decisions[i].Kind) {
        case ProvisionKind::Twin:
        case ProvisionKind::Translated:
            p.Materials++;
            if (plan->MaterialServed[i].load(std::memory_order_relaxed)) p.MaterialsServed++;
            if (plan->ReadBefore[i].load(std::memory_order_relaxed)) p.ReadBefore++;
            break;
        case ProvisionKind::Refused:
            p.Refused++;
            if (plan->RefusedAsked[i].load(std::memory_order_relaxed)) p.RefusedAsked++;
            break;
        default:
            break;
        }
    }
    return p;
}

void ShaderProvisioner::LogProgress() const
{
    if (!plan_.load(std::memory_order_acquire)) return;
    auto p = Progress();
    INFO("[Shaders] the game has opened %llu files; of %u provisioned files it asked for %u (%llu requests): %u served, "
        "%llu requests withheld; %u of %u materials served, %u read before the plan and not since; %u of %u refused materials asked for",
        (unsigned long long)FileReaderOpenCount(), p.Files, p.FilesAsked, (unsigned long long)p.Requests, p.FilesServed,
        (unsigned long long)p.Withheld, p.MaterialsServed, p.Materials, p.ReadBefore, p.RefusedAsked, p.Refused);
    auto functions = CountTranslatedFunctions();
    if (functions.Kept || functions.Waiting || functions.Asked) {
        INFO("[Shaders] translated functions: %u compiled, %u not asked for yet; the game asked for one %llu times and got it %llu times",
            functions.Kept, functions.Waiting, (unsigned long long)functions.Asked, (unsigned long long)functions.Answered);
    }
}

bool ShaderProvisioner::LogProgressIfChanged()
{
    if (!plan_.load(std::memory_order_acquire)) return false;
    auto now = Progress();
    {
        std::lock_guard lock(progressMutex_);
        if (everLogged_ && now == logged_) return false;
        logged_ = now;
        everLogged_ = true;
    }
    LogProgress();
    return true;
}

InstallStamp InstallStamp::Read(InstallPaths const& paths)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    auto mix = [&](void const* p, size_t n) {
        for (size_t i = 0; i < n; i++) hash = (hash ^ static_cast<uint8_t const*>(p)[i]) * 0x100000001b3ull;
    };
    auto mixFile = [&](std::string const& path, std::string_view name) {
        struct stat st{};
        if (stat(path.c_str(), &st) != 0) return;
        mix(name.data(), name.size());
        mix(&st.st_size, sizeof(st.st_size));
        mix(&st.st_mtimespec, sizeof(st.st_mtimespec));
    };

    mixFile(paths.ModSettings, "modsettings");
    std::vector<std::string> packages;
    if (DIR* dir = opendir(paths.ModsDir.c_str())) {
        while (dirent* e = readdir(dir)) {
            std::string name = e->d_name;
            if (name.size() >= 5 && strcasecmp(name.c_str() + name.size() - 4, ".pak") == 0) packages.push_back(std::move(name));
        }
        closedir(dir);
    }
    std::sort(packages.begin(), packages.end());    // directory order is not stable
    for (auto const& name : packages) mixFile(paths.ModsDir + "/" + name, name);
    return { hash };
}

InstallWatcher::InstallWatcher(InstallPaths paths, ShaderProvisioner& provisioner, ContainerIdentity identity, ProvisionTranslator translator,
    bool translateFirst, std::function<void()> planned)
    : paths_(std::move(paths)), provisioner_(provisioner), identity_(std::move(identity)), translator_(std::move(translator)),
      translateFirst_(translateFirst), afterPlan_(std::move(planned))
{}

void InstallWatcher::PlanNow()
{
    planned_ = seen_ = InstallStamp::Read(paths_);
    auto plan = PlanFromInstall(paths_, identity_, translator_, translateFirst_);
    LogPlan(plan);
    // With no plan (the game's Materials.pak unreadable) what was served stays served.
    if (plan.Map) provisioner_.Adopt(std::move(plan.Map));
    if (afterPlan_) afterPlan_();
}

bool InstallWatcher::Poll()
{
    auto now = InstallStamp::Read(paths_);
    bool settled = now == seen_;
    seen_ = now;
    if (!settled || now == planned_) return false;

    INFO("[Shaders] the load order or the Mods folder changed; planning again");
    PlanNow();
    return true;
}

namespace {

// The Mac game's interface, once it has been read, and the interface table as
// it stands for this game: translation waits for them.
struct TranslationState
{
    std::mutex Mutex;
    std::shared_ptr<MacInterface const> Interface;
    std::shared_ptr<InterfaceTable const> Table;
    // What was translated for this game and this build before, in bg3mse's own folder.
    std::shared_ptr<TranslationCache> Cache;
    TranslationCache::Counts Said{};
    // Why the game's functions could not be read, when they could not.
    std::string NotRead;
    // The game's bundle, and how a plan is made again once its functions have been read: both set before the
    // first plan, and not changed after.
    std::string App;
    std::function<void()> PlanAgain;

    std::tuple<std::shared_ptr<MacInterface const>, std::shared_ptr<InterfaceTable const>, std::shared_ptr<TranslationCache>> Get()
    {
        std::lock_guard lock(Mutex);
        return { Interface, Table, Cache };
    }
    std::string WhyNotRead()
    {
        std::lock_guard lock(Mutex);
        return NotRead;
    }
    // After a plan: how much of it was as kept, and what of the cache was not usable.
    void SayWhatWasKept()
    {
        std::shared_ptr<TranslationCache> cache;
        TranslationCache::Counts before;
        {
            std::lock_guard lock(Mutex);
            cache = Cache;
            before = Said;
            if (cache) Said = cache->Count();
        }
        if (!cache) return;
        auto now = cache->Count();
        uint32_t found = now.Found - before.Found, kept = now.Kept - before.Kept, stale = now.Stale - before.Stale,
            damaged = now.Damaged - before.Damaged;
        if (!found && !kept && !stale && !damaged) return;
        auto functions = CountTranslatedFunctions();
        INFO("[Shaders] translated materials kept in %s: %u were as kept from an earlier run, %u were translated now and kept "
            "(%u kept ones were made from something else, %u could not be read back: translated afresh); %u functions are compiled, "
            "%u more are compiled when the game asks for them, or before",
            cache->Directory().c_str(), found, kept, stale, damaged, functions.Kept, functions.Waiting);
    }
    void Set(MacInterface mac)
    {
        // The members of the lighting constants that are worked out from the game's own stand only when this
        // game fills them the way that was read, and its functions read them where it writes them.
        BridgeProblems problems;
        problems.SunShadow = problems.TextureTable = "the game's symbol table cannot be read";
        auto image = GameImage::Current();
        auto table = std::make_shared<InterfaceTable const>(image ? InterfaceFor(*image, mac, problems)
            : BuiltInInterface().WithoutDerived().WithoutTextureTable());
        if (problems.SunShadow.empty()) {
            INFO("[Shaders] ls::SetupForwardLightingConstants fills the sun's shadow constants as they were read, where the game's functions read them: a shader that reads the Windows members is given them worked out");
        } else {
            WARN("[Shaders] the sun's shadow constants are not worked out for this game (%s): a material whose shader reads them is not translated",
                problems.SunShadow.c_str());
        }
        if (problems.TextureTable.empty()) {
            INFO("[Shaders] ls::ClusteredAssignmentStage packs a light probe's cubes and a light's cookie as they were read, and the game's functions take them as read: a shader that takes the Windows table of textures is given the game's arrays");
        } else {
            WARN("[Shaders] the table of textures is not bridged for this game (%s): a material whose shader lights by clusters is not translated",
                problems.TextureTable.c_str());
        }
        std::string data = DataDirectory();
        auto cache = data.empty() ? nullptr : OpenTranslationCache(data + "/" + kTranslationCacheDirectory, mac, *table);
        if (!cache) WARN("[Shaders] translated materials are not kept between runs: %s", data.empty() ? "there is no home directory" : "this build has no identifier of its own");
        std::lock_guard lock(Mutex);
        Interface = std::make_shared<MacInterface const>(std::move(mac));
        Table = std::move(table);
        Cache = std::move(cache);
        Said = {};
    }
};

// The game's own Metal functions, for a game translation has just been set up in: as kept for this version of
// the game, or read now.
void ReadGameInterface(std::shared_ptr<TranslationState> state)
{
    std::string data = DataDirectory();
    std::string app = state->App, stamp = MacInterfaceStamp(app), file = data + "/" + kMacInterfaceFile;
    if (auto kept = LoadMacInterface(file, stamp)) {
        INFO("[Shaders] the game's Metal interface is as kept in %s (%zu function pairs)", file.c_str(), kept->Passes());
        state->Set(std::move(*kept));
        return;
    }
    // Not there, or of another game version or an earlier build's: nothing outside the game has read
    // it for this game (bg3mse-install shader-interface does, Mac/Installer/InstallTool.cpp). Read here,
    // which takes minutes beside a running game: on threads of its own, and planned again when it is
    // there. A material the game asks for before that gets what the plan without translation has for
    // it, and keeps it for as long as the game holds it; the plan that follows says which those were.
    INFO("[Shaders] reading the game's own Metal functions, once for this game build; materials that need translation wait for it");
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        uint64_t start = mach_absolute_time();
        std::optional<MacInterface> mac(std::in_place);
        std::string why;
        if (!SurveyMacInterface(app, *mac, why)) {
            WARN("[Shaders] the game's own Metal functions cannot be read (%s); nothing is translated", why.c_str());
            std::lock_guard lock(state->Mutex);
            state->NotRead = why.empty() ? "no reason was given" : why;
            return;
        }
        if (!SaveMacInterface(file, stamp, *mac)) WARN("[Shaders] cannot keep the game's Metal interface in %s", file.c_str());
        INFO("[Shaders] the game's own Metal functions were read in %.0f seconds", MillisecondsSince(start) / 1000);
        INFO("[Shaders] the game's Metal interface is read (%zu function pairs); planning again", mac->Passes());
        state->Set(std::move(*mac));
        if (state->PlanAgain) state->PlanAgain();
    });
}

// Sets translation up in the running game: what it stands on is checked, the game's Metal libraries are made
// to find translated functions, and the game's own functions are loaded or read. Called when a plan first has
// a material to translate (TranslatorSetUpWhenNeeded), on the queue plans are made on, so a game with no such
// material has nothing of it. No translator, with the reason, when translation does not stand in this game.
ProvisionTranslator SetUpTranslation(std::shared_ptr<TranslationState> state, std::string& why)
{
    auto image = GameImage::Current();
    if (!image) {
        why = "the game's symbol table cannot be read";
        WARN("[Shaders] %s is on, but %s; nothing is translated", kShaderTranslationSetting, why.c_str());
        return {};
    }
    auto check = CheckShaderCreation(*image);
    if (!check.Ok() || !InstallTranslatedFunctions(why)) {
        if (!check.Ok()) why = check.Problem;
        WARN("[Shaders] %s is on, but %s; nothing is translated", kShaderTranslationSetting, why.c_str());
        return {};
    }
    INFO("[Shaders] %s is on: the game asks its Metal libraries for a stub's functions in %u places, and now finds translated ones there",
        kShaderTranslationSetting, check.Requests);
    ReadGameInterface(state);
    return [state](MaterialSource const& mod, std::string const& material, std::string& why) -> std::optional<TranslatedFiles> {
        auto [mac, table, cache] = state->Get();
        if (!mac || !table) {
            auto notRead = state->WhyNotRead();
            why = notRead.empty() ? "the game's own Metal functions are still being read; it is translated when that is done"
                                  : "the game's own Metal functions cannot be read (" + notRead + ")";
            return {};
        }
        return MaterialTranslator(mac, table, cache)(mod, material, why);
    };
}

// The translator of the running game. Nothing of translation is there until a plan has a material for it.
ProvisionTranslator TranslatorWith(std::shared_ptr<TranslationState> state)
{
    std::string only = ConfValue(kShaderTranslationOnlySetting).value_or("");
    if (!only.empty()) INFO("[Shaders] %s: only materials with \"%s\" in their path are translated", kShaderTranslationOnlySetting, only.c_str());
    auto translator = TranslatorSetUpWhenNeeded([state](std::string& why) { return SetUpTranslation(state, why); });
    return [translator, only](MaterialSource const& mod, std::string const& material, std::string& why) -> std::optional<TranslatedFiles> {
        if (!only.empty() && material.find(only) == std::string::npos) {
            why = std::string(kShaderTranslationOnlySetting) + " leaves it out";
            return {};
        }
        return translator(mod, material, why);
    };
}

}

namespace {

// The identifier the linker gave the image this function is in (LC_UUID): made of the image's content, so
// another build of this code has another. Empty when the image has none.
std::string ThisImageIdentifier()
{
    Dl_info info{};
    if (!dladdr((void const*)&ThisImageIdentifier, &info) || !info.dli_fbase) return {};
    auto const* header = static_cast<mach_header_64 const*>(info.dli_fbase);
    if (header->magic != MH_MAGIC_64) return {};
    auto const* command = reinterpret_cast<load_command const*>(header + 1);
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (command->cmd == LC_UUID) return HexOf(reinterpret_cast<uuid_command const*>(command)->uuid);
        command = reinterpret_cast<load_command const*>(reinterpret_cast<uint8_t const*>(command) + command->cmdsize);
    }
    return {};
}

std::string SystemBuild()
{
    char build[64] = "";
    size_t size = sizeof(build) - 1;
    if (sysctlbyname("kern.osversion", build, &size, nullptr, 0) != 0) return {};
    return build;
}

}

std::string TranslationMadeWith(MacInterface const& mac, InterfaceTable const& table)
{
    std::string image = ThisImageIdentifier(), system = SystemBuild();
    if (image.empty() || system.empty()) return {};
    std::string interface = mac.Serialize();
    // The table is this build's (BuiltInInterface), less what does not stand for the game that runs: said by
    // what is in it, piece by piece.
    std::string stands = table.Build + ", " + std::to_string(table.Slots.size()) + " slots, " + std::to_string(table.Attributes.size())
        + " attributes, " + std::to_string(table.Faults.size()) + " faults (" + std::to_string(table.FaultyResources.size()) + ", "
        + std::to_string(table.FaultyVertexStages.size()) + ", " + std::to_string(table.FaultyPairs.size()) + "), "
        + std::to_string(table.GameVertexStages.size()) + " vertex stages, " + (table.Textures ? "with" : "without") + " the table of textures";
    for (auto const& block : table.Blocks) {
        stands += ", " + block.Name + " " + std::to_string(block.Members.size()) + "/" + std::to_string(block.Derived.size());
    }
    return "bg3mse image " + image + "; system " + system + "; the game's interface "
        + HexOf(HashOf({ (uint8_t const*)interface.data(), interface.size() })) + "; the interface table of " + stands;
}

std::shared_ptr<TranslationCache> OpenTranslationCache(std::string directory, MacInterface const& mac, InterfaceTable const& table)
{
    std::string madeWith = TranslationMadeWith(mac, table);
    if (madeWith.empty()) return nullptr;
    return std::make_shared<TranslationCache>(std::move(directory), std::move(madeWith));
}

ProvisionTranslator TranslatorOf(MaterialTranslation translation, std::shared_ptr<TranslationCache> cache)
{
    return [translation = std::move(translation), cache](MaterialSource const& mod, std::string const& material, std::string& why)
               -> std::optional<TranslatedFiles> {
        auto passesAndFunctions = [](size_t passes, size_t functions) {
            return std::to_string(passes) + " passes from Vulkan to Metal, " + std::to_string(functions) + " functions ";
        };
        if (auto kept = cache ? cache->Find(mod, material) : std::nullopt) {
            // They compiled when they were kept, on this system: nothing waits for the compiler now. Should
            // one not compile after all it is tried again when the game asks for it, and the material is
            // translated afresh the next time the game starts (TranslatedFunctions.h).
            KeepTranslatedSources(kept->Functions, material, [cache, package = mod.Label, material] { cache->Forget(package, material); });
            TranslatedFiles files;
            files.Note = passesAndFunctions(kept->Stubs.size(), kept->Functions.size()) + "as kept from an earlier run" + kept->Note;
            files.Lsf = std::move(kept->Lsf);
            files.Stubs = std::move(kept->Stubs);
            return files;
        }

        // Every file translation reads is noted with what was read: what the kept translation stands on.
        std::optional<RecordedSource> recorded;
        if (cache) recorded.emplace(mod);
        auto translated = translation(recorded ? recorded->Source() : mod, material, why);
        if (!translated) return {};

        KeptTranslation made;
        for (auto& pass : translated->Passes) {
            made.Functions.push_back({ std::move(pass.VertexName), std::move(pass.VertexSource) });
            if (!pass.FragmentSource.empty()) made.Functions.push_back({ std::move(pass.FragmentName), std::move(pass.FragmentSource) });
            made.Stubs.emplace_back(pass.Pass, std::move(pass.Stub));
        }
        if (!AddTranslatedFunctions(made.Functions, why, material)) return {};
        made.Lsf = std::move(translated->Lsf);
        // Said with the material wherever it is said to be translated.
        if (translated->OwnVertexStages) {
            made.Note += "; its vertex stage is the mod's own (" + std::to_string(translated->OwnVertexStages) + " of " + std::to_string(made.Stubs.size()) + " passes)";
        }
        // A pairing made by order alone is used, and said: a refused material is no safer, since the game then
        // gets the mod's own file, which it cannot draw.
        for (size_t i = 0; i < translated->ByOrderOnly.size(); i++) {
            made.Note += (i ? ", " : "; taken by a pairing made by order only: ") + translated->ByOrderOnly[i];
        }
        if (recorded) {
            if (auto inputs = recorded->Inputs()) cache->Keep(mod, material, *inputs, made);
        }

        TranslatedFiles files;
        files.Note = passesAndFunctions(made.Stubs.size(), made.Functions.size()) + "compiled" + made.Note;
        files.Lsf = std::move(made.Lsf);
        files.Stubs = std::move(made.Stubs);
        return files;
    };
}

ProvisionTranslator MaterialTranslator(std::shared_ptr<MacInterface const> mac, std::shared_ptr<InterfaceTable const> table,
    std::shared_ptr<TranslationCache> cache)
{
    return TranslatorOf([mac, table](MaterialSource const& mod, std::string const& material, std::string& why) {
        return TranslateMaterial(mod, material, *table, *mac, why);
    }, std::move(cache));
}

TranslationSetting TranslationSettingOf(std::optional<std::string> const& value)
{
    if (!value) return TranslationSetting::On;
    if (value->size() == 3 && strncasecmp(value->c_str(), "all", 3) == 0) return TranslationSetting::All;
    return FlagValue(*value) ? TranslationSetting::On : TranslationSetting::Off;
}

bool TranslationHasWork(InstallPaths const& paths, TranslationSetting setting, std::string& why, ContainerIdentity const& identity)
{
    if (setting == TranslationSetting::Off) {
        why = std::string(kShaderTranslationSetting) + " is off in bg3mse.conf";
        return false;
    }
    // The plan the game makes, with a translator that only notes it was asked.
    bool asked = false;
    ProvisionTranslator ask = [&](MaterialSource const&, std::string const&, std::string& reason) -> std::optional<TranslatedFiles> {
        asked = true;
        reason = "only asked whether there is one to translate";
        return {};
    };
    auto plan = PlanFromInstall(paths, identity, ask, setting == TranslationSetting::All);
    if (!plan.Map) {
        why = plan.Problem.empty() ? "the install cannot be planned" : plan.Problem;
        return false;
    }
    if (!asked) why = "no mod in the Mods folder has a material with shaders to translate";
    return asked;
}

ProvisionTranslator TranslatorSetUpWhenNeeded(std::function<ProvisionTranslator(std::string& why)> setUp)
{
    struct Late
    {
        std::mutex Mutex;
        std::function<ProvisionTranslator(std::string& why)> SetUp;
        bool Done{ false };
        ProvisionTranslator Translator;
        std::string Why;
    };
    auto late = std::make_shared<Late>();
    late->SetUp = std::move(setUp);
    return [late](MaterialSource const& mod, std::string const& material, std::string& why) -> std::optional<TranslatedFiles> {
        {
            std::lock_guard lock(late->Mutex);
            if (!late->Done) {
                late->Translator = late->SetUp(late->Why);
                late->SetUp = {};
                late->Done = true;
            }
        }
        // Neither changes once the set-up is done.
        if (!late->Translator) {
            why = late->Why;
            return {};
        }
        return late->Translator(mod, material, why);
    };
}

void StartShaderProvisioning()
{
    // Both live for the rest of the process: the game reads served bytes in place.
    static auto provisioner = new ShaderProvisioner();
    static auto paths = InstallPaths::ForThisProcess();
    // A mod's own shaders are translated unless bg3mse.conf switches that off. What translation needs of the
    // game is set up when a plan first has a material to translate, at this start or after the player has
    // enabled such a mod: a game without one gets no hook, no check and no read from it.
    static auto setting = TranslationSettingOf(ConfValue(kShaderTranslationSetting));
    static auto translation = setting == TranslationSetting::Off ? nullptr : std::make_shared<TranslationState>();
    if (!translation) INFO("[Shaders] %s is off in bg3mse.conf: a mod's own shaders are not translated", kShaderTranslationSetting);
    // The game's bundle is three levels above its Materials.pak.
    if (translation) translation->App = paths.MaterialsPak.substr(0, paths.MaterialsPak.size() - strlen("/Contents/Data/Materials.pak"));
    static auto watcher = new InstallWatcher(paths, *provisioner, VanillaContainerIdentity(),
        translation ? TranslatorWith(translation) : ProvisionTranslator{}, setting == TranslationSetting::All,
        translation ? std::function<void()>([] { translation->SayWhatWasKept(); }) : std::function<void()>{});
    static dispatch_queue_t queue = dispatch_queue_create("bg3mse.shaders", DISPATCH_QUEUE_SERIAL);
    if (translation) {
        translation->PlanAgain = [] {
            dispatch_async(queue, ^{
                try {
                    watcher->PlanNow();
                } catch (...) {
                }
            });
        };
    }
    // On the queue everything else of the watcher runs on.
    dispatch_sync(queue, ^{ watcher->PlanNow(); });

    // Hooked at start-up even when there is nothing to serve yet: the player
    // can enable a mod while the game runs, and every file the game opens
    // from now on goes through the hook.
    if (!InstallFileReaderHook(provisioner)) return;

    static dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_SEC / 4);
    dispatch_source_set_event_handler(timer, ^{
        try {
            watcher->Poll();
        } catch (...) {
            // Keep what is served; try again on the next tick.
        }
        // How often the game came for a translated function, said whenever that has changed and at most every
        // ten seconds: a short run shows it took them, without waiting for the report below.
        static TranslatedFunctionCounts said;
        static uint32_t ticksSinceSaid = 10;
        auto functions = CountTranslatedFunctions();
        if (++ticksSinceSaid >= 10 && (functions.Asked != said.Asked || functions.Answered != said.Answered)) {
            INFO("[Shaders] translated functions: %u compiled, %u not asked for yet; the game asked for one %llu times and got it %llu times",
                functions.Kept, functions.Waiting, (unsigned long long)functions.Asked, (unsigned long long)functions.Answered);
            said = functions;
            ticksSinceSaid = 0;
        }
    });
    dispatch_resume(timer);

    // What the game asked for is only known as it plays: reported a minute
    // in, and from then on every five minutes in which something changed.
    // (Not at exit: the game leaves through _Exit, which runs no handlers.)
    static dispatch_source_t report = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_timer(report, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC), 300 * NSEC_PER_SEC, NSEC_PER_SEC);
    dispatch_source_set_event_handler(report, ^{ provisioner->LogProgressIfChanged(); });
    dispatch_resume(report);
}

}
