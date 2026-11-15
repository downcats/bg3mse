#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

// What the overlay's two threads tell each other without a lock.
//
// The game presents and takes its events on the main thread; Norbyte's code
// builds the ImGui frame on the client thread. The main thread says how large
// the drawable is; the client thread says whether the overlay is being drawn
// and what ImGui wanted after its last frame, which decides which events the
// overlay takes from the game. Each value is one frame old at most where it
// is read, as on Windows.

namespace bg3se::mac::overlay {

struct OverlayState
{
    // The size of the drawable last presented, in pixels: width in the upper
    // half, height in the lower; 0 before the first.
    std::atomic<uint64_t> DrawableSize{ 0 };

    // A frame of the overlay is being drawn: the renderer has its objects
    // and its font texture, a frame is published, and the target is one the
    // overlay draws on. Only then does it take events from the game
    // (Overlay/FrameHandover.h keeps it).
    std::atomic<bool> Presenting{ false };
    std::atomic<bool> WantTextInput{ false };
    std::atomic<bool> WantCaptureMouse{ false };

    // The pointer, as the client thread asks for it before each frame and
    // the main thread makes it (Overlay/Pointer.h): ImGui's cursor, or
    // kKeepCursor; and where to move the pointer to, in the drawable's
    // pixels, each time WarpSerial changes.
    std::atomic<int32_t> MouseCursor{ -2 };
    std::atomic<uint64_t> WarpTo{ 0 };
    std::atomic<uint32_t> WarpSerial{ 0 };

    // Where ImGui's text cursor is, for the input method's window
    // (Overlay/TextInput.h), in the drawable's pixels; the client thread
    // says it after a frame whose text field wants it shown.
    std::atomic<uint64_t> ImePosition{ 0 };
    std::atomic<float> ImeLineHeight{ 0 };

    void SetIme(float x, float y, float lineHeight)
    {
        ImePosition.store(Pack(x, y), std::memory_order_relaxed);
        ImeLineHeight.store(lineHeight, std::memory_order_relaxed);
    }
    void ImePoint(float& x, float& y) const { Unpack(ImePosition.load(std::memory_order_relaxed), x, y); }

    static uint64_t Pack(float x, float y)
    {
        uint32_t bits[2];
        memcpy(&bits[0], &x, sizeof(x));
        memcpy(&bits[1], &y, sizeof(y));
        return (uint64_t)bits[0] << 32 | bits[1];
    }
    static void Unpack(uint64_t packed, float& x, float& y)
    {
        uint32_t bits[2] = { (uint32_t)(packed >> 32), (uint32_t)packed };
        memcpy(&x, &bits[0], sizeof(x));
        memcpy(&y, &bits[1], sizeof(y));
    }

    void RequestWarp(float x, float y)
    {
        WarpTo.store(Pack(x, y), std::memory_order_relaxed);
        WarpSerial.fetch_add(1, std::memory_order_release);
    }
    void WarpPoint(float& x, float& y) const { Unpack(WarpTo.load(std::memory_order_relaxed), x, y); }

    void SetDrawableSize(uint32_t width, uint32_t height) { DrawableSize.store((uint64_t)width << 32 | height, std::memory_order_relaxed); }
    uint32_t DrawableWidth() const { return (uint32_t)(DrawableSize.load(std::memory_order_relaxed) >> 32); }
    uint32_t DrawableHeight() const { return (uint32_t)DrawableSize.load(std::memory_order_relaxed); }
};

inline OverlayState& State()
{
    static OverlayState state;
    return state;
}

}
