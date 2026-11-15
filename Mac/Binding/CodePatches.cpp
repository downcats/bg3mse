#include <Binding/CodePatches.h>

#include <cstring>
#include <mutex>

#include <Binding/Startup.h>
#include <Engine/Entries/PatchNames.h>
#include <Platform/CodeWriter.h>
#include <Platform/Log.h>

namespace bg3se::mac::binding {

namespace {

namespace site = engine::patches;

constexpr char const* kKillLauncher[] = { site::kRestartThroughSteam };
// Every function that holds the loop of ls::ModuleSettings::IsModded (Engine/Entries/Patches.cpp): on Windows the patch
// changes the one function all of them call.
constexpr char const* kIsModded[] = {
    site::kUnlockAchievementGate, site::kAchievementCounterGate, site::kStoryAchievementGate,
    site::kSavedAsModded, site::kLoadedAsModded, site::kSaveErrorSaveMods, site::kSaveErrorServerMods, site::kSaveScreenHasMods,
};
constexpr char const* kThrowError[] = { site::kSaveErrorsWithoutMods };

// The names are those of BinaryMappings.xml, which Norbyte's code passes.
const CodePatchMapping kMappings[] = {
    { "KillLauncher", kKillLauncher },
    { "ls::ModuleSettings::IsModded", kIsModded },
    { "esv::SavegameManager::ThrowError", kThrowError },
};

struct State
{
    bool Applied{ false };
    bool RefusalLogged{ false };
    std::vector<CodeSiteWrite> Sites;
    std::vector<uint32_t> Originals;
};

std::mutex gMutex;
State gStates[std::size(kMappings)];

int IndexOf(std::string_view mapping)
{
    for (size_t i = 0; i < std::size(kMappings); i++) {
        if (mapping == kMappings[i].Mapping) return static_cast<int>(i);
    }
    return -1;
}

uint32_t WordAt(void const* at)
{
    uint32_t word;
    memcpy(&word, at, sizeof(word));
    return word;
}

// The resolved site of `name`, or nullptr with the reason.
engine::Resolved const* SiteOf(char const* name, engine::Resolution const& resolution, std::string& problem)
{
    auto entry = resolution.Find(name);
    if (!entry) {
        problem = std::string(name) + ": the engine table has no such entry";
    } else if (entry->Of->What != engine::Kind::CodeSite || !entry->Of->Derived) {
        problem = std::string(name) + ": the entry is not a code site";
    } else if (!entry->Usable()) {
        problem = std::string(name) + ": not resolved: " + entry->Reason;
    } else {
        return entry;
    }
    return nullptr;
}

}

std::span<CodePatchMapping const> CodePatchMappings() { return kMappings; }

std::string CodePatchRefusal(std::string_view mapping, engine::Resolution const& resolution)
{
    int index = IndexOf(mapping);
    if (index < 0) return "there is no such mapping";
    std::string problem;
    for (auto name : kMappings[index].Sites) {
        if (!SiteOf(name, resolution, problem)) return problem;
    }
    return {};
}

std::string WriteCodeSites(std::span<CodeSiteWrite const> sites, std::vector<uint32_t>& originals)
{
    std::vector<uint32_t> found;
    for (auto const& site : sites) {
        uint32_t word = WordAt(site.At);
        if (!site.Was.Matches(word)) {
            char text[96];
            snprintf(text, sizeof(text), "the instruction at %p is %08x, not the one that was read; nothing was written", site.At, word);
            return text;
        }
        found.push_back(word);
    }
    for (size_t i = 0; i < sites.size(); i++) {
        auto problem = WriteCodeWord(sites[i].At, sites[i].Becomes);
        if (problem.empty()) continue;
        for (size_t back = 0; back < i; back++) {
            if (auto undo = WriteCodeWord(sites[back].At, found[back]); !undo.empty()) problem += "; and a site already written could not be put back: " + undo;
        }
        return problem;
    }
    originals = std::move(found);
    return {};
}

std::string RestoreCodeSites(std::span<CodeSiteWrite const> sites, std::span<uint32_t const> originals)
{
    std::string problems;
    for (size_t i = 0; i < sites.size() && i < originals.size(); i++) {
        std::string problem;
        if (WordAt(sites[i].At) != sites[i].Becomes) {
            char text[96];
            snprintf(text, sizeof(text), "the instruction at %p is no longer the one written; left as it is", sites[i].At);
            problem = text;
        } else {
            problem = WriteCodeWord(sites[i].At, originals[i]);
        }
        if (problem.empty()) continue;
        if (!problems.empty()) problems += "; ";
        problems += problem;
    }
    return problems;
}

bool ApplyCodePatch(std::string_view mapping)
{
    int index = IndexOf(mapping);
    if (index < 0) {
        // Norbyte's words for a mapping the table of mappings does not have.
        Log(LogLevel::Error, "Cannot apply patch - no such mapping: '%.*s'", static_cast<int>(mapping.size()), mapping.data());
        return false;
    }
    std::lock_guard lock(gMutex);
    auto& state = gStates[index];
    if (state.Applied) return true;

    auto refuse = [&](std::string const& why) {
        if (!state.RefusalLogged) Log(LogLevel::Warning, "patch '%s' not applied: %s", kMappings[index].Mapping, why.c_str());
        state.RefusalLogged = true;
        return false;
    };
    auto resolution = ResolutionForLibraries();
    if (!resolution) return refuse("the engine table was not resolved for this game");

    std::vector<CodeSiteWrite> sites;
    for (auto name : kMappings[index].Sites) {
        std::string problem;
        auto entry = SiteOf(name, *resolution, problem);
        if (!entry) return refuse(problem);
        void* at = resolution->Address(*entry);
        if (!at) return refuse(std::string(name) + ": its image is not loaded in this process");
        auto const& derivation = *entry->Of->Derived;
        sites.push_back({ at, derivation.Pattern[derivation.At], entry->Of->Becomes });
    }
    std::vector<uint32_t> originals;
    if (auto problem = WriteCodeSites(sites, originals); !problem.empty()) return refuse(problem);

    state.Sites = std::move(sites);
    state.Originals = std::move(originals);
    state.Applied = true;
    Log(LogLevel::Info, "patch '%s' applied: %zu instruction(s) of the game replaced", kMappings[index].Mapping, state.Sites.size());
    return true;
}

bool UndoCodePatch(std::string_view mapping)
{
    int index = IndexOf(mapping);
    if (index < 0) {
        Log(LogLevel::Error, "Cannot undo patch - no such mapping: '%.*s'", static_cast<int>(mapping.size()), mapping.data());
        return false;
    }
    std::lock_guard lock(gMutex);
    auto& state = gStates[index];
    if (!state.Applied) return false;
    if (auto problem = RestoreCodeSites(state.Sites, state.Originals); !problem.empty()) {
        // Still applied, as far as anyone asking is concerned: some of its instructions are.
        Log(LogLevel::Error, "patch '%s' not fully taken out: %s", kMappings[index].Mapping, problem.c_str());
        return false;
    }
    Log(LogLevel::Info, "patch '%s' taken out", kMappings[index].Mapping);
    state.Applied = false;
    state.Sites.clear();
    state.Originals.clear();
    return true;
}

bool WasCodePatchApplied(std::string_view mapping)
{
    int index = IndexOf(mapping);
    if (index < 0) {
        Log(LogLevel::Error, "Cannot apply patch - no such mapping: '%.*s'", static_cast<int>(mapping.size()), mapping.data());
        return false;
    }
    std::lock_guard lock(gMutex);
    return gStates[index].Applied;
}

}
