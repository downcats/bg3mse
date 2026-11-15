// Tests for the Metal hooks (Gfx/GfxMetal.mm) on Metal's own objects: this
// machine's device, with queues, command buffers, encoders and pipelines made
// here the way the game makes its own. No game needed.
//
// The hooks go on once in a process, so the tests run in one order: first that
// asking for nothing touches nothing, then everything on for the rest.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include <Gfx/GfxJitter.h>
#include <Gfx/GfxMetal.h>
#include <Gfx/GfxMetalCalls.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <span>
#include <string>
#include <thread>
#include <vector>

#if __has_feature(objc_arc)
#error "GfxMetalTests.mm uses manual retain/release"
#endif

using namespace bg3se::mac::gfx;

// Hung on an object, so a test can tell when the object is deallocated.
@interface BG3MSEGfxTestDeallocWatch : NSObject {
@public
    std::atomic<bool>* gone;
}
@end

@implementation BG3MSEGfxTestDeallocWatch
- (void)dealloc
{
    gone->store(true, std::memory_order_release);
    [super dealloc];
}
@end

namespace {

char gDeallocWatchKey;

void WatchDealloc(id object, std::atomic<bool>* gone)
{
    BG3MSEGfxTestDeallocWatch* watch = [BG3MSEGfxTestDeallocWatch new];
    watch->gone = gone;
    objc_setAssociatedObject(object, &gDeallocWatchKey, watch, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [watch release];
}

bool GoneSoon(std::atomic<bool> const& gone)
{
    for (int i = 0; i < 200 && !gone.load(std::memory_order_acquire); i++) usleep(1000);
    return gone.load(std::memory_order_acquire);
}

int gFailures = 0;
int gTests = 0;
char const* gTest = "";

#define CHECK(cond) do { if (!(cond)) { printf("FAIL gfx metal %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; gTests++; name(); } static void name()

id<MTLDevice> gDevice;

// The methods of `object`'s class for `selectors`, as they are now.
std::vector<IMP> Methods(id object, std::span<char const* const> selectors)
{
    std::vector<IMP> out;
    for (char const* selector : selectors) {
        Method method = class_getInstanceMethod(object_getClass(object), sel_registerName(selector));
        out.push_back(method ? method_getImplementation(method) : nullptr);
    }
    return out;
}

// One object of each class the hooks go on, and every hooked method of each.
struct MetalMethods
{
    std::vector<IMP> Device, Queue, Buffer, Render, Compute;
    bool operator == (MetalMethods const&) const = default;
};

MTLRenderPassDescriptor* PassTo(id<MTLTexture> target)
{
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    return pass;
}

id<MTLTexture> NewTexture(MTLPixelFormat format, NSUInteger width, NSUInteger height, MTLTextureUsage usage)
{
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
    desc.usage = usage;
    desc.storageMode = MTLStorageModePrivate;
    return [gDevice newTextureWithDescriptor:desc];
}

MetalMethods AllMethods()
{
    MetalMethods m;
    @autoreleasepool {
        id<MTLCommandQueue> queue = [gDevice newCommandQueueWithMaxCommandBufferCount:8];
        id<MTLCommandBuffer> buffer = [queue commandBufferWithUnretainedReferences];
        id<MTLTexture> target = NewTexture(MTLPixelFormatRGBA8Unorm, 16, 16, MTLTextureUsageRenderTarget);
        id<MTLRenderCommandEncoder> render = [buffer renderCommandEncoderWithDescriptor:PassTo(target)];
        m.Render = Methods(render, HookedRenderEncoderSelectors());
        [render endEncoding];
        id<MTLComputeCommandEncoder> compute = [buffer computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
        m.Compute = Methods(compute, HookedComputeEncoderSelectors());
        [compute endEncoding];
        m.Device = Methods(gDevice, HookedDeviceSelectors());
        m.Queue = Methods(queue, HookedQueueSelectors());
        m.Buffer = Methods(buffer, HookedBufferSelectors());
        [buffer commit];
        [buffer waitUntilCompleted];
        [target release];
        [queue release];
    }
    return m;
}

MetalMethods gBefore;
bool gTemporal = false;

TEST(nothing_asked_changes_no_method)
{
    gBefore = AllMethods();
    // Every hooked selector is a method of the objects of this machine, or the
    // tests below would pass by testing nothing. (A method only newer systems
    // have may be missing; none of the ones the game calls is.)
    for (IMP imp : gBefore.Queue) CHECK(imp != nullptr);
    CHECK(gBefore.Device[1] != nullptr && gBefore.Buffer[0] != nullptr && gBefore.Buffer[3] != nullptr);

    auto none = InstallMetalHooks(MetalChoices{});
    CHECK(none.All.empty() && none.ClusterGuard.empty());
    CHECK(AllMethods() == gBefore);
    // ... and it may still be asked for something after.
}

TEST(everything_asked_goes_on_once)
{
    MetalChoices choices;
    choices.StallLog = true;
    choices.BindDedup = true;
    choices.ClusterGuard = true;
    choices.Temporal = true;
    auto outcome = InstallMetalHooks(choices);
    CHECK(outcome.All.empty());
    gTemporal = outcome.Temporal.empty();
    if (!gTemporal) printf("gfx metal: MetalFX cannot be used here (%s): its tests are skipped\n", outcome.Temporal.c_str());
    CHECK(TemporalStats().On == gTemporal);
    if (@available(macOS 13.0, *)) {
        CHECK(outcome.ClusterGuard.empty());
    } else {
        CHECK(outcome.ClusterGuard.find("macOS 13") != std::string::npos);
    }
    CHECK(InstallMetalHooks(choices).All.find("already") != std::string::npos);

    // With everything on, every method the hooks are said to replace
    // (GfxMetalCalls.h) is replaced, on the classes of this machine's objects.
    auto after = AllMethods();      // makes one object of each class: all are hooked by now
    auto allReplaced = [](std::vector<IMP> const& was, std::vector<IMP> const& is) {
        bool all = was.size() == is.size();
        for (size_t i = 0; all && i < was.size(); i++) all = !was[i] || was[i] != is[i];
        return all;
    };
    CHECK(allReplaced(gBefore.Device, after.Device));
    CHECK(allReplaced(gBefore.Queue, after.Queue));
    CHECK(allReplaced(gBefore.Buffer, after.Buffer));
    if (gTemporal) {
        CHECK(allReplaced(gBefore.Render, after.Render));
        CHECK(allReplaced(gBefore.Compute, after.Compute));
    }
}

TEST(stall_log_counts_commits_and_passes_them_on)
{

    auto before = StallLogCounts();
    @autoreleasepool {
        id<MTLCommandQueue> queue = [gDevice newCommandQueueWithMaxCommandBufferCount:8];
        for (int i = 0; i < 3; i++) {
            id<MTLCommandBuffer> buffer = i == 1 ? [queue commandBuffer] : [queue commandBufferWithUnretainedReferences];
            CHECK(buffer != nil);
            [buffer commit];
            [buffer waitUntilCompleted];
            CHECK(buffer.status == MTLCommandBufferStatusCompleted);    // the commit went through to Metal
        }
        [queue release];
    }
    // Completion handlers run on Metal's own thread, just after the wait returns.
    for (int i = 0; i < 200 && StallLogCounts().BuffersInFlight != 0; i++) usleep(5000);
    auto counts = StallLogCounts();
    CHECK(counts.Commits == before.Commits + 3);
    CHECK(counts.BuffersInFlight == 0);
    CHECK(counts.Presents == before.Presents);
}

// --- drawing, to see what reached the GPU ---------------------------------------------------------

// A triangle over the whole target, whose colour is the sum of the first
// texels of the textures at fragment slots 0, 1 and 2.
NSString* const kSumSource = @R"METAL(
#include <metal_stdlib>
using namespace metal;
struct V { float4 position [[position]]; };
vertex V sum_vs(uint id [[vertex_id]]) {
    V v;
    v.position = float4(float2((id << 1) & 2, id & 2) * 2.0 - 1.0, 0.0, 1.0);
    return v;
}
fragment float4 sum_ps(V in [[stage_in]], texture2d<float> a [[texture(0)]], texture2d<float> b [[texture(1)]], texture2d<float> c [[texture(2)]]) {
    return a.read(uint2(0, 0)) + b.read(uint2(0, 0)) + c.read(uint2(0, 0));
}
)METAL";

id<MTLRenderPipelineState> NewPipeline(NSString* source, NSString* vertex, NSString* fragment, MTLPixelFormat format)
{
    NSError* error = nil;
    id<MTLLibrary> library = [gDevice newLibraryWithSource:source options:nil error:&error];
    if (!library) {
        printf("gfx metal: shader: %s\n", error.localizedDescription.UTF8String);
        return nil;
    }
    MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor new] autorelease];
    desc.vertexFunction = [[library newFunctionWithName:vertex] autorelease];
    desc.fragmentFunction = [[library newFunctionWithName:fragment] autorelease];
    desc.colorAttachments[0].pixelFormat = format;
    id<MTLRenderPipelineState> state = [gDevice newRenderPipelineStateWithDescriptor:desc error:&error];
    [library release];
    return state;
}

