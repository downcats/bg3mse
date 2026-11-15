// See ShaderInterface.h.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <atomic>

#include <Shaders/ShaderInterface.h>

#if !__has_feature(objc_arc)
#error "MetalFunctions.mm is built with ARC"
#endif

// The argument reflection that exists on the game's minimum macOS.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace bg3se::mac {

namespace {

std::string ScalarType(MTLDataType type)
{
    struct Row { MTLDataType First; char Letter; };
    // Each row is a scalar followed by its 2, 3 and 4 vectors.
    static constexpr Row rows[] = {
        { MTLDataTypeFloat, 'f' }, { MTLDataTypeHalf, 'h' }, { MTLDataTypeInt, 'i' }, { MTLDataTypeUInt, 'u' },
        { MTLDataTypeShort, 'i' }, { MTLDataTypeUShort, 'u' }, { MTLDataTypeChar, 'i' }, { MTLDataTypeUChar, 'u' },
        { MTLDataTypeBool, 'b' },
    };
    for (auto const& row : rows) {
        if (type >= row.First && type < row.First + 4) return std::string(1, row.Letter) + std::to_string(type - row.First + 1);
    }
    // Float2x2 ... Float4x4, columns by rows.
    if (type >= MTLDataTypeFloat2x2 && type <= MTLDataTypeFloat4x4) {
        NSUInteger n = type - MTLDataTypeFloat2x2;
        return "f" + std::to_string((n / 3 + 2) * (n % 3 + 2));
    }
    if (type == MTLDataTypeStruct) return "s1";
    return "?" + std::to_string((unsigned)type);
}

std::string MemberType(MTLStructMember* member)
{
    if (member.dataType != MTLDataTypeArray) return ScalarType(member.dataType);
    std::string dims;
    MTLArrayType* array = member.arrayType;
    while (true) {
        dims += "[" + std::to_string(array.arrayLength) + "]";
        if (array.elementType != MTLDataTypeArray) break;
        array = array.elementArrayType;
    }
    return ScalarType(array.elementType) + dims;
}

std::string TextureType(MTLTextureType type)
{
    switch (type) {
    case MTLTextureType1D: return "1d";
    case MTLTextureType1DArray: return "1darray";
    case MTLTextureType2D: return "2d";
    case MTLTextureType2DArray: return "2darray";
    case MTLTextureType2DMultisample: return "2dms";
    case MTLTextureTypeCube: return "cube";
    case MTLTextureTypeCubeArray: return "cubearray";
    case MTLTextureType3D: return "3d";
    default: return "?";
    }
}

std::vector<MacResource> Arguments(NSArray<MTLArgument*>* arguments)
{
    std::vector<MacResource> out;
    for (MTLArgument* a in arguments) {
        MacResource r;
        r.Name = a.name.UTF8String;
        r.Index = (uint32_t)a.index;
        r.Used = a.active;
        switch (a.type) {
        case MTLArgumentTypeBuffer:
            r.Kind = ResourceKind::Buffer;
            r.Size = (uint32_t)a.bufferDataSize;
            r.Type = a.bufferStructType ? "block" : "bytes";
            for (MTLStructMember* m in a.bufferStructType.members) {
                r.Members.push_back({ m.name.UTF8String, (uint32_t)m.offset, MemberType(m) });
            }
            break;
        case MTLArgumentTypeTexture:
            r.Kind = ResourceKind::Texture;
            r.Type = TextureType(a.textureType);
            r.Depth = a.isDepthTexture;
            r.Written = a.access != MTLArgumentAccessReadOnly;
            r.Read = a.access != MTLArgumentAccessWriteOnly;
            if (a.arrayLength > 1) {
                // Several at one place: how many.
                r.Type += "[]";
                r.Size = (uint32_t)a.arrayLength;
            }
            break;
        case MTLArgumentTypeSampler:
            r.Kind = ResourceKind::Sampler;
            break;
        default:
            continue;
        }
        out.push_back(std::move(r));
    }
    return out;
}

}

