#include <Gfx/GfxMetalCalls.h>

#include <algorithm>
#include <iterator>

namespace bg3se::mac::gfx {

namespace {

constexpr char const* kDevice[] = {
    "newCommandQueue",
    "newCommandQueueWithMaxCommandBufferCount:",
    "newRenderPipelineStateWithDescriptor:error:",
    "newComputePipelineStateWithFunction:error:",
};

constexpr char const* kQueue[] = {
    "commandBuffer",
    "commandBufferWithUnretainedReferences",
};

constexpr char const* kBuffer[] = {
    "renderCommandEncoderWithDescriptor:",
    "computeCommandEncoder",
    "computeCommandEncoderWithDispatchType:",
    "commit",
    "presentDrawable:",
    "presentDrawable:afterMinimumDuration:",
    "presentDrawable:atTime:",
};

constexpr char const* kRenderEncoder[] = {
    "setRenderPipelineState:",
    "setFragmentTexture:atIndex:",
    "setFragmentTextures:withRange:",
    "setFragmentBuffer:offset:atIndex:",
    "setFragmentBuffers:offsets:withRange:",
    "setFragmentBufferOffset:atIndex:",
    "setFragmentBytes:length:atIndex:",
    "drawPrimitives:vertexStart:vertexCount:",
    "drawPrimitives:vertexStart:vertexCount:instanceCount:",
    "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:",
    "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:",
    "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:",
    "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",
    "endEncoding",
};

constexpr char const* kComputeEncoder[] = {
    "setComputePipelineState:",
    "setTexture:atIndex:",
    "setTextures:withRange:",
    "setBuffer:offset:atIndex:",
    "setBuffers:offsets:withRange:",
    "setBytes:length:atIndex:",
    "dispatchThreads:threadsPerThreadgroup:",
    "dispatchThreadgroups:threadsPerThreadgroup:",
    "dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:",
    "endEncoding",
};

// A family: every selector that starts with one of these makes, binds, draws
// or presents something the hooks follow.
constexpr std::string_view kFamilies[] = {
    "newCommandQueue",              // queues
    "commandBuffer",                // command buffers of a queue
    "renderCommandEncoder",         // encoders of a command buffer
    "parallelRenderCommandEncoder",
    "computeCommandEncoder",
    "newRenderPipelineState",       // pipelines, recognised by their functions' names
    "newComputePipelineState",
    "setRenderPipelineState",
    "setComputePipelineState",
    "setFragmentTexture",           // what a draw or dispatch reads
    "setFragmentBuffer",
    "setFragmentBytes",
    "setTexture:atIndex:",
    "setTextures:",
    "setBuffer:offset:atIndex:",
    "setBuffers:",
    "setBytes:",
    "drawPrimitives",               // draws and dispatches
    "drawIndexedPrimitives",
    "drawPatches",
    "drawIndexedPatches",
    "drawMesh",
    "dispatchThread",
    "executeCommandsInBuffer",
    "presentDrawable",              // presents
};

// What the hooks cannot do without: with none of a group, the game does not
// render the way they follow.
constexpr char const* kNeeded[] = {
    "newCommandQueueWithMaxCommandBufferCount:",
    "renderCommandEncoderWithDescriptor:",
    "commit",
};

bool Among(std::span<char const* const> list, std::string_view selector)
{
    return std::any_of(list.begin(), list.end(), [&](char const* s) { return selector == s; });
}

}

std::span<char const* const> HookedDeviceSelectors() { return kDevice; }
std::span<char const* const> HookedQueueSelectors() { return kQueue; }
std::span<char const* const> HookedBufferSelectors() { return kBuffer; }
std::span<char const* const> HookedRenderEncoderSelectors() { return kRenderEncoder; }
std::span<char const* const> HookedComputeEncoderSelectors() { return kComputeEncoder; }

bool InHookedFamily(std::string_view selector)
{
    return std::any_of(std::begin(kFamilies), std::end(kFamilies), [&](std::string_view family) { return selector.starts_with(family); });
}

bool IsHooked(std::string_view selector)
{
    return Among(kDevice, selector) || Among(kQueue, selector) || Among(kBuffer, selector) || Among(kRenderEncoder, selector) ||
           Among(kComputeEncoder, selector);
}

std::string CheckMetalCalls(std::span<std::string_view const> selectors)
{
    if (selectors.empty()) return "no Objective-C call is in the game's symbol table";
    for (char const* needed : kNeeded) {
        if (std::find(selectors.begin(), selectors.end(), std::string_view(needed)) == selectors.end()) {
            return std::string("the game does not send ") + needed;
        }
    }
    for (std::string_view selector : selectors) {
        if (InHookedFamily(selector) && !IsHooked(selector)) {
            return "the game sends " + std::string(selector) + ", which the Metal hooks do not follow";
        }
    }
    return {};
}

}
