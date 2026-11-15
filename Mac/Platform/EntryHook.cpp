#include <Platform/CodeWriter.h>
#include <Platform/EntryHook.h>
#include <Platform/Islands.h>
#include <Platform/Symbols.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace bg3se::mac {

namespace {

// The hooks of this copy of the platform layer: constant-initialised, never freed.
constinit EntryHook* gHooks = nullptr;

// An island block: the far jump to the replacement, then the trampoline.
constexpr size_t kBlockWords = kFarJumpWords + kTrampolineMaxWords;
constexpr uint64_t kTrampolineOffset = kFarJumpWords * 4;

constexpr uint32_t kLdrX16 = 0x58000050;    // ldr x16, #8
constexpr uint32_t kBrX16 = 0xd61f0200;     // br x16

std::string Refused(HookRefusal refusal, void const* target, std::string const& detail = {})
{
    char text[256];
    snprintf(text, sizeof(text), "no entry hook on %p: %s%s%s", target, Describe(refusal), detail.empty() ? "" : ": ", detail.c_str());
    return text;
}

struct Placement
{
    std::span<uint32_t const> Function;
    uint64_t Address;
    uint64_t Replacement;
    HookRefusal Refusal;
};

// The island block for the address the allocator offers, or why not there.
std::string BuildHookBlock(uintptr_t at, std::span<uint32_t> words, void* context)
{
    auto& p = *static_cast<Placement*>(context);
    auto plan = PlanTrampoline(p.Function, p.Address, at + kTrampolineOffset);
    if (plan.Ok() && !EncodeBranch(p.Address, at, false)) plan.Refusal = HookRefusal::NoIsland;
    if (!plan.Ok()) {
        p.Refusal = plan.Refusal;
        return Describe(plan.Refusal);
    }
    EncodeFarJump(words.first<kFarJumpWords>(), p.Replacement);
    std::copy(plan.Words, plan.Words + plan.Count, words.begin() + kFarJumpWords);
    return {};
}

// Whether the instruction at `at` branches to a far jump: the patch of an entry
// hook (this copy's or another's).
bool BranchesToFarJump(uint64_t at, uint32_t word)
{
    if ((word & 0xfc000000) != 0x14000000) return false;
    uint64_t to = DirectBranchTarget(at, word);
    if (!QueryPage(to).Readable || !QueryPage(to + 4).Readable) return false;
    uint32_t jump[2];
    memcpy(jump, reinterpret_cast<void const*>(to), sizeof(jump));
    return jump[0] == kLdrX16 && jump[1] == kBrX16;
}

// Whether every page of [at, at + bytes) is mapped readable and executable.
bool IsCode(uintptr_t at, size_t bytes)
{
    uintptr_t pageSize = CodePageSize();
    for (uintptr_t page = at & ~(pageSize - 1); page < at + bytes; page += pageSize) {
        auto info = QueryPage(page);
        if (!info.Readable || !info.Executable) return false;
    }
    return true;
}

}

struct EntryHookAccess
{
    static EntryHook* Make(void* target, uint32_t first, void* island)
    {
        auto hook = new EntryHook(target, first, island, static_cast<uint8_t*>(island) + kTrampolineOffset);
        hook->next_ = gHooks;
        gHooks = hook;
        return hook;
    }

    static EntryHook* Find(void const* target)
    {
        for (EntryHook* h = gHooks; h; h = h->next_) {
            if (h->target_ == target) return h;
        }
        return nullptr;
    }

    static uint32_t First(EntryHook const& h) { return h.first_; }
    static uint32_t Patch(EntryHook const& h) { return EncodeBranch((uintptr_t)h.target_, (uintptr_t)h.island_, false); }
    static void SetCommitted(EntryHook& h, bool committed) { h.committed_ = committed; }

