#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CAMetalLayer.h>

#include <Overlay/MetalOverlay.h>

#include <Overlay/FontAtlas.h>
#include <Platform/Log.h>

#include <backends/imgui_impl_metal.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <unistd.h>

#if !__has_feature(objc_arc)
#error "MetalOverlay.mm is written for automatic reference counting"
#endif

namespace bg3se::mac::overlay {

namespace {

std::atomic<int> gInFlight{ 0 };

// The device ImGui's renderer was made with, from its initialisation to its
// shutdown; ordered between the threads as gComposite is.
id<MTLDevice> gDevice{ nil };

// The converting pass for HDR targets (Overlay/ColourSpace.h): made with the
// renderer on the client thread, used on the main thread under the backend's
// lock, which also orders the two. The overlay's own texture is the main
// thread's: made there for the size of the drawable, and held, as every
// object a pass uses, by each buffer that has one until it completes.
struct Composite
{
    id<MTLRenderPipelineState> Bgra8{ nil };
    id<MTLRenderPipelineState> Bgr10a2{ nil };
    id<MTLTexture> Overlay{ nil };

    id<MTLRenderPipelineState> For(MTLPixelFormat format) const
    {
        return format == MTLPixelFormatBGR10A2Unorm ? Bgr10a2 : format == MTLPixelFormatBGRA8Unorm ? Bgra8 : nil;
    }
};
Composite gComposite;

id<MTLRenderPipelineState> MakeCompositePipeline(id<MTLDevice> device, id<MTLLibrary> library, MTLPixelFormat format, NSError** error)
{
    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.label = @"bg3mse overlay HDR";
    descriptor.vertexFunction = [library newFunctionWithName:@"composite_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"composite_fragment"];
    if (!descriptor.vertexFunction || !descriptor.fragmentFunction) return nil;
    // The shader blends itself: it reads the pixel it replaces.
    descriptor.colorAttachments[0].pixelFormat = format;
    descriptor.colorAttachments[0].blendingEnabled = NO;
    return [device newRenderPipelineStateWithDescriptor:descriptor error:error];
}

// The converting pass's pipeline states, or why there are none.
std::string MakeComposite(id<MTLDevice> device)
{
    gComposite = {};
    // Reading the target's pixel in the fragment function: every Apple GPU
    // can, no other one can.
    if (![device supportsFamily:MTLGPUFamilyApple1]) return "the GPU cannot read the target's pixels in a shader";
    NSError* error = nil;
    NSString* source = [NSString stringWithUTF8String:CompositeShaderSource().c_str()];
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (!library) return std::string("its shader does not compile: ") + (error ? error.localizedDescription.UTF8String : "no reason given");
    Composite made;
    made.Bgra8 = MakeCompositePipeline(device, library, MTLPixelFormatBGRA8Unorm, &error);
    made.Bgr10a2 = made.Bgra8 ? MakeCompositePipeline(device, library, MTLPixelFormatBGR10A2Unorm, &error) : nil;
    if (!made.Bgra8 || !made.Bgr10a2) {
        return std::string("its pipeline state cannot be made: ") + (error ? error.localizedDescription.UTF8String : "no reason given");
    }
    gComposite = made;
    return {};
}

// The overlay's own texture for a drawable of this size: the colour ImGui's
// renderer leaves when it draws onto nothing, premultiplied, 8 bits as the
// standard target has.
id<MTLTexture> OverlayTexture(id<MTLDevice> device, NSUInteger width, NSUInteger height)
{
    id<MTLTexture> current = gComposite.Overlay;
    if (current && current.device == device && current.width == width && current.height == height) return current;
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:width height:height mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    texture.label = @"bg3mse overlay HDR";
    // The one it replaces stays alive in the buffers that use it.
    gComposite.Overlay = texture;
    return texture;
}

// The colour space a layer's is, by the name Core Graphics gives it.
ColourSpace ColourSpaceOf(CGColorSpaceRef space)
{
    if (!space) return ColourSpace::Unknown;
    // The game sets the same named space each frame: one comparison then.
    static CGColorSpaceRef known = nullptr;
    static ColourSpace knownSpace = ColourSpace::Unknown;
    if (space == known) return knownSpace;

    ColourSpace found = ColourSpace::Unknown;
    if (CFStringRef name = CGColorSpaceCopyName(space)) {
        if (CFEqual(name, kCGColorSpaceSRGB)) found = ColourSpace::SRGB;
        else if (CFEqual(name, kCGColorSpaceDisplayP3)) found = ColourSpace::DisplayP3;
        else if (CFEqual(name, kCGColorSpaceITUR_2100_PQ)) found = ColourSpace::Bt2020Pq;
        else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            // The name the game asks for, and the other older one of the same space.
            if (CFEqual(name, kCGColorSpaceITUR_2020_PQ_EOTF) || CFEqual(name, kCGColorSpaceITUR_2020_PQ)) found = ColourSpace::Bt2020Pq;
#pragma clang diagnostic pop
        }
        CFRelease(name);
    }
    if (known) CGColorSpaceRelease(known);
    known = CGColorSpaceRetain(space);
    knownSpace = found;
    return found;
}

bool IsBgra(uint64_t format)
{
    return format == MTLPixelFormatBGRA8Unorm || format == MTLPixelFormatBGR10A2Unorm;
}

// Whether the texture's values are integers or a stencil: what a shader
// reads through another kind of texture argument than the renderer's.
bool NotFloat(MTLPixelFormat format)
{
    switch (format) {
    case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint:
    case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint:
    case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint:
    case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint:
    case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatRGB10A2Uint:
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint:
    case MTLPixelFormatStencil8: case MTLPixelFormatX32_Stencil8:
        return true;
    default:
        return false;
    }
}

// The id<MTLTexture> a draw command draws with, or nil: a texture of
// ImGui's own (by now made, when the frame brought it) or one that was
// registered. Not ImDrawCmd::GetTexID(), which asserts that a texture of
// ImGui's has been made: a command whose texture was destroyed under it is
// drawn with an empty rectangle and names none.
id<MTLTexture> TextureOf(ImDrawCmd const& command)
{
    ImTextureID named = command.TexRef._TexData ? command.TexRef._TexData->TexID : command.TexRef._TexID;
    return named != ImTextureID_Invalid ? (__bridge id<MTLTexture>)(void*)(intptr_t)named : nil;
}

// Whether the renderer's fragment shader can be given the texture: its
// argument is a texture2d<float>, and Metal leaves undefined what a draw does
// with a texture of another type there (an array, a cube, a multisampled
// one), with integer values, or one that may not be read by a shader.
bool Sampled2D(id<MTLTexture> texture)
{
    if (texture.textureType != MTLTextureType2D || texture.sampleCount > 1) return false;
    if (NotFloat(texture.pixelFormat)) return false;
    return texture.usage == MTLTextureUsageUnknown || (texture.usage & MTLTextureUsageShaderRead);
}

}

char const* Describe(TargetKind kind)
{
    switch (kind) {
    case TargetKind::Standard: return "standard dynamic range";
    case TargetKind::HighDynamicRange: return "high dynamic range";
    default: return "an unsupported format";
    }
}

TargetKind ClassifyTarget(uint64_t pixelFormat, ColourSpace space, bool extendedRange)
{
    // -[LSMTLView initWithFrame:device:display:hdr:vsync:] gives its layer
    // BGRA8Unorm, or BGR10A2Unorm for HDR; -[LSMTLView updateHDRState:], on
    // every frame, Display P3 without extended range, or BT.2020 PQ with it
    // when HDR is on and the screen can show more than its white.
    if (!IsBgra(pixelFormat)) return TargetKind::Unsupported;
    switch (space) {
    case ColourSpace::SRGB:
    case ColourSpace::DisplayP3:
        // Extended range on a BGRA layer of these is nothing the game asks for.
        return extendedRange ? TargetKind::Unsupported : TargetKind::Standard;
    case ColourSpace::Bt2020Pq:
        return extendedRange ? TargetKind::HighDynamicRange : TargetKind::Unsupported;
    default:
        return TargetKind::Unsupported;
    }
}

std::string Describe(TargetFormat const& format)
{
    char text[160];
    snprintf(text, sizeof(text), "MTLPixelFormat %llu, %s%s", (unsigned long long)format.PixelFormat, Describe(format.Space),
        format.ExtendedRange ? ", extended range" : "");
    return text;
}

TargetFormat TargetOf(void* drawablePointer)
{
    TargetFormat format;
    if (!drawablePointer) return format;
    id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)drawablePointer;
    id<MTLTexture> texture = drawable.texture;
    CAMetalLayer* layer = drawable.layer;
    format.PixelFormat = texture ? texture.pixelFormat : MTLPixelFormatInvalid;
    format.Space = layer ? ColourSpaceOf(layer.colorspace) : ColourSpace::Unknown;
    format.ExtendedRange = layer ? layer.wantsExtendedDynamicRangeContent : false;
    format.Kind = ClassifyTarget(format.PixelFormat, format.Space, format.ExtendedRange);
    return format;
}

