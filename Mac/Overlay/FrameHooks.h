#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <Overlay/Dependencies.h>
#include <Overlay/MetalOverlay.h>

// Where the overlay meets the game's frame.
//
// rf::metal::MetalRenderFrame::FlipBuffers(bool) is where the game schedules
// the present of a frame: it sends presentDrawable: to the main renderer's
// command buffer. Its slot in the vtable is replaced by a function that
// first notes the frame's drawable and that command buffer, then calls the
// game's. Nothing is drawn there: the renderer may still have an encoder
// open on the buffer.
//
// rf::metal::MetalRenderer::CommitCommandBuffer(bool) commits a renderer's
// buffer. It ends no encoder of the renderer's, and Metal does not commit a
// buffer with an encoder open: at its entry the buffer is complete. A hook
// there asks, for the buffer that was noted and no other, for the overlay to
// be drawn, and then runs the game's function, which commits the buffer with
// the overlay's pass at its end and the present already scheduled.
//
// The game runs both on its main thread for the frame it presents; every
// renderer, on any thread, commits through the second. A commit of another
// buffer, or on another thread, is passed on untouched.

namespace bg3se::mac::overlay {

struct FrameInfo
{
    void* Device;           // the id<MTLDevice> of the drawable's texture; kept alive by the tracker
    uint32_t Width, Height; // of the drawable's texture, in pixels
    TargetKind Target;
    TargetFormat Format;    // what the target kind was decided from
};

class FrameListener
{
public:
    virtual ~FrameListener() = default;
    // Main thread: the game has scheduled the present of a frame.
    virtual void OnFrame(FrameInfo const& frame) = 0;
    // Main thread: the game is about to commit that frame's command buffer
    // (an id<MTLCommandBuffer>), which will present `drawable` (an id<CAMetalDrawable>).
    virtual void OnCommit(void* commandBuffer, void* drawable) = 0;
};

// What the two hooks do before the game's functions run, on the offsets the
// game's code gave. The hooks call it; the tests call it on objects of their own.
class FrameTracker
{
public:
    // The offsets of the drawable and of the renderer in a render frame, and
    // of the command buffer in a renderer.
    FrameTracker(uint32_t frameDrawable, uint32_t frameRenderer, uint32_t rendererBuffer, FrameListener* listener);
    ~FrameTracker();
    FrameTracker(FrameTracker const&) = delete;
    FrameTracker& operator = (FrameTracker const&) = delete;

    void OnFlipBuffers(void* frame, bool present);
    void OnCommitCommandBuffer(void* renderer);
    // Null: frames are noted and nobody is told. Returns once no call of the
    // listener that was set before is in progress, so that one may be freed
    // after. Not to be called from a listener's own methods.
    void SetListener(FrameListener* listener);

    // Whether a frame is noted and waits for its commit.
    bool Pending() const { return pendingBuffer_.load(std::memory_order_relaxed) != nullptr; }

private:
    void Drop();

    uint32_t frameDrawable_, frameRenderer_, rendererBuffer_;
    // Held across every call of the listener: the hooks run on the main
    // thread, and the listener is taken out on another.
    std::mutex listenerLock_;
    FrameListener* listener_;
    // The noted frame: both retained; touched on the main thread only.
    void* drawable_{ nullptr };
    void* buffer_{ nullptr };
    void* device_{ nullptr };       // retained
    // The noted buffer again, for the test every committing thread makes.
    std::atomic<void*> pendingBuffer_{ nullptr };
    // The classes of the last objects that were found to be what they must be.
    void* drawableClass_{ nullptr };
    void* bufferClass_{ nullptr };
    bool refused_{ false };
    // Why a frame that was to be presented was not noted, each said once:
    // without it "the hooks are on" and no frame seen would not say why.
    enum class Skipped : uint8_t { OffMainThread, NothingInFrame, NoTexture, CommitOffMainThread, UnknownColourSpace, Count };
    void SayOnce(Skipped why);
    std::atomic<bool> said_[(size_t)Skipped::Count]{};
};

// Replaces the vtable slot and hooks the commit. Empty when both are in;
// otherwise why neither is. `listener` must live until RemoveFrameHooks has
// returned.
std::string InstallFrameHooks(Dependencies const& dependencies, FrameListener* listener);
// Puts the slot back and takes the hook out. When it returns the listener is
// not being called and will not be called again.
void RemoveFrameHooks();
bool FrameHooksInstalled();

}
