#pragma once

#include <cstddef>
#include <cstdint>

// The Mac game's table of fixed strings (ls::gst), as its own code shows it
// (Layout/GameShapes.cpp and the engine table's entry for
// ls::gst::s_Instance say which code). Norbyte's declaration
// (CoreLib/Base/BaseString.h, GlobalStringTable) is asserted to give the same
// offsets (Layout/DefinitionChecks.cpp).
//
// ls::gst::s_Instance points to one block, 64-byte aligned: eleven pools
// (ls::gst::NodePool, Norbyte's SubTable), one after another from the start,
// and after them the map from a text's hash to its string (ls::gst::Map, his
// MainTable). Every member of a pool and of the map is on a 64-byte line of
// its own. A pool is as large as the Windows one although its lock is the
// Mac game's 0x14-byte critical section: the lock has the last line to itself.
//
// A fixed string's index names its entry: the pool in the low four bits, the
// pool's bucket in the next sixteen, the entry within the bucket in the top
// twelve. The null string is index 0xffffffff, which no function here tests
// for: the callers do.

namespace bg3se::mac {

constexpr size_t kStringLine = 0x40;

// ls::gst::NodePool.
constexpr size_t kStringPoolSize = 0x1200;
constexpr size_t kStringPoolEpochSlots = 0;             // 64 words, one per thread index, each on its own line
constexpr size_t kStringPoolEpoch = 0x1000;             // what the readers copy into their slot
constexpr size_t kStringPoolEvent = 0x1040;             // ls::ThreadEvent: its Mach semaphore first
constexpr size_t kStringPoolIndex = 0x1080;             // which pool this is, 32 bits
constexpr size_t kStringPoolEntrySize = 0x1088;         // bytes per entry, 64 bits
constexpr size_t kStringPoolEntriesPerBucket = 0x1090;  // 32 bits: kStringBucketBytes / entry size
constexpr size_t kStringPoolBucketCount = 0x10c0;       // 32 bits
constexpr size_t kStringPoolUsed = 0x1100;              // 32 bits
constexpr size_t kStringPoolBuckets = 0x1140;           // pointer to an array of pointers, one per bucket
constexpr size_t kStringPoolFreeList = 0x1180;          // 64 bits
constexpr size_t kStringPoolLock = 0x11c0;              // ls::CriticalSection
constexpr size_t kStringBucketBytes = 0x10000;

// The table.
constexpr size_t kStringPools = 11;
constexpr size_t kStringTableMain = kStringPools * kStringPoolSize;     // 0xc600: the map

// ls::gst::Map.
constexpr size_t kStringMapEpoch = 0x1000;
constexpr size_t kStringMapEvent = 0x1040;
constexpr size_t kStringMapStorage = 0x1080;            // pointer to the pools: the table itself
constexpr size_t kStringMapSlots = 0x10c0;              // kStringMapSlotCount words, each the head of a chain
constexpr size_t kStringMapSlotCount = 0xffff;

// An entry of a pool: Norbyte's FixedStringBase::Header, and the text after it.
constexpr size_t kStringEntryHash = 0;
constexpr size_t kStringEntryReferences = 4;
constexpr size_t kStringEntryLength = 8;
constexpr size_t kStringEntryId = 0xc;
constexpr size_t kStringEntryNext = 0x10;
constexpr size_t kStringEntryText = 0x18;

// A fixed string's index.
constexpr uint32_t kStringIndexPoolMask = 0xf;
constexpr uint32_t kStringIndexEntryShift = 20;
constexpr uint32_t kNullStringIndex = 0xffffffffu;

static_assert(kStringTableMain == 0xc600);

}
