#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>

#include <Overlay/FrameHooks.h>
#include <Overlay/OverlayState.h>
#include <Platform/CodeWriter.h>
#include <Platform/EntryHook.h>
#include <Platform/Log.h>

#include <pthread.h>

#if !__has_feature(objc_arc)
#error "FrameHooks.mm is written for automatic reference counting"
#endif

namespace bg3se::mac::overlay {

namespace {

void* Field(void* object, uint32_t offset)
{
    return *reinterpret_cast<void**>(static_cast<uint8_t*>(object) + offset);
}

void Release(void*& held)
{
    if (held) CFRelease((CFTypeRef)held);
    held = nullptr;
}

// Whether the object is of the protocol; `known` remembers the class of the
// last one that was, so that a frame costs one comparison.
bool Is(id object, Protocol* protocol, void*& known)
{
    void* cls = (__bridge void*)object_getClass(object);
    if (cls == known) return true;
    if (![object conformsToProtocol:protocol]) return false;
    known = cls;
    return true;
}

}

FrameTracker::FrameTracker(uint32_t frameDrawable, uint32_t frameRenderer, uint32_t rendererBuffer, FrameListener* listener)
    : frameDrawable_(frameDrawable), frameRenderer_(frameRenderer), rendererBuffer_(rendererBuffer), listener_(listener)
{}

FrameTracker::~FrameTracker()
{
    Drop();
    Release(device_);
}

void FrameTracker::SetListener(FrameListener* listener)
{
    std::lock_guard _(listenerLock_);
    listener_ = listener;
}

void FrameTracker::Drop()
{
    pendingBuffer_.store(nullptr, std::memory_order_relaxed);
    Release(drawable_);
    Release(buffer_);
}

void FrameTracker::SayOnce(Skipped why)
{
    if (said_[(size_t)why].exchange(true)) return;
    switch (why) {
    case Skipped::OffMainThread:
        Log(LogLevel::Warning, "overlay: a frame was presented on a thread that is not the main thread; such a frame is not drawn on");
        break;
    case Skipped::NothingInFrame:
        Log(LogLevel::Warning, "overlay: a frame to present had no drawable or its renderer no command buffer; such a frame is not drawn on");
        break;
    case Skipped::NoTexture:
        Log(LogLevel::Warning, "overlay: a frame's drawable had no texture; such a frame is not drawn on");
        break;
    case Skipped::CommitOffMainThread:
        Log(LogLevel::Warning, "overlay: a noted frame's command buffer was committed on a thread that is not the main thread; such a frame is not drawn on");
        break;
    case Skipped::UnknownColourSpace:
        break;      // said with the name, by the caller
    case Skipped::Count:
        break;
    }
}

void FrameTracker::OnFlipBuffers(void* frame, bool present)
{
    // As the game's function: nothing without a frame to present.
    if (!present || !frame) return;
    if (!pthread_main_np()) {
        SayOnce(Skipped::OffMainThread);
        return;
    }
    void* drawablePointer = Field(frame, frameDrawable_);
    void* renderer = Field(frame, frameRenderer_);
    void* bufferPointer = drawablePointer && renderer ? Field(renderer, rendererBuffer_) : nullptr;
    if (!bufferPointer) {
        SayOnce(Skipped::NothingInFrame);
        return;
    }

    id drawable = (__bridge id)drawablePointer, buffer = (__bridge id)bufferPointer;
    if (!Is(drawable, @protocol(CAMetalDrawable), drawableClass_) || !Is(buffer, @protocol(MTLCommandBuffer), bufferClass_)) {
        if (!refused_) {
            refused_ = true;
            Log(LogLevel::Error, "overlay: the frame's drawable (%s) or command buffer (%s) is not what it should be: nothing is drawn",
                object_getClassName(drawable), object_getClassName(buffer));
        }
        Drop();
        return;
    }
    id<MTLTexture> texture = [(id<CAMetalDrawable>)drawable texture];
    if (!texture) {
        SayOnce(Skipped::NoTexture);
        Drop();
        return;
    }

    // This frame takes the place of one that was never committed.
    Drop();
    drawable_ = (void*)CFBridgingRetain(drawable);
    buffer_ = (void*)CFBridgingRetain(buffer);
    pendingBuffer_.store(bufferPointer, std::memory_order_relaxed);

    id<MTLDevice> device = texture.device;
    if ((__bridge void*)device != device_) {
        Release(device_);
        device_ = (void*)CFBridgingRetain(device);
    }
    FrameInfo info;
    info.Device = device_;
    info.Width = (uint32_t)texture.width;
    info.Height = (uint32_t)texture.height;
    info.Format = TargetOf(drawablePointer);
    info.Target = info.Format.Kind;
    if (info.Format.Space == ColourSpace::Unknown && !said_[(size_t)Skipped::UnknownColourSpace].exchange(true)) {
        Log(LogLevel::Warning, "overlay: the game's layer has a colour space the overlay has no conversion for (%s): it is not drawn on",
            ColourSpaceNameOf(drawablePointer).c_str());
    }
    State().SetDrawableSize(info.Width, info.Height);
    std::lock_guard _(listenerLock_);
    if (listener_) listener_->OnFrame(info);
}

void FrameTracker::OnCommitCommandBuffer(void* renderer)
{
    // Every renderer's commit comes through here, on any thread: one load and
    // one comparison for all but the commit of the noted frame.
    void* pending = pendingBuffer_.load(std::memory_order_relaxed);
    if (!pending || !renderer || Field(renderer, rendererBuffer_) != pending) return;
    if (!pthread_main_np()) {
        SayOnce(Skipped::CommitOffMainThread);
        return;
    }

    id<MTLCommandBuffer> buffer = (__bridge id<MTLCommandBuffer>)buffer_;
    {
        std::lock_guard _(listenerLock_);
        // Not yet committed, or there is nothing to add to it.
        if (listener_ && buffer.status < MTLCommandBufferStatusCommitted) listener_->OnCommit(buffer_, drawable_);
    }
    Drop();
}

// --- the hooks --------------------------------------------------------------

namespace {

using FlipBuffersFn = void (*)(void* frame, bool present);
using CommitFn = void (*)(void* renderer, bool wait);

FrameTracker* gTracker = nullptr;
FlipBuffersFn gFlipBuffers = nullptr;
CommitFn gCommit = nullptr;
EntryHook* gCommitHook = nullptr;
void** gSlot = nullptr;
bool gInstalled = false;

void FlipBuffers(void* frame, bool present)
{
    gTracker->OnFlipBuffers(frame, present);
    gFlipBuffers(frame, present);
}

void CommitCommandBuffer(void* renderer, bool wait)
{
    gTracker->OnCommitCommandBuffer(renderer);
    gCommit(renderer, wait);
}

}

std::string InstallFrameHooks(Dependencies const& d, FrameListener* listener)
{
    if (gInstalled) return {};
    if (!listener) return "no listener for the frames";
    if (!d.Live) return "the dependencies were not checked on the running game";
    if (!d.Usable(Part::Frame)) return d.Problem(Part::Frame);

    auto slot = reinterpret_cast<void**>((uintptr_t)d.FlipBuffersSlot);
    auto flipBuffers = reinterpret_cast<void*>((uintptr_t)d.FlipBuffers);
    if (!slot || !flipBuffers || *slot != flipBuffers) return "the vtable slot does not hold the game's FlipBuffers";

    // The tracker is never freed: a hook that was taken out may still be running.
    if (!gTracker) gTracker = new FrameTracker(d.FrameDrawable, d.FrameRenderer, d.RendererCommandBuffer, listener);
    gTracker->SetListener(listener);
    gFlipBuffers = reinterpret_cast<FlipBuffersFn>(flipBuffers);

    std::string problem;
    gCommitHook = PrepareEntryHook(reinterpret_cast<void*>((uintptr_t)d.CommitCommandBuffer), (size_t)d.CommitCommandBufferBytes,
        reinterpret_cast<void const*>(&CommitCommandBuffer), reinterpret_cast<void**>(&gCommit), problem);
    if (!gCommitHook) return "CommitCommandBuffer cannot be hooked: " + problem;
    problem = CommitEntryHook(gCommitHook);
    if (!problem.empty()) return "CommitCommandBuffer cannot be hooked: " + problem;

    problem = StorePointer(slot, reinterpret_cast<void const*>(&FlipBuffers));
    if (!problem.empty()) {
        RemoveEntryHook(gCommitHook);
        return "the FlipBuffers slot cannot be written: " + problem;
    }
    gSlot = slot;
    gInstalled = true;
    return {};
}

void RemoveFrameHooks()
{
    if (!gInstalled) return;
    gInstalled = false;
    if (gSlot && *gSlot == reinterpret_cast<void*>(&FlipBuffers)) StorePointer(gSlot, reinterpret_cast<void const*>(gFlipBuffers));
    if (gCommitHook) RemoveEntryHook(gCommitHook);
    gTracker->SetListener(nullptr);
}

bool FrameHooksInstalled()
{
    return gInstalled;
}

}