// A 1x1 texture of one colour, and reading a texture's first texel.
id<MTLTexture> NewColour(uint8_t r, uint8_t g, uint8_t b)
{
    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [gDevice newTextureWithDescriptor:desc];
    uint8_t texel[4] = { r, g, b, 0 };
    [texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:texel bytesPerRow:4];
    return texture;
}

uint32_t FirstTexel(id<MTLTexture> texture)
{
    uint8_t texel[4] = {};
    [texture getBytes:texel bytesPerRow:4 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
    return (uint32_t)texel[0] << 16 | (uint32_t)texel[1] << 8 | texel[2];
}

TEST(bind_dedup_drops_only_binds_of_what_is_bound)
{
    @autoreleasepool {
        id<MTLRenderPipelineState> sum = NewPipeline(kSumSource, @"sum_vs", @"sum_ps", MTLPixelFormatRGBA8Unorm);
        CHECK(sum != nil);
        if (!sum) return;
        id<MTLCommandQueue> queue = [gDevice newCommandQueueWithMaxCommandBufferCount:8];
        id<MTLTexture> red = NewColour(0x40, 0, 0), green = NewColour(0, 0x40, 0), blue = NewColour(0, 0, 0x40), black = NewColour(0, 0, 0);
        id<MTLTexture> targets[4];
        for (auto& target : targets) target = NewColour(0xff, 0xff, 0xff);

        auto before = BindDedupCounts();
        id<MTLCommandBuffer> buffer = [queue commandBufferWithUnretainedReferences];
        auto draw = [&](id<MTLTexture> target, auto&& binds) {
            id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:PassTo(target)];
            binds(encoder);
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
        };
        id<MTLTexture> rgb[3] = { red, green, blue };
        // As the game binds: the whole set before every draw.
        draw(targets[0], [&](id<MTLRenderCommandEncoder> e) {
            [e setRenderPipelineState:sum];
            [e setFragmentTextures:rgb withRange:NSMakeRange(0, 3)];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e setRenderPipelineState:sum];                             // dropped
            [e setFragmentTextures:rgb withRange:NSMakeRange(0, 3)];    // three dropped
        });
        // One slot of three changes: the other two are not sent again, the changed one is.
        draw(targets[1], [&](id<MTLRenderCommandEncoder> e) {
            [e setRenderPipelineState:sum];
            [e setFragmentTextures:rgb withRange:NSMakeRange(0, 3)];
            id<MTLTexture> rkb[3] = { red, black, blue };
            [e setFragmentTextures:rkb withRange:NSMakeRange(0, 3)];    // two dropped
        });
        // One at a time, back and forth: a slot bound over and bound back is sent both times.
        draw(targets[2], [&](id<MTLRenderCommandEncoder> e) {
            [e setRenderPipelineState:sum];
            [e setFragmentTexture:red atIndex:0];
            [e setFragmentTexture:black atIndex:1];
            [e setFragmentTexture:black atIndex:2];
            [e setFragmentTexture:green atIndex:0];
            [e setFragmentTexture:red atIndex:0];
            [e setFragmentTexture:red atIndex:0];                       // dropped
            [e setFragmentTexture:nil atIndex:1];
            [e setFragmentTexture:blue atIndex:1];
        });
        // A new encoder starts with nothing bound: what the last one held is sent again.
        draw(targets[3], [&](id<MTLRenderCommandEncoder> e) {
            [e setRenderPipelineState:sum];
            [e setFragmentTextures:rgb withRange:NSMakeRange(0, 3)];
        });
        [buffer commit];
        [buffer waitUntilCompleted];
        CHECK(buffer.status == MTLCommandBufferStatusCompleted);

        CHECK(FirstTexel(targets[0]) == 0x404040);
        CHECK(FirstTexel(targets[1]) == 0x400040);
        CHECK(FirstTexel(targets[2]) == 0x400040);
        CHECK(FirstTexel(targets[3]) == 0x404040);
        auto counts = BindDedupCounts();
        CHECK(counts.Seen - before.Seen == 8 + 7 + 9 + 4);
        CHECK(counts.Dropped - before.Dropped == 4 + 2 + 1 + 0);

        for (id<MTLTexture> texture : { red, green, blue, black, targets[0], targets[1], targets[2], targets[3] }) [texture release];
        [sum release];
        [queue release];
    }
}

