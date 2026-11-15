#include <stdafx.h>
#include <GameDefinitions/Ai.h>
#include <GameDefinitions/AllSpark.h>
#include <GameDefinitions/Animation.h>
#include <GameDefinitions/Character.h>
#include <GameDefinitions/Cursor.h>
#include <GameDefinitions/Dialog.h>
#include <GameDefinitions/DragDrop.h>
#include <GameDefinitions/Item.h>
#include <GameDefinitions/Level.h>
#include <GameDefinitions/Misc.h>
#include <GameDefinitions/Components/All.h>
#include <GameDefinitions/Controllers/Input.h>
#include <GameDefinitions/EntitySystem.h>
#include <GameDefinitions/FileReader.h>
#include <GameDefinitions/GameState.h>
#include <GameDefinitions/Module.h>
#include <GameDefinitions/Input.h>
#include <GameDefinitions/Net.h>
#include <GameDefinitions/Render.h>
#include <GameDefinitions/Resources.h>
#include <GameDefinitions/RootTemplates.h>
#include <GameDefinitions/Sound.h>
#include <GameDefinitions/Stats/Prototype.h>
#include <GameDefinitions/Stats/Stats.h>
#include <GameDefinitions/Stats/Expression.h>
#include <GameDefinitions/Status.h>
#include <GameDefinitions/Symbols.h>
#include <GameDefinitions/TranslatedString.h>
#include <GameDefinitions/UI.h>
#include <GameDefinitions/VirtualTextureFormat.h>

#include "GameAudio.h"
#include "GameCamera.h"
#include "GameCharacter.h"
#include "GameComponents.h"
#include "GameEffectsManager.h"
#include "GameEntityWorld.h"
#include "GameFiles.h"
#include "GameGlobals.h"
#include "GameMembers.h"
#include "GameNoesis.h"
#include "GameQueues.h"
#include "GameResourceManager.h"
#include "GameSession.h"
#include "GameStats.h"
#include "GameStringTable.h"
#include "GameSwitches.h"
#include "GameSystems.h"
#include "GameUi.h"
#include "GameVirtualResults.h"

// offsetof on Norbyte's structures, which are not standard-layout (most have
// a base class): what his property maps do throughout, and what clang
// supports for them.
#pragma clang diagnostic ignored "-Winvalid-offsetof"

// What Norbyte's definitions must come to in this build for the Mac shapes
// of Mac/Layout to be the shapes of his structures: asserted here, where the
// definitions are compiled as his sources are. This file has no code.

BEGIN_SE()

// Base/Lock.h: the two recursive locks are the Mac game's. Each has three
// members whose sizes leave one possible layout at this size.
static_assert(sizeof(SRWLock) == sizeof(mac::GameRecursiveKernelLock) && alignof(SRWLock) == alignof(mac::GameRecursiveKernelLock));
static_assert(sizeof(SRWSpinLock) == sizeof(mac::GameRecursiveSpinLock) && alignof(SRWSpinLock) == alignof(mac::GameRecursiveSpinLock));

// Animation.h: GenomeManager spells the recursive kernel lock out as three members.
static_assert(offsetof(gn::GenomeManager, LockThreadID) - offsetof(gn::GenomeManager, BlueprintInstantiationRWLock)
    == offsetof(mac::GameRecursiveKernelLock, OwnerThread));
static_assert(offsetof(gn::GenomeManager, LockWriteEnterCount) - offsetof(gn::GenomeManager, BlueprintInstantiationRWLock)
    == offsetof(mac::GameRecursiveKernelLock, WriteEnterCount));
static_assert(offsetof(gn::GenomeManager, SourceReaderPlugins) - offsetof(gn::GenomeManager, BlueprintInstantiationRWLock)
    == sizeof(mac::GameRecursiveKernelLock));

// Base/Base.h: std::variant is the Mac game's variant wherever Norbyte's
// definitions and bindings name it, so the templates written for
// std::variant<Args...> (Base/TypeMetadata.h, Base/BaseTypeInformation.h and
// the Lua helpers) are the ones chosen for the game's variants.
static_assert(std::is_same_v<std::variant<int, STDString>, mac::GameVariant<int, STDString>>);
static_assert(IsVariantLike<mac::GameVariant<int, STDString>> && IsVariant<mac::GameVariant<int, STDString>>);
// The variant Norbyte could not declare on Windows (Controllers/Behaviour.h,
// BSWander): 16 bytes, the index after the vector.
static_assert(sizeof(std::variant<EntityHandle, glm::vec3>) == 16 && alignof(std::variant<EntityHandle, glm::vec3>) == 8);

// EntitySystem.h: the lock-free queues, on the Mac game's 128-byte lines.
using CheckedQueue = MPMCQueueBounded<short>;
using CheckedLockedQueue = SynchronizedMPMCQueueBounded<void*>;
static_assert(sizeof(MPMCQueueSlot<short>) == mac::kQueueLine && alignof(MPMCQueueSlot<short>) == mac::kQueueLine);
static_assert(offsetof(MPMCQueueSlot<short>, Value) == mac::kQueueSlotValue);
static_assert(sizeof(MPMCQueueSlot<std::array<uint64_t, 20>>) == 2 * mac::kQueueLine);
static_assert(sizeof(CheckedQueue) == mac::kQueueSize && alignof(CheckedQueue) == mac::kQueueLine);
static_assert(offsetof(CheckedQueue, Capacity) == 0 && offsetof(CheckedQueue, Slots) == 8);
static_assert(offsetof(CheckedQueue, WriteIndex) == mac::kQueueWriteIndex && offsetof(CheckedQueue, ReadIndex) == mac::kQueueReadIndex);
static_assert(sizeof(CheckedLockedQueue) == mac::kLockedQueueSize && alignof(CheckedLockedQueue) == mac::kQueueLine);
static_assert(offsetof(CheckedLockedQueue, Lock) == 0 && offsetof(CheckedLockedQueue, Queue) == mac::kLockedQueueQueue);
static_assert(sizeof(ecs::EntityHandleGenerator::ThreadState) == mac::kHandleGeneratorThreadState);

// A structure that derives from ProtectedGameObject and whose first member
// does too begins with that member, as under MSVC: the two empty bases are
// of different types here (CoreLib/Base/BaseUtilities.h), so the member is
// not moved away from the base.
static_assert(offsetof(ecs::EntityHandleGenerator, ThreadStates) == 0);
static_assert(sizeof(ecs::EntityHandleGenerator) == 0x40 * mac::kHandleGeneratorThreadState);
static_assert(offsetof(ui::DeferredPredicatesManager, Queue) == 0);
static_assert(offsetof(ui::DeferredPropertiesManager, PropertyChanges) == 0);

// VirtualTextureFormat.h: the page file record of a .gts file starts with
// 256 16-bit characters, as the file has them.
static_assert(sizeof(vt::GTSPageFileInfo::FileNameBuf) == 512 && offsetof(vt::GTSPageFileInfo, NumPages) == 512);
static_assert(std::is_trivially_copyable_v<vt::GTSPageFileInfo> && std::is_standard_layout_v<vt::GTSPageFileInfo>);

// Symbols.h: the two Noesis locks are reached as the spin locks they are.
static_assert(std::is_same_v<decltype(StaticSymbols::Noesis__Reflection__Lock), mac::GameNoesisSpinLock*>);
static_assert(std::is_same_v<decltype(StaticSymbols::Noesis__SymbolManager__Lock), mac::GameNoesisSpinLock*>);
// ... and the owner word of the reflection lock is the 32 bits GetCurrentThreadId() returns.
static_assert(std::is_same_v<decltype(StaticSymbols::Noesis__Reflection__LockedByThreadId), std::atomic<uint32_t>*>);
static_assert(sizeof(DWORD) == sizeof(uint32_t));

// EntitySystem.h: the component registry and the members of EntityWorld
// whose place a game function shows. Nothing was edited to get these: they
// follow from the Mac locks and queues inside the structures before them.
static_assert(sizeof(ecs::ComponentTypeEntry) == mac::kComponentTypeEntrySize);
static_assert(offsetof(ecs::ComponentTypeEntry, InlineSize) == mac::kComponentTypeEntrySizes
    && offsetof(ecs::ComponentTypeEntry, TotalSize) == mac::kComponentTypeEntrySizes + 2);
static_assert(offsetof(ecs::ComponentTypeEntry, DependentComponentIndices) == mac::kComponentTypeEntryDependents
    && offsetof(ecs::ComponentTypeEntry, DependencyComponentIndices) == mac::kComponentTypeEntryDependencies);
static_assert(offsetof(ecs::EntityWorld, ComponentRegistry_) + offsetof(ecs::ComponentRegistry, Types) == mac::kWorldComponentTypes);
static_assert(offsetof(ecs::EntityWorld, ComponentCallbacks) == mac::kWorldComponentCallbacks);
static_assert(offsetof(ecs::EntityWorld, HandleGenerator) == mac::kWorldHandleGenerator);
static_assert(offsetof(ecs::EntityWorld, Storage) == mac::kWorldStorage);
static_assert(offsetof(ecs::EntityWorld, ComponentOps) == mac::kWorldComponentOps);
static_assert(offsetof(ecs::EntityWorld, Cache) == mac::kWorldCache);
// The way from an entity to a component, which EntitySystemHelpers walks as
// the game's own lookup does.
static_assert(offsetof(ecs::EntityStorageContainer, Storages) == mac::kStorageStorages
    && offsetof(ecs::EntityStorageContainer, Salts) == mac::kStorageSalts
    && sizeof(PagedArray<ecs::EntityStorageContainer::TypeSalt>) == (size_t(1) << mac::kStorageSaltsShift)
    && sizeof(ecs::EntityStorageContainer::TypeSalt) == (size_t(1) << mac::kSaltShift)
    && offsetof(ecs::EntityStorageContainer::TypeSalt, StorageIndex) == mac::kSaltStorageIndex);
static_assert(offsetof(ecs::EntityStorageData, Components) == mac::kStorageDataComponents
    && offsetof(ecs::EntityStorageData, ComponentTypeToIndex) == mac::kStorageDataComponentSlots
    && offsetof(ecs::EntityStorageData, InstanceToPageMap) == mac::kStorageDataEntities
    && sizeof(ecs::EntityStorageData::ComponentTypeToIndex) == mac::kStorageDataEntities - mac::kStorageDataComponentSlots
    && sizeof(ecs::EntityStorageComponentPage::ComponentInfo) == (size_t(1) << mac::kComponentPageSlotShift)
    && offsetof(ecs::EntityStorageComponentPage::ComponentInfo, ComponentBuffer) == 0);
// The signals of a component type, to which Lua/Shared/EntityComponentEvents.inl
// adds connections of its own for the game to call.
static_assert(offsetof(ecs::ComponentOps, Callbacks) == mac::kComponentOpsSignals && offsetof(ecs::ComponentOps, World) == mac::kComponentOpsWorld);
static_assert(offsetof(ecs::ComponentCallbacks, OnConstruct) == mac::kSignalsAttached
    && offsetof(ecs::ComponentCallbacks, OnDestroy) == mac::kSignalsDetached
    && sizeof(ecs::ComponentSignal) == mac::kSignalsDetached - mac::kSignalsAttached
    && offsetof(ecs::ComponentSignal, Connections) == mac::kSignalConnections
    && sizeof(ecs::ComponentSignal::Connection) == mac::kSignalConnectionSize
    && offsetof(ecs::ComponentSignal::Connection, Handler) == 0
    && offsetof(ecs::ComponentSignal::Connection, RegistrantIndex) == mac::kSignalConnectionSize - sizeof(uint64_t));

// Base/BitSet.h: the game's bit sets keep the size before the capacity.
static_assert(sizeof(BitSet<>) == 0x10 && offsetof(BitSet<>, Buf) == 0
    && offsetof(BitSet<>, Size) == mac::kBitSetSize && offsetof(BitSet<>, Capacity) == mac::kBitSetCapacity);

// EntitySystem.h: the handles, and the rest of a storage that his code reads.
using CheckedThreadState = ecs::EntityHandleGenerator::ThreadState;
static_assert(offsetof(CheckedThreadState, Entries) == mac::kPagedArrayPages
    && offsetof(CheckedThreadState, NextFreeSlotIndex) == mac::kHandleNextFree
    && offsetof(CheckedThreadState, LastFreeSlotIndex) == mac::kHandleLastFree
    && offsetof(CheckedThreadState, NumFreeSlots) == mac::kHandleFreeCount
    && sizeof(PagedArray<CheckedThreadState::Entry>) == mac::kPagedArraySize + sizeof(uint32_t)
    && offsetof(CheckedThreadState::Entry, Index) == 0 && offsetof(CheckedThreadState::Entry, Salt) == 4);
static_assert(EntityHandle(uint64_t(1) << mac::kHandleThreadShift).GetThreadIndex() == 1
    && EntityHandle(mac::kHandleFirstSalt).GetSalt() == 1 && EntityHandle(mac::kHandleFirstSalt).GetIndex() == 0);
static_assert(offsetof(ecs::EntityStorageData, GroupMask) == mac::kStorageDataGroupMask
    && offsetof(ecs::EntityStorageData, ComponentSizes) == mac::kStorageDataComponentSizes
    && offsetof(ecs::EntityStorageData, ComponentDtors) == mac::kStorageDataComponentDtors
    && sizeof(ecs::EntityStorageData::ComponentEntry) == (size_t(1) << mac::kComponentDtorEntryShift)
    && offsetof(ecs::EntityStorageData::ComponentEntry, DtorProc) == mac::kComponentDtorEntryProc
    && offsetof(ecs::EntityStorageData, StorageIndex) == mac::kStorageDataStorageIndex
    && offsetof(ecs::EntityStorageData, ComponentIndexListSize) == mac::kStorageDataSlotCount
    && offsetof(ecs::EntityStorageData, Handles) == mac::kStorageDataHandles
    && offsetof(ecs::EntityStorageData, PageInfos) == mac::kStorageDataPageInfos
    && sizeof(ecs::EntityStorageData::PageInfo) == 0x10
    && offsetof(ecs::EntityStorageData::PageInfo, UsedMask) == mac::kPageInfoUsedMask
    && offsetof(ecs::EntityStorageData, OneFrameComponents) == mac::kStorageDataOneFrameComponents
    && offsetof(ecs::EntityStorageData, HasOneFrameComponents) == mac::kStorageDataHasOneFrameComponents
    && offsetof(ecs::EntityStorageData, ModifiedComponents) == mac::kStorageDataModifiedComponents
    && sizeof(ecs::EntityStorageData::ModifiedComponents) == 0x20
    && offsetof(ecs::EntityStorageComponentPage::ComponentInfo, ModifiedEntities) == mac::kComponentPageSlotModified);
static_assert(offsetof(ecs::EntityStorageContainer, UsedFrameDataStorages) == mac::kStorageUsedFrameData);
// SparseHashMapHash and HashMapHash of a ComponentTypeIndex (EntitySystem.h)
// place a one-frame type after this many others, and the latter repeats the
// place in the high half, as the game's lookups do.
static_assert(ecs::ComponentMapSize == mac::kComponentTypeCount);
static_assert(sizeof(HashMap<ecs::ComponentTypeIndex, HashMap<EntityHandle, void*>>) == mac::kHashMapSize);

// The command buffers and what they allocate from.
static_assert(offsetof(ecs::EntityCommandBuffer, HandleGenerator) == mac::kCommandsHandleGenerator
    && offsetof(ecs::EntityCommandBuffer, Allocator) == mac::kCommandsAllocator
    && offsetof(ecs::EntityCommandBuffer, Data) + offsetof(ecs::ECBData, EntityChanges) == mac::kCommandsChanges
    && offsetof(ecs::EntityCommandBuffer, Data) + offsetof(ecs::ECBData, ComponentPools) == mac::kCommandsPools);
using CheckedPools = decltype(ecs::ECBData::ComponentPools);
static_assert(offsetof(CheckedPools, Keys) == mac::kPoolsKeys - sizeof(void*)
    && offsetof(CheckedPools, Values) == mac::kPoolsValues - sizeof(void*)
    && sizeof(CheckedPools::LookupTable) == mac::kPoolsKeys - sizeof(void*));
static_assert(sizeof(ecs::ECBEntityChangeSet) == mac::kEntityChangeSize
    && offsetof(ecs::ECBEntityChangeSet, Store) == mac::kEntityChangeStore
    && offsetof(ecs::ECBEntityChangeSet, Flags) == mac::kEntityChangeFlags
    && offsetof(ecs::ECBEntityChangeSet, field_2A) == mac::kEntityChangeLast
    && (uint32_t)ecs::EntityChangeFlags::Create == mac::kEntityChangeCreate
    && (uint32_t)ecs::EntityChangeFlags::Destroy == mac::kEntityChangeDestroy
    // His flags are the low byte of the game's 16-bit word; the high byte is
    // zeroed when the change is made, and none of his flags is in it.
    && sizeof(ecs::ECBEntityComponentChange) == 8
    && offsetof(ecs::ECBEntityComponentChange, ComponentTypeId) * 8 == mac::kComponentChangeTypeShift);
