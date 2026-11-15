// See GfxShaderCheck.h.

#import "GfxMetalInternal.h"

#include <Gfx/GfxShaderCheck.h>
#include <Gfx/GfxTemporalLogic.h>

#if __has_feature(objc_arc)
#error "GfxShaderCheck.mm uses manual retain/release"
#endif

namespace bg3se::mac::gfx {

namespace {

// A vertex descriptor that feeds `vertex` its attributes from one buffer: what
// the attributes hold does not matter here, only that the pipeline can be made.
MTLVertexDescriptor* DescriptorFor(id<MTLFunction> vertex)
{
    if (!vertex.vertexAttributes.count) return nil;
    MTLVertexDescriptor* descriptor = [MTLVertexDescriptor vertexDescriptor];
    NSUInteger offset = 0;
    for (MTLVertexAttribute* attribute in vertex.vertexAttributes) {
        MTLVertexFormat format = MTLVertexFormatFloat4;
        switch (attribute.attributeType) {
        case MTLDataTypeFloat: format = MTLVertexFormatFloat; break;
        case MTLDataTypeFloat2: format = MTLVertexFormatFloat2; break;
        case MTLDataTypeFloat3: format = MTLVertexFormatFloat3; break;
        case MTLDataTypeInt: format = MTLVertexFormatInt; break;
        case MTLDataTypeInt2: format = MTLVertexFormatInt2; break;
        case MTLDataTypeInt3: format = MTLVertexFormatInt3; break;
        case MTLDataTypeInt4: format = MTLVertexFormatInt4; break;
        case MTLDataTypeUInt: format = MTLVertexFormatUInt; break;
        case MTLDataTypeUInt2: format = MTLVertexFormatUInt2; break;
        case MTLDataTypeUInt3: format = MTLVertexFormatUInt3; break;
        case MTLDataTypeUInt4: format = MTLVertexFormatUInt4; break;
        default: break;
        }
        descriptor.attributes[attribute.attributeIndex].format = format;
        descriptor.attributes[attribute.attributeIndex].offset = offset;
        descriptor.attributes[attribute.attributeIndex].bufferIndex = 0;
        offset += 16;
    }
    descriptor.layouts[0].stride = offset;
    return descriptor;
}

// A fragment function is described through a pipeline, which needs a vertex
// function whose outputs it can take: the first of the library's that fits.
MTLRenderPipelineReflection* FragmentReflection(id<MTLDevice> device, id<MTLLibrary> library, NSArray<id<MTLFunction>>* vertexFunctions,
    NSString* name, MTLPixelFormat format, std::string& problem) API_AVAILABLE(macos(13.0))
{
    id<MTLFunction> fragment = [[library newFunctionWithName:name] autorelease];
    if (!fragment || fragment.functionType != MTLFunctionTypeFragment) {
        problem = "the library has no fragment function of that name";
        return nil;
    }
    for (id<MTLFunction> vertex in vertexFunctions) {
        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor new] autorelease];
        descriptor.vertexFunction = vertex;
        descriptor.vertexDescriptor = DescriptorFor(vertex);
        descriptor.fragmentFunction = fragment;
        descriptor.colorAttachments[0].pixelFormat = format;
        MTLRenderPipelineReflection* reflection = nil;
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:descriptor
                                                                                options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
                                                                             reflection:&reflection
                                                                                  error:nil];
        [state release];
        if (state && reflection) return reflection;
    }
    problem = "no vertex function of the library makes a pipeline with it";
    return nil;
}

MTLComputePipelineReflection* KernelReflection(id<MTLDevice> device, id<MTLLibrary> library, NSString* name, std::string& problem)
    API_AVAILABLE(macos(13.0))
{
    id<MTLFunction> kernel = [[library newFunctionWithName:name] autorelease];
    if (!kernel || kernel.functionType != MTLFunctionTypeKernel) {
        problem = "the library has no kernel of that name";
        return nil;
    }
    MTLComputePipelineReflection* reflection = nil;
    NSError* error = nil;
    id<MTLComputePipelineState> state = [device newComputePipelineStateWithFunction:kernel
                                                                            options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo
                                                                         reflection:&reflection
                                                                              error:&error];
    [state release];
    if (!state || !reflection) {
        problem = std::string("no pipeline could be made of it: ") + (error ? error.localizedDescription.UTF8String : "unknown");
        return nil;
    }
    return reflection;
}