// --- the cluster guard ----------------------------------------------------------------------------

// Kernels with the arguments of the game's: one reads ClusterInfoBuffer as its
// clustered-lighting kernels do, one writes a buffer of that name, one has
// another buffer in that slot.
NSString* const kClusterSource = @R"METAL(
#include <metal_stdlib>
using namespace metal;
kernel void ClusteredLighting_0(texture2d<float, access::write> lit [[texture(0)]], constant float& bias [[buffer(0)]],
                                device const int4* ClusterInfoBuffer [[buffer(1)]], uint2 p [[thread_position_in_grid]]) {
    lit.write(float4(float(ClusterInfoBuffer[0].x) / 255.0 + bias, 0.0, 0.0, 1.0), p);
}
kernel void ClusterAssignment(device int4* ClusterInfoBuffer [[buffer(1)]], uint2 p [[thread_position_in_grid]]) {
    ClusterInfoBuffer[0] = int4(9);
}
kernel void ClusteredClear(device const int4* ClusteredTileCounts [[buffer(1)]], texture2d<float, access::write> lit [[texture(0)]],
                           uint2 p [[thread_position_in_grid]]) {
    lit.write(float4(float(ClusteredTileCounts[0].x)), p);
}
)METAL";

TEST(cluster_guard_stands_in_only_for_an_unbound_cluster_buffer)
{
    if (@available(macOS 13.0, *)) {
    } else {
        return;     // the guard is off on this system, and says so (above)
    }
    @autoreleasepool {
        NSError* error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithSource:kClusterSource options:nil error:&error];
        CHECK(library != nil);
        if (!library) return;
        auto kernel = [&](NSString* name) {
            id<MTLFunction> function = [[library newFunctionWithName:name] autorelease];
            return [gDevice newComputePipelineStateWithFunction:function error:&error];
        };
        id<MTLComputePipelineState> lighting = kernel(@"ClusteredLighting_0"), assignment = kernel(@"ClusterAssignment"), clear = kernel(@"ClusteredClear");
        CHECK(lighting && assignment && clear);
        // Watched: the kernel that only reads a buffer of that name, at the slot its source gives.
        CHECK(ClusterGuardSlot(lighting) == 1);
        CHECK(ClusterGuardSlot(assignment) == -1 && ClusterGuardSlot(clear) == -1);

        id<MTLCommandQueue> queue = [gDevice newCommandQueueWithMaxCommandBufferCount:8];
        id<MTLTexture> targets[4];
        for (auto& target : targets) {
            MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
            desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            desc.storageMode = MTLStorageModeShared;
            target = [gDevice newTextureWithDescriptor:desc];
        }
        int32_t clusters[4] = { 128, 0, 0, 0 };
        id<MTLBuffer> own = [gDevice newBufferWithBytes:clusters length:sizeof clusters options:MTLResourceStorageModeShared];
        uint32_t groups[3] = { 1, 1, 1 };
        id<MTLBuffer> indirect = [gDevice newBufferWithBytes:groups length:sizeof groups options:MTLResourceStorageModeShared];
        float bias = 0.25f;
        MTLSize one = MTLSizeMake(1, 1, 1);

        auto before = ClusterGuardCount();
        id<MTLCommandBuffer> buffer = [queue commandBufferWithUnretainedReferences];
        auto encode = [&](id<MTLTexture> target, auto&& rest) {
            id<MTLComputeCommandEncoder> encoder = [buffer computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
            [encoder setComputePipelineState:lighting];
            [encoder setTexture:target atIndex:0];
            [encoder setBytes:&bias length:sizeof bias atIndex:0];
            rest(encoder);
            [encoder endEncoding];
        };
        // Nothing bound to the slot: the stand-in, once for the encoder however often it dispatches.
        encode(targets[0], [&](id<MTLComputeCommandEncoder> e) {
            [e dispatchThreadgroups:one threadsPerThreadgroup:one];
            [e dispatchThreads:one threadsPerThreadgroup:one];
        });
        CHECK(ClusterGuardCount() == before + 1);
        // The way the game dispatches its tiles.
        encode(targets[1], [&](id<MTLComputeCommandEncoder> e) {
            [e dispatchThreadgroupsWithIndirectBuffer:indirect indirectBufferOffset:0 threadsPerThreadgroup:one];
        });
        CHECK(ClusterGuardCount() == before + 2);
        // The game's own buffer: left alone, whichever way it was bound.
        encode(targets[2], [&](id<MTLComputeCommandEncoder> e) {
            [e setBuffer:own offset:0 atIndex:1];
            [e dispatchThreadgroups:one threadsPerThreadgroup:one];
        });
        encode(targets[3], [&](id<MTLComputeCommandEncoder> e) {
            NSUInteger offset = 0;
            [e setBuffers:&own offsets:&offset withRange:NSMakeRange(1, 1)];
            [e dispatchThreadgroups:one threadsPerThreadgroup:one];
        });
        CHECK(ClusterGuardCount() == before + 2);
        [buffer commit];
        [buffer waitUntilCompleted];
        CHECK(buffer.status == MTLCommandBufferStatusCompleted);

        // Zeros from the stand-in, 128 from the game's buffer; the bias in both.
        CHECK(FirstTexel(targets[0]) >> 16 == 0x40 && FirstTexel(targets[1]) >> 16 == 0x40);
        CHECK(FirstTexel(targets[2]) >> 16 == 0xc0 && FirstTexel(targets[3]) >> 16 == 0xc0);

        for (id<MTLTexture> target : targets) [target release];
        for (id object : { (id)own, (id)indirect, (id)lighting, (id)assignment, (id)clear, (id)library, (id)queue }) [object release];
    }
}