static_assert(sizeof(ecs::ComponentFrameStorage) == mac::kFrameStorageSize
    && offsetof(ecs::ComponentFrameStorage, NumComponents) == mac::kFrameStorageCount
    && offsetof(ecs::ComponentFrameStorage, ComponentSizeInBytes) == mac::kFrameStorageComponentSize
    && offsetof(ecs::ComponentFrameStorage, ComponentTypeId) == mac::kFrameStorageType
    && offsetof(ecs::ComponentFrameStorage, DestructorProc) == mac::kFrameStorageDtor);
static_assert(offsetof(ecs::FrameAllocator, CurrentPage) == mac::kFrameAllocatorCurrentPage
    && offsetof(ecs::FrameAllocator, PageSize) == mac::kFrameAllocatorPageSize
    && offsetof(ecs::FrameAllocator, CS) == mac::kFrameAllocatorLock
    && offsetof(ecs::FrameAllocator, FrameBuffers) == mac::kFrameAllocatorPages
    && offsetof(ecs::FrameAllocator, FrameBuffers) + sizeof(Array<void*>) == mac::kFrameAllocatorOldPages);
static_assert(offsetof(ecs::ImmediateWorldCache, WriteChanges) == mac::kCacheWrite
    && offsetof(ecs::ImmediateWorldCache, ReadChanges) == mac::kCacheRead
    && offsetof(ecs::ImmediateWorldCache::Changes, ComponentsByType) == mac::kChangesComponents
    && sizeof(ecs::ImmediateWorldCache::ComponentChanges) == (size_t(1) << mac::kChangesComponentsShift)
    && sizeof(ecs::ImmediateWorldCache::Changes::AvailableComponentTypes) * 8 == mac::kComponentTypeCount
    && offsetof(ecs::ImmediateWorldCache, Callbacks) == mac::kCacheCallbacks
    && offsetof(ecs::ImmediateWorldCache, Allocator) == mac::kCacheAllocator
    && offsetof(ecs::ImmediateWorldCache, EntityWorld) == mac::kCacheWorld
    && offsetof(ecs::ImmediateWorldCache::ComponentChanges, Components) == mac::kComponentChangesMap
    && offsetof(ecs::ImmediateWorldCache::ComponentChanges, FrameStorage) == mac::kComponentChangesStorage
    && sizeof(ecs::ImmediateWorldCache::ComponentChange) == 0x10
    && offsetof(ecs::ComponentCallbackRegistry, Callbacks) == mac::kCallbacksArray);

// The queries and the systems.
static_assert(sizeof(ecs::QueryDescription) == mac::kQuerySize
    && offsetof(ecs::QueryDescription, ComponentIndices) == mac::kQueryComponents
    && offsetof(ecs::QueryDescription, Flags) == mac::kQueryFlags
    && offsetof(ecs::QueryDescription, IncludeAny) == mac::kQueryEnds
    && offsetof(ecs::QueryDescription, WriteEnd) == mac::kQueryEndsLast
    && sizeof(ecs::StorageComponentMap) == mac::kQueryStorageSize
    && offsetof(ecs::StorageComponentMap, Storage) == mac::kQueryStorageStorage
    && offsetof(ecs::StorageComponentMap, ComponentIndices) == mac::kQueryStorageSlots
    && sizeof(ecs::StorageComponentMap::InlineComponentIndices) == mac::kQueryStorageInlineSlots
    && offsetof(ecs::StorageComponentMap, NumComponents) == mac::kQueryStorageSlotCount
    && offsetof(ecs::QueryRegistry, Queries) == mac::kRegistryQueries
    && offsetof(ecs::QueryRegistry, PersistentQueries) == mac::kRegistryPersistent
    && offsetof(ecs::QueryRegistry, AliveQueries) == mac::kRegistryAdded
    && offsetof(ecs::QueryRegistry, RemovedQueries) == mac::kRegistryRemoved
    && offsetof(ecs::QueryRegistry, DeadQueries) == mac::kRegistryDead
    && offsetof(ecs::QueryRegistry, FrameData) == mac::kRegistryFrameData);
static_assert(offsetof(ecs::EntityWorld, CommandBuffers) == mac::kWorldCommandBuffers
    && sizeof(ecs::EntityCommandBuffer) == mac::kCommandsSize);
static_assert(offsetof(ecs::EntityWorld, Replication) == mac::kWorldReplication
    && offsetof(ecs::EntityWorld, Systems) + offsetof(ecs::SystemRegistry, Systems) == mac::kWorldSystems
    && offsetof(ecs::EntityWorld, ActiveSystems) == mac::kWorldActiveSystems
    && offsetof(ecs::EntityWorld, Time) == mac::kWorldTime
    && offsetof(ecs::EntityWorld, Active) == mac::kWorldActive
    && offsetof(ecs::EntityWorld, Queries) == mac::kWorldQueries
    && sizeof(ecs::SystemTypeEntry) == mac::kSystemEntrySize
    && offsetof(ecs::SystemTypeEntry, System) == mac::kSystemEntrySystem
    && offsetof(ecs::SystemTypeEntry, Activated) == mac::kSystemEntryActivated
    && offsetof(ecs::SystemTypeEntry, UpdateProc) == mac::kSystemEntryUpdate);

// Replication.
static_assert(offsetof(ecs::SyncBuffers, ComponentPools) == mac::kDirtyFieldsPools
    && offsetof(ecs::SyncBuffers, Dirty) == mac::kDirtyFieldsDirty
    && sizeof(HashMap<EntityHandle, BitSet<>>) == mac::kHashMapSize);
static_assert(offsetof(ecs::ComponentDeserializer, FieldTrackers) == mac::kDeserializerTrackers
    && sizeof(SparseArray<ecs::FieldTracker*>) == mac::kSparseArrayValues + sizeof(Array<ecs::FieldTracker*>)
    && offsetof(ecs::FieldTracker, ChangeEvent) == mac::kFieldTrackerChangeEvent
    && offsetof(ecs::FieldTracker, Changes) == mac::kFieldTrackerChanges
    && sizeof(ecs::FieldTracker::FieldChangeData) == mac::kFieldChangeSize
    && sizeof(decltype(ecs::FieldTracker::ChangeEvent)::Connection) == mac::kSignalConnectionSize
    && offsetof(decltype(ecs::FieldTracker::ChangeEvent), Connections) == mac::kSignalConnections);
static_assert(offsetof(ecs::EntityReplicationPeer, NetIdToEntity) == mac::kPeerNetIdToEntity
    && offsetof(ecs::EntityReplicationPeer, EntityToNetId) == mac::kPeerEntityToNetId
    && offsetof(ecs::EntityReplicationPeer, Deserializers) == mac::kPeerDeserializers
    && offsetof(ecs::EntityReplicationAuthority, EntityToNetId) == mac::kAuthorityEntityToNetId
    && offsetof(ecs::EntityReplicationAuthority, NetIdToEntity) == mac::kAuthorityNetIdToEntity
    && offsetof(net::GameClient, ReplicationPeer) == mac::kClientPeer
    && offsetof(net::GameServer, Replication) == mac::kServerAuthority
    && offsetof(ecl::EoCClient, GameClient) == mac::kEocClientGameClient);

// Components/Effect.h: the effects manager. The one edit is the width of the
// creating thread's id; the rest follows from the Mac critical section.
static_assert(sizeof(EffectsManager) <= mac::kEffectsManagerSize);
static_assert(offsetof(EffectsManager, OverlayMaterialRequestSignal) + sizeof(uint64_t) == mac::kEffectsSignalConnections);
static_assert(offsetof(EffectsManager, CanUpdate) == mac::kEffectsCanUpdate);
static_assert(offsetof(EffectsManager, FxCreateThreadId) == mac::kEffectsCreateThread && sizeof(EffectsManager::FxCreateThreadId) == 8);
static_assert(offsetof(EffectsManager, DestroyEffectData) == mac::kEffectsDestroyData);
static_assert(offsetof(EffectsManager, Invokes) == mac::kEffectsInvokes);
static_assert(offsetof(EffectsManager, InvokesCS) == mac::kEffectsInvokesLock);
static_assert(offsetof(EffectsManager, SoundObjectsOnLevel) == mac::kEffectsSoundObjects);
static_assert(offsetof(EffectsManager, RCB_CS) == mac::kEffectsCommandBufferLock);
static_assert(offsetof(EffectsManager, RCB) == mac::kEffectsCommandBuffer);
static_assert(offsetof(EffectsManager, RCBs) == mac::kEffectsCommandBuffers);
static_assert(offsetof(EffectsManager, AddOverlayMaterialRequestsCS) == mac::kEffectsOverlayRequestsLock);
static_assert(offsetof(EffectsManager, OverlayMaterialRequests) == mac::kEffectsOverlayRequests);
static_assert(offsetof(EffectsManager, NonConstructedEffectsCS) == mac::kEffectsNonConstructedLock);
static_assert(offsetof(EffectsManager, NonConstructedEffects) == mac::kEffectsNonConstructed);
static_assert(offsetof(EffectsManager, RenderBatchesGlobalVB) == mac::kEffectsVertexBuffers
    && offsetof(EffectsManager, RibbonGlobalVB) == mac::kEffectsVertexBuffers + 16);

// Resources.h: the resource manager. The one edit is its first member, the
// lock that Norbyte's declaration has as a pointer-sized word.
static_assert(sizeof(ResourceManager) <= mac::kResourceManagerSize);
static_assert(offsetof(ResourceManager, Lock) == mac::kResourceManagerLock && sizeof(ResourceManager::Lock) == sizeof(mac::GameSRWKernelLock));
static_assert(offsetof(ResourceManager, PreviewResources) == mac::kResourceManagerPreviewResources);
static_assert(offsetof(ResourceManager, ResourceBanks) == mac::kResourceManagerBanks);
static_assert(offsetof(ResourceManager, EffectDependencies) == mac::kResourceManagerEffectDependencies);
static_assert(offsetof(ResourceManager, TextureManager) == mac::kResourceManagerTextureManager);
static_assert(offsetof(ResourceManager, SoundManager) == mac::kResourceManagerSoundManager);
static_assert(offsetof(ResourceManager, CriticalSection) == mac::kResourceManagerCriticalSection);
static_assert(offsetof(ResourceManager, ResourceDependencies) == mac::kResourceManagerDependencies);
static_assert(offsetof(ResourceManager, Sources) == mac::kResourceManagerSources);
static_assert(offsetof(ResourceManager, VisualLoaders) == mac::kResourceManagerVisualLoaders);
static_assert(offsetof(ResourceManager, GenomeAnimationManagers) == mac::kResourceManagerGenomeManagers);

// CoreLib/Base/BaseString.h: the table of fixed strings. The one edit is the
// end of a pool, after the lock. What his code reads of a pool, of the map
// and of an entry is where the game has it; his map's slots are half as wide
// as the game's, which nothing of his reads, so his table ends inside the
// game's.
using StringPool = GlobalStringTable::SubTable;
using StringMap = GlobalStringTable::MainTable;
static_assert(sizeof(StringPool) == mac::kStringPoolSize);
static_assert(sizeof(GlobalStringTable::SubTables) == mac::kStringPools * mac::kStringPoolSize && offsetof(GlobalStringTable, SubTables) == 0);
static_assert(offsetof(GlobalStringTable, Main) == mac::kStringTableMain);
static_assert(sizeof(StringPool::Element) == mac::kStringLine && offsetof(StringPool, field_0) == mac::kStringPoolEpochSlots);
static_assert(offsetof(StringPool, LockCounter) == mac::kStringPoolEpoch && offsetof(StringPool, LockEvent) == mac::kStringPoolEvent);
static_assert(offsetof(StringPool, TableIndex) == mac::kStringPoolIndex && sizeof(StringPool::TableIndex) == 4);
static_assert(offsetof(StringPool, EntrySize) == mac::kStringPoolEntrySize && sizeof(StringPool::EntrySize) == 8);
static_assert(offsetof(StringPool, EntriesPerBucket) == mac::kStringPoolEntriesPerBucket && sizeof(StringPool::EntriesPerBucket) == 4);
static_assert(offsetof(StringPool, NumBuckets) == mac::kStringPoolBucketCount && sizeof(StringPool::NumBuckets) == 4);
static_assert(offsetof(StringPool, field_1100) == mac::kStringPoolUsed);
static_assert(offsetof(StringPool, Buckets) == mac::kStringPoolBuckets && std::is_same_v<decltype(StringPool::Buckets), uint8_t**>);
static_assert(offsetof(StringPool, field_1180) == mac::kStringPoolFreeList);
static_assert(offsetof(StringPool, CriticalSection) == mac::kStringPoolLock
    && sizeof(StringPool::CriticalSection) == sizeof(mac::GameCriticalSection));
static_assert(offsetof(StringMap, field_1000) == mac::kStringMapEpoch && offsetof(StringMap, EventHandle) == mac::kStringMapEvent);
static_assert(offsetof(StringMap, StringTable) == mac::kStringMapStorage && offsetof(StringMap, field_10C0) == mac::kStringMapSlots);
static_assert(sizeof(GlobalStringTable) <= mac::kStringTableMain + mac::kStringMapSlots + mac::kStringMapSlotCount * sizeof(uint64_t));
static_assert(sizeof(FixedStringBase::Header) == mac::kStringEntryText && sizeof(GlobalStringTable::StringEntry) > mac::kStringEntryText);
static_assert(offsetof(FixedStringBase::Header, Hash) == mac::kStringEntryHash
    && offsetof(FixedStringBase::Header, RefCount) == mac::kStringEntryReferences && sizeof(FixedStringBase::Header::RefCount) == 4
    && offsetof(FixedStringBase::Header, Length) == mac::kStringEntryLength && sizeof(FixedStringBase::Header::Length) == 4
    && offsetof(FixedStringBase::Header, Id) == mac::kStringEntryId
    && offsetof(FixedStringBase::Header, NextFreeIndex) == mac::kStringEntryNext);
static_assert(offsetof(GlobalStringTable::StringEntry, Str) == mac::kStringEntryText);
static_assert(FixedStringBase::NullIndex == mac::kNullStringIndex && sizeof(FixedStringBase) == sizeof(uint32_t));
// The view the game's functions take and return: the characters, then a
// 32-bit length (a standard-layout class has its members in their order, the
// first at its start, so the length follows the pointer).
static_assert(std::is_standard_layout_v<LSStringView> && std::is_trivially_copyable_v<LSStringView>);
static_assert(sizeof(LSStringView) == 16 && alignof(LSStringView) == 8 && sizeof(LSStringView::size_type) == 4);
static_assert(sizeof(LSStringView::const_pointer) == mac::kStringViewLength);
// A path is its text, a string of this build's standard library, which says
// where the characters are in the string's last byte.
static_assert(offsetof(Path, Name) == 0 && sizeof(Path) == sizeof(STDString) && sizeof(STDString) - 1 == mac::kGamePathTextFlag);

// The members the engine table's operand checks and the call adapters name
// by offset (Layout/GameMembers.h): each is where his declaration has it.
// A map of his keeps its fields in a private base, which is the whole map.
using AtlasMapFields = RefMapInternals<STDString, TextureAtlas*>;
using IconMapFields = MapInternals<FixedString, TextureAtlas*>;
static_assert(sizeof(decltype(TextureAtlasMap::AtlasMap)) == sizeof(AtlasMapFields)
    && sizeof(decltype(TextureAtlasMap::IconMap)) == sizeof(IconMapFields));
static_assert(offsetof(TextureAtlasMap, AtlasMap) + offsetof(AtlasMapFields, ItemCount) == mac::kAtlasMapItemCount);
static_assert(offsetof(TextureAtlasMap, IconMap) + offsetof(IconMapFields, HashSize) == mac::kIconMapHashSize
    && offsetof(TextureAtlasMap, IconMap) + offsetof(IconMapFields, HashTable) == mac::kIconMapHashTable
    && offsetof(TextureAtlasMap, IconMap) + offsetof(IconMapFields, ItemCount) == mac::kIconMapItemCount);
using PassiveMapFields = RefMapInternals<FixedString, stats::PassivePrototype>;
static_assert(sizeof(decltype(stats::PassivePrototypeManager::Passives)) == sizeof(PassiveMapFields));
static_assert(offsetof(stats::PassivePrototypeManager, Passives) + offsetof(PassiveMapFields, HashSize) == mac::kPassivesMapHashSize
    && offsetof(stats::PassivePrototypeManager, Passives) + offsetof(PassiveMapFields, HashTable) == mac::kPassivesMapHashTable);
using PassiveMapNode = MapNode<FixedString, stats::PassivePrototype>;
static_assert(offsetof(PassiveMapNode, Key) == mac::kPassiveNodeKey);
static_assert(offsetof(GlobalTemplateManager, Banks) == mac::kTemplateManagerBanks);
static_assert(offsetof(Visual, BlendShapeBase) == mac::kVisualBlendShapeBase && sizeof(Visual::BlendShapeBase) == 2);

