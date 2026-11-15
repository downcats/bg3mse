#pragma once

#include <cstddef>
#include <cstdint>

// Where the Mac game keeps the parts of ecs::EntityWorld and of its component
// registry that Norbyte's code reads, as far as a game function shows the
// place (Layout/GameShapes.cpp says which). Norbyte's declarations
// (GameDefinitions/EntitySystem.h) are asserted to give the same offsets
// (Layout/DefinitionChecks.cpp). Apart from the bit set's two counts
// (Base/BitSet.h), the declarations are not edited for this: they come out
// right once the locks and queues inside EntityWorld have their Mac shapes.

namespace bg3se::mac {

// ecs::core::ComponentTypeEntry
constexpr size_t kComponentTypeEntrySize = 0x40;
constexpr size_t kComponentTypeEntrySizes = 0x10;           // inline size (16 bits), then total size (16 bits)
constexpr size_t kComponentTypeEntryDependents = 0x20;      // array: pointer, capacity at +8, size at +0xc
constexpr size_t kComponentTypeEntryDependencies = 0x30;

// ecs::EntityWorld
constexpr size_t kWorldComponentTypes = 0x18;       // the registry's entries
constexpr size_t kWorldComponentCallbacks = 0x240;
constexpr size_t kWorldHandleGenerator = 0x2c8;
constexpr size_t kWorldStorage = 0x2d0;
constexpr size_t kWorldComponentOps = 0x390;
constexpr size_t kWorldCache = 0x3f0;

// ecs::EntityStorageContainer: the storages by index, and per thread a
// paged array of the entities' salts (16 bytes each: the pages, the bits of
// an index within a page at +8, the size at +0xc).
constexpr size_t kStorageStorages = 0x0;
constexpr size_t kStorageSalts = 0x50;
constexpr size_t kStorageSaltsShift = 4;
// An entity's salt entry is 8 bytes: the salt, then the index of the
// storage the entity is in.
constexpr size_t kSaltStorageIndex = 0x4;
constexpr size_t kSaltShift = 3;

// ecs::EntityStorageData: the component pages, the hash map from a
// component type to its slot in a page, and the one from an entity to its
// page and index.
constexpr size_t kStorageDataComponents = 0x138;
constexpr size_t kStorageDataComponentSlots = 0x180;
constexpr size_t kStorageDataEntities = 0x1c0;
// A component page has 16 bytes per slot, the components' buffer first.
constexpr size_t kComponentPageSlotShift = 4;

// ecs::ComponentOps<T>: the component type's signals, and the world.
constexpr size_t kComponentOpsSignals = 0x10;
constexpr size_t kComponentOpsWorld = 0x20;
// ecs::ComponentSignals<T>: after a vtable pointer, the signal sent when a
// component is attached and the one sent when it is detached. A signal is
// the next registrant's id and the array of its connections (pointer at +8
// of the signal, size at +0x14). A connection is 0x48 bytes: a function
// object, called through the pointer it begins with and given what follows
// that pointer, then the registrant's id.
constexpr size_t kSignalsAttached = 0x8;
constexpr size_t kSignalsDetached = 0x20;
constexpr size_t kSignalConnections = 0x8;
constexpr size_t kSignalConnectionSize = 0x48;
constexpr size_t kSignalFunctionState = 0x8;

// ls::DynamicBitSet: the words (up to 64 bits, the bits themselves in their
// place), the size in bits, then the capacity in bits. The words are inline
// while the capacity is at most 64.
constexpr size_t kBitSetSize = 0x8;
constexpr size_t kBitSetCapacity = 0xc;
constexpr uint32_t kBitSetInlineBits = 64;

// ecs::EntityHandleGenerator::ThreadState: a paged array of entries (the
// pages, the bits of an index within a page at +8, the number of pages at
// +0xa, the size at +0xc), then the first and last free entries and the
// number of free entries. An entry is the index, then the salt; a new one
// has salt 1. A handle is the index, the salt from bit 32 and the thread's
// index from bit 54.
constexpr size_t kPagedArrayPages = 0x0;
constexpr size_t kPagedArrayBits = 0x8;
constexpr size_t kPagedArrayPageCount = 0xa;
constexpr size_t kPagedArraySize = 0xc;
constexpr size_t kHandleNextFree = 0x10;
constexpr size_t kHandleLastFree = 0x14;
constexpr size_t kHandleFreeCount = 0x18;
constexpr uint64_t kHandleFirstSalt = uint64_t(1) << 32;
constexpr uint32_t kHandleThreadShift = 54;

// ecs::EntityStorageData, past the members above: the sizes and destructors
// of the components (an entry is 16 bytes, the destructor at +8), the
// storage's index, the number of component slots, the component pages' and
// the entity pages' arrays, the pages' masks (16 bytes each, the used mask
// at +8), the one-frame components (a hash map from the type to a hash map
// of the entities, behind a byte that says there are any), the slots whose
// components were changed (256 bits).
constexpr size_t kStorageDataGroupMask = 0x110;
constexpr size_t kStorageDataComponentSizes = 0x118;
constexpr size_t kStorageDataComponentDtors = 0x120;
constexpr size_t kComponentDtorEntryShift = 4;
constexpr size_t kComponentDtorEntryProc = 0x8;
constexpr size_t kStorageDataStorageIndex = 0x128;
constexpr size_t kStorageDataSlotCount = 0x12c;
constexpr size_t kStorageDataHandles = 0x148;
constexpr size_t kStorageDataPageInfos = 0x158;
constexpr size_t kPageInfoUsedMask = 0x8;
constexpr size_t kStorageDataOneFrameComponents = 0x2a0;
constexpr size_t kStorageDataHasOneFrameComponents = 0x2e0;
constexpr size_t kStorageDataModifiedComponents = 0x2f0;
// A component page's slot: the components' buffer, then the mask of the
// entities whose component was changed.
constexpr size_t kComponentPageSlotModified = 0x8;
// ecs::EntityStorageContainer: the storages that have changed components.
constexpr size_t kStorageUsedFrameData = 0x498;

// A component type's index: the low 15 bits, and a one-frame type has bit
// 15 set. Where both kinds share one table, a one-frame type's place is
// after the 0x880 places of the others.
constexpr uint32_t kComponentTypeIndexMask = 0x7fff;
constexpr uint32_t kComponentTypeCount = 0x880;
// The hash of a component type in the game's hash maps: its place, in both
// halves of a word.
constexpr uint32_t kComponentTypeHashFactor = 0x10001;

// ecs::EntityCommandBuffer: the handle generator, the frame allocator, the
// changes per entity (a paged hash map whose values are at +0x30: the
// pages at +8, the bits at +0x10, the size at +0x14), then the component
// pools (a lookup of 64-bit mask words: pages +8, bits +0x10, size +0x14;
// the indices: pages +0x18, bits +0x20; the keys: pages +0x30, bits +0x38;
// the values: pages +0x48, bits +0x50).
constexpr size_t kCommandsHandleGenerator = 0x0;
constexpr size_t kCommandsAllocator = 0x8;
constexpr size_t kCommandsChanges = 0x10;
constexpr size_t kPagedMapValues = 0x30;
constexpr size_t kCommandsPools = 0x58;
constexpr size_t kPoolsMask = 0x8;
constexpr size_t kPoolsIndices = 0x18;
constexpr size_t kPoolsKeys = 0x30;
constexpr size_t kPoolsValues = 0x48;
// ecs::ECBEntityChange: the component changes (a paged array with its
// allocator first: pages +8, bits +0x10, pages' count +0x12, size +0x14),
// the masks, the flags (create 1, destroy 2), 0xffff. 0x30 bytes. A
// component change is 8 bytes, the type at bit 48.
constexpr size_t kEntityChangeSize = 0x30;
constexpr size_t kEntityChangeStore = 0x0;
constexpr size_t kEntityChangeFlags = 0x28;
constexpr size_t kEntityChangeLast = 0x2a;
constexpr uint32_t kEntityChangeCreate = 1;
constexpr uint32_t kEntityChangeDestroy = 2;
constexpr uint32_t kComponentChangeTypeShift = 48;
constexpr uint32_t kEntityChangeStoreBits = 4;
// ecs::ComponentFrameStorage: a paged array of pages with its allocator
// first, the number of components, the component's size and type and its
// destructor. 0x28 bytes.
constexpr size_t kFrameStorageSize = 0x28;
constexpr size_t kFrameStoragePages = 0x8;
constexpr size_t kFrameStorageBits = 0x10;
constexpr size_t kFrameStoragePageCount = 0x12;
constexpr size_t kFrameStoragePagesSize = 0x14;
constexpr size_t kFrameStorageCount = 0x18;
constexpr size_t kFrameStorageComponentSize = 0x1c;
constexpr size_t kFrameStorageType = 0x1e;
constexpr size_t kFrameStorageDtor = 0x20;
// How many components of a size go on each page of a frame storage: the
// size's bucket (sizes from 0 to 64), the components on each of the first
// nine pages per bucket, and the components on the pages up to each.
constexpr uint32_t kFrameSizeBuckets[65] = {
    0, 0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9,
    10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 12, 12, 12,
    13, 13, 13, 13, 13, 13, 13, 13,
};
constexpr uint32_t kFrameComponentsPerPage[14][9] = {
    { 64, 64, 64, 64, 64, 64, 64, 64, 64 },
    { 32, 64, 64, 64, 64, 64, 64, 64, 64 },
    { 16, 32, 48, 64, 64, 64, 64, 64, 64 },
    { 8, 16, 24, 40, 64, 64, 64, 64, 64 },
    { 5, 10, 16, 26, 42, 64, 64, 64, 64 },
    { 4, 8, 12, 20, 32, 52, 64, 64, 64 },
    { 3, 6, 9, 16, 25, 41, 64, 64, 64 },
    { 2, 5, 8, 13, 21, 34, 56, 64, 64 },
    { 2, 4, 6, 11, 18, 29, 48, 64, 64 },
    { 2, 4, 6, 10, 16, 26, 42, 64, 64 },
    { 1, 3, 4, 8, 12, 20, 33, 54, 64 },
    { 1, 2, 4, 6, 10, 17, 28, 45, 64 },
    { 1, 2, 3, 5, 9, 14, 24, 38, 64 },
    { 1, 2, 3, 5, 8, 13, 21, 34, 64 },
};
constexpr uint32_t kFrameComponentsAggregate[14][9] = {
    { 64, 128, 192, 256, 320, 384, 448, 512, 576 },
    { 32, 96, 160, 224, 288, 352, 416, 480, 544 },
    { 16, 48, 96, 160, 224, 288, 352, 416, 480 },
    { 8, 24, 48, 88, 152, 216, 280, 344, 408 },
    { 5, 15, 31, 57, 99, 163, 227, 291, 355 },
    { 4, 12, 24, 44, 76, 128, 192, 256, 320 },
    { 3, 9, 18, 34, 59, 100, 164, 228, 292 },
    { 2, 7, 15, 28, 49, 83, 139, 203, 267 },
    { 2, 6, 12, 23, 41, 70, 118, 182, 246 },
    { 2, 6, 12, 22, 38, 64, 106, 170, 234 },
    { 1, 4, 8, 16, 28, 48, 81, 135, 199 },
    { 1, 3, 7, 13, 23, 40, 68, 113, 177 },
    { 1, 3, 6, 11, 20, 34, 58, 96, 160 },
    { 1, 3, 6, 11, 19, 32, 53, 87, 151 },
};

// ecs::FrameAllocator: the current page, the page size, the critical
// section, the arrays of pages (pointer, capacity at +8, size at +0xc).
// A page is 64-byte aligned memory from posix_memalign, released with
// free: a 64-byte header, zeroed, whose first word is the offset of the
// next allocation, 0x40 on a new page. Sizes are rounded up to 64.
constexpr size_t kFrameAllocatorCurrentPage = 0x0;
constexpr size_t kFrameAllocatorPageSize = 0x8;
constexpr size_t kFrameAllocatorLock = 0x10;
constexpr size_t kFrameAllocatorPages = 0x28;
constexpr size_t kFrameAllocatorOldPages = 0x38;
constexpr size_t kFramePageHeader = 0x40;
constexpr size_t kFramePageAlignment = 0x40;
constexpr uint32_t kFrameAllocationMask = 0xffc0;

// ecs::legacy::ImmediateWorldCache: the changes written this frame and
// those being read (each a mask of 0x880 bits, then an array of 0x880
// changes of 0x80 bytes each, one per type that is not one-frame), the
// world's component callbacks, the frame allocator, the world. 0x260 bytes.
// A type's changes: a paged hash map from the entity to its change (16
// bytes: the component, its place in the frame storage), whose values
// have 5 bits a page, then a frame storage of the components (3 bits a
// page).
constexpr size_t kCacheSize = 0x260;
constexpr size_t kCacheWrite = 0x0;
constexpr size_t kCacheRead = 0x120;
constexpr size_t kChangesComponents = 0x110;
constexpr size_t kChangesComponentsShift = 7;
constexpr size_t kCacheCallbacks = 0x240;
constexpr size_t kCacheAllocator = 0x248;
constexpr size_t kCacheWorld = 0x250;
constexpr size_t kComponentChangesMap = 0x0;
constexpr size_t kComponentChangesValueBits = 0x40;
constexpr size_t kComponentChangesStorage = 0x48;
constexpr uint32_t kComponentChangesMapBits = 5;
constexpr uint32_t kComponentChangesStorageBits = 3;
constexpr size_t kCallbacksArray = 0x0;

// ecs::QueryDescription: after the hash map of the storages it matches
// (values 0x40 bytes each: the storage at +0x30), the component types
// (16-bit), the flags, then the ends of each group of types in that list.
// 0x58 bytes. ecs::QueryRegistry: the queries, then the persistent ones.
constexpr size_t kQuerySize = 0x58;
constexpr size_t kQueryComponents = 0x40;
constexpr size_t kQueryFlags = 0x48;
constexpr size_t kQueryEnds = 0x4c;
constexpr size_t kQueryEndsLast = 0x55;
constexpr size_t kQueryStorageSize = 0x40;
constexpr size_t kQueryStorageStorage = 0x30;
// A query's storage entry: its component slots in the storage (8 inline,
// else a pointer to them) and their number.
constexpr size_t kQueryStorageSlots = 0x20;
constexpr size_t kQueryStorageSlotCount = 0x2d;
constexpr uint32_t kQueryStorageInlineSlots = 8;
constexpr size_t kRegistryQueries = 0x0;
constexpr size_t kRegistryPersistent = 0x10;      // with the modified ones
constexpr size_t kRegistryAdded = 0x20;           // Norbyte's AliveQueries
constexpr size_t kRegistryRemoved = 0x30;
constexpr size_t kRegistryDead = 0x40;
constexpr size_t kRegistryFrameData = 0x60;
// ecs::EntityWorld: the dirty fields of the replication first, the systems
// (after the registry's vtable: the array of entries, 0xf8 bytes each),
// the active systems, the time given to the systems, the active byte, the
// queries. ecs::core::SystemTypeEntry: the system, the activated byte, the
// update function, called as (system, world, time).
constexpr size_t kWorldReplication = 0x0;
constexpr size_t kWorldSystems = 0x30;
constexpr size_t kWorldActiveSystems = 0x48;
constexpr size_t kWorldTime = 0x200;
constexpr size_t kWorldActive = 0x251;
constexpr size_t kWorldQueries = 0x258;
// The command buffers, one per thread, by the thread's index.
constexpr size_t kWorldCommandBuffers = 0x230;
constexpr size_t kCommandsSize = 0xc0;
constexpr size_t kSystemEntrySize = 0xf8;
constexpr size_t kSystemEntrySystem = 0x0;
constexpr size_t kSystemEntryActivated = 0x12;
constexpr size_t kSystemEntryUpdate = 0x18;

// ecs::sync::EntitiesDirtyFieldsBuffer: an array of hash maps (0x40 bytes
// each) from an entity to the bit set of its dirty fields, then the dirty
// byte. ecs::sync::EntityReplicationPeer and EntityReplicationAuthority:
// the hash map from a network id to the entity. The client keeps its peer
// and the server its authority inside the network object.
constexpr size_t kDirtyFieldsPools = 0x0;
constexpr size_t kDirtyFieldsDirty = 0x10;
constexpr size_t kPeerNetIdToEntity = 0x18;
constexpr size_t kPeerEntityToNetId = 0x58;
constexpr size_t kPeerDeserializers = 0x98;
// A component's deserializer: after its vtable pointer, the sparse array of
// its field trackers (the mask, a bit set, then the array of pointers). A
// field tracker: after its vtable pointer, the signal of a change (the
// next id, then the connections: pointer at +8, size at +0x14), then the
// hash map from an entity to its change (0x18 bytes each). Its slot 4 is
// the one that fires the changes of all entities (FireEvents).
constexpr size_t kDeserializerTrackers = 0x8;
constexpr size_t kSparseArrayValues = 0x10;
constexpr size_t kFieldTrackerChangeEvent = 0x8;
constexpr size_t kFieldTrackerChanges = 0x20;
constexpr size_t kFieldChangeSize = 0x18;
constexpr size_t kFieldTrackerFireSlot = 4;
constexpr size_t kAuthorityEntityToNetId = 0x90;
constexpr size_t kAuthorityNetIdToEntity = 0xd0;
constexpr size_t kClientPeer = 0x400;
constexpr size_t kServerAuthority = 0x3a8;
constexpr size_t kEocClientGameClient = 0x90;

}
