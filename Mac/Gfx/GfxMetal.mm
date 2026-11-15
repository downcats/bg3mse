// Hooks on Metal's objects, and the features built on them. See GfxMetal.h.
//
// Manual retain/release: a hook passes objects straight through with no
// reference counting of its own.

#import "GfxMetalInternal.h"

#include <Gfx/GfxLog.h>
#include <Gfx/GfxMetal.h>
#include <Gfx/GfxStall.h>
#include <Gfx/GfxJitter.h>
#include <Gfx/GfxTemporalLogic.h>

#include <algorithm>
#include <mach/mach_time.h>
#include <new>
#include <os/lock.h>
#include <pthread.h>

#if __has_feature(objc_arc)
#error "GfxMetal.mm uses manual retain/release"
#endif

namespace bg3se::mac::gfx {

thread_local id tInside;
thread_local bool tOwnEncoding;

namespace {

// --- Feature flags, set once at install --------------------------------------------------------

bool sStallLog, sBindDedup, sClusterGuard, sTemporal;

// --- Replacing methods -----------------------------------------------------------------------

os_unfair_lock sHookLock = OS_UNFAIR_LOCK_INIT;

}

IMP OriginalSlow(HookSlot& slot, Class cls)
{
    // A subclass that inherits a replaced method: remember what its ancestor had.
    os_unfair_lock_lock(&sHookLock);
    unsigned count = slot.count.load(std::memory_order_relaxed);
    for (Class c = cls; c; c = class_getSuperclass(c)) {
        for (unsigned i = 0; i < count; i++) {
            if (slot.cls[i] != c) continue;
            IMP found = slot.imp[i];
            if (count < kSlotClasses) {
                slot.cls[count] = cls;
                slot.imp[count] = found;
                slot.count.store(count + 1, std::memory_order_release);
            }
            os_unfair_lock_unlock(&sHookLock);
            return found;
        }
    }
    os_unfair_lock_unlock(&sHookLock);
    // A replacement runs only for a class it was put on, or one below it.
    LogError("[Gfx] No original method for %s", class_getName(cls));
    abort();
}

std::string HookClass(id object, std::initializer_list<HookSpec> specs)
{
    Class cls = object_getClass(object);
    struct Replace
    {
        HookSlot* slot;
        SEL sel;
        Method method;
        IMP replacement;
    };
    Replace replace[24];
    unsigned count = 0;

    os_unfair_lock_lock(&sHookLock);
    for (auto const& spec : specs) {
        SEL sel = sel_registerName(spec.selector);
        Method method = class_getInstanceMethod(cls, sel);
        if (!method) continue;
        // Already replaced, on this class or on one above it that this one
        // takes the method from.
        if (method_getImplementation(method) == spec.replacement) continue;
        if (spec.slot.count.load(std::memory_order_relaxed) >= kSlotClasses || count == std::size(replace)) {
            os_unfair_lock_unlock(&sHookLock);
            return std::string("too many classes implement ") + spec.selector;
        }
        replace[count++] = { &spec.slot, sel, method, spec.replacement };
    }
    for (unsigned i = 0; i < count; i++) {
        HookSlot& slot = *replace[i].slot;
        unsigned at = slot.count.load(std::memory_order_relaxed);
        slot.cls[at] = cls;
        slot.imp[at] = method_getImplementation(replace[i].method);
        // The original is in place before the replacement can be called.
        slot.count.store(at + 1, std::memory_order_release);
        class_replaceMethod(cls, replace[i].sel, replace[i].replacement, method_getTypeEncoding(replace[i].method));
    }
    os_unfair_lock_unlock(&sHookLock);
    return {};
}

namespace {

// Which classes have had their hooks put on, so a new object is one array scan.
struct SeenClasses
{
    std::atomic<Class> entries[16];
    bool Contains(Class cls)
    {
        for (auto& e : entries) {
            Class v = e.load(std::memory_order_acquire);
            if (v == cls) return true;
            if (!v) return false;
        }
        return true;   // full: stop hooking rather than scan forever
    }
    void Add(Class cls)
    {
        for (auto& e : entries) {
            Class expected = nil;
            if (e.compare_exchange_strong(expected, cls) || expected == cls) return;
        }
    }
};

// A class met after install that cannot be hooked is said once; its objects
// are then used as the game made them.
void HookLater(id object, SeenClasses& seen, std::initializer_list<HookSpec> specs)
{
    Class cls = object_getClass(object);
    if (seen.Contains(cls)) return;
    std::string problem = HookClass(object, specs);
    if (!problem.empty()) LogError("[Gfx] Metal hooks: %s is not hooked (%s)", class_getName(cls), problem.c_str());
    seen.Add(cls);
}

// --- Stall log: no effect on the frame, only timing. ---------------------------------------------

double sTicksPerSecond;
std::atomic<uint64_t> sLastPresent, sLastCommit, sPresents, sCommits;
std::atomic<int> sBuffersInFlight;
std::atomic<unsigned> sStallsLogged;
StallWaiters sStallWaiters;

inline double SecondsSince(uint64_t then, uint64_t now)
{
    return then && now > then ? (double)(now - then) / sTicksPerSecond : 0;
}

bool StallLogAllowed()
{
    // A loading screen can stall hundreds of times; keep the log readable.
    unsigned n = sStallsLogged.fetch_add(1, std::memory_order_relaxed);
    return n < 200 || n % 100 == 0;
}

// The key of the mark MarkOwnWork leaves on a buffer.
char sOwnWorkKey;

void StallNoteCommit(id<MTLCommandBuffer> buffer)
{
    uint64_t committed = mach_absolute_time();
    bool own = objc_getAssociatedObject(buffer, &sOwnWorkKey) != nil;
    sLastCommit.store(committed, std::memory_order_relaxed);
    sCommits.fetch_add(1, std::memory_order_relaxed);
    sBuffersInFlight.fetch_add(1, std::memory_order_relaxed);
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        sBuffersInFlight.fetch_sub(1, std::memory_order_relaxed);
        double total = SecondsSince(committed, mach_absolute_time());
        double gpu = done.GPUEndTime - done.GPUStartTime;
        bool failed = done.status != MTLCommandBufferStatusCompleted;
        switch (ClassifyBufferStall(total, gpu, failed)) {
        case BufferStall::None:
            break;
        case BufferStall::Waited:
            // The watchdog's thread says how many, a moment later.
            sStallWaiters.Add(total - gpu);
            break;
        case BufferStall::Own:
            if (StallLogAllowed()) {
                LogWarn("[Gfx] Stall: command buffer took %.0f ms to finish (GPU ran %.0f ms, so it waited "
                        "%.0f ms), status %ld, %s; %s",
                    total * 1000, gpu * 1000, (total - gpu) * 1000, (long)done.status,
                    done.error ? done.error.localizedDescription.UTF8String : "no error",
                    own ? "it holds bg3mse's MetalFX scaling" : "the game's work only");
            }
            break;
        }
    }];
}

