// MetalFX temporal upscaling in place of BG3's FSR 1.0 (gfx_temporal).
//
// With FSR on, BG3 renders the scene below the output size, runs its own TAA
// (PPTAA_PS), post-processes at that size, and at the end of the post chain
// compresses the image (FSR_Tonemap) and scales it up in one compute encoder:
// FSR writes the scaled image, FSR_RCAS sharpens it into the texture the rest
// of the frame (output-size passes, then the UI) goes on from, undoing the
// compression. MetalFX's temporal scaler does the anti-aliasing and the
// scaling in one step from the raw, jittered image, so:
//
//   - LinearizeDepth and VelocityBufferCamera_PS give a frame's depth texture
//     and camera constants; PPTAA_PS its colour, motion and jitter.
//   - PPTAA_PS is drawn with a copy shader in its place (the colour goes to
//     its target unchanged), for a target that has been seen to reach FSR.
//   - FSR and FSR_RCAS are not dispatched; when their encoder has ended,
//     MetalFX scales the image FSR_Tonemap was given (linear HDR) into the
//     texture RCAS would have written, in the game's own command buffer at the
//     place FSR had in it. Nothing later in the frame moves: what ran at the
//     output size after FSR, and the overlay's pass at the very end, still do.
//
// Nothing is taken over on a guess. The first time FSR comes with a pair of
// textures, the game's own TAA, FSR and RCAS run while this watches: only when
// RCAS followed FSR in the same encoder, every texture is of the kind MetalFX
// was set up for, and MetalFX has made its scaler, is that path ("route")
// taken over from the next frame on. A frame that does not fit runs the
// game's FSR. A frame whose TAA was already replaced when something stopped
// its scaling counts as a strike, as does a GPU error; three in a row turn
// Temporal off for the session.
//
// The research behind this (BG3's passes, constant layouts and jitter) is
// bg3mfx's, https://github.com/AndrewTriesToCode/bg3mfx (MIT, Copyright (c)
// 2026 AndrewTriesToCode). The code is written for bg3mse.

#import "GfxMetalInternal.h"

#import <MetalFX/MetalFX.h>

#include <Gfx/GfxJitter.h>
#include <Gfx/GfxLog.h>
#include <Gfx/GfxMetal.h>
#include <Gfx/GfxTemporalLogic.h>

#include <os/lock.h>

#if __has_feature(objc_arc)
#error "GfxTemporal.mm uses manual retain/release"
#endif

using namespace bg3se::mac::gfx;

// One scene frame: what PPTAA_PS was given.
__attribute__((visibility("hidden")))
@interface BG3MSEGfxFrame : NSObject {
@public
    id<MTLTexture> color, taaTarget, depth, motion;
    TemporalParams params;
    bool reset;         // BG3 gave no motion: fresh history
    bool replaced;      // drawn with the copy shader, not BG3's TAA
    bool scaled;        // MetalFX has been encoded for it
}
@end

// What one command buffer has seen. Lives and dies with the buffer.
__attribute__((visibility("hidden")))
@interface BG3MSEGfxScene : NSObject {
@public
    id<MTLTexture> depth;
    uint8_t camera[kCameraConstantBytes];
    bool hasCamera;
    BG3MSEGfxFrame* frames[4];
    unsigned frameCount;
    id<MTLTexture> tonemapSource, tonemapTarget;
}
@end

// The pipeline drawn in place of one TAA pipeline of BG3's. Associated with
// that pipeline, so it is let go of when the pipeline is deallocated.
__attribute__((visibility("hidden")))
@interface BG3MSEGfxTaaCopy : NSObject {
@public
    id pipeline;        // retained
}
@end

namespace bg3se::mac::gfx {
namespace {
void FrameMissed(BG3MSEGfxFrame* frame);
void GiveCopyBack();
}
}

@implementation BG3MSEGfxTaaCopy
- (void)dealloc
{
    [pipeline release];
    GiveCopyBack();
    [super dealloc];
}
@end

@implementation BG3MSEGfxFrame
- (void)dealloc
{
    [color release];
    [taaTarget release];
    [depth release];
    [motion release];
    [super dealloc];
}
@end

@implementation BG3MSEGfxScene
- (void)dealloc
{
    for (unsigned i = 0; i < frameCount; i++) {
        // Its TAA was replaced and the buffer is gone without MetalFX in it.
        if (frames[i]->replaced && !frames[i]->scaled) FrameMissed(frames[i]);
        [frames[i] release];
    }
    [depth release];
    [tonemapSource release];
    [tonemapTarget release];
    [super dealloc];
}
@end