std::string ColourSpaceNameOf(void* drawablePointer)
{
    if (!drawablePointer) return "no drawable";
    CAMetalLayer* layer = ((__bridge id<CAMetalDrawable>)drawablePointer).layer;
    if (!layer) return "no layer";
    CGColorSpaceRef space = layer.colorspace;
    if (!space) return "no colour space";
    CFStringRef name = CGColorSpaceCopyName(space);
    if (!name) return "a colour space without a name";
    std::string text = ((__bridge NSString*)name).UTF8String ?: "?";
    CFRelease(name);
    return text;
}

bool MetalInitialize(void* device)
{
    if (!device || !ImGui::GetCurrentContext()) return false;
    if (!ImGui_ImplMetal_Init((__bridge id<MTLDevice>)device)) return false;
    gDevice = (__bridge id<MTLDevice>)device;
    // The font atlas grows as glyphs are asked for, up to its own limit: no
    // further than a texture of the device can be.
    int limit = BoundFontAtlas(*ImGui::GetIO().Fonts, MetalMaxTextureSize(device));
    ImGui::GetPlatformIO().Renderer_TextureMaxWidth = ImGui::GetPlatformIO().Renderer_TextureMaxHeight = limit;
    std::string problem = MakeComposite((__bridge id<MTLDevice>)device);
    if (!problem.empty()) {
        Log(LogLevel::Error, "overlay: the pass that draws it on HDR output could not be made (%s): on HDR output it is not drawn", problem.c_str());
    }
    return true;
}