// The buffers that only waited behind other work since the last report.
void StallReportWaiters()
{
    auto waiters = sStallWaiters.Take();
    if (waiters.Count == 0 || !StallLogAllowed()) return;
    LogWarn("[Gfx] Stall: %u command buffer%s waited behind other work on the GPU, up to %u ms",
        waiters.Count, waiters.Count == 1 ? "" : "s", waiters.LongestMs);
}

void StallNotePresent()
{
    uint64_t now = mach_absolute_time();
    uint64_t previous = sLastPresent.exchange(now, std::memory_order_relaxed);
    uint64_t presents = sPresents.fetch_add(1, std::memory_order_relaxed) + 1;
    double gap = SecondsSince(previous, now);
    if (gap >= kStallSeconds && StallLogAllowed()) {
        LogWarn("[Gfx] Stall: %.0f ms between presents at frame %llu, %d buffers in flight", gap * 1000, (unsigned long long)presents,
            sBuffersInFlight.load(std::memory_order_relaxed));
    }
}

// Catches the freeze while it is happening, and shows a hard hang that never gets
// to log anything after it.
void* StallWatchdog(void*)
{
    pthread_setname_np("bg3mse-gfx-stall");
    double nextReport = 2.0;
    for (;;) {
        struct timespec delay = { 0, 500 * 1000 * 1000 };
        nanosleep(&delay, nullptr);
        StallReportWaiters();
        uint64_t now = mach_absolute_time();
        double since = SecondsSince(sLastPresent.load(std::memory_order_relaxed), now);
        if (since < 2.0) {
            nextReport = 2.0;
            continue;
        }
        if (since < nextReport) continue;
        nextReport = since + 10.0;
        LogWarn("[Gfx] Stall in progress: no present for %.1f s, last commit %.1f s ago, %d buffers in "
                "flight, %llu frames so far",
            since, SecondsSince(sLastCommit.load(std::memory_order_relaxed), now), sBuffersInFlight.load(std::memory_order_relaxed),
            (unsigned long long)sPresents.load(std::memory_order_relaxed));
    }
    return nullptr;
}

// --- Encoder records. Few are live at once, and one thread uses its encoder for
// many calls in a row, so a one-entry per-thread cache answers nearly all
// lookups. Any create or end bumps the generation, which voids that cache. ---

constexpr unsigned kEncoderPool = 128;
GfxEncoder* sEncoders;
os_unfair_lock sEncoderLock = OS_UNFAIR_LOCK_INIT;
std::atomic<uint32_t> sEncoderGeneration{ 1 };
unsigned sEncoderEvict;

struct EncoderCache
{
    id encoder;
    GfxEncoder* record;
    uint32_t generation;
};
thread_local EncoderCache tEncoderCache;

GfxEncoder* EncoderFindSlow(id encoder, uint32_t generation)
{
    GfxEncoder* found = nullptr;
    os_unfair_lock_lock(&sEncoderLock);
    for (unsigned i = 0; i < kEncoderPool && !found; i++) {
        if (sEncoders[i].encoder == encoder) found = &sEncoders[i];
    }
    os_unfair_lock_unlock(&sEncoderLock);
    tEncoderCache = { encoder, found, generation };
    return found;
}

GfxEncoder* EncoderBegin(id encoder, id<MTLCommandBuffer> buffer, bool compute, id<MTLTexture> colorTarget)
{
    GfxEncoder* record = nullptr;
    os_unfair_lock_lock(&sEncoderLock);
    // The same object again: Metal reusing the address of an encoder whose end
    // was not seen.
    for (unsigned i = 0; i < kEncoderPool && !record; i++) {
        if (sEncoders[i].encoder == encoder) record = &sEncoders[i];
    }
    for (unsigned i = 0; i < kEncoderPool && !record; i++) {
        if (!sEncoders[i].encoder) record = &sEncoders[i];
    }
    // More encoders open at once than there are records: the oldest loses its
    // record, and is from then on used as the game made it.
    if (!record) record = &sEncoders[sEncoderEvict++ % kEncoderPool];
    TemporalClear(record);
    memset((void*)record, 0, sizeof *record);
    record->encoder = encoder;
    record->buffer = buffer;
    record->compute = compute;
    record->colorTarget = colorTarget;
    os_unfair_lock_unlock(&sEncoderLock);
    sEncoderGeneration.fetch_add(1, std::memory_order_acq_rel);
    return record;
}

void EncoderEnd(GfxEncoder* record)
{
    os_unfair_lock_lock(&sEncoderLock);
    record->encoder = nil;
    os_unfair_lock_unlock(&sEncoderLock);
    sEncoderGeneration.fetch_add(1, std::memory_order_acq_rel);
}

}

GfxEncoder* EncoderFind(id encoder)
{
    uint32_t generation = sEncoderGeneration.load(std::memory_order_acquire);
    EncoderCache& cache = tEncoderCache;
    if (cache.encoder == encoder && cache.generation == generation) return cache.record;
    return EncoderFindSlow(encoder, generation);
}

// --- Pipelines ----------------------------------------------------------------------------------------