// Base/BaseFunction.h: a function object is the game's ls::Function, a
// pointer to its methods and then the methods and the captures.
static_assert(sizeof(Function<void()>) == mac::kFunctionSize);
static_assert(offsetof(FunctionStorage<void()>, call_) == mac::kFunctionCall - mac::kFunctionCall
    && offsetof(FunctionStorage<void()>, copy_) == mac::kFunctionCopy - mac::kFunctionCall
    && offsetof(FunctionStorage<void()>, move_) == mac::kFunctionMoveDestroy - mac::kFunctionCall
    && offsetof(FunctionStorage<void()>, data_) == mac::kFunctionCaptures - mac::kFunctionCall
    && sizeof(FunctionStorage<void()>) == mac::kFunctionSize - mac::kFunctionCall);

// Status.h: the members of ecl::StatusVFX's subclasses that the game's code
// shows, at the offsets the engine table reads from that code. The object's
// first word is its vtable pointer, which is what tells the classes apart.
static_assert(offsetof(ecl::StatusVFX, VMT) == 0);
static_assert(offsetof(ecl::StatusVFXEffect, EffectHandler) == mac::kStatusVfxEffectHandler);
static_assert(offsetof(ecl::StatusVFXApplyEffect, EffectHandler) == mac::kStatusVfxEffectHandler);
static_assert(offsetof(ecl::StatusVFXBeam, EffectHandler) == mac::kStatusVfxEffectHandler);
static_assert(offsetof(ecl::StatusVFXBeam, Initialized) == mac::kStatusVfxBeamFlags
    && offsetof(ecl::StatusVFXBeam, Created) == mac::kStatusVfxBeamFlags + 1);
static_assert(offsetof(ecl::ManagedStatusVFX, EffectHandler) == mac::kManagedStatusVfxEffectHandler);
static_assert(offsetof(ecl::StatusVFXBoostMaterial, Created) == mac::kStatusVfxBoostMaterialCreated);
using PathMapFields = RefMapInternals<AiPathId, AiPath*>;
using PathMapNode = MapNode<AiPathId, AiPath*>;
static_assert(sizeof(decltype(AiGrid::PathMap)) == sizeof(PathMapFields) && sizeof(AiPathId) == 4);
static_assert(offsetof(AiGrid, PathMap) + offsetof(PathMapFields, HashSize) == mac::kAiGridPathBuckets
    && offsetof(AiGrid, PathMap) + offsetof(PathMapFields, HashTable) == mac::kAiGridPathTable);
static_assert(offsetof(PathMapNode, Next) == mac::kAiPathNodeNext && offsetof(PathMapNode, Key) == mac::kAiPathNodeId
    && offsetof(PathMapNode, Value) == mac::kAiPathNodePath);
static_assert(offsetof(resource::Resource, SourceFile) == mac::kGameResourceSourceFile
    && offsetof(resource::Resource, Guid) == mac::kGameResourceGuid);
static_assert(offsetof(resource::LoadableResource, State) == mac::kLoadableResourceState && sizeof(resource::LoadableResource::State) == 1);
static_assert(offsetof(resource::TextureResource, Streaming) == mac::kTextureResourceStreaming
    && offsetof(resource::TextureResource, SRGB) == mac::kTextureResourceSRGB
    && offsetof(resource::TextureResource, Type) == mac::kTextureResourceType && sizeof(resource::TextureResource::Type) == 1);
static_assert(offsetof(input::InputRawChange, Value) + offsetof(input::InputValue, State) == mac::kInputChangeState);

// Stats/Stats.h, Stats/Common.h, Stats/Entries.h: the stats object, its
// managers, a stats object, a list of modifiers, a modifier, a list of
// values (Layout/GameStats.h). Two edits: the random number generator's
// sixth word, and a list's name in its manager's last bytes.
using NamedObjects = stats::CNamedElementManager<stats::Object>;
using NameMapFields = HashMap<FixedString, int32_t>;
using IntMapFields = MapInternals<FixedString, int32_t>;
using IntMapNode = MapNode<FixedString, int32_t>;
using PointerRefMapFields = RefMapInternals<FixedString, void*>;
// A hash map's buckets, chains and keys are in its set, a base class whose
// members are not public: three arrays, each as large as the game's.
static_assert(sizeof(StaticArray<int32_t>) == mac::kHashMapNext - mac::kHashMapBuckets
    && sizeof(Array<int32_t>) == mac::kHashMapKeys - mac::kHashMapNext
    && sizeof(Array<FixedString>) == mac::kHashMapValues - mac::kHashMapKeys
    && sizeof(HashSet<FixedString>) == mac::kHashMapValues && std::is_base_of_v<HashSet<FixedString>, NameMapFields>);
static_assert(sizeof(Array<void*>) == 0x10 && sizeof(TrackedCompactSet<FixedString>) == 0x10);
// A set that stores its size: where the block starts before the elements
// (CoreLib/Base/LegacyArray.h, one edit).
static_assert(TrackedCompactSet<FixedString>::size_offset == mac::kSetHeaderBytes && TrackedCompactSet<int32_t>::size_offset == 0
    && !std::is_trivially_destructible_v<FixedString> && sizeof(FixedString) == sizeof(uint32_t));
static_assert(offsetof(NamedObjects, Values) == mac::kNamedManagerValues && offsetof(NamedObjects, NameToHandle) == mac::kNamedManagerNames
    && offsetof(NamedObjects, NextHandle) == mac::kNamedManagerNextHandle && sizeof(NamedObjects) == mac::kNamedManagerSize);
static_assert(offsetof(IntMapFields, HashSize) == mac::kMapBucketCount && offsetof(IntMapFields, HashTable) == mac::kMapBuckets
    && offsetof(IntMapFields, ItemCount) == mac::kMapItemCount && sizeof(IntMapFields) == mac::kMapSize
    && sizeof(LegacyMap<FixedString, int32_t>) == mac::kMapSize);
static_assert(offsetof(PointerRefMapFields, ItemCount) == mac::kRefMapItemCount && offsetof(PointerRefMapFields, HashSize) == mac::kRefMapBucketCount
    && offsetof(PointerRefMapFields, HashTable) == mac::kRefMapBuckets);
static_assert(offsetof(IntMapNode, Next) == mac::kMapNodeNext && offsetof(IntMapNode, Key) == mac::kMapNodeKey
    && offsetof(IntMapNode, Value) == mac::kMapNodeIntValue);
// A fixed string's bucket in such a map is its index, in a hash map its hash:
// BaseString.h and BaseMap.h say so, and the game's code does the same
// (RPGStats::InsertDataBuffer, CNamedElementManager::GetHandleByName).
static_assert(sizeof(FixedString) == 4);

static_assert(sizeof(stats::RPGStats) == mac::kStatsSize);
static_assert(offsetof(stats::RPGStats, ModifierValueLists) == mac::kStatsValueLists
    && offsetof(stats::RPGStats, ModifierLists) == mac::kStatsModifierLists
    && offsetof(stats::RPGStats, Objects) == mac::kStatsObjects
    && offsetof(stats::RPGStats, TreasureCategories) == mac::kStatsTreasureCategories
    && offsetof(stats::RPGStats, TreasureTables) == mac::kStatsTreasureTables
    && offsetof(stats::RPGStats, ItemTypes) == mac::kStatsItemTypes
    && sizeof(stats::ItemTypeManager) == mac::kNamedManagerSize + 8);
static_assert(offsetof(stats::RPGStats, StatsFunctors) == mac::kStatsFunctors
    && offsetof(stats::RPGStats, CurrentParsedValueList) == mac::kStatsCurrentValueList
    && offsetof(stats::RPGStats, CurrentParsedType) == mac::kStatsCurrentType
    && offsetof(stats::RPGStats, RNG) == mac::kStatsRandom
    && offsetof(stats::RPGStats, ExtraData) == mac::kStatsExtraData);
static_assert(offsetof(stats::RPGStats, CategoryMappings) == mac::kStatsCategoryMappings
    && offsetof(stats::RPGStats, WeaponCounters) == mac::kStatsWeaponCounters
    && offsetof(stats::RPGStats, SpellbookCounters) == mac::kStatsSpellbookCounters
    && offsetof(stats::RPGStats, ArmorCounters) == mac::kStatsArmorCounters
    && offsetof(stats::RPGStats, field_2F0) == mac::kStatsRefMap
    && offsetof(stats::RPGStats, TreasureRarities) == mac::kStatsTreasureRarities);
static_assert(offsetof(stats::RPGStats, FixedStrings) == mac::kStatsFixedStrings
    && offsetof(stats::RPGStats, Int64s) == mac::kStatsInt64s
    && offsetof(stats::RPGStats, GUIDs) == mac::kStatsGuids
    && offsetof(stats::RPGStats, Floats) == mac::kStatsFloats
    && offsetof(stats::RPGStats, TranslatedStrings) == mac::kStatsTranslatedStrings);
static_assert(offsetof(stats::RPGStats, EquipmentSetManager) == mac::kStatsEquipmentSets
    && offsetof(stats::RPGStats, SpellSetManager) == mac::kStatsSpellSets
    && offsetof(stats::RPGStats, ItemProgressionManager) == mac::kStatsItemProgression
    && offsetof(stats::RPGStats, ItemCombinationManager) == mac::kStatsItemCombinations);
static_assert(offsetof(stats::RPGStats, CurrentDataBuffer) == mac::kStatsCurrentBuffer
    && offsetof(stats::RPGStats, CurrentDataBufferPath) == mac::kStatsCurrentBufferPath
    && offsetof(stats::RPGStats, PreParsedDataBufferMap) == mac::kStatsBufferMap
    && offsetof(stats::RPGStats, PreParsedDataBuffers) == mac::kStatsBuffers
    && offsetof(stats::RPGStats, BloodTypes) == mac::kStatsBloodTypes);
static_assert(offsetof(stats::RPGStats, Loaded) == mac::kStatsLoaded && offsetof(stats::RPGStats, FastLock) == mac::kStatsLock
    && offsetof(stats::RPGStats, Conditions) == mac::kStatsConditions);
static_assert(offsetof(stats::PreParsedDataBuffer, Type) == mac::kStatsBufferType
    && offsetof(stats::PreParsedDataBuffer, field_28) == mac::kStatsBufferNext);

// A stats object's values are a vector of this build's standard library:
// the begin and end pointers the game's code reads.
static_assert(offsetof(stats::Object, IndexedProperties) == mac::kStatsObjectProperties && sizeof(stats::Object::IndexedProperties) == 0x18);
static_assert(offsetof(stats::Object, Name) == mac::kStatsObjectName
    && offsetof(stats::Object, Functors) == mac::kStatsObjectFunctors
    && offsetof(stats::Object, RollConditions) == mac::kStatsObjectRollConditions
    && offsetof(stats::Object, AIFlags) == mac::kStatsObjectAIFlags
    && offsetof(stats::Object, Requirements) == mac::kStatsObjectRequirements
    && offsetof(stats::Object, ComboProperties) == mac::kStatsObjectComboProperties
    && offsetof(stats::Object, ComboCategories) == mac::kStatsObjectComboCategories
    && offsetof(stats::Object, Using) == mac::kStatsObjectUsing && sizeof(stats::Object::Using) == 4
    && offsetof(stats::Object, ModifierListIndex) == mac::kStatsObjectModifierList
    && offsetof(stats::Object, Level) == mac::kStatsObjectLevel);
// A treasure category and its items, a treasure table and what it is made
// of: the sizes are what Norbyte's Lua readers allocate a new one by.
static_assert(offsetof(stats::TreasureCategory, Category) == mac::kTreasureCategoryName
    && offsetof(stats::TreasureCategory, Items) == mac::kTreasureCategoryItems && sizeof(stats::TreasureCategory::Items) == 0x18
    && sizeof(stats::TreasureCategory) == mac::kTreasureCategorySize);
static_assert(offsetof(stats::TreasureCategoryItem, Name) == mac::kTreasureItemName
    && offsetof(stats::TreasureCategoryItem, Priority) == mac::kTreasureItemNumbers
    && offsetof(stats::TreasureCategoryItem, MaxLevel) == mac::kTreasureItemLastNumber
    && offsetof(stats::TreasureCategoryItem, Categories) == mac::kTreasureItemCategories
    && sizeof(stats::TreasureCategoryItem::Categories) == 1 && std::is_signed_v<decltype(stats::TreasureCategoryItem::Categories)>
    && sizeof(stats::TreasureCategoryItem) == mac::kTreasureItemSize);
static_assert(offsetof(stats::TreasureTable, Name) == 0 && offsetof(stats::TreasureTable, SubTables) == mac::kTreasureTableSubTables
    && sizeof(stats::TreasureTable) == mac::kTreasureTableSize);
static_assert(offsetof(stats::TreasureSubTable, field_54) == mac::kTreasureSubTableName
    && sizeof(stats::TreasureSubTable) == mac::kTreasureSubTableSize);
static_assert(offsetof(stats::TreasureSubTableCategory, Index) == 0 && offsetof(stats::TreasureSubTableCategory, Frequency) == 4
    && sizeof(stats::TreasureSubTableCategory) == mac::kTreasureSubTableCategorySize);
static_assert(offsetof(stats::ModifierList, Attributes) == 0 && offsetof(stats::ModifierList, Name) == mac::kModifierListName
    && sizeof(stats::ModifierList) == mac::kNamedManagerSize);
static_assert(offsetof(stats::Modifier, Name) == mac::kModifierName && offsetof(stats::Modifier, EnumerationIndex) == mac::kModifierValueList
    && sizeof(stats::Modifier::EnumerationIndex) == 4);
// What a stats attribute set from its text writes (Binding/StatsFunctions.h):
// the elements of an object's functors and roll conditions, the functor
// lists of the stats by name, and the map a text is split into.
static_assert(sizeof(stats::FunctorGroup) == mac::kFunctorGroupSize && offsetof(stats::FunctorGroup, TextKey) == mac::kFunctorGroupTextKey
    && offsetof(stats::FunctorGroup, Functors) == mac::kFunctorGroupList);
static_assert(sizeof(stats::Object::RollCondition) == mac::kRollConditionSize
    && offsetof(stats::Object::RollCondition, Name) == mac::kRollConditionTextKey
    && offsetof(stats::Object::RollCondition, Conditions) == mac::kRollConditionCondition && sizeof(stats::ConditionId) == 4);
using FunctorListMapNode = MapNode<FixedString, stats::Functors*>;
static_assert(offsetof(FunctorListMapNode, Next) == mac::kMapNodeNext && offsetof(FunctorListMapNode, Key) == mac::kMapNodeKey
    && offsetof(FunctorListMapNode, Value) == mac::kMapNodePointerValue);
static_assert(sizeof(decltype(stats::RPGStats::StatsFunctors)) == mac::kMapSize);
static_assert(sizeof(HashMap<FixedString, STDString>) == mac::kHashMapSize && sizeof(STDString) == mac::kGameStringBytes);
static_assert(sizeof(Array<stats::FunctorGroup>) == 0x10 && sizeof(Array<stats::Object::RollCondition>) == 0x10);
// Stats/Prototype.h, Stats/Functors.h: a passive's and an interrupt's
// prototype, a functor list and a functor, as the code that fills a
// prototype from its stats object reads and writes them
// (Binding/StatsFunctions.h).
static_assert(sizeof(stats::Functors) == 0x60 && offsetof(stats::Functors, UniqueName) == mac::kFunctorListUniqueName);
static_assert(offsetof(stats::Functor, PropertyContext) == mac::kFunctorPropertyContext && sizeof(stats::Functor::PropertyContext) == 8);
static_assert(sizeof(stats::PassivePrototype) == mac::kPassiveSize
    && offsetof(stats::PassivePrototype, Properties) == mac::kPassiveProperties && sizeof(stats::PassivePrototype::Properties) == 4
    && offsetof(stats::PassivePrototype, Name) == mac::kPassiveName
    && offsetof(stats::PassivePrototype, Description) == mac::kPassiveDescription
    && offsetof(stats::PassivePrototype, EnabledConditions) == mac::kPassiveEnabledConditions
    && offsetof(stats::PassivePrototype, EnabledContext) == mac::kPassiveEnabledContext
    && offsetof(stats::PassivePrototype, ToggleOnEffect) == mac::kPassiveToggleOnEffect
    && offsetof(stats::PassivePrototype, ToggleOffEffect) == mac::kPassiveToggleOffEffect
    && offsetof(stats::PassivePrototype, StatsFunctorContext) == mac::kPassiveStatsFunctorContext
    && offsetof(stats::PassivePrototype, ConditionsIndex) == mac::kPassiveConditions);