// The same kernel made into a pipeline again and again, as a long session
// does: each time a new object, and each one is remembered, past any number
// a table of fixed size would hold, while other threads look pipelines up.
// Half of them are then let go of, which leaves the entries of the others
// among those that went: the others are all still found.
TEST(every_pipeline_is_remembered_however_many_are_made)
{
    if (@available(macOS 13.0, *)) {
    } else {
        return;
    }
    @autoreleasepool {
        NSError* error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithSource:kClusterSource options:nil error:&error];
        CHECK(library != nil);
        if (!library) return;
        id<MTLFunction> lighting = [[library newFunctionWithName:@"ClusteredLighting_0"] autorelease];
        id<MTLFunction> assignment = [[library newFunctionWithName:@"ClusterAssignment"] autorelease];
        id<MTLComputePipelineState> first = [gDevice newComputePipelineStateWithFunction:lighting error:&error];
        id<MTLComputePipelineState> other = [gDevice newComputePipelineStateWithFunction:assignment error:&error];
        CHECK(first && other);
        if (!first || !other) return;

        std::atomic<bool> done{ false };
        std::atomic<int> wrong{ 0 };
        std::atomic<uint64_t> looks{ 0 };
        auto look = [&] {
            while (!done.load(std::memory_order_acquire)) {
                if (ClusterGuardSlot(first) != 1 || ClusterGuardSlot(other) != -1) wrong.fetch_add(1);
                looks.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread readers[2] = { std::thread(look), std::thread(look) };

        constexpr int kMade = 3000;     // past three doublings of the first table
        std::vector<id> made;
        made.reserve(kMade);
        for (int i = 0; i < kMade; i++) {
            id<MTLComputePipelineState> state = [gDevice newComputePipelineStateWithFunction:lighting error:&error];
            CHECK(state != nil);
            if (!state) break;
            CHECK(ClusterGuardSlot(state) == 1);
            made.push_back(state);
        }
        std::vector<id> distinct = made;
        std::sort(distinct.begin(), distinct.end());
        distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
        CHECK(distinct.size() == (size_t)kMade);

        // Nothing is made from here on, so no other pipeline is at a released one's address.
        for (size_t i = 0; i < made.size(); i += 2) [made[i] release];
        int forgotten = 0, kept = 0;
        for (size_t i = 0; i < made.size(); i++) {
            int slot = ClusterGuardSlot(made[i]);
            if (i % 2) {
                forgotten += slot != 1;
            } else {
                kept += slot != -1;
            }
        }
        CHECK(forgotten == 0 && kept == 0);
        done.store(true, std::memory_order_release);
        for (auto& reader : readers) reader.join();
        CHECK(ClusterGuardSlot(first) == 1 && ClusterGuardSlot(other) == -1);
        CHECK(wrong.load() == 0 && looks.load() > 0);

        for (size_t i = 1; i < made.size(); i += 2) [made[i] release];
        for (id object : { (id)first, (id)other, (id)library }) [object release];
    }
}

// A pipeline the hooks remember is still deallocated when the game lets go
// of it, and its entry goes with it: what Metal makes at that address next
// is not taken for it.
TEST(a_released_pipeline_is_freed_and_its_address_starts_fresh)
{
    if (@available(macOS 13.0, *)) {
    } else {
        return;
    }
    @autoreleasepool {
        NSError* error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithSource:kClusterSource options:nil error:&error];
        CHECK(library != nil);
        if (!library) return;
        id<MTLFunction> lighting = [[library newFunctionWithName:@"ClusteredLighting_0"] autorelease];
        id<MTLFunction> assignment = [[library newFunctionWithName:@"ClusterAssignment"] autorelease];

        constexpr int kRounds = 64;
        int freed = 0, reused = 0, mistaken = 0;
        for (int round = 0; round < kRounds; round++) {
            std::atomic<bool> gone{ false };
            void* address = nullptr;
            @autoreleasepool {
                id<MTLComputePipelineState> state = [gDevice newComputePipelineStateWithFunction:lighting error:&error];
                CHECK(state != nil);
                if (!state) break;
                CHECK(ClusterGuardSlot(state) == 1);
                WatchDealloc(state, &gone);
                address = (void*)state;
                [state release];
            }
            if (!GoneSoon(gone)) continue;
            freed++;
            mistaken += ClusterGuardSlot(address) != -1;
            // Kernels the guard has nothing to do with, until one is made where the released one was.
            std::vector<id> held;
            for (int i = 0; i < 8; i++) {
                id next = [gDevice newComputePipelineStateWithFunction:assignment error:&error];
                if (!next) break;
                held.push_back(next);
                if ((void*)next != address) continue;
                reused++;
                mistaken += ClusterGuardSlot(next) != -1;
                break;
            }
            for (id object : held) [object release];
        }
        CHECK(freed == kRounds);
        CHECK(reused > 0);              // or nothing was shown of an address given out again
        CHECK(mistaken == 0);
        [library release];
    }
}

// --- Temporal -------------------------------------------------------------------------------------