struct MetalFunctions::Impl
{
    id<MTLDevice> Device;
    NSMutableArray<id<MTLLibrary>>* Libraries;
    // The pipelines Draw has made, by what they were made of: the same pair is
    // drawn many times with other numbers, and making one is most of a draw.
    NSMutableDictionary<NSString*, id<MTLRenderPipelineState>>* Pipelines;
    NSMutableDictionary<NSString*, NSNumber*>* PipelineFormats;
    // Which set of functions this is, of all that were ever made: what a kept
    // pipeline is known by, for an address is given again when a set is gone.
    uint64_t Serial{ NextSerial() };
    static uint64_t NextSerial() { static std::atomic<uint64_t> next{ 1 }; return next++; }
};

MetalFunctions::MetalFunctions() : impl_(std::make_unique<Impl>()) {}
MetalFunctions::~MetalFunctions() = default;

std::unique_ptr<MetalFunctions> MetalFunctions::OpenGame(std::string const& appBundle, std::string& why)
{
    @autoreleasepool {
        std::unique_ptr<MetalFunctions> out(new MetalFunctions());
        out->impl_->Device = MTLCreateSystemDefaultDevice();
        out->impl_->Libraries = [NSMutableArray array];
        if (!out->impl_->Device) {
            why = "no Metal device";
            return nullptr;
        }
        // The order rf::GPUDevice::CreateShader asks them in.
        for (char const* name : { "Core.metallib", "Materials.metallib" }) {
            std::string path = appBundle + "/Contents/Data/Shaders/Metal/" + name;
            NSError* error = nil;
            id<MTLLibrary> library = [out->impl_->Device newLibraryWithURL:[NSURL fileURLWithPath:@(path.c_str())] error:&error];
            if (!library) {
                why = "cannot open " + path + ": " + (error.localizedDescription.UTF8String ?: "");
                return nullptr;
            }
            [out->impl_->Libraries addObject:library];
        }
        return out;
    }
}

std::unique_ptr<MetalFunctions> MetalFunctions::Compile(std::span<std::string const> sources, std::string& why)
{
    @autoreleasepool {
        std::unique_ptr<MetalFunctions> out(new MetalFunctions());
        out->impl_->Device = MTLCreateSystemDefaultDevice();
        out->impl_->Libraries = [NSMutableArray array];
        if (!out->impl_->Device) {
            why = "no Metal device";
            return nullptr;
        }
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion2_3;
        // As the game's own vertex functions were compiled, and as the translated ones are for the game.
        options.preserveInvariance = YES;
        // One library per source: each is a whole translation unit.
        for (auto const& source : sources) {
            NSError* error = nil;
            if (char const* math = getenv("BG3MSE_SHADERS_MATH"); math && source.find("\nvertex ") != std::string::npos) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                std::string mode = math;
                options = [MTLCompileOptions new];
                options.languageVersion = MTLLanguageVersion2_3;
                if (mode == "nofast") options.fastMathEnabled = NO;
                if (mode == "nofast-invariant") { options.fastMathEnabled = NO; options.preserveInvariance = YES; }
                if (@available(macOS 15.0, *)) {
                    if (mode == "safe") options.mathMode = MTLMathModeSafe;
                    if (mode == "relaxed") options.mathMode = MTLMathModeRelaxed;
                    if (mode == "relaxed-precise") { options.mathMode = MTLMathModeRelaxed; options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise; }
                    if (mode == "safe-fast") { options.mathMode = MTLMathModeSafe; options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsFast; }
                }
#pragma clang diagnostic pop
            } else if (getenv("BG3MSE_SHADERS_MATH")) {
                options = [MTLCompileOptions new];
                options.languageVersion = MTLLanguageVersion2_3;
            }
            id<MTLLibrary> library = [out->impl_->Device newLibraryWithSource:@(source.c_str()) options:options error:&error];
            if (!library) {
                why = std::string("the Metal compiler refused the source: ") + (error.localizedDescription.UTF8String ?: "");
                return nullptr;
            }
            [out->impl_->Libraries addObject:library];
        }
        return out;
    }
}

