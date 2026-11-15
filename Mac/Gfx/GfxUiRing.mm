// Noesis geometry rings. See GfxUiRing.h.
//
// ns::Renderer keeps, per ring, a pointer to BG3's rf buffer wrapper, the ring
// offset and the offset of the current map; the wrapper holds a mapped flag,
// the mapped range (u64 offset, u64 length) and the id<MTLBuffer>. All of these
// offsets are read from MapVertices / MapIndices (UiRingSites). DrawBatch binds
// the wrapper's buffer at draw time and keys its bind cache on that pointer, so
// a swapped buffer is picked up by the next draw; Unmap* flush the wrapper's
// range of whatever buffer it holds. All four are checked by shape.

#import <Metal/Metal.h>

#include <Gfx/GfxLog.h>
#include <Gfx/GfxSites.h>
#include <Gfx/GfxUiRing.h>
#include <Platform/EntryHook.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mach/mach_time.h>

#if __has_feature(objc_arc)
#error "GfxUiRing.mm uses manual retain/release"
#endif

namespace bg3se::mac::gfx {

namespace {

using MapFn = void* (*)(void* self, uint32_t bytes);

struct Ring
{
    char const* name;
    uint32_t wrapperField{};  // ns::Renderer field holding the rf buffer
    uint32_t offsetField{};   // ring offset
    uint32_t startField{};    // offset of the current map
    uint32_t flagOff{};       // in the wrapper: mapped flag
    uint32_t rangeOff{};      // in the wrapper: u64 offset, u64 length
    uint32_t bufferOff{};     // in the wrapper: id<MTLBuffer>
    uint32_t origSize{};
    uint32_t size{};          // ring size in use
    MapFn orig{};
    id<MTLBuffer> big{};      // replacement, retained for the process lifetime
    id<MTLBuffer> old{};      // the game's buffer, kept alive
    bool failed{};
    // stats since the last report
    std::atomic<uint32_t> maps{}, wraps{}, maxMap{};
    std::atomic<uint64_t> bytes{}, minWrapGap{};
    uint64_t lastWrap{};
};

Ring sV{ "vertices" };
Ring sI{ "indices" };
bool sObserve;
mach_timebase_info_data_t sTb;
std::atomic<uint64_t> sNextReport;

uint64_t ToNs(uint64_t ticks) { return ticks * sTb.numer / sTb.denom; }

// Replace the wrapper's MTLBuffer by a larger one with the same options.
bool Grow(Ring& r, char* wrapper, uint32_t size)
{
    if (r.big) return true;
    if (r.failed) return false;
    id<MTLBuffer> old = *(id<MTLBuffer> __unsafe_unretained*)(wrapper + r.bufferOff);
    if (!old) {
        r.failed = true;
        return false;
    }
    MTLResourceOptions opts = (MTLResourceOptions)old.cpuCacheMode | ((MTLResourceOptions)old.storageMode << MTLResourceStorageModeShift);
    opts |= (MTLResourceOptions)old.hazardTrackingMode << MTLResourceHazardTrackingModeShift;
    id<MTLBuffer> big = [old.device newBufferWithLength:size options:opts];
    if (!big) {
        LogWarn("[Gfx] ui_ring_fix: could not allocate a %u MB buffer for the UI %s", size >> 20, r.name);
        r.failed = true;
        return false;
    }
    big.label = [NSString stringWithFormat:@"bg3mse Noesis %s ring", r.name];
    // Draws already recorded against the old buffer bind the wrapper's buffer
    // when they execute, so carry the old contents over.
    if (old.contents && big.contents) memcpy(big.contents, old.contents, r.origSize);
    r.old = [old retain];
    r.big = big;   // +1 from alloc; the wrapper's reference
    *(void**)(wrapper + r.bufferOff) = (void*)big;
    r.size = size;
    LogInfo("[Gfx] ui_ring_fix: UI %s ring %u MB -> %u MB (storage mode %lu)", r.name, r.origSize >> 20, size >> 20,
        (unsigned long)old.storageMode);
    return true;
}

void Note(Ring& r, uint32_t bytes, bool wrapped, uint64_t now)
{
    r.maps.fetch_add(1, std::memory_order_relaxed);
    r.bytes.fetch_add(bytes, std::memory_order_relaxed);
    uint32_t seen = r.maxMap.load(std::memory_order_relaxed);
    while (bytes > seen && !r.maxMap.compare_exchange_weak(seen, bytes)) {}
    if (wrapped) {
        r.wraps.fetch_add(1, std::memory_order_relaxed);
        if (r.lastWrap) {
            uint64_t gap = now - r.lastWrap;
            uint64_t best = r.minWrapGap.load(std::memory_order_relaxed);
            while ((best == 0 || gap < best) && !r.minWrapGap.compare_exchange_weak(best, gap)) {}
        }
        r.lastWrap = now;
    }
}

void ReportRing(Ring& r, char const* mode)
{
    uint32_t maps = r.maps.exchange(0), wraps = r.wraps.exchange(0), maxMap = r.maxMap.exchange(0);
    uint64_t bytes = r.bytes.exchange(0), gap = r.minWrapGap.exchange(0);
    if (!maps) return;
    char closest[32] = "";
    if (wraps > 1) snprintf(closest, sizeof closest, " (closest %.0f ms apart)", ToNs(gap) / 1e6);
    LogInfo("[Gfx] UI %s (%s): %u maps, largest %u KB, %llu KB total, %u wraps of the %u MB ring%s", r.name, mode, maps, maxMap >> 10,
        (unsigned long long)(bytes >> 10), wraps, r.size >> 20, closest);
}

void Report(uint64_t now)
{
    uint64_t next = sNextReport.load(std::memory_order_relaxed);
    if (now < next) return;
    uint64_t later = now + 5000000000ULL * sTb.denom / sTb.numer;
    if (!sNextReport.compare_exchange_strong(next, later)) return;
    // Only worth a line when the game's ring was under real pressure.
    bool busy = sV.maxMap.load() > (256u << 10) || sI.maxMap.load() > (128u << 10) || sV.wraps.load() > 1 || sI.wraps.load() > 1;
    if (!busy) {
        sV.maps = 0; sV.wraps = 0; sV.maxMap = 0; sV.bytes = 0; sV.minWrapGap = 0;
        sI.maps = 0; sI.wraps = 0; sI.maxMap = 0; sI.bytes = 0; sI.minWrapGap = 0;
        return;
    }
    char const* mode = sObserve ? "observe" : "fixed";
    ReportRing(sV, mode);
    ReportRing(sI, mode);
}

void* Map(Ring& r, void* self, uint32_t bytes)
{
    char* me = (char*)self;
    char* wrapper = *(char**)(me + r.wrapperField);
    uint64_t now = mach_absolute_time();
    if (sObserve || !wrapper || !Grow(r, wrapper, r.size == r.origSize ? (r.origSize << 4) : r.size)) {
        uint32_t off = *(uint32_t*)(me + r.offsetField);
        Note(r, bytes, off + bytes > r.origSize, now);
        Report(now);
        return r.orig(self, bytes);
    }
    // ns::Renderer::MapVertices / MapIndices over the larger ring.
    uint32_t off = *(uint32_t*)(me + r.offsetField);
    bool wrapped = off + bytes > r.size;
    if (wrapped) {
        off = 0;
        *(uint32_t*)(me + r.offsetField) = 0;
    }
    char* base = (char*)[r.big contents];
    void* result = base + off;
    wrapper[r.flagOff] = 1;
    *(uint64_t*)(wrapper + r.rangeOff) = off;
    *(uint64_t*)(wrapper + r.rangeOff + 8) = bytes;
    memset(result, 0, bytes);
    *(uint32_t*)(me + r.offsetField) = off + bytes;
    *(uint32_t*)(me + r.startField) = off;
    Note(r, bytes, wrapped, now);
    Report(now);
    return result;
}

void* HookMapVertices(void* self, uint32_t bytes) { return Map(sV, self, bytes); }
void* HookMapIndices(void* self, uint32_t bytes) { return Map(sI, self, bytes); }

void Configure(Ring& r, UiRingSites::Ring const& s)
{
    r.wrapperField = s.Wrapper;
    r.offsetField = s.Offset;
    r.startField = s.MapStart;
    r.flagOff = s.Mapped;
    r.rangeOff = s.Range;
    r.bufferOff = s.Buffer;
    r.origSize = s.Size;
    r.size = s.Size;
}

}

std::string InstallUiRing(GameImage const& image, UiRingSites const& sites, bool observe)
{
    if (!sites.Ok()) return sites.Problem;
    mach_timebase_info(&sTb);
    sObserve = observe;
    Configure(sV, sites.Vertices);
    Configure(sI, sites.Indices);
    // Both prepared, then committed together; each original is set before its patch goes live.
    std::string problem;
    EntryHook* hooks[2] = {
        PrepareEntryHook(image.LoadedAt(sites.Vertices.Map), sites.Vertices.MapBytes, (void const*)HookMapVertices,
            reinterpret_cast<void**>(&sV.orig), problem),
        nullptr,
    };
    if (!hooks[0]) return "could not hook ns::Renderer::MapVertices: " + problem;
    hooks[1] = PrepareEntryHook(image.LoadedAt(sites.Indices.Map), sites.Indices.MapBytes, (void const*)HookMapIndices,
        reinterpret_cast<void**>(&sI.orig), problem);
    if (!hooks[1]) return "could not hook ns::Renderer::MapIndices: " + problem;
    if (problem = CommitEntryHooks(hooks); !problem.empty()) {
        for (EntryHook* h : hooks) RemoveEntryHook(h);
        return "could not hook ns::Renderer's MapVertices and MapIndices: " + problem;
    }
    if (observe) LogInfo("[Gfx] ui_ring_fix: observing only (gfx_ui_ring_observe = 1)");
    return {};
}

}