// The passes Temporal knows, with the names and arguments the game's have
// (Core.metallib of 4.1.1.7631656), each doing something that can be told
// apart in the result: this TAA halves the image, this RCAS adds red.
NSString* const kFrameSource = @R"METAL(
#include <metal_stdlib>
using namespace metal;
struct V { float4 position [[position]]; float2 texCoord; };
vertex V pp_vs(uint id [[vertex_id]]) {
    V v;
    v.texCoord = float2((id << 1) & 2, id & 2);
    v.position = float4(v.texCoord * 2.0 - 1.0, 0.0, 1.0);
    return v;
}
struct _TemporalConstants {
    float4x4 g_ViewMat_prev; float4x4 g_ProjectionMat_prev; float4x4 g_OffsetViewProjectionMat_prev;
    packed_float3 g_CameraWorldPos_prev; float g_CurrentTime_prev; float2 g_Jitter; float2 g_PreviousJitter;
};
struct _Viewport { float2 g_ViewportScale; float2 g_ViewportOffset; uint2 g_ViewportSize; float g_Rest[36]; };
struct _PerCamera {
    float4x4 g_ViewMat; float4x4 g_ProjectionMat; float4x4 g_OffsetViewProjectionMat;
    float g_NearPlane; float g_FarPlane; float2 g_PerCamera_PADDING0; packed_float3 g_CameraWorldPos; int g_TetrahedronFaceIndex;
};
fragment float LinearizeDepth(V in [[stage_in]], texture2d<float> _sceneDepth [[texture(0)]]) {
    return _sceneDepth.read(uint2(in.position.xy)).r;
}
fragment float2 VelocityBufferCamera_PS(V in [[stage_in]], constant _PerCamera& PerCamera [[buffer(11)]],
                                        constant _TemporalConstants& TemporalConstants [[buffer(8)]]) {
    return float2(PerCamera.g_NearPlane, TemporalConstants.g_Jitter.x) * 1e-6;
}
fragment float4 PPTAA_PS(V in [[stage_in]], texture2d<float> Current [[texture(0)]], texture2d<float> History [[texture(1)]],
                         texture2d<float> Velocity [[texture(2)]], constant _TemporalConstants& TemporalConstants [[buffer(8)]],
                         constant _Viewport& Viewport [[buffer(9)]]) {
    uint2 at = uint2(in.position.xy);
    float unused = (Velocity.read(at).r + History.read(at).r + TemporalConstants.g_Jitter.x + Viewport.g_ViewportScale.x) * 1e-6;
    return float4(Current.read(at).rgb * 0.5 + unused, 1.0);
}
fragment float4 FSR_Tonemap(V in [[stage_in]], texture2d<float> InputTexture [[texture(0)]]) {
    float3 c = InputTexture.read(uint2(in.position.xy)).rgb;
    return float4(c / (1.0 + max(c.r, max(c.g, c.b))), 1.0);
}
kernel void FSR(texture2d<half> InputTexture [[texture(0)]], texture2d<half, access::write> OutputTexture [[texture(1)]],
                uint2 p [[thread_position_in_grid]]) {
    OutputTexture.write(InputTexture.read(p / 2), p);
}
kernel void FSR_RCAS(texture2d<half> InputTexture [[texture(0)]], texture2d<half, access::write> OutputTexture [[texture(2)]],
                     uint2 p [[thread_position_in_grid]]) {
    half4 c = InputTexture.read(p);
    OutputTexture.write(half4(c.r + 0.5h, c.g, c.b, 1.0h), p);
}
)METAL";

constexpr NSUInteger kRenderW = 160, kRenderH = 120, kOutW = 320, kOutH = 240;

struct FrameKit
{
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> linearize, velocity, taa, tonemap, sum;
    id<MTLComputePipelineState> fsr, rcas;
    id<MTLTexture> depth, linear, color, history, motion, taaTarget, tonemapped, scaled, destination, otherDestination, black;
    id<MTLTexture> smallColor, smallTaaTarget;      // a view that is never scaled
    id<MTLBuffer> temporalConstants;
    float camera[56], temporal[56];
    struct { float scaleOffset[4]; uint32_t size[2]; float rest[36]; } viewport, smallViewport;
};

MTLRenderPassDescriptor* PassKeeping(id<MTLTexture> target, MTLLoadAction load, MTLClearColor clear)
{
    MTLRenderPassDescriptor* pass = PassTo(target);
    pass.colorAttachments[0].loadAction = load;
    pass.colorAttachments[0].clearColor = clear;
    return pass;
}

bool MakeFrameKit(FrameKit& k)
{
    NSError* error = nil;
    id<MTLLibrary> library = [gDevice newLibraryWithSource:kFrameSource options:nil error:&error];
    if (!library) {
        printf("gfx metal: shader: %s\n", error.localizedDescription.UTF8String);
        return false;
    }
    auto render = [&](NSString* fragment, MTLPixelFormat format) {
        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor new] autorelease];
        desc.vertexFunction = [[library newFunctionWithName:@"pp_vs"] autorelease];
        desc.fragmentFunction = [[library newFunctionWithName:fragment] autorelease];
        desc.colorAttachments[0].pixelFormat = format;
        return [gDevice newRenderPipelineStateWithDescriptor:desc error:&error];
    };
    auto compute = [&](NSString* name) {
        return [gDevice newComputePipelineStateWithFunction:[[library newFunctionWithName:name] autorelease] error:&error];
    };
    k.linearize = render(@"LinearizeDepth", MTLPixelFormatR32Float);
    k.velocity = render(@"VelocityBufferCamera_PS", MTLPixelFormatRG16Float);
    k.taa = render(@"PPTAA_PS", MTLPixelFormatRG11B10Float);
    k.tonemap = render(@"FSR_Tonemap", MTLPixelFormatRG11B10Float);
    k.fsr = compute(@"FSR");
    k.rcas = compute(@"FSR_RCAS");
    k.sum = NewPipeline(kSumSource, @"sum_vs", @"sum_ps", MTLPixelFormatRGBA8Unorm);
    [library release];
    if (!k.linearize || !k.velocity || !k.taa || !k.tonemap || !k.fsr || !k.rcas || !k.sum) return false;

    MTLTextureUsage target = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    MTLTextureUsage written = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    k.depth = NewTexture(MTLPixelFormatDepth32Float_Stencil8, kRenderW, kRenderH, target);
    k.linear = NewTexture(MTLPixelFormatR32Float, kRenderW, kRenderH, target);
    k.color = NewTexture(MTLPixelFormatRG11B10Float, kRenderW, kRenderH, target);
    k.history = NewTexture(MTLPixelFormatRG11B10Float, kRenderW, kRenderH, target);
    k.motion = NewTexture(MTLPixelFormatRG16Float, kRenderW, kRenderH, target);
    k.taaTarget = NewTexture(MTLPixelFormatRG11B10Float, kRenderW, kRenderH, target);
    k.tonemapped = NewTexture(MTLPixelFormatRG11B10Float, kRenderW, kRenderH, target);
    k.scaled = NewTexture(MTLPixelFormatRG11B10Float, kOutW, kOutH, written);
    k.destination = NewTexture(MTLPixelFormatRG11B10Float, kOutW, kOutH, written);
    k.otherDestination = NewTexture(MTLPixelFormatRG11B10Float, kOutW, kOutH, written);
    k.smallColor = NewTexture(MTLPixelFormatRG11B10Float, 64, 64, target);
    k.smallTaaTarget = NewTexture(MTLPixelFormatRG11B10Float, 64, 64, target);
    k.black = NewColour(0, 0, 0);
    k.queue = [gDevice newCommandQueueWithMaxCommandBufferCount:8];

    // A reversed-Z projection (depth 1 at the near plane, 0 at the far) with a 60 degree view, and a sub-pixel jitter.
    memset(k.camera, 0, sizeof k.camera);
    memset(k.temporal, 0, sizeof k.temporal);
    float nearPlane = 0.1f, farPlane = 1000.0f;
    k.camera[21] = 1.7320508f;
    k.camera[48] = nearPlane;
    k.camera[49] = farPlane;
    k.camera[30] = nearPlane * farPlane / (farPlane - nearPlane);
    k.camera[26] = 1.0f - k.camera[30] / nearPlane;
    k.temporal[52] = 0.25f;
    k.temporal[53] = -0.125f;
    k.temporalConstants = [gDevice newBufferWithBytes:k.temporal length:sizeof k.temporal options:MTLResourceStorageModeShared];
    k.viewport = { { 1, 1, 0, 0 }, { (uint32_t)kRenderW, (uint32_t)kRenderH }, {} };
    k.smallViewport = { { 1, 1, 0, 0 }, { 64, 64 }, {} };
    return true;
}