namespace bg3se::mac::gfx {

namespace {

constexpr int kMaxStrikes = 3;
constexpr MTLPixelFormat kColorFormat = MTLPixelFormatRG11B10Float;
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float_Stencil8;
constexpr MTLPixelFormat kMotionFormat = MTLPixelFormatRG16Float;

std::atomic<bool> sOn{ false };
std::atomic<bool> sResetPending{ true };
std::atomic<int> sStrikes{ 0 };
std::atomic<unsigned> sScaled{ 0 }, sLeftToFsr{ 0 }, sRoutesProven{ 0 }, sMissed{ 0 };
float sSharpen = 0.3f;
char sSceneKey;

// A frame path seen from FSR to RCAS: its two textures, and what MetalFX needs for it.
struct Route
{
    id<MTLTexture> scaled, destination;     // retained
    uint32_t inW, inH, outW, outH;
    id scaler;                              // id<MTLFXTemporalScaler>, retained; nil: refused
    id<MTLTexture> output;                  // retained: where MetalFX writes when not into the destination
    bool sharpen;                           // the kernel takes `output` to the destination; else a copy does
};
constexpr unsigned kRoutes = 4, kKnownTargets = 4, kCopies = 8;

// How many TAA pipelines alive have a copy, at most kCopies.
std::atomic<unsigned> sCopies{ 0 };
char sCopyKey;

os_unfair_lock sLock = OS_UNFAIR_LOCK_INIT;     // everything below
Route sRoutes[kRoutes];
unsigned sRouteNext;
id sKnownTaaTargets[kKnownTargets];             // retained
unsigned sKnownNext;
uint8_t sLastCamera[kCameraConstantBytes];
bool sHasLastCamera;
id<MTLTexture> sZeroMotion;
SizeSettle sSettle;
std::string sLastReason;

// The longer jitter sequence is for MetalFX: BG3's own TAA keeps its own
// while no route is taken over. Called with sLock held.
void JitterFollowsRoutes();

void Stop(char const* why)
{
    if (!sOn.exchange(false)) return;
    SetJitterActive(false);
    LogError("[Gfx] gfx_temporal: off for this session, BG3's own TAA and FSR from here on (%s)", why);
}

// One frame went to BG3's FSR. Said for the first few and then now and again:
// with FSR as it is in the game this is every frame until a route is proven.
void LeftToFsr(char const* reason)
{
    sResetPending.store(true, std::memory_order_relaxed);
    unsigned n = sLeftToFsr.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8 || n % 3600 == 0) LogInfo("[Gfx] gfx_temporal: BG3's FSR for this frame (%u so far): %s", n, reason);
}

void Strike(char const* reason)
{
    sResetPending.store(true, std::memory_order_relaxed);
    int n = sStrikes.fetch_add(1, std::memory_order_relaxed) + 1;
    LogWarn("[Gfx] gfx_temporal: a frame was not scaled as planned (%d in a row): %s", n, reason);
    if (n >= kMaxStrikes) Stop(reason);
}

bool TargetKnown(id target)
{
    bool known = false;
    os_unfair_lock_lock(&sLock);
    for (id t : sKnownTaaTargets) known = known || t == target;
    os_unfair_lock_unlock(&sLock);
    return known;
}

void ForgetTarget(id target)
{
    id gone = nil;
    os_unfair_lock_lock(&sLock);
    for (id& t : sKnownTaaTargets) {
        if (t == target) {
            gone = t;
            t = nil;
        }
    }
    os_unfair_lock_unlock(&sLock);
    [gone release];
}

void KnowTarget(id target)
{
    if (!target || TargetKnown(target)) return;
    os_unfair_lock_lock(&sLock);
    id old = sKnownTaaTargets[sKnownNext % kKnownTargets];
    sKnownTaaTargets[sKnownNext++ % kKnownTargets] = [target retain];
    os_unfair_lock_unlock(&sLock);
    [old release];
}

void FrameMissed(BG3MSEGfxFrame* frame)
{
    // BG3's TAA draws for this target again, until a frame of it has reached MetalFX.
    sMissed.fetch_add(1, std::memory_order_relaxed);
    ForgetTarget(frame->taaTarget);
    Strike("its TAA was replaced, and FSR did not come for it in the same command buffer");
}

// --- Scenes -----------------------------------------------------------------------------------------

BG3MSEGfxScene* SceneOf(id<MTLCommandBuffer> buffer, bool create)
{
    BG3MSEGfxScene* scene = objc_getAssociatedObject(buffer, &sSceneKey);
    if (!scene && create) {
        scene = [BG3MSEGfxScene new];
        objc_setAssociatedObject(buffer, &sSceneKey, scene, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [scene release];
    }
    return scene;
}

void Assign(id<MTLTexture>& slot, id<MTLTexture> texture)
{
    [texture retain];
    [slot release];
    slot = texture;
}

bool ConstantBytes(GfxConstant const& c, size_t size, void* out)
{
    if (c.byteCount) {
        if (c.byteCount < size) return false;
        memcpy(out, c.bytes, size);
        return true;
    }
    id<MTLBuffer> buffer = c.buffer;
    if (!buffer || buffer.storageMode != MTLStorageModeShared) return false;
    void* contents = buffer.contents;
    NSUInteger length = buffer.length;
    if (!contents || c.offset > length || size > length - c.offset) return false;
    memcpy(out, (uint8_t const*)contents + c.offset, size);
    return true;
}

bool Plain2D(id<MTLTexture> texture)
{
    return texture && texture.textureType == MTLTextureType2D && texture.sampleCount == 1 && texture.mipmapLevelCount == 1 &&
           texture.storageMode != MTLStorageModeMemoryless && !texture.framebufferOnly &&
           texture.hazardTrackingMode != MTLHazardTrackingModeUntracked;
}

bool SameSize(id<MTLTexture> a, id<MTLTexture> b)
{
    return a.width == b.width && a.height == b.height;
}

// --- Draws -------------------------------------------------------------------------------------------

// A shared, all-zero motion texture for BG3's frames without motion: only ever read.
id<MTLTexture> ZeroMotionTexture(id<MTLDevice> device, NSUInteger width, NSUInteger height)
{
    os_unfair_lock_lock(&sLock);
    id<MTLTexture> texture = sZeroMotion;
    bool usable = texture && texture.width == width && texture.height == height && texture.device == device;
    if (usable) [texture retain];
    os_unfair_lock_unlock(&sLock);
    if (usable) return [texture autorelease];

    MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kMotionFormat width:width height:height mipmapped:NO];
    desc.storageMode = MTLStorageModeShared;
    desc.usage = MTLTextureUsageShaderRead;
    texture = [device newTextureWithDescriptor:desc];
    if (!texture) return nil;
    void* zeros = calloc(height, width * 4);
    if (zeros) [texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:zeros bytesPerRow:width * 4];
    free(zeros);
    if (!zeros) {
        [texture release];
        return nil;
    }

    os_unfair_lock_lock(&sLock);
    id<MTLTexture> old = sZeroMotion;
    sZeroMotion = [texture retain];
    os_unfair_lock_unlock(&sLock);
    [old release];
    return [texture autorelease];
}

bool TakeCopy()
{
    unsigned copies = sCopies.load(std::memory_order_relaxed);
    do {
        if (copies >= kCopies) return false;
    } while (!sCopies.compare_exchange_weak(copies, copies + 1, std::memory_order_relaxed));
    return true;
}

void GiveCopyBack()
{
    sCopies.fetch_sub(1, std::memory_order_relaxed);
}

// Read while the game has `taa` bound, so it is alive.
id CopyPipelineFor(id taa)
{
    BG3MSEGfxTaaCopy* copy = objc_getAssociatedObject(taa, &sCopyKey);
    return copy ? copy->pipeline : nil;
}

// PPTAA_PS is about to draw. True when the copy pipeline was bound in its place.
bool DrawTaa(id encoder, GfxEncoder* e)
{
    id<MTLTexture> color = e->fragment[0], motion = e->fragment[2], target = e->colorTarget;
    BG3MSEGfxScene* scene = SceneOf(e->buffer, false);
    id<MTLTexture> depth = scene ? scene->depth : nil;
    if (!color || !motion || !target || !depth || scene->frameCount == std::size(scene->frames)) return false;
    if (!Plain2D(color) || color.pixelFormat != kColorFormat || !Plain2D(target) || !SameSize(target, color) || !SameSize(depth, color)) return false;

    // BG3's stand-in on a frame with no valid motion, such as right after a cut.
    bool reset = motion.width == 16 && motion.height == 16 && motion.pixelFormat == MTLPixelFormatRGBA8Unorm;
    if (reset) motion = ZeroMotionTexture(e->buffer.device, color.width, color.height);
    if (!motion || !Plain2D(motion) || motion.pixelFormat != kMotionFormat || !SameSize(motion, color)) return false;

    uint8_t camera[kCameraConstantBytes], temporal[kTemporalConstantBytes], viewport[kViewportConstantBytes];
    bool haveCamera = false;
    if (scene->hasCamera) {
        memcpy(camera, scene->camera, sizeof camera);
        haveCamera = true;
    } else if (reset) {
        os_unfair_lock_lock(&sLock);
        if (sHasLastCamera) {
            memcpy(camera, sLastCamera, sizeof camera);
            haveCamera = true;
        }
        os_unfair_lock_unlock(&sLock);
    }
    if (!haveCamera || !ConstantBytes(e->constants[kConstTemporal], sizeof temporal, temporal) ||
        !ConstantBytes(e->constants[kConstViewport], sizeof viewport, viewport)) {
        return false;
    }
    if (!ViewportIsWhole(viewport, (uint32_t)color.width, (uint32_t)color.height)) return false;
    TemporalParams params;
    if (!TemporalParamsFrom(camera, temporal, &params)) return false;

    BG3MSEGfxFrame* frame = [BG3MSEGfxFrame new];
    frame->color = [color retain];
    frame->taaTarget = [target retain];
    frame->depth = [depth retain];
    frame->motion = [motion retain];
    frame->params = params;
    frame->reset = reset;
    scene->frames[scene->frameCount++] = frame;

    // Only for a target that has reached MetalFX before, and only by a shader
    // that writes the same values: the target takes the colour's format.
    id copy = TargetKnown(target) && target.pixelFormat == color.pixelFormat ? CopyPipelineFor(e->pipeline) : nil;
    if (!copy) return false;
    RawSetRenderPipeline(encoder, copy);
    frame->replaced = true;
    return true;
}

// The latest frame of the scene whose colour has the size of `like`.
BG3MSEGfxFrame* FrameSized(BG3MSEGfxScene* scene, id<MTLTexture> like)
{
    for (unsigned i = scene->frameCount; i-- > 0;) {
        if (SameSize(scene->frames[i]->color, like)) return scene->frames[i];
    }
    return nil;
}

// --- Routes ------------------------------------------------------------------------------------------

// The index of the route for these two textures, or kRoutes.
unsigned RouteFind(id scaled)
{
    for (unsigned i = 0; i < kRoutes; i++) {
        if (sRoutes[i].scaled == scaled) return i;
    }
    return kRoutes;
}

void JitterFollowsRoutes()
{
    bool any = false;
    for (auto const& route : sRoutes) any = any || route.scaler;
    SetJitterActive(any && sOn.load(std::memory_order_relaxed));
}

void RouteClear(Route& route)
{
    [route.scaled release];
    [route.destination release];
    [route.scaler release];
    [route.output release];
    route = Route{};
}

// --- Sharpening. MetalFX is softer than FSR with RCAS, which it replaces. This
// is the same idea as RCAS: in a compressed range, push each pixel away from its
// four neighbours, limited so it cannot overshoot them. With an amount of 0 it
// copies. ---

NSString* const kShaderSource = @R"METAL(
#include <metal_stdlib>
using namespace metal;
fragment float4 bg3mse_taa_copy(float4 position [[position]], texture2d<float> Current [[texture(0)]]) {
    return Current.read(uint2(position.xy));
}
/* Sharpening runs on compressed values, so HDR highlights do not ring. */
static float3 compressed(texture2d<float, access::read> src, int2 q, int2 size) {
    float3 c = max(src.read(uint2(clamp(q, int2(0), size - 1))).rgb, float3(0.0f));
    return c / (1.0f + max(c.r, max(c.g, c.b)));
}
kernel void bg3mse_sharpen(texture2d<float, access::read> src [[texture(0)]],
                           texture2d<float, access::write> dst [[texture(1)]],
                           constant float &amount [[buffer(0)]],
                           uint2 p [[thread_position_in_grid]]) {
    int2 size = int2(dst.get_width(), dst.get_height());
    int2 at = int2(p);
    if (at.x >= size.x || at.y >= size.y) return;
    if (amount <= 0.0f) {
        dst.write(float4(src.read(p).rgb, 1.0), p);
        return;
    }
    float3 n = compressed(src, at + int2(0, -1), size), s = compressed(src, at + int2(0, 1), size);
    float3 w = compressed(src, at + int2(-1, 0), size), e = compressed(src, at + int2(1, 0), size);
    float3 m = compressed(src, at, size);
    float3 low = min(n, min(s, min(w, e))), high = max(n, max(s, max(w, e)));
    float3 from_low = min(low, m) / (4.0 * high + 1e-5);
    float3 from_high = (1.0 - max(high, m)) / (4.0 * low - 4.0);
    float3 lobes = max(-from_low, from_high);
    float lobe = max(-0.1875, min(max(lobes.r, max(lobes.g, lobes.b)), 0.0)) * amount;
    float3 out = (lobe * (n + s + w + e) + m) / (4.0 * lobe + 1.0);
    float peak = max(out.r, max(out.g, out.b));
    dst.write(float4(out / max(1.0 - peak, 1e-3), 1.0), p);
}
)METAL";