static_assert(offsetof(stats::PassivePrototype, StatsFunctors) == mac::kPassiveStatsFunctors
    && offsetof(stats::PassivePrototype, ToggleOnFunctors) == mac::kPassiveToggleOnFunctors
    && offsetof(stats::PassivePrototype, ToggleOffFunctors) == mac::kPassiveToggleOffFunctors
    && offsetof(stats::PassivePrototype, ToggleGroup) == mac::kPassiveToggleGroup
    && offsetof(stats::PassivePrototype, ToggleOffContext) == mac::kPassiveToggleOffContext
    && offsetof(stats::PassivePrototype, BoostContext) == mac::kPassiveBoostContext
    && offsetof(stats::PassivePrototype, BoostConditionsIndex) == mac::kPassiveBoostConditions
    && offsetof(stats::PassivePrototype, Boosts) == mac::kPassiveBoosts
    && offsetof(stats::PassivePrototype, PriorityOrder) == mac::kPassivePriorityOrder
    && offsetof(stats::PassivePrototype, TooltipConditionalDamage) == mac::kPassiveTooltipConditionalDamage);
// A new passive is a node of the manager's map, made by Norbyte's map: the
// game's own node is the next node, the key, and the prototype.
static_assert(offsetof(PassiveMapNode, Value) == 0x10 && sizeof(PassiveMapNode) == 0x10 + mac::kPassiveSize);
static_assert(sizeof(stats::InterruptPrototype) == mac::kInterruptSize
    && offsetof(stats::InterruptPrototype, Name) == mac::kInterruptName
    && offsetof(stats::InterruptPrototype, Description) == mac::kInterruptDescription
    && offsetof(stats::InterruptPrototype, InterruptContext) == mac::kInterruptContext && sizeof(stats::InterruptPrototype::InterruptContext) == 2
    && offsetof(stats::InterruptPrototype, InterruptContextScope) == mac::kInterruptContextScope
    && offsetof(stats::InterruptPrototype, Conditions) == mac::kInterruptConditions
    && offsetof(stats::InterruptPrototype, Container) == mac::kInterruptContainer
    && offsetof(stats::InterruptPrototype, RollCondition) == mac::kInterruptRoll
    && offsetof(stats::InterruptPrototype, Costs) == mac::kInterruptCosts
    && offsetof(stats::InterruptPrototype, Stack) == mac::kInterruptStack);
static_assert(offsetof(stats::InterruptPrototype, PropertiesFunctors) == mac::kInterruptPropertiesFunctors
    && offsetof(stats::InterruptPrototype, SuccessFunctors) == mac::kInterruptSuccessFunctors
    && offsetof(stats::InterruptPrototype, FailureFunctors) == mac::kInterruptFailureFunctors
    && offsetof(stats::InterruptPrototype, InterruptDefaultValue) == mac::kInterruptDefaultValue
    && offsetof(stats::InterruptPrototype, EnableCondition) == mac::kInterruptEnableCondition
    && offsetof(stats::InterruptPrototype, EnableContext) == mac::kInterruptEnableContext
    && offsetof(stats::InterruptPrototype, InterruptFlags) == mac::kInterruptFlags);
static_assert(sizeof(Array<Guid>) == 0x10 && sizeof(Guid) == 0x10);
// Stats/Prototype.h: the spell and status prototypes, their animations and
// the four managers, as the game's code that makes, fills and destroys them
// reads them (Layout/GameShapes.cpp, GameShape::Prototypes). One edit: a
// spell animation's set of names is a set made by new[], not an array.
using SpellAnimationNames = decltype(stats::SpellPrototypeAnimationData::Part2);
static_assert(std::is_same_v<SpellAnimationNames, TrackedCompactSet<std::array<FixedString, 3>>>
    && SpellAnimationNames::size_offset == mac::kSetHeaderBytes && sizeof(std::array<FixedString, 3>) == mac::kAnimationNamesSize);
static_assert(sizeof(stats::SpellPrototypeAnimationData) == mac::kSpellAnimationsSize
    && offsetof(stats::SpellPrototypeAnimationData, Part2) == mac::kSpellAnimationsNames
    && offsetof(stats::SpellPrototypeAnimationData, Part8) + 2 * sizeof(FixedString) == mac::kSpellAnimationsLastName
    && offsetof(stats::SpellPrototypeAnimationData, Flags) == mac::kSpellAnimationsFlags
    && sizeof(SpellAnimationNames) == mac::kArrayBytes);
static_assert(offsetof(stats::SpellPrototype, SpellId) == mac::kSpellName
    && offsetof(stats::SpellPrototype, SpellContainerId) == mac::kSpellContainer
    && offsetof(stats::SpellPrototype, Description) == mac::kSpellDescription
    && offsetof(stats::SpellPrototype, ChildPrototypes) == mac::kSpellChildren
    && offsetof(stats::SpellPrototype, UseCosts) == mac::kSpellUseCosts
    && offsetof(stats::SpellPrototype, RitualCosts) == mac::kSpellRitualCosts
    && offsetof(stats::SpellPrototype, DualWieldingUseCosts) == mac::kSpellDualWieldingUseCosts
    && offsetof(stats::SpellPrototype, HitCostGroups) == mac::kSpellHitCostGroups
    && offsetof(stats::SpellPrototype, VariableUseCosts) == mac::kSpellVariableUseCosts
    && offsetof(stats::SpellPrototype, VariableDualWieldingUseCosts) == mac::kSpellVariableDualWieldingUseCosts
    && offsetof(stats::SpellPrototype, VariableRitualCosts) == mac::kSpellVariableRitualCosts
    && offsetof(stats::SpellPrototype, SpellAnimation) == mac::kSpellAnimation
    && offsetof(stats::SpellPrototype, DualWieldingSpellAnimation) == mac::kSpellDualWieldingAnimation);
static_assert(offsetof(stats::SpellPrototype, PrepareEffect) == mac::kSpellPrepareEffect
    && offsetof(stats::SpellPrototype, PrepareSound) == mac::kSpellPrepareSound
    && offsetof(stats::SpellPrototype, PrepareLoopSound) == mac::kSpellPrepareLoopSound
    && offsetof(stats::SpellPrototype, CastSound) == mac::kSpellCastSound
    && offsetof(stats::SpellPrototype, AlternativeCastTextEvents) == mac::kSpellAlternativeCastTextEvents
    && offsetof(stats::SpellPrototype, ContainerSpells) == mac::kSpellContainerSpells
    && offsetof(stats::SpellPrototype, Trajectories) == mac::kSpellTrajectories
    && offsetof(stats::SpellPrototype, PathfindTemplateId) == mac::kSpellPathfindTemplate && sizeof(stats::SpellPrototype::PathfindTemplateId) == sizeof(FixedString)
    && offsetof(stats::SpellPrototype, MetaConditions) == mac::kSpellMetaConditions
    && offsetof(stats::SpellPrototype, ItemWall) == mac::kSpellItemWall
    && offsetof(stats::SpellPrototype, InterruptPrototype) == mac::kSpellInterruptPrototype
    && offsetof(stats::SpellPrototype, CombatAIOverrideSpell) == mac::kSpellCombatAIOverrideSpell
    && offsetof(stats::SpellPrototype, CombatAIOverrideSpells) == mac::kSpellCombatAIOverrideSpells);
static_assert(offsetof(stats::SpellPrototypeManager, Spells) == mac::kSpellManagerSpells
    && offsetof(stats::SpellPrototypeManager, CombatAIOverrideSpells) == mac::kSpellManagerCombatAIOverrideSpells
    && offsetof(stats::SpellPrototypeManager, SpellNames) == mac::kSpellManagerNames
    && offsetof(stats::SpellPrototypeManager, Initialized) == mac::kSpellManagerInitialized);
static_assert(offsetof(stats::StatusPrototype, StatusName) == mac::kStatusName
    && offsetof(stats::StatusPrototype, Description) == mac::kStatusDescription
    && offsetof(stats::StatusPrototype, AbsorbSurfaceTypes) == mac::kStatusAbsorbSurfaceTypes
    && offsetof(stats::StatusPrototype, Boosts) == mac::kStatusBoosts
    && offsetof(stats::StatusPrototype, SoundStart) == mac::kStatusSoundStart
    && offsetof(stats::StatusPrototype, SoundLoop) == mac::kStatusSoundLoop
    && offsetof(stats::StatusPrototype, SoundStop) == mac::kStatusSoundStop);
static_assert(offsetof(stats::StatusPrototypeManager, Statuses) == mac::kStatusManagerStatuses
    && offsetof(stats::StatusPrototypeManager, StatusNames) == mac::kStatusManagerNames
    && offsetof(stats::StatusPrototypeManager, Initialized) == mac::kStatusManagerInitialized);
static_assert(offsetof(stats::PassivePrototypeManager, Initialized) == mac::kPassivesInitialized
    && offsetof(stats::InterruptPrototypeManager, Interrupts) == mac::kInterruptManagerInterrupts);
static_assert(offsetof(stats::RPGEnumeration, Name) == mac::kValueListName && offsetof(stats::RPGEnumeration, Values) == mac::kValueListValues);

// GameState.h, Module.h: the client and server objects and the mod manager
// (Layout/GameSession.h). One edit: the client object's four vtable pointers
// follow one another.
static_assert(offsetof(ecl::EoCClient, ComponentCallbacks) == mac::kClientVtables * sizeof(void*));
static_assert(offsetof(ecl::EoCClient, GameStateMachine) == mac::kClientGameStateMachine
    && offsetof(ecl::EoCClient, GameClient) == mac::kClientGameClient
    && offsetof(ecl::EoCClient, ModManager) == mac::kClientModManager
    && offsetof(ecl::EoCClient, EntityWorld) == mac::kClientEntityWorld);
static_assert(std::is_same_v<decltype(ecl::EoCClient::ModManager), ModManager*>);
static_assert(offsetof(esv::EoCServer, GameStateMachine) == mac::kServerGameStateMachine
    && offsetof(esv::EoCServer, GameServer) == mac::kServerGameServer
    && offsetof(esv::EoCServer, ModManager) == mac::kServerModManager
    && offsetof(esv::EoCServer, EntityWorld) == mac::kServerEntityWorld);
static_assert(std::is_same_v<decltype(esv::EoCServer::ModManager), ModManager>);
static_assert(offsetof(esv::GameStateMachine, State) == mac::kServerStateMachineState);
// Net.h: the game server. One edit: three vtable pointers after the peer.
static_assert(offsetof(net::GameServer, Replication) == mac::kGameServerReplication
    && offsetof(net::GameServer, ConnectedPeerIds) == mac::kGameServerConnectedPeers
    && offsetof(net::GameServer, UserMappings) + offsetof(net::GameServerUserMappings, UserIds) == mac::kGameServerUserIds
    && offsetof(net::GameServer, ActivePeerIds) == mac::kGameServerActivePeers
    && offsetof(net::GameServer, KickedPeerIds) == mac::kGameServerKickedPeers
    && offsetof(net::GameServer, PeerInfo) == mac::kGameServerPeerInfo
    && offsetof(net::GameServer, LocalPeerId) == mac::kGameServerLocalPeer);
static_assert(std::is_same_v<decltype(esv::EoCServer::GameServer), net::GameServer*>);
static_assert(offsetof(ModManager, BaseModule) == mac::kModManagerBaseModule
    && offsetof(ModManager, LoadOrderedModules) == mac::kModManagerLoadOrder
    && mac::kModManagerLoadOrderSize == mac::kModManagerLoadOrder + mac::kArraySize && sizeof(Array<Module>) == 0x10);
static_assert(sizeof(Module) == mac::kModuleSize && offsetof(Module, Info) == mac::kModuleInfo
    && offsetof(Module, Dependencies) == mac::kModuleDependencies && offsetof(Module, ModConflicts) == mac::kModuleConflicts
    && offsetof(Module, Addons) == mac::kModuleAddons);
static_assert(sizeof(STDString) == mac::kGameStringSize);
static_assert(offsetof(ModuleInfo, ModuleUUIDString) == mac::kModuleInfoId && offsetof(ModuleInfo, ModuleUUID) == mac::kModuleInfoGuid
    && offsetof(ModuleInfo, Name) == mac::kModuleInfoName && offsetof(ModuleInfo, StartLevelName) == mac::kModuleInfoLevels
    && offsetof(ModuleInfo, PhotoBoothLevelName) == mac::kModuleInfoLevels + 0x10
    && offsetof(ModuleInfo, Hash) == mac::kModuleInfoHash && offsetof(ModuleInfo, Directory) == mac::kModuleInfoDirectory
    && offsetof(ModuleInfo, Scripts) == mac::kModuleInfoScripts && offsetof(ModuleInfo, Author) == mac::kModuleInfoAuthor
    && offsetof(ModuleInfo, Description) == mac::kModuleInfoDescription);

// FileReader.h, Symbols.h: the file reader, the path roots and a string
// (Layout/GameFiles.h). That his string reads the game's 16 bytes as the
// game does is a test (Tests/GameStringNorbyte.cpp).
static_assert(sizeof(STDString) == mac::kGameStringBytes && sizeof(STDString::size_type) == 4);
static_assert(std::is_same_v<decltype(StaticSymbols::ls__PathRoots), STDString**>);
static_assert(offsetof(FileReader, IsLoaded) == mac::kFileReaderLoaded && sizeof(FileReader::IsLoaded) == 1
    && offsetof(FileReader, DataPtr) == mac::kFileReaderData && offsetof(FileReader, ReadPtr) == mac::kFileReaderRead
    && offsetof(FileReader, FileSize) == mac::kFileReaderSize && offsetof(FileReader, FileHandle) == mac::kFileReaderHandle
    && offsetof(FileReader, Type) == mac::kFileReaderType && sizeof(FileReader::Type) == 4
    && offsetof(FileReader, FileObject) == mac::kFileReaderFile);

// GameState.h: the game state event managers' listeners, and the client's
// state machine (Layout/GameSession.h). The listeners his code adds
// (Extender/ScriptExtender.cpp) declare a virtual destructor and then the
// handler: slots 0 and 1, and the handler in slot 2, where the game calls it.
static_assert(offsetof(GameStateEventManager, Callbacks) == mac::kStateEventListeners
    && mac::kStateEventListenersSize == mac::kStateEventListeners + mac::kArraySize && sizeof(Array<void*>) == 0x10);
static_assert(mac::kStateListenerChanged == 2 * sizeof(void*) && mac::kStateListenerTickLoad == 3 * sizeof(void*));
static_assert(offsetof(ecl::GameStateMachine, State) == mac::kClientStateMachineState && sizeof(ecl::GameState) == 4 && sizeof(esv::GameState) == 4);

// UI.h: the game UI. Two edits: the five vtable pointers it begins with, and
// the request manager's pointer in the Twitch overlay manager.
static_assert(offsetof(ui::GameUI, SplitScreenChangedSignal) == mac::kGameUiVtables * sizeof(void*)
    && offsetof(ui::GameUI, SplitScreenChangedSignal) == mac::kGameUiSignal);
static_assert(offsetof(ui::GameUI, NoesisUIManager) == mac::kGameUiNoesisManager);
// The state machine's templates, as their StaticFillClassType registers them.
static_assert(offsetof(ui::UIState, Name) == mac::kUiTemplateFirstString && sizeof(ui::UIState::Name) == mac::kUiTemplateString
    && offsetof(ui::UIState, ForceVisible) == mac::kUiTemplateForceVisible
    && offsetof(ui::UIState, ForceSplitscreen) == mac::kUiTemplateForceSplitscreen
    && offsetof(ui::UIState, DisableSoundLayersAbove) == mac::kUiTemplateDisableSoundLayersAbove
    && offsetof(ui::UIState, InitialSubstates) == mac::kUiTemplateInitialSubstates && offsetof(ui::UIState, Events) == mac::kUiTemplateEvents
    && offsetof(ui::UIState, Widgets) == mac::kUiTemplateWidgets && offsetof(ui::UIState, ActivateOnRestore) == mac::kUiTemplateActivateOnRestore);
static_assert(offsetof(ui::UIStateWidget, Filename) == mac::kUiTemplateFirstString
    && offsetof(ui::UIStateWidget, SoundLayer) == mac::kUiTemplateFirstString + 2 * mac::kUiTemplateString
    && offsetof(ui::UIStateWidget, IgnoreHitTest) == mac::kUiWidgetTemplateIgnoreHitTest
    && offsetof(ui::UIStateWidget, BlockedLoading) == mac::kUiWidgetTemplateBlockedLoading
    && offsetof(ui::UIStateWidget, AllowCreationDelay) == mac::kUiWidgetTemplateAllowCreationDelay
    && offsetof(ui::UIStateWidget, Libraries) == mac::kUiWidgetTemplateLibraries);
static_assert(offsetof(ui::UIStateEvent, Name) == mac::kUiTemplateFirstString
    && offsetof(ui::UIStateEvent, HandlesEvent) == mac::kUiEventHandlesEvent && sizeof(ui::UIStateEvent::HandlesEvent) == 1
    && offsetof(ui::UIStateEvent, Actions) == mac::kUiEventActions);
static_assert(offsetof(ui::UIInitialSubstate, Name) == mac::kUiTemplateFirstString
    && offsetof(ui::UIInitialSubstate, MetaData) == mac::kUiTemplateFirstString + mac::kUiTemplateString);
