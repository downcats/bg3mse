#include <Platform/CodeWriter.h>
#include <Platform/Islands.h>

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <vector>

namespace bg3se::mac {

namespace {

// Bytes left untouched right after the load commands.
constexpr uintptr_t kPaddingMargin = 0x100;

// A run of memory islands are handed out from, bottom up. Every block below
// Cursor is taken; a block passed over because it was out of a caller's range
// or refused by its builder stays above Cursor for later callers.
struct Area
{
    void const* Header;     // the image whose padding this is; nullptr for a mapped page
    bool Jit;               // a MAP_JIT page
    uintptr_t Low, High, Cursor;
    Area* Next;
};

// FillBlock's answer when the block is no longer zero.
constexpr char kTaken[] = "the block is taken";

// Constant-initialised and never freed: islands live as long as the process.
constinit Area* gPaddings = nullptr;
constinit Area* gNearPages = nullptr;

uintptr_t AlignUp(uintptr_t value, uintptr_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

bool AllZero(uint8_t const* p, size_t bytes)
{
    for (size_t i = 0; i < bytes; i++) {
        if (p[i]) return false;
    }
    return true;
}

struct ReserveEdit
{
    std::span<uint32_t const> Words;
    size_t Bytes;
};

// Inside the swap: the block must still be zero in the copy being patched.
std::string FillBlock(uint8_t* copyOfBlock, void* context)
{
    auto const& e = *static_cast<ReserveEdit const*>(context);
    if (!AllZero(copyOfBlock, e.Bytes)) return kTaken;
    for (size_t at = 0; at < e.Bytes; at += 4) memcpy(copyOfBlock + at, &kIslandFill, 4);
    memcpy(copyOfBlock, e.Words.data(), e.Words.size_bytes());
    return {};
}

struct Request
{
    uintptr_t Near;
    size_t WordCount;
    IslandBuilder Build;
    void* Context;
};

enum class Outcome { Reserved, NothingFits, Failed };

// The lowest block of `area` that is zero, in range, does not cross a page,
// and that the builder accepts; reserved and written in one swap.
Outcome ReserveFrom(Area& area, Request const& r, uintptr_t& block, std::string& problem)
{
    size_t bytes = AlignUp(r.WordCount * 4, kIslandAlign);
    uintptr_t pageSize = CodePageSize();
    uint32_t words[64];
    if (r.WordCount > std::size(words)) {
        problem = "an island of more than 64 words was asked for";
        return Outcome::Failed;
    }
    uintptr_t passedOver = 0;     // the first block left for other callers
    for (uintptr_t at = AlignUp(std::max(area.Cursor, area.Low), kIslandAlign); at + bytes <= area.High && at + bytes > at;
         at += kIslandAlign) {
        if ((at & ~(pageSize - 1)) != ((at + bytes - 1) & ~(pageSize - 1))) {
            if (!passedOver) passedOver = at;                  // a smaller island may still fit
            at = AlignUp(at + 1, pageSize) - kIslandAlign;     // the next block starts the next page
            continue;
        }
        if (!InBranchRange(r.Near, at, bytes)) {
            if (at > r.Near) break;     // only further away from here on
            if (!passedOver) passedOver = at;
            continue;
        }
        if (!AllZero(reinterpret_cast<uint8_t const*>(at), bytes)) continue;
        std::span<uint32_t> built(words, r.WordCount);
        std::fill(built.begin(), built.end(), kIslandFill);
        if (r.Build) {
            if (std::string why = r.Build(at, built, r.Context); !why.empty()) {
                problem = why;
                if (!passedOver) passedOver = at;
                continue;
            }
        }
        ReserveEdit edit{ built, bytes };
        std::string why = EditCodeLocked(reinterpret_cast<void*>(at), bytes, FillBlock, &edit);
        if (why == kTaken) continue;
        if (!why.empty()) {
            problem = why;
            return Outcome::Failed;
        }
        area.Cursor = passedOver ? passedOver : at + bytes;
        block = at;
        return Outcome::Reserved;
    }
    return Outcome::NothingFits;
}

Area* PaddingArea(void const* header)
{
    for (Area* a = gPaddings; a; a = a->Next) {
        if (a->Header == header) return a;
    }
    auto padding = HeaderPaddingOf(header);
    if (!padding) return nullptr;
    gPaddings = new Area{ header, false, padding->Low, padding->High, padding->Low, gPaddings };
    return gPaddings;
}

// Pages within range of `near` that the VM reports no region at, nearest
// first: one per gap between the regions it reports. A gap below the lowest
// region the walk sees may be outside what the process can map at all (below
// the main image lies its __PAGEZERO), so each is only a candidate.
std::vector<uintptr_t> FreePagesNear(uintptr_t near)
{
    uintptr_t pageSize = CodePageSize();
    uintptr_t low = near > (uintptr_t)kIslandBranchRange ? AlignUp(near - kIslandBranchRange + 1, pageSize) : pageSize;
    uintptr_t high = (near + kIslandBranchRange) & ~(pageSize - 1);     // pages end at or below this
    std::vector<uintptr_t> pages;
    auto consider = [&](uintptr_t gapStart, uintptr_t gapEnd) {
        gapStart = std::max(AlignUp(gapStart, pageSize), low);
        gapEnd = std::min(gapEnd & ~(pageSize - 1), high);
        if (gapEnd < gapStart + pageSize) return;
        // The page of the gap nearest `near`.
        uintptr_t page = near < gapStart ? gapStart : near >= gapEnd ? gapEnd - pageSize : (near & ~(pageSize - 1));
        if (page + pageSize > gapEnd) page = gapEnd - pageSize;
        if (InBranchRange(near, page, pageSize)) pages.push_back(page);
    };
    uintptr_t gapStart = low;
    while (gapStart < high) {
        mach_vm_address_t start = gapStart;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
            reinterpret_cast<vm_region_info_t>(&info), &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS) {
            consider(gapStart, high);       // nothing mapped above
            break;
        }
        if (start > gapStart) consider(gapStart, std::min<uintptr_t>(start, high));
        gapStart = std::max<uintptr_t>(gapStart, start + size);
    }
    auto distance = [&](uintptr_t p) { return p > near ? p - near : near - p; };
    std::sort(pages.begin(), pages.end(), [&](uintptr_t a, uintptr_t b) { return distance(a) < distance(b); });
    return pages;
}

// A new page for islands near `near`, zero-filled: r-x and written through
// the code writer, or MAP_JIT.
Area* MapNearPage(uintptr_t near, bool jit, std::string& problem)
{
    uintptr_t pageSize = CodePageSize();
    auto candidates = FreePagesNear(near);
    problem = "no free page within branch range";
    for (uintptr_t candidate : candidates) {
        uintptr_t at = 0;
        if (!jit) {
            mach_vm_address_t address = candidate;
            // FIXED without OVERWRITE: if the gap cannot be mapped, or another
            // thread has mapped it meanwhile, this fails and changes nothing.
            if (mach_vm_allocate(mach_task_self(), &address, pageSize, VM_FLAGS_FIXED) != KERN_SUCCESS) continue;
            if (mach_vm_protect(mach_task_self(), address, pageSize, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) {
                mach_vm_deallocate(mach_task_self(), address, pageSize);
                problem = "executable memory that is not MAP_JIT is refused";
                return nullptr;
            }
            at = address;
        } else {
            // A hint, not MAP_FIXED: MAP_FIXED would replace whatever another
            // thread may have mapped there meanwhile.
            void* mapped = mmap(reinterpret_cast<void*>(candidate), pageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
            if (mapped == MAP_FAILED) {
                problem = "no MAP_JIT page could be mapped";
                return nullptr;
            }
            at = reinterpret_cast<uintptr_t>(mapped);
            if (!InBranchRange(near, at, pageSize)) {
                munmap(mapped, pageSize);
                continue;
            }
        }
        problem.clear();
        gNearPages = new Area{ nullptr, jit, at, at + pageSize, at, gNearPages };
        return gNearPages;
    }
    return nullptr;
}

std::string BuildFarJump(uintptr_t, std::span<uint32_t> words, void* context)
{
    EncodeFarJump(words.first<kFarJumpWords>(), reinterpret_cast<uint64_t>(context));
    return {};
}

}

std::optional<HeaderPadding> HeaderPaddingOf(void const* header)
{
    auto mh = static_cast<mach_header_64 const*>(header);
    if (!mh || mh->magic != MH_MAGIC_64) return {};

    // The lowest section start in __TEXT is the end of the padding.
    auto base = reinterpret_cast<uintptr_t>(mh);
    uintptr_t firstSection = 0;
    auto lc = reinterpret_cast<uint8_t const*>(mh + 1);
    auto end = lc + mh->sizeofcmds;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        auto cmd = reinterpret_cast<load_command const*>(lc);
        if (lc + sizeof(*cmd) > end || cmd->cmdsize < sizeof(*cmd) || lc + cmd->cmdsize > end) return {};
        if (cmd->cmd == LC_SEGMENT_64 && cmd->cmdsize >= sizeof(segment_command_64)) {
            auto seg = reinterpret_cast<segment_command_64 const*>(cmd);
            if (strncmp(seg->segname, SEG_TEXT, sizeof(seg->segname)) == 0 &&
                cmd->cmdsize >= sizeof(*seg) + (uint64_t)seg->nsects * sizeof(section_64)) {
                intptr_t slide = (intptr_t)base - (intptr_t)seg->vmaddr;
                auto sect = reinterpret_cast<section_64 const*>(seg + 1);
                for (uint32_t s = 0; s < seg->nsects; s++) {
                    auto a = (uintptr_t)((intptr_t)sect[s].addr + slide);
                    if (sect[s].size && (!firstSection || a < firstSection)) firstSection = a;
                }
            }
        }
        lc += cmd->cmdsize;
    }
    uintptr_t low = AlignUp(base + sizeof(*mh) + mh->sizeofcmds + kPaddingMargin, kIslandAlign);
    if (!firstSection || firstSection <= low) return {};
    return HeaderPadding{ low, firstSection };
}

bool InBranchRange(uintptr_t from, uintptr_t at, size_t bytes)
{
    // A b/bl reaches [from - 128 MB, from + 128 MB - 4]; the farthest word of
    // the block must be reachable, and from it `from` again.
    auto reach = [](uintptr_t a, uintptr_t b) {
        int64_t d = (int64_t)(b - a);
        return d >= -kIslandBranchRange && d < kIslandBranchRange;
    };
    return bytes && reach(from, at) && reach(from, at + bytes - 4) && reach(at, from) && reach(at + bytes - 4, from);
}

void* ReserveIslandLocked(uintptr_t near, void const* imageHeader, size_t wordCount, IslandBuilder build, void* context,
    std::string& problem, unsigned sources)
{
    problem.clear();
    if (!wordCount) {
        problem = "an empty island was asked for";
        return nullptr;
    }
    Request request{ near, wordCount, build, context };
    std::string lastRefusal;
    uintptr_t block = 0;
    auto take = [&](Area& area) {
        std::string why;
        auto outcome = ReserveFrom(area, request, block, why);
        if (!why.empty()) lastRefusal = why;
        return outcome;
    };

    if ((sources & kIslandFromPadding) && imageHeader) {
        if (Area* padding = PaddingArea(imageHeader)) {
            auto outcome = take(*padding);
            if (outcome == Outcome::Reserved) return reinterpret_cast<void*>(block);
            if (outcome == Outcome::Failed) {
                problem = lastRefusal;
                return nullptr;
            }
        }
    }
    auto allowed = [&](bool jit) { return (sources & (jit ? kIslandFromNearJitPage : kIslandFromNearPage)) != 0; };
    for (Area* page = gNearPages; page; page = page->Next) {
        if (allowed(page->Jit) && take(*page) == Outcome::Reserved) return reinterpret_cast<void*>(block);
    }
    // A new page: r-x if the system allows it, MAP_JIT otherwise.
    for (bool jit : { false, true }) {
        if (!allowed(jit)) continue;
        std::string why;
        Area* fresh = MapNearPage(near, jit, why);
        if (!fresh) {
            lastRefusal = why;
            continue;
        }
        if (take(*fresh) == Outcome::Reserved) return reinterpret_cast<void*>(block);
        break;      // a fresh page that cannot take it: another would not either
    }
    problem = lastRefusal.empty() ? "no room for an island within branch range" : "no island within branch range: " + lastRefusal;
    return nullptr;
}

void const* ImageHeaderOf(void const* address)
{
    Dl_info info{};
    return dladdr(address, &info) ? info.dli_fbase : nullptr;
}

void* ReserveIsland(void const* near, size_t bytes, std::string& problem)
{
    void const* header = ImageHeaderOf(near);
    std::lock_guard lock(CodeLock());
    return ReserveIslandLocked(reinterpret_cast<uintptr_t>(near), header, AlignUp(bytes, 4) / 4, nullptr, nullptr, problem);
}

void* MakeJumpPad(void const* near, void const* target, std::string& problem)
{
    void const* header = ImageHeaderOf(near);
    std::lock_guard lock(CodeLock());
    return ReserveIslandLocked(reinterpret_cast<uintptr_t>(near), header, kFarJumpWords, BuildFarJump, const_cast<void*>(target), problem);
}

void EncodeFarJump(std::span<uint32_t, kFarJumpWords> out, uint64_t target)
{
    out[0] = 0x58000050;    // ldr x16, #8
    out[1] = 0xd61f0200;    // br x16
    out[2] = (uint32_t)target;
    out[3] = (uint32_t)(target >> 32);
}

}
