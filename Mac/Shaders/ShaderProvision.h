#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <Platform/FileReaderHook.h>
#include <Shaders/Provision.h>
#include <Shaders/ShaderMapping.h>
#include <Shaders/Translation.h>
#include <Shaders/TranslationCache.h>

// The game side of shader provisioning: which packages the install has, in
// which order, and serving the result through the file reader hook. The tool
// runs this same code, so a dry run exercises what the game runs.

namespace bg3se::mac {

struct InstallPaths
{
    std::string MaterialsPak;   // <game>.app/Contents/Data/Materials.pak
    std::string ModsDir;        // ~/Documents/Larian Studios/Baldur's Gate 3/Mods
    std::string ModSettings;    // .../PlayerProfiles/Public/modsettings.lsx

    // For the running process: the game is the main executable.
    static InstallPaths ForThisProcess();
    // For a tool: the game's .app bundle is named.
    static InstallPaths ForGame(std::string const& appBundle);
};

// The Folder of every module in a modsettings.lsx document, in load order.
std::vector<std::string> ParseLoadOrder(std::string_view modSettingsXml);

struct ProvisionPlan
{
    std::unique_ptr<ProvisionMap> Map;
    std::string Problem;                    // set when nothing could be planned
    std::vector<std::string> Packages;      // mod packages in the order used
    std::vector<std::string> NotInLoadOrder; // planned all the same: the game mounts every package of the folder
    std::vector<std::string> Unreadable;
    // Two packages of one priority that ship the same material: which the
    // game reads is not known, in the load order or outside it.
    std::vector<std::string> Ties;
    double Milliseconds{ 0 };
};

// Opens Materials.pak and every package in the Mods folder, orders them, and
// decides every mod material. Every package, whether its module is in the
// load order or not: the game collects every .pak of the folder and mounts
// it (ls::PackedFileManager::LoadPackFiles asks nothing about modules), and
// a file is read from the package of the highest priority that has it. The
// order is that one: by the priority in a package's header, then the load
// order, then the packages outside it.
ProvisionPlan PlanFromInstall(InstallPaths const& paths, ContainerIdentity const& identity = VanillaContainerIdentity(),
    ProvisionTranslator const& translator = {}, bool translateFirst = false);

// What translation makes of a material, before anything is compiled:
// TranslateMaterial (Translation.h) for one game.
using MaterialTranslation = std::function<std::optional<TranslatedMaterial>(MaterialSource const& mod, std::string const& material, std::string& why)>;

// A translator made of a translation: compiles the material's functions where
// the game finds them (TranslatedFunctions.h). With a cache
// (TranslationCache.h), a material that was translated before from what the
// package has now is neither translated nor compiled: what was kept is
// served, and its functions are compiled when the game asks for them or
// before that in the background. One that was not is translated and compiled
// as without a cache, and kept.
ProvisionTranslator TranslatorOf(MaterialTranslation translation, std::shared_ptr<TranslationCache> cache = {});

// A translator for the game whose Metal interface `mac` is, with the
// interface table as it stands for that game (GameInterface.h: InterfaceFor).
ProvisionTranslator MaterialTranslator(std::shared_ptr<MacInterface const> mac, std::shared_ptr<InterfaceTable const> table,
    std::shared_ptr<TranslationCache> cache = {});

// Everything a translation depends on besides the mod's files, as the text a
// cache keeps its entries by: this build (the identifier the linker gave the
// image this code is in, which differs for any other code), the system's
// build (its Metal compiler took the sources that are kept), the game's Metal
// interface by its content, and the interface table as it stands for the
// game. Empty when this image has no identifier: nothing is then kept.
std::string TranslationMadeWith(MacInterface const& mac, InterfaceTable const& table);

// The cache of translated materials in `directory` for that game and this
// build; nullptr when there is nothing to keep entries by.
std::shared_ptr<TranslationCache> OpenTranslationCache(std::string directory, MacInterface const& mac, InterfaceTable const& table);

// The same decision over packages named explicitly, in the order given.
ProvisionPlan PlanFromPackages(std::string const& materialsPak, std::vector<std::string> const& modPackages,
    ProvisionTranslator const& translator = {}, bool translateFirst = false);

// One line per decision and a summary.
void LogPlan(ProvisionPlan const& plan);

// What the game has done with the current plan so far. A request is the game
// opening a provisioned path; it is served, or withheld (a stub whose .lsf the
// game did not get from the plan).
struct ProvisionProgress
{
    uint32_t Files{ 0 };            // provisioned files
    uint32_t FilesAsked{ 0 };       // of those, asked for at least once
    uint32_t FilesServed{ 0 };
    uint64_t Requests{ 0 };
    uint64_t Withheld{ 0 };         // requests, not files
    uint32_t Materials{ 0 };        // twins and translated materials
    uint32_t MaterialsServed{ 0 };  // their .lsf was served
    uint32_t ReadBefore{ 0 };       // read by the game before this plan served them, and not since
    uint32_t Refused{ 0 };
    uint32_t RefusedAsked{ 0 };     // refused materials the game asked for