struct FrameResult
{
    uint32_t taaTarget, destination, otherDestination, smallTaaTarget;
    bool completed;
};

// One frame as the game encodes it, in one of its command buffers. `rcasTo`:
// where this frame's RCAS writes (nil: no RCAS after FSR).
FrameResult EncodeFrame(FrameKit& k, id<MTLTexture> rcasTo)
{
    FrameResult result{};
    @autoreleasepool {
        id<MTLCommandBuffer> buffer = [k.queue commandBufferWithUnretainedReferences];
        MTLClearColor none = MTLClearColorMake(0, 0, 0, 0);
        auto pass = [&](id<MTLTexture> target, id<MTLRenderPipelineState> pipeline, auto&& binds) {
            id<MTLRenderCommandEncoder> e = [buffer renderCommandEncoderWithDescriptor:PassKeeping(target, MTLLoadActionClear, none)];
            [e setRenderPipelineState:pipeline];
            binds(e);
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
        };
        // The scene: depth, and a colour of plain green.
        MTLRenderPassDescriptor* scene = PassKeeping(k.color, MTLLoadActionClear, MTLClearColorMake(0, 0.5, 0, 1));
        scene.depthAttachment.texture = k.depth;
        scene.depthAttachment.loadAction = MTLLoadActionClear;
        scene.depthAttachment.clearDepth = 0.5;
        scene.depthAttachment.storeAction = MTLStoreActionStore;
        scene.stencilAttachment.texture = k.depth;
        scene.stencilAttachment.loadAction = MTLLoadActionClear;
        scene.stencilAttachment.storeAction = MTLStoreActionStore;
        [[buffer renderCommandEncoderWithDescriptor:scene] endEncoding];
        [[buffer renderCommandEncoderWithDescriptor:PassKeeping(k.smallColor, MTLLoadActionClear, MTLClearColorMake(0, 0.5, 0, 1))] endEncoding];

        id<MTLTexture> depthOnly[1] = { k.depth };
        pass(k.linear, k.linearize, [&](id<MTLRenderCommandEncoder> e) { [e setFragmentTextures:depthOnly withRange:NSMakeRange(0, 1)]; });
        pass(k.motion, k.velocity, [&](id<MTLRenderCommandEncoder> e) {
            [e setFragmentBytes:k.camera length:sizeof k.camera atIndex:11];
            [e setFragmentBuffer:k.temporalConstants offset:0 atIndex:8];
        });
        id<MTLTexture> taaInputs[3] = { k.color, k.history, k.motion };
        pass(k.taaTarget, k.taa, [&](id<MTLRenderCommandEncoder> e) {
            [e setFragmentTextures:taaInputs withRange:NSMakeRange(0, 3)];
            [e setFragmentBuffer:k.temporalConstants offset:0 atIndex:8];
            [e setFragmentBytes:&k.viewport length:sizeof k.viewport atIndex:9];
        });
        // Another view with TAA and no FSR, as the game's portraits are.
        id<MTLTexture> smallInputs[3] = { k.smallColor, k.history, k.motion };
        pass(k.smallTaaTarget, k.taa, [&](id<MTLRenderCommandEncoder> e) {
            [e setFragmentTextures:smallInputs withRange:NSMakeRange(0, 3)];
            [e setFragmentBuffer:k.temporalConstants offset:0 atIndex:8];
            [e setFragmentBytes:&k.smallViewport length:sizeof k.smallViewport atIndex:9];
        });
        id<MTLTexture> tonemapInput[1] = { k.taaTarget };
        pass(k.tonemapped, k.tonemap, [&](id<MTLRenderCommandEncoder> e) { [e setFragmentTextures:tonemapInput withRange:NSMakeRange(0, 1)]; });

        id<MTLComputeCommandEncoder> c = [buffer computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
        id<MTLTexture> fsrTextures[2] = { k.tonemapped, k.scaled };
        [c setComputePipelineState:k.fsr];
        [c setTextures:fsrTextures withRange:NSMakeRange(0, 2)];
        [c dispatchThreads:MTLSizeMake(kOutW, kOutH, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        if (rcasTo) {
            id<MTLTexture> rcasTextures[3] = { k.scaled, k.black, rcasTo };
            [c setComputePipelineState:k.rcas];
            [c setTextures:rcasTextures withRange:NSMakeRange(0, 3)];
            [c dispatchThreads:MTLSizeMake(kOutW, kOutH, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        }
        [c endEncoding];

        // What the rest of the frame would read.
        id<MTLTexture> shown[4] = { k.taaTarget, k.destination, k.otherDestination, k.smallTaaTarget };
        id<MTLTexture> readable[4];
        for (int i = 0; i < 4; i++) {
            readable[i] = NewColour(0xff, 0xff, 0xff);
            id<MTLRenderCommandEncoder> e = [buffer renderCommandEncoderWithDescriptor:PassTo(readable[i])];
            id<MTLTexture> textures[3] = { shown[i], k.black, k.black };
            [e setRenderPipelineState:k.sum];
            [e setFragmentTextures:textures withRange:NSMakeRange(0, 3)];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
        }
        [buffer commit];
        [buffer waitUntilCompleted];
        result.completed = buffer.status == MTLCommandBufferStatusCompleted;
        result.taaTarget = FirstTexel(readable[0]);
        result.destination = FirstTexel(readable[1]);
        result.otherDestination = FirstTexel(readable[2]);
        result.smallTaaTarget = FirstTexel(readable[3]);
        for (id<MTLTexture> texture : readable) [texture release];
    }
    return result;
}

bool Near(uint32_t texel, int r, int g, int b)
{
    auto close = [](int have, int want) { return have >= want - 3 && have <= want + 3; };
    return close((int)(texel >> 16), r) && close((int)(texel >> 8 & 0xff), g) && close((int)(texel & 0xff), b);
}

// The counts move when the GPU has finished a frame, on Metal's thread.
unsigned ScaledSoon(unsigned want)
{
    for (int i = 0; i < 200 && TemporalStats().Scaled < want; i++) usleep(5000);
    return TemporalStats().Scaled;
}

TEST(temporal_watches_a_route_then_takes_it_over)
{
    if (!gTemporal) return;
    static FrameKit k;
    CHECK(MakeFrameKit(k));
    if (!k.queue) return;
    auto start = TemporalStats();
    CHECK(start.On && start.Scaled == 0 && start.RoutesProven == 0);
    // BG3's own TAA keeps BG3's own jitter while nothing is taken over.
    CHECK(!JitterActive());

    // The first frame is the game's own: its TAA (the image halved), its FSR
    // and its RCAS (red added), while the route is watched and proven.
    auto first = EncodeFrame(k, k.destination);
    CHECK(first.completed);
    CHECK(Near(first.taaTarget, 0, 0x40, 0));
    CHECK(Near(first.destination, 0x80, 0x33, 0));      // 0.25 compressed to 0.2, and RCAS's red
    CHECK(Near(first.smallTaaTarget, 0, 0x40, 0));
    auto proven = TemporalStats();
    CHECK(proven.On && proven.RoutesProven == 1 && proven.Scaled == 0 && proven.LeftToFsr == start.LeftToFsr + 1);
    CHECK(JitterActive());      // the longer sequence, from the frame MetalFX first takes

    // From the second on: the colour reaches TAA's target unchanged, FSR and
    // RCAS do not run, and MetalFX's image of the plain green scene is in
    // RCAS's destination, linear. The view that is not scaled keeps its TAA.
    for (unsigned frame = 1; frame <= 3; frame++) {
        auto next = EncodeFrame(k, k.destination);
        CHECK(next.completed);
        CHECK(Near(next.taaTarget, 0, 0x80, 0));
        CHECK(Near(next.destination, 0, 0x80, 0));
        CHECK(Near(next.smallTaaTarget, 0, 0x40, 0));
        CHECK(ScaledSoon(frame) == frame);
    }
    CHECK(TemporalStats().LeftToFsr == proven.LeftToFsr && TemporalStats().RoutesProven == 1);
}

TEST(temporal_gives_a_route_up_when_the_frame_leaves_it)
{
    if (!gTemporal) return;
    FrameKit k;
    CHECK(MakeFrameKit(k));
    if (!k.queue) return;
    auto start = TemporalStats();
    // New textures are a new route: watched first, then taken over.
    auto watched = EncodeFrame(k, k.destination);
    CHECK(Near(watched.destination, 0x80, 0x33, 0) && TemporalStats().RoutesProven == start.RoutesProven + 1);
    auto taken = EncodeFrame(k, k.destination);
    CHECK(Near(taken.destination, 0, 0x80, 0));
    CHECK(ScaledSoon(start.Scaled + 1) == start.Scaled + 1);

    // RCAS writes somewhere else than it did when the route was proven: it
    // runs as the game's, the route is dropped, and that is one strike.
    auto left = EncodeFrame(k, k.otherDestination);
    CHECK(left.completed && TemporalStats().On);
    CHECK((left.otherDestination >> 16) >= 0x7d);                   // RCAS's red: the game's kernel ran
    CHECK(TemporalStats().Scaled == start.Scaled + 1);
    // The frames after are the game's own again (its TAA too), until the new route is proven and taken over.
    auto again = EncodeFrame(k, k.otherDestination);
    CHECK(Near(again.taaTarget, 0, 0x40, 0) && Near(again.otherDestination, 0x80, 0x33, 0));
    CHECK(TemporalStats().RoutesProven == start.RoutesProven + 2);
    auto back = EncodeFrame(k, k.otherDestination);
    CHECK(Near(back.taaTarget, 0, 0x80, 0) && Near(back.otherDestination, 0, 0x80, 0));
    CHECK(ScaledSoon(start.Scaled + 2) == start.Scaled + 2);

    // A frame whose TAA was replaced and whose FSR never comes (the buffer
    // ends without it): a strike, and BG3's TAA draws for that target again.
    // The route is still proven, so the next whole frame is scaled, with
    // BG3's TAA that once; the one after has the copy again.
    {
        @autoreleasepool {
            id<MTLCommandBuffer> buffer = [k.queue commandBufferWithUnretainedReferences];
            auto pass = [&](id<MTLTexture> target, id<MTLRenderPipelineState> pipeline, auto&& binds) {
                id<MTLRenderCommandEncoder> e = [buffer renderCommandEncoderWithDescriptor:PassTo(target)];
                [e setRenderPipelineState:pipeline];
                binds(e);
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e endEncoding];
            };
            id<MTLTexture> depthOnly[1] = { k.depth };
            pass(k.linear, k.linearize, [&](id<MTLRenderCommandEncoder> e) { [e setFragmentTextures:depthOnly withRange:NSMakeRange(0, 1)]; });
            pass(k.motion, k.velocity, [&](id<MTLRenderCommandEncoder> e) {
                [e setFragmentBytes:k.camera length:sizeof k.camera atIndex:11];
                [e setFragmentBuffer:k.temporalConstants offset:0 atIndex:8];
            });
            id<MTLTexture> taaInputs[3] = { k.color, k.history, k.motion };
            pass(k.taaTarget, k.taa, [&](id<MTLRenderCommandEncoder> e) {
                [e setFragmentTextures:taaInputs withRange:NSMakeRange(0, 3)];
                [e setFragmentBuffer:k.temporalConstants offset:0 atIndex:8];
                [e setFragmentBytes:&k.viewport length:sizeof k.viewport atIndex:9];
            });
            [buffer commit];
            [buffer waitUntilCompleted];
        }
        // Metal lets go of the buffer a moment after it has run; what it had seen goes with it.
        for (int i = 0; i < 400 && TemporalStats().Missed == start.Missed; i++) usleep(5000);
        CHECK(TemporalStats().Missed == start.Missed + 1);
        auto recovering = EncodeFrame(k, k.otherDestination);
        CHECK(Near(recovering.taaTarget, 0, 0x40, 0));                  // BG3's TAA: the target was forgotten
        CHECK(Near(recovering.otherDestination, 0, 0x40, 0));           // and MetalFX all the same: the route stands
        CHECK(ScaledSoon(start.Scaled + 3) == start.Scaled + 3);
        auto recovered = EncodeFrame(k, k.otherDestination);
        CHECK(Near(recovered.taaTarget, 0, 0x80, 0) && Near(recovered.otherDestination, 0, 0x80, 0));
        CHECK(ScaledSoon(start.Scaled + 4) == start.Scaled + 4 && TemporalStats().On);
    }

    // FSR with no RCAS after it is not a shape that is taken over: the game's FSR, every frame.
    FrameKit bare;
    CHECK(MakeFrameKit(bare));
    for (int frame = 0; frame < 3; frame++) {
        auto alone = EncodeFrame(bare, nil);
        CHECK(alone.completed && Near(alone.taaTarget, 0, 0x40, 0));
    }
    CHECK(TemporalStats().RoutesProven == start.RoutesProven + 2 && TemporalStats().On);
}


// The same of a render pipeline Temporal remembers: let go of, it is freed.
TEST(a_released_render_pipeline_is_freed)
{
    if (!gTemporal || !TemporalStats().On) return;
    @autoreleasepool {
        NSError* error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithSource:kFrameSource options:nil error:&error];
        CHECK(library != nil);
        if (!library) return;
        std::atomic<bool> gone{ false };
        @autoreleasepool {
            MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor new] autorelease];
            desc.vertexFunction = [[library newFunctionWithName:@"pp_vs"] autorelease];
            desc.fragmentFunction = [[library newFunctionWithName:@"LinearizeDepth"] autorelease];
            desc.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float;
            id<MTLRenderPipelineState> linearize = [gDevice newRenderPipelineStateWithDescriptor:desc error:&error];
            CHECK(linearize != nil);
            if (linearize) {
                WatchDealloc(linearize, &gone);
                [linearize release];
            }
        }
        CHECK(GoneSoon(gone));
        CHECK(TemporalStats().On);
        [library release];
    }
}

// A TAA pipeline the game lets go of is freed, and the pipeline drawn in its
// place goes with it: TAA pipelines made one after another, any number of
// them, each get one, as long as no more than eight are alive at once.
TEST(a_released_taa_pipeline_gives_its_copy_back)
{
    if (!gTemporal || !TemporalStats().On) return;
    @autoreleasepool {
        NSError* error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithSource:kFrameSource options:nil error:&error];
        CHECK(library != nil);
        if (!library) return;
        MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor new] autorelease];
        desc.vertexFunction = [[library newFunctionWithName:@"pp_vs"] autorelease];
        desc.fragmentFunction = [[library newFunctionWithName:@"PPTAA_PS"] autorelease];
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatRG11B10Float;
        unsigned const before = TemporalStats().Copies;     // the frame kits' TAA pipelines, still alive
        CHECK(before < 8);
        int copied = 0, freed = 0;
        constexpr int kRounds = 12;                         // more than there are copies
        for (int round = 0; round < kRounds; round++) {
            std::atomic<bool> gone{ false };
            @autoreleasepool {
                id<MTLRenderPipelineState> taa = [gDevice newRenderPipelineStateWithDescriptor:desc error:&error];
                CHECK(taa != nil);
                if (!taa) break;
                copied += TemporalStats().Copies == before + 1;
                WatchDealloc(taa, &gone);
                [taa release];
            }
            freed += GoneSoon(gone);
            CHECK(TemporalStats().Copies == before);
        }
        CHECK(copied == kRounds && freed == kRounds);
        CHECK(TemporalStats().On);
        [library release];
    }
}
}

void RunGfxMetalTests(int* tests, int* failures)
{
    gDevice = MTLCreateSystemDefaultDevice();
    if (!gDevice) {
        // A machine without a GPU Metal can use: nothing here can be tested on it.
        printf("gfx metal: no Metal device, tests skipped\n");
        return;
    }
    for (auto test : { nothing_asked_changes_no_method_run, everything_asked_goes_on_once_run, stall_log_counts_commits_and_passes_them_on_run,
             bind_dedup_drops_only_binds_of_what_is_bound_run, cluster_guard_stands_in_only_for_an_unbound_cluster_buffer_run,
             every_pipeline_is_remembered_however_many_are_made_run,
             a_released_pipeline_is_freed_and_its_address_starts_fresh_run, temporal_watches_a_route_then_takes_it_over_run, temporal_gives_a_route_up_when_the_frame_leaves_it_run,
             a_released_render_pipeline_is_freed_run, a_released_taa_pipeline_gives_its_copy_back_run }) {
        test();
    }
    [gDevice release];
    *tests += gTests;
    *failures += gFailures;
}