bool MetalFunctions::Has(std::string const& function) const
{
    @autoreleasepool {
        for (id<MTLLibrary> library in impl_->Libraries) {
            if ([library newFunctionWithName:@(function.c_str())]) return true;
        }
        return false;
    }
}

bool MetalFunctions::Reflect(std::string const& vertexName, std::string const& fragmentName, MacPass& pass, std::string& why) const
{
    @autoreleasepool {
        id<MTLFunction> vertex = nil, fragment = nil;
        for (id<MTLLibrary> library in impl_->Libraries) {
            if (!vertex) vertex = [library newFunctionWithName:@(vertexName.c_str())];
            if (!fragment && !fragmentName.empty()) fragment = [library newFunctionWithName:@(fragmentName.c_str())];
        }
        if (!vertex || vertex.functionType != MTLFunctionTypeVertex) {
            why = "no vertex function named " + vertexName;
            return false;
        }
        if (!fragmentName.empty() && (!fragment || fragment.functionType != MTLFunctionTypeFragment)) {
            why = "no fragment function named " + fragmentName;
            return false;
        }

        // What a function takes is told through a pipeline made with it. What
        // the attributes hold does not matter for that, only that each has a
        // format of its kind.
        pass = {};
        MTLVertexDescriptor* layout = [MTLVertexDescriptor vertexDescriptor];
        NSUInteger offset = 0;
        for (MTLVertexAttribute* a in vertex.vertexAttributes) {
            std::string type = ScalarType(a.attributeType);
            // One the function does not read is not one it takes.
            if (a.active) pass.Attributes.push_back({ a.name.UTF8String, (uint32_t)a.attributeIndex, type });
            MTLVertexFormat format = type[0] == 'i' ? MTLVertexFormatInt4 : type[0] == 'u' ? MTLVertexFormatUInt4 : MTLVertexFormatFloat4;
            layout.attributes[a.attributeIndex].format = format;
            layout.attributes[a.attributeIndex].offset = offset;
            layout.attributes[a.attributeIndex].bufferIndex = 30;
            offset += 16;
        }
        layout.layouts[30].stride = offset ? offset : 16;

        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        if (vertex.vertexAttributes.count) descriptor.vertexDescriptor = layout;
        descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

        // Which attachments a pass writes, and as what, is not known here.
        struct Targets { MTLPixelFormat Format; int Count; };
        static constexpr Targets tries[] = {
            { MTLPixelFormatRGBA16Float, 8 }, { MTLPixelFormatRGBA16Float, 1 }, { MTLPixelFormatInvalid, 0 }, { MTLPixelFormatRGBA8Uint, 1 },
        };
        NSError* error = nil;
        for (auto const& targets : tries) {
            for (int i = 0; i < 8; i++) {
                descriptor.colorAttachments[i].pixelFormat = i < targets.Count ? targets.Format : MTLPixelFormatInvalid;
            }
            MTLRenderPipelineReflection* reflection = nil;
            id<MTLRenderPipelineState> state = [impl_->Device newRenderPipelineStateWithDescriptor:descriptor
                                                                                           options:MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo
                                                                                        reflection:&reflection
                                                                                             error:&error];
            if (!state || !reflection) continue;
            pass.Vertex = Arguments(reflection.vertexArguments);
            pass.Fragment = Arguments(reflection.fragmentArguments);
            return true;
        }
        why = std::string("no pipeline can be made of the functions: ") + (error.localizedDescription.UTF8String ?: "");
        return false;
    }
}