namespace {

// An open-addressing table keyed by the pipeline's address. Readers take no
// lock: a table is only ever written under sPipelineLock, and an entry's key
// is stored last. When a table is half full its entries are copied into one
// twice the size, which then takes its place; the one it replaced is never
// freed, since a reader may still be in it (and never written again). All of
// them together are less than twice the size of the newest.
//
// The table holds no reference to a pipeline. A watch object hung on each one
// (BG3MSEGfxPipelineWatch) is deallocated with it, before its memory can be
// given out again, and marks its entry gone: a key no pointer has, which a
// lookup passes over as it would another pipeline's, so a lookup that goes
// past it still comes to the pipelines beyond. A gone entry is made empty
// only where no lookup of a remembered pipeline goes through it, and is
// otherwise taken again by the next pipeline whose way it is on; so the table
// grows with the pipelines alive, not with all there have been. An entry is
// only rewritten once it is gone, and a lookup only reads past the key of its
// own pipeline, which is alive while it is looked up.
constexpr uintptr_t kPipelineGone = 1;

struct PipelineEntry
{
    std::atomic<uintptr_t> key;
    std::atomic<uint32_t> fact;
    char name[48];
};

struct PipelineTable
{
    PipelineEntry* entries;
    size_t mask;
    unsigned shift;         // 64 - log2(entries)
    size_t used;            // entries that are not empty: remembered, or gone
};

constexpr unsigned kFirstPipelineBits = 10;

os_unfair_lock sPipelineLock = OS_UNFAIR_LOCK_INIT;
std::atomic<PipelineTable*> sPipelines{ nullptr };
bool sPipelinesGrowthFailed = false;    // under sPipelineLock; said in the log once

inline size_t PipelineHash(PipelineTable const* table, uintptr_t p)
{
    return (size_t)((p >> 4) * 0x9E3779B97F4A7C15ull >> table->shift) & table->mask;
}

// A table is never more than 7/8 full (half full but when it could not grow),
// so a lookup always comes to an empty entry.
PipelineEntry* PipelineEntryOf(id state)
{
    uintptr_t key = (uintptr_t)state;
    PipelineTable* table = sPipelines.load(std::memory_order_acquire);
    if (!key || !table) return nullptr;
    for (size_t i = PipelineHash(table, key);; i = (i + 1) & table->mask) {
        uintptr_t found = table->entries[i].key.load(std::memory_order_acquire);
        if (!found) return nullptr;
        if (found == key) return &table->entries[i];
    }
}

PipelineTable* NewPipelineTable(unsigned bits)
{
    size_t count = (size_t)1 << bits;
    auto entries = new (std::nothrow) PipelineEntry[count]();
    if (!entries) return nullptr;
    auto table = new (std::nothrow) PipelineTable{ entries, count - 1, 64 - bits, 0 };
    if (!table) delete[] entries;
    return table;
}

// The entry `key` goes in: the first on its way that is empty or gone. In a
// table no one reads yet, or under sPipelineLock.
PipelineEntry& PipelineSlot(PipelineTable* table, uintptr_t key)
{
    size_t i = PipelineHash(table, key);
    for (;; i = (i + 1) & table->mask) {
        uintptr_t found = table->entries[i].key.load(std::memory_order_relaxed);
        if (!found || found == kPipelineGone) return table->entries[i];
    }
}

// Makes empty each gone entry of the run around `at` (the entries between
// two empty ones) that no remembered pipeline's way goes through: a lookup
// starts at the pipeline's hash and goes on to its entry, all within one run.
// A lookup of a remembered pipeline then never meets an entry made empty, and
// one of a pipeline not remembered only stops sooner. Under sPipelineLock.
void SweepPipelines(PipelineTable* table, size_t at)
{
    size_t const mask = table->mask;
    auto keyAt = [&](size_t i) { return table->entries[i & mask].key.load(std::memory_order_relaxed); };
    size_t start = at, end = at;
    while (keyAt(start - 1)) start = (start - 1) & mask;
    while (keyAt(end + 1)) end = (end + 1) & mask;
    // From the end back: the earliest place in the run that the way of a
    // remembered pipeline after this entry starts from.
    size_t const length = ((end - start) & mask) + 1;
    size_t earliest = length;
    for (size_t offset = length; offset-- > 0;) {
        PipelineEntry& entry = table->entries[(start + offset) & mask];
        uintptr_t key = entry.key.load(std::memory_order_relaxed);
        if (key == kPipelineGone && earliest > offset) {
            entry.key.store(0, std::memory_order_release);
            table->used--;
        } else if (key != kPipelineGone) {
            earliest = std::min(earliest, (PipelineHash(table, key) - start) & mask);
        }
    }
}

// A remembered pipeline is being deallocated.
void ForgetPipeline(uintptr_t key)
{
    if (!key) return;
    os_unfair_lock_lock(&sPipelineLock);
    PipelineTable* table = sPipelines.load(std::memory_order_relaxed);
    if (PipelineEntry* entry = PipelineEntryOf((id)key)) {
        entry->key.store(kPipelineGone, std::memory_order_release);
        SweepPipelines(table, (size_t)(entry - table->entries));
    }
    os_unfair_lock_unlock(&sPipelineLock);
}

}

}

@interface BG3MSEGfxPipelineWatch : NSObject {
@public
    uintptr_t key;      // 0 until it is hung on the pipeline of that address
}
@end

@implementation BG3MSEGfxPipelineWatch
// Run inside the pipeline's own deallocation, which lets go of what is
// associated with it before its memory is freed.
- (void)dealloc
{
    bg3se::mac::gfx::ForgetPipeline(key);
    [super dealloc];
}
@end