static_assert(offsetof(ui::GameUI, BrushManager) == mac::kGameUiBrushManager
    && offsetof(ui::BrushManagerBase, CS_) == mac::kGameUiBrushManagerLock);
static_assert(offsetof(ui::GameUI, field_380) == mac::kGameUiCommandProcessor && offsetof(ui::GameUI, CommandQueue) == mac::kGameUiCommandQueue);
static_assert(offsetof(ui::GameUI, WindowManager) == mac::kGameUiWindowManager);
static_assert(offsetof(ui::GameUI, StateMachine) == mac::kGameUiStateMachine);
static_assert(offsetof(ui::GameUI, ViewModelProvider) == mac::kGameUiViewModelProvider);
static_assert(offsetof(ui::GameUI, TwitchOverlayManager) == mac::kGameUiTwitchManager);
static_assert(offsetof(ui::GameUI, TutorialManager) == mac::kGameUiTutorialManager);
static_assert(offsetof(ui::GameUI, EventsCS_) == mac::kGameUiEventsLock && offsetof(ui::GameUI, UpdateCS_) == mac::kGameUiUpdateLock);
static_assert(offsetof(ui::GameUI, RenderCS_) == mac::kGameUiRenderLock
    && offsetof(ui::GameUI, RenderOffscreenWithoutUpdatingCS_) == mac::kGameUiRenderOffscreenLock);
static_assert(sizeof(ui::TwitchOverlayManager) == mac::kTwitchManagerSize);
static_assert(offsetof(ui::TwitchOverlayManager, CS_) == mac::kTwitchManagerLock);
static_assert(offsetof(ui::TwitchOverlayManager, net__defaulthttprequestmanager120) == mac::kTwitchManagerRequests);

// UI.h: the UI state machine's component (Layout/GameUi.h). Two edits: the
// array and the two sets after the third map, and what follows the player's id.
static_assert(offsetof(ui::UIStateMachine, State) == mac::kUiStatesState);
static_assert(offsetof(ui::UIStateMachine, field_170_Mac) == mac::kUiStatesExtraArray
    && offsetof(ui::UIStateMachine, field_180_Mac) == mac::kUiStatesExtraSets
    && sizeof(HashSet<Guid>) == mac::kUiStatesSetBytes
    && offsetof(ui::UIStateMachine, field_170_MHS_pState) == mac::kUiStatesStateSet);
static_assert(offsetof(ui::UIStateMachine, RootState) == mac::kUiStatesRootState && sizeof(ui::UIStateMachine::RootState) == 0x20
    && offsetof(ui::UIStateMachine, States) == mac::kUiStatesStates
    && offsetof(ui::UIStateMachine, PlayerID) == mac::kUiStatesPlayer && sizeof(ui::UIStateMachine::PlayerID) == 2);
static_assert(offsetof(ui::UIStateMachine, DeferredStateEvents) == mac::kUiStatesDeferredEvents
    && offsetof(ui::UIStateMachine, CriticalSections) == mac::kUiStatesLocks
    && sizeof(ui::UIStateMachine::CriticalSections) == mac::kUiStatesLockCount * sizeof(CRITICAL_SECTION));

// ui::Widget and ui::State (Layout/GameUi.h).
static_assert(offsetof(ui::UIWidget, CS_) == mac::kUiWidgetLock && offsetof(ui::UIWidget, field_2A0_InputChildInfo) == mac::kUiWidgetInputChildren
    && offsetof(ui::UIWidget, qword2C0) == mac::kUiWidgetVector && offsetof(ui::UIWidget, ContextMenuManager) == mac::kUiWidgetContextMenuManager
    && offsetof(ui::UIWidget, TooltipManager) == mac::kUiWidgetTooltipManager && offsetof(ui::UIWidget, WindowManager) == mac::kUiWidgetWindowManager
    && offsetof(ui::UIWidget, StateMachine) == mac::kUiWidgetStateMachine && offsetof(ui::UIWidget, WidgetData) == mac::kUiWidgetData
    && offsetof(ui::UIWidget, InputManager) == mac::kUiWidgetInputManager && offsetof(ui::UIWidget, StateId) == mac::kUiWidgetStateId
    && offsetof(ui::UIWidget, ParentCanvas) == mac::kUiWidgetParentCanvas && offsetof(ui::UIWidget, LayerIndex) == mac::kUiWidgetLayerIndex
    && offsetof(ui::UIWidget, IndexInLayer) == mac::kUiWidgetIndexInLayer && offsetof(ui::UIWidget, FileName) == mac::kUiWidgetFileName
    && offsetof(ui::UIWidget, ContextMenu) == mac::kUiWidgetContextMenu && offsetof(ui::UIWidget, FocusRefresh) == mac::kUiWidgetFocusRefresh
    && offsetof(ui::UIWidget, FocusDownEvent) == mac::kUiWidgetEvents
    && offsetof(ui::UIWidget, MoveFocusInputState) == mac::kUiWidgetMoveFocusInputState
    && offsetof(ui::UIWidget, Flags) == mac::kUiWidgetFlags && offsetof(ui::UIWidget, Flags2) == mac::kUiWidgetFlags + 1
    && offsetof(ui::UIWidget, Flags3) == mac::kUiWidgetFlags + 2 && offsetof(ui::UIWidget, PlayerId) == mac::kUiWidgetPlayer
    && sizeof(ui::UIWidget::PlayerId) == 2 && sizeof(ui::UIWidget::LayerIndex) == 4);
static_assert(offsetof(ui::UIStateInstance, Widgets) == mac::kUiInstanceWidgets && offsetof(ui::UIStateInstance, States) == mac::kUiInstanceStacks
    && offsetof(ui::UIStateInstance, State) == mac::kUiInstanceTemplate && offsetof(ui::UIStateInstance, PlayerID) == mac::kUiInstancePlayer
    && offsetof(ui::UIStateInstance, WidgetsLock) == mac::kUiInstanceLock && offsetof(ui::UIStateInstance, Flags) == mac::kUiInstanceFlags
    && offsetof(ui::UIStateInstance, Flags2) == mac::kUiInstanceFlags + 1 && offsetof(ui::UIStateInstance, Layout) == mac::kUiInstanceLayout
    && offsetof(ui::UIStateInstance, Owner) == mac::kUiInstanceOwner && offsetof(ui::UIStateInstance, field_3E) == mac::kUiInstanceByte
    && offsetof(ui::UIStateInstance, field_40) == mac::kUiInstanceName && offsetof(ui::UIStateInstance, field_48) == mac::kUiInstanceStrings
    && sizeof(ui::UIStateInstance::field_48) == mac::kUiTemplateString
    && offsetof(ui::UIStateInstance, TutorialLayer) == mac::kUiInstanceStrings + 2 * mac::kUiTemplateString
    && offsetof(ui::UIStateInstance, StateGuid) == mac::kUiInstanceGuid && offsetof(ui::UIStateInstance, field_B8) == mac::kUiInstanceStackGuid
    && offsetof(ui::UIStateInstance, field_D0) == mac::kUiInstanceExtra
    && offsetof(ui::UIStateInstance, StateWidgets) == mac::kUiInstanceWidgetTemplates && sizeof(ui::UIStateInstance) == mac::kUiInstanceSize);
// A stack of states is, to his declaration, the array of states it begins with.
static_assert(std::is_same_v<decltype(ui::UIStateInstance::States), Array<Array<ui::UIStateInstance*>*>> && mac::kUiStackStates == 0);
static_assert(offsetof(ui::UIStateMachine, field_B0_MHM_Guid_pStateStack) == 0xb0);

// esv::Item (Layout/GameCharacter.h).
static_assert(offsetof(esv::Item, field_10) == mac::kItemObjectHandle && offsetof(esv::Item, Flags) == mac::kItemFlags && sizeof(esv::Item::Flags) == 8
    && offsetof(esv::Item, MyHandle) == mac::kItemEntity && offsetof(esv::Item, Level) == mac::kItemLevel
    && offsetof(esv::Item, ItemType) == mac::kItemType && offsetof(esv::Item, UpdatePeerIds) == mac::kItemPeers
    && offsetof(esv::Item, Template) == mac::kItemTemplate && offsetof(esv::Item, OriginalTemplate) == mac::kItemOriginalTemplate
    && offsetof(esv::Item, StatsObject) == mac::kItemStatsObject && offsetof(esv::Item, field_60) == mac::kItemWord
    && offsetof(esv::Item, field_68) == mac::kItemWord + 8 && offsetof(esv::Item, field_70) == mac::kItemNames
    && offsetof(esv::Item, Stats) == mac::kItemStats && offsetof(esv::Item, PreviousLevel) == mac::kItemPreviousLevel
    && offsetof(esv::Item, TreasureLevel) == mac::kItemTreasureLevel && offsetof(esv::Item, Amount) == mac::kItemTreasureLevel + 4
    && offsetof(esv::Item, Flags2) == mac::kItemFlags2 && sizeof(esv::Item::Flags2) == 1);

// ecl::Character (Layout/GameCharacter.h).
static_assert(offsetof(ecl::Character, Entity) == mac::kClientCharacterObjectHandle && offsetof(ecl::Character, Flags) == mac::kClientCharacterFlags
    && sizeof(ecl::Character::Flags) == 8 && offsetof(ecl::Character, Level) == mac::kClientCharacterLevel
    && offsetof(ecl::Character, SelfEntity) == mac::kClientCharacterEntity
    && offsetof(ecl::Character, SurfacePathInfluences) == mac::kClientCharacterPathInfluences
    && offsetof(ecl::Character, DialogRequest) == mac::kClientCharacterRequests && sizeof(ecl::Character::DialogRequest) == 8
    && offsetof(ecl::Character, ListenRequest) == mac::kClientCharacterRequests + 8
    && offsetof(ecl::Character, Template) == mac::kClientCharacterTemplate
    && offsetof(ecl::Character, OriginalTemplate) == mac::kClientCharacterOriginalTemplate
    && offsetof(ecl::Character, PlayerData) == mac::kClientCharacterPlayerData && offsetof(ecl::Character, Owner) == mac::kClientCharacterOwner
    && offsetof(ecl::Character, InUseByCharacter) == mac::kClientCharacterOwner + 8
    && offsetof(ecl::Character, FXCircles_Arr_EntityRef) == mac::kClientCharacterCircles
    && offsetof(ecl::Character, ObscurementIndicationEffect) == mac::kClientCharacterEffects
    && offsetof(ecl::Character, FollowCharacter) == mac::kClientCharacterEffects + 8 && offsetof(ecl::Character, Light) == mac::kClientCharacterEffects + 24
    && offsetof(ecl::Character, OwnerUserID) == mac::kClientCharacterOwnerUser && offsetof(ecl::Character, ReservedUserID) == mac::kClientCharacterOwnerUser + 4
    && offsetof(ecl::Character, CircleColors_Arr_EntityRef) == mac::kClientCharacterCircleColors
    && offsetof(ecl::Character, LastTick) == mac::kClientCharacterLastTick && offsetof(ecl::Character, SneakCheck) == mac::kClientCharacterLastTick + 4
    && offsetof(ecl::Character, SpeedMultiplier) == mac::kClientCharacterSpeedMultiplier
    && offsetof(ecl::Character, DialogData) == mac::kClientCharacterSpeedMultiplier + 4
    && offsetof(ecl::Character, PhysicsFlags_M) == mac::kClientCharacterPhysicsFlags
    && offsetof(ecl::Character, FeetStatus) == mac::kClientCharacterFeetStatus && offsetof(ecl::Character, Flags2) == mac::kClientCharacterFlags2
    && offsetof(ecl::Character, Flags3) == mac::kClientCharacterFlags2 + 1);
// The client character's input controller and its controller mode
// (Layout/GameCharacter.h).
static_assert(offsetof(ecl::Character, InputController) == mac::kClientCharacterInputController
    && offsetof(ecl::InputController, Flags) == mac::kClientInputFlags && sizeof(ecl::InputController::Flags) == 1
    && (uint8_t)ecl::InputControllerFlags::ControllerMode == mac::kClientInputControllerMode);

// esv::Status (Layout/GameCharacter.h).
static_assert(offsetof(esv::Status, field_18) == mac::kStatusWord && offsetof(esv::Status, StoryActionID) == mac::kStatusStoryAction
    && offsetof(esv::Status, Originator) == mac::kStatusOriginator
    && offsetof(esv::Status, Originator) + offsetof(ActionOriginator, PassiveId) == mac::kStatusOriginatorStrings
    && offsetof(esv::Status, Originator) + offsetof(ActionOriginator, CanApplyConcentration) == mac::kStatusOriginatorStrings + 12
    && offsetof(esv::Status, field_48) == mac::kStatusDouble && offsetof(esv::Status, StatusId) == mac::kStatusId
    && offsetof(esv::Status, StartTimer) == mac::kStatusId + 4 && offsetof(esv::Status, LifeTime) == mac::kStatusLifeTime
    && offsetof(esv::Status, CurrentLifeTime) == mac::kStatusLifeTime + 4 && offsetof(esv::Status, TurnTimer) == mac::kStatusTurnTimer
    && offsetof(esv::Status, Strength) == mac::kStatusTurnTimer + 4 && offsetof(esv::Status, CauseType) == mac::kStatusCauseType
    && offsetof(esv::Status, OriginCauseType) == mac::kStatusCauseType + 1 && offsetof(esv::Status, StatusHandle) == mac::kStatusHandle
    && offsetof(esv::Status, SyncEntity) == mac::kStatusSyncEntity && offsetof(esv::Status, Owner) == mac::kStatusOwner
    && offsetof(esv::Status, StatusOwner) == mac::kStatusOwners && offsetof(esv::Status, Cause) == mac::kStatusCause
    && offsetof(esv::Status, CauseGUID) == mac::kStatusCauseGuid && offsetof(esv::Status, StatusSource) == mac::kStatusSource
    && offsetof(esv::Status, RootCause) == mac::kStatusRootCause && offsetof(esv::Status, field_E0) == mac::kStatusRootCause + 0x10
    && offsetof(esv::Status, field_E8) == mac::kStatusGuid && offsetof(esv::Status, RemoveConditions) == mac::kStatusRemoveConditions
    && offsetof(esv::Status, RemoveEvents) == mac::kStatusRemoveConditions + 4 && offsetof(esv::Status, Flags) == mac::kStatusFlags
    && offsetof(esv::Status, Flags2) == mac::kStatusFlags + 1 && offsetof(esv::Status, Flags3) == mac::kStatusFlags + 2
    && offsetof(esv::Status, FreezeDuration) == mac::kStatusFreezeDuration && offsetof(esv::Status, ExitCause) == mac::kStatusFreezeDuration + 1
    && offsetof(esv::Status, SourceSpell) == mac::kStatusSourceSpell
    && offsetof(esv::Status, SourceSpell) + offsetof(SpellId, SourceType) == mac::kStatusSpellSourceType
    && offsetof(esv::Status, SourceSpell) + offsetof(SpellId, Prototype) == mac::kStatusSpellPrototype
    && offsetof(esv::Status, SpellCastSourceUuid) == mac::kStatusSpellCastSource && offsetof(esv::Status, StackId) == mac::kStatusStackId
    && offsetof(esv::Status, StackPriority) == mac::kStatusStackId + 4 && offsetof(esv::Status, IsRecoverable) == mac::kStatusRecoverable
    && offsetof(esv::Status, DifficultyStatus) == mac::kStatusDifficultyStatus
    && offsetof(esv::Status, StackedSpellCastSourceUuid) == mac::kStatusStackedSpellCastSource
    && offsetof(esv::Status, ConditionRolls) == mac::kStatusStackedSpellCastSource + 0x10
    && offsetof(esv::Status, TickType) == mac::kStatusTickType && offsetof(esv::Status, Flags4) == mac::kStatusFlags4);

// rf::CameraController and rf::Camera (Layout/GameCamera.h): every member a
// script reads, and the hidden ones that keep them in place.
static_assert(offsetof(Transform, RotationQuat) == 0 && offsetof(Transform, Translate) == mac::kTransformTranslate
    && offsetof(Transform, Scale) == mac::kTransformScale && sizeof(Transform) == mac::kTransformSize);