namespace {

// A stream of numbers that depends on nothing but its seed.
struct Noise
{
    uint64_t State;
    explicit Noise(uint64_t seed) : State(seed * 0x9e3779b97f4a7c15ull + 0x632be59bd9b4e019ull) {}
    uint32_t Next()
    {
        State ^= State >> 12; State ^= State << 25; State ^= State >> 27;
        return (uint32_t)((State * 0x2545f4914f6cdd1dull) >> 32);
    }
    float Unit() { return (float)(Next() >> 8) / 16777216.0f; }
};

id<MTLTexture> NoiseTexture(id<MTLDevice> device, DrawSetup::Texture const& t)
{
    MTLTextureDescriptor* d = [MTLTextureDescriptor new];
    bool cube = t.Type.rfind("cube", 0) == 0, volume = t.Type == "3d", array = t.Type.find("array") != std::string::npos;
    NSUInteger size = t.Written ? DrawResult::kSize : DrawSetup::kTextureAcross, layers = array ? 4 : 1;
    if (t.Type.rfind("1d", 0) == 0) d.textureType = array ? MTLTextureType1DArray : MTLTextureType1D;
    else if (cube) d.textureType = array ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
    else if (volume) d.textureType = MTLTextureType3D;
    else if (t.Type.rfind("2d", 0) == 0 && t.Type.find("ms") == std::string::npos) d.textureType = array ? MTLTextureType2DArray : MTLTextureType2D;
    else return nil;
    d.pixelFormat = t.Depth ? MTLPixelFormatDepth32Float : MTLPixelFormatRGBA8Unorm;
    d.width = size;
    d.height = d.textureType == MTLTextureType1D || d.textureType == MTLTextureType1DArray ? 1 : size;
    d.depth = volume ? size : 1;
    d.arrayLength = layers;
    NSUInteger levels = t.Levels && !t.Depth && !volume ? 4 : 1;        // 8, 4, 2 and 1 across
    d.mipmapLevelCount = levels;
    d.usage = t.Depth ? MTLTextureUsageShaderRead : MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:d];
    if (!texture) return texture;

    Noise noise(t.Noise);
    if (t.Depth) {
        // A depth texture holds depths of chance between none and all, so that a function that compares against
        // it (a shadow map) finds some of what it asks about in the light and some in the shade.
        std::vector<float> depths(size * size * (volume ? size : 1));
        NSUInteger slices = layers * (cube ? 6 : 1);
        for (NSUInteger slice = 0; slice < slices; slice++) {
            for (auto& depth : depths) depth = (float)(noise.Next() >> 8) / 16777216.0f;
            for (NSUInteger texel = 0, half = size / 2; t.Quarters && !volume && texel < size * size; texel++) {
                depths[texel] = depths[(texel / size / half * half) * size + texel % size / half * half];
            }
            [texture replaceRegion:MTLRegionMake3D(0, 0, 0, d.width, d.height, d.depth) mipmapLevel:0 slice:slice withBytes:depths.data()
                       bytesPerRow:d.width * 4 bytesPerImage:d.width * d.height * 4];
        }
        return texture;
    }
    std::vector<uint8_t> allowed;
    for (int v = 0; t.LowBits >= 0 && v < 256; v++) {
        float f = (float)v / 255.0f;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        if ((int)(bits & 15) <= t.LowBits) allowed.push_back((uint8_t)v);
    }
    std::vector<uint8_t> pixels(size * size * (volume ? size : 1) * 4);
    NSUInteger slices = layers * (cube ? 6 : 1);
    for (NSUInteger slice = 0; slice < slices; slice++) {
        for (auto& b : pixels) {
            b = allowed.empty() ? (uint8_t)(noise.Next() >> 24) : allowed[(noise.Next() >> 24) % allowed.size()];
            if (t.AtLeast > 0 && b < t.AtLeast) b = (uint8_t)(t.AtLeast + b % (256 - t.AtLeast));
        }
        for (size_t texel = 0; t.Sparse && texel + 4 <= pixels.size(); texel += 4) {
            if ((noise.Next() >> 24) & 3) memset(&pixels[texel], 0, 4);
        }
        for (size_t texel = 4; t.Flat && texel + 4 <= pixels.size(); texel += 4) memcpy(&pixels[texel], &pixels[0], 4);
        [texture replaceRegion:MTLRegionMake3D(0, 0, 0, d.width, d.height, d.depth) mipmapLevel:0 slice:slice withBytes:pixels.data()
                   bytesPerRow:d.width * 4 bytesPerImage:d.width * d.height * 4];
    }
    // The smaller levels, each numbers of its own.
    for (NSUInteger level = 1; level < levels; level++) {
        NSUInteger across = size >> level;
        for (NSUInteger slice = 0; slice < slices; slice++) {
            for (size_t i = 0; i < across * across * 4; i++) pixels[i] = (uint8_t)(noise.Next() >> 24);
            [texture replaceRegion:MTLRegionMake3D(0, 0, 0, across, across, 1) mipmapLevel:level slice:slice withBytes:pixels.data()
                       bytesPerRow:across * 4 bytesPerImage:across * across * 4];
        }
    }
    return texture;
}

}

