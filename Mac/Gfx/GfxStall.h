#pragma once

#include <atomic>
#include <cstdint>

// What the stall log (gfx_stall_log, GfxMetal.mm) writes for one command
// buffer that took long to finish. No Metal, so the tests check it.
//
// A queue runs its buffers in order: when one runs long on the GPU (the game's
// first frames after a load), every buffer committed after it waits, and they
// all finish in the same moment. Those are not stalls of their own: they are
// counted, and one line a moment later says how many there were.

namespace bg3se::mac::gfx {

constexpr double kStallSeconds = 0.25;   // well past any real frame

enum class BufferStall : uint8_t {
    None,       // finished in time
    Own,        // ran long on the GPU, or failed: a line of its own
    Waited,     // waited behind other work: counted
};

inline BufferStall ClassifyBufferStall(double total, double gpu, bool failed)
{
    if (failed) return BufferStall::Own;
    if (total < kStallSeconds) return BufferStall::None;
    return gpu >= kStallSeconds ? BufferStall::Own : BufferStall::Waited;
}

// The buffers that waited, since the last time they were reported. Any thread
// adds; one thread takes.
class StallWaiters
{
public:
    void Add(double waited)
    {
        count_.fetch_add(1, std::memory_order_relaxed);
        auto ms = static_cast<uint32_t>(waited * 1000);
        auto longest = longestMs_.load(std::memory_order_relaxed);
        while (ms > longest && !longestMs_.compare_exchange_weak(longest, ms, std::memory_order_relaxed)) {}
    }

    struct Taken
    {
        uint32_t Count;
        uint32_t LongestMs;
    };

    // What was added since the last call; nothing (Count 0) when nothing was.
    Taken Take()
    {
        uint32_t count = count_.exchange(0, std::memory_order_relaxed);
        uint32_t longest = longestMs_.exchange(0, std::memory_order_relaxed);
        return { count, longest };
    }

private:
    std::atomic<uint32_t> count_{ 0 };
    std::atomic<uint32_t> longestMs_{ 0 };
};

}