static_assert(offsetof(rf::CameraController, ViewDirty) == mac::kCameraViewDirty
    && offsetof(rf::CameraController, ProjectionDirty) == mac::kCameraProjectionDirty
    && offsetof(rf::CameraController, IsOrthographic) == mac::kCameraOrthographic
    && offsetof(rf::CameraController, OffCenter) == mac::kCameraOffCenter && offsetof(rf::CameraController, FOV) == mac::kCameraFov
    && offsetof(rf::CameraController, NearPlane) == mac::kCameraNearPlane && offsetof(rf::CameraController, FarPlane) == mac::kCameraFarPlane
    && offsetof(rf::CameraController, AspectRatio) == mac::kCameraAspectRatio
    && offsetof(rf::CameraController, WorldTransform) == mac::kCameraWorldTransform
    && offsetof(rf::CameraController, LookAt) == mac::kCameraLookAt && offsetof(rf::CameraController, field_58) == mac::kCameraVector
    && offsetof(rf::CameraController, field_5C) == mac::kCameraVector + 0xc && offsetof(rf::CameraController, OrthoLeft) == mac::kCameraOrtho
    && offsetof(rf::CameraController, OrthoRight) == mac::kCameraOrtho + 4 && offsetof(rf::CameraController, OrthoBottom) == mac::kCameraOrtho + 8
    && offsetof(rf::CameraController, OrthoTop) == mac::kCameraOrtho + 0xc && offsetof(rf::CameraController, ID) == mac::kCameraId
    && offsetof(rf::CameraController, field_74) == mac::kCameraId + 4 && offsetof(rf::CameraController, Camera) == mac::kCameraCamera
    && offsetof(rf::CameraController, FrustumPoints) == mac::kCameraFrustumPoints
    && sizeof(rf::CameraController::FrustumPoints) == 8 * 0xc && sizeof(rf::CameraController) == mac::kCameraControllerSize);
static_assert(offsetof(rf::Camera, ViewMatrix) == mac::kCameraView && offsetof(rf::Camera, InvViewMatrix) == mac::kCameraInvView
    && offsetof(rf::Camera, ProjectionMatrix) == mac::kCameraProjection && offsetof(rf::Camera, InvProjectionMatrix) == mac::kCameraInvProjection
    && offsetof(rf::Camera, ViewProjectionMatrix) == mac::kCameraViewProjection
    && offsetof(rf::Camera, OffsetViewProjectionMatrix) == mac::kCameraOffsetViewProjection
    && offsetof(rf::Camera, CullingPlanes) == mac::kCameraCullingPlanes && sizeof(rf::Camera::CullingPlanes) == 10 * 0x10
    && sizeof(rf::Camera) == mac::kCameraSize);

// ls::CameraComponent, the head of it (Layout/GameCamera.h): the type of the
// master behaviour is one byte.
static_assert(offsetof(CameraComponent, MasterBehaviorType) == mac::kCameraComponentBehaviorType
    && sizeof(CameraComponent::MasterBehaviorType) == 1 && offsetof(CameraComponent, Controller) == mac::kCameraComponentController
    && offsetof(CameraComponent, AcceptsInput) == mac::kCameraComponentAcceptsInput
    && sizeof(CameraComponent) == mac::kCameraComponentSize);

// AllSpark.h: the range of a timeline parameter's channels is two pointers
// that come back in two registers (two edits: the game's function takes no
// argument to write them through).
static_assert(sizeof(aspk::TLMaterialComponent::Parameter::Range) == 0x10
    && std::is_trivially_copyable_v<aspk::TLMaterialComponent::Parameter::Range>
    && std::is_trivially_destructible_v<aspk::TLMaterialComponent::Parameter::Range>
    && sizeof(aspk::TimelineActorPropertiesReflectionComponent::Parameter::Range) == 0x10
    && std::is_trivially_copyable_v<aspk::TimelineActorPropertiesReflectionComponent::Parameter::Range>);

// Components/Death.h: a death request has its own three bytes after the
// whole of DeathData, as the game's has (the components that hold one are
// sized by the game's registrations: bg3mse-layouts components).
static_assert(sizeof(death::DeathData) == 0x78 && offsetof(death::DeathRequestData, field_78) == 0x78
    && offsetof(death::DeathRequestData, SkipDowned) == 0x7a && sizeof(death::DeathRequestData) == 0x80);

// Animation.h, Components/Level.h, Components/Progression.h: three objects
// the macOS game has without a member of the Windows ones.
static_assert(offsetof(gn::GenomeManager, UnloadBatch) == mac::kGenomeUnloadBatch
    && offsetof(gn::GenomeManager, UnloadBatchJob) == mac::kGenomeUnloadBatchJob);
static_assert(offsetof(gn::GenomeManager, SyncedTime) == mac::kGenomeSyncedTime
    && offsetof(gn::GenomeManager, IsMasterTime) == mac::kGenomeIsMasterTime);
static_assert(sizeof(LevelInstanceLoadSystem) == mac::kLevelLoadSystemSize);
static_assert(offsetof(LevelInstanceLoadSystem, LevelManager) == mac::kLevelLoadSystemManager
    && offsetof(LevelInstanceLoadSystem, LoadRequests) == mac::kLevelLoadSystemRequests
    && offsetof(LevelInstanceLoadSystem, LoadRequestsCS) == mac::kLevelLoadSystemLock);
static_assert(sizeof(esv::progression::ManagementSystem) == mac::kProgressionSystemSize
    && offsetof(esv::progression::ManagementSystem, ImmutableDataHeadmaster) == mac::kProgressionSystemPointers);
// Dialog.h: the dialog interface's two queues are a word shorter each.
static_assert(offsetof(dlg::SharedDialogGameInterface, DialogIOLoadRequests_MPSCQueue2) == mac::kDialogInterfaceSecondQueue
    && offsetof(dlg::SharedDialogGameInterface, DialogIOLoadRequests) == mac::kDialogInterfaceRequests
    && offsetof(dlg::SharedDialogGameInterface, DialogManager) == mac::kDialogInterfaceManager);
static_assert(offsetof(esv::DialogSystem, GameInterface) == mac::kDialogSystemInterface);
// Dialog.h: the dialog manager has an array more before its lock.
static_assert(offsetof(dlg::DialogManager, Dialogs) == mac::kDialogManagerDialogs
    && offsetof(dlg::DialogManager, CachedDialogs) == mac::kDialogManagerCachedDialogs
    && offsetof(dlg::DialogManager, ScriptFlags) == mac::kDialogManagerScriptFlags
    && offsetof(dlg::DialogManager, DialogVariables) == mac::kDialogManagerVariables
    && offsetof(dlg::DialogManager, SpeakerGroups) == mac::kDialogManagerSpeakerGroups
    && offsetof(dlg::DialogManager, DialogToVariable) == mac::kDialogManagerDialogToVariable
    && offsetof(dlg::DialogManager, field_F8) == mac::kDialogManagerFieldF8
    && offsetof(dlg::DialogManager, field_108) == mac::kDialogManagerField108
    && offsetof(dlg::DialogManager, CriticalSection) == mac::kDialogManagerLock
    && offsetof(dlg::DialogManager, ModManager) == mac::kDialogManagerModManager);
// Dialog.h, Components/Timeline.h: what loading and releasing a dialogue
// from Lua reads and writes, and where the client finds its dialog manager.
static_assert(offsetof(dlg::Dialog, FileName) == mac::kDialogFileName
    && offsetof(dlg::Dialog, UUID) == mac::kDialogUuid
    && offsetof(dlg::Dialog, TimelineId) == mac::kDialogTimelineId
    && offsetof(dlg::Dialog, UnloadedTimeStamp) == mac::kDialogUnloadedTime
    && offsetof(dlg::Dialog, RefCount) == mac::kDialogRefCount
    && offsetof(dlg::Dialog, field_128) == mac::kDialogNodeVisitor);
static_assert(offsetof(GameTime, Time) == 0 && sizeof(GameTime::Time) == 8);
static_assert(offsetof(ecl::TimelineSystem, GameStateEventListenerVMT) == mac::kClientTimelineStateListener
    && offsetof(ecl::TimelineSystem, DialogEventListenerAdapterVMT) == mac::kClientTimelineDialogListener
    && offsetof(ecl::TimelineSystem, SoundSystem) == mac::kClientTimelineSoundSystem
    && offsetof(ecl::TimelineSystem, MusicManager) == mac::kClientTimelineMusicManager
    && offsetof(ecl::TimelineSystem, DialogManager) == mac::kClientTimelineDialogManager
    && offsetof(ecl::TimelineSystem, LevelUnloaded_Arr_UnloadRequest) == mac::kClientTimelineUnloadRequests
    && offsetof(ecl::TimelineSystem, Arr_ReactivateCameraRequest) == mac::kClientTimelineCameraRequests
    && offsetof(ecl::TimelineSystem, ClientSessionLoaded) == mac::kClientTimelineSessionLoaded
    && offsetof(ecl::TimelineSystem, LevelUnloaded) == mac::kClientTimelineLevelUnloaded
    && offsetof(ecl::TimelineSystem, LevelDeactivated) == mac::kClientTimelineLevelDeactivated);

// Components/Trigger.h, Components/Projectile.h, Components/Visual.h: the
// members that follow the placeholder words the macOS game's objects lack.
static_assert(offsetof(esv::SoundVolumeTrigger, Entity) == mac::kTriggerEntity);
static_assert(offsetof(esv::TimelineSceneTrigger, Entity) == mac::kTriggerEntity);
static_assert(offsetof(ecl::TimelineSceneTrigger, Entity) == mac::kTriggerEntity);
static_assert(offsetof(esv::projectile::Projectile, Entity) == mac::kProjectileEntity);
static_assert(offsetof(esv::projectile::Projectile, SpellRollResult) == mac::kProjectileHit);
static_assert(offsetof(esv::projectile::InitializationComponent, Data) == 0);
static_assert(offsetof(Construction, Tiles) == 0 && offsetof(Construction, InstanceId) == mac::kConstructionInstanceId);

// Components/Timeline.h: the actor control component has its two bytes, then
// the word, and no members in the padding between them.
static_assert(offsetof(SyncedTimelineActorControlComponent, Timeline) == mac::kActorControlTimeline
    && offsetof(SyncedTimelineActorControlComponent, Slot) == mac::kActorControlSlot
    && offsetof(SyncedTimelineActorControlComponent, IsListener) == mac::kActorControlListener
    && offsetof(SyncedTimelineActorControlComponent, field_24) == mac::kActorControlWord
    && sizeof(SyncedTimelineActorControlComponent) == mac::kActorControlSize);
static_assert(sizeof(SyncedTimelineActorControlComponent::Slot) == 1 && sizeof(SyncedTimelineActorControlComponent::IsListener) == 1
    && sizeof(SyncedTimelineActorControlComponent::field_24) == 4);

// Components/CharacterCreation.h: the companion definition component's last
// two members follow the whole of the definition it begins with.
static_assert(offsetof(character_creation::CompanionDefinitionComponent, IconVersion) == mac::kCompanionIconVersion
    && sizeof(character_creation::CompanionDefinitionComponent::IconVersion) == 4
    && offsetof(character_creation::CompanionDefinitionComponent, HasIcon) == mac::kCompanionHasIcon
    && sizeof(character_creation::CompanionDefinitionComponent::HasIcon) == 1);

// AllSpark.h: a timeline shot's first member is in the last bytes of its
// base, and its map follows the byte the macOS game has before it. The
// sizes of the classes that follow such a base are compared with what the
// game allocates (bg3mse-layouts objects).
static_assert(offsetof(aspk::TLBaseComponent, IsSnappedToEnd) == mac::kTimelineComponentLastBytes
    && offsetof(aspk::TLBaseComponent, IsMimicry) == mac::kTimelineComponentLastBytes + 1);
static_assert(offsetof(aspk::TLShotComponent, SwitchInterval) == mac::kShotSwitchInterval
    && offsetof(aspk::TLShotComponent, IsLogicEnabled) == mac::kShotLogicEnabled
    && offsetof(aspk::TLShotComponent, VTPrefetchData) == mac::kShotPrefetchData
    && sizeof(aspk::TLShotComponent) == mac::kShotSize);

// Net.h: the two members of the peer the extender's networking goes through.
static_assert(offsetof(net::AbstractPeer, NetMessageFactory) == mac::kPeerMessageFactory);
static_assert(offsetof(net::AbstractPeer, ProtocolList) == mac::kPeerProtocols);
static_assert(offsetof(net::AbstractPeer, ProtocolMap) == mac::kPeerProtocolMap);

// Net.h: the message factory and its pools, which Norbyte's own code fills
// and empties (Extender/Shared/ExtenderNet.cpp), a message, which a class of
// his derives from, and the serializer his message is handed.
static_assert(offsetof(net::MessageFactory, MessagePools) == 0 && sizeof(net::MessageFactory::MessagePools) == mac::kFactoryLock
    && offsetof(net::MessageFactory, CriticalSection) == mac::kFactoryLock);
static_assert(offsetof(net::MessagePool, Template) == mac::kPoolTemplate && offsetof(net::MessagePool, GrowSize) == mac::kPoolGrowSize
    && sizeof(net::MessagePool::GrowSize) == 8 && offsetof(net::MessagePool, Messages) == mac::kPoolMessages
    && sizeof(net::MessagePool::Messages) == mac::kPoolLeased - mac::kPoolMessages
    && offsetof(net::MessagePool, LeasedMessages) == mac::kPoolLeased && sizeof(net::MessagePool) == mac::kPoolSize);
static_assert(offsetof(net::Message, MsgId) == mac::kMessageId && sizeof(net::Message::MsgId) == 4
    && offsetof(net::Message, Reliability) == mac::kMessageId + 4 && offsetof(net::Message, Priority) == mac::kMessagePriority
    && offsetof(net::Message, OrderingSequence) == mac::kMessageOrdering && offsetof(net::Message, Timestamped) == mac::kMessageOrdering + 1
    && offsetof(net::Message, Timestamp) == mac::kMessageTimestamp && sizeof(net::Message) == mac::kMessageSize);
static_assert((uint32_t)NetMessage::NETMSG_CLIENT_CONNECT == mac::kClientConnectMessageId);
static_assert(offsetof(net::BitstreamSerializer, IsWriting) == mac::kSerializerIsWriting
    && offsetof(net::BitstreamSerializer, Bitstream) == mac::kSerializerStream);

// Misc.h: the global switches (Layout/GameSwitches.h). Four edits: a sound
// setting's second word after its function, the float that is the base
// class's tail padding under MSVC, the alignment of the array before the
// hotbar switches, and the Twitch overlay's switch before its neighbour.
using SoundSetting = GlobalSwitches::SoundSetting;
static_assert(sizeof(GlobalSwitches) == mac::kSwitchesSize && offsetof(GlobalSwitches, Language) == mac::kSwitchesLanguage
    && sizeof(GlobalSwitches::Language) == mac::kGameStringBytes);
static_assert(offsetof(GlobalSwitches, saveSystemECBCheckNumberOfFramesToWait) == mac::kSwitchesBaseLastInt
    && offsetof(GlobalSwitches, field_1A0) == mac::kSwitchesBaseLast
    && offsetof(GlobalSwitches, UseLevelCache) == mac::kSwitchesBaseLast + sizeof(float));
static_assert(offsetof(GlobalSwitches, SoundSettings) == mac::kSwitchesSoundSettings && sizeof(SoundSetting) == mac::kSoundSettingSize
    && sizeof(GlobalSwitches::SoundSettings) == mac::kSoundSettingCount * mac::kSoundSettingSize
    && offsetof(SoundSetting, ConfigValue) + 2 * sizeof(float) == mac::kSoundSettingValue && offsetof(SoundSetting, ConfigKey) == mac::kSoundSettingKey
    && offsetof(SoundSetting, field_40) == mac::kSoundSettingFlag && offsetof(SoundSetting, field_44) == mac::kSoundSettingText
    && offsetof(SoundSetting, UpdateProc) == mac::kSoundSettingProc);
static_assert(offsetof(GlobalSwitches, CameraSwitches) == mac::kSwitchesCameras && offsetof(GlobalSwitches, ControlSwitches) == mac::kSwitchesControls
    && offsetof(GlobalSwitches, SomeSettings) == mac::kSwitchesAlignedArray);
static_assert(offsetof(GlobalSwitches, AutoListenEnabled) == mac::kSwitchesAutoListenEnabled && offsetof(GlobalSwitches, CameraShakeEnabled) == mac::kSwitchesCameraShakeEnabled
    && offsetof(GlobalSwitches, TwitchEnableOverlay) == mac::kSwitchesTwitchEnableOverlay && offsetof(GlobalSwitches, LongRestScriptWaitingTime) == mac::kSwitchesLongRestScriptWaitingTime
    && offsetof(GlobalSwitches, UseLevelCache) == mac::kSwitchesUseLevelCache && offsetof(GlobalSwitches, LoadAllEffectPools) == mac::kSwitchesLoadAllEffectPools
    && offsetof(GlobalSwitches, ScriptLog) == mac::kSwitchesScriptLog && offsetof(GlobalSwitches, CheckRequirements) == mac::kSwitchesCheckRequirements
    && offsetof(GlobalSwitches, UseEndTurnFallback) == mac::kSwitchesUseEndTurnFallback && offsetof(GlobalSwitches, ShowOriginIntroInCC) == mac::kSwitchesShowOriginIntroInCC
    && offsetof(GlobalSwitches, AlwaysShowSplitter) == mac::kSwitchesAlwaysShowSplitter && offsetof(GlobalSwitches, NorthFacingMinimap) == mac::kSwitchesNorthFacingMinimap);