// The module's shaders for `device`, compiled once. With no options, Metal
// picks the language version from the SDK the running program was linked
// against; the game's is older than this dylib's, so the version is stated.
// Metal 3.0 comes with macOS 13, which MetalFX needs anyway.
id<MTLLibrary> ShaderLibrary(id<MTLDevice> device) API_AVAILABLE(macos(13.0))
{
    static id<MTLLibrary> sLibrary;
    static id<MTLDevice> sDevice;
    static os_unfair_lock sLibraryLock = OS_UNFAIR_LOCK_INIT;
    os_unfair_lock_lock(&sLibraryLock);
    if (!sLibrary || sDevice != device) {
        MTLCompileOptions* options = [[MTLCompileOptions new] autorelease];
        options.languageVersion = MTLLanguageVersion3_0;
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:kShaderSource options:options error:&error];
        if (!library) LogError("[Gfx] gfx_temporal: its shaders do not compile: %s", error ? error.localizedDescription.UTF8String : "unknown");
        [sLibrary release];
        sLibrary = library;
        sDevice = device;
    }
    id<MTLLibrary> library = [[sLibrary retain] autorelease];
    os_unfair_lock_unlock(&sLibraryLock);
    return library;
}

id<MTLComputePipelineState> SharpenPipeline(id<MTLDevice> device) API_AVAILABLE(macos(13.0))
{
    static std::atomic<id<MTLComputePipelineState>> sPipeline{ nil };
    id<MTLComputePipelineState> pipeline = sPipeline.load(std::memory_order_acquire);
    if (pipeline) return pipeline.device == device ? pipeline : nil;
    id<MTLFunction> function = [[ShaderLibrary(device) newFunctionWithName:@"bg3mse_sharpen"] autorelease];
    tOwnEncoding = true;        // a pipeline of the module's own: nothing to learn from it
    pipeline = function ? [device newComputePipelineStateWithFunction:function error:nil] : nil;
    tOwnEncoding = false;
    if (!pipeline) return nil;
    id<MTLComputePipelineState> expected = nil;
    if (!sPipeline.compare_exchange_strong(expected, pipeline)) {
        [pipeline release];
        return expected.device == device ? expected : nil;
    }
    return pipeline;
}