bool MetalFunctions::Draw(std::string const& vertexName, std::string const& fragmentName, DrawSetup const& setup, DrawResult& result,
    std::string& why, MetalFunctions const* fragmentFrom) const
{
    @autoreleasepool {
        id<MTLDevice> device = impl_->Device;
        id<MTLFunction> vertex = nil, fragment = nil;
        for (id<MTLLibrary> library in impl_->Libraries) {
            if (!vertex) vertex = [library newFunctionWithName:@(vertexName.c_str())];
            if (!fragment && !fragmentName.empty() && !fragmentFrom) fragment = [library newFunctionWithName:@(fragmentName.c_str())];
        }
        if (fragmentFrom && !fragmentName.empty()) {
            for (id<MTLLibrary> library in fragmentFrom->impl_->Libraries) {
                if (!fragment) fragment = [library newFunctionWithName:@(fragmentName.c_str())];
            }
        }
        if (!vertex || (!fragmentName.empty() && !fragment)) {
            why = "the functions are not there";
            return false;
        }

        // The vertices: four values per attribute, the same for every pair of functions.
        constexpr NSUInteger kStride = 32 * 16;
        std::vector<uint8_t> vertices(setup.Vertices * kStride);
        MTLVertexDescriptor* layout = [MTLVertexDescriptor vertexDescriptor];
        for (auto const& a : setup.Attributes) {
            if (a.Index >= 31) { why = "an attribute index past 30"; return false; }
            layout.attributes[a.Index].format = a.Scalar == 'i' ? MTLVertexFormatInt4 : a.Scalar == 'u' ? MTLVertexFormatUInt4 : MTLVertexFormatFloat4;
            layout.attributes[a.Index].offset = a.Index * 16;
            layout.attributes[a.Index].bufferIndex = setup.VertexBuffer;
            Noise noise(setup.Seed * 977 + a.Index + a.Noise);
            for (uint32_t v = 0; v < setup.Vertices; v++) {
                for (int c = 0; c < 4; c++) {
                    auto at = vertices.data() + v * kStride + a.Index * 16 + c * 4;
                    if (a.Scalar == 'f') {
                        float value = noise.Unit() * 2 - 1;
                        memcpy(at, &value, 4);
                    } else {
                        uint32_t value = noise.Next() & 3;
                        memcpy(at, &value, 4);
                    }
                }
            }
        }
        layout.layouts[setup.VertexBuffer].stride = kStride;

        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        if (vertex.vertexAttributes.count) descriptor.vertexDescriptor = layout;
        descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        // A function that writes fewer components than a target has cannot
        // draw to it: the targets are as wide as the functions allow.
        NSError* error = nil;
        std::string made = vertexName + "|" + fragmentName + "|" + std::to_string(fragmentFrom ? fragmentFrom->impl_->Serial : 0) + "|" + std::to_string(setup.VertexBuffer);
        for (auto const& a : setup.Attributes) made += "|" + std::to_string(a.Index) + a.Scalar;
        NSString* key = @(made.c_str());
        if (!impl_->Pipelines) {
            impl_->Pipelines = [NSMutableDictionary dictionary];
            impl_->PipelineFormats = [NSMutableDictionary dictionary];
        }
        id<MTLRenderPipelineState> pipeline = impl_->Pipelines[key];
        MTLPixelFormat format = pipeline ? (MTLPixelFormat)impl_->PipelineFormats[key].unsignedIntegerValue : MTLPixelFormatRGBA32Float;
        for (MTLPixelFormat tried : { MTLPixelFormatRGBA32Float, MTLPixelFormatRG32Float, MTLPixelFormatR32Float }) {
            if (pipeline) break;
            format = tried;
            for (uint32_t i = 0; i < DrawResult::kTargets; i++) descriptor.colorAttachments[i].pixelFormat = format;
            pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        }
        if (!pipeline) {
            why = std::string("no pipeline: ") + (error.localizedDescription.UTF8String ?: "");
            return false;
        }
        // Kept while there are few: a run over every pass of the game would keep them all.
        if (impl_->Pipelines.count > 64) {
            [impl_->Pipelines removeAllObjects];
            [impl_->PipelineFormats removeAllObjects];
        }
        impl_->Pipelines[key] = pipeline;
        impl_->PipelineFormats[key] = @((NSUInteger)format);

        NSUInteger channels = format == MTLPixelFormatRGBA32Float ? 4 : format == MTLPixelFormatRG32Float ? 2 : 1;
        MTLTextureDescriptor* target = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                          width:DrawResult::kSize height:DrawResult::kSize mipmapped:NO];
        target.usage = MTLTextureUsageRenderTarget;
        target.storageMode = MTLStorageModeShared;
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        NSMutableArray<id<MTLTexture>>* colors = [NSMutableArray array];
        for (uint32_t i = 0; i < DrawResult::kTargets; i++) {
            id<MTLTexture> color = [device newTextureWithDescriptor:target];
            [colors addObject:color];
            pass.colorAttachments[i].texture = color;
            pass.colorAttachments[i].loadAction = MTLLoadActionClear;
            pass.colorAttachments[i].storeAction = MTLStoreActionStore;
            pass.colorAttachments[i].clearColor = MTLClearColorMake(kClearColor, kClearColor, kClearColor, kClearColor);
        }
        target.pixelFormat = MTLPixelFormatDepth32Float;
        id<MTLTexture> depth = [device newTextureWithDescriptor:target];
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = 1.0;

        MTLDepthStencilDescriptor* depthState = [MTLDepthStencilDescriptor new];
        depthState.depthCompareFunction = MTLCompareFunctionLess;
        depthState.depthWriteEnabled = YES;
        MTLSamplerDescriptor* samplerDescriptor = [MTLSamplerDescriptor new];
        samplerDescriptor.minFilter = samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
        // Between a texture's levels too: without it every sample is of the largest level, whatever level the
        // function asks for, and a texture's smaller levels show in no picture.
        samplerDescriptor.mipFilter = MTLSamplerMipFilterLinear;
        samplerDescriptor.sAddressMode = samplerDescriptor.tAddressMode = samplerDescriptor.rAddressMode = MTLSamplerAddressModeRepeat;
        // Each compares as a shadow map's sampler does when a function asks it to compare (the function says which
        // of the two it wants, the sampler only how); one that never passes would put everything in the shade.
        samplerDescriptor.compareFunction = MTLCompareFunctionLessEqual;
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDescriptor];
        // Three more: clamped, mirrored and not filtered, mirrored.
        id<MTLSamplerState> samplers[4] = { sampler, nil, nil, nil };
        samplerDescriptor.sAddressMode = samplerDescriptor.tAddressMode = samplerDescriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
        samplers[1] = [device newSamplerStateWithDescriptor:samplerDescriptor];
        samplerDescriptor.sAddressMode = samplerDescriptor.tAddressMode = samplerDescriptor.rAddressMode = MTLSamplerAddressModeMirrorRepeat;
        samplers[3] = [device newSamplerStateWithDescriptor:samplerDescriptor];
        samplerDescriptor.minFilter = samplerDescriptor.magFilter = MTLSamplerMinMagFilterNearest;
        samplers[2] = [device newSamplerStateWithDescriptor:samplerDescriptor];

        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setDepthStencilState:[device newDepthStencilStateWithDescriptor:depthState]];
        [encoder setCullMode:MTLCullModeNone];
        for (auto const& b : setup.Buffers) {
            if (b.Index >= 31 || (b.Vertex && b.Index == setup.VertexBuffer) || b.Bytes.empty()) continue;
            id<MTLBuffer> buffer = [device newBufferWithBytes:b.Bytes.data() length:b.Bytes.size() options:MTLResourceStorageModeShared];
            if (b.Vertex) [encoder setVertexBuffer:buffer offset:0 atIndex:b.Index];
            else [encoder setFragmentBuffer:buffer offset:0 atIndex:b.Index];
        }
        if (setup.MatrixRows[0] >= 0 && setup.MatrixRows[1] >= 0 && setup.MatrixRows[2] >= 0) {
            Noise noise(setup.Seed * 7919 + 13);
            for (uint32_t v = 0; v < setup.Vertices; v++) {
                // A turn from four numbers of chance made a unit quaternion, a scale between a half and one and a half.
                float q[4], length = 0;
                for (float& c : q) { c = noise.Unit() * 2 - 1; length += c * c; }
                length = sqrtf(length > 1e-6f ? length : 1e-6f);
                for (float& c : q) c /= length;
                float scale = 0.5f + noise.Unit();
                float x = q[0], y = q[1], z = q[2], w = q[3];
                float turn[3][3] = {
                    { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
                    { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
                    { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) },
                };
                for (int row = 0; row < 3; row++) {
                    float values[4] = { scale * turn[row][0], scale * turn[row][1], scale * turn[row][2], noise.Unit() * 2 - 1 };
                    memcpy(vertices.data() + v * kStride + (NSUInteger)setup.MatrixRows[row] * 16, values, 16);
                }
            }
        }
        id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:vertices.data() length:vertices.size() options:MTLResourceStorageModeShared];
        [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:setup.VertexBuffer];
        std::vector<std::pair<uint32_t, id<MTLTexture>>> written;
        for (auto const& t : setup.Textures) {
            id<MTLTexture> texture = NoiseTexture(device, t);
            if (!texture) { [encoder endEncoding]; why = "a texture of type " + t.Type + " cannot be made"; return false; }
            if (t.Vertex) [encoder setVertexTexture:texture atIndex:t.Index];
            else [encoder setFragmentTexture:texture atIndex:t.Index];
            if (texture.textureType == MTLTextureType2D && !t.Depth) {
                auto& before = result.Textures[t.Noise].Before;
                before.resize(texture.width * texture.height * 4);
                [texture getBytes:before.data() bytesPerRow:texture.width * 4 fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height) mipmapLevel:0];
                written.emplace_back(t.Noise, texture);
            }
        }
        for (auto const& s : setup.Samplers) {
            if (s.Vertex) [encoder setVertexSamplerState:samplers[s.Which % 4] atIndex:s.Index];
            else [encoder setFragmentSamplerState:samplers[s.Which % 4] atIndex:s.Index];
        }
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:setup.Vertices];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted) {
            why = std::string("the draw did not complete: ") + (commands.error.localizedDescription.UTF8String ?: "");
            return false;
        }

        NSUInteger pixels = DrawResult::kSize * DrawResult::kSize;
        for (uint32_t i = 0; i < DrawResult::kTargets; i++) {
            std::vector<float> read(pixels * channels);
            [colors[i] getBytes:read.data() bytesPerRow:DrawResult::kSize * channels * 4
                     fromRegion:MTLRegionMake2D(0, 0, DrawResult::kSize, DrawResult::kSize) mipmapLevel:0];
            // The channels a target does not have read as cleared.
            result.Color[i].assign(pixels * 4, kClearColor);
            for (NSUInteger p = 0; p < pixels; p++) {
                for (NSUInteger c = 0; c < channels; c++) result.Color[i][p * 4 + c] = read[p * channels + c];
            }
        }
        result.Depth.resize(pixels);
        [depth getBytes:result.Depth.data() bytesPerRow:DrawResult::kSize * 4 fromRegion:MTLRegionMake2D(0, 0, DrawResult::kSize, DrawResult::kSize)
            mipmapLevel:0];
        for (auto const& [noise, texture] : written) {
            auto& bytes = result.Textures[noise].After;
            bytes.resize(texture.width * texture.height * 4);
            [texture getBytes:bytes.data() bytesPerRow:texture.width * 4 fromRegion:MTLRegionMake2D(0, 0, texture.width, texture.height) mipmapLevel:0];
        }
        return true;
    }
}

}
