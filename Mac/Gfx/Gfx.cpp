#include <stdafx.h>
#include <Gfx/Gfx.h>
#include <Gfx/GfxJitter.h>
#include <Gfx/GfxMetal.h>
#include <Gfx/GfxPatch.h>
#include <Gfx/GfxShadows.h>
#include <Gfx/GfxSites.h>
#include <Gfx/GfxTemporalLogic.h>
#include <Gfx/GfxUiRing.h>
#include <Platform/CodeWriter.h>
#include <Platform/Islands.h>
#include <Platform/Config.h>


#include <mach/mach_time.h>
#include <optional>
#include <os/lock.h>
#include <pthread.h>
#include <setjmp.h>

// Every fix follows the same shape: its sites are found and checked first
// (GfxSites), then its new code is built, then the islands are written before
// the sites that branch to them, so no site ever runs into an island that is
// not there yet. A fix that fails a check changes nothing.

namespace bg3se::mac::gfx {

namespace {

GameImage const* gImage = nullptr;

void* Live(uint64_t va) { return gImage->LoadedAt(va); }
uint64_t LiveVA(uint64_t va) { return (uint64_t)(uintptr_t)gImage->LoadedAt(va); }
uint64_t Address(void const* p) { return (uint64_t)(uintptr_t)p; }

// One write of a fix. A single word may be written at any time; several words
// in a row only where no thread can be running them, which the edit states.
struct Edit
{
    void* Addr;
    uint32_t const* Words;
    int Count;
    std::optional<BlockContract> Contract;      // for a run of several words; none for one word
};

// An island's words: nothing branches to the island until a later edit of the batch.
Edit Island(void* at, uint32_t const* words, int count)
{
    return { at, words, count, BlockContract::Unreachable };
}

// Several of the game's own words in a row: StartGfx runs from the dylib
// constructor, before the game has started its threads.
Edit AtStartUp(void* at, uint32_t const* words, int count)
{
    return { at, words, count, BlockContract::BeforeGameThreads };
}

// One word, which may be written at any time.
Edit Word(void* at, uint32_t const& word)
{
    return { at, &word, 1, std::nullopt };
}

// The edits in the order given. A run of several words must state its
// contract, and a run that crosses a page cannot be written as one; otherwise
// the whole batch is refused before anything of it is written.
std::string WriteBatch(std::initializer_list<Edit> edits)
{
    for (auto const& e : edits) {
        if (e.Count != 1 && !e.Contract) return "a run of several words states no contract";
        if (!OnOnePage(Address(e.Addr), (uint64_t)e.Count, CodePageSize())) {
            char why[96];
            snprintf(why, sizeof(why), "the %d words at %p cross a page boundary", e.Count, e.Addr);
            return why;
        }
    }
    for (auto const& e : edits) {
        std::string problem = e.Count == 1 ? WriteCodeWord(e.Addr, e.Words[0])
                                           : WriteCodeBlock(e.Addr, { e.Words, (size_t)e.Count }, *e.Contract);
        if (!problem.empty()) return "could not write the code: " + problem;
    }
    return {};
}

// --- Lua -------------------------------------------------------------------------------
// BG3's Lua was built without LUA_USE_POSIX, so each protected call uses setjmp
// and each error longjmp. On macOS those save and restore the signal mask and
// alternate stack, two syscalls per call. Lua changes neither, so the
// underscore versions behave the same.

std::string InstallLuaJumpFix(GfxSites const& sites)
{
    auto const& s = sites.LuaJump;
    if (!s.Ok()) return s.Problem;
    std::string why;
    auto island = static_cast<uint8_t*>(ReserveIsland(Live(s.ThrowCall), 2 * kFarJumpWords * 4, why));
    if (!island) return "no room for its islands: " + why;
    uint8_t* setjmpIsland = island + kFarJumpWords * 4;
    uint32_t toLongjmp[kFarJumpWords], toSetjmp[kFarJumpWords];
    WriteFarJump(toLongjmp, Address((void const*)&_longjmp));
    WriteFarJump(toSetjmp, Address((void const*)&_setjmp));

    uint32_t throwWord = EncodeBl(LiveVA(s.ThrowCall), Address(island));
    uint32_t protectWord = EncodeBl(LiveVA(s.ProtectCall), Address(setjmpIsland));
    if (!throwWord || !protectWord) return "islands out of range";
    // The throw goes first: _longjmp can unwind either kind of buffer, but
    // longjmp must never see a buffer from _setjmp.
    return WriteBatch({
        Island(island, toLongjmp, kFarJumpWords),
        Island(setjmpIsland, toSetjmp, kFarJumpWords),
        Word(Live(s.ThrowCall), throwWord),
        Word(Live(s.ProtectCall), protectWord),
    });
}

// --- Thread idling -------------------------------------------------------------------------
// BG3's workers, its scheduler and CapFrame spin while idle. Workers and the
// scheduler now sleep at once. CapFrame keeps stealing jobs between short
// sleeps, then sleeps almost to its deadline. Batch completion waits stay
// BG3's: their event has one semaphore that each blocking waiter resets, so
// more blocking waiters could strand one.

constexpr uint64_t kCapPollUs = 250;     // lands inside CapFrame's 0.5 ms job-check rhythm
constexpr uint64_t kCapTailUs = 1000;    // CapFrame spins its last 1 ms itself
constexpr uint64_t kCapMarginUs = 200;   // how much of that spin stays, for wake-up lateness

uint64_t gUsTicks = 0;
uint32_t gClockStartTicks = 0, gClockStarted = 0;

// Called from CapFrame's idle step in place of pthread_yield_np, with the
// clock and its wait target (microseconds from the clock's start).
void CapIdle(char const* clock, int64_t targetUs)
{
    bool started = clock[gClockStarted] != 0;
    uint64_t deadline = 0;
    if (started) {
        uint64_t start;
        memcpy(&start, clock + gClockStartTicks, sizeof(start));
        deadline = start + (uint64_t)targetUs * gUsTicks;
    }
    uint64_t wake = 0;
    if (CapIdleStep(mach_absolute_time(), deadline, started, kCapPollUs * gUsTicks, kCapTailUs * gUsTicks, kCapMarginUs * gUsTicks, &wake) == IdleAction::Sleep) {
        mach_wait_until(wake);
    } else {
        pthread_yield_np();
    }
}

std::string InstallThreadIdling(GfxSites const& sites)
{
    auto const& s = sites.ThreadIdling;
    if (!s.Ok()) return s.Problem;
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    gUsTicks = 1000ull * timebase.denom / timebase.numer;
    gClockStartTicks = s.ClockStartTicks;
    gClockStarted = s.ClockStarted;

    std::string why;
    void* island = ReserveIsland(Live(s.CapIdleYield), kCapIdleIslandWords * 4, why);
    if (!island) return "no room for its island: " + why;
    uint32_t islandWords[kCapIdleIslandWords];
    WriteCapIdleIsland(islandWords, s.CapClockReg, s.CapTargetReg, Address((void const*)&CapIdle));
    uint32_t yieldWord = EncodeBl(LiveVA(s.CapIdleYield), Address(island));
    if (!yieldWord) return "island out of range";
    uint32_t budget = ZeroInsteadOfLoad(s.WorkerBudgetWord);
    uint32_t window[2];
    ZeroInsteadOfConstant(window, s.SchedulerWindowWord);
    return WriteBatch({
        Island(island, islandWords, kCapIdleIslandWords),
        Word(Live(s.WorkerBudget), budget),
        AtStartUp(Live(s.SchedulerWindow[0]), window, 2),
        AtStartUp(Live(s.SchedulerWindow[1]), window, 2),
        Word(Live(s.CapIdleYield), yieldWord),
    });
}

// --- Reduced locking -------------------------------------------------------------------------
// Every draw's uniforms come from one per-frame bump offset on the renderer,
// under one lock, inlined at about 130 sites. The two hottest sites now take
// from a per-thread slice of the same buffer. A slice is reserved through
// BG3's own lock and bump, so the other sites still see BG3's protocol, and
// BeginFrame's reset retires every slice.

constexpr uint64_t kUniformSliceBytes = 16384;

std::atomic<uint64_t> gSliceGeneration{ 0 };
thread_local UniformSlice tSlice;
uint32_t gRendererLock = 0, gRendererOffset = 0;

uint64_t UniformAlloc(char* renderer, uint64_t size)
{
    uint64_t generation = gSliceGeneration.load(std::memory_order_acquire);
    uint64_t offset;
    if (SliceTake(tSlice, renderer, generation, size, &offset)) return offset;

    uint64_t bytes = SliceReserveBytes(kUniformSliceBytes, size);
    auto lock = reinterpret_cast<os_unfair_lock*>(renderer + gRendererLock);
    auto bump = reinterpret_cast<uint64_t*>(renderer + gRendererOffset);
    os_unfair_lock_lock(lock);
    uint64_t at = (*bump + 15) & ~(uint64_t)15;
    *bump = at + bytes;
    os_unfair_lock_unlock(lock);

    tSlice = { renderer, generation, at + size, at + bytes };
    return at;
}

static_assert(sizeof(std::atomic<uint64_t>) == sizeof(uint64_t) && std::atomic<uint64_t>::is_always_lock_free,
    "the frame-reset island increments the generation as a plain u64");

std::string InstallReducedLocking(GfxSites const& sites)
{
    auto const& s = sites.ReducedLocking;
    if (!s.Ok()) return s.Problem;
    gRendererLock = s.RendererLock;
    gRendererOffset = s.RendererOffset;

    std::string why;
    auto allocIsland = static_cast<uint8_t*>(ReserveIsland(Live(s.BindAlloc), (kFarJumpWords + kFrameResetIslandWords) * 4, why));
    if (!allocIsland) return "no room for its islands: " + why;
    uint8_t* resetIsland = allocIsland + kFarJumpWords * 4;

    uint32_t allocWords[kFarJumpWords], resetWords[kFrameResetIslandWords];
    WriteFarJump(allocWords, Address((void const*)&UniformAlloc));
    WriteFrameResetIsland(resetWords, s.FrameResetWord, Address(&gSliceGeneration));

    uint32_t bindWords[kBindAllocWords], setupWords[kSetupAllocWords];
    uint32_t resetWord = EncodeBl(LiveVA(s.FrameReset), Address(resetIsland));
    if (!resetWord || !WriteBindAlloc(bindWords, s.BindWords, LiveVA(s.BindAlloc), Address(allocIsland), LiveVA(s.BindResume)) ||
        !WriteSetupAlloc(setupWords, s.SetupWords, LiveVA(s.SetupAlloc), Address(allocIsland), LiveVA(s.SetupResume), s.SetupRendererInHolder)) {
        return "islands out of range";
    }
    return WriteBatch({
        Island(allocIsland, allocWords, kFarJumpWords),
        Island(resetIsland, resetWords, kFrameResetIslandWords),
        AtStartUp(Live(s.BindAlloc), bindWords, kBindAllocWords),
        AtStartUp(Live(s.SetupAlloc), setupWords, kSetupAllocWords),
        Word(Live(s.FrameReset), resetWord),
    });
}

// --- AO blur -------------------------------------------------------------------------------------
// BG3 blurs SSAO in two passes through one constant ring. With unified memory
// each bind records the ring, and replay reads whichever slot the counter
// names by then: the vertical direction, for both passes. Now each bind
// records the slot it just wrote, as BG3 does without unified memory.

std::string InstallAoBlurFix(GfxSites const& sites)
{
    auto const& s = sites.AoBlur;
    if (!s.Ok()) return s.Problem;
    std::string why;
    void* island = ReserveIsland(Live(s.Bind), kAoBlurIslandWords * 4, why);
    if (!island) return "no room for its island: " + why;
    uint32_t islandWords[kAoBlurIslandWords], bindWords[kAoBlurBindWords];
    WriteAoBlurIsland(islandWords, s.CounterLoad, s.CounterMask, s.SlotLoad, s.BindWords[5]);
    if (!WriteAoBlurBind(bindWords, s.BindWords, LiveVA(s.Bind), Address(island), s.BufferCommand)) return "island out of range";
    return WriteBatch({
        Island(island, islandWords, kAoBlurIslandWords),
        AtStartUp(Live(s.Bind), bindWords, kAoBlurBindWords),
    });
}

// ------------------------------------------------------------------------------------------------

// --- Noesis rings ------------------------------------------------------------------------------

std::string InstallUiRingFix(GfxSites const& sites, GfxHost const& host)
{
    return InstallUiRing(*gImage, sites.UiRing, host.Enabled("gfx_ui_ring_observe"));
}

// --- Shadows -------------------------------------------------------------------------------------

std::string InstallDuplicateShadows(GfxSites const& sites, GfxHost const&)
{
    return InstallDuplicateShadowFix(*gImage, sites.StagePacks);
}

std::string InstallImprovedShadowMask(GfxSites const& sites, GfxHost const&)
{
    return InstallImprovedShadows(sites.ShadowMask, sites.Programs);
}

// ------------------------------------------------------------------------------------------------

struct Fix
{
    char const* Key;
    std::string (*Install)(GfxSites const&, GfxHost const&);
};

template <std::string (*Install)(GfxSites const&)>
std::string Plain(GfxSites const& sites, GfxHost const&)
{
    return Install(sites);
}

// Code fixes first, then the hooks that build on them. The Metal features
// (kMetalKeys) come after these: they make a Metal device and may start a
// thread, which nothing that writes the game's code should have beside it.
constexpr Fix kFixes[] = {
    { "gfx_lua_jump_fix", Plain<InstallLuaJumpFix> },
    { "gfx_thread_idling", Plain<InstallThreadIdling> },
    { "gfx_reduced_locking", Plain<InstallReducedLocking> },
    { "gfx_ao_blur_fix", Plain<InstallAoBlurFix> },
    { "gfx_ui_ring_fix", InstallUiRingFix },
    { "gfx_duplicate_shadow_fix", InstallDuplicateShadows },
    { "gfx_improved_shadows", InstallImprovedShadowMask },
};

// --- Metal -----------------------------------------------------------------------------------------
// The Metal features share one set of hooks (GfxMetal.h), put on once for
// those that are on. They change none of the game's code, and all depend on
// the game reaching Metal through the methods the hooks replace. The cluster
// guard also depends on BG3's clustered-lighting pass being the one it was
// written against, and Temporal on the functions that fill the constants it
// reads and on the three loads of BG3's jitter sample, which it lengthens:
// without the longer sequence it still runs, on BG3's 8 samples.

constexpr char const* kStallLog = "gfx_stall_log";
constexpr char const* kBindDedup = "gfx_bind_dedup";
constexpr char const* kClusterGuard = "gfx_cluster_guard";
constexpr char const* kTemporal = "gfx_temporal";
constexpr char const* kTemporalSharpen = "gfx_temporal_sharpen";
constexpr char const* kMetalKeys[] = { kStallLog, kBindDedup, kClusterGuard, kTemporal };

void StartMetal(GfxSites const& sites, GfxHost const& host, std::function<void(char const*, std::string)> const& note)
{
    auto const& enabled = host.Enabled;
    // Why a key cannot be on, by key; the others go on without it.
    std::string refusal[std::size(kMetalKeys)];
    bool wanted = false;
    for (size_t i = 0; i < std::size(kMetalKeys); i++) {
        if (!enabled(kMetalKeys[i])) continue;
        refusal[i] = sites.MetalCalls.Problem;
        if (refusal[i].empty() && kMetalKeys[i] == kClusterGuard) refusal[i] = sites.Cluster.Problem;
        if (refusal[i].empty() && kMetalKeys[i] == kTemporal) refusal[i] = sites.Jitter.Problem;
        wanted = wanted || refusal[i].empty();
    }
    auto on = [&](char const* key) {
        size_t i = (size_t)(std::find(std::begin(kMetalKeys), std::end(kMetalKeys), key) - std::begin(kMetalKeys));
        return enabled(key) && refusal[i].empty();
    };

    MetalChoices choices;
    choices.StallLog = on(kStallLog);
    choices.BindDedup = on(kBindDedup);
    choices.ClusterGuard = on(kClusterGuard);
    choices.Temporal = on(kTemporal);
    if (choices.Temporal) {
        auto sharpen = host.Value ? host.Value(kTemporalSharpen) : std::nullopt;
        choices.Sharpen = ParseStrength(sharpen ? sharpen->c_str() : nullptr, choices.Sharpen);
    }
    MetalOutcome outcome;
    if (wanted) {
        if (host.InstallMetal) {
            outcome = host.InstallMetal(choices);
        } else {
            outcome.All = "no Metal hooks in this build";
        }
    }
    // The longer jitter goes in only once Temporal is on: one word at each
    // load, which may be written at any time.
    if (choices.Temporal && outcome.All.empty() && outcome.Temporal.empty()) {
        // Temporal turns the longer sequence on while it has a frame path taken over.
        std::string why = InstallJitter(*gImage, sites.Jitter);
        if (!why.empty()) {
            WARN("[Gfx] %s: the longer jitter sequence is not installed (%s); it runs on BG3's 8 samples", kTemporal, why.c_str());
        }
    }
    for (size_t i = 0; i < std::size(kMetalKeys); i++) {
        char const* key = kMetalKeys[i];
        if (!enabled(key)) continue;
        std::string problem = !refusal[i].empty() ? refusal[i]
                              : !outcome.All.empty() ? outcome.All
                              : key == kClusterGuard ? outcome.ClusterGuard
                              : key == kTemporal ? outcome.Temporal
                              : "";
        note(key, std::move(problem));
    }
}

// Resolved once, the first time any gfx key is on, and kept for the hooks that
// run later. Never freed: hooks may read it until the process ends.
GfxSites const* gSites = nullptr;

}

std::span<char const* const> GfxKeys()
{
    static constexpr auto kKeys = [] {
        std::array<char const*, std::size(kFixes) + std::size(kMetalKeys)> keys{};
        size_t at = 0;
        for (auto const& fix : kFixes) keys[at++] = fix.Key;
        for (char const* key : kMetalKeys) keys[at++] = key;
        return keys;
    }();
    return kKeys;
}

GfxSites const* ResolvedSites()
{
    return gSites;
}

std::vector<GfxOutcome> StartGfx(GameImage const* image, GfxHost const& host)
{
    auto const& enabled = host.Enabled;
    std::vector<GfxOutcome> outcomes;
    bool any = false;
    for (char const* key : GfxKeys()) any = any || enabled(key);
    if (!any) return outcomes;

    auto note = [&](char const* key, std::string problem) {
        if (problem.empty()) {
            INFO("[Gfx] %s: installed", key);
        } else {
            WARN("[Gfx] %s: SKIPPED (%s)", key, problem.c_str());
        }
        outcomes.push_back({ key, std::move(problem) });
    };
    if (!image) {
        for (char const* key : GfxKeys()) {
            if (enabled(key)) note(key, "the game's symbol table cannot be read");
        }
        return outcomes;
    }
    gImage = image;
    if (!gSites) gSites = new GfxSites(FindGfxSites(*image));
    GfxSites const& sites = *gSites;

    for (auto const& fix : kFixes) {
        if (enabled(fix.Key)) note(fix.Key, fix.Install(sites, host));
    }
    StartMetal(sites, host, note);
    return outcomes;
}

bool FeatureEnabled(std::string_view key)
{
    return mac::FeatureEnabled(key);
}

void StartGfx()
{
    StartGfx(GameImage::Current(), GfxHost{
        [](std::string_view key) { return FeatureEnabled(key); },
        InstallMetalHooks,
        [](std::string_view key) { return ConfValue(key); },
    });
}

}