static_assert(offsetof(GlobalSwitches, HoldToEndTurn) == mac::kSwitchesHoldToEndTurn && offsetof(GlobalSwitches, SoundVolumeDynamicRange) == mac::kSwitchesSoundVolumeDynamicRange
    && offsetof(GlobalSwitches, SoundOcclusionQuality) == mac::kSwitchesSoundOcclusionQuality && offsetof(GlobalSwitches, MuteSoundWhenNotFocused) == mac::kSwitchesMuteSoundWhenNotFocused
    && offsetof(GlobalSwitches, CentreDialogue) == mac::kSwitchesCentreDialogue && offsetof(GlobalSwitches, ForceMono) == mac::kSwitchesForceMono
    && offsetof(GlobalSwitches, SoundPartyLosingThreshold) == mac::kSwitchesSoundPartyLosingThreshold && offsetof(GlobalSwitches, SoundPartyWinningThreshold) == mac::kSwitchesSoundPartyWinningThreshold
    && offsetof(GlobalSwitches, GameCameraSpeedMultiplier) == mac::kSwitchesGameCameraSpeedMultiplier && offsetof(GlobalSwitches, GameCameraRotationLocked) == mac::kSwitchesGameCameraRotationLocked
    && offsetof(GlobalSwitches, GameCameraEnableCloseUpDialog) == mac::kSwitchesGameCameraEnableCloseUpDialog && offsetof(GlobalSwitches, GameCameraEnableDynamicCombatCamera) == mac::kSwitchesGameCameraEnableDynamicCombatCamera);
static_assert(offsetof(GlobalSwitches, GameCameraEnableAttackCamera) == mac::kSwitchesGameCameraEnableAttackCamera && offsetof(GlobalSwitches, GameCameraEnableAttackCameraOtherPlayers) == mac::kSwitchesGameCameraEnableAttackCameraOtherPlayers
    && offsetof(GlobalSwitches, GameCameraEnableFlyFollow) == mac::kSwitchesGameCameraEnableFlyFollow && offsetof(GlobalSwitches, GameCameraAutoRotateSpeed) == mac::kSwitchesGameCameraAutoRotateSpeed
    && offsetof(GlobalSwitches, NrOfQuickSaves) == mac::kSwitchesNrOfQuickSaves && offsetof(GlobalSwitches, NrOfAutoSaves) == mac::kSwitchesNrOfAutoSaves
    && offsetof(GlobalSwitches, CanAutoSave) == mac::kSwitchesCanAutoSave && offsetof(GlobalSwitches, CanCrossSave) == mac::kSwitchesCanCrossSave
    && offsetof(GlobalSwitches, CanSaveDuringTimeline) == mac::kSwitchesCanSaveDuringTimeline && offsetof(GlobalSwitches, CanLoadDuringTimeline) == mac::kSwitchesCanLoadDuringTimeline
    && offsetof(GlobalSwitches, FirstTimeWithCrossSave) == mac::kSwitchesFirstTimeWithCrossSave && offsetof(GlobalSwitches, HasSeenEAMsg) == mac::kSwitchesHasSeenEAMsg);
static_assert(offsetof(GlobalSwitches, ForceSplitscreen) == mac::kSwitchesForceSplitscreen && offsetof(GlobalSwitches, MaxAmountDialogsInLog) == mac::kSwitchesMaxAmountDialogsInLog
    && offsetof(GlobalSwitches, ControllerCharacterWalkThreshold) == mac::kSwitchesControllerCharacterWalkThreshold && offsetof(GlobalSwitches, ControllerCharacterRunThreshold) == mac::kSwitchesControllerCharacterRunThreshold
    && offsetof(GlobalSwitches, ControllerRumbleStrength) == mac::kSwitchesControllerRumbleStrength && offsetof(GlobalSwitches, EnableVoiceLogging) == mac::kSwitchesEnableVoiceLogging
    && offsetof(GlobalSwitches, EnableLongPressAndHold) == mac::kSwitchesEnableLongPressAndHold && offsetof(GlobalSwitches, MouseLock) == mac::kSwitchesMouseLock
    && offsetof(GlobalSwitches, DisableEdgePanning) == mac::kSwitchesDisableEdgePanning && offsetof(GlobalSwitches, EscClosesAllUI) == mac::kSwitchesEscClosesAllUI
    && offsetof(GlobalSwitches, ShowSubtitles) == mac::kSwitchesShowSubtitles && offsetof(GlobalSwitches, DisplaySpeaker) == mac::kSwitchesDisplaySpeaker);
static_assert(offsetof(GlobalSwitches, ShowTextBackground) == mac::kSwitchesShowTextBackground && offsetof(GlobalSwitches, TextBackgroundOpacity) == mac::kSwitchesTextBackgroundOpacity
    && offsetof(GlobalSwitches, DialogueFontSizeModifier) == mac::kSwitchesDialogueFontSizeModifier && offsetof(GlobalSwitches, OverheadFontSizeModifier) == mac::kSwitchesOverheadFontSizeModifier
    && offsetof(GlobalSwitches, UseRadialContextMenu) == mac::kSwitchesUseRadialContextMenu && offsetof(GlobalSwitches, PeaceCharacterHighlightMode) == mac::kSwitchesPeaceCharacterHighlightMode
    && offsetof(GlobalSwitches, CombatCharacterHighlightMode) == mac::kSwitchesCombatCharacterHighlightMode && offsetof(GlobalSwitches, TacticalCharacterHighlightMode) == mac::kSwitchesTacticalCharacterHighlightMode
    && offsetof(GlobalSwitches, UnitSystem) == mac::kSwitchesUnitSystem && offsetof(GlobalSwitches, WeightedRolls) == mac::kSwitchesWeightedRolls
    && offsetof(GlobalSwitches, PostCombatAssignation) == mac::kSwitchesPostCombatAssignation && offsetof(GlobalSwitches, AutoFillHotbarCategories) == mac::kSwitchesAutoFillHotbarCategories);
static_assert(offsetof(GlobalSwitches, PointAndClickSoundFrequency) == mac::kSwitchesPointAndClickSoundFrequency && offsetof(GlobalSwitches, StartYear) == mac::kSwitchesStartYear
    && offsetof(GlobalSwitches, StartDay) == mac::kSwitchesStartDay);

// UI.h: two lock members that the survey of the game placed, and this build agrees with.
static_assert(offsetof(ui::WindowManager, LoadXAMLCS) == 0x78);
static_assert(offsetof(ui::StateMachine, CS_) == 0x20);

// The translated string repository (Layout/GameGlobals.h): the pools Norbyte's
// code reads, keyed as the game keys them (the handle alone, which his hash and
// comparison of a handle take), and the lock he takes for reading, the
// game's recursive spin lock (Layout/GameLocks.h).
static_assert(offsetof(TranslatedStringRepository, TranslatedStrings) == mac::kTranslatedPools
    && std::extent_v<decltype(TranslatedStringRepository::TranslatedStrings)> == mac::kTranslatedPoolCount
    && offsetof(TranslatedStringRepository, FallbackPool) == mac::kTranslatedFallbackPool
    && offsetof(TranslatedStringRepository, VersionedFallbackPool) == mac::kTranslatedVersionedFallbackPool
    && offsetof(TranslatedStringRepository, Lock) == mac::kTranslatedLock && sizeof(TranslatedStringRepository::Lock) == sizeof(mac::GameRecursiveSpinLock)
    && mac::kTranslatedLockOwner - mac::kTranslatedLock == offsetof(mac::GameRecursiveSpinLock, OwnerThread));
static_assert(offsetof(TranslatedStringRepository::TextPool, Texts) == mac::kTextPoolTexts && sizeof(LSStringView) == mac::kTextSize
    && sizeof(HashMap<RuntimeStringHandle, LSStringView>) == mac::kHashMapSize
    && offsetof(RuntimeStringHandle, Version) == mac::kTextHandleVersion && sizeof(RuntimeStringHandle) == 8);

// The input manager (Layout/GameGlobals.h), which Ext.Input.GetInputManager
// hands to Lua: the members the game's code shows. The signal and the end of
// the scheme are placed by their declarations only. The states' and the
// repeats' keys are hashed sign-extended, as the game's, and are to Lua the
// unsigned integers of their size.
using InputStatesKey = decltype(input::InputManager::InputStates)::key_type;
using InputRepeatsKey = decltype(input::InputManager::HoldRepeatEvents)::key_type;
static_assert(offsetof(input::InputManager, InputStates) == mac::kInputStates
    && sizeof(decltype(input::InputManager::InputStates)) == mac::kHashMapSize && sizeof(InputStatesKey) == 2
    && std::is_same_v<std::underlying_type_t<InputStatesKey>, input::InputDeviceId> && Hash(InputStatesKey(0xfffe)) == ~1ull
    && offsetof(input::InputManager, InputDefinitions) == mac::kInputDefinitions
    && offsetof(input::InputManager, InputScheme) == mac::kInputScheme
    && offsetof(input::InputManager, DeviceListeners) == mac::kInputDeviceListeners
    && offsetof(input::InputManager, PlayerDevices) == mac::kInputPlayerDevices && sizeof(input::InputManager::PlayerDevices[0]) == 4
    && offsetof(input::InputManager, PlayerDeviceIDs) == mac::kInputPlayerDeviceIds && sizeof(input::InputManager::PlayerDeviceIDs[0]) == 2
    && offsetof(input::InputManager, PlayerColors) == mac::kInputPlayerColors && sizeof(input::InputManager::PlayerColors[0]) == 16
    && offsetof(input::InputManager, PerDeviceData) == mac::kInputDevices
    && std::tuple_size_v<decltype(input::InputManager::PerDeviceData)> == mac::kInputDeviceCount);
static_assert(sizeof(input::InputDevice) == mac::kInputDeviceSize && offsetof(input::InputDevice, field_14) == mac::kInputDeviceColor
    && offsetof(input::InputDevice, field_24) == mac::kInputDeviceHasColor);
static_assert(offsetof(input::InputManager, CS) == mac::kInputLock && offsetof(input::InputManager, CS2) == mac::kInputInjectLock
    && offsetof(input::InputManager, OnInputEvent) == mac::kInputAfterLocks
    && offsetof(input::InputManager, HoldRepeatEvents) == mac::kInputRepeats && sizeof(input::HoldRepeatEvents) == mac::kInputRepeatSize
    && sizeof(InputRepeatsKey) == 1 && std::is_unsigned_v<std::underlying_type_t<InputRepeatsKey>> && Hash(InputRepeatsKey(0xff)) == ~0ull
    && offsetof(input::InputManager, PressedModifiers) == mac::kInputModifiers && sizeof(input::InputManager::PressedModifiers) == 1
    && offsetof(input::InputManager, LastUpdateTime) == mac::kInputUpdateTime
    && offsetof(input::InputManager, ControllerAllowKeyboardMouseInput) == mac::kInputFlags
    && offsetof(input::InputManager, AllowDeviceEvents) == mac::kInputFlags + 1
    && offsetof(input::InputManager, InputListeners) == mac::kInputListeners
    && offsetof(input::InputManager, NotifyUpListeners) == mac::kInputNotifyUpListeners
    && sizeof(input::InputListenerGroup) == mac::kListenerGroupSize && offsetof(input::InputListenerGroup, RefCount) == 0
    && offsetof(input::InputListenerGroup, TraversingListeners) == mac::kListenerGroupListeners
    && offsetof(input::InputListenerGroup, IsTraversing) == mac::kListenerGroupCompact
    && offsetof(input::InputManager, RawInputs) == mac::kInputRawInputs
    && offsetof(input::InputManager, CurrentRemap) == mac::kInputRemap
    && offsetof(input::InputManager, CurrentRemap) + offsetof(input::PendingInputRemap, Binding) == mac::kInputRemapBinding
    && offsetof(input::InputBinding, Modifiers) == mac::kBindingModifiers && offsetof(input::InputBinding, InputPlayerIndex) == mac::kBindingPlayer
    && offsetof(input::InputManager, Events) == mac::kInputEvents && sizeof(input::FireEventDesc) == mac::kInputEventSize
    && offsetof(input::InputManager, InputInjects) == mac::kInputInjects && sizeof(input::InjectInputData) == mac::kInjectSize
    && offsetof(input::InjectInputData, InjectRawInput) == mac::kInjectRaw
    && offsetof(input::InputManager, DeviceEventInjects) == mac::kInputDeviceEvents && sizeof(input::InjectDeviceEvent) == mac::kDeviceEventSize
    && offsetof(input::InjectDeviceEvent, DeviceId) == mac::kDeviceEventDevice
    && offsetof(input::InputManager, KeyboardLayoutTranslator) == mac::kInputTranslator && sizeof(input::InputManager) == mac::kInputSize);

// The boost prototype manager (Layout/GameGlobals.h): the map
// Ext.Stats.GetCachedBoost looks a GUID up in, keyed and hashed as the game
// does, and the prototype it hands to Lua.
static_assert(offsetof(stats::BoostPrototypeManager, ConditionsManager) == mac::kBoostManagerConditions
    && offsetof(stats::BoostPrototypeManager, Boosts) == mac::kBoostManagerBoosts
    && sizeof(HashMap<Guid, stats::BoostPrototype*>) == mac::kHashMapSize && sizeof(Guid) == 16
    && offsetof(stats::BoostPrototypeManager, ConditionHandles) == mac::kBoostManagerHandles
    && Hash(Guid{ 0x0123456789abcdefull, 0xfedcba9876543210ull }) == (0x0123456789abcdefull ^ 0xfedcba9876543210ull));
static_assert(sizeof(stats::BoostPrototype) == mac::kBoostSize && offsetof(stats::BoostPrototype, Description) == 0
    && offsetof(stats::BoostPrototype, TypeName) == mac::kBoostTypeName && offsetof(stats::BoostPrototype, BoostData) == mac::kBoostData
    && offsetof(stats::BoostPrototype, Boost) == mac::kBoostType && sizeof(stats::BoostPrototype::Boost) == 1
    && offsetof(stats::BoostPrototype, Condition) == mac::kBoostCondition);

// The texture atlases by path (Layout/GameGlobals.h): Norbyte's key is the
// game's 16-byte string, and his hash of it MurmurHash3 x64-128 with the
// seed 0, its first half (CoreLib/Base/BaseString.h), which is the game's.
using AtlasNode = MapNode<STDString, TextureAtlas*>;
static_assert(sizeof(STDString) == mac::kPathSize && offsetof(AtlasNode, Key) == mac::kAtlasNodePath
    && offsetof(AtlasNode, Value) == mac::kAtlasNodeAtlas && sizeof(AtlasNode) == mac::kAtlasNodeSize);

// The cursor control (Layout/GameGlobals.h), which Ext.UI.GetCursorControl
// hands to Lua. The name is what the destructor releases and the overlay the
// member the game makes none next to the cursor; that it is the overlay
// shown is his name for it, not something code shows.
static_assert(offsetof(ecl::CursorControl, ShowCursor) == mac::kCursorShow && sizeof(ecl::CursorControl::ShowCursor) == 1
    && offsetof(ecl::CursorControl, VisibleTimer) == mac::kCursorVisibleTimer
    && offsetof(ecl::CursorControl, FadeTimer) == mac::kCursorFadeTimer
    && offsetof(ecl::CursorControl, RequestedCursor) == mac::kCursorRequested
    && offsetof(ecl::CursorControl, RequestedOverlay) == mac::kCursorRequestedOverlay
    && offsetof(ecl::CursorControl, CursorOverrides) == mac::kCursorOverrides && sizeof(CursorType) == mac::kCursorTypeSize
    && offsetof(ecl::CursorControl, CursorName) == mac::kCursorFadeTimer + sizeof(float)
    && offsetof(ecl::CursorControl, CurrentOverlay) == mac::kCursorRequested - mac::kCursorTypeSize);
static_assert(offsetof(ecl::CursorControl, CursorName) == mac::kCursorName && offsetof(ecl::CursorControl, CurrentOverlay) == mac::kCursorOverlay
    && sizeof(ecl::CursorControl) == mac::kCursorSize
    && (uint32_t)CursorType::None == mac::kCursorNoOverlay && (uint32_t)CursorType::System == mac::kCursorDefault);

// The drag and drop manager (Layout/GameGlobals.h): the map of players
// Ext.UI.GetDragDrop looks a player up in, the drag data it hands to Lua and
// the controllers' player ids. The map's key is signed, so that his hash
// sign-extends it as the game's.
using DragKey = decltype(ecl::DragDropManager::PlayerData)::KeyType;
using DragNode = MapNode<DragKey, ecl::PlayerDragData>;
static_assert(offsetof(ecl::DragDropManager, PlayerData) == mac::kDragPlayers
    && offsetof(DragNode, Value) == mac::kDragNodeData && sizeof(DragNode) == mac::kDragNodeSize
    && offsetof(ecl::PlayerDragData, Entities) == 0 && offsetof(ecl::PlayerDragData, ScreenPosition) == mac::kDragDataPosition
    && offsetof(ecl::PlayerDragData, DraggingAction) == mac::kDragDataFlags && offsetof(ecl::PlayerDragData, IsDragging) == mac::kDragDataFlags + 1
    && offsetof(ecl::PlayerDragData, Active) == mac::kDragDataFlags + 2
    && offsetof(ecl::PlayerDragData, ActiveController) == mac::kDragDataActiveController
    && offsetof(ecl::PlayerDragData, Controllers) == mac::kDragDataControllers
    && offsetof(ecl::DragController, PlayerId) == mac::kDragControllerPlayer
    && sizeof(DragKey) == 2 && std::is_signed_v<DragKey> && Hash(DragKey{ -2 }) == ~1ull);