namespace bg3se::mac::gfx {

namespace {

// The key of the watch on a pipeline.
char sPipelineWatchKey;

}

uint32_t PipelineFact(id state)
{
    PipelineEntry const* entry = PipelineEntryOf(state);
    return entry ? entry->fact.load(std::memory_order_relaxed) : 0;
}

char const* PipelineName(id state)
{
    PipelineEntry const* entry = PipelineEntryOf(state);
    return entry ? entry->name : "";
}

// Called before the pipeline is given to whoever asked for it, so nothing
// looks it up while its entry is being written, and nothing lets go of it
// before it is watched.
void RememberPipeline(id state, uint32_t fact, char const* name)
{
    if (!state || !fact) return;
    uintptr_t key = (uintptr_t)state;
    // Made before the lock is taken: one let go of under it would take the lock again.
    BG3MSEGfxPipelineWatch* watch = [BG3MSEGfxPipelineWatch new];
    bool couldNotGrow = false, remembered = false;
    os_unfair_lock_lock(&sPipelineLock);
    PipelineTable* table = sPipelines.load(std::memory_order_relaxed);
    if (PipelineEntryOf(state)) {
        // Metal gave out an object it had given out before, and it is watched already.
        os_unfair_lock_unlock(&sPipelineLock);
        [watch release];
        return;
    }
    PipelineEntry* entry = table ? &PipelineSlot(table, key) : nullptr;
    bool takesEmpty = !entry || !entry->key.load(std::memory_order_relaxed);
    if (takesEmpty && (!table || (table->used + 1) * 2 > table->mask + 1)) {
        PipelineTable* grown = NewPipelineTable(table ? 64 - table->shift + 1 : kFirstPipelineBits);
        if (grown) {
            for (size_t i = 0; table && i <= table->mask; i++) {
                PipelineEntry const& from = table->entries[i];
                uintptr_t fromKey = from.key.load(std::memory_order_relaxed);
                if (!fromKey || fromKey == kPipelineGone) continue;
                PipelineEntry& to = PipelineSlot(grown, fromKey);
                to.fact.store(from.fact.load(std::memory_order_relaxed), std::memory_order_relaxed);
                memcpy(to.name, from.name, sizeof to.name);
                to.key.store(fromKey, std::memory_order_relaxed);
                grown->used++;
            }
            sPipelines.store(grown, std::memory_order_release);
            table = grown;
            entry = &PipelineSlot(table, key);
        } else if (!sPipelinesGrowthFailed) {
            sPipelinesGrowthFailed = couldNotGrow = true;
        }
    }
    bool room = entry && (!takesEmpty || (table->used + 1) * 8 <= (table->mask + 1) * 7);
    if (room) {
        if (!entry->key.load(std::memory_order_relaxed)) table->used++;
        entry->fact.store(fact, std::memory_order_relaxed);
        strlcpy(entry->name, name ? name : "", sizeof entry->name);
        entry->key.store(key, std::memory_order_release);
        remembered = true;
    }
    os_unfair_lock_unlock(&sPipelineLock);
    if (remembered && watch) {
        watch->key = key;
        objc_setAssociatedObject(state, &sPipelineWatchKey, watch, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    } else if (remembered) {
        [state retain];         // no memory for a watch: kept for good, so its address is never another's
    }
    [watch release];
    if (couldNotGrow) {
        LogWarn("[Gfx] The pipeline table could not grow; it is tried again for each new pipeline, and one it has no room for is not remembered");
    }
}

constexpr char kClusterBufferName[] = "ClusterInfoBuffer";

// The fact for a compute pipeline, from its description; 0 when the guard has
// nothing to do with it.
uint32_t ClusterFact(MTLComputePipelineReflection* reflection)
{
    for (id<MTLBinding> binding in reflection.bindings) {
        if (binding.type != MTLBindingTypeBuffer || ![binding.name isEqualToString:@(kClusterBufferName)]) continue;
        if (binding.access != MTLBindingAccessReadOnly || binding.index >= 32) return 0;
        return kFactClusterSlot | (uint32_t)binding.index;
    }
    return 0;
}

namespace {

// --- Cluster guard (gfx_cluster_guard) --------------------------------------------------------------
// The kernels of BG3's clustered lighting (ClusteredLighting_*, and those of
// reflections and fog) read which lights reach a cluster from a buffer named
// ClusterInfoBuffer in their source. A kernel dispatched with nothing bound
// to that slot reads through no buffer at all, and that GPU fault wedges the
// driver. When such a kernel is about to be dispatched in an encoder that has
// had nothing bound to the slot, a zero-filled stand-in is bound first, so the
// kernel reads zeros; the log says so. A dispatch the game bound a buffer for
// is not touched.
//
// Which kernels, and which slot, is read from each compute pipeline's own
// description when it is made: a buffer the kernel only reads, named
// ClusterInfoBuffer. Nothing is assumed of a kernel that has none.
// The stand-in is 8 MB, enough for the clusters of an 8K render at 16 bytes
// a cluster; it is never written, so it needs no hazard tracking and one
// serves every encoder.

constexpr NSUInteger kClusterStandInBytes = 8u << 20;
std::atomic<uint64_t> sClusterStoodIn;

id<MTLBuffer> ClusterStandIn(id<MTLDevice> device)
{
    static std::atomic<id<MTLBuffer>> sBuffer{ nil };
    id<MTLBuffer> buffer = sBuffer.load(std::memory_order_acquire);
    if (buffer) return buffer.device == device ? buffer : nil;
    // New buffers are zero-filled.
    buffer = [device newBufferWithLength:kClusterStandInBytes options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeUntracked];
    if (!buffer) return nil;
    buffer.label = @"bg3mse cluster stand-in";
    id<MTLBuffer> expected = nil;
    if (!sBuffer.compare_exchange_strong(expected, buffer)) {
        [buffer release];
        return expected.device == device ? expected : nil;
    }
    return buffer;
}


bool BindingsAvailable()
{
    if (@available(macOS 13.0, *)) return true;
    return false;
}

HookSlot sSetBuffer;

// Before a dispatch of `e`'s pipeline.
inline void ClusterGuardBeforeDispatch(id self, GfxEncoder* e)
{
    if (!(e->pipelineFact & kFactClusterSlot)) return;
    uint32_t slot = e->pipelineFact & 31;
    if (e->buffersBound & (1u << slot)) return;
    id<MTLBuffer> standIn = ClusterStandIn(e->buffer.device);
    if (!standIn) return;
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger))Original(sSetBuffer, self))(self, @selector(setBuffer:offset:atIndex:), standIn, 0, slot);
    e->buffersBound |= 1u << slot;
    uint64_t n = sClusterStoodIn.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || n % 10000 == 0) {
        LogWarn("[Gfx] cluster_guard: %s was dispatched with nothing bound to %s (buffer %u); a zeroed stand-in was bound (%llu so far)",
            PipelineName(e->pipeline), kClusterBufferName, slot, (unsigned long long)n);
    }
}

// --- Compute encoder hooks --------------------------------------------------------------------------

HookSlot sSetComputePipeline, sSetBuffers, sSetBytes, sDispatchThreads, sDispatchGroups, sDispatchIndirect;
HookSlot sComputeTexture, sComputeTextures;

void HComputeTexture(id self, SEL cmd, id texture, NSUInteger index)
{
    Entered entered(self);
    ((void (*)(id, SEL, id, NSUInteger))Original(sComputeTexture, self))(self, cmd, texture, index);
    if (entered.nested || index >= kComputeTextures) return;
    if (GfxEncoder* e = EncoderFind(self)) e->computeTextures[index] = texture;
}

void HComputeTextures(id self, SEL cmd, id const* textures, NSRange range)
{
    Entered entered(self);
    ((void (*)(id, SEL, id const*, NSRange))Original(sComputeTextures, self))(self, cmd, textures, range);
    if (entered.nested || range.location >= kComputeTextures) return;
    if (GfxEncoder* e = EncoderFind(self)) {
        for (NSUInteger i = 0; i < range.length && range.location + i < kComputeTextures; i++) e->computeTextures[range.location + i] = textures[i];
    }
}

void HSetComputePipeline(id self, SEL cmd, id state)
{
    Entered entered(self);
    ((void (*)(id, SEL, id))Original(sSetComputePipeline, self))(self, cmd, state);
    if (entered.nested) return;
    if (GfxEncoder* e = EncoderFind(self)) {
        e->pipeline = state;
        e->pipelineFact = PipelineFact(state);
    }
}

void HSetBuffer(id self, SEL cmd, id buffer, NSUInteger offset, NSUInteger index)
{
    Entered entered(self);
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger))Original(sSetBuffer, self))(self, cmd, buffer, offset, index);
    if (entered.nested || index >= 32) return;
    if (GfxEncoder* e = EncoderFind(self)) {
        if (buffer) {
            e->buffersBound |= 1u << index;
        } else {
            e->buffersBound &= ~(1u << index);
        }
    }
}

void HSetBuffers(id self, SEL cmd, id const* buffers, NSUInteger const* offsets, NSRange range)
{
    Entered entered(self);
    ((void (*)(id, SEL, id const*, NSUInteger const*, NSRange))Original(sSetBuffers, self))(self, cmd, buffers, offsets, range);
    if (entered.nested) return;
    if (GfxEncoder* e = EncoderFind(self)) {
        for (NSUInteger i = 0; i < range.length && range.location + i < 32; i++) {
            if (buffers[i]) {
                e->buffersBound |= 1u << (range.location + i);
            } else {
                e->buffersBound &= ~(1u << (range.location + i));
            }
        }
    }
}

void HSetBytes(id self, SEL cmd, void const* bytes, NSUInteger length, NSUInteger index)
{
    Entered entered(self);
    ((void (*)(id, SEL, void const*, NSUInteger, NSUInteger))Original(sSetBytes, self))(self, cmd, bytes, length, index);
    if (entered.nested || index >= 32) return;
    if (GfxEncoder* e = EncoderFind(self)) e->buffersBound |= 1u << index;
}