// Whether the route from `frame` through `color` and `scaled` to `destination`
// can be taken over, with everything MetalFX needs for it made here. Empty
// when it can; "" with `later` set when it is too early to say; else why not.
std::string RouteProve(id<MTLDevice> device, BG3MSEGfxFrame* frame, id<MTLTexture> color, id<MTLTexture> scaled, id<MTLTexture> destination,
    Route* out, bool* later) API_AVAILABLE(macos(13.0))
{
    *later = false;
    if (!Plain2D(color) || color.pixelFormat != kColorFormat) return "the image FSR_Tonemap was given is not a plain RG11B10Float texture";
    if (!SameSize(color, frame->color)) return "the image FSR_Tonemap was given is not the size TAA rendered at";
    if (!Plain2D(frame->depth) || frame->depth.pixelFormat != kDepthFormat) return "the scene's depth is not a plain Depth32Float_Stencil8 texture";
    if (!Plain2D(destination) || !SameSize(destination, scaled)) return "RCAS's destination is not a plain texture of FSR's output size";
    if (destination.pixelFormat != kColorFormat && destination.pixelFormat != MTLPixelFormatRGBA16Float) {
        return "RCAS's destination is neither RG11B10Float nor RGBA16Float (is the game in HDR output?)";
    }
    uint32_t inW = (uint32_t)color.width, inH = (uint32_t)color.height, outW = (uint32_t)destination.width, outH = (uint32_t)destination.height;
    if (!ScaleSupported(inW, inH, outW, outH)) return "MetalFX scales up, by at most three times: FSR's sizes are outside that";

    // A scaler is only made for a size that has held for a moment: a window
    // being dragged goes through many.
    os_unfair_lock_lock(&sLock);
    bool any = false, haveThis = false;
    for (auto const& r : sRoutes) {
        any = any || r.scaler;
        haveThis = haveThis || (r.scaler && r.inW == inW && r.inH == inH && r.outW == outW && r.outH == outH);
    }
    bool settled = SizeSettled(&sSettle, inW, inH, outW, outH, NSProcessInfo.processInfo.systemUptime, any, haveThis);
    os_unfair_lock_unlock(&sLock);
    if (!settled) {
        *later = true;
        return {};
    }

    MTLFXTemporalScalerDescriptor* desc = [[MTLFXTemporalScalerDescriptor new] autorelease];
    desc.inputWidth = inW;
    desc.inputHeight = inH;
    desc.outputWidth = outW;
    desc.outputHeight = outH;
    desc.colorTextureFormat = kColorFormat;
    desc.depthTextureFormat = kDepthFormat;
    desc.motionTextureFormat = kMotionFormat;
    desc.outputTextureFormat = kColorFormat;
    desc.autoExposureEnabled = NO;
    desc.requiresSynchronousInitialization = YES;
    tOwnEncoding = true;
    id<MTLFXTemporalScaler> scaler = [desc newTemporalScalerWithDevice:device];
    tOwnEncoding = false;
    if (!scaler) return "MetalFX did not make a scaler for these sizes and formats";

    auto usable = [](id<MTLTexture> texture, MTLTextureUsage usage) { return (texture.usage & usage) == usage; };
    std::string problem;
    if (!usable(color, scaler.colorTextureUsage) || !usable(frame->depth, scaler.depthTextureUsage) || !usable(frame->motion, scaler.motionTextureUsage)) {
        problem = "the game's colour, depth or motion texture cannot be used the way MetalFX reads it";
    }
    // Straight into the game's texture when nothing else has to happen and it
    // allows it; otherwise through a texture of this module's and the kernel.
    Route route{};
    bool direct = sSharpen <= 0 && destination.pixelFormat == kColorFormat && usable(destination, scaler.outputTextureUsage);
    if (problem.empty() && !direct) {
        if (!usable(destination, MTLTextureUsageShaderWrite)) {
            problem = "RCAS's destination can be written neither by MetalFX nor by a kernel";
        } else if (!SharpenPipeline(device)) {
            problem = "the sharpening kernel could not be made";
        } else {
            MTLTextureDescriptor* texture = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kColorFormat width:outW height:outH mipmapped:NO];
            texture.storageMode = MTLStorageModePrivate;
            texture.usage = scaler.outputTextureUsage | MTLTextureUsageShaderRead;
            route.output = [device newTextureWithDescriptor:texture];
            route.sharpen = true;
            if (!route.output) problem = "no memory for MetalFX's output";
        }
    }
    if (!problem.empty()) {
        [scaler release];
        return problem;
    }
    route.scaled = [scaled retain];
    route.destination = [destination retain];
    route.inW = inW;
    route.inH = inH;
    route.outW = outW;
    route.outH = outH;
    route.scaler = scaler;
    *out = route;
    return {};
}