// The translated string key manager (Layout/GameGlobals.h): the map of keys
// Ext.Loca reads and inserts into, and the string in its nodes.
using TranslatedKeyNode = MapNode<FixedString, TranslatedString>;
static_assert(offsetof(TranslatedStringKeyManager, Keys) == mac::kTranslatedKeys
    && sizeof(LegacyMap<FixedString, TranslatedString>) == mac::kMapSize
    && offsetof(TranslatedKeyNode, Value) == mac::kTranslatedKeyString
    && offsetof(TranslatedString, ArgumentString) == mac::kTranslatedStringArgument && sizeof(TranslatedString) == 2 * sizeof(RuntimeStringHandle));

// The GUID resource manager (Layout/GameGlobals.h): the map of managers he
// looks a type's index up in, its key a 32-bit index that his hash
// sign-extends as the game does.
static_assert(offsetof(resource::GuidResourceManager, Definitions) == mac::kStaticDataManagers
    && sizeof(resource::StaticDataTypeIndex) == mac::kStaticDataIndexSize
    && std::is_same_v<std::underlying_type_t<resource::StaticDataTypeIndex>, int32_t>
    && Hash(int32_t{ -1 }) == ~0ull
    && sizeof(HashMap<resource::StaticDataTypeIndex, resource::GuidResourceBankBase*>) == mac::kHashMapSize);

// The level managers (Layout/GameGlobals.h): the current level his code
// reads for the server's and the client's, and the map of levels by name.
// The local template manager's place is shown too.
using LevelNode = MapNode<FixedString, EoCLevel*>;
static_assert(offsetof(LevelManager, LocalTemplateManager) == mac::kLevelManagerTemplates
    && offsetof(LevelManager, CurrentLevel) == mac::kLevelManagerCurrent
    && offsetof(LevelManager, Levels) == mac::kLevelManagerLevels && sizeof(LegacyMap<FixedString, EoCLevel*>) == mac::kMapSize
    && offsetof(LevelNode, Value) == mac::kMapNodePointerValue
    && std::is_base_of_v<LevelManager, esv::LevelManager> && std::is_base_of_v<LevelManager, ecl::LevelManager>);
// What Ext.Level.GetLevelInfo reads through the local template manager, and
// the server level's template cache that Ext.Template reads: the game's own
// lookup of a level's data goes through these members and this hash.
static_assert(offsetof(LocalTemplateManager, LevelDataManager) == mac::kTemplatesLevelData
    && offsetof(LevelDataManager, Levels) == mac::kLevelDataLevels && sizeof(HashMap<FixedString, LevelData*>) == mac::kHashMapSize
    && std::is_same_v<decltype(std::declval<FixedString const&>().GetHash()), uint32_t>
    && offsetof(LevelBase, LocalTemplateManager) == mac::kLevelTemplates
    && sizeof(esv::Level) == mac::kServerLevelSize && offsetof(esv::Level, CacheTemplateManager) == mac::kServerLevelTemplateCache
    && sizeof(LevelCacheTemplateManager) == mac::kLevelTemplateCacheSize && offsetof(LevelCacheTemplateManager, LevelName) == mac::kLevelTemplateCacheName
    && (uint32_t)TemplateType::LevelCacheTemplate == mac::kLevelTemplateCacheType);
// The templates a level's data tracks, which Ext.Level.GetLevelInfo's result
// shows: an array of them for each name.
static_assert(offsetof(LevelData, TrackedTemplateLists) == mac::kLevelDataTrackedTemplates
    && sizeof(decltype(LevelData::TrackedTemplateLists)::value_type) == mac::kLevelDataTrackedValueSize
    && std::is_same_v<decltype(LevelData::TrackedTemplateLists)::value_type, Array<GameObjectTemplate*>>);

// The template cache (Layout/GameGlobals.h): the members his caching of
// templates and the counts of their references read and write, and the lock
// he takes, the game's recursive kernel lock (Layout/GameLocks.h).
static_assert(offsetof(CacheTemplateManagerBase, TemplateManagerType) == mac::kTemplateCacheType && sizeof(TemplateType) == 1
    && offsetof(CacheTemplateManagerBase, Templates) == mac::kTemplateCacheByName
    && offsetof(CacheTemplateManagerBase, TemplatesByHandle) == mac::kTemplateCacheByHandle
    && offsetof(CacheTemplateManagerBase, RefCountsByHandle) == mac::kTemplateCacheRefCounts
    && sizeof(HashMap<FixedString, GameObjectTemplate*>) == mac::kHashMapSize && sizeof(HashMap<uint32_t, uint32_t>) == mac::kHashMapSize
    && offsetof(CacheTemplateManagerBase, Lock) == mac::kTemplateCacheLock && sizeof(CacheTemplateManagerBase::Lock) == sizeof(mac::GameRecursiveKernelLock)
    && offsetof(CacheTemplateManagerBase, AddedTemplates) == mac::kTemplateCacheAdded
    && offsetof(CacheTemplateManagerBase, RemovedTemplates) == mac::kTemplateCacheRemoved
    && offsetof(CacheTemplateManagerBase, EnableTemplateSync) == mac::kTemplateCacheSync);

// The sound manager's sound objects (Layout/GameAudio.h), which Ext.Audio
// reads by name. The game's code shows all seven arrays and the music object.
static_assert(offsetof(SoundManager, PlayerEmitter) == mac::kSoundPlayerEmitters
    && offsetof(SoundManager, Listener) == mac::kSoundListeners
    && offsetof(SoundManager, RumbleListener) == mac::kSoundRumbleListeners
    && offsetof(SoundManager, ControllerSpeakerListener) == mac::kSoundSpeakerListeners
    && offsetof(SoundManager, Ambient) == mac::kSoundAmbient
    && offsetof(SoundManager, HUD) == mac::kSoundHud
    && offsetof(SoundManager, CineHUD) == mac::kSoundCineHud
    && sizeof(SoundManager::Listener) == mac::kSoundObjectArray && offsetof(SoundManager, MusicHandle) == mac::kSoundMusic);

// A physics resource's boxes and capsules (Layout/GameResourceManager.h),
// which Lua reads by their members.
using PhysicsBox = resource::PhysicsResource::ObjectTemplate::PhysicsBox;
using PhysicsCapsule = resource::PhysicsResource::ObjectTemplate::PhysicsCapsule;
static_assert(offsetof(PhysicsBox, Kinematic) == mac::kPhysicsKinematic && offsetof(PhysicsBox, Mass) == mac::kPhysicsMass
    && offsetof(PhysicsBox, UUID) == mac::kPhysicsName && offsetof(PhysicsBox, Extends) == mac::kPhysicsBoxExtents
    && sizeof(PhysicsBox) == mac::kPhysicsObjectSize);
static_assert(offsetof(PhysicsCapsule, Kinematic) == mac::kPhysicsKinematic && offsetof(PhysicsCapsule, Mass) == mac::kPhysicsMass
    && offsetof(PhysicsCapsule, UUID) == mac::kPhysicsName && offsetof(PhysicsCapsule, Height) == mac::kPhysicsCapsuleHeight
    && offsetof(PhysicsCapsule, Radius) == mac::kPhysicsCapsuleRadius && sizeof(PhysicsCapsule) == mac::kPhysicsObjectSize);

// A sound component's object, which Ext.Audio reads for an entity.
static_assert(offsetof(SoundComponent, ActiveData) == mac::kSoundComponentActiveData && offsetof(SoundComponent, Flags) == mac::kSoundComponentFlags
    && offsetof(SoundComponent, Effect) == mac::kSoundComponentEffect
    && sizeof(SoundComponentActiveData) == mac::kActiveDataSize && offsetof(SoundComponentActiveData, SoundObjectId) == mac::kActiveDataObject
    && offsetof(SoundComponentActiveData, Entity) == mac::kActiveDataEntity && offsetof(SoundComponentActiveData, SoundSwitches) == mac::kActiveDataSwitches);

// The sound manager's ids come back in a register: a bank's and a name's as
// 32 bits, a sound object's as 64, and each function takes only the name.
static_assert(std::is_same_v<decltype(std::declval<SoundManager&>().LoadBank(nullptr)), SoundNameId>
    && std::is_same_v<decltype(std::declval<SoundManager&>().PrepareBank(nullptr)), SoundNameId>
    && std::is_same_v<decltype(std::declval<SoundManager&>().GetIDFromString(nullptr)), SoundNameId>
    && std::is_same_v<decltype(std::declval<SoundManager&>().RegisterSoundObject(nullptr)), SoundObjectId>
    && sizeof(SoundNameId) == 4 && sizeof(SoundObjectId) == 8);

// The results of virtual functions (Layout/GameVirtualResults.h). A stats
// expression is handed back as one pointer to the game's object, which clang
// passes through x8 because his reference counts it; the object's count is
// where the game starts it.
static_assert(sizeof(StatsExpressionRef) == sizeof(void*) && !std::is_trivially_copyable_v<StatsExpressionRef>);
static_assert(std::is_same_v<decltype(std::declval<DynamicStatsExpressionManager&>().CreateStatsExpression(std::declval<Guid const&>(),
    std::declval<StringView const&>())), StatsExpressionRef>);
static_assert(sizeof(StatsExpressionPooled) == mac::kStatsExpressionSize && offsetof(StatsExpressionPooled, RefCount) == mac::kStatsExpressionRefCount
    && sizeof(StatsExpressionPooled::RefCount) == sizeof(uint32_t));
// A position is three floats, which clang returns in s0 to s2; an optional
// one is trivially copyable and 16 bytes, which it returns in x0 and x1, the
// flag after the floats; an optional targeting type is two bytes in one register.
static_assert(std::is_same_v<decltype(std::declval<spell_cast::BaseTarget const&>().LEGACY_GetTargetPosition(std::declval<ecs::EntityWorld&>())), glm::vec3>);
static_assert(sizeof(glm::vec3) == 3 * sizeof(float) && std::is_trivially_copyable_v<glm::vec3>);
static_assert(sizeof(std::optional<glm::vec3>) == mac::kOptionalPositionSize && std::is_trivially_copyable_v<std::optional<glm::vec3>>);
static_assert(sizeof(std::optional<SpellType>) == mac::kOptionalTargetingSize && sizeof(SpellType) == 1
    && std::is_trivially_copyable_v<std::optional<SpellType>>);
static_assert(sizeof(decltype(std::declval<gn::TGenomeSet<float> const&>().GetTypeHash())) == mac::kGenomeTypeHashSize);

// The server's character (Layout/GameCharacter.h): his UserID is the user the
// game's GetOwnerUserID gives and UserID2 the one it is reserved for; both
// start unassigned, as his reserved user.
static_assert(offsetof(esv::Character, Template) == mac::kCharacterTemplate
    && offsetof(esv::Character, OriginalTemplate) == mac::kCharacterOriginalTemplate
    && offsetof(esv::Character, PlayerData) == mac::kCharacterPlayerData
    && offsetof(esv::Character, OwnerCharacter) == mac::kCharacterOwner
    && offsetof(esv::Character, FollowCharacter) == mac::kCharacterFollow
    && offsetof(esv::Character, EnemyCharacter) == mac::kCharacterEnemy && sizeof(EntityHandle) == 8
    && offsetof(esv::Character, BaseVisual) == mac::kCharacterBaseVisual
    && offsetof(esv::Character, UserID) == mac::kCharacterOwnerUser
    && offsetof(esv::Character, UserID2) == mac::kCharacterReservedUser && sizeof(UserId) == 4
    && offsetof(esv::Character, GeneralSpeedMultiplier) == mac::kCharacterSpeedMultiplier);
static_assert((uint32_t)UserId::Unassigned == mac::kUnassignedUser && ReservedUserId.Id == UserId::Unassigned);

END_SE()

BEGIN_NS(lua)
static_assert(std::is_same_v<std::variant<int, float>, mac::GameVariant<int, float>>);
END_NS()

BEGIN_BARE_NS(Noesis)
static_assert(std::is_same_v<std::variant<int, float>, bg3se::mac::GameVariant<int, float>>);

// The NoesisGUI members his UI code reads and writes, as the SDK's headers
// declare them, where the game's code has them (Layout/GameNoesis.h). The
// private ones (Type's symbol, the reference count, DependencyProperty's and
// RoutedEvent's members) are read through the SDK's accessors in
// Tests/NoesisTests.cpp.
static_assert(sizeof(BaseRefCounted) == bg3se::mac::kNoesisRefCountedSize);
static_assert(sizeof(Vector<int>) == bg3se::mac::kNoesisVectorBytes && sizeof(Vector<TypeMetaData*>) == bg3se::mac::kNoesisVectorBytes);
static_assert(sizeof(HashMap<uint32_t, Type*>) == bg3se::mac::kNoesisMapBytes);
static_assert(offsetof(SymbolManagerInternals, Strings) == bg3se::mac::kNoesisSymbolStrings
    && offsetof(SymbolManagerInternals, Keys) == bg3se::mac::kNoesisSymbolKeys);
static_assert(offsetof(ReflectionInternals, Names) == bg3se::mac::kNoesisReflectionNames
    && offsetof(ReflectionInternals, NameToType) == bg3se::mac::kNoesisReflectionTypes);
static_assert(offsetof(TypeMeta, mMetaData) == bg3se::mac::kNoesisTypeMetaData && sizeof(TypeMeta) == bg3se::mac::kNoesisTypeMetaSize);
static_assert(offsetof(TypeClass, mIsInterface) == bg3se::mac::kNoesisClassIsInterface && offsetof(TypeClass, mBase) == bg3se::mac::kNoesisClassBase
    && offsetof(TypeClass, mInterfaces) == bg3se::mac::kNoesisClassInterfaces && offsetof(TypeClass, mProperties) == bg3se::mac::kNoesisClassProperties
    && offsetof(TypeClass, mEvents) == bg3se::mac::kNoesisClassEvents && offsetof(TypeClass, mAncestors) == bg3se::mac::kNoesisClassAncestors);
static_assert(offsetof(TypeClass::AncestorInfo, offset) == bg3se::mac::kNoesisAncestorOffset && sizeof(TypeClass::AncestorInfo) == bg3se::mac::kNoesisAncestorBytes);
static_assert(offsetof(TypeProperty, mName) == bg3se::mac::kNoesisPropertyName && offsetof(TypeProperty, mType) == bg3se::mac::kNoesisPropertyType
    && offsetof(TypeProperty, mMetaData) == bg3se::mac::kNoesisPropertyMetaData && sizeof(TypeProperty) == bg3se::mac::kNoesisPropertySize);
static_assert(offsetof(TypeEnum, mValues) == bg3se::mac::kNoesisEnumValues);
static_assert(sizeof(String) == bg3se::mac::kNoesisStringBytes && offsetof(Uri, mUri) == bg3se::mac::kNoesisUriText);
static_assert(offsetof(DependencyObject, mValues) == bg3se::mac::kNoesisObjectValues);
static_assert(offsetof(StoredValue, provider) == bg3se::mac::kNoesisStoredProvider && sizeof(StoredValue) == 0x10);
static_assert(offsetof(StoredValue::ComplexValue, base) == bg3se::mac::kNoesisComplexBase && offsetof(StoredValue::ComplexValue, animated) == bg3se::mac::kNoesisComplexAnimated
    && offsetof(StoredValue::ComplexValue, coerced) == bg3se::mac::kNoesisComplexCoerced);
static_assert(offsetof(DependencyData, mProperties) == bg3se::mac::kNoesisDataProperties && offsetof(UIElementData, mEvents) == bg3se::mac::kNoesisDataEvents);
static_assert(offsetof(Visual, mVisualParent) == bg3se::mac::kNoesisVisualParent && offsetof(UIElement, mRoutedEventHandlers) == bg3se::mac::kNoesisElementHandlers);
static_assert(offsetof(FrameworkElement, mParent) == bg3se::mac::kNoesisFrameworkParent
    && offsetof(FrameworkElement, mTemplatedParent) == bg3se::mac::kNoesisFrameworkTemplatedParent
    && offsetof(FrameworkElement, mInheritanceParent) == bg3se::mac::kNoesisFrameworkInheritanceParent);
static_assert(sizeof(RoutedEventHandler) == bg3se::mac::kNoesisDelegateBytes);
// BaseCollection's items are protected: read from a class derived from it.
struct BaseCollectionItems : BaseCollection
{
    static constexpr size_t Offset() { return offsetof(BaseCollectionItems, mItems); }
};
static_assert(BaseCollectionItems::Offset() == bg3se::mac::kNoesisCollectionItems);
static_assert(offsetof(Boxed<int>, mValue) == bg3se::mac::kNoesisBoxedInt);
END_BARE_NS()