// Before a dispatch of the game's. True: it must not run (Temporal took FSR or RCAS over).
inline bool BeforeDispatch(id self, bool nested)
{
    if (nested) return false;
    GfxEncoder* e = EncoderFind(self);
    if (!e) return false;
    if (sClusterGuard) ClusterGuardBeforeDispatch(self, e);
    return sTemporal && (e->pipelineFact & kFactKindMask) && TemporalDispatch(e);
}

void HDispatchThreads(id self, SEL cmd, MTLSize grid, MTLSize threads)
{
    Entered entered(self);
    if (BeforeDispatch(self, entered.nested)) return;
    ((void (*)(id, SEL, MTLSize, MTLSize))Original(sDispatchThreads, self))(self, cmd, grid, threads);
}
void HDispatchGroups(id self, SEL cmd, MTLSize groups, MTLSize threads)
{
    Entered entered(self);
    if (BeforeDispatch(self, entered.nested)) return;
    ((void (*)(id, SEL, MTLSize, MTLSize))Original(sDispatchGroups, self))(self, cmd, groups, threads);
}
void HDispatchIndirect(id self, SEL cmd, id indirect, NSUInteger offset, MTLSize threads)
{
    Entered entered(self);
    if (BeforeDispatch(self, entered.nested)) return;
    ((void (*)(id, SEL, id, NSUInteger, MTLSize))Original(sDispatchIndirect, self))(self, cmd, indirect, offset, threads);
}

// --- Bind dedup (gfx_bind_dedup) -------------------------------------------------------------------
// The game binds every draw's textures and pipeline again, though most are the
// ones already bound: about 30,000 texture binds a frame, each a call into the
// driver. A bind of what the slot already holds in this encoder is not passed
// on. Metal keeps what is bound to an encoder until it is bound over or the
// encoder ends, so dropping such a bind changes nothing the GPU sees.
//
// The slot's holder is known by its pointer, which is not retained. An equal
// pointer is the same object as long as the object bound first is still alive
// at the second bind. It is: an encoder of a buffer that retains its references
// holds what is bound to it, and for a buffer that does not (the game's own
// kind) the game must keep everything its commands use alive until the buffer
// has run. The one case that leaves out is a texture bound to a slot no draw
// reads, freed while the encoder is open, and another made at its address and
// bound to that slot of that encoder.

std::atomic<uint64_t> sBindsSeen, sBindsDropped;
constexpr uint64_t kBindReportFirst = 1000000, kBindReportEvery = 100000000;

void BindDedupEnd(GfxEncoder* e)
{
    if (!e->bindsSeen) return;
    uint64_t before = sBindsSeen.fetch_add(e->bindsSeen, std::memory_order_relaxed);
    uint64_t dropped = sBindsDropped.fetch_add(e->bindsDropped, std::memory_order_relaxed) + e->bindsDropped;
    uint64_t seen = before + e->bindsSeen;
    bool first = before < kBindReportFirst && seen >= kBindReportFirst;
    if (first || before / kBindReportEvery != seen / kBindReportEvery) {
        LogInfo("[Gfx] bind_dedup: %llu of %llu texture and pipeline binds were of what was already bound, and were dropped",
            (unsigned long long)dropped, (unsigned long long)seen);
    }
}

// --- Render encoder hooks ---------------------------------------------------------------------------

HookSlot sSetRenderPipeline, sFragTexture, sFragTextures, sEndEncoding;

void HSetRenderPipeline(id self, SEL cmd, id state)
{
    Entered entered(self);
    auto forward = (void (*)(id, SEL, id))Original(sSetRenderPipeline, self);
    GfxEncoder* e = entered.nested ? nullptr : EncoderFind(self);
    if (e && sBindDedup) {
        e->bindsSeen++;
        if (state && e->pipeline == state) {
            e->bindsDropped++;
            return;
        }
    }
    forward(self, cmd, state);
    if (e) {
        e->pipeline = state;
        if (sTemporal) e->pipelineFact = PipelineFact(state);
    }
}

void HFragTexture(id self, SEL cmd, id texture, NSUInteger index)
{
    Entered entered(self);
    auto forward = (void (*)(id, SEL, id, NSUInteger))Original(sFragTexture, self);
    GfxEncoder* e = entered.nested || index >= kEncoderTextures ? nullptr : EncoderFind(self);
    if (e && sBindDedup) {
        e->bindsSeen++;
        if (texture && e->fragment[index] == texture) {
            e->bindsDropped++;
            return;
        }
    }
    forward(self, cmd, texture, index);
    if (e) e->fragment[index] = texture;
}

void HFragTextures(id self, SEL cmd, id const* textures, NSRange range)
{
    Entered entered(self);
    auto forward = (void (*)(id, SEL, id const*, NSRange))Original(sFragTextures, self);
    GfxEncoder* e = entered.nested ? nullptr : EncoderFind(self);
    if (!e || range.location >= kEncoderTextures || range.length > kEncoderTextures - range.location) {
        forward(self, cmd, textures, range);
        return;
    }
    if (!sBindDedup) {
        forward(self, cmd, textures, range);
        for (NSUInteger i = 0; i < range.length; i++) e->fragment[range.location + i] = textures[i];
        return;
    }
    // Pass on each run of slots that change, and nothing of the rest.
    e->bindsSeen += (uint32_t)range.length;
    NSUInteger i = 0;
    while (i < range.length) {
        if (textures[i] && e->fragment[range.location + i] == textures[i]) {
            e->bindsDropped++;
            i++;
            continue;
        }
        NSUInteger start = i;
        while (i < range.length && !(textures[i] && e->fragment[range.location + i] == textures[i])) i++;
        forward(self, cmd, textures + start, NSMakeRange(range.location + start, i - start));
        for (NSUInteger k = start; k < i; k++) e->fragment[range.location + k] = textures[k];
    }
}

void HEndEncoding(id self, SEL cmd)
{
    Entered entered(self);
    GfxEncoder* e = entered.nested ? nullptr : EncoderFind(self);
    TemporalEnding ending{};
    if (e) {
        if (sBindDedup) BindDedupEnd(e);
        // Metal may reuse the encoder's address as soon as it has ended, and
        // what Temporal does next makes encoders of its own: what it needs of
        // the record is taken first, and the record freed.
        if (e->fsr.state != kFsrNone) TemporalTakeEnding(e, &ending);
        EncoderEnd(e);
    }
    ((void (*)(id, SEL))Original(sEndEncoding, self))(self, cmd);
    if (ending.state != kFsrNone) TemporalFinishEnding(&ending);
}

// --- Temporal's view of a render encoder: the constants it reads, and every draw ---------------------

HookSlot sFragBuffer, sFragBuffers, sFragOffset, sFragBytes;
HookSlot sDraw, sDrawInst, sDrawBase, sIDraw, sIDrawInst, sIDrawBase;

