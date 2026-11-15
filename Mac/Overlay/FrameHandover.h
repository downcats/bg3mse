#pragma once

#include <cstdint>
#include <mutex>

#include <Overlay/OverlayState.h>

// What the rendering backend's two threads share: whether ImGui's Metal
// renderer can draw, and which finished frame it is to draw.
//
// The client thread builds the frames and owns the renderer's objects; the
// main thread draws the frame that was published last, in the game's commit
// (Overlay/FrameHooks.h). One lock orders the two, as globalResourceLock_
// does in Norbyte's backends; unlike there it is not held while the
// renderer's objects are made or while the renderer waits to be shut down,
// because in the Mac game the thread that draws is the main thread, which a
// wait would keep from the game's frame and from its events. The frames themselves (a ring
// of viewports and their draw lists) stay with the backend: here is only
// which of them is published, and the rules for when it may be drawn.
//
// Nothing here touches ImGui, Metal or the game: what is done to them is
// passed in, so the rules run in the tests.

namespace bg3se::mac::overlay {

class FrameHandover
{
public:
    static constexpr int32_t kNoFrame = -1;

    explicit FrameHandover(OverlayState& shared = State()) : shared_(shared) {}

    // --- the client thread --------------------------------------------------

    // InitializeUI. `init()` gives the renderer the device; false when it could not.
    template <class Init>
    bool Initialize(Init&& init)
    {
        std::lock_guard client(renderer_);
        if (!init()) return false;
        std::lock_guard _(lock_);
        failed_ = false;
        objects_ = false;
        initialized_ = true;
        return true;
    }

    // DestroyUI. Nothing is drawn from here on, and no draw is in progress
    // when `shutdown()` shuts the renderer down. That may wait for buffers
    // that are on their way, so it runs without the lock the main thread takes.
    template <class Shutdown>
    void ShutDown(Shutdown&& shutdown)
    {
        std::lock_guard client(renderer_);
        {
            std::lock_guard _(lock_);
            initialized_ = false;
            objects_ = false;
            slot_ = kNoFrame;
            PublishPresenting();
        }
        shutdown();
    }

    // NewFrame, while the renderer has no objects yet. `make()` makes them
    // and says whether it could. False when it could not: nothing is drawn
    // until the renderer is initialised again.
    //
    // `make` runs without the lock the main thread takes, which draws
    // nothing meanwhile: the renderer has no objects yet.
    template <class Make>
    bool MakeObjects(Make&& make)
    {
        std::lock_guard client(renderer_);
        {
            std::lock_guard _(lock_);
            // Once: frames already on their way use them.
            if (failed_ || objects_) return true;
        }
        bool made = make();
        std::lock_guard _(lock_);
        failed_ = !made;
        if (made) objects_ = true;
        PublishPresenting();
        return made;
    }

    // FinishFrame. `copy()` completes the frame in `slot` (which is not the
    // published one); the frame is then the one to draw.
    template <class Copy>
    void Publish(int32_t slot, Copy&& copy)
    {
        std::lock_guard _(lock_);
        copy();
        slot_ = slot;
        PublishPresenting();
    }

    // ClearFrame: the published frame is not to be drawn any more. Returns
    // once it is not being drawn either: what it names may be freed after.
    void Withdraw()
    {
        std::lock_guard _(lock_);
        slot_ = kNoFrame;
        PublishPresenting();
    }

    // Whether the game presents on a target the overlay draws on.
    void SetTargetDrawable(bool drawable)
    {
        std::lock_guard _(lock_);
        targetDrawable_ = drawable;
        PublishPresenting();
    }

    bool Initialized() const { return Read(initialized_); }
    bool HasObjects() const { return Read(objects_); }
    bool TargetDrawable() const { return Read(targetDrawable_); }

    // --- the main thread ----------------------------------------------------

    // The game is about to commit the frame's buffer: `draw(slot)` draws the
    // published frame, when there is one and the renderer can draw it.
    template <class Draw>
    void Commit(Draw&& draw)
    {
        std::lock_guard _(lock_);
        if (!initialized_ || failed_ || !objects_ || slot_ == kNoFrame) return;
        draw(slot_);
    }

private:
    bool Read(bool const& value) const
    {
        std::lock_guard _(lock_);
        return value;
    }

    // Whether the overlay is on screen, which is exactly when Commit draws a
    // frame on a target the overlay draws on: what the input goes by when it
    // takes an event from the game. An overlay that is not drawn takes none.
    void PublishPresenting()
    {
        shared_.Presenting.store(initialized_ && objects_ && !failed_ && targetDrawable_ && slot_ != kNoFrame);
    }

    OverlayState& shared_;
    // Orders the client thread's uses of the renderer (and the thread that
    // shuts it down, should that be another). Held across the slow work; the
    // main thread never takes it.
    std::mutex renderer_;
    // What the main thread draws under. Never held across anything that can
    // take long: the main thread is also the one that takes the game's events.
    mutable std::mutex lock_;
    bool initialized_{ false };
    bool objects_{ false };
    bool failed_{ false };
    bool targetDrawable_{ false };
    int32_t slot_{ kNoFrame };
};

}