    // Points the island of an uncommitted hook at another replacement: the far
    // jump's literal, both of its words in one swap (or, on a MAP_JIT page, one
    // aligned 8-byte store). Nothing branches to the island while the hook is
    // not committed, but a thread that entered it before a removal may still be
    // there; its ldr reads the literal from the page before the swap or from the
    // one after, never a mix, so it reaches the old replacement or the new one,
    // and both stay callable.
    static std::string Retarget(EntryHook& h, void const* replacement)
    {
        uint32_t live;
        memcpy(&live, h.target_, sizeof(live));
        if (live != h.first_) {
            char text[160];
            snprintf(text, sizeof(text), "the first instruction of %p changed since its hook was prepared", h.target_);
            return text;
        }
        auto island = static_cast<uint32_t*>(h.island_);
        if (island[0] != kLdrX16 || island[1] != kBrX16) return "the hook's island does not hold its far jump";
        uint32_t jump[kFarJumpWords];
        EncodeFarJump(jump, reinterpret_cast<uint64_t>(replacement));
        WordPatch literal[] = { { island + 2, jump[2] }, { island + 3, jump[3] } };
        return WriteCodeWordsLocked(literal);
    }
};

EntryHook* PrepareEntryHook(void* target, size_t functionBytes, void const* replacement, void** original, std::string& problem,
    HookRefusal* refusal)
{
    problem.clear();
    HookRefusal ignored;
    HookRefusal& refused = refusal ? *refusal : ignored;
    refused = HookRefusal::None;
    auto address = reinterpret_cast<uintptr_t>(target);
    if (!target || !replacement || !original) {
        problem = "an entry hook needs a function, a replacement and a place for the original";
        return nullptr;
    }
    if (address & 3) {
        refused = HookRefusal::Misaligned;
        problem = Refused(refused, target);
        return nullptr;
    }
    if (functionBytes < 4 || (functionBytes & 3)) {
        refused = HookRefusal::NoFunction;
        problem = Refused(refused, target);
        return nullptr;
    }
    // Before the lock: dladdr takes the dynamic loader's.
    void const* header = ImageHeaderOf(target);

    std::lock_guard lock(CodeLock());
    if (EntryHook* existing = EntryHookAccess::Find(target)) {
        if (existing->Committed()) {
            refused = HookRefusal::AlreadyHooked;
            problem = Refused(refused, target);
            return nullptr;
        }
        problem = EntryHookAccess::Retarget(*existing, replacement);
        if (!problem.empty()) return nullptr;
        __atomic_store_n(original, existing->Trampoline(), __ATOMIC_RELEASE);
        return existing;
    }
    if (!IsCode(address, functionBytes)) {
        refused = HookRefusal::NotExecutable;
        problem = Refused(refused, target);
        return nullptr;
    }
    std::vector<uint32_t> words(functionBytes / 4);
    memcpy(words.data(), target, functionBytes);
    if (BranchesToFarJump(address, words[0])) {
        refused = HookRefusal::AlreadyHooked;
        problem = Refused(refused, target, "its first instruction branches to a far jump");
        return nullptr;
    }

    // What does not depend on where the trampoline goes is refused before a
    // block is looked for.
    auto probe = PlanTrampoline(words, address, address);
    if (!probe.Ok() && probe.Refusal != HookRefusal::OutOfRange) {
        refused = probe.Refusal;
        problem = Refused(refused, target);
        return nullptr;
    }

    Placement placement{ words, address, reinterpret_cast<uint64_t>(replacement), HookRefusal::None };
    std::string why;
    void* island = ReserveIslandLocked(address, header, kBlockWords, BuildHookBlock, &placement, why);
    if (!island) {
        refused = placement.Refusal != HookRefusal::None ? placement.Refusal : HookRefusal::NoIsland;
        problem = Refused(refused, target, why);
        return nullptr;
    }
    EntryHook* hook = EntryHookAccess::Make(target, words[0], island);
    // Published before the patch can be live.
    __atomic_store_n(original, hook->Trampoline(), __ATOMIC_RELEASE);
    return hook;
}