bool MetalDrawsHighDynamicRange()
{
    return gComposite.Bgra8 && gComposite.Bgr10a2;
}

int MetalMaxTextureSize(void* device)
{
    if (!device) return 0;
    id<MTLDevice> metal = (__bridge id<MTLDevice>)device;
    // The width and height a two-dimensional texture has at most: 16384 from
    // the third family of Apple's GPUs on, which every Mac that runs this
    // (arm64) code has; 8192 before it.
    return [metal supportsFamily:MTLGPUFamilyApple3] ? 16384 : 8192;
}

bool MetalCreateDeviceObjects(void* device)
{
    if (!device || !ImGui::GetCurrentContext() || !ImGui::GetIO().BackendRendererUserData) return false;
    // Creates the depth-stencil state and the samplers.
    return ImGui_ImplMetal_CreateDeviceObjects((__bridge id<MTLDevice>)device);
}

void MetalUpdateTexture(ImTextureData* texture)
{
    if (!texture || !ImGui::GetCurrentContext() || !ImGui::GetIO().BackendRendererUserData) return;
    ImGui_ImplMetal_UpdateTexture(texture);
}

bool MetalShutdown(uint32_t waitMilliseconds)
{
    if (!ImGui::GetCurrentContext() || !ImGui::GetIO().BackendRendererUserData) return true;
    // The pipeline states belong to the renderer, and a buffer still to run
    // uses them without holding them.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMilliseconds);
    while (gInFlight.load() > 0 && std::chrono::steady_clock::now() < deadline) usleep(1000);
    if (gInFlight.load() == 0) {
        ImGui_ImplMetal_Shutdown();
        gComposite = {};
        gDevice = nil;
        return true;
    }

    // A buffer has not run within the wait. Destroying what it uses is the
    // one thing that must not happen, so the renderer is not shut down:
    // ImGui is parted from it as a shutdown parts it, and its objects stay
    // where they are for good. Its textures are let go of as a shutdown lets
    // go of them (the buffer holds the ones it draws with), so that ImGui
    // asks the next renderer for them again.
    ImGuiIO& io = ImGui::GetIO();
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures) {
        if (texture->RefCount != 1 || !texture->BackendUserData) continue;
        texture->Status = ImTextureStatus_WantDestroy;
        if (texture->UnusedFrames < 1) texture->UnusedFrames = 1;
        ImGui_ImplMetal_UpdateTexture(texture);
    }
    io.BackendRendererUserData = nullptr;
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    ImGui::GetPlatformIO().ClearRendererHandlers();
    // What the buffer uses of the converting pass, it holds.
    gComposite = {};
    gDevice = nil;
    return false;
}