// RCAS followed a watched FSR: prove the route, or say once why it is not taken over.
void RouteWatched(id<MTLDevice> device, BG3MSEGfxFrame* frame, id<MTLTexture> color, id<MTLTexture> scaled, id<MTLTexture> destination)
{
    if (@available(macOS 13.0, *)) {
        Route route{};
        bool later = false;
        std::string problem = RouteProve(device, frame, color, scaled, destination, &route, &later);
        if (later) return;
        os_unfair_lock_lock(&sLock);
        bool said = sLastReason == problem;
        if (!problem.empty()) sLastReason = problem;
        // A route with no scaler stands for "these textures were looked at and refused".
        if (!problem.empty()) {
            route.scaled = [scaled retain];
            route.destination = [destination retain];
            route.outW = (uint32_t)destination.width;
            route.outH = (uint32_t)destination.height;
        }
        // In the place of what was known of these textures, or of the oldest route.
        unsigned at = RouteFind(scaled);
        if (at == kRoutes) at = sRouteNext++ % kRoutes;
        Route gone[kRoutes]{};
        gone[at] = sRoutes[at];
        sRoutes[at] = route;
        // A route of another output size is of a size the game has left: its
        // scaler and the game's textures it holds are let go.
        for (unsigned i = 0; i < kRoutes && problem.empty(); i++) {
            if (i != at && sRoutes[i].scaled && (sRoutes[i].outW != route.outW || sRoutes[i].outH != route.outH)) {
                gone[i] = sRoutes[i];
                sRoutes[i] = Route{};
            }
        }
        JitterFollowsRoutes();
        os_unfair_lock_unlock(&sLock);
        for (Route& old : gone) RouteClear(old);
        if (!problem.empty()) {
            if (!said) LogWarn("[Gfx] gfx_temporal: not taking over FSR here: %s", problem.c_str());
            return;
        }
        sRoutesProven.fetch_add(1, std::memory_order_relaxed);
        KnowTarget(frame->taaTarget);
        LogInfo("[Gfx] gfx_temporal: taking over from BG3's FSR, %ux%u to %ux%u, sharpening %.2f%s", route.inW, route.inH, route.outW, route.outH,
            sSharpen, route.sharpen ? "" : " (MetalFX writes the game's texture directly)");
    }
}