    bool operator == (ProvisionProgress const&) const = default;
};

class ShaderProvisioner : public FileReaderSubscriber
{
public:
    ShaderProvisioner() = default;
    explicit ShaderProvisioner(std::unique_ptr<ProvisionMap> map) { Adopt(std::move(map)); }

    // Replaces what is served, from the next file the game opens. Earlier
    // maps stay alive: the game may still be reading bytes served from them.
    //
    // The game keeps a material it has read (ls::MaterialSystem::Request) and
    // a shader it has made (ls::ShaderManager::GetShader) for as long as
    // something uses them, and opens their files again only after that. A
    // material of the new plan that the game read before, with other bytes
    // than the plan serves, is therefore counted as read before: the plan
    // reaches it when the game next loads it.
    void Adopt(std::unique_ptr<ProvisionMap> map);

    std::span<uint8_t const> OnFileOpened(std::string_view path) override;

    ProvisionProgress Progress() const;

    // How much of the current plan the game has asked for so far, and what it has not.
    void LogProgress() const;
    // The same, when it differs from what was last logged. True when it logged.
    bool LogProgressIfChanged();

private:
    struct Plan
    {
        std::unique_ptr<ProvisionMap> Map;
        std::unique_ptr<std::atomic<uint32_t>[]> Asked;         // per file: requests seen
        std::unique_ptr<std::atomic<uint32_t>[]> Served;        // per file: times served
        std::unique_ptr<std::atomic<bool>[]> MaterialServed;    // per decision: its .lsf was served
        std::unique_ptr<std::atomic<bool>[]> ReadBefore;        // per decision: read before this plan, not since
        std::unique_ptr<std::atomic<bool>[]> RefusedAsked;      // per decision: refused, and the game asked for it
        mutable std::atomic<uint64_t> Withheld{ 0 };
    };

    void Remember(std::string_view path, uint8_t const* served, size_t size);

    std::atomic<Plan const*> plan_{ nullptr };
    std::mutex adoptMutex_;
    std::vector<std::unique_ptr<Plan>> plans_;                  // every plan adopted, newest last
    std::atomic<uint32_t> held_{ 0 };                           // stubs withheld because their .lsf was not served
    std::atomic<uint32_t> pathsLogged_{ 0 };

    // Every material .lsf the game has opened, and the bytes it last got for
    // it from a plan (none: the file as the game found it).
    struct Seen
    {
        uint8_t const* Data{ nullptr };
        size_t Size{ 0 };
    };
    mutable std::mutex seenMutex_;
    std::unordered_map<std::string, Seen> seen_;
    std::mutex progressMutex_;
    ProvisionProgress logged_{};
    bool everLogged_{ false };
};

// What the plan was made from: the load order file and every package in the
// Mods folder, by name, size and modification time.
struct InstallStamp
{
    uint64_t Value{ 0 };

    static InstallStamp Read(InstallPaths const& paths);
    bool operator == (InstallStamp const&) const = default;
};

// Re-plans when the player changes mods while the game runs (the in-game mod
// manager rewrites the load order; a download adds a package).
class InstallWatcher
{
public:
    // `planned`, when given, is called after each plan was made and adopted.
    InstallWatcher(InstallPaths paths, ShaderProvisioner& provisioner, ContainerIdentity identity = VanillaContainerIdentity(),
        ProvisionTranslator translator = {}, bool translateFirst = false, std::function<void()> planned = {});

    // Plans for the install as it is now and adopts the result.
    void PlanNow();

    // Called about once a second. Re-plans once the install has changed and
    // then stayed the same for one call, so a change in progress is not read
    // half-written. True when it adopted a new plan.
    bool Poll();

private:
    InstallPaths paths_;
    ShaderProvisioner& provisioner_;
    ContainerIdentity identity_;
    ProvisionTranslator translator_;
    bool translateFirst_{ false };
    std::function<void()> afterPlan_;
    InstallStamp planned_{};
    InstallStamp seen_{};
};

// shader_translation in bg3mse.conf (Platform/Config.h), by its value there, nothing when the file or the key
// is missing: a mod's own shaders are translated unless it is switched off, and "all" (in any case) also
// translates the materials that need none.
enum class TranslationSetting { Off, On, All };
TranslationSetting TranslationSettingOf(std::optional<std::string> const& value);

// Whether translation has anything to do for an install: it is not switched off, and a plan of the install
// hands a material to the translator (one whose shaders are not the game's own; with All, any a mod ships
// Vulkan shaders for). What a read of the game's own Metal functions outside the game goes by (the launcher's
// before the game starts, the installer's): with nothing to translate there is nothing to read them for.
// `why` says why not.
bool TranslationHasWork(InstallPaths const& paths, TranslationSetting setting, std::string& why,
    ContainerIdentity const& identity = VanillaContainerIdentity());

// A translator that is set up only when a plan first has a material for it. `setUp` runs then, once, on the
// thread that plans; what it gives translates that material and every one after. When it gives none, the
// reason it left in `why` is every material's.
ProvisionTranslator TranslatorSetUpWhenNeeded(std::function<ProvisionTranslator(std::string& why)> setUp);

// Called from the dylib's constructor: plans, hooks the file reader, and
// keeps the plan current while the game runs. Translation is set up when a
// plan first has a material to translate, not before.
void StartShaderProvisioning();

}