inline int ConstantSlot(NSUInteger index)
{
    switch (index) {
    case 8: return kConstTemporal;
    case 9: return kConstViewport;
    case 11: return kConstCamera;
    default: return -1;
    }
}

void HFragBuffer(id self, SEL cmd, id buffer, NSUInteger offset, NSUInteger index)
{
    Entered entered(self);
    ((void (*)(id, SEL, id, NSUInteger, NSUInteger))Original(sFragBuffer, self))(self, cmd, buffer, offset, index);
    int slot = entered.nested ? -1 : ConstantSlot(index);
    if (slot < 0) return;
    if (GfxEncoder* e = EncoderFind(self)) {
        e->constants[slot].buffer = buffer;
        e->constants[slot].offset = offset;
        e->constants[slot].byteCount = 0;
    }
}

void HFragBuffers(id self, SEL cmd, id const* buffers, NSUInteger const* offsets, NSRange range)
{
    Entered entered(self);
    ((void (*)(id, SEL, id const*, NSUInteger const*, NSRange))Original(sFragBuffers, self))(self, cmd, buffers, offsets, range);
    if (entered.nested || range.location > 11 || range.location + range.length <= 8) return;
    GfxEncoder* e = EncoderFind(self);
    if (!e) return;
    for (NSUInteger i = 0; i < range.length; i++) {
        int slot = ConstantSlot(range.location + i);
        if (slot < 0) continue;
        e->constants[slot].buffer = buffers[i];
        e->constants[slot].offset = offsets[i];
        e->constants[slot].byteCount = 0;
    }
}

void HFragOffset(id self, SEL cmd, NSUInteger offset, NSUInteger index)
{
    Entered entered(self);
    ((void (*)(id, SEL, NSUInteger, NSUInteger))Original(sFragOffset, self))(self, cmd, offset, index);
    int slot = entered.nested ? -1 : ConstantSlot(index);
    if (slot < 0) return;
    if (GfxEncoder* e = EncoderFind(self)) e->constants[slot].offset = offset;
}

void HFragBytes(id self, SEL cmd, void const* bytes, NSUInteger length, NSUInteger index)
{
    Entered entered(self);
    ((void (*)(id, SEL, void const*, NSUInteger, NSUInteger))Original(sFragBytes, self))(self, cmd, bytes, length, index);
    int slot = entered.nested ? -1 : ConstantSlot(index);
    if (slot < 0) return;
    if (GfxEncoder* e = EncoderFind(self)) {
        GfxConstant& c = e->constants[slot];
        c.buffer = nil;
        c.byteCount = length < sizeof c.bytes ? (uint32_t)length : (uint32_t)sizeof c.bytes;
        memcpy(c.bytes, bytes, c.byteCount);
    }
}

// The record of an encoder whose bound pipeline Temporal knows, for a draw of
// the game's; null for every other draw, after two reads.
inline GfxEncoder* DrawOfKnownPipeline(id self, bool nested)
{
    if (nested) return nullptr;
    GfxEncoder* e = EncoderFind(self);
    return e && (e->pipelineFact & kFactKindMask) ? e : nullptr;
}