std::string CommitEntryHooks(std::span<EntryHook* const> hooks)
{
    std::lock_guard lock(CodeLock());
    std::vector<EntryHook*> pending;
    for (EntryHook* h : hooks) {
        if (!h) return "no hook to commit";
        if (h->Committed()) continue;
        uint32_t live;
        memcpy(&live, h->Target(), sizeof(live));
        if (live != EntryHookAccess::First(*h)) {
            char text[160];
            snprintf(text, sizeof(text), "the first instruction of %p changed since its hook was prepared", h->Target());
            return text;
        }
        pending.push_back(h);
    }
    // One swap per page.
    uintptr_t pageSize = CodePageSize();
    auto page = [&](EntryHook const* h) { return reinterpret_cast<uintptr_t>(h->Target()) & ~(pageSize - 1); };
    std::sort(pending.begin(), pending.end(), [&](EntryHook* a, EntryHook* b) { return page(a) < page(b); });
    for (size_t from = 0; from < pending.size();) {
        size_t to = from;
        std::vector<WordPatch> patches;
        while (to < pending.size() && page(pending[to]) == page(pending[from])) {
            patches.push_back({ pending[to]->Target(), EntryHookAccess::Patch(*pending[to]) });
            to++;
        }
        if (auto problem = WriteCodeWordsLocked(patches); !problem.empty()) {
            return from ? "hooks on earlier pages went in, the rest did not: " + problem : problem;
        }
        for (size_t i = from; i < to; i++) EntryHookAccess::SetCommitted(*pending[i], true);
        from = to;
    }
    return {};
}

std::string CommitEntryHook(EntryHook* hook)
{
    return CommitEntryHooks({ &hook, 1 });
}

EntryHook* InstallEntryHook(void* target, size_t functionBytes, void const* replacement, void** original, std::string& problem)
{
    EntryHook* hook = PrepareEntryHook(target, functionBytes, replacement, original, problem);
    if (!hook) return nullptr;
    problem = CommitEntryHook(hook);
    return problem.empty() ? hook : nullptr;
}

std::string RemoveEntryHook(EntryHook* hook)
{
    if (!hook) return "no hook to remove";
    std::lock_guard lock(CodeLock());
    if (!hook->Committed()) return {};
    uint32_t live;
    memcpy(&live, hook->Target(), sizeof(live));
    if (live != EntryHookAccess::Patch(*hook)) {
        char text[160];
        snprintf(text, sizeof(text), "the first instruction of %p is not the hook's branch any more", hook->Target());
        return text;
    }
    WordPatch restore{ hook->Target(), EntryHookAccess::First(*hook) };
    if (auto problem = WriteCodeWordsLocked({ &restore, 1 }); !problem.empty()) return problem;
    EntryHookAccess::SetCommitted(*hook, false);
    return {};
}

EntryHook* FindEntryHook(void const* target)
{
    std::lock_guard lock(CodeLock());
    return EntryHookAccess::Find(target);
}

HookRefusal CheckEntryHook(GameImage const& image, uint64_t va, uint64_t end)
{
    if (va & 3) return HookRefusal::Misaligned;
    if (end <= va || ((end - va) & 3)) return HookRefusal::NoFunction;
    std::span<uint32_t const> words(static_cast<uint32_t const*>(image.CodeAt(va)), (end - va) / 4);
    auto probe = PlanTrampoline(words, va, va);
    if (!probe.Ok() && probe.Refusal != HookRefusal::OutOfRange) return probe.Refusal;

    // The padding in link-time addresses.
    auto header = reinterpret_cast<uintptr_t>(image.CodeAt(image.BaseVA()));
    auto padding = HeaderPaddingOf(reinterpret_cast<void const*>(header));
    if (!padding) return HookRefusal::NoIsland;
    uint64_t low = image.BaseVA() + (padding->Low - header);
    uint64_t high = image.BaseVA() + (padding->High - header);
    uint64_t bytes = kBlockWords * 4, pageSize = CodePageSize();
    auto crossesPage = [&](uint64_t block) { return block / pageSize != (block + bytes - 1) / pageSize; };
    uint64_t lowest = (low + kIslandAlign - 1) & ~(uint64_t)(kIslandAlign - 1);
    while (lowest + bytes <= high && crossesPage(lowest)) lowest += kIslandAlign;
    if (lowest + bytes > high) return HookRefusal::NoIsland;
    uint64_t highest = (high - bytes) & ~(uint64_t)(kIslandAlign - 1);
    while (highest > lowest && crossesPage(highest)) highest -= kIslandAlign;
    // Branch range and the trampoline's reach are each satisfied by an
    // interval of addresses, so both ends passing means every block between does.
    for (uint64_t block : { lowest, highest }) {
        if (!InBranchRange(va, block, bytes)) return HookRefusal::NoIsland;
        auto plan = PlanTrampoline(words, va, block + kTrampolineOffset);
        if (!plan.Ok()) return plan.Refusal;
    }
    return HookRefusal::None;
}

}