// MetalFX for `frame`, into the route's destination. The encoder FSR was in has ended.
void EncodeScaler(id<MTLCommandBuffer> buffer, unsigned routeIndex, BG3MSEGfxFrame* frame, id<MTLTexture> color) API_AVAILABLE(macos(13.0))
{
    os_unfair_lock_lock(&sLock);
    Route route = sRoutes[routeIndex];
    id<MTLFXTemporalScaler> scaler = [(id<MTLFXTemporalScaler>)route.scaler retain];
    id<MTLTexture> destination = [route.destination retain], output = [route.output retain];
    os_unfair_lock_unlock(&sLock);
    if (!scaler || buffer.status != MTLCommandBufferStatusNotEnqueued) {
        [scaler release];
        [destination release];
        [output release];
        frame->scaled = true;       // accounted for here, not again when its scene goes
        Strike("the route or the command buffer was gone when FSR's encoder ended");
        return;
    }

    bool reset = sResetPending.exchange(false, std::memory_order_relaxed) || frame->reset || !frame->replaced;
    tOwnEncoding = true;
    scaler.colorTexture = color;
    scaler.depthTexture = frame->depth;
    scaler.motionTexture = frame->motion;
    scaler.outputTexture = output ? output : destination;
    scaler.inputContentWidth = route.inW;
    scaler.inputContentHeight = route.inH;
    scaler.jitterOffsetX = frame->params.JitterX;
    scaler.jitterOffsetY = frame->params.JitterY;
    // BG3's velocity is current minus previous, in render pixels; MetalFX wants the way back.
    scaler.motionVectorScaleX = -1;
    scaler.motionVectorScaleY = -1;
    scaler.depthReversed = YES;
    scaler.preExposure = 1.0f;
    scaler.reset = reset;
    [scaler encodeToCommandBuffer:buffer];
    MarkOwnWork(buffer);
    // It would otherwise hold the game's render targets past a resize.
    scaler.colorTexture = nil;
    scaler.depthTexture = nil;
    scaler.motionTexture = nil;
    scaler.outputTexture = nil;

    if (output) {
        id<MTLComputePipelineState> sharpen = SharpenPipeline(buffer.device);
        id<MTLComputeCommandEncoder> compute = sharpen ? [buffer computeCommandEncoder] : nil;
        if (compute) {
            float amount = sSharpen;
            [compute setComputePipelineState:sharpen];
            [compute setTexture:output atIndex:0];
            [compute setTexture:destination atIndex:1];
            [compute setBytes:&amount length:sizeof amount atIndex:0];
            [compute dispatchThreads:MTLSizeMake(route.outW, route.outH, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
            [compute endEncoding];
        } else {
            Strike("the pass from MetalFX's output to the game's texture could not be encoded");
        }
    }
    tOwnEncoding = false;
    frame->scaled = true;
    // Its TAA was BG3's (the target is new, or was forgotten after a frame
    // that missed FSR), and it has now reached MetalFX down a proven route:
    // from the next frame the copy is drawn for it.
    if (!frame->replaced) KnowTarget(frame->taaTarget);

    // The handler holds the scaler and the textures until the GPU is done with them.
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (done.status == MTLCommandBufferStatusCompleted) {
            sStrikes.store(0, std::memory_order_relaxed);
            unsigned n = sScaled.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n == 1 || n % 3600 == 0) {
                LogInfo("[Gfx] gfx_temporal: MetalFX has scaled %u frames, BG3's FSR %u", n, sLeftToFsr.load(std::memory_order_relaxed));
            }
        } else {
            Strike(done.error ? done.error.localizedDescription.UTF8String : "the GPU did not finish the frame");
        }
        [scaler release];
        [destination release];
        [output release];
    }];
}

// FSR is about to be dispatched. True when it is taken over.
bool DispatchFsr(GfxEncoder* e)
{
    id<MTLTexture> input = e->computeTextures[0], scaled = e->computeTextures[1];
    BG3MSEGfxScene* scene = SceneOf(e->buffer, false);
    if (!scene || !input || !scaled || input != scene->tonemapTarget || !scene->tonemapSource) {
        LeftToFsr(!scene || !scene->depth ? "no scene depth was seen in its command buffer (is TAA on?)" : "FSR's input is not what FSR_Tonemap wrote");
        return false;
    }
    BG3MSEGfxFrame* frame = FrameSized(scene, scene->tonemapSource);
    if (!frame) {
        LeftToFsr("no TAA draw of FSR's render size was seen in its command buffer");
        return false;
    }

    os_unfair_lock_lock(&sLock);
    unsigned index = RouteFind(scaled);
    bool proven = index < kRoutes && sRoutes[index].scaler;
    bool refused = index < kRoutes && !sRoutes[index].scaler;
    bool fits = proven && scene->tonemapSource.width == sRoutes[index].inW && scene->tonemapSource.height == sRoutes[index].inH;
    os_unfair_lock_unlock(&sLock);

    e->fsr.frame = [frame retain];
    e->fsr.color = scene->tonemapSource;
    e->fsr.scaled = scaled;
    e->fsr.route = index;
    e->fsr.destinationSeen = false;
    if (refused) {
        e->fsr.state = kFsrNone;
        [e->fsr.frame release];
        e->fsr.frame = nil;
        LeftToFsr("these textures were looked at before and are not ones MetalFX can take");
        return false;
    }
    if (!fits || !Plain2D(e->fsr.color) || e->fsr.color.pixelFormat != kColorFormat) {
        // Not proven yet, or no longer what was proven: the game's FSR and RCAS run, watched.
        e->fsr.state = kFsrWatched;
        return false;
    }
    e->fsr.state = kFsrTakenOver;
    return true;
}

}