// Each draw: Temporal looks at one with a pipeline it knows, and may have its
// own pipeline bound for it; the game's draw runs either way.
#define GFX_DRAW(slot, ...)                                             \
    Entered entered(self);                                              \
    GfxEncoder* e = DrawOfKnownPipeline(self, entered.nested);          \
    bool own = e && TemporalBeforeDraw(self, e);                        \
    ((decltype(&H##slot))Original(s##slot, self))(self, cmd, __VA_ARGS__); \
    if (own) TemporalAfterDraw(self, e)

void HDraw(id self, SEL cmd, MTLPrimitiveType type, NSUInteger start, NSUInteger count)
{
    GFX_DRAW(Draw, type, start, count);
}
void HDrawInst(id self, SEL cmd, MTLPrimitiveType type, NSUInteger start, NSUInteger count, NSUInteger instances)
{
    GFX_DRAW(DrawInst, type, start, count, instances);
}
void HDrawBase(id self, SEL cmd, MTLPrimitiveType type, NSUInteger start, NSUInteger count, NSUInteger instances, NSUInteger base)
{
    GFX_DRAW(DrawBase, type, start, count, instances, base);
}
void HIDraw(id self, SEL cmd, MTLPrimitiveType type, NSUInteger count, MTLIndexType indexType, id buffer, NSUInteger offset)
{
    GFX_DRAW(IDraw, type, count, indexType, buffer, offset);
}
void HIDrawInst(id self, SEL cmd, MTLPrimitiveType type, NSUInteger count, MTLIndexType indexType, id buffer, NSUInteger offset, NSUInteger instances)
{
    GFX_DRAW(IDrawInst, type, count, indexType, buffer, offset, instances);
}
void HIDrawBase(id self, SEL cmd, MTLPrimitiveType type, NSUInteger count, MTLIndexType indexType, id buffer, NSUInteger offset, NSUInteger instances,
    NSInteger baseVertex, NSUInteger baseInstance)
{
    GFX_DRAW(IDrawBase, type, count, indexType, buffer, offset, instances, baseVertex, baseInstance);
}
#undef GFX_DRAW

SeenClasses sSeenRenderEncoders, sSeenComputeEncoders;

void EncoderCreated(id encoder, id<MTLCommandBuffer> buffer, bool compute, id<MTLTexture> colorTarget)
{
    // An encoder of the module's own passes, or of MetalFX's, is nobody's to follow.
    if (!encoder || tOwnEncoding) return;
    if (compute) {
        HookLater(encoder, sSeenComputeEncoders, {
            { sSetComputePipeline, "setComputePipelineState:", (IMP)HSetComputePipeline },
            { sSetBuffer, "setBuffer:offset:atIndex:", (IMP)HSetBuffer },
            { sSetBuffers, "setBuffers:offsets:withRange:", (IMP)HSetBuffers },
            { sSetBytes, "setBytes:length:atIndex:", (IMP)HSetBytes },
            { sComputeTexture, "setTexture:atIndex:", (IMP)HComputeTexture },
            { sComputeTextures, "setTextures:withRange:", (IMP)HComputeTextures },
            { sDispatchThreads, "dispatchThreads:threadsPerThreadgroup:", (IMP)HDispatchThreads },
            { sDispatchGroups, "dispatchThreadgroups:threadsPerThreadgroup:", (IMP)HDispatchGroups },
            { sDispatchIndirect, "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:", (IMP)HDispatchIndirect },
            { sEndEncoding, "endEncoding", (IMP)HEndEncoding },
        });
        Method end = class_getInstanceMethod(object_getClass(encoder), @selector(endEncoding));
        if (end && method_getImplementation(end) == (IMP)HEndEncoding) EncoderBegin(encoder, buffer, true, nil);
    } else if (sTemporal) {
        HookLater(encoder, sSeenRenderEncoders, {
            { sSetRenderPipeline, "setRenderPipelineState:", (IMP)HSetRenderPipeline },
            { sFragTexture, "setFragmentTexture:atIndex:", (IMP)HFragTexture },
            { sFragTextures, "setFragmentTextures:withRange:", (IMP)HFragTextures },
            { sFragBuffer, "setFragmentBuffer:offset:atIndex:", (IMP)HFragBuffer },
            { sFragBuffers, "setFragmentBuffers:offsets:withRange:", (IMP)HFragBuffers },
            { sFragOffset, "setFragmentBufferOffset:atIndex:", (IMP)HFragOffset },
            { sFragBytes, "setFragmentBytes:length:atIndex:", (IMP)HFragBytes },
            { sDraw, "drawPrimitives:vertexStart:vertexCount:", (IMP)HDraw },
            { sDrawInst, "drawPrimitives:vertexStart:vertexCount:instanceCount:", (IMP)HDrawInst },
            { sDrawBase, "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:", (IMP)HDrawBase },
            { sIDraw, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:", (IMP)HIDraw },
            { sIDrawInst, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:", (IMP)HIDrawInst },
            { sIDrawBase, "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",
                (IMP)HIDrawBase },
            { sEndEncoding, "endEncoding", (IMP)HEndEncoding },
        });
        Method end = class_getInstanceMethod(object_getClass(encoder), @selector(endEncoding));
        if (end && method_getImplementation(end) == (IMP)HEndEncoding) EncoderBegin(encoder, buffer, false, colorTarget);
    } else {
        HookLater(encoder, sSeenRenderEncoders, {
            { sSetRenderPipeline, "setRenderPipelineState:", (IMP)HSetRenderPipeline },
            { sFragTexture, "setFragmentTexture:atIndex:", (IMP)HFragTexture },
            { sFragTextures, "setFragmentTextures:withRange:", (IMP)HFragTextures },
            { sEndEncoding, "endEncoding", (IMP)HEndEncoding },
        });
        // Only an encoder whose end will be seen gets a record.
        Method end = class_getInstanceMethod(object_getClass(encoder), @selector(endEncoding));
        if (end && method_getImplementation(end) == (IMP)HEndEncoding) EncoderBegin(encoder, buffer, false, colorTarget);
    }
}

// --- Command buffer and queue hooks ----------------------------------------------------------------

HookSlot sRenderEncoder, sComputeEncoder, sComputeEncoderType;

id HRenderEncoder(id self, SEL cmd, MTLRenderPassDescriptor* pass)
{
    Entered entered(self);
    id encoder = ((id (*)(id, SEL, id))Original(sRenderEncoder, self))(self, cmd, pass);
    if (!entered.nested && encoder) EncoderCreated(encoder, self, false, sTemporal ? pass.colorAttachments[0].texture : nil);
    return encoder;
}
id HComputeEncoder(id self, SEL cmd)
{
    Entered entered(self);
    id encoder = ((id (*)(id, SEL))Original(sComputeEncoder, self))(self, cmd);
    if (!entered.nested) EncoderCreated(encoder, self, true, nil);
    return encoder;
}
id HComputeEncoderType(id self, SEL cmd, MTLDispatchType type)
{
    Entered entered(self);
    id encoder = ((id (*)(id, SEL, MTLDispatchType))Original(sComputeEncoderType, self))(self, cmd, type);
    if (!entered.nested) EncoderCreated(encoder, self, true, nil);
    return encoder;
}

HookSlot sCommit, sPresent, sPresentAfter, sPresentAt;

void HCommit(id self, SEL cmd)
{
    Entered entered(self);
    if (!entered.nested && sStallLog) StallNoteCommit(self);
    ((void (*)(id, SEL))Original(sCommit, self))(self, cmd);
}
void HPresent(id self, SEL cmd, id drawable)
{
    Entered entered(self);
    if (!entered.nested && sStallLog) StallNotePresent();
    ((void (*)(id, SEL, id))Original(sPresent, self))(self, cmd, drawable);
}
void HPresentAfter(id self, SEL cmd, id drawable, CFTimeInterval duration)
{
    Entered entered(self);
    if (!entered.nested && sStallLog) StallNotePresent();
    ((void (*)(id, SEL, id, CFTimeInterval))Original(sPresentAfter, self))(self, cmd, drawable, duration);
}
void HPresentAt(id self, SEL cmd, id drawable, CFTimeInterval time)
{
    Entered entered(self);
    if (!entered.nested && sStallLog) StallNotePresent();
    ((void (*)(id, SEL, id, CFTimeInterval))Original(sPresentAt, self))(self, cmd, drawable, time);
}

SeenClasses sSeenBuffersRender, sSeenBuffersCompute, sSeenBuffersStall;

// Only what a feature that is on needs is replaced: the stall log alone
// follows no encoder, and the cluster guard alone no render encoder.
void BufferCreated(id buffer)
{
    if (!buffer) return;
    if (sBindDedup || sTemporal) {
        HookLater(buffer, sSeenBuffersRender, {
            { sRenderEncoder, "renderCommandEncoderWithDescriptor:", (IMP)HRenderEncoder },
        });
    }
    if (sClusterGuard || sTemporal) {
        HookLater(buffer, sSeenBuffersCompute, {
            { sComputeEncoder, "computeCommandEncoder", (IMP)HComputeEncoder },
            { sComputeEncoderType, "computeCommandEncoderWithDispatchType:", (IMP)HComputeEncoderType },
        });
    }
    if (sStallLog) {
        HookLater(buffer, sSeenBuffersStall, {
            { sCommit, "commit", (IMP)HCommit },
            { sPresent, "presentDrawable:", (IMP)HPresent },
            { sPresentAfter, "presentDrawable:afterMinimumDuration:", (IMP)HPresentAfter },
            { sPresentAt, "presentDrawable:atTime:", (IMP)HPresentAt },
        });
    }
}

HookSlot sQueueBuffer, sQueueBufferUnretained;

id HQueueBuffer(id self, SEL cmd)
{
    Entered entered(self);
    id buffer = ((id (*)(id, SEL))Original(sQueueBuffer, self))(self, cmd);
    if (!entered.nested) BufferCreated(buffer);
    return buffer;
}

id HQueueBufferUnretained(id self, SEL cmd)
{
    Entered entered(self);
    id buffer = ((id (*)(id, SEL))Original(sQueueBufferUnretained, self))(self, cmd);
    if (!entered.nested) BufferCreated(buffer);
    return buffer;
}

SeenClasses sSeenQueues;

void QueueCreated(id queue)
{
    if (!queue) return;
    HookLater(queue, sSeenQueues, {
        { sQueueBuffer, "commandBuffer", (IMP)HQueueBuffer },
        { sQueueBufferUnretained, "commandBufferWithUnretainedReferences", (IMP)HQueueBufferUnretained },
    });
}

// --- Device hooks. new* methods return +1, and these hand that straight back. ----------------------

HookSlot sNewQueue, sNewQueueLimit, sNewComputePipeline, sNewRenderPipeline;

id HNewQueue(id self, SEL cmd)
{
    Entered entered(self);
    id queue = ((id (*)(id, SEL))Original(sNewQueue, self))(self, cmd);
    if (!entered.nested) QueueCreated(queue);
    return queue;
}
id HNewQueueLimit(id self, SEL cmd, NSUInteger count)
{
    Entered entered(self);
    id queue = ((id (*)(id, SEL, NSUInteger))Original(sNewQueueLimit, self))(self, cmd, count);
    if (!entered.nested) QueueCreated(queue);
    return queue;
}

// The pipeline is made as the game asked, with its description asked for
// besides: what it is made of is the same either way. Should Metal refuse
// that, it is made exactly as the game asked and nothing is known of it.
id HNewComputePipeline(id self, SEL cmd, id<MTLFunction> function, NSError** error)
{
    Entered entered(self);
    auto original = (id (*)(id, SEL, id, NSError**))Original(sNewComputePipeline, self);
    if (entered.nested || tOwnEncoding || !(sClusterGuard || sTemporal) || !function) return original(self, cmd, function, error);
    if (@available(macOS 13.0, *)) {
        MTLComputePipelineReflection* reflection = nil;
        id state = [(id<MTLDevice>)self newComputePipelineStateWithFunction:function options:MTLPipelineOptionBindingInfo reflection:&reflection error:nil];
        if (state) {
            if (reflection) {
                uint32_t fact = (sClusterGuard ? ClusterFact(reflection) : 0) | (sTemporal ? TemporalComputeFact(reflection, function) : 0);
                RememberPipeline(state, fact, function.name.UTF8String);
            }
            return state;
        }
    }
    return original(self, cmd, function, error);
}

// A render pipeline whose fragment function has one of the names Temporal
// knows is made the same way, with its description asked for besides.
id HNewRenderPipeline(id self, SEL cmd, MTLRenderPipelineDescriptor* descriptor, NSError** error)
{
    Entered entered(self);
    auto original = (id (*)(id, SEL, id, NSError**))Original(sNewRenderPipeline, self);
    if (entered.nested || tOwnEncoding || !sTemporal || !descriptor ||
        ClassifyFragment(descriptor.fragmentFunction.name.UTF8String) == PipelineKind::None) {
        return original(self, cmd, descriptor, error);
    }
    if (@available(macOS 13.0, *)) {
        MTLRenderPipelineReflection* reflection = nil;
        id state = [(id<MTLDevice>)self newRenderPipelineStateWithDescriptor:descriptor
                                                                     options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
                                                                  reflection:&reflection
                                                                       error:nil];
        if (state) {
            if (reflection) {
                RememberPipeline(state, TemporalRenderFact(self, descriptor, reflection, state), descriptor.fragmentFunction.name.UTF8String);
            }
            return state;
        }
    }
    return original(self, cmd, descriptor, error);
}

std::string HookDevice(id device)
{
    return HookClass(device, {
        { sNewQueue, "newCommandQueue", (IMP)HNewQueue },
        { sNewQueueLimit, "newCommandQueueWithMaxCommandBufferCount:", (IMP)HNewQueueLimit },
        { sNewComputePipeline, "newComputePipelineStateWithFunction:error:", (IMP)HNewComputePipeline },
        { sNewRenderPipeline, "newRenderPipelineStateWithDescriptor:error:", (IMP)HNewRenderPipeline },
    });
}

}

StallCounts StallLogCounts()
{
    return { sPresents.load(), sCommits.load(), sBuffersInFlight.load() };
}

BindCounts BindDedupCounts()
{
    return { sBindsSeen.load(), sBindsDropped.load() };
}

void RawSetRenderPipeline(id encoder, id state)
{
    ((void (*)(id, SEL, id))Original(sSetRenderPipeline, encoder))(encoder, @selector(setRenderPipelineState:), state);
}

void MarkOwnWork(id buffer)
{
    // Read when the buffer is committed, which is after this.
    if (sStallLog) objc_setAssociatedObject(buffer, &sOwnWorkKey, (id)kCFBooleanTrue, OBJC_ASSOCIATION_ASSIGN);
}

unsigned long long ClusterGuardCount()
{
    return sClusterStoodIn.load();
}

int ClusterGuardSlot(void* computePipeline)
{
    uint32_t fact = PipelineFact((id)computePipeline);
    return fact & kFactClusterSlot ? (int)(fact & 31) : -1;
}

MetalOutcome InstallMetalHooks(MetalChoices const& choices)
{
    static bool installed = false;
    MetalOutcome outcome;
    if (installed) {
        outcome.All = "the Metal hooks are already installed";
        return outcome;
    }
    // Which kernels to guard is read from each pipeline's description.
    bool clusterGuard = choices.ClusterGuard && BindingsAvailable();
    if (choices.ClusterGuard && !clusterGuard) {
        outcome.ClusterGuard = "it reads each compute pipeline's description the way macOS 13 or later gives it";
    }
    // MetalFX, on the device the game will render with.
    bool temporal = choices.Temporal;
    if (temporal) {
        @autoreleasepool {
            id<MTLDevice> device = [MTLCreateSystemDefaultDevice() autorelease];
            outcome.Temporal = TemporalUnavailable(device);
            temporal = outcome.Temporal.empty();
        }
    }
    if (!choices.StallLog && !choices.BindDedup && !clusterGuard && !temporal) return outcome;

    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    sTicksPerSecond = 1e9 * (double)timebase.denom / (double)timebase.numer;

    if (!sEncoders) sEncoders = (GfxEncoder*)calloc(kEncoderPool, sizeof(GfxEncoder));
    if (!sEncoders) {
        outcome.All = "no memory for the encoder records";
        return outcome;
    }
    // The flags are set before the first replacement can run, and only when
    // the devices are hooked do they stay set.
    sStallLog = choices.StallLog;
    sBindDedup = choices.BindDedup;
    sClusterGuard = clusterGuard;
    sTemporal = temporal;

    bool any = false;
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device) {
            any = true;
            outcome.All = HookDevice(device);
        }
        [device release];
        NSArray<id<MTLDevice>>* all = MTLCopyAllDevices();
        for (id<MTLDevice> other in all) {
            if (!outcome.All.empty()) break;
            any = true;
            outcome.All = HookDevice(other);
        }
        [all release];
    }
    if (!any) outcome.All = "no Metal device";
    if (!outcome.All.empty()) {
        // What was hooked passes everything on from here.
        sStallLog = sBindDedup = sClusterGuard = sTemporal = false;
        return outcome;
    }
    installed = true;
    // No pipeline of the game's exists yet: nothing is missed by starting now.
    if (sTemporal) TemporalStart(choices.Sharpen);

    if (sStallLog) {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, StallWatchdog, nullptr) == 0) pthread_detach(thread);
    }
    return outcome;
}

}
