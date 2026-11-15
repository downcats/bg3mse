#pragma once

#include <cstddef>
#include <cstdint>

// The game's lock-free queues keep each part that different threads write on
// a cache line of its own. Norbyte's definitions (GameDefinitions/
// EntitySystem.h) align those parts to 64 bytes, the line of the Windows
// game's processors; the Mac game is built for 128-byte lines.
//
//   ls::MPMCQueueBounded<T>     { capacity, slots } at +0, the write index
//                               one line on, the read index two lines on;
//                               three lines in all. Each slot of the array is
//                               one line: a sequence word, then the value.
//   ls::MPMCQueueUnbounded<T>   a kernel reader-writer lock on a line of its
//                               own, then a bounded queue: four lines.
//                               (Norbyte's SynchronizedMPMCQueueBounded.)

namespace bg3se::mac {

constexpr size_t kQueueLine = 128;

constexpr size_t kQueueWriteIndex = kQueueLine;
constexpr size_t kQueueReadIndex = 2 * kQueueLine;
constexpr size_t kQueueSize = 3 * kQueueLine;
constexpr size_t kQueueSlotValue = sizeof(uint64_t);

constexpr size_t kLockedQueueQueue = kQueueLine;
constexpr size_t kLockedQueueSize = kLockedQueueQueue + kQueueSize;

// What stays at 64 bytes on Mac: the per-thread states of
// ecs::EntityHandleGenerator.
constexpr size_t kHandleGeneratorThreadState = 64;

}