// --- What the hooks call ----------------------------------------------------------------------------

std::string TemporalUnavailable(id<MTLDevice> device)
{
    if (@available(macOS 13.0, *)) {
        // The framework is linked weakly: on a system without it the class is not there.
        if (!NSClassFromString(@"MTLFXTemporalScalerDescriptor")) return "MetalFX is not on this system";
        if (!device || ![MTLFXTemporalScalerDescriptor supportsDevice:device]) return "this GPU does not do MetalFX temporal scaling";
        return {};
    }
    return "MetalFX needs macOS 13 or later";
}

void TemporalStart(float sharpen)
{
    sSharpen = sharpen;
    sOn.store(true);
}

bool TemporalOn()
{
    return sOn.load(std::memory_order_acquire);
}

TemporalCounts TemporalStats()
{
    return { sScaled.load(), sLeftToFsr.load(), sRoutesProven.load(), sMissed.load(), sCopies.load(), TemporalOn() };
}

// What a pipeline's description says of its arguments, in the terms of GfxTemporalLogic.
std::string TemporalArgumentsProblem(uint8_t kindValue, NSArray* bindings)
{
    PipelineKind kind = (PipelineKind)kindValue;
    BindingSeen seen[32];
    MemberSeen members[32][24];
    uint32_t count = 0;
    for (id<MTLBinding> binding in bindings) {
        if (count == std::size(seen)) break;
        BindingSeen& s = seen[count];
        s = {};
        s.Name = binding.name.UTF8String;
        s.Index = (uint32_t)binding.index;
        s.Written = binding.access != MTLBindingAccessReadOnly;
        if (binding.type == MTLBindingTypeTexture) {
            s.Texture = true;
        } else if (binding.type == MTLBindingTypeBuffer) {
            id<MTLBufferBinding> buffer = (id<MTLBufferBinding>)binding;
            s.Bytes = (uint32_t)buffer.bufferDataSize;
            s.Members = members[count];
            for (MTLStructMember* member in buffer.bufferStructType.members) {
                if (s.MemberCount == std::size(members[count])) break;
                members[count][s.MemberCount++] = { member.name.UTF8String, (uint32_t)member.offset };
            }
        } else {
            continue;
        }
        count++;
    }
    return CheckBindings(kind, seen, count);
}

namespace {

uint32_t FactOrStop(PipelineKind kind, std::string const& problem)
{
    if (problem.empty()) return (uint32_t)kind << kFactKindShift;
    std::string why = std::string(PipelineKindName(kind)) + " is not the shader Temporal was written against: " + problem;
    Stop(why.c_str());
    return 0;
}

}