void Check(std::string const& metallib, std::vector<ShaderCheck>& checks, std::string& unavailable) API_AVAILABLE(macos(13.0))
{
    id<MTLDevice> device = [MTLCreateSystemDefaultDevice() autorelease];
    if (!device) {
        unavailable = "this machine has no GPU Metal can use";
        return;
    }
    NSError* error = nil;
    id<MTLLibrary> library = [[device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error] autorelease];
    if (!library) {
        checks.push_back({ metallib, std::string("the shader library cannot be loaded: ") + (error ? error.localizedDescription.UTF8String : "unknown") });
        return;
    }
    NSMutableArray<id<MTLFunction>>* vertexFunctions = [NSMutableArray array];
    for (NSString* name in library.functionNames) {
        id<MTLFunction> function = [[library newFunctionWithName:name] autorelease];
        if (function.functionType == MTLFunctionTypeVertex) [vertexFunctions addObject:function];
    }

    // Temporal's passes.
    struct
    {
        PipelineKind Kind;
        MTLPixelFormat Format;      // of its one colour output
    } const passes[] = {
        { PipelineKind::Taa, MTLPixelFormatRG11B10Float },
        { PipelineKind::Linearize, MTLPixelFormatR32Float },
        { PipelineKind::Velocity, MTLPixelFormatRG16Float },
        { PipelineKind::FsrTonemap, MTLPixelFormatRG11B10Float },
    };
    for (auto const& pass : passes) {
        ShaderCheck check{ std::string("gfx_temporal: ") + PipelineKindName(pass.Kind), {} };
        if (auto reflection = FragmentReflection(device, library, vertexFunctions, @(PipelineKindName(pass.Kind)), pass.Format, check.Problem)) {
            check.Problem = TemporalArgumentsProblem((uint8_t)pass.Kind, reflection.fragmentBindings);
        }
        checks.push_back(std::move(check));
    }
    for (PipelineKind kind : { PipelineKind::Fsr, PipelineKind::Rcas }) {
        ShaderCheck check{ std::string("gfx_temporal: ") + PipelineKindName(kind), {} };
        if (auto reflection = KernelReflection(device, library, @(PipelineKindName(kind)), check.Problem)) {
            check.Problem = TemporalArgumentsProblem((uint8_t)kind, reflection.bindings);
        }
        checks.push_back(std::move(check));
    }

    // The cluster guard: a kernel of the clustered lighting is one it watches,
    // and the kernels that write the clusters are not.
    struct
    {
        char const* Name;
        bool Watched;
    } const kernels[] = { { "ClusteredLighting_0", true }, { "SSRApply", true }, { "ClusterAssignment", false }, { "ClusteredClear", false } };
    for (auto const& kernel : kernels) {
        ShaderCheck check{ std::string("gfx_cluster_guard: ") + kernel.Name, {} };
        if (auto reflection = KernelReflection(device, library, @(kernel.Name), check.Problem)) {
            uint32_t fact = ClusterFact(reflection);
            if (fact) check.What += " reads its clusters from buffer " + std::to_string(fact & 31);
            if (kernel.Watched && !fact) check.Problem = "it has no buffer named ClusterInfoBuffer that it only reads";
            if (!kernel.Watched && fact) check.Problem = "it would be given the stand-in, and it is not a kernel that only reads the clusters";
        }
        checks.push_back(std::move(check));
    }
}

}

std::vector<ShaderCheck> CheckGameShaders(std::string const& metallib, std::string& unavailable)
{
    std::vector<ShaderCheck> checks;
    unavailable.clear();
    @autoreleasepool {
        if (@available(macOS 13.0, *)) {
            Check(metallib, checks, unavailable);
        } else {
            unavailable = "pipeline descriptions are read the way macOS 13 or later gives them";
        }
    }
    return checks;
}

}