Encoded MetalEncode(ImDrawData* frame, void* commandBuffer, void* drawablePointer, float uiWhiteNits, TargetKind& target,
    std::string& problem)
{
    target = TargetKind::Unsupported;
    if (!frame || !commandBuffer || !drawablePointer) {
        problem = "no frame, command buffer or drawable";
        return Encoded::Failed;
    }
    if (!ImGui::GetCurrentContext() || !ImGui::GetIO().BackendRendererUserData) {
        problem = "ImGui's Metal renderer is not initialised";
        return Encoded::Failed;
    }
    id<MTLCommandBuffer> buffer = (__bridge id<MTLCommandBuffer>)commandBuffer;
    // The renderer's objects belong to one device. The game chooses its device
    // once (rf::metal::MetalAPI::Init) and gives it to its view's layer once
    // (-[LSMTLView initWithFrame:device:display:hdr:vsync:]), and an Apple
    // Silicon Mac has one GPU; a buffer of another device is not drawn into.
    if (buffer.device != gDevice) {
        problem = "the game's frame is drawn by another Metal device than the one the overlay was made for";
        return Encoded::Failed;
    }
    id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>)drawablePointer;
    id<MTLTexture> texture = drawable.texture;
    if (!texture) {
        problem = "the drawable has no texture";
        return Encoded::Failed;
    }
    target = TargetOf(drawablePointer).Kind;
    bool hdr = target == TargetKind::HighDynamicRange;
    if (target == TargetKind::Unsupported) return Encoded::NotDrawable;
    id<MTLRenderPipelineState> composite = hdr ? gComposite.For(texture.pixelFormat) : nil;
    if (hdr && (!composite || !UsablePaperWhite(uiWhiteNits))) return Encoded::NotDrawable;

    if (frame->CmdLists.Size == 0) return Encoded::NothingToDraw;
    // The viewport and every scissor rectangle of the frame are in the pixels
    // of the drawable it was built for.
    if ((NSUInteger)frame->DisplaySize.x != texture.width || (NSUInteger)frame->DisplaySize.y != texture.height) return Encoded::StaleSize;

    // On an HDR target ImGui draws into the overlay's own texture, onto nothing.
    id<MTLTexture> overlay = hdr ? OverlayTexture(buffer.device, texture.width, texture.height) : nil;
    if (hdr && !overlay) {
        problem = "no texture could be made for the overlay on HDR output";
        return Encoded::Failed;
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = hdr ? overlay : texture;
    pass.colorAttachments[0].loadAction = hdr ? MTLLoadActionClear : MTLLoadActionLoad;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
        problem = "the command buffer gave no render encoder";
        return Encoded::Failed;
    }
    encoder.label = @"bg3mse overlay";

    // The textures the frame brings, brought up to date before the pass is
    // encoded: the renderer does the same when it draws, and then it is too
    // late to hold a texture that was made there.
    if (frame->Textures) {
        for (ImTextureData* brought : *frame->Textures) {
            if (brought->Status != ImTextureStatus_OK) ImGui_ImplMetal_UpdateTexture(brought);
        }
    }

    // The textures the frame's commands name: held until the buffer completes.
    NSMutableArray* held = [NSMutableArray array];
    for (ImDrawList const* list : frame->CmdLists) {
        for (ImDrawCmd const& command : list->CmdBuffer) {
            id<MTLTexture> used = TextureOf(command);
            if (used && ![held containsObject:used]) [held addObject:used];
        }
    }

    // Tells the renderer the target's format (it keeps a pipeline for each),
    // then draws.
    ImGui_ImplMetal_NewFrame(pass);
    ImGui_ImplMetal_RenderDrawData(frame, buffer, encoder);
    [encoder endEncoding];

    if (hdr) {
        // The overlay over the game's pixels, converted.
        MTLRenderPassDescriptor* over = [MTLRenderPassDescriptor renderPassDescriptor];
        over.colorAttachments[0].texture = texture;
        over.colorAttachments[0].loadAction = MTLLoadActionLoad;
        over.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> converting = [buffer renderCommandEncoderWithDescriptor:over];
        if (!converting) {
            // What ImGui drew went to the overlay's texture only: the target is as it was.
            problem = "the command buffer gave no render encoder for the pass onto HDR output";
            gInFlight.fetch_add(1);
            [buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                (void)held;
                (void)overlay;
                gInFlight.fetch_sub(1);
            }];
            return Encoded::Failed;
        }
        converting.label = @"bg3mse overlay HDR";
        float white = (float)(uiWhiteNits / kPqPeakNits);
        [converting setRenderPipelineState:composite];
        [converting setFragmentTexture:overlay atIndex:0];
        [converting setFragmentBytes:&white length:sizeof(white) atIndex:0];
        [converting drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [converting endEncoding];
        [held addObject:overlay];
        [held addObject:composite];
    }

    gInFlight.fetch_add(1);
    [buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
        (void)held;
        (void)drawable;
        gInFlight.fetch_sub(1);
    }];
    return Encoded::Drawn;
}

int MetalBuffersInFlight()
{
    return gInFlight.load();
}

void* MetalRetainTexture(void const* gameTexture, uint32_t offset, uint32_t& width, uint32_t& height)
{
    width = height = 0;
    if (!gameTexture) return nullptr;
    void* held = *reinterpret_cast<void* const*>(static_cast<uint8_t const*>(gameTexture) + offset);
    if (!held) return nullptr;
    id object = (__bridge id)held;
    if (![object conformsToProtocol:@protocol(MTLTexture)]) return nullptr;
    id<MTLTexture> texture = (id<MTLTexture>)object;
    if (!Sampled2D(texture)) return nullptr;
    width = (uint32_t)texture.width;
    height = (uint32_t)texture.height;
    return (void*)CFBridgingRetain(texture);
}

void MetalReleaseTexture(void* texture)
{
    if (texture) CFRelease((CFTypeRef)texture);
}

}