uint32_t TemporalRenderFact(id<MTLDevice> device, MTLRenderPipelineDescriptor* descriptor, MTLRenderPipelineReflection* reflection, id state)
{
    PipelineKind kind = ClassifyFragment(descriptor.fragmentFunction.name.UTF8String);
    if (kind == PipelineKind::None || !TemporalOn()) return 0;
    if (@available(macOS 13.0, *)) {
        uint32_t fact = FactOrStop(kind, TemporalArgumentsProblem((uint8_t)kind, reflection.fragmentBindings));
        if (!fact || kind != PipelineKind::Taa) return fact;
        if (!TakeCopy()) return fact;   // more TAA pipelines alive than expected: the later ones keep BG3's TAA

        // The pipeline drawn in PPTAA_PS's place: everything of the game's but the fragment function.
        MTLRenderPipelineDescriptor* copyDescriptor = [[descriptor copy] autorelease];
        copyDescriptor.fragmentFunction = [[ShaderLibrary(device) newFunctionWithName:@"bg3mse_taa_copy"] autorelease];
        NSError* error = nil;
        tOwnEncoding = true;
        id copy = copyDescriptor.fragmentFunction ? [device newRenderPipelineStateWithDescriptor:copyDescriptor error:&error] : nil;
        tOwnEncoding = false;
        if (!copy) {
            GiveCopyBack();
            Stop("the shader drawn in place of BG3's TAA could not be made into a pipeline with BG3's vertex function");
            return 0;
        }
        // The game has not been given `state` yet, so nothing lets go of it before this.
        BG3MSEGfxTaaCopy* held = [BG3MSEGfxTaaCopy new];
        if (!held) {
            [copy release];
            GiveCopyBack();
            return fact;
        }
        held->pipeline = copy;
        objc_setAssociatedObject(state, &sCopyKey, held, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [held release];
        return fact;
    }
    return 0;
}

uint32_t TemporalComputeFact(MTLComputePipelineReflection* reflection, id<MTLFunction> kernel)
{
    PipelineKind kind = ClassifyCompute(kernel.name.UTF8String);
    if (kind == PipelineKind::None || !TemporalOn()) return 0;
    if (@available(macOS 13.0, *)) return FactOrStop(kind, TemporalArgumentsProblem((uint8_t)kind, reflection.bindings));
    return 0;
}

bool TemporalBeforeDraw(id encoder, GfxEncoder* e)
{
    if (!TemporalOn()) return false;
    switch ((PipelineKind)((e->pipelineFact & kFactKindMask) >> kFactKindShift)) {
    case PipelineKind::Linearize: {
        id<MTLTexture> depth = e->fragment[0], target = e->colorTarget;
        if (depth && target && target.pixelFormat == MTLPixelFormatR32Float && depth.pixelFormat == kDepthFormat && SameSize(target, depth)) {
            Assign(SceneOf(e->buffer, true)->depth, depth);
        }
        return false;
    }
    case PipelineKind::Velocity: {
        uint8_t camera[kCameraConstantBytes];
        if (ConstantBytes(e->constants[kConstCamera], sizeof camera, camera)) {
            BG3MSEGfxScene* scene = SceneOf(e->buffer, true);
            memcpy(scene->camera, camera, sizeof camera);
            scene->hasCamera = true;
            os_unfair_lock_lock(&sLock);
            memcpy(sLastCamera, camera, sizeof camera);
            sHasLastCamera = true;
            os_unfair_lock_unlock(&sLock);
        }
        return false;
    }
    case PipelineKind::Taa:
        e->copyBound = DrawTaa(encoder, e);
        return e->copyBound;
    case PipelineKind::FsrTonemap: {
        id<MTLTexture> source = e->fragment[0], target = e->colorTarget;
        if (BG3MSEGfxScene* scene = source && target ? SceneOf(e->buffer, false) : nil) {
            Assign(scene->tonemapSource, source);
            Assign(scene->tonemapTarget, target);
        }
        return false;
    }
    default:
        return false;
    }
}

void TemporalAfterDraw(id encoder, GfxEncoder* e)
{
    if (!e->copyBound) return;
    e->copyBound = false;
    RawSetRenderPipeline(encoder, e->pipeline);
}

bool TemporalDispatch(GfxEncoder* e)
{
    PipelineKind kind = (PipelineKind)((e->pipelineFact & kFactKindMask) >> kFactKindShift);
    if (kind == PipelineKind::Fsr) {
        // A second FSR in one encoder is not a shape this knows: what was started is finished, and no more.
        if (e->fsr.state != kFsrNone || !TemporalOn()) return false;
        return DispatchFsr(e);
    }
    if (kind != PipelineKind::Rcas || e->fsr.state == kFsrNone || e->fsr.destinationSeen) return false;
    id<MTLTexture> input = e->computeTextures[0], destination = e->computeTextures[2];
    if (e->fsr.state == kFsrWatched) {
        if (input == e->fsr.scaled && destination) {
            e->fsr.destinationSeen = true;
            RouteWatched(e->buffer.device, e->fsr.frame, e->fsr.color, e->fsr.scaled, destination);
        }
        return false;
    }
    os_unfair_lock_lock(&sLock);
    bool same = e->fsr.route < kRoutes && sRoutes[e->fsr.route].scaled == e->fsr.scaled && sRoutes[e->fsr.route].destination == destination;
    os_unfair_lock_unlock(&sLock);
    if (input != e->fsr.scaled || !same) return false;      // RCAS runs; the ending sees that the route was left
    e->fsr.destinationSeen = true;
    return true;
}

void TemporalClear(GfxEncoder* e)
{
    [e->fsr.frame release];
    e->fsr.frame = nil;
    e->fsr.state = kFsrNone;
}

void TemporalTakeEnding(GfxEncoder* e, TemporalEnding* out)
{
    out->state = e->fsr.state;
    out->destinationSeen = e->fsr.destinationSeen;
    out->buffer = [e->buffer retain];
    out->frame = e->fsr.frame;      // the record's reference goes with it
    out->color = [e->fsr.color retain];
    out->scaled = [e->fsr.scaled retain];
    out->route = e->fsr.route;
    e->fsr.frame = nil;
    e->fsr.state = kFsrNone;
}

void TemporalFinishEnding(TemporalEnding* ending)
{
    BG3MSEGfxFrame* frame = ending->frame;
    if (ending->state == kFsrTakenOver) {
        if (ending->destinationSeen) {
            if (@available(macOS 13.0, *)) EncodeScaler(ending->buffer, ending->route, frame, ending->color);
        } else {
            // FSR was not dispatched and RCAS did not come as it did when the
            // route was proven: this frame shows what the scaled texture held.
            // The route is dropped, so the game's own passes run until one is proven again.
            os_unfair_lock_lock(&sLock);
            Route old{};
            if (ending->route < kRoutes && sRoutes[ending->route].scaled == ending->scaled) {
                old = sRoutes[ending->route];
                sRoutes[ending->route] = Route{};
            }
            JitterFollowsRoutes();
            os_unfair_lock_unlock(&sLock);
            RouteClear(old);
            ForgetTarget(frame->taaTarget);
            frame->scaled = true;       // accounted for here, not again when its scene goes
            Strike("FSR was taken over, and RCAS did not follow it with the textures it had before");
        }
    } else if (ending->state == kFsrWatched) {
        if (!ending->destinationSeen) LeftToFsr("RCAS did not follow FSR in its encoder: not a shape Temporal takes over");
        else LeftToFsr("watching BG3's own FSR and RCAS before taking over");
    }
    [ending->buffer release];
    [frame release];
    [ending->color release];
    [ending->scaled release];
}

}
