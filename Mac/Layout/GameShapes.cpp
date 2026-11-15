#include <Layout/GameShapes.h>

#include <Layout/CodeProof.h>
#include <Layout/GameComponents.h>
#include <Layout/GameEffectsManager.h>
#include <Layout/GameEntityWorld.h>
#include <Layout/GameFiles.h>
#include <Layout/GameAudio.h>
#include <Layout/GameCamera.h>
#include <Layout/GameCharacter.h>
#include <Layout/GameGlobals.h>
#include <Layout/GameLocks.h>
#include <Layout/GameNetwork.h>
#include <Layout/GameNoesis.h>
#include <Layout/GameQueues.h>
#include <Layout/GameResourceManager.h>
#include <Layout/GameSession.h>
#include <Layout/GameStats.h>
#include <Layout/GameStringTable.h>
#include <Layout/GameSwitches.h>
#include <Layout/GameSystems.h>
#include <Layout/GameUi.h>
#include <Layout/GameVirtualResults.h>
#include <Layout/GameVariant.h>
#include <Platform/CodeSearch.h>
#include <Platform/Log.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <span>

namespace bg3se::mac {

namespace {

using proof::Code;
using proof::Expect;
using proof::Operand;
using proof::CheckOperand;
using proof::Hex;
using proof::LoadedAddress;
using proof::MoveValue;
using proof::PageOf;
using proof::ShiftLeftOf;
using proof::UnscaledOffset;

struct Proof
{
    GameShape Shape;
    char const* Function;
    char const* Symbol;
    CodeShape Expected;
    std::span<Expect const> Operands;
    // What the operand list cannot say: values that depend on one another,
    // data the code refers to.
    bool (*Extra)(Code const&, std::string&){ nullptr };
};

// ---------------------------------------------------------------------------
// ls::CriticalSection
// ---------------------------------------------------------------------------

constexpr uint64_t kLockCount = offsetof(GameCriticalSection, LockCount);
constexpr uint64_t kRecursionCount = offsetof(GameCriticalSection, RecursionCount);
constexpr uint64_t kOwnerThread = offsetof(GameCriticalSection, OwnerThread);
constexpr uint64_t kSpinCount = offsetof(GameCriticalSection, SpinCount);
constexpr uint64_t kSemaphore = offsetof(GameCriticalSection, Semaphore);

// Lock(): takes the count from -1 to 0 at +4, or adds to it; stores the
// caller's Mach thread port as the owner at +0xc; counts entries at +8; spins
// by the count at +0x10; sleeps on the semaphore at +0, creating it on first use.
constexpr Expect kCriticalSectionLock[] = {
    { 6, Operand::Add, kLockCount },
    { 7, Operand::Move, (uint32_t)kFreeCriticalSection.LockCount },
    { 11, Operand::CallsImport, 0, "_pthread_self" },
    { 12, Operand::CallsImport, 0, "_pthread_mach_thread_np" },
    { 14, Operand::Offset, kOwnerThread },
    { 16, Operand::Offset, kOwnerThread },
    { 24, Operand::Offset, kRecursionCount },
    { 26, Operand::Offset, kRecursionCount },
    { 32, Operand::Offset, kSpinCount },
    { 75, Operand::Offset, kSemaphore },
    { 81, Operand::CallsImport, 0, "_semaphore_create" },
    { 95, Operand::CallsImport, 0, "_semaphore_timedwait" },
};

// Unlock(): one entry less at +8; at zero the owner at +0xc becomes 0, the
// count at +4 drops by one and a waiter, if any, is signalled.
constexpr Expect kCriticalSectionUnlock[] = {
    { 6, Operand::Offset, kRecursionCount },
    { 8, Operand::Offset, kRecursionCount },
    { 14, Operand::Offset, kOwnerThread },
    { 14, Operand::StoresZero },
    { 15, Operand::Add, kLockCount },
    { 20, Operand::Offset, kSemaphore },
    { 41, Operand::CallsImport, 0, "_semaphore_signal" },
};

// ~CriticalSection(): count -1 and recursion 0 at +4, owner 0 at +0xc, the
// semaphore at +0 destroyed and zeroed.
constexpr Expect kCriticalSectionDestroy[] = {
    { 5, Operand::Unscaled, kLockCount },
    { 6, Operand::Offset, kOwnerThread },
    { 6, Operand::StoresZero },
    { 7, Operand::Offset, kSemaphore },
    { 11, Operand::CallsImport, 0, "_semaphore_destroy" },
    { 12, Operand::Offset, kSemaphore },
    { 12, Operand::StoresZero },
};

// ls::Dispatcher::Dispatcher(unsigned) constructs two critical sections that
// are adjacent members, and the member after them: the distance between the
// two is the size of one, and what is stored is a free critical section.
bool CheckDispatcherConstructor(Code const& f, std::string& problem)
{
    auto first = UnscaledOffset(f.At(12));      // str q0, [x19, #first]!
    auto second = UnscaledOffset(f.At(18));     // str q0, [x20, #second]!
    auto spin = MoveValue(f.At(14));            // mov w8, #spin count
    auto firstSpin = LoadStoreOffset(f.At(15)); // str w8, [x0, #first + SpinCount]
    auto secondSpin = LoadStoreOffset(f.At(19));
    auto next = LoadStoreOffset(f.At(21));      // the member after the second
    if (!first || !second || !spin || !firstSpin || !secondSpin || !next) {
        problem = "the stores that construct the two critical sections are not where they were";
        return false;
    }
    if (*second - *first != (int64_t)sizeof(GameCriticalSection) || *next != (uint64_t)*second + sizeof(GameCriticalSection)) {
        problem = "two adjacent critical sections are " + Hex((uint64_t)(*second - *first)) + " apart and the next member follows at +"
            + Hex(*next - (uint64_t)*second) + ", where the Mac type has " + Hex(sizeof(GameCriticalSection));
        return false;
    }
    if (*firstSpin != (uint64_t)*first + kSpinCount || *secondSpin != (uint64_t)*second + kSpinCount
        || *spin != (uint64_t)kFreeCriticalSection.SpinCount) {
        problem = "a new critical section's spin count is not stored as the Mac type has it";
        return false;
    }
    // The 16 bytes both stores write: the fields before the spin count.
    auto bytes = f.Image.DataAt(LoadedAddress(f, 9), kSpinCount);
    if (!bytes || memcmp(bytes, &kFreeCriticalSection, kSpinCount) != 0) {
        problem = "a new critical section does not start as the Mac type's free one";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ls::SRWKernelLock
// ---------------------------------------------------------------------------

constexpr uint64_t kState = offsetof(GameSRWKernelLock, State);
constexpr uint64_t kSharedSemaphore = offsetof(GameSRWKernelLock, SharedSemaphore);
constexpr uint64_t kExclusiveSemaphore = offsetof(GameSRWKernelLock, ExclusiveSemaphore);
constexpr char kReadUnlockSymbol[] = "__ZN2ls13SRWKernelLock10ReadUnlockEv";
constexpr char kWriteLockSymbol[] = "__ZN2ls13SRWKernelLock9WriteLockEv";
constexpr char kLeaveExclusiveSymbol[] = "__ZL23srwlock_leave_exclusiveP12_RTL_SRWLOCKj";

// ReadLock(): while no writer holds or waits (the mask at +8), adds one
// reader; otherwise queues as a writer would, sleeps on the semaphore at +4,
// and after being let through sleeps on the one at +0 if writers remain.
constexpr Expect kKernelReadLock[] = {
    { 6, Operand::Offset, kState },
    { 7, Operand::Mask, kKernelWriters },
    { 10, Operand::Add, 1 },
    { 11, Operand::Add, kState },
    { 45, Operand::Mask, kKernelWriters },
    { 50, Operand::Add, kKernelWriter },
    { 72, Operand::Offset, kExclusiveSemaphore },
    { 94, Operand::CallsImport, 0, "_semaphore_wait" },
    { 118, Operand::Calls, 0, kLeaveExclusiveSymbol },
    { 121, Operand::Offset, kSharedSemaphore },
    { 142, Operand::CallsImport, 0, "_semaphore_wait" },
};

// ReadUnlock(): one reader less at +8; the last reader out wakes a waiting
// writer through the semaphore at +4.
constexpr Expect kKernelReadUnlock[] = {
    { 7, Operand::Unscaled, kState },
    { 8, Operand::Add, 1 },
    { 9, Operand::Mask, kKernelWriters },
    { 10, Operand::Mask, kKernelReaders },
    { 12, Operand::Mask, kKernelExclusive },
    { 34, Operand::Offset, kExclusiveSemaphore },
    { 56, Operand::CallsImport, 0, "_semaphore_signal" },
};

// WriteLock(): a free lock (0 at +8) becomes one writer in exclusive mode;
// otherwise the writer is counted and sleeps on the semaphore at +4.
constexpr Expect kKernelWriteLock[] = {
    { 6, Operand::Add, kState },
    { 7, Operand::Move, kKernelExclusive | kKernelWriter },
    { 22, Operand::Add, kKernelWriter },
    { 23, Operand::Mask, kKernelWriters },
    { 24, Operand::Mask, kKernelReaders },
    { 26, Operand::Mask, kKernelExclusive },
    { 44, Operand::Offset, kExclusiveSemaphore },
    { 66, Operand::CallsImport, 0, "_semaphore_wait" },
};

// WriteUnlock(): one writer less at +8 (and out of exclusive mode when it
// was the last), then whoever waits is woken.
constexpr Expect kKernelWriteUnlock[] = {
    { 1, Operand::Unscaled, kState },
    { 2, Operand::Add, kKernelWriter },
    { 3, Operand::Mask, kKernelReaders },
    { 4, Operand::Mask, kKernelWriters },
    { 20, Operand::JumpsTo, 0, kLeaveExclusiveSymbol },
};

// The wake-up after a writer leaves: the next writer through the semaphore
// at +4, or else every waiting reader through the one at +0.
constexpr Expect kKernelLeaveExclusive[] = {
    { 6, Operand::Mask, kKernelWriters },
    { 8, Operand::Offset, kExclusiveSemaphore },
    { 26, Operand::Mask, kKernelReaders },
    { 28, Operand::Offset, kSharedSemaphore },
};

constexpr Expect kKernelDestroy[] = {
    { 7, Operand::Offset, kSharedSemaphore },
    { 8, Operand::CallsImport, 0, "_semaphore_destroy" },
    { 10, Operand::Offset, kExclusiveSemaphore },
    { 11, Operand::CallsImport, 0, "_semaphore_destroy" },
};

// vt::GraniteManager::KeepWarm indexes an array of kernel locks and takes
// the write side of one: the index is scaled by the size of a lock.
bool CheckKeepWarm(Code const& f, std::string& problem)
{
    auto shift = ShiftLeftOf(f.At(50));         // add x21, x21, x8, lsl #shift
    if (!shift || (1ull << *shift) != sizeof(GameSRWKernelLock)) {
        problem = "an array of kernel locks is not indexed in steps of " + Hex(sizeof(GameSRWKernelLock));
        return false;
    }
    return true;
}
constexpr Expect kKeepWarm[] = {
    { 54, Operand::Calls, 0, kWriteLockSymbol },
};

// ---------------------------------------------------------------------------
// ls::RecursiveSRWLock<ls::SRWKernelLock>
// ---------------------------------------------------------------------------

// ~ScopedWriteLock(): one entry less at +0x18; at zero the owner at +0x10
// becomes "none" and the kernel lock's write side is released.
constexpr Expect kRecursiveKernelWriteUnlock[] = {
    { 6, Operand::Offset, offsetof(GameRecursiveKernelLock, WriteEnterCount) },
    { 8, Operand::Offset, offsetof(GameRecursiveKernelLock, WriteEnterCount) },
    { 11, Operand::Unscaled, offsetof(GameRecursiveKernelLock, Lock) + kState },
    { 12, Operand::Move, kNoRecursiveOwner },
    { 13, Operand::Offset, offsetof(GameRecursiveKernelLock, OwnerThread) - (offsetof(GameRecursiveKernelLock, Lock) + kState) },
    { 14, Operand::Add, kKernelWriter },
    { 32, Operand::Calls, 0, kLeaveExclusiveSymbol },
};

// ~ScopedReadLock(): nothing when the caller (its Mach thread port,
// zero-extended) is the owner at +0x10; otherwise the kernel lock's read
// side is released.
constexpr Expect kRecursiveKernelReadUnlock[] = {
    { 6, Operand::Offset, offsetof(GameRecursiveKernelLock, OwnerThread) },
    { 7, Operand::Add, 1 },         // cmn: the owner is ~0
    { 9, Operand::CallsImport, 0, "_pthread_self" },
    { 10, Operand::CallsImport, 0, "_pthread_mach_thread_np" },
    { 11, Operand::Offset, offsetof(GameRecursiveKernelLock, OwnerThread) },
    { 15, Operand::Calls, 0, kReadUnlockSymbol },
};

// ---------------------------------------------------------------------------
// ls::RecursiveSRWLock<ls::SRWSpinLock>
// ---------------------------------------------------------------------------

// WriteLock(): unless the caller owns the lock (+8), adds one writer to the
// word at +0, backs off while another writer is there, waits for the readers
// below it to leave, and becomes the owner; then one more entry at +0x10.
constexpr Expect kRecursiveSpinWriteLock[] = {
    { 5, Operand::Offset, offsetof(GameRecursiveSpinLock, OwnerThread) },
    { 6, Operand::Add, 1 },
    { 8, Operand::CallsImport, 0, "_pthread_self" },
    { 9, Operand::CallsImport, 0, "_pthread_mach_thread_np" },
    { 10, Operand::Offset, offsetof(GameRecursiveSpinLock, OwnerThread) },
    { 13, Operand::Move, kSpinWriter },
    { 14, Operand::Move, (uint32_t)(0 - kSpinWriter) },
    { 22, Operand::ShiftRight, 20 },
    { 44, Operand::Mask, kSpinReaders },
    { 69, Operand::Offset, offsetof(GameRecursiveSpinLock, OwnerThread) },
    { 70, Operand::Offset, offsetof(GameRecursiveSpinLock, WriteEnterCount) },
    { 72, Operand::Offset, offsetof(GameRecursiveSpinLock, WriteEnterCount) },
};
static_assert(kSpinWriter == 1u << 20);

// ~ScopedReadLock(): nothing when the caller is the owner at +8; otherwise
// one reader less in the word at +0.
constexpr Expect kRecursiveSpinReadUnlock[] = {
    { 6, Operand::Offset, offsetof(GameRecursiveSpinLock, OwnerThread) },
    { 7, Operand::Add, 1 },
    { 9, Operand::CallsImport, 0, "_pthread_self" },
    { 10, Operand::CallsImport, 0, "_pthread_mach_thread_np" },
    { 11, Operand::Offset, offsetof(GameRecursiveSpinLock, OwnerThread) },
    { 14, Operand::Move, 0xffffffffull },
};

// ---------------------------------------------------------------------------
// ls::Variant
// ---------------------------------------------------------------------------

// Stand-ins with the size and alignment of the game's alternatives: a string
// (16 bytes, aligned to 8), Vector3f, an entity handle, a FixedString, an
// empty class.
struct String16 { uint64_t Words[2]; };
struct Vector3 { float X, Y, Z; };
struct Empty {};
using StringVariant = GameVariant<String16, int, double, bool, Vector3>;
using RulesetVariant = GameVariant<Empty, int, float, uint32_t, bool>;
using TargetVariant = GameVariant<uint64_t, Vector3>;
static_assert(sizeof(StringVariant) == 0x18 && alignof(StringVariant) == 8 && StringVariant::IndexOffset == 0x10);
static_assert(sizeof(RulesetVariant) == 8 && alignof(RulesetVariant) == 4 && RulesetVariant::IndexOffset == 4);
static_assert(sizeof(TargetVariant) == 0x10 && alignof(TargetVariant) == 8 && TargetVariant::IndexOffset == 0xc);

// The destructor reads the index as a byte after the 16 bytes of storage.
constexpr Expect kStringVariantDestroy[] = {
    { 4, Operand::ByteOffset, StringVariant::IndexOffset },
};
// Storage::Assign compares the two indexes, bytes after 4 bytes of storage.
constexpr Expect kRulesetVariantAssign[] = {
    { 6, Operand::ByteOffset, RulesetVariant::IndexOffset },
    { 7, Operand::ByteOffset, RulesetVariant::IndexOffset },
};
// scr::CreatePuddleAt takes an ls::Variant<entity handle, Vector3f> by
// reference and reads its index after the 12 bytes of the vector, not after
// the 16 bytes a union of the two would take.
constexpr Expect kTargetVariantRead[] = {
    { 30, Operand::ByteOffset, TargetVariant::IndexOffset },
    { 66, Operand::ByteOffset, TargetVariant::IndexOffset },
};

// ---------------------------------------------------------------------------
// The lock-free queues
// ---------------------------------------------------------------------------

constexpr uint64_t kLineShift = 7;
static_assert(kQueueLine == 1ull << kLineShift);

// ls::MPMCQueueBounded<short>::Grow(unsigned long) allocates the new slots
// aligned to a line and one line each, moves the values (at +8 of a slot)
// from the read index (two lines into the queue) on, and sets the write index
// (one line in) to their number and the read index to zero.
constexpr Expect kQueueGrow[] = {
    { 6, Operand::Offset, 0 },                              // the capacity
    { 33, Operand::ShiftLeftBy, kLineShift },               // bytes to allocate: slots << 7
    { 36, Operand::Move, kQueueLine },                      // their alignment
    { 37, Operand::CallsImport, 0, "_posix_memalign" },
    { 70, Operand::Offset, 8 },                             // the slots
    { 72, Operand::Add, kQueueReadIndex },
    { 74, Operand::PairOffset, 0 },
    { 77, Operand::ShiftLeftBy, kLineShift },
    { 92, Operand::Offset, kQueueSlotValue },
    { 105, Operand::Offset, kQueueWriteIndex },
    { 106, Operand::Offset, kQueueReadIndex },
    { 106, Operand::StoresZero },
};

// ls::MPMCQueueUnbounded<int>::Emplace<int&>(int&) takes the read side of
// the kernel lock at +0, then works on the bounded queue one line in: its
// capacity and slots, its write index, slots of one line each.
constexpr Expect kLockedQueueEmplace[] = {
    { 7, Operand::Add, kLockedQueueQueue },
    { 13, Operand::Calls, 0, "__ZN2ls13SRWKernelLock8ReadLockEv" },
    { 16, Operand::Add, kLockedQueueQueue + kQueueWriteIndex },
    { 24, Operand::PairOffset, kLockedQueueQueue },
    { 27, Operand::ShiftLeft, kLineShift },
    { 47, Operand::Offset, kQueueSlotValue },
};

// ecs::EntityHandleGenerator::Create() indexes the per-thread states, which
// stay 64 bytes apart.
constexpr Expect kHandleGeneratorCreate[] = {
    { 12, Operand::ShiftLeft, 6 },
    { 15, Operand::Offset, kHandleFreeCount },
    { 22, Operand::Offset, kPagedArraySize },
    { 23, Operand::Offset, kPagedArrayPageCount },
    { 42, Operand::Offset, kHandleNextFree },
    { 55, Operand::ShiftLeftBy, kHandleThreadShift },
    { 133, Operand::Offset, kPagedArrayBits },
    { 137, Operand::Offset, kHandleLastFree },
    { 167, Operand::Mask, kHandleFirstSalt },
};
static_assert(kHandleGeneratorThreadState == 1 << 6);

// ---------------------------------------------------------------------------
// The locks of the game's NoesisGUI
// ---------------------------------------------------------------------------

// Noesis::Reflection::RegisterType(Symbol, creator, filler) takes the
// reflection lock unless the caller (its Mach thread port) already owns it,
// with OSSpinLockLock, and on the way out resets the owner to "none" and
// releases with OSSpinLockUnlock.
constexpr Expect kNoesisRegisterType[] = {
    { 11, Operand::CallsImport, 0, "_pthread_self" },
    { 12, Operand::CallsImport, 0, "_pthread_mach_thread_np" },
    { 20, Operand::CallsImport, 0, "_OSSpinLockLock" },
    { 81, Operand::Move, kNoNoesisOwner },
    { 85, Operand::CallsImport, 0, "_OSSpinLockUnlock" },
};

// Noesis::SymbolManager::AddString(char const*) holds the symbol lock, the
// same kind of lock, without an owner.
constexpr Expect kNoesisAddString[] = {
    { 10, Operand::CallsImport, 0, "_OSSpinLockLock" },
    { 128, Operand::CallsImport, 0, "_OSSpinLockUnlock" },
    { 147, Operand::CallsImport, 0, "_OSSpinLockUnlock" },
};

// Noesis::TypeClass::TypeClass(Symbol, bool) allocates the ancestors object
// and zeroes the four bytes of its lock; CollapseAncestors takes that lock.
constexpr Expect kNoesisTypeClass[] = {
    { 27, Operand::Move, kNoesisAncestorsSize },
    { 31, Operand::Unscaled, kNoesisAncestorsLock },
    { 31, Operand::StoresZero },
};
constexpr Expect kNoesisCollapseAncestors[] = {
    { 16, Operand::Add, kNoesisAncestorsLock },
    { 17, Operand::CallsImport, 0, "_OSSpinLockLock" },
};

// ---------------------------------------------------------------------------
// ecs::EntityWorld and the component registry
// ---------------------------------------------------------------------------

// RegisterComponentType<navcloud::AgentComponent>(ecs::EntityWorld&) builds a
// component type entry with both sizes in one word, makes room in the
// world's callbacks, indexes the registry's entries by the type index in
// steps of one entry, reads the world's storage and makes room in its
// component operations.
constexpr uint64_t kAgentComponentSizes = 4 | (4 << 16);        // navcloud::AgentComponent: 4 bytes inline, 4 in all
constexpr Expect kRegisterComponentType[] = {
    { 22, Operand::Mask, kAgentComponentSizes },     // mov w8, #0x40004, encoded as a bitmask
    { 65, Operand::Add, kWorldComponentCallbacks },
    { 89, Operand::Offset, kWorldComponentTypes },
    { 90, Operand::ShiftLeft, 6 },
    { 104, Operand::Offset, kWorldStorage },
    { 147, Operand::Add, kWorldComponentOps },
};
static_assert(kComponentTypeEntrySize == 1 << 6);
// The sizes word is stored into the entry being built on the stack, at the
// entry's start plus kComponentTypeEntrySizes: the entry starts where the
// type index is stored.
bool CheckRegisterComponentType(Code const& f, std::string& problem)
{
    auto index = LoadStoreOffset(f.At(19));     // strh w19, [sp, #entry]
    auto sizes = LoadStoreOffset(f.At(23));     // str w8, [sp, #entry + 0x10]
    if (!index || !sizes || *sizes - *index != kComponentTypeEntrySizes) {
        problem = "the component sizes are not stored at +" + Hex(kComponentTypeEntrySizes) + " of the new entry";
        return false;
    }
    return true;
}

// ~ComponentTypeEntry() frees the two arrays of the entry.
constexpr Expect kComponentTypeEntryDestroy[] = {
    { 8, Operand::Offset, kComponentTypeEntryDependencies + 0xc },
    { 12, Operand::Offset, kComponentTypeEntryDependencies },
    { 58, Operand::Offset, kComponentTypeEntryDependencies + 8 },
    { 88, Operand::Offset, kComponentTypeEntryDependents + 0xc },
    { 92, Operand::Offset, kComponentTypeEntryDependents },
    { 138, Operand::Offset, kComponentTypeEntryDependents + 8 },
};

// ecs::EntityWorld::IsEntityAlive reads the handle generator and indexes its
// per-thread states.
constexpr Expect kWorldIsEntityAlive[] = {
    { 7, Operand::Offset, kWorldHandleGenerator },
    { 8, Operand::ShiftLeft, 6 },
};

// ecs::ComponentOps<eoc::HealthComponent>::AddImmediateDefaultComponent
// reaches the immediate cache through the world.
constexpr Expect kWorldAddImmediate[] = {
    { 7, Operand::Offset, kWorldCache },
};

// ls::DynamicBitSet<ls::TaggedAllocator<int>>::Ensure(index, value) grows
// the set when its size is not past the index, and reads the words from
// the set itself unless the capacity is past 64 bits. The destructor
// zeroes the size and frees the words when the capacity is past 64.
constexpr Expect kBitSetEnsure[] = {
    { 7, Operand::Offset, kBitSetSize },
    { 14, Operand::Offset, kBitSetCapacity },
    { 15, Operand::Add, kBitSetInlineBits + 1 },
};
constexpr Expect kBitSetDestroy[] = {
    { 4, Operand::Offset, kBitSetSize },
    { 4, Operand::StoresZero },
    { 5, Operand::Offset, kBitSetCapacity },
    { 6, Operand::Add, kBitSetInlineBits + 1 },
};

// ecs::EntityWorld::GetComponent<eoc::HealthComponent, true> finds the
// component and marks it changed: the entity's bit in the slot of its
// component page, the slot's bit in the storage's mask of changed slots,
// and, when that mask was empty, the storage's index in the container's
// set of changed storages (inline unless past 64 bits).
constexpr Expect kWorldMarkChanged[] = {
    { 106, Operand::Offset, kStorageDataComponents },
    { 109, Operand::Offset, kStorageDataComponentSlots + kHashMapValues },
    { 111, Operand::ShiftLeft, kComponentPageSlotShift },
    { 112, Operand::Add, kComponentPageSlotModified },
    { 113, Operand::Add, kStorageDataModifiedComponents },
    { 133, Operand::Add, kStorageUsedFrameData },
    { 134, Operand::Offset, kStorageDataStorageIndex },
    { 137, Operand::Offset, kStorageUsedFrameData + kBitSetCapacity },
    { 138, Operand::Add, kBitSetInlineBits + 1 },
};

// ecs::EntityStorageData::~EntityStorageData() frees, from the last member
// back, the arrays of the queries, the frame data (which holds the
// one-frame components), the maps from an entity and from a component
// type, then the arrays of page indices, page masks, entity pages and
// component pages.
constexpr Expect kStorageDataDestroy[] = {
    { 272, Operand::Add, kStorageDataEntities },
    { 298, Operand::Add, kStorageDataComponentSlots },
    { 380, Operand::Offset, kStorageDataPageInfos + kArrayCapacity },
    { 386, Operand::Offset, kStorageDataPageInfos },
    { 460, Operand::Offset, kStorageDataHandles + kArrayCapacity },
    { 462, Operand::Offset, kStorageDataHandles + kArraySize },
    { 466, Operand::Offset, kStorageDataHandles },
    { 489, Operand::CallsImport, 0, "_free" },
    { 540, Operand::Offset, kStorageDataComponents + kArrayCapacity },
    { 546, Operand::Offset, kStorageDataComponents },
};

// ecs::EntityStorageClearFrameData(storage, types) reads the one-frame
// components only when the storage says it has any: the keys (16-bit
// types) and the values (hash maps, 0x40 bytes each) of their map.
constexpr Expect kStorageClearFrameData[] = {
    { 23, Operand::Add, kStorageDataModifiedComponents },
    { 37, Operand::Offset, kStorageDataComponents },
    { 131, Operand::ByteOffset, kStorageDataHasOneFrameComponents },
    { 134, Operand::Offset, kStorageDataOneFrameComponents + kHashMapKeysSize },
    { 136, Operand::Offset, kStorageDataOneFrameComponents + kHashMapKeys },
    { 138, Operand::Offset, kStorageDataOneFrameComponents + kHashMapValues },
};

// ecs::EntityStoragePurgeAll(storage, types) calls each slot's destructor
// on the used entries of each component page: the slots' entries in steps
// of 16 with the destructor at +8, the sizes, the pages' used masks, the
// component pages.
constexpr Expect kStoragePurgeAll[] = {
    { 10, Operand::Offset, kStorageDataComponents + kArraySize },
    { 13, Operand::Offset, kStorageDataSlotCount },
    { 23, Operand::Offset, kStorageDataComponentDtors },
    { 24, Operand::ShiftLeft, kComponentDtorEntryShift },
    { 25, Operand::Offset, kComponentDtorEntryProc },
    { 28, Operand::Offset, kStorageDataComponentSizes },
    { 34, Operand::Offset, kStorageDataPageInfos },
    { 36, Operand::Offset, kPageInfoUsedMask },
    { 46, Operand::Offset, kStorageDataComponents },
};

// ---------------------------------------------------------------------------
// Command buffers, frame storages, the frame allocator, the immediate cache
// ---------------------------------------------------------------------------

constexpr char kEnsureEntityChange[] =
    "__ZN2ls12PagedHashMapINS_2IDIN3ecs18EntityHandleTraitsEEENS2_15ECBEntityChangeENS2_25ECBHashTablePagedFrameOpsEE15EnsureUniversalIRKS4_JRNS2_14FrameAllocatorEEEERS5_OT_DpOT0_";
constexpr char kHandleGeneratorCreateSymbol[] = "__ZN3ecs21EntityHandleGenerator6CreateEv";
constexpr char kFrameAllocate[] = "__ZN3ecs14FrameAllocator8AllocateEt";
constexpr char kFrameStorageAllocRaw[] = "__ZN3ecs29ComponentFrameStorageAllocRawERNS_21ComponentFrameStorageEiRNS_26ComponentFrameStorageIndexE";

// ecs::EntityCommandBuffer::CreateEntity() makes a handle with the buffer's
// generator, adds the entity's change with the buffer's allocator and sets
// its create flag; DestroyEntity(entity) sets the destroy flag.
constexpr Expect kCommandsCreateEntity[] = {
    { 6, Operand::Calls, 0, kHandleGeneratorCreateSymbol },
    { 8, Operand::Add, kCommandsChanges },
    { 9, Operand::Offset, kCommandsAllocator },
    { 11, Operand::Calls, 0, kEnsureEntityChange },
    { 12, Operand::Offset, kEntityChangeFlags },
    { 13, Operand::Mask, kEntityChangeCreate },
};
constexpr Expect kCommandsDestroyEntity[] = {
    { 9, Operand::Add, kCommandsChanges },
    { 10, Operand::Offset, kCommandsAllocator },
    { 13, Operand::Calls, 0, kEnsureEntityChange },
    { 14, Operand::Offset, kEntityChangeFlags },
    { 15, Operand::Mask, kEntityChangeDestroy },
};
// RemoveComponentUnchecked(type, entity) appends to the entity's change a
// component change with the type in its top 16 bits.
constexpr Expect kCommandsRemoveComponent[] = {
    { 6, Operand::Add, kCommandsChanges },
    { 7, Operand::Offset, kCommandsAllocator },
    { 10, Operand::Calls, 0, kEnsureEntityChange },
    { 14, Operand::Offset, kEntityChangeStore + kFrameStoragePageCount },
    { 16, Operand::Offset, kEntityChangeStore + kFrameStoragePagesSize },
    { 30, Operand::Offset, kEntityChangeStore + kFrameStoragePages },
    { 32, Operand::ShiftLeftBy, kComponentChangeTypeShift },
};
// The map's EnsureUniversal(entity, allocator) builds a new change in its
// values, 0x30 bytes each: the allocator, 4 bits a page, 0xffff last.
constexpr Expect kEnsureEntityChangeCode[] = {
    { 17, Operand::Add, kPagedMapValues },
    { 24, Operand::Offset, kPagedMapValues + kFrameStoragePagesSize },
    { 38, Operand::Offset, kPagedMapValues + kFrameStoragePages },
    { 41, Operand::Move, kEntityChangeSize },
    { 45, Operand::Offset, kEntityChangeStore + kFrameStorageBits },
    { 50, Operand::Offset, kEntityChangeLast },
};
// AccessAddStorage<navcloud::AgentComponent>() finds the type's frame
// storage among the buffer's pools, or adds one with the component's
// size, type and destructor.
constexpr Expect kCommandsAddStorage[] = {
    { 7, Operand::Mask, kComponentTypeIndexMask },
    { 10, Operand::Offset, kCommandsPools + kPoolsMask + kPagedArraySize },
    { 14, Operand::Offset, kCommandsPools + kPoolsMask + kPagedArrayBits },
    { 19, Operand::Offset, kCommandsPools + kPoolsMask },
    { 24, Operand::Offset, kCommandsPools + kPoolsIndices + kPagedArrayBits },
    { 29, Operand::Offset, kCommandsPools + kPoolsIndices },
    { 39, Operand::Offset, kCommandsPools + kPoolsKeys + kPagedArrayBits },
    { 45, Operand::Offset, kCommandsPools + kPoolsKeys },
    { 50, Operand::Offset, kCommandsPools + kPoolsValues + kPagedArrayBits },
    { 52, Operand::Offset, kCommandsPools + kPoolsValues },
    { 58, Operand::Move, kFrameStorageSize },
    { 69, Operand::Offset, kFrameStorageComponentSize },
    { 70, Operand::Offset, kFrameStorageType },
    { 71, Operand::Offset, kFrameStorageDtor },
};
// AddComponentUnchecked<eoc::splatter::ValueChangedOneFrameComponent>
// finds a one-frame type's storage in the pools at its place after the
// 0x880 others (Norbyte's SparseHashMapHash).
constexpr Expect kCommandsAddOneFrame[] = {
    { 9, Operand::Mask, 0x8000 },
    { 11, Operand::Mask, kComponentTypeIndexMask },
    { 12, Operand::Add, kComponentTypeCount },
    { 13, Operand::Offset, kCommandsPools + kPoolsMask + kPagedArraySize },
    { 17, Operand::Offset, kCommandsPools + kPoolsMask + kPagedArrayBits },
    { 22, Operand::Offset, kCommandsPools + kPoolsMask },
    { 27, Operand::Offset, kCommandsPools + kPoolsIndices + kPagedArrayBits },
};
// AddComponentUnchecked<navcloud::AgentComponent>(entity) allocates the
// component in that storage and appends its change to the entity's.
constexpr Expect kCommandsAddComponent[] = {
    { 15, Operand::Calls, 0, kFrameStorageAllocRaw },
    { 19, Operand::Offset, kCommandsAllocator },
    { 21, Operand::Calls, 0, kEnsureEntityChange },
    { 25, Operand::Offset, kEntityChangeStore + kFrameStoragePageCount },
    { 27, Operand::Offset, kEntityChangeStore + kFrameStoragePagesSize },
    { 41, Operand::Offset, kEntityChangeStore + kFrameStoragePages },
};

// ecs::ComponentFrameStorageAllocRaw(storage, size, index): the size's
// bucket (sizes past 64 count as 64), then the components per page and up
// to each page from the game's tables, which must be the ones in
// Layout/GameEntityWorld.h (Norbyte's computed tables are asserted to be
// those, GameDefinitions/EntitySystem.cpp); new pages come from the frame
// allocator.
constexpr Expect kFrameStorageAlloc[] = {
    { 11, Operand::Move, 0x40 },
    { 14, Operand::Offset, kFrameStoragePagesSize },
    { 21, Operand::Offset, kFrameStoragePageCount },
    { 23, Operand::Offset, kFrameStorageBits },
    { 30, Operand::Offset, kFrameStoragePages },
    { 50, Operand::Offset, kFrameStorageCount },
    { 53, Operand::Offset, kFrameStorageComponentSize },
    { 62, Operand::Offset, 0 },
    { 65, Operand::Calls, 0, kFrameAllocate },
};
std::optional<uint64_t> AdrpAddTarget(Code const& f, uint32_t adrp)
{
    auto add = AddImmediate(f.At(adrp + 1));
    if (!add || RegN(f.At(adrp + 1)) != RegD(f.At(adrp))) return {};
    auto page = PageOf(f.VA(adrp), f.At(adrp));
    if (!page) return {};
    return page + *add;
}
bool SameWords(GameImage const& image, uint64_t va, uint32_t const* expected, size_t count)
{
    auto data = static_cast<uint32_t const*>(image.DataAt(va, count * sizeof(uint32_t)));
    return data && memcmp(data, expected, count * sizeof(uint32_t)) == 0;
}
bool SameRows(GameImage const& image, uint64_t table, uint32_t const (&expected)[14][9])
{
    for (size_t row = 0; row < 14; row++) {
        auto va = image.PointerAt(table + row * sizeof(uint64_t));
        if (!va || !SameWords(image, *va, expected[row], 9)) return false;
    }
    return true;
}
bool CheckFrameStorageTables(Code const& f, std::string& problem)
{
    auto buckets = AdrpAddTarget(f, 15);
    auto perPage = AdrpAddTarget(f, 17);
    auto aggregate = AdrpAddTarget(f, 41);
    if (!buckets || !perPage || !aggregate) {
        problem = "the tables of components per page are not where the code was read to take them from";
        return false;
    }
    if (!SameWords(f.Image, *buckets, kFrameSizeBuckets, std::size(kFrameSizeBuckets))) {
        problem = "the size buckets differ from this build's";
        return false;
    }
    if (!SameRows(f.Image, *perPage, kFrameComponentsPerPage) || !SameRows(f.Image, *aggregate, kFrameComponentsAggregate)) {
        problem = "the numbers of components per page differ from this build's";
        return false;
    }
    return true;
}

// ecs::FrameAllocator::Allocate(size) rounds the size up to 64 and takes
// it from the current page; past the page's end it takes the lock and
// makes a page with posix_memalign(64, page size), zeroes its header and
// sets its offset to the header's end, and adds it to the array of pages.
// ~FrameAllocator() releases the pages of both arrays with free, which is
// why a page of Norbyte's must come from posix_memalign too.
constexpr Expect kFrameAllocatorAllocate[] = {
    { 10, Operand::Mask, kFrameAllocationMask },
    { 11, Operand::Add, kFrameAllocatorLock },
    { 12, Operand::Add, kFrameAllocatorPages },
    { 21, Operand::Move, kFramePageHeader },
    { 25, Operand::Offset, kFrameAllocatorPages + kArraySize },
    { 26, Operand::Offset, kFrameAllocatorPages },
    { 31, Operand::Offset, 0 },
    { 41, Operand::Offset, kFrameAllocatorPageSize },
    { 46, Operand::Calls, 0, "__ZN2ls15CriticalSection4LockEv" },
    { 50, Operand::Offset, kFrameAllocatorPageSize },
    { 70, Operand::Move, kFramePageAlignment },
    { 72, Operand::CallsImport, 0, "_posix_memalign" },
};
// The header of the new page (x24) is zeroed with vector stores before
// its offset is written: movi.2d v0, #0 ; stp q0, q0, [x24, #0x20] ;
// stp q0, q0, [x24] ; str x28, [x24].
constexpr uint32_t kZeroHeader[] = { 0x6f00e400, 0xad010300, 0xad000300 };
static_assert(2 * 2 * 16 == kFramePageHeader);    // two pairs of 16-byte registers
bool CheckFramePageHeader(Code const& f, std::string& problem)
{
    for (uint32_t i = 0; i < std::size(kZeroHeader); i++) {
        if (f.At(28 + i) != kZeroHeader[i]) {
            problem = "a new page's header is not zeroed as the code was read to do";
            return false;
        }
    }
    return true;
}
constexpr Expect kFrameAllocatorDestroy[] = {
    { 9, Operand::Offset, kFrameAllocatorPages },
    { 10, Operand::Offset, kFrameAllocatorPages + kArraySize },
    { 15, Operand::Offset, kFrameAllocatorOldPages },
    { 16, Operand::Offset, kFrameAllocatorOldPages + kArraySize },
    { 91, Operand::Offset, 0 },
    { 108, Operand::CallsImport, 0, "_free" },
};

// ecs::legacy::ImmediateWorldCache::GetChange(entity, type) tests the
// type's bit (its low 15 bits) in the mask of the changes written, then
// finds the entity in the type's changes, 0x80 bytes each; then the same
// in the changes being read.
constexpr Expect kCacheGetChange[] = {
    { 1, Operand::Mask, kComponentTypeIndexMask },
    { 8, Operand::Offset, kCacheWrite + kChangesComponents },
    { 9, Operand::Move, kComponentTypeCount },
    { 12, Operand::ShiftLeft, kChangesComponentsShift },
    { 13, Operand::Offset, kComponentChangesMap + 0x28 },
    { 46, Operand::Offset, kCacheRead },
    { 49, Operand::Offset, kCacheRead + kChangesComponents },
    { 88, Operand::Offset, kComponentChangesMap + kComponentChangesValueBits },
    { 93, Operand::Offset, kComponentChangesMap + kComponentChangesValueBits - 8 },
};
// AddComponent<navcloud::PathRequestComponent>(entity) builds the type's
// changes when its bit was clear (the map with 5 bits a page, the frame
// storage with 3, the component's size, type and destructor), and after
// adding the component calls the type's subscribers through the cache's
// callbacks, with the entity and the world.
constexpr Expect kCacheAddComponent[] = {
    { 13, Operand::Mask, kComponentTypeIndexMask },
    { 20, Operand::Offset, kCacheWrite + kChangesComponents },
    { 21, Operand::ShiftLeft, kChangesComponentsShift },
    { 55, Operand::Offset, kCacheAllocator },
    { 63, Operand::Move, kComponentChangesMapBits },
    { 64, Operand::PairOffset, kComponentChangesValueBits - 8 },
    { 65, Operand::PairOffset, kComponentChangesStorage },
    { 66, Operand::Move, kComponentChangesStorageBits },
    { 67, Operand::Offset, kComponentChangesStorage + kFrameStorageBits },
    { 75, Operand::Offset, kComponentChangesStorage + kFrameStorageComponentSize },
    { 76, Operand::Offset, kComponentChangesStorage + kFrameStorageType },
    { 79, Operand::Offset, kComponentChangesStorage + kFrameStorageDtor },
    { 103, Operand::Offset, kCacheCallbacks },
    { 104, Operand::Offset, kCallbacksArray },
    { 106, Operand::Offset, kSignalsAttached + kSignalConnections },
    { 107, Operand::Offset, kSignalsAttached + kSignalConnections + 0xc },
    { 112, Operand::Offset, kCacheWorld },
    { 113, Operand::Move, kSignalConnectionSize },
    { 115, Operand::Add, kSignalFunctionState },
};
// ecs::EntityWorld::EntityWorld(settings) makes the cache: 0x260 bytes,
// and for each of the two sets of changes 0x880 changes of 0x80 bytes,
// 64-byte aligned; then the callbacks, the allocator and the world.
constexpr uint32_t kMovkW2Bits16Of4 = 0x72a00082;  // movk w2, #0x4, lsl #16: 0x40000 + 0x4000 = 0x880 * 0x80
static_assert(0x40000 + 0x4000 == kComponentTypeCount << kChangesComponentsShift);
constexpr Expect kWorldCreateCache[] = {
    { 1398, Operand::Move, kCacheSize },
    { 1399, Operand::CallsImport, 0, "_malloc" },
    { 1441, Operand::Move, kFramePageAlignment },
    { 1442, Operand::Move, 0x4000 },
    { 1444, Operand::CallsImport, 0, "_posix_memalign" },
    { 1455, Operand::Offset, kCacheWrite + kChangesComponents },
    { 1487, Operand::Move, kFramePageAlignment },
    { 1488, Operand::Move, 0x4000 },
    { 1490, Operand::CallsImport, 0, "_posix_memalign" },
    { 1501, Operand::Offset, kCacheRead + kChangesComponents },
    { 1502, Operand::Offset, kCacheCallbacks },
    { 1503, Operand::Offset, kCacheAllocator },
    { 1504, Operand::Offset, kCacheWorld },
    { 1506, Operand::Offset, kWorldCache },
};
bool CheckWorldCreateCache(Code const& f, std::string& problem)
{
    if (f.At(1443) != kMovkW2Bits16Of4 || f.At(1489) != kMovkW2Bits16Of4) {
        problem = "the cache's changes are not allocated as 0x880 changes of 0x80 bytes";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Queries and systems
// ---------------------------------------------------------------------------

// ecs::QueryDescription::Init(counts...) stores the end of each group of
// component types, the list's pointer.
constexpr Expect kQueryInit[] = {
    { 23, Operand::ByteOffset, kQueryEndsLast },
    { 51, Operand::CallsImport, 0, "_malloc" },
    { 99, Operand::Offset, kQueryComponents },
    { 101, Operand::ByteOffset, kQueryEnds },
    { 110, Operand::ByteOffset, kQueryEndsLast - 1 },
};
// QueryRegistry::RegisterQueryDescription<...ToHandleMappingComponent...>
// makes the query in the registry's array (0x58 bytes each) and adds its
// index to the persistent queries.
constexpr Expect kRegisterQuery[] = {
    { 27, Operand::Calls, 0, "__ZN3ecs16QueryDescription4InitEhhhhhhhhhh" },
    { 50, Operand::Mask, kComponentTypeIndexMask },
    { 55, Operand::Offset, kRegistryQueries },
    { 56, Operand::Move, kQuerySize },
    { 61, Operand::Unscaled, kQueryComponents },
    { 86, Operand::Add, kQueryFlags },
    { 95, Operand::Offset, kQueryFlags },
    { 107, Operand::Offset, kRegistryPersistent + kArraySize },
    { 110, Operand::Offset, kRegistryPersistent },
};
// QueryIterator<Persistent, Alive>::MoveToNextStorage walks the query's
// storages (the values of its map, 0x40 bytes each, the storage at +0x30)
// and finds a one-frame type in a storage's one-frame components by its
// place after the 0x880 types, hashed into both halves of a word.
constexpr Expect kQueryNextStorage[] = {
    { 12, Operand::Mask, kComponentTypeHashFactor },
    { 18, Operand::Move, kComponentTypeCount },
    { 40, Operand::Offset, kHashMapValues },
    { 41, Operand::Offset, kHashMapKeysSize },
    { 42, Operand::ShiftLeft, 6 },
    { 50, Operand::Offset, kQueryStorageStorage },
    { 51, Operand::Offset, kStorageDataEntities + kHashMapKeysSize },
    { 54, Operand::Offset, kStorageDataGroupMask },
    { 59, Operand::Offset, kQueryComponents },
    { 67, Operand::Offset, kStorageDataOneFrameComponents + kHashMapBucketCount },
    { 71, Operand::Offset, kStorageDataOneFrameComponents + kHashMapValues },
    { 72, Operand::ShiftLeft, 6 },
    { 88, Operand::Offset, kStorageDataOneFrameComponents + kHashMapBuckets },
    { 91, Operand::Offset, kStorageDataOneFrameComponents + kHashMapKeys },
};
static_assert(kQueryStorageSize == 1 << 6 && kHashMapSize == 1 << 6);
// EntityStorageIterator<0>::FetchPage() takes the entity page and, for
// each of the query's component slots, the slot of the storage's component
// page (16 bytes each): the slots inline unless there are more than 8.
constexpr Expect kStorageFetchPage[] = {
    { 2, Operand::Offset, kStorageDataComponents + kArraySize },
    { 6, Operand::Offset, kStorageDataEntities + kHashMapKeysSize },
    { 18, Operand::Offset, kStorageDataHandles },
    { 22, Operand::Offset, kStorageDataComponents },
    { 31, Operand::ShiftLeft, kComponentPageSlotShift },
    { 40, Operand::ByteOffset, kQueryStorageSlotCount },
    { 41, Operand::Add, kQueryStorageSlots },
    { 42, Operand::Add, kQueryStorageInlineSlots },
};
// SystemUpdate<ecl::splatter::StateSystem>(system, world, time) reads the
// world's storages, queries (0x58 bytes each) and cache, and gives the
// world's first word to the synced data of a component it writes, as the
// sync buffers the data marks dirty (below).
constexpr char kSyncedDataCreate[] =
    "__ZN3ecs4sync10SyncedDataIN3eoc8splatter14StateComponentEEC2ERKNS_25ComponentModificationViewIJS4_EEEN2ls2IDINS_18EntityHandleTraitsEEEPNS0_11SyncBuffersE";
// It keeps the world at sp+0x48 (that is what it gives GetComponent), and
// adds a component through the command buffer of its thread: the buffers'
// array, 0xc0 bytes each, by the thread's index.
constexpr Expect kSplatterUpdate[] = {
    { 14, Operand::Offset, kWorldStorage },
    { 17, Operand::Offset, kWorldQueries + kRegistryQueries },
    { 18, Operand::Move, kQuerySize },
    { 20, Operand::Offset, kWorldCache },
    { 51, Operand::Offset, 0x48 },
    { 174, Operand::Move, kCommandsSize },
    { 205, Operand::Offset, 0x48 },
    { 206, Operand::Calls, 0,
        "__ZN3ecs11EntityWorld12GetComponentIKN3eoc8splatter14StateComponentELb1EEEPT_N2ls2IDINS_18EntityHandleTraitsEEENSt3__117integral_constantIbLb0EEE" },
    { 208, Operand::Calls, 0, "__ZN2ls14ThreadRegistry18RequestThreadIndexEv" },
    { 209, Operand::Offset, 0x48 },
    { 210, Operand::Offset, kWorldCommandBuffers },
    { 222, Operand::Offset, 0x48 },
    { 223, Operand::Offset, kWorldQueries + kRegistryQueries },
    { 257, Operand::Offset, 0x188 },
    { 273, Operand::Offset, 0x188 },
    { 322, Operand::Offset, kWorldReplication },
    { 325, Operand::Calls, 0, kSyncedDataCreate },
};
// A query's index goes to the registry's array for its kind: added queries
// (Norbyte's AliveQueries), removed ones, dead ones. ecs::EntityWorld::
// FlushECBs() empties the frame data array, the last of the registry's,
// then looks at the storages that changed.
constexpr Expect kRegisterAddedQuery[] = {
    { 126, Operand::Offset, kRegistryAdded + kArraySize },
    { 129, Operand::Offset, kRegistryAdded },
};
constexpr Expect kRegisterRemovedQuery[] = {
    { 117, Operand::Offset, kRegistryRemoved + kArraySize },
    { 120, Operand::Offset, kRegistryRemoved },
};
constexpr Expect kRegisterDeadQuery[] = {
    { 97, Operand::Offset, kRegistryDead + kArraySize },
    { 100, Operand::Offset, kRegistryDead },
};
constexpr Expect kFlushCommands[] = {
    { 9, Operand::Offset, kWorldQueries + kRegistryFrameData },
    { 10, Operand::Offset, kWorldQueries + kRegistryFrameData + kArraySize },
    { 16, Operand::Offset, kWorldStorage },
    { 18, Operand::Offset, kStorageUsedFrameData + kBitSetCapacity },
};
// ecs::core::SystemDependencyExecutor::ExecuteWTKernel() calls a system's
// update function, if the system is activated, with the system, the world
// and the world's time.
constexpr Expect kExecuteSystem[] = {
    { 13, Operand::Offset, kSystemEntryUpdate },
    { 15, Operand::ByteOffset, kSystemEntryActivated },
    { 18, Operand::Offset, 0x28 },
    { 19, Operand::Add, kWorldTime },
};
// ecs::EntityWorld::ActivateSystemInternal(entry) reads the entry's
// activated byte and the world's active one, and keeps the active systems
// sorted.
constexpr Expect kActivateSystem[] = {
    { 6, Operand::ByteOffset, kSystemEntryActivated },
    { 12, Operand::ByteOffset, kWorldActive },
    { 23, Operand::Offset, kWorldActiveSystems + kArraySize },
    { 33, Operand::Offset, kWorldActiveSystems },
    { 58, Operand::Offset, kWorldActiveSystems + kArrayCapacity },
};
// ecs::EntityWorld::EndRegisterPhase() walks the systems, 0xf8 bytes each.
constexpr Expect kEndRegisterPhase[] = {
    { 876, Operand::Offset, kWorldSystems + kArraySize },
    { 931, Operand::Unscaled, kWorldSystems },
    { 935, Operand::Move, kSystemEntrySize },
    { 957, Operand::Add, kSystemEntrySize },
};

// ---------------------------------------------------------------------------
// Replication
// ---------------------------------------------------------------------------

// ecs::sync::EntitiesDirtyFieldsBuffer::MarkDirty(entity, type, fields)
// sets the dirty byte and adds the fields to the type's map (0x40 bytes
// each), whose values are bit sets.
constexpr Expect kMarkDirty[] = {
    { 12, Operand::ByteOffset, kDirtyFieldsDirty },
    { 13, Operand::Offset, kDirtyFieldsPools },
    { 15, Operand::ShiftLeft, 6 },
    { 16, Operand::Offset, kHashMapKeysSize },
    { 21, Operand::Unscaled, kHashMapValues },
};
// SyncedData<eoc::splatter::StateComponent> keeps the sync buffers it is
// made with and, when destroyed, marks the component's fields dirty in them.
constexpr Expect kSyncedDataCreateCode[] = {
    { 5, Operand::PairOffset, 0x20 },
};
constexpr Expect kSyncedDataDestroy[] = {
    { 4, Operand::Offset, 0x20 },
    { 32, Operand::Calls, 0,
        "__ZN3ecs4sync25EntitiesDirtyFieldsBuffer9MarkDirtyEN2ls2IDINS_18EntityHandleTraitsEEEiONS2_13DynamicBitSetINS2_15TaggedAllocatorIiEEEE" },
};
// EntityReplicationPeer::GetEntity(id) and EntityReplicationAuthority::
// GetEntity(id) look the id up in their maps from a network id.
constexpr Expect kPeerGetEntity[] = {
    { 0, Operand::Offset, kPeerNetIdToEntity + kHashMapBucketCount },
    { 4, Operand::Offset, kPeerNetIdToEntity + kHashMapBuckets },
    { 8, Operand::Offset, kPeerNetIdToEntity + kHashMapKeys },
    { 12, Operand::Offset, kPeerNetIdToEntity + kHashMapNext },
    { 17, Operand::Offset, kPeerNetIdToEntity + kHashMapValues },
};
constexpr Expect kAuthorityGetEntity[] = {
    { 0, Operand::Offset, kAuthorityNetIdToEntity + kHashMapBucketCount },
    { 4, Operand::Offset, kAuthorityNetIdToEntity + kHashMapBuckets },
    { 8, Operand::Offset, kAuthorityNetIdToEntity + kHashMapKeys },
    { 12, Operand::Offset, kAuthorityNetIdToEntity + kHashMapNext },
    { 17, Operand::Offset, kAuthorityNetIdToEntity + kHashMapValues },
};
// ~GameClient() destroys the peer inside it: the deserializers, then the
// map from an entity to its network id and the one back.
constexpr Expect kClientDestroy[] = {
    { 412, Operand::Add, kClientPeer + kPeerDeserializers },
    { 413, Operand::Calls, 0, "__ZN2ls11SparseArrayINS_9UniquePtrIN3ecs4sync22IComponentDeserializerEiEENS_15TaggedAllocatorIiEEED2Ev" },
    { 435, Operand::Add, kClientPeer + kPeerEntityToNetId },
    { 458, Operand::Add, kClientPeer + kPeerNetIdToEntity },
    { 459, Operand::Calls, 0, "__ZN2ls9HashTableINS_2IDIN3net11NetIDTraitsEEEN3ecs4sync15RPLHashTableOpsEED2Ev" },
};
// ComponentDeserializer<eoc::splatter::StateComponent>::DispatchChanges()
// has each field tracker in its sparse array fire its changes through
// slot 4; FieldTracker<...m_SweatAmount>::Dispatch() fires each entity's
// change (0x18 bytes) to the connections of its signal (0x48 bytes each).
constexpr Expect kDeserializerDispatch[] = {
    { 6, Operand::Add, kDeserializerTrackers },
    { 7, Operand::Offset, kDeserializerTrackers + kBitSetCapacity },
    { 8, Operand::Add, kBitSetInlineBits + 1 },
    { 47, Operand::Offset, kDeserializerTrackers + kSparseArrayValues },
    { 50, Operand::Offset, kFieldTrackerFireSlot * sizeof(void*) },
};
constexpr Expect kFieldTrackerDispatch[] = {
    { 6, Operand::Offset, kFieldTrackerChanges + kHashMapKeysSize },
    { 11, Operand::Offset, kFieldTrackerChanges + kHashMapValues },
    { 12, Operand::Move, kSignalConnectionSize },
    { 15, Operand::Add, kFieldChangeSize },
    { 18, Operand::Offset, kFieldTrackerChangeEvent + kSignalConnections },
    { 19, Operand::Offset, kFieldTrackerChangeEvent + kSignalConnections + 0xc },
};
// esv::GameServer::PostUpdate(time) syncs the authority inside the server;
// ecl::CharacterTask_PickUp::Enter() asks the peer inside the client's
// network object for an entity.
constexpr Expect kServerSync[] = {
    { 8, Operand::Add, kServerAuthority },
    { 9, Operand::Calls, 0, "__ZN3ecs4sync26EntityReplicationAuthority4SyncERKN2ls8GameTimeE" },
};
constexpr Expect kClientPeerGetEntity[] = {
    { 277, Operand::Offset, kEocClientGameClient },
    { 278, Operand::Add, kClientPeer },
    { 280, Operand::Calls, 0, "__ZNK3ecs4sync21EntityReplicationPeer9GetEntityEN2ls2IDIN3net11NetIDTraitsEEE" },
};

// ---------------------------------------------------------------------------
// ls::EffectsManager
// ---------------------------------------------------------------------------

constexpr char kSemaphoreDestroy[] = "_semaphore_destroy";
constexpr char kEntityHashTableDestroy[] = "__ZN2ls9HashTableINS_2IDIN3ecs18EntityHandleTraitsEEENS_19HashTableOpsDefaultEED2Ev";

// ls::RegisterClientSystems(ecs::EntityWorld&) builds the effects manager in
// place: 0x1e0 bytes; the "can update" byte set at +0x30; then the creating
// thread's Mach port, widened to 64 bits, stored at +0x38 together with a
// zero for the pointer of the array after it; the queue of invokes
// constructed at +0x50.
//     mov w0, #0x1e0 ; bl malloc ; ...
//     strb w8, [x25, #0x30] ; bl pthread_self ; bl pthread_mach_thread_np
//     mov w8, w0 ; stp x8, xzr, [x25, #0x38] ; ... ; add x23, x25, #0x50 ; mov x0, x23 ; bl ThreadSafeQueue(unsigned)
constexpr Expect kEffectsManagerCreate[] = {
    { 5226, Operand::Move, kEffectsManagerSize },
    { 5227, Operand::CallsImport, 0, "_malloc" },
    { 5247, Operand::ByteOffset, kEffectsCanUpdate },
    { 5248, Operand::CallsImport, 0, "_pthread_self" },
    { 5249, Operand::CallsImport, 0, "_pthread_mach_thread_np" },
    { 5251, Operand::PairOffset, kEffectsCreateThread },
    { 5289, Operand::Add, kEffectsInvokes },
    { 5291, Operand::Calls, 0, "__ZN2ls15ThreadSafeQueueINS_12EffectInvokeENS_12NewInterfaceIS1_EEEC2Ej" },
};

// The pair at +0x38 is two 64-bit registers (x8 and the zero register), so
// the thread's id takes the 8 bytes at +0x38 and the array begins at +0x40.
bool CheckEffectsManagerCreate(Code const& f, std::string& problem)
{
    uint32_t widen = f.At(5250), pair = f.At(5251);
    if (widen != 0x2a0003e8 || (pair >> 31) != 1 || RegD(pair) != 8 || ((pair >> 10) & 31) != 31) {
        problem = "the thread's id is not stored as 64 bits ahead of a zero";
        return false;
    }
    return true;
}

// ~EffectsManager() takes the members apart from the last to the first, each
// by its own destructor or as its kind requires: three vertex buffer
// providers by pointer, the set of effects not yet constructed, the critical
// section before it (its semaphore destroyed), the overlay requests and
// their critical section, the command buffers and theirs, the sound objects,
// the invokes' critical section and queue, the array of effects to destroy,
// and the connections of the signal.
constexpr Expect kEffectsManagerDestroy[] = {
    { 147, Operand::Offset, kEffectsVertexBuffers },
    { 195, Operand::Offset, kEffectsVertexBuffers + 8 },
    { 242, Operand::Offset, kEffectsVertexBuffers + 16 },
    { 291, Operand::Offset, kEffectsCommandBuffer },
    { 370, Operand::Add, kEffectsNonConstructed },
    { 371, Operand::Calls, 0, kEntityHashTableDestroy },
    { 375, Operand::Offset, kEffectsNonConstructedLock + offsetof(GameCriticalSection, Semaphore) },
    { 379, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 381, Operand::Add, kEffectsOverlayRequests },
    { 412, Operand::Offset, kEffectsOverlayRequestsLock + offsetof(GameCriticalSection, Semaphore) },
    { 414, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 422, Operand::Offset, kEffectsCommandBuffers },
    { 499, Operand::Offset, kEffectsCommandBufferLock + offsetof(GameCriticalSection, Semaphore) },
    { 501, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 572, Operand::Add, kEffectsSoundObjects },
    { 577, Operand::Calls, 0, kEntityHashTableDestroy },
    { 581, Operand::Offset, kEffectsInvokesLock + offsetof(GameCriticalSection, Semaphore) },
    { 583, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 584, Operand::Add, kEffectsInvokes },
    { 586, Operand::Calls, 0, "__ZN2ls5QueueINS_12EffectInvokeENS_12NewInterfaceIS1_EEED2Ev" },
    { 593, Operand::Offset, kEffectsDestroyData },
    { 667, Operand::Add, kEffectsSignalConnections },
};

// ---------------------------------------------------------------------------
// ls::ResourceManager
// ---------------------------------------------------------------------------

// GameEngineSDM::Init builds the resource manager in place: 0x140 bytes; the
// first 16 zeroed (a free lock); the map of preview resources after them
// (its bucket count at +0x10, its buckets at +0x18, its item count at +0x20).
//     mov w0, #0x140 ; bl malloc ; ... ; stp xzr, xzr, [x21] ; mov w8, #37 ; mov x27, x21
//     str w8, [x27, #0x10]! ; str wzr, [x21, #0x20] ; ... ; str x20, [x21, #0x18]
constexpr Expect kResourceManagerCreate[] = {
    { 1813, Operand::Move, kResourceManagerSize },
    { 1814, Operand::CallsImport, 0, "_malloc" },
    { 1825, Operand::PairOffset, kResourceManagerLock },
    { 1825, Operand::StoresZero },
    { 1828, Operand::Unscaled, kResourceManagerPreviewResources },
    { 1829, Operand::Offset, kResourceManagerPreviewResources + 0x10 },
    { 1862, Operand::Offset, kResourceManagerPreviewResources + 8 },
};

// GameEngineSDM::Shutdown takes it apart, from the last member to the first
// (x19 is the manager, loaded from ls::ResourceManager::m_ptr): the map of
// genome managers, the visual loaders, the sources, the dependencies, the
// critical section (its semaphore destroyed), the effect dependencies, the
// preview resources, and last the lock at +0, whose two semaphores (the
// readers' at +0, the writers' at +4) are what make it an ls::SRWKernelLock.
constexpr Expect kResourceManagerShutdown[] = {
    { 3170, Operand::Calls, 0, "__ZN2ls15ResourceManager7DestroyEv" },
    { 3177, Operand::Add, kResourceManagerGenomeManagers },
    { 3178, Operand::Calls, 0, "__ZN2ls14FixedStringMapIPN2gn13GenomeManagerEED2Ev" },
    { 3179, Operand::Offset, kResourceManagerVisualLoaders },
    { 3244, Operand::Add, kResourceManagerSources },
    { 3245, Operand::Calls, 0, "__ZN2ls14FixedStringMapIPNS_4PathEED2Ev" },
    { 3246, Operand::Add, kResourceManagerDependencies },
    { 3251, Operand::Offset, kResourceManagerCriticalSection + offsetof(GameCriticalSection, Semaphore) },
    { 3253, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 3255, Operand::Offset, kResourceManagerEffectDependencies },
    { 3298, Operand::Add, kResourceManagerPreviewResources },
    { 3299, Operand::Calls, 0, "__ZN2ls14FixedStringMapIPNS_8ResourceEED2Ev" },
    { 3301, Operand::Offset, kResourceManagerLock + offsetof(GameSRWKernelLock, SharedSemaphore) },
    { 3302, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 3304, Operand::Offset, kResourceManagerLock + offsetof(GameSRWKernelLock, ExclusiveSemaphore) },
    { 3305, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 3327, Operand::CallsImport, 0, "_free" },
};

// ls::ResourceManager::Destroy() releases the two resource banks and then
// reaches the sound manager.
constexpr Expect kResourceManagerDestroy[] = {
    { 121, Operand::Offset, kResourceManagerBanks },
    { 126, Operand::Offset, kResourceManagerBanks },
    { 129, Operand::Offset, kResourceManagerBanks + 8 },
    { 134, Operand::Offset, kResourceManagerBanks + 8 },
    { 138, Operand::Offset, kResourceManagerSoundManager },
};

// ls::ResourceManager::RemoveTexture(rf::Texture*) takes the texture manager.
constexpr Expect kResourceManagerRemoveTexture[] = {
    { 5, Operand::Offset, kResourceManagerTextureManager },
};

// ---------------------------------------------------------------------------
// ls::gst: the table of fixed strings
// ---------------------------------------------------------------------------

// None of the functions below is one the extender hooks: the proof is made
// again in the game after the hooks are on. What CoreLibSDM::Init shows of
// the table (it builds it, and is hooked) is in the engine table, which is
// resolved before any hook: Engine/Entries/CoreLibrary.cpp.

constexpr char kStringTableSymbol[] = "__ZN2ls3gst10s_InstanceE";
constexpr char kStringReleaseSymbol[] = "__ZN2ls3gst3Map7ReleaseERKNS0_12NodePoolDataE";

// NodePool(int, PoolSpec const&) fills the pool member by member: the 64
// epoch slots, a line apart, with -1; its index; the entry size (the
// string's room rounded down to 8, plus 0x20) and how many entries of that
// size a bucket of 0x10000 bytes holds; the bucket count; the array of bucket
// pointers, 8 bytes a bucket; the free list; and the lock, a free critical
// section whose first 16 bytes are one store and whose spin count is the next.
//     str x8, [x0] ; str x8, [x0, #0x40] ; ... ; str x8, [x0, #0xfc0]
//     str w1, [x0, #0x1080] ; mov w10, #0x10000 ; str xzr, [x0, #0x1000] ; str wzr, [x0, #0x1040]
//     str x8, [x0, #0x1088] ; udiv x8, x10, x8 ; str w8, [x0, #0x1090] ; ... ; str w8, [x19, #0x10c0] ; str wzr, [x19, #0x1100]
//     sbfiz x0, x22, #3, #32 ; bl malloc ; ... ; str x0, [x19, #0x1140] ; ... ; str x25, [x19, #0x1180]
//     str q0, [x19, #0x11c0] ; mov w8, #4000 ; str w8, [x19, #0x11d0] ; ... ; mov w0, #0x10000 ; bl malloc
constexpr Expect kStringPoolCreate[] = {
    { 10, Operand::Offset, kStringPoolEpochSlots },
    { 11, Operand::Offset, kStringPoolEpochSlots + kStringLine },
    { 75, Operand::Offset, kStringPoolEpoch - kStringLine },
    { 86, Operand::Offset, kStringPoolIndex },
    { 87, Operand::Move, kStringBucketBytes },
    { 88, Operand::Offset, kStringPoolEpoch },
    { 88, Operand::StoresZero },
    { 89, Operand::Offset, kStringPoolEvent },
    { 90, Operand::Offset, kStringPoolEntrySize },
    { 92, Operand::Offset, kStringPoolEntriesPerBucket },
    { 100, Operand::Offset, kStringPoolBucketCount },
    { 101, Operand::Offset, kStringPoolUsed },
    { 101, Operand::StoresZero },
    { 126, Operand::CallsImport, 0, "_malloc" },
    { 137, Operand::Offset, kStringPoolBuckets },
    { 139, Operand::Offset, kStringPoolFreeList },
    { 142, Operand::Offset, kStringPoolLock },
    { 143, Operand::Move, (uint32_t)kFreeCriticalSection.SpinCount },
    { 144, Operand::Offset, kStringPoolLock + kSpinCount },
    { 177, Operand::Move, kStringBucketBytes },
    { 178, Operand::CallsImport, 0, "_malloc" },
};

// The 16 bytes NodePool's one store writes at the lock are a free critical
// section's, before its spin count.
bool CheckStringPoolLock(Code const& f, std::string& problem)
{
    // adrp x20, constant@PAGE ; ... ; ldr q0, [x20, constant@PAGEOFF]
    uint32_t adrp = f.At(105), load = f.At(140);
    uint64_t page = PageOf(f.VA(105), adrp);
    auto offset = LoadStoreOffset(load);
    auto bytes = page && offset && RegN(load) == RegD(adrp) ? f.Image.DataAt(page + *offset, kSpinCount) : nullptr;
    if (!bytes || memcmp(bytes, &kFreeCriticalSection, kSpinCount) != 0) {
        problem = "a new pool's lock does not start as the Mac type's free critical section";
        return false;
    }
    return true;
}

// Whether the function loads the table's address from ls::gst::s_Instance at
// instruction `adrp`: adrp x8, s_Instance@PAGE ; ldr x8, [x8, s_Instance@PAGEOFF]
bool LoadsStringTable(Code const& f, uint32_t adrp, std::string& problem)
{
    uint64_t table = f.Image.FindVA(kStringTableSymbol);
    if (!table || LoadedAddress(f, adrp) != table) {
        problem = "the table it reads is not the one ls::gst::s_Instance points to";
        return false;
    }
    return true;
}

// Get(unsigned int): the pool is the table plus (index & 0xf) pools, so the
// pools start where the table starts; the bucket comes from the pool's array
// of pointers, the entry is (index >> 20) entry sizes into it, and the result
// is the text after the entry's header with the length from the header.
//     adrp x8, s_Instance ; ldr x8, [x8] ; and w9, w0, #0xf ; mov w10, #0x1200 ; umaddl x20, w9, w10, x8
//     add x23, x20, #0x1000 ; ... ; mov w8, #0x1140 ; add x8, x20, x8 ; ldapr x8, [x8]
//     ubfx w9, w19, #4, #16 ; ldr x21, [x8, w9, uxtw #3] ; ldr x22, [x20, #0x1088]
//     ... ; mov w8, #0x1040 ; add x0, x20, x8 ; bl ls::ThreadEvent::OpenDoor
//     lsr w8, w19, #20 ; madd x8, x22, x8, x21 ; add x0, x8, #0x18 ; ldr w1, [x8, #0x8]
constexpr Expect kStringGet[] = {
    { 8, Operand::Mask, kStringIndexPoolMask },
    { 9, Operand::Move, kStringPoolSize },
    { 14, Operand::Add, kStringPoolEpoch },
    { 17, Operand::Move, kStringPoolBuckets },
    { 22, Operand::Offset, kStringPoolEntrySize },
    { 31, Operand::Move, kStringPoolEvent },
    { 34, Operand::ShiftRight, kStringIndexEntryShift },
    { 36, Operand::Add, kStringEntryText },
    { 37, Operand::Offset, kStringEntryLength },
};
bool CheckStringGet(Code const& f, std::string& problem) { return LoadsStringTable(f, 6, problem); }

// Acquire(unsigned int) takes the pools from the map's pointer to them, which
// it reads at the map's place in the table, and adds a reference to the entry.
//     adrp x8, s_Instance ; ldr x8, [x8] ; mov w9, #0xd680 ; ldr x8, [x8, x9] ; ... ; mov w10, #0x1200
//     ... ; add x8, x8, #0x4 ; ldapr w10, [x8]
constexpr Expect kStringAcquire[] = {
    { 8, Operand::Move, kStringTableMain + kStringMapStorage },
    { 11, Operand::Move, kStringPoolSize },
    { 38, Operand::Add, kStringEntryReferences },
};
bool CheckStringAcquire(Code const& f, std::string& problem) { return LoadsStringTable(f, 6, problem); }

// Map::Release(NodePoolData const&): `this` is the map. It takes the pools
// from its pointer to them, drops a reference of the entry, and when that was
// the last, unlinks the entry from the chain of its slot, which is the
// entry's hash modulo the slot count, and gives it back to its pool.
//     ldr x8, [x0, #0x1080] ; ... ; mov w10, #0x1200 ; ...
//     ldr w9, [x21, #0xc] ; ... ; add x8, x21, #0x4 ; mov w9, #-1 ; ldaddal w9, w8, [x8] ; cmp w8, #1 ; b.ne
//     ldr w8, [x21] ; mov w9, #0x8001 ; movk w9, #0x8000, lsl #16 ; mul x9, x8, x9 ; lsr x9, x9, #47
//     sub w9, w9, w9, lsl #16 ; add w8, w8, w9 ; add x8, x19, w8, uxtw #3 ; mov w9, #0x10c0 ; add x28, x8, x9
//     mov w8, #0x1040 ; ... ; add x9, x19, #0x1000 ; ... ; add x28, x8, #0x10
//     ... ; ldr x0, [x19, #0x1080] ; add x1, sp, #0x18 ; bl ls::gst::NodeStorage::Remove
constexpr uint32_t kStringSlotDivisionShift = 47;
static_assert(kStringMapSlotCount == (1u << 16) - 1);
constexpr Expect kStringRelease[] = {
    { 11, Operand::Offset, kStringMapStorage },
    { 14, Operand::Move, kStringPoolSize },
    { 43, Operand::Offset, kStringEntryId },
    { 47, Operand::Add, kStringEntryReferences },
    { 52, Operand::Offset, kStringEntryHash },
    { 56, Operand::ShiftRight, kStringSlotDivisionShift },
    { 57, Operand::ShiftLeft, 16 },
    { 60, Operand::Move, kStringMapSlots },
    { 62, Operand::Move, kStringMapEvent },
    { 71, Operand::Add, kStringMapEpoch },
    { 109, Operand::Add, kStringEntryNext },
    { 171, Operand::Offset, kStringMapStorage },
    { 173, Operand::Calls, 0, "__ZN2ls3gst11NodeStorage6RemoveERKNS0_9NodeIndexE" },
};
// The hash is divided by the slot count with a multiplication: the factor is
// 2^47 / count, rounded up.
bool CheckStringRelease(Code const& f, std::string& problem)
{
    auto factor = MovzMovkValue(f.At(53), f.At(54));
    if (!factor || *factor != ((1ull << kStringSlotDivisionShift) + kStringMapSlotCount - 1) / kStringMapSlotCount) {
        problem = "the map's slot is not the hash modulo " + Hex(kStringMapSlotCount);
        return false;
    }
    return true;
}

// ls::FixedString::~FixedString() releases a string that is not the null
// one through the map, which is at its place in the table.
//     ldr w8, [x0] ; cmn w8, #1 ; b.eq ; str w8, [sp, #8] ; adrp x8, s_Instance ; ldr x8, [x8]
//     mov w9, #0xc600 ; add x0, x8, x9 ; add x1, sp, #8 ; bl ls::gst::Map::Release
constexpr Expect kFixedStringDestroy[] = {
    { 5, Operand::Offset, 0 },
    { 6, Operand::Add, (uint32_t)(0 - kNullStringIndex) },
    { 11, Operand::Move, kStringTableMain },
    { 14, Operand::Calls, 0, kStringReleaseSymbol },
};
bool CheckFixedStringDestroy(Code const& f, std::string& problem) { return LoadsStringTable(f, 9, problem); }

// ---------------------------------------------------------------------------
// gui::GameUI
// ---------------------------------------------------------------------------

// GameUI() stores the vtable pointers of the object and of its four listener
// bases into its first five words, one after another, and then clears the
// signal that follows them; further on it sets the command processor's two
// flag bytes and clears the first two words of its queue.
//     stp x8, x9, [x20] ; ... ; stp x9, x10, [x20, #0x10] ; ... ; str x8, [x20, #0x20] ; ... ; stur q2, [x20, #0x28]
//     strh w8, [x20, #0x300] ; ... ; str xzr, [x20, #0x388] ; str xzr, [x20, #0x380]
constexpr Expect kGameUiCreate[] = {
    { 56, Operand::PairOffset, 0 },
    { 61, Operand::PairOffset, 0x10 },
    { 63, Operand::Offset, (kGameUiVtables - 1) * 8 },
    { 65, Operand::Unscaled, kGameUiSignal },
    { 169, Operand::Offset, kGameUiCommandProcessor },
    { 173, Operand::Offset, kGameUiCommandQueue + 8 },
    { 174, Operand::Offset, kGameUiCommandQueue },
};

// ~GameUI() destroys the four critical sections at the end, the tutorial and
// Twitch managers, the view model provider, the state machine, the window
// manager, the brush manager's critical section and the Noesis manager, each
// at its place.
constexpr Expect kGameUiDestroy[] = {
    { 506, Operand::Offset, kGameUiRenderOffscreenLock + offsetof(GameCriticalSection, Semaphore) },
    { 510, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 514, Operand::Offset, kGameUiRenderLock + offsetof(GameCriticalSection, Semaphore) },
    { 516, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 521, Operand::Offset, kGameUiUpdateLock + offsetof(GameCriticalSection, Semaphore) },
    { 523, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 527, Operand::Offset, kGameUiEventsLock + offsetof(GameCriticalSection, Semaphore) },
    { 529, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 1343, Operand::Move, kGameUiTutorialManager },
    { 1345, Operand::Calls, 0, "__ZN3gui15TutorialManagerD2Ev" },
    { 1346, Operand::Move, kGameUiTwitchManager },
    { 1348, Operand::Calls, 0, "__ZN3gui20TwitchOverlayManagerD1Ev" },
    { 1421, Operand::Add, kGameUiViewModelProvider },
    { 1422, Operand::Calls, 0, "__ZN3gui17ViewModelProviderD2Ev" },
    { 1423, Operand::Add, kGameUiStateMachine },
    { 1424, Operand::Calls, 0, "__ZN2ui12StateMachineD2Ev" },
    { 1425, Operand::Add, kGameUiWindowManager },
    { 1426, Operand::Calls, 0, "__ZN2ui13WindowManagerD2Ev" },
    { 1441, Operand::Offset, kGameUiCommandQueue + 8 },
    { 1491, Operand::Offset, kGameUiBrushManager + kGameUiBrushManagerLock + offsetof(GameCriticalSection, Semaphore) },
    { 1493, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 1500, Operand::Add, kGameUiNoesisManager },
    { 1501, Operand::Calls, 0, "__ZN2ui15NoesisUIManagerD2Ev" },
};

// ~TwitchOverlayManager() releases the request manager it holds by pointer,
// then destroys the critical section before it.
constexpr Expect kTwitchManagerDestroy[] = {
    { 33, Operand::Offset, kTwitchManagerRequests },
    { 38, Operand::Offset, kTwitchManagerRequests },
    { 38, Operand::StoresZero },
    { 42, Operand::Offset, kTwitchManagerLock + offsetof(GameCriticalSection, Semaphore) },
    { 46, Operand::CallsImport, 0, kSemaphoreDestroy },
};

// ---------------------------------------------------------------------------
// Objects without a member of the Windows ones
// ---------------------------------------------------------------------------

// gn::GenomeManager's constructor ends with the unload batch and its job,
// then clears the two times together and the flag after them: there is no
// critical section between the job and the times.
//     str x0, [x19, #0x500] ; ... ; str x23, [x19, #0x508] ; movi v0, #0 ; str q0, [x19, #0x510] ; strb wzr, [x19, #0x520]
constexpr Expect kGenomeManagerCreate[] = {
    { 287, Operand::Offset, kGenomeUnloadBatch },
    { 325, Operand::Offset, kGenomeUnloadBatchJob },
    { 327, Operand::Offset, kGenomeSyncedTime },
    { 328, Operand::ByteOffset, kGenomeIsMasterTime },
    { 328, Operand::StoresZero },
};

// ~LevelInstanceLoadSystem() destroys the critical section and then frees
// the queue's buffer.
constexpr Expect kLevelLoadSystemDestroy[] = {
    { 10, Operand::Offset, kLevelLoadSystemLock + offsetof(GameCriticalSection, Semaphore) },
    { 14, Operand::CallsImport, 0, kSemaphoreDestroy },
    { 33, Operand::Offset, kLevelLoadSystemRequests },
};

// ecl::RegisterSystems builds that system in 0x48 bytes, the level manager
// right after the system's two words.
constexpr Expect kLevelLoadSystemCreate[] = {
    { 1208, Operand::Move, kLevelLoadSystemSize },
    { 1209, Operand::CallsImport, 0, "_malloc" },
    { 1223, Operand::PairOffset, kLevelLoadSystemManager },
};

// esv::RegisterSystems builds the progression system in 0x90 bytes: zeroes
// from +0x10 to +0x80, then its two pointers, which end the object.
constexpr Expect kProgressionSystemCreate[] = {
    { 16641, Operand::Move, kProgressionSystemSize },
    { 16642, Operand::CallsImport, 0, "_malloc" },
    { 16660, Operand::PairOffset, kProgressionSystemPointers },
};

// eoc::SharedDialogGameInterface::OnManagerStart(manager) keeps the manager.
//     str x1, [x0, #0x1b8] ; ret
constexpr Expect kDialogManagerStart[] = {
    { 0, Operand::Offset, kDialogInterfaceManager },
};

// Its constructor clears the first queue from +0x08 and starts the second at
// +0xd8 (26 words each), then the array of requests and the manager after it.
//     mov x24, x0 ; stp x8, x8, [x24, #0xd8]! ; ... ; stp xzr, xzr, [x24, #0xd8] ; str xzr, [x24, #0xd0]
constexpr Expect kDialogInterfaceCreate[] = {
    { 30, Operand::PairOffset, kDialogInterfaceSecondQueue },
    { 39, Operand::PairOffset, kDialogInterfaceManager - kDialogInterfaceSecondQueue - 8 },
    { 39, Operand::StoresZero },
    { 40, Operand::Offset, kDialogInterfaceRequests - kDialogInterfaceSecondQueue },
    { 40, Operand::StoresZero },
};

// ~esv::DialogSystem() destroys the interface at +0x30, after the system's
// word, its world and four pointers.
//     mov x19, x0 ; ... ; add x0, x19, #0x30 ; bl esv::EoCDialogGameInterface::~EoCDialogGameInterface()
constexpr Expect kDialogSystemDestroy[] = {
    { 146, Operand::Add, kDialogSystemInterface },
    { 147, Operand::Calls, 0, "__ZN3esv22EoCDialogGameInterfaceD2Ev" },
};

// The dialog manager's constructor clears the two arrays from +0xf8, makes
// the lock at +0x118 free (its spin count 0x10 into it) and keeps the mod
// manager, its first argument, at +0x130.
//     mov x25, x1 ; ... ; add x24, x19, #0x118 ; ... ; str q0, [x10, #0xf8]! ; str q0, [x10, #0x10] ; ...
//     str q1, [x24] ; mov w9, #0xfa0 ; str w9, [x10, #0x30] ; ... ; str x25, [x10, #0x38]
constexpr Expect kDialogManagerCreate[] = {
    { 605, Operand::Add, kDialogManagerLock },
    { 608, Operand::Unscaled, kDialogManagerFieldF8 },
    { 609, Operand::Offset, kDialogManagerField108 - kDialogManagerFieldF8 },
    { 615, Operand::Offset, kDialogManagerLock + offsetof(GameCriticalSection, SpinCount) - kDialogManagerFieldF8 },
    { 617, Operand::Offset, kDialogManagerModManager - kDialogManagerFieldF8 },
};

// LSDialogs::DialogManager::PerformDialogLoad(resource, visitor, data), which
// Ext.StaticData.LoadResource calls. It looks for a cached dialogue with the
// resource's file name; one that is there gets a reference more and is the
// result. Otherwise it makes a dialogue for the manager, keeps the visitor
// and its data in it, loads it, gives it its first reference and appends it
// to the cached dialogues.
//     ldr w8, [x20, #0x8c] ; ... ; ldr x24, [x21, #0x80] ; ... ; ldr x19, [x24] ; ldrb w8, [x19, #0xf7] ; ...
//     ldr w10, [x19, #0xf0] ; ... ; ldr x12, [x11, #0xe8]! ; ... ; bl LSDialogs::Dialog::Dialog(LSDialogs::DialogManager*)
//     stp x22, x21, [x19, #0x110] ; ... ; ldr w8, [x19, #0x108] ; add w8, w8, #1 ; str w8, [x19, #0x108] ; ... (twice)
//     str w8, [x20, #0x8c]
constexpr Expect kDialogLoad[] = {
    { 21, Operand::Offset, kDialogManagerCachedDialogs + kArraySize },
    { 24, Operand::Offset, kDialogManagerCachedDialogs },
    { 34, Operand::ByteOffset, kDialogFileName + kGameStringFlag },
    { 36, Operand::Offset, kDialogFileName + kGameStringLongLength },
    { 38, Operand::Unscaled, kDialogFileName },
    { 94, Operand::Calls, 0, "__ZN9LSDialogs6DialogC1EPNS_13DialogManagerE" },
    { 95, Operand::PairOffset, kDialogNodeVisitor },
    { 192, Operand::Offset, kDialogRefCount },
    { 193, Operand::Add, 1 },
    { 194, Operand::Offset, kDialogRefCount },
    { 241, Operand::Offset, kDialogRefCount },
    { 242, Operand::Add, 1 },
    { 243, Operand::Offset, kDialogRefCount },
    { 522, Operand::Offset, kDialogManagerCachedDialogs + kArraySize },
};

// LSDialogs::DialogManager::Update(time) unloads a cached dialogue without
// references once the delay has passed since the time kept in it, and gives
// back the references of the instances that ended: at zero the dialogue
// keeps the time, which is what releasing one from Lua does.
//     ldr x9, [x19, #0x80] ; ldr x22, [x9, x23, lsl #3] ; ldr w9, [x22, #0x108] ; cbnz ... ; ldr d0, [x22, #0x100]
//     ldr d1, [x20] ; fsub d0, d1, d0 ; ...
//     ldr d0, [x20] ; ... ; ldr w12, [x11, #0x108] ; subs w12, w12, #1 ; str w12, [x11, #0x108] ; b.ne ... ; str d0, [x11, #0x100]
constexpr Expect kDialogUnload[] = {
    { 135, Operand::Offset, kDialogManagerCachedDialogs },
    { 137, Operand::Offset, kDialogRefCount },
    { 139, Operand::Offset, kDialogUnloadedTime },
    { 140, Operand::Offset, 0 },
    { 514, Operand::Offset, 0 },
    { 531, Operand::Offset, kDialogRefCount },
    { 532, Operand::Add, 1 },
    { 533, Operand::Offset, kDialogRefCount },
    { 535, Operand::Offset, kDialogUnloadedTime },
};

// LSDialogs::Dialog::Visit(visitor) gives the visitor the two fixed strings
// after the file name under their attributes' names: LSDFS::strUUID the
// first, LSDFS::strTimelineId the second.
//     add x2, x23, #0xf8 ; ... ; adrp x1, LSDFS::strUUID@page ; add x1, x1, LSDFS::strUUID@pageoff
//     add x2, x23, #0xfc ; ... ; adrp x1, LSDFS::strTimelineId@page ; add x1, x1, LSDFS::strTimelineId@pageoff
constexpr Expect kDialogVisit[] = {
    { 259, Operand::Add, kDialogUuid },
    { 289, Operand::Add, kDialogTimelineId },
};
bool CheckDialogVisit(Code const& f, std::string& problem)
{
    uint64_t uuid = f.Image.FindVA("__ZN5LSDFS7strUUIDE");
    uint64_t timeline = f.Image.FindVA("__ZN5LSDFS13strTimelineIdE");
    if (!uuid || !timeline || PageAddress(f.VA(263), f.At(263), f.At(264)) != uuid
        || PageAddress(f.VA(293), f.At(293), f.At(294)) != timeline) {
        problem = "the two fixed strings after a dialogue's file name are not visited as its UUID and its timeline's id";
        return false;
    }
    return true;
}

// ecl::DialogSystem::SystemLoad() makes the client's dialog manager, keeps
// it, and puts it into the system it holds before it:
//     bl LSDialogs::DialogManager::DialogManager(...) ; ldr x22, [x19, #0x30] ; str x21, [x19, #0x30] ; ...
//     ldp x8, x9, [x19, #0x28] ; str x9, [x8, #0x318]
// and ecl::TimelineSystem::HasAnyInteractiveTimelineActive() reads it there
// and looks the timelines' dialogs up in its map of instances:
//     ldr x11, [x0, #0x318] ; ldr w12, [x11, #0x24] ; ... ; ldr x16, [x11, #0x28]
constexpr Expect kClientDialogManagerCreate[] = {
    { 49, Operand::Calls, 0, "__ZN9LSDialogs13DialogManagerC1ERKN2ls10ModManagerEPNS_14INodeLogicBaseE" },
    { 51, Operand::Offset, 0x30 },
    { 132, Operand::PairOffset, 0x28 },
    { 133, Operand::Offset, kClientTimelineDialogManager },
};
// ecl::TimelineSystem's constructor puts the vtables of its three listeners
// at +0x10, +0x18 and +0x20 and keeps the systems it is given from +0x2b8 on,
// the sound system first and the music manager last:
//     stp x11, x12, [x19, #0x10] ; add x10, x10, #0x150 ; str x10, [x19, #0x20] ; ...
//     str x28, [x19, #0x2b8] ; ... ; str x8, [x19, #0x2f8]
// and two of its event handlers append to the arrays of requests at +0x770
// and +0x780:
//     add x0, x0, #0x770 ; bl DynamicArray<UnloadRequest>::Append... ; mov w8, #0x1380 ; mov w9, #1 ; strb w9, [x19, x8]
//     add x0, x0, #0x780 ; bl DynamicArray<ReactivateCameraRequest>::Append...
// The flags that end the object are set by handlers of five instructions:
//     mov w8, #0x137e ; mov w9, #1 ; strb w9, [x0, x8] ; mov w0, #1 ; ret       (the client's session loaded)
//     mov w8, #0x1382 ; ...                                                     (the level deactivated)
constexpr Expect kClientTimelineCreate[] = {
    { 51, Operand::PairOffset, kClientTimelineStateListener - 8 },
    { 53, Operand::Offset, kClientTimelineDialogListener },
    { 98, Operand::Offset, kClientTimelineSoundSystem },
    { 106, Operand::Offset, kClientTimelineMusicManager },
};
constexpr Expect kClientTimelineLevelUnload[] = {
    { 4, Operand::Add, kClientTimelineUnloadRequests },
    { 6, Operand::Move, kClientTimelineLevelUnloaded },
};
constexpr Expect kClientTimelineSessionLoadedSet[] = {
    { 0, Operand::Move, kClientTimelineSessionLoaded },
};
constexpr Expect kClientTimelineLevelDeactivatedSet[] = {
    { 0, Operand::Move, kClientTimelineLevelDeactivated },
};
constexpr Expect kClientTimelineStateChanged[] = {
    { 13, Operand::Add, kClientTimelineCameraRequests },
};
constexpr Expect kClientDialogManagerRead[] = {
    { 5, Operand::Offset, kClientTimelineDialogManager },
    { 6, Operand::Offset, kDialogManagerDialogs + kRefMapBucketCount },
    { 17, Operand::Offset, kDialogManagerDialogs + kRefMapBuckets },
};

// ~DialogManager(): the dialogs, the cached dialogs, the four maps after the
// keyword parser, the lock, the second array and field_F8.
//     ldr w8, [x19, #0x24] ; ... ; ldr x10, [x19, #0x28] ; ... ; ldr w8, [x19, #0x8c] ; ldr x22, [x19, #0x80] ; ...
//     add x8, x19, #0x118 ; ... ; ldr w1, [x19, #0x118] ; ... ; bl semaphore_destroy ; ... ; ldr w8, [x19, #0x110] ; ...
//     ldr x21, [x19, #0x108] ; ... ; ldr w8, [x19, #0x100] ; ... ; ldr w8, [x19, #0x104] ; ... ; ldr x21, [x19, #0xf8]
constexpr Expect kDialogManagerDestroy[] = {
    { 14, Operand::Offset, kDialogManagerDialogs + kRefMapBucketCount },
    { 17, Operand::Offset, kDialogManagerDialogs + kRefMapBuckets },
    { 73, Operand::Offset, kDialogManagerCachedDialogs + kArraySize },
    { 75, Operand::Offset, kDialogManagerCachedDialogs },
    { 307, Operand::Offset, kDialogManagerScriptFlags + kRefMapItemCount },
    { 312, Operand::Offset, kDialogManagerScriptFlags + kRefMapBuckets },
    { 360, Operand::Offset, kDialogManagerVariables + kRefMapBucketCount },
    { 363, Operand::Offset, kDialogManagerVariables + kRefMapBuckets },
    { 426, Operand::Offset, kDialogManagerDialogToVariable + kRefMapItemCount },
    { 431, Operand::Offset, kDialogManagerDialogToVariable + kRefMapBuckets },
    { 492, Operand::Offset, kDialogManagerSpeakerGroups + kRefMapItemCount },
    { 497, Operand::Offset, kDialogManagerSpeakerGroups + kRefMapBuckets },
    { 541, Operand::Add, kDialogManagerLock },
    { 545, Operand::Offset, kDialogManagerLock + offsetof(GameCriticalSection, Semaphore) },
    { 551, Operand::Offset, kDialogManagerField108 + kArrayCapacity },
    { 553, Operand::Offset, kDialogManagerField108 + kArraySize },
    { 557, Operand::Offset, kDialogManagerField108 },
    { 631, Operand::Offset, kDialogManagerFieldF8 + kArrayCapacity },
    { 633, Operand::Offset, kDialogManagerFieldF8 + kArraySize },
    { 637, Operand::Offset, kDialogManagerFieldF8 },
};

// ecs::EntityWorld::GetComponent<T const, true>(entity) is the whole way from
// an entity to one of its components: the salts of the entity's thread in
// the storage container, the salt entry's storage index, that storage, its
// map from the entity to a page and an index, its map from the component
// type to a slot, and the slot's buffer in the page.
//     ldr x8, [x0, #0x2d0] ; add x9, x8, x9, lsl #4 ; ldr w10, [x9, #0x5c] ; ... ; add x9, x9, #0x50 ; ldrh w10, [x9, #0x8]
//     ... ; ldrh w9, [x9, #0x4] ; ldr x8, [x8] ; ldr x20, [x8, x9, lsl #3]
//     ldrsw x8, [x20, #0x1c8] ; ... ; ldr x9, [x20, #0x1c0] ; ... ; ldr x9, [x20, #0x1e0] ; ... ; ldr x10, [x20, #0x1d0]
//     ... ; ldr x9, [x20, #0x1f0] ; ... ; ldr w8, [x20, #0x188] ; ... ; ldr x9, [x20, #0x180] ; ... ; ldr x9, [x20, #0x1a0]
//     ... ; ldr x10, [x20, #0x190] ; ... ; ldr x9, [x20, #0x1b0] ; ldr x10, [x20, #0x138] ; ... ; lsl x8, x8, #4 ; ldr x8, [x10, x8]
constexpr Expect kWorldGetComponent[] = {
    { 16, Operand::Offset, kWorldStorage },
    { 17, Operand::ShiftLeft, kStorageSaltsShift },
    { 18, Operand::Offset, kStorageSalts + 0xc },
    { 21, Operand::Add, kStorageSalts },
    { 22, Operand::Offset, 0x8 },
    { 29, Operand::ShiftLeftBy, kSaltShift },
    { 36, Operand::Offset, kSaltStorageIndex },
    { 37, Operand::Offset, kStorageStorages },
    { 39, Operand::Offset, kStorageDataEntities + kHashMapBucketCount },
    { 43, Operand::Offset, kStorageDataEntities + kHashMapBuckets },
    { 47, Operand::Offset, kStorageDataEntities + kHashMapKeys },
    { 51, Operand::Offset, kStorageDataEntities + kHashMapNext },
    { 59, Operand::Offset, kWorldCache },
    { 67, Operand::Offset, kStorageDataEntities + kHashMapValues },
    { 89, Operand::Offset, kStorageDataComponentSlots + kHashMapBucketCount },
    { 95, Operand::Offset, kStorageDataComponentSlots + kHashMapBuckets },
    { 98, Operand::Offset, kStorageDataComponentSlots + kHashMapKeys },
    { 102, Operand::Offset, kStorageDataComponentSlots + kHashMapNext },
    { 106, Operand::Offset, kStorageDataComponentSlots + kHashMapValues },
    { 107, Operand::Offset, kStorageDataComponents },
    { 111, Operand::ShiftLeftBy, kComponentPageSlotShift },
};

// ecs::ComponentOps<T>::SendComponentAttachedSignal(...) calls every
// connection of the type's attached signal; SendComponentDetachedSignal the
// same for the detached one.
//     ldr x9, [x20, #0x10] ; ldr x28, [x9, #0x10] ; ldrsw x9, [x9, #0x1c] ; ... ; ldr x23, [x20, #0x20] ; mov w8, #0x48 ;
//     madd x25, x9, x8, x28 ; add x0, x28, #0x8 ; ldr x8, [x28], #0x48 ; ldr x8, [x8] ; ... ; blr x8
constexpr Expect kSignalAttached[] = {
    { 32, Operand::Offset, kComponentOpsSignals },
    { 33, Operand::Offset, kSignalsAttached + kSignalConnections },
    { 34, Operand::Offset, kSignalsAttached + kSignalConnections + 0xc },
    { 44, Operand::Offset, kComponentOpsWorld },
    { 45, Operand::Move, kSignalConnectionSize },
    { 47, Operand::Add, kSignalFunctionState },
    { 48, Operand::Unscaled, kSignalConnectionSize },
    { 49, Operand::Offset, 0 },
};
constexpr Expect kSignalDetached[] = {
    { 32, Operand::Offset, kComponentOpsSignals },
    { 33, Operand::Offset, kSignalsDetached + kSignalConnections },
    { 34, Operand::Offset, kSignalsDetached + kSignalConnections + 0xc },
    { 44, Operand::Offset, kComponentOpsWorld },
    { 45, Operand::Move, kSignalConnectionSize },
    { 47, Operand::Add, kSignalFunctionState },
    { 48, Operand::Unscaled, kSignalConnectionSize },
    { 49, Operand::Offset, 0 },
};

// ---------------------------------------------------------------------------
// Component objects, and the network peer
// ---------------------------------------------------------------------------

// GetEntityObjectHandle() of each of the three triggers: ldr x0, [x0, #0xa8] ; ret
constexpr Expect kTriggerEntityRead[] = {
    { 0, Operand::Offset, kTriggerEntity },
};

// esv::Projectile::Projectile() stores a null handle into each of the eight
// handles from +0x8 on, and constructs the hit at +0x70.
//     stur q0, [x0, #0x8] ; ... ; add x0, x0, #0x70 ; bl eoc::HitDesc::HitDesc()
constexpr Expect kProjectileCreate[] = {
    { 10, Operand::Unscaled, kProjectileEntity },
    { 19, Operand::Add, kProjectileHit },
    { 20, Operand::Calls, 0, "__ZN3eoc7HitDescC1Ev" },
};

// ls::Construction::Construction(...) clears the two arrays the object
// begins with and stores the parsed id after them.
//     str q0, [x0] ; ... ; bl ls::Guid::ParseFromString ; stp x0, x1, [x19, #0x20]
constexpr Expect kConstructionCreate[] = {
    { 12, Operand::Offset, 0 },
    { 39, Operand::Calls, 0, "__ZN2ls4Guid15ParseFromStringERKNS_11_StringViewIcEE" },
    { 40, Operand::PairOffset, kConstructionInstanceId },
};

// esv::TimelineSystem::RegisterActor(...) builds the actor control component
// in 0x28 bytes of frame storage: the default values first (the two ids, the
// two bytes as one 16-bit zero, the word at +0x24; nothing for +0x22 and
// +0x23), then its arguments.
//     mov w1, #0x28 ; bl ecs::ComponentFrameStorageAllocRaw
//     str q0, [x21, #0x10]! ; str wzr, [x0, #0x24] ; ... ; str q0, [x20] ; strh wzr, [x20, #0x20]
//     str w8, [x20, #0x24] ; ... ; strb w8, [x20, #0x20] ; strb w25, [x20, #0x21]
constexpr char kRegisterActorSymbol[] =
    "__ZN3esv14TimelineSystem13RegisterActorERN3ecs9WorldViewIJN3eoc35SyncedTimelineActorControlComponentENS3_30Syn"
    "cedTimelineControlComponentENS3_8timeline24ActorVisualDataComponentENS6_17LongRestComponentENS_9CharacterENS_4"
    "ItemENS_4rest37RestTypeRequestContentLoadedComponentENS_32ServerTimelineActorDataComponentENS_43ServerTimeline"
    "CreationConfirmationComponentENS_27ServerTimelineDataComponentENS_46ServerTimelineDestructionConfirmationCompo"
    "nentENS_37ServerTimelineWorldCinematicComponentEKNS6_22HideEquipmentComponentEKNS3_28TimelineReplicationCompon"
    "entEKNS3_6dialog14StateComponentEKNS3_4camp17PresenceComponentEKNS3_9gamestate16RunningComponentEKNS3_4item13I"
    "temComponentEKNS3_5party32CurrentlyFollowingPartyComponentEKNSY_13ViewComponentEKNS3_15PlayerComponentEKNS3_4r"
    "est16LongRestTimelineEKNS15_13LongRestUsersEKNS3_10spell_cast34PrepareStartEventOneFrameComponentEKNS1A_14Stat"
    "eComponentEKNS_8timeline39BackgroundActorRequestOneFrameComponentEKNS1F_28DeferredEndingStateComponentEKNS1F_4"
    "1TimelineFadeClearRequestOneFrameComponentEKNS_9placement14StateComponentEKNS1M_19ValidationComponentEKNS_20Ti"
    "melineSceneTriggerEKNS1F_21FadedInUsersComponentEKNS_7trigger34TriggerWorldAutoTriggeredComponentEKN2ls27Level"
    "InstanceStateComponentEKNS1Y_18TransformComponentEKNS1Y_17SavegameComponentEKNS1Y_7trigger13AreaComponentEKNS1"
    "Y_4uuid9ComponentEKNS28_24ToHandleMappingComponentEEEERKNS1Y_4GuidERKjRKNS6_5ActorES2H_b";

constexpr Expect kActorControlCreate[] = {
    { 107, Operand::Move, kActorControlSize },
    { 108, Operand::Calls, 0, "__ZN3ecs29ComponentFrameStorageAllocRawERNS_21ComponentFrameStorageEiRNS_26ComponentFrameStorageIndexE" },
    { 112, Operand::Unscaled, kActorControlTimeline },
    { 113, Operand::Offset, kActorControlWord },
    { 113, Operand::StoresZero },
    { 117, Operand::Offset, 0 },
    { 118, Operand::Offset, kActorControlSlot },
    { 118, Operand::StoresZero },
    { 146, Operand::Offset, kActorControlWord },
    { 152, Operand::ByteOffset, kActorControlSlot },
    { 153, Operand::ByteOffset, kActorControlListener },
};

// The widths of the default values: 16 bits for the two bytes, 32 for the word.
bool CheckActorControlDefaults(Code const& f, std::string& problem)
{
    static_assert(kActorControlListener == kActorControlSlot + 1);
    bool word = (f.At(113) & 0xffc00000) == 0xb9000000;     // str wT, [xN, #offset]
    bool half = (f.At(118) & 0xffc00000) == 0x79000000;     // strh wT, [xN, #offset]
    if (!word || !half) {
        problem = "a new actor control component's two bytes and its word are not zeroed as 16 and 32 bits";
        return false;
    }
    return true;
}

// The component's replication writes its four replicated members: 16 bytes
// at +0 and at +0x10, one byte at +0x20 and at +0x21.
//     mov w25, #0x10 ; mov w26, #0x1 ; ... ; add x8, x22, #0x10 ; ... ; add x8, x22, #0x20 ; ... ; add x8, x22, #0x21
constexpr Expect kActorControlSerialize[] = {
    { 13, Operand::Move, sizeof(uint64_t[2]) },
    { 14, Operand::Move, 1 },
    { 22, Operand::PairOffset, 0 },
    { 24, Operand::Calls, 0, "__ZN2ls13ScratchBuffer5WriteERKNS_7MemViewIKhEE" },
    { 25, Operand::Add, kActorControlTimeline },
    { 27, Operand::PairOffset, 0 },
    { 30, Operand::Add, kActorControlSlot },
    { 32, Operand::PairOffset, 0 },
    { 35, Operand::Add, kActorControlListener },
    { 37, Operand::PairOffset, 0 },
    { 39, Operand::Calls, 0, "__ZN2ls13ScratchBuffer5WriteERKNS_7MemViewIKhEE" },
};

// The companion definition component's replication hands the definition the
// component begins with to its own function, then writes four bytes at
// +0x138 and one at +0x13c.
//     mov w25, #0x4 ; mov w26, #0x1 ; ... ; bl ecs::sync::Serialize<eoc::character_creation::CompanionDefinition, ...>
//     add x8, x22, #0x138 ; ... ; stp x8, x25, [sp] ; ... ; add x8, x22, #0x13c ; ... ; stp x8, x26, [sp]
constexpr Expect kCompanionSerialize[] = {
    { 13, Operand::Move, sizeof(uint32_t) },
    { 14, Operand::Move, 1 },
    { 24, Operand::Add, kCompanionIconVersion },
    { 26, Operand::PairOffset, 0 },
    { 28, Operand::Calls, 0, "__ZN2ls13ScratchBuffer5WriteERKNS_7MemViewIKhEE" },
    { 29, Operand::Add, kCompanionHasIcon },
    { 31, Operand::PairOffset, 0 },
    { 33, Operand::Calls, 0, "__ZN2ls13ScratchBuffer5WriteERKNS_7MemViewIKhEE" },
};

// Which register each of the two views takes its size from.
bool CheckCompanionSizes(Code const& f, std::string& problem)
{
    auto second = [](uint32_t stp) { return (stp >> 10) & 0x1f; };
    if (RegD(f.At(13)) != second(f.At(26)) || RegD(f.At(14)) != second(f.At(31))) {
        problem = "the icon's version and flag are not written as four bytes and one";
        return false;
    }
    return true;
}

// ls::TLShot::TLShot() gives every member its default: the two bytes the
// base ends with, the switch interval right after them, and at the end a
// byte and then the map, which is the last 0x40 bytes of the object.
//     strh wzr, [x0, #0x60] ; ... ; str w8, [x0, #0x64] ; ... ; strb wzr, [x0, #0xb8] ; ... ; strb wzr, [x0, #0xf0]
//     mov x8, x0 ; str q0, [x8, #0xf8]! ; stp q0, q0, [x8, #0x10] ; str q0, [x8, #0x30] ; ret
constexpr Expect kShotCreate[] = {
    { 14, Operand::Offset, kTimelineComponentLastBytes },
    { 14, Operand::StoresZero },
    { 19, Operand::Offset, kShotSwitchInterval },
    { 31, Operand::ByteOffset, kShotLogicEnabled },
    { 34, Operand::ByteOffset, kShotPrefetchData - 8 },
    { 36, Operand::Unscaled, kShotPrefetchData },
};

// The map's last 16 bytes are the object's last: stp q0, q0, [x8, #0x10] ; str q0, [x8, #0x30].
bool CheckShotEnd(Code const& f, std::string& problem)
{
    static_assert(kShotPrefetchData + 0x40 == kShotSize);
    bool pair = f.At(37) == (0xad000000 | (1u << 15) | (8u << 5));     // stp q0, q0, [x8, #0x10]
    bool last = f.At(38) == (0x3d800000 | (3u << 10) | (8u << 5));     // str q0, [x8, #0x30]
    if (!pair || !last) {
        problem = "the shot's map is not cleared as the 0x40 bytes that end the object";
        return false;
    }
    return true;
}

// net::AbstractPeer::ProcessMsg walks the protocols: their count, then the array.
//     ldr w8, [x0, #0x2dc] ; ... ; ldr x23, [x0, #0x2d0]
constexpr Expect kPeerProcessMsg[] = {
    { 5, Operand::Offset, kPeerProtocols + 0xc },
    { 11, Operand::Offset, kPeerProtocols },
};

// net::AbstractPeer::GetProtocol<esv::LoadProtocol>() looks the protocol's id
// up in the map: the count of buckets, the buckets, the keys, the next links.
//     ldrsw x9, [x0, #0x2e8] ; ... ; ldr x10, [x0, #0x2e0] ; ... ; ldr x10, [x0, #0x300] ; ... ; ldr x11, [x0, #0x2f0]
constexpr Expect kPeerGetProtocol[] = {
    { 0, Operand::Offset, kPeerProtocolMap + 0x8 },
    { 7, Operand::Offset, kPeerProtocolMap },
    { 11, Operand::Offset, kPeerProtocolMap + 0x20 },
    { 15, Operand::Offset, kPeerProtocolMap + 0x10 },
};

// net::MessageFactory::GetFreeMessage(int) takes the pool of that id from
// the array and locks the factory; an empty pool makes its template create
// as many messages as the pool grows by and queues them; then one message
// leaves the queue and joins the leased ones.
//     ldr x8, [x0, #0x8] ; ldr x24, [x8, w1, sxtw #3] ; ... ; add x19, x0, #0x20 ; bl ls::CriticalSection::Lock()
//     ldr w8, [x24, #0x1c] ; ... ; ldr x22, [x24, #0x8] ; ... ; add x20, x24, #0x10 ; ldr x0, [x24] ; ldr x8, [x0] ;
//     ldr x8, [x8, #0x20] ; blr x8 ; ... ; bl ls::Queue<net::Message*>::Add
//     ldr x8, [x24, #0x10] ; ldp w9, w10, [x24, #0x20] ; ... ; ldr w11, [x24, #0x18] ; ... ; ldp w9, w8, [x24, #0x30]
constexpr Expect kFactoryGetFree[] = {
    { 7, Operand::Offset, kFactoryPools },
    { 11, Operand::Add, kFactoryLock },
    { 13, Operand::Calls, 0, "__ZN2ls15CriticalSection4LockEv" },
    { 14, Operand::Offset, kPoolMessages + 0xc },
    { 16, Operand::Offset, kPoolGrowSize },
    { 18, Operand::Add, kPoolMessages },
    { 19, Operand::Offset, kPoolTemplate },
    { 21, Operand::Offset, kMessageCreateSlot },
    { 26, Operand::Calls, 0, "__ZN2ls5QueueIPN3net7MessageENS_15MallocInterfaceIS3_EEE3AddIS3_EEvOT_" },
    { 29, Operand::Offset, kPoolMessages },
    { 30, Operand::PairOffset, kPoolMessages + 0x10 },
    { 33, Operand::Offset, kPoolMessages + 0x8 },
    { 44, Operand::PairOffset, kPoolLeased + 0x8 },
    { 94, Operand::Offset, kPoolLeased },
};

// net::MessageFactory::RegisterMessage(...) makes a pool of 0x38 bytes and
// stores the template and the grow size into its first two words.
//     mov w0, #0x38 ; bl malloc ; ... ; stp x23, x26, [x19]
constexpr Expect kFactoryRegister[] = {
    { 33, Operand::Move, kPoolSize },
    { 34, Operand::CallsImport, 0, "_malloc" },
    { 50, Operand::PairOffset, kPoolTemplate },
};

// eocnet::ClientConnectMessage::CreateInstance() gives a new message the
// defaults of net::Message: the id with the reliability as one constant of
// 8 bytes, the priority, two zero bytes, and 16 zero bytes from the timestamp.
//     ldr d0, [x8, #...] ; str d0, [x19, #0x8] ; mov w8, #0x1 ; str w8, [x19, #0x10] ; strh wzr, [x19, #0x14] ; stp xzr, xzr, [x19, #0x18]
constexpr Expect kMessageCreate[] = {
    { 38, Operand::Offset, kMessageId },
    { 39, Operand::Move, kMessageDefaultPriority },
    { 40, Operand::Offset, kMessagePriority },
    { 41, Operand::Offset, kMessageOrdering },
    { 41, Operand::StoresZero },
    { 42, Operand::PairOffset, kMessageTimestamp },
};

bool CheckMessageDefaults(Code const& f, std::string& problem)
{
    static_assert(kMessageTimestamp + 0x10 == kMessageSize);
    auto value = static_cast<uint64_t const*>(f.Image.DataAt(LoadedAddress(f, 36), sizeof(uint64_t)));
    if (!value || *value != (((uint64_t)kMessageDefaultReliability << 32) | kClientConnectMessageId)) {
        problem = "a new connect message's id and reliability are not the eight bytes the Mac type has at +" + Hex(kMessageId);
        return false;
    }
    return true;
}

// net::Message::Reset() clears the 16 bytes from the timestamp: stp xzr, xzr, [x0, #0x18] ; ret
constexpr Expect kMessageReset[] = {
    { 0, Operand::PairOffset, kMessageTimestamp },
};

// net::MessageVisitor::Visit(ls::ID<net::NetIDTraits>&) picks the write or
// the read function of the visitor by the word at +8, and passes 8 bytes.
//     ldr w8, [x0, #0x8] ; ldr x9, [x0] ; cmp w8, #0 ; mov w8, #0x10 ; mov w10, #0x18 ; csel x8, x10, x8, eq ; ldr x3, [x9, x8]
constexpr Expect kVisitorVisitId[] = {
    { 0, Operand::Offset, kSerializerIsWriting },
    { 3, Operand::Move, kSerializerWriteSlot },
    { 4, Operand::Move, kSerializerReadSlot },
    { 7, Operand::Move, sizeof(uint64_t) },
};

// Zero at +8 selects the second of the two (the read function): csel x8, x10, x8, eq.
bool CheckVisitorSelects(Code const& f, std::string& problem)
{
    uint32_t select = f.At(5);
    bool csel = (select & 0xffe00c00) == 0x9a800000 && ((select >> 12) & 0xf) == 0;      // csel xd, xn, xm, eq
    if (!csel || ((select >> 5) & 0x1f) != RegD(f.At(4)) || ((select >> 16) & 0x1f) != RegD(f.At(3))) {
        problem = "the visitor does not take its read function when the word at +" + Hex(kSerializerIsWriting) + " is zero";
        return false;
    }
    return true;
}

// net::BitStreamSerializer::WriteInternal(void const*, unsigned long): ldr x0, [x0, #0x10] ; b RakNet::BitStream::Write
constexpr Expect kSerializerWrite[] = {
    { 0, Operand::Offset, kSerializerStream },
    { 1, Operand::JumpsTo, 0, "__ZN6RakNet9BitStream5WriteEPKcj" },
};

// ---------------------------------------------------------------------------
// The network session: the client's connection, the connect message, the
// server's peers and their players, where a message came from
// ---------------------------------------------------------------------------

constexpr char kGetFreeMessageSymbol[] = "__ZN3net14MessageFactory14GetFreeMessageEi";
constexpr char kStringAssignSymbol[] = "__ZN2ls9STDStringaSERKS0_";
constexpr char kVisitStringSymbol[] = "__ZN3net14MessageVisitor5VisitERN2ls9STDStringE";
constexpr char kPeerInfoGetPlayerSymbol[] = "__ZN3esv12GamePeerInfo13GetPlayerInfoERKs";

// ecl::JoinProtocol::SendClientConnectMsg() takes a connect message from its
// peer's factory, assigns the game's version to the first string and the
// build's name to the second, and sends it to the host's peer.
//     ldr x8, [x19, #0x8] ; ldr x0, [x8, #0x1f8] ; mov w1, #6 ; bl net::MessageFactory::GetFreeMessage ; mov x20, x0
//     ... ; bl ls::FileSystem::GetExeVersion ; add x0, x20, #0x28 ; ... ; bl ls::STDString::operator=
//     ... ; add x0, x20, #0x38 ; ... ; bl ls::STDString::operator=
//     ... ; ldr x0, [x19, #0x8] ; ldr w1, [x0, #0x398] ; mov x2, x20 ; bl net::AbstractPeer::SendMsg(peer, message)
constexpr Expect kJoinSendConnect[] = {
    { 17, Operand::Offset, kProtocolPeer },
    { 18, Operand::Offset, kPeerMessageFactory },
    { 19, Operand::Move, kClientConnectMessageId },
    { 20, Operand::Calls, 0, kGetFreeMessageSymbol },
    { 31, Operand::Calls, 0, "__ZN2ls10FileSystem13GetExeVersionEv" },
    { 32, Operand::Add, kConnectGameVersion },
    { 34, Operand::Calls, 0, kStringAssignSymbol },
    { 71, Operand::Add, kConnectBuild },
    { 73, Operand::Calls, 0, kStringAssignSymbol },
    { 102, Operand::Offset, kProtocolPeer },
    { 103, Operand::Offset, kClientHostPeer },
    { 105, Operand::Calls, 0, "__ZN3net12AbstractPeer7SendMsgEN2ls8TypeWrapIiNS_15PeerIDClassnameELb1EEEPNS_7MessageE" },
};

// The two strings are the new message's, and the host's peer is the protocol's peer's.
bool CheckConnectStrings(Code const& f, std::string& problem)
{
    uint32_t message = RegD(f.At(21));
    if (RegN(f.At(32)) != message || RegN(f.At(71)) != message) {
        problem = "the strings assigned are not the connect message's";
        return false;
    }
    if (RegN(f.At(103)) != RegD(f.At(102))) {
        problem = "the peer the message is sent to is not read from the protocol's peer";
        return false;
    }
    return true;
}

// ecl::GameClient::OnConnectionRequestAccepted(...) reads the status and the
// lobby manager, sets the status to 2 when the host accepted, back to 0 after
// shutting the peer down when the connection cannot go on, and keeps the
// host's peer from the event.
//     ldr w8, [x0, #0x4c0] ; ... ; ldr x21, [x19, #0x4b8] ; ... ; mov w8, #2 ; str w8, [x19, #0x4c0]
//     ... ; bl net::AbstractPeer::Shutdown(unsigned int) ; str xzr, [x19, #0x530] ; str wzr, [x19, #0x4c0]
//     ... ; ldr w8, [x20, #0x8] ; str w8, [x19, #0x398]
constexpr Expect kClientAccepted[] = {
    { 13, Operand::Offset, kGameClientStatus },
    { 16, Operand::Offset, kGameClientLobby },
    { 40, Operand::Move, kGameClientAccepted },
    { 41, Operand::Offset, kGameClientStatus },
    { 184, Operand::Calls, 0, "__ZN3net12AbstractPeer8ShutdownEj" },
    { 186, Operand::Offset, kGameClientStatus },
    { 186, Operand::StoresZero },
    { 307, Operand::Offset, kClientHostPeer },
};

// The 2 is what is stored.
bool CheckClientAcceptedStatus(Code const& f, std::string& problem)
{
    if (RegD(f.At(40)) != RegD(f.At(41))) {
        problem = "the status stored is not the value moved before it";
        return false;
    }
    return true;
}

// eocnet::ClientConnectMessage::Visit(net::MessageVisitor*), the function the
// extender wraps: the game's version is the first string visited and the
// build's name, which the extender appends to, a later one.
//     add x1, x0, #0x28 ; mov x0, x19 ; bl net::MessageVisitor::Visit(ls::STDString&)
//     ... ; add x1, x20, #0x38 ; mov x0, x19 ; bl net::MessageVisitor::Visit(ls::STDString&)
constexpr Expect kConnectVisit[] = {
    { 13, Operand::Add, kConnectGameVersion },
    { 15, Operand::Calls, 0, kVisitStringSymbol },
    { 203, Operand::Add, kConnectBuild },
    { 205, Operand::Calls, 0, kVisitStringSymbol },
};

// esv::GameServer::GetPeerInfo(peer const&) looks the peer up in the map of
// peers, and adds a node of 0x68 bytes with the peer's information at +0x10
// when it is not there.
//     ldr w9, [x0, #0x694] ; ... ; ldr x9, [x0, #0x698] ; ... ; ldr w9, [x21, #0x8]
//     ... ; mov w0, #0x68 ; ... ; add x0, x21, #0x10 ; bl esv::GamePeerInfo::GamePeerInfo()
//     ... ; ldr w8, [x19, #0x690] ; add w8, w8, #1 ; str w8, [x19, #0x690] ; add x0, x21, #0x10
constexpr Expect kServerPeerInfo[] = {
    { 6, Operand::Offset, kGameServerPeerInfo + kRefMapBucketCount },
    { 10, Operand::Offset, kGameServerPeerInfo + kRefMapBuckets },
    { 14, Operand::Offset, kMapNodeKey },
    { 39, Operand::Move, kMapNodeObjectValue + kPeerInfoSize },
    { 46, Operand::Add, kMapNodeObjectValue },
    { 47, Operand::Calls, 0, "__ZN3esv12GamePeerInfoC1Ev" },
    { 65, Operand::Offset, kGameServerPeerInfo + kRefMapItemCount },
    { 67, Operand::Offset, kGameServerPeerInfo + kRefMapItemCount },
    { 68, Operand::Add, kMapNodeObjectValue },
};

// esv::GamePeerInfo::GetPlayerInfo(short const&) does the same in the map of
// the peer's players, whose 16-bit keys are the players' indices and whose
// nodes are 0x58 bytes.
//     ldr w9, [x0, #0xc] ; ... ; ldr x9, [x0, #0x10] ; ... ; ldrh w9, [x21, #0x8]
//     ... ; mov w0, #0x58 ; ... ; strh w8, [x21, #0x8] ; ... ; ldr w8, [x19, #0x8] ; ... ; add x0, x21, #0x10
constexpr Expect kPeerInfoPlayerInfo[] = {
    { 6, Operand::Offset, kPeerInfoPlayers + kRefMapBucketCount },
    { 11, Operand::Offset, kPeerInfoPlayers + kRefMapBuckets },
    { 15, Operand::Offset, kMapNodeKey },
    { 40, Operand::Move, kMapNodeObjectValue + kPlayerInfoSize },
    { 65, Operand::Offset, kMapNodeKey },
    { 71, Operand::Offset, kPeerInfoPlayers + kRefMapItemCount },
    { 74, Operand::Add, kMapNodeObjectValue },
};

// The key read and written is 16 bits: ldrh and strh.
bool CheckPlayerKeys(Code const& f, std::string& problem)
{
    if ((f.At(15) & 0xffc00000) != 0x79400000 || (f.At(65) & 0xffc00000) != 0x79000000) {
        problem = "a player's key is not read and written as 16 bits";
        return false;
    }
    return true;
}

// esv::GameServer::GetPlayerInfo(net::UserID const&) finds the user's peer in
// the upper 16 bits of the user, and its player by the lower 16.
//     ldr w8, [x1] ; lsr w22, w8, #16 ; ldr w9, [x0, #0x694] ; ... ; ldr x9, [x0, #0x698]
//     ... ; ldr w8, [x19] ; add x0, x21, #0x10 ; strh w8, [sp, #0xe] ; add x1, sp, #0xe ; bl esv::GamePeerInfo::GetPlayerInfo
constexpr Expect kServerPlayerInfo[] = {
    { 8, Operand::Offset, 0 },
    { 9, Operand::ShiftRight, kUserPeerShift },
    { 10, Operand::Offset, kGameServerPeerInfo + kRefMapBucketCount },
    { 13, Operand::Offset, kGameServerPeerInfo + kRefMapBuckets },
    { 70, Operand::Offset, 0 },
    { 71, Operand::Add, kMapNodeObjectValue },
    { 74, Operand::Calls, 0, kPeerInfoGetPlayerSymbol },
};

// The peer is the user shifted, the player's index the user's low 16 bits.
bool CheckUserParts(Code const& f, std::string& problem)
{
    bool shifted = RegN(f.At(9)) == RegD(f.At(8)) && RegN(f.At(8)) == 1;
    bool index = (f.At(72) & 0xffc00000) == 0x79000000 && RegD(f.At(72)) == RegD(f.At(70)) && RegN(f.At(70)) == 19;
    if (!shifted || !index) {
        problem = "the peer and the player are not the user's upper and lower 16 bits";
        return false;
    }
    return true;
}

// esv::ChatProtocol::ProcessMsg(...) reads the user a message came from at the
// start of its context, and the user's peer is that shifted right by 16.
//     ldr w19, [x2] ; ldr x8, [x0, #0x8] ; ldr x0, [x8, #0x1f8] ; mov w1, #0x9a ; bl net::MessageFactory::GetFreeMessage
//     ... ; lsr x22, x19, #16 ; ldr x8, [x25, #0x8] ; ldr x1, [x8, #0x200] ; add x0, sp, #0x38 ; mov x2, x22
//     bl net::AbstractPeer::GetGUIDForPeer(peer) const
constexpr Expect kChatContext[] = {
    { 14, Operand::Offset, kContextUser },
    { 15, Operand::Offset, kProtocolPeer },
    { 16, Operand::Offset, kPeerMessageFactory },
    { 18, Operand::Calls, 0, kGetFreeMessageSymbol },
    { 30, Operand::ShiftRight, kUserPeerShift },
    { 35, Operand::Calls, 0, "__ZNK3net12AbstractPeer14GetGUIDForPeerEN2ls8TypeWrapIiNS_15PeerIDClassnameELb1EEE" },
};

// The user is read from the context (the third argument) and is what is shifted.
bool CheckContextUser(Code const& f, std::string& problem)
{
    if (RegN(f.At(14)) != 2 || RegN(f.At(30)) != RegD(f.At(14))) {
        problem = "the peer is not taken from the user at the start of the message's context";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// RPGStats and what hangs off it
// ---------------------------------------------------------------------------
// None of the functions below is one the extender hooks (RPGStats::Start is,
// and is not used here).

constexpr char kStatsSymbol[] = "__ZN8RPGStats5m_ptrE";
constexpr char kTranslatedStringDestroySymbol[] = "__ZN2ls16TranslatedStringD1Ev";
constexpr char kBufferResetSymbol[] = "__ZN2ls9UniquePtrI19RPGStats_DataBuffervE5ResetEv";
constexpr char kNameMapSetSymbol[] = "__ZN2ls7HashMapINS_11FixedStringEiNS_19HashTableOpsDefaultEE12SetUniversalIRKS1_iEENS_7EMapSetEOT_OT0_";

// RPGStats::Destroy() empties every member in turn. What identifies a member
// is the function it is handed to, or how it is walked.
//     add x8, x0, #0x40a ; stlrb wzr, [x8]             Loaded
//     add x22, x19, #0x40c ; ldar w8, [x22] ...        the lock
//     ldr w8, [x19, #0x318] ; ldr w9, [x19, #0x31c] ; ldr x8, [x19, #0x320]    the map of counts, buckets, table
//     ldr x20, [x19, #0x280] ; ... free ; str xzr, [x19, #0x280]             the random numbers
//     the five managers and the item types: each its elements (pointer +8, size +0x14), its names
//     (+0x18 ... +0x44) and its next handle (+0x58); the second through CRPGStats_Modifier_List_Manager::Destroy
//     ldr x20, [x19, #0x2b0]                           the extra data
//     str wzr, [x19, #0x354]                           the fixed strings' size
//     ldr w8, [x19, #0x3f0] ; ldr x10, [x19, #0x3f8] ; str wzr, [x19, #0x400]  the blood types
//     the four arrays from 0x358, the translated strings destroyed one by one
//     add x0, x19, #0x248 ; bl eoc::StatsFunctorListManager::Clear
//     ldr x9, [x19, #0x410] ; str wzr, [x19, #0x41c]   the conditions
//     the four manager pointers from 0x398
//     add x0, x19, #0x3b8 ; bl ls::UniquePtr<RPGStats_DataBuffer>::Reset
//     ldr w8, [x19, #0x3c0] ... ls::gst::Map::Release  the current buffer's path
//     ldr w9, [x19, #0x3c8] ; ldr x8, [x19, #0x3d0] ; str wzr, [x19, #0x3d8]   the map of buffers
//     add x0, x19, #0x3e0 ; bl ls::DynamicArray<ls::UniquePtr<RPGStats_DataBuffer>>::RemoveAllOrPurge
constexpr Expect kStatsDestroy[] = {
    { 9, Operand::Add, kStatsLoaded },
    { 13, Operand::Add, kStatsLock },
    { 71, Operand::Offset, kStatsRefMap + kRefMapItemCount },
    { 76, Operand::Offset, kStatsRefMap + kRefMapBucketCount },
    { 79, Operand::Offset, kStatsRefMap + kRefMapBuckets },
    { 130, Operand::Offset, kStatsRandom },
    { 152, Operand::Offset, kStatsRandom },
    { 152, Operand::StoresZero },
    { 153, Operand::Offset, kStatsValueLists + kNamedManagerValuesSize },
    { 171, Operand::Offset, kStatsValueLists + kNamedManagerValues },
    { 175, Operand::Calls, 0, "__ZN2ls14FixedStringMapIiED2Ev" },
    { 203, Operand::Offset, kStatsValueLists + kNamedManagerNames + kHashMapBuckets },
    { 204, Operand::Offset, kStatsValueLists + kNamedManagerNames + kHashMapBucketCount },
    { 213, Operand::Offset, kStatsValueLists + kNamedManagerNames + kHashMapKeysSize },
    { 222, Operand::Offset, kStatsValueLists + kNamedManagerNames + kHashMapKeys },
    { 235, Operand::Offset, kStatsValueLists + kNamedManagerNames + kHashMapNextSize },
    { 236, Operand::Offset, kStatsValueLists + kNamedManagerNextHandle },
    { 237, Operand::Add, kStatsModifierLists },
    { 238, Operand::Calls, 0, "__ZN31CRPGStats_Modifier_List_Manager7DestroyEv" },
    { 239, Operand::Offset, kStatsObjects + kNamedManagerValuesSize },
    { 247, Operand::Offset, kStatsObjects + kNamedManagerValues },
    { 290, Operand::Offset, kStatsObjects + kNamedManagerNextHandle },
    { 308, Operand::Offset, kStatsTreasureCategories + kNamedManagerValues },
    { 312, Operand::Calls, 0, "__ZN27CRPGStats_Treasure_SubTableD1Ev" },
    { 364, Operand::Offset, kStatsTreasureCategories + kNamedManagerNextHandle },
    { 382, Operand::Offset, kStatsTreasureTables + kNamedManagerValues },
    { 386, Operand::Calls, 0, "__ZN24CRPGStats_Treasure_TableD1Ev" },
    { 438, Operand::Offset, kStatsTreasureTables + kNamedManagerNextHandle },
    { 456, Operand::Offset, kStatsItemTypes + kNamedManagerValues },
    { 460, Operand::Calls, 0, "__ZN12ItemTypeDataD2Ev" },
    { 512, Operand::Offset, kStatsItemTypes + kNamedManagerNextHandle },
    { 513, Operand::Offset, kStatsExtraData },
    { 560, Operand::Offset, kStatsFixedStrings + kArraySize },
    { 561, Operand::Offset, kStatsBloodTypes + kMapBucketCount },
    { 563, Operand::Offset, kStatsBloodTypes + kMapBuckets },
    { 752, Operand::Offset, kStatsBloodTypes + kMapItemCount },
    { 770, Operand::Offset, kStatsInt64s },
    { 788, Operand::Offset, kStatsInt64s + kArraySize },
    { 789, Operand::Offset, kStatsGuids + kArraySize },
    { 790, Operand::Offset, kStatsFloats + kArraySize },
    { 796, Operand::Offset, kStatsTranslatedStrings },
    { 798, Operand::Calls, 0, kTranslatedStringDestroySymbol },
    { 804, Operand::Offset, kStatsTranslatedStrings + kArraySize },
    { 805, Operand::Add, kStatsFunctors },
    { 806, Operand::Calls, 0, "__ZN3eoc23StatsFunctorListManager5ClearEv" },
    { 825, Operand::Offset, kStatsConditions },
    { 848, Operand::Offset, kStatsConditions + kArraySize },
    { 849, Operand::Offset, kStatsSpellSets },
    { 860, Operand::Offset, kStatsEquipmentSets },
    { 871, Operand::Offset, kStatsItemProgression },
    { 1221, Operand::Offset, kStatsItemCombinations },
    { 1232, Operand::Add, kStatsCurrentBuffer },
    { 1233, Operand::Calls, 0, kBufferResetSymbol },
    { 1234, Operand::Offset, kStatsCurrentBufferPath },
    { 1243, Operand::Calls, 0, kStringReleaseSymbol },
    { 1246, Operand::Offset, kStatsBufferMap + kMapBucketCount },
    { 1249, Operand::Offset, kStatsBufferMap + kMapBuckets },
    { 1299, Operand::Offset, kStatsBufferMap + kMapItemCount },
    { 1300, Operand::Add, kStatsBuffers },
    { 1301, Operand::Calls, 0, "__ZN2ls12DynamicArrayINS_9UniquePtrI19RPGStats_DataBuffervEENS_15TaggedAllocatorIiEEE16RemoveAllOrPurgeEm" },
};

// RPGStats::InsertDataBuffer(buffer, name) looks the name up in the map of
// buffers (bucket: the name's index modulo the bucket count; a node's key at
// +8, its 32-bit value at +0xc), and puts the buffer in the array at that
// index or appends it. A buffer's type is at +0x20 and the buffer it
// replaces is kept at +0x28.
//     ldr w9, [x2] ; ldr w10, [x0, #0x3c8] ; udiv ; msub ; ldr x10, [x0, #0x3d0] ; add x10, x10, w24, uxtw #3
//     ldr x11, [x11] ; cbz ; ldr w12, [x11, #0x8] ; cmp ; b.ne ; ldrsw x9, [x11, #0xc] ; ldr x10, [x20, #0x3e0]
//     ldr w10, [x9, #0x20] ; ldr w11, [x8, #0x20] ; ... ; ldr x10, [x8, #0x28] ; str x9, [x8, #0x28]
//     ldr w23, [x20, #0x3ec] ; ... ; add x0, x20, #0x3d8 ; bl (new node) ; ... ; ldr w8, [x20, #0x3e8]
constexpr Expect kStatsInsertBuffer[] = {
    { 13, Operand::Offset, kStatsBufferMap + kMapBucketCount },
    { 16, Operand::Offset, kStatsBufferMap + kMapBuckets },
    { 21, Operand::Offset, kMapNodeKey },
    { 24, Operand::Offset, kMapNodeIntValue },
    { 25, Operand::Offset, kStatsBuffers },
    { 28, Operand::Offset, kStatsBufferType },
    { 32, Operand::Offset, kStatsBufferNext },
    { 36, Operand::Calls, 0, kBufferResetSymbol },
    { 49, Operand::Offset, kStatsBuffers + kArraySize },
    { 57, Operand::Add, kStatsBufferMap + kMapItemCount },
    { 79, Operand::Offset, kMapNodeIntValue },
    { 85, Operand::Offset, kStatsBufferMap + kMapItemCount },
    { 89, Operand::Offset, kStatsBuffers + kArrayCapacity },
};

// RPGStats::FinishAndCleanCurrentParsedType() inserts the list of modifiers
// being parsed into the manager of lists and clears the pointer to it.
//     ldr x8, [x0, #0x268] ; ... ; ldr x9, [x21, #0x60]!
constexpr Expect kStatsFinishType[] = {
    { 9, Operand::Offset, kStatsCurrentType },
    { 12, Operand::Unscaled, kStatsModifierLists },
};

// RPGStats::AddCondition(ls::STDString const&) searches the conditions and appends.
//     ldr x23, [x0, #0x410] ; ldr w22, [x0, #0x41c]
constexpr Expect kStatsAddCondition[] = {
    { 10, Operand::Offset, kStatsConditions },
    { 11, Operand::Offset, kStatsConditions + kArraySize },
    { 58, Operand::Offset, kStatsConditions + kArrayCapacity },
};

// EocSharedSDM::Init builds the stats object in place: 0x420 bytes; the map
// of functors with 0x3f5 buckets; the extra data's pointer null; four maps of
// 0x25 buckets, 0x18 bytes apart; the map of counts with 0x65 buckets; the
// treasure rarities; the fixed strings.
//     mov w0, #0x420 ; bl malloc ; mov x20, x0
//     mov w8, #0x3f5 ; str w8, [x20, #0x248] ; ... ; str xzr, [x20, #0x2b0]
//     mov w8, #0x25 ; str w8, [x20, #0x2b8] ; str wzr, [x20, #0x2c8] ; ... ; str x19, [x20, #0x2c0]   (and at 0x2d0, 0x2e8, 0x300)
//     add x22, x20, #0x318 ; ... ; str x19, [x20, #0x320] ; mov w8, #0x65 ; str w8, [x20, #0x31c]
//     str x8, [x20, #0x328] ; ... ; str x8, [x20, #0x348]
constexpr Expect kStatsCreate[] = {
    { 6839, Operand::Move, kStatsSize },
    { 6840, Operand::CallsImport, 0, "_malloc" },
    { 6876, Operand::Offset, kStatsObjects + kNamedManagerNextHandle },
    { 6911, Operand::Offset, kStatsItemTypes + kNamedManagerSize },
    { 6912, Operand::Move, 0x3f5 },
    { 6913, Operand::Offset, kStatsFunctors + kMapBucketCount },
    { 6914, Operand::Offset, kStatsFunctors + kMapItemCount },
    { 6947, Operand::Offset, kStatsFunctors + kMapBuckets },
    { 6958, Operand::Offset, kStatsExtraData },
    { 6958, Operand::StoresZero },
    { 6963, Operand::Move, 0x25 },
    { 6964, Operand::Offset, kStatsCategoryMappings + kMapBucketCount },
    { 6965, Operand::Offset, kStatsCategoryMappings + kMapItemCount },
    { 6997, Operand::Offset, kStatsCategoryMappings + kMapBuckets },
    { 7007, Operand::Move, 0x25 },
    { 7008, Operand::Offset, kStatsWeaponCounters + kMapBucketCount },
    { 7042, Operand::Offset, kStatsWeaponCounters + kMapBuckets },
    { 7052, Operand::Move, 0x25 },
    { 7053, Operand::Offset, kStatsSpellbookCounters + kMapBucketCount },
    { 7087, Operand::Offset, kStatsSpellbookCounters + kMapBuckets },
    { 7097, Operand::Move, 0x25 },
    { 7098, Operand::Offset, kStatsArmorCounters + kMapBucketCount },
    { 7132, Operand::Offset, kStatsArmorCounters + kMapBuckets },
    { 7142, Operand::Add, kStatsRefMap },
    { 7176, Operand::Offset, kStatsRefMap + kRefMapBuckets },
    { 7185, Operand::Move, 0x65 },
    { 7186, Operand::Offset, kStatsRefMap + kRefMapBucketCount },
    { 7210, Operand::Offset, kStatsTreasureRarities },
    { 7260, Operand::Offset, kStatsFixedStrings },
};

// ls::LEGACY_Set<ls::FixedString, 0, ...>::Realloc(unsigned long), which the
// stats object's set of fixed strings grows through: the block is the
// elements and 16 bytes, which get the element's size and the count; the
// set's pointer is 16 bytes into the block; the old buffer's count is read
// 8 bytes before it and the old block freed from 16 bytes before it.
//     lsl x23, x19, #2 ; adds x9, x23, #0x10 ; ... ; bl malloc ; ... ; mov w8, #0x4 ; stp x8, x19, [x0] ; add x21, x0, #0x10
//     ... ; ldur x8, [x23, #-0x8] ; ... ; sub x0, x23, #0x10 ; bl free
constexpr Expect kFixedStringSetRealloc[] = {
    { 17, Operand::ShiftLeftBy, 2 },
    { 18, Operand::Add, kSetHeaderBytes },
    { 45, Operand::CallsImport, 0, "_malloc" },
    { 47, Operand::Move, sizeof(uint32_t) },
    { 48, Operand::PairOffset, kSetHeaderElementSize },
    { 49, Operand::Add, kSetHeaderBytes },
    { 113, Operand::Unscaled, (uint64_t)(int64_t)(-(int64_t)(kSetHeaderBytes - kSetHeaderCount)) },
    { 151, Operand::Add, kSetHeaderBytes },
    { 152, Operand::CallsImport, 0, "_free" },
};
// ls::LEGACY_Set<int, 4, ...>::Clone(...): a set of a type without a
// destructor has nothing before its elements.
//     lsl x0, x25, #2 ; bl malloc ; ... ; ldr x22, [x20] ; ... ; mov x0, x22 ; bl free ; str x21, [x20]
constexpr Expect kIntSetClone[] = {
    { 37, Operand::ShiftLeftBy, 2 },
    { 38, Operand::CallsImport, 0, "_malloc" },
    { 49, Operand::Offset, 0 },
    { 104, Operand::CallsImport, 0, "_free" },
    { 105, Operand::Offset, 0 },
};

// ---------------------------------------------------------------------------
// The spell, status, passive and interrupt prototypes and their managers
// (Layout/GameStats.h)

constexpr char kFixedStringArrayEnd[] = "__ZN2ls12DynamicArrayINS_11FixedStringENS_15TaggedAllocatorIiEEED2Ev";
constexpr char kFixedStringTableEnd[] = "__ZN2ls9HashTableINS_11FixedStringENS_19HashTableOpsDefaultEED2Ev";
constexpr char kDescriptionEnd[] = "__ZN3eoc16StatsDescriptionD2Ev";
constexpr char kSpellAnimationsEnd[] = "__ZN3eoc15SpellAnimationsD2Ev";
constexpr char kParseSpellAnimations[] = "__ZN3eoc14SpellPrototype20ParseSpellAnimationsERNS_15SpellAnimationsERKN2ls11FixedStringE";

// eoc::SpellAnimations::~SpellAnimations() releases the fixed strings from
// the last down, then the set at +0x40: its count is 8 bytes before its
// elements, an element is three fixed strings, and the block is freed from
// 16 bytes before the elements.
//     ldr w8, [x0, #0x70] ; ... ; ldr x21, [x19, #0x40] ; cbz ; ldur x8, [x21, #-0x8] ; cbz
//     add x8, x8, x8, lsl #1 ; lsl x22, x8, #2 ; ... ; sub x0, x21, #0x10 ; bl free
constexpr Expect kSpellAnimationsDestroy[] = {
    { 7, Operand::Offset, kSpellAnimationsLastName },
    { 89, Operand::Offset, kSpellAnimationsNames },
    { 91, Operand::Unscaled, (uint64_t)(int64_t)(-(int64_t)(kSetHeaderBytes - kSetHeaderCount)) },
    { 93, Operand::ShiftLeft, 1 },
    { 94, Operand::ShiftLeftBy, 2 },
    { 146, Operand::Add, kSetHeaderBytes },
    { 147, Operand::CallsImport, 0, "_free" },
};
static_assert(kAnimationNamesSize == 3 * 4);

// eoc::SpellPrototype::ParseSpellAnimations(animations, name) keeps the flags
// in the byte at +0x74 of the animations it fills.
//     mov x19, x1 ; ... ; strb w23, [x19, #0x74] ; ... ; ldrb w8, [x19, #0x74] ; ... ; strb w8, [x19, #0x74]
constexpr Expect kSpellAnimationsParse[] = {
    { 304, Operand::ByteOffset, kSpellAnimationsFlags },
    { 434, Operand::ByteOffset, kSpellAnimationsFlags },
    { 436, Operand::ByteOffset, kSpellAnimationsFlags },
};

// ls::LEGACY_Set<ls::AnimationSimpleNamesSet, 0, ...>::Realloc(count): the
// capacity is at +8, the size at +0xc, an element 12 bytes.
//     ldr w8, [x0, #0x8] ; cmp x8, x1 ; ... ; ldr x22, [x20] ; ldr w8, [x20, #0xc] ; ... ; add x23, x23, #0xc
constexpr Expect kAnimationNamesRealloc[] = {
    { 8, Operand::Offset, kSetCapacity },
    { 17, Operand::Offset, 0 },
    { 18, Operand::Offset, kSetSize },
    { 31, Operand::Offset, kSetSize },
    { 32, Operand::Add, kAnimationNamesSize },
};

// ls::NewInterface<ls::AnimationSimpleNamesSet>::Allocate(count): malloc of
// the elements and 16 bytes, which get the element's size and the count;
// the set's pointer is 16 bytes into the block.
//     mov w8, #0xc ; ... ; adds x9, x21, #0x10 ; ... ; bl malloc ; ... ; mov w9, #0xc ; stp x9, x19, [x0], #0x10
constexpr Expect kAnimationNamesAllocate[] = {
    { 5, Operand::Move, kAnimationNamesSize },
    { 11, Operand::Add, kSetHeaderBytes },
    { 39, Operand::CallsImport, 0, "_malloc" },
    { 42, Operand::Move, kAnimationNamesSize },
    { 43, Operand::PairOffset, kSetHeaderBytes },
};

// eoc::SpellPrototype::Init(name) keeps the name at +8 and fills the two
// animations, at +0x180 and +0x1f8.
//     str w21, [x28, #0x8] ; ... ; add x1, x28, #0x180 ; ... ; bl ParseSpellAnimations ; ... ; add x1, x28, #0x1f8 ; ... ; bl
constexpr Expect kSpellPrototypeInit[] = {
    { 31, Operand::Offset, kSpellName },
    { 1877, Operand::Add, kSpellAnimation },
    { 1881, Operand::Calls, 0, kParseSpellAnimations },
    { 1884, Operand::Add, kSpellDualWieldingAnimation },
    { 1888, Operand::Calls, 0, kParseSpellAnimations },
};

// eoc::SpellPrototypeManager::Terminate() clears the flag, then destroys
// every prototype of the map's values, member by member from the last.
//     strb wzr, [x0, #0x98] ; ldrsw x8, [x0, #0x34] ; ... ; ldr x25, [x19, #0x28] ; ... ; ldr x20, [x19, #0x38]
//     add x0, x21, #0x320 ; bl ~DynamicArray<ls::FixedString> ; ldr w8, [x21, #0x318] ; ...
constexpr Expect kSpellManagerTerminate[] = {
    { 9, Operand::ByteOffset, kSpellManagerInitialized },
    { 10, Operand::Offset, kSpellManagerSpells + kHashMapKeysSize },
    { 13, Operand::Offset, kSpellManagerSpells + kHashMapKeys },
    { 16, Operand::Offset, kSpellManagerSpells + kHashMapValues },
    { 37, Operand::Add, kSpellCombatAIOverrideSpells },
    { 38, Operand::Calls, 0, kFixedStringArrayEnd },
    { 39, Operand::Offset, kSpellCombatAIOverrideSpell },
    { 47, Operand::Offset, kSpellInterruptPrototype },
    { 55, Operand::Offset, kSpellItemWall },
    { 66, Operand::Add, kSpellMetaConditions },
    { 68, Operand::Calls, 0, "__ZN2ls7HashMapIN3eoc5spell20ESpellConditionsTypeENS_12DynamicArrayINS2_13MetaConditionENS_15TaggedAllocatorIiEEEENS_19HashTableOpsDefaultEE13DestroyValuesEv" },
    { 89, Operand::Offset, kSpellPathfindTemplate },
    { 99, Operand::Offset, kSpellTrajectories + kArrayCapacity },
    { 106, Operand::Offset, kSpellTrajectories },
    { 111, Operand::Add, kArrayBytes },
    { 175, Operand::Add, kSpellContainerSpells },
    { 176, Operand::Calls, 0, kFixedStringArrayEnd },
    { 177, Operand::Add, kSpellAlternativeCastTextEvents },
    { 178, Operand::Calls, 0, kFixedStringArrayEnd },
    { 179, Operand::Offset, kSpellCastSound },
    { 187, Operand::Offset, kSpellPrepareLoopSound },
    { 195, Operand::Offset, kSpellPrepareSound },
    { 203, Operand::Offset, kSpellPrepareEffect },
    { 211, Operand::Add, kSpellDualWieldingAnimation },
    { 212, Operand::Calls, 0, kSpellAnimationsEnd },
    { 213, Operand::Add, kSpellAnimation },
    { 214, Operand::Calls, 0, kSpellAnimationsEnd },
    { 215, Operand::Offset, kSpellVariableRitualCosts },
    { 234, Operand::Offset, kSpellVariableDualWieldingUseCosts },
    { 253, Operand::Offset, kSpellVariableUseCosts },
    { 272, Operand::Add, kSpellHitCostGroups },
    { 275, Operand::Calls, 0, "__ZN2ls12DynamicArrayIN3eoc15TextKeyHitCostsENS_15TaggedAllocatorIiEEE10ReallocateEi" },
    { 294, Operand::Add, kSpellDualWieldingUseCosts },
    { 316, Operand::Add, kSpellRitualCosts },
    { 338, Operand::Add, kSpellUseCosts },
    { 360, Operand::Offset, kSpellChildren },
    { 379, Operand::Add, kSpellDescription },
    { 380, Operand::Calls, 0, kDescriptionEnd },
    { 381, Operand::Offset, kSpellContainer },
    { 389, Operand::Offset, kSpellName },
};

// ~SpellPrototypeManager(): the names, the second map, the first.
//     bl Terminate ; add x0, x19, #0x88 ; bl ~DynamicArray<ls::FixedString> ; ldr x20, [x19, #0x78] ; ...
//     add x0, x19, #0x48 ; bl ~HashTable ; ldr x20, [x19, #0x38] ; ... ; add x0, x19, #0x8 ; bl ~HashTable
constexpr Expect kSpellManagerDestroy[] = {
    { 8, Operand::Calls, 0, "__ZN3eoc21SpellPrototypeManager9TerminateEv" },
    { 9, Operand::Add, kSpellManagerNames },
    { 10, Operand::Calls, 0, kFixedStringArrayEnd },
    { 11, Operand::Offset, kSpellManagerCombatAIOverrideSpells + kHashMapValues },
    { 36, Operand::Add, kSpellManagerCombatAIOverrideSpells },
    { 37, Operand::Calls, 0, kFixedStringTableEnd },
    { 38, Operand::Offset, kSpellManagerSpells + kHashMapValues },
    { 62, Operand::Add, kSpellManagerSpells },
    { 63, Operand::Calls, 0, kFixedStringTableEnd },
};

// eoc::StatusPrototypeManager::Terminate(): the flag, the map's values, and
// what each prototype owns.
//     strb wzr, [x0, #0x58] ; ldr x25, [x0, #0x38] ; ldr w8, [x0, #0x34] ; ... ; ldr x22, [x21, #0xb8] ; ...
//     add x22, x21, #0xd8 ; add x0, x21, #0xf8 ; bl ~DynamicArray<StatsSoundInfo> ; add x0, x21, #0xe8 ; bl ; ...
constexpr Expect kStatusManagerTerminate[] = {
    { 9, Operand::ByteOffset, kStatusManagerInitialized },
    { 10, Operand::Offset, kStatusManagerStatuses + kHashMapValues },
    { 11, Operand::Offset, kStatusManagerStatuses + kHashMapKeysSize },
    { 35, Operand::Offset, kStatusAbsorbSurfaceTypes },
    { 75, Operand::Offset, kStatusAbsorbSurfaceTypes },
    { 76, Operand::Add, kStatusSoundStart },
    { 77, Operand::Add, kStatusSoundStop },
    { 78, Operand::Calls, 0, "__ZN2ls12DynamicArrayIN3eoc5sound14StatsSoundInfoENS_15TaggedAllocatorIiEEED2Ev" },
    { 79, Operand::Add, kStatusSoundLoop },
    { 80, Operand::Calls, 0, "__ZN2ls12DynamicArrayIN3eoc5sound14StatsSoundInfoENS_15TaggedAllocatorIiEEED2Ev" },
    { 82, Operand::Calls, 0, "__ZN2ls12DynamicArrayIN3eoc5sound14StatsSoundInfoENS_15TaggedAllocatorIiEEED2Ev" },
    { 83, Operand::Offset, kStatusBoosts + kArrayCapacity },
    { 85, Operand::Offset, kStatusBoosts + kArraySize },
    { 89, Operand::Offset, kStatusBoosts },
    { 148, Operand::Add, kStatusDescription },
    { 149, Operand::Calls, 0, kDescriptionEnd },
    { 150, Operand::Offset, kStatusName },
};

// ~StatusPrototypeManager(): the names, then the map.
//     bl Terminate ; add x0, x19, #0x48 ; bl ~DynamicArray<ls::FixedString> ; ldr x20, [x19, #0x38] ; ... ; add x0, x19, #0x8 ; bl ~HashTable
constexpr Expect kStatusManagerDestroy[] = {
    { 8, Operand::Calls, 0, "__ZN3eoc22StatusPrototypeManager9TerminateEv" },
    { 9, Operand::Add, kStatusManagerNames },
    { 10, Operand::Calls, 0, kFixedStringArrayEnd },
    { 11, Operand::Offset, kStatusManagerStatuses + kHashMapValues },
    { 36, Operand::Add, kStatusManagerStatuses },
    { 37, Operand::Calls, 0, kFixedStringTableEnd },
};

// eoc::PassivePrototype::~PassivePrototype(), from the last member: the
// tooltip's fixed string, the boosts, the toggle group, the three functor
// lists (a manager of named elements each, its name at +0x5c), the effects,
// the description, the name.
//     bl Clean ; ldr w8, [x19, #0x20c] ; ... ; ldr w8, [x19, #0x200] ; ... ; ldr x20, [x19, #0x1f8] ; ... ; ldr w8, [x19, #0x1d8]
//     str x8, [x19, #0x178] ; ldr w8, [x19, #0x1d4] ; ... ; ldr x20, [x19, #0x1c0] ; ... ; add x0, x19, #0x190 ; bl ~HashTable
constexpr Expect kPassiveDestroy[] = {
    { 7, Operand::Calls, 0, "__ZN3eoc16PassivePrototype5CleanEv" },
    { 8, Operand::Offset, kPassiveTooltipConditionalDamage },
    { 18, Operand::Offset, kPassiveBoosts + kArrayCapacity },
    { 25, Operand::Offset, kPassiveBoosts },
    { 99, Operand::Offset, kPassiveToggleGroup },
    { 111, Operand::Offset, kPassiveToggleOffFunctors },
    { 112, Operand::Offset, kPassiveToggleOffFunctors + kFunctorListUniqueName },
    { 125, Operand::Offset, kPassiveToggleOffFunctors + kNamedManagerNames + kHashMapValues },
    { 149, Operand::Add, kPassiveToggleOffFunctors + kNamedManagerNames },
    { 150, Operand::Calls, 0, kFixedStringTableEnd },
    { 177, Operand::Offset, kPassiveToggleOnFunctors },
    { 241, Operand::Offset, kPassiveStatsFunctors },
    { 304, Operand::Offset, kPassiveToggleOffEffect },
    { 313, Operand::Offset, kPassiveToggleOnEffect },
    { 322, Operand::Add, kPassiveDescription },
    { 323, Operand::Calls, 0, kDescriptionEnd },
    { 324, Operand::Offset, kPassiveName },
};
static_assert(kPassiveToggleOnFunctors - kPassiveStatsFunctors == kNamedManagerSize
    && kPassiveToggleOffFunctors - kPassiveToggleOnFunctors == kNamedManagerSize);

// eoc::Passives::~Passives(): the flag after the map, then the map: its
// count, its buckets' count, its buckets.
//     strb wzr, [x0, #0x18] ; ldr w8, [x0, #0x8]! ; ... ; ldr w9, [x19, #0xc] ; ... ; ldr x8, [x19, #0x10]
constexpr Expect kPassivesDestroy[] = {
    { 12, Operand::ByteOffset, kPassivesInitialized },
    { 13, Operand::Unscaled, kPassivesMapHashSize - kRefMapBucketCount + kRefMapItemCount },
    { 16, Operand::Offset, kPassivesMapHashSize },
    { 19, Operand::Offset, kPassivesMapHashTable },
};

// eoc::InterruptPrototype::~InterruptPrototype(), from the last member: the
// three functor lists, the stack, the costs, the container, the
// description, the name.
//     str x8, [x19, #0x178] ; ldr w8, [x19, #0x1d4] ; ... ; add x0, x19, #0x190 ; bl ~HashTable ; ... ; str x8, [x19, #0x118]
//     ... ; str x8, [x19, #0xb8] ; ... ; ldr w8, [x19, #0xb0] ; ... ; add x20, x19, #0xa0 ; ... ; bl DynamicArray<ActionResourceCost>::Reallocate
constexpr Expect kInterruptDestroy[] = {
    { 164, Operand::Offset, kInterruptFailureFunctors },
    { 165, Operand::Offset, kInterruptFailureFunctors + kFunctorListUniqueName },
    { 203, Operand::Add, kInterruptFailureFunctors + kNamedManagerNames },
    { 204, Operand::Calls, 0, kFixedStringTableEnd },
    { 231, Operand::Offset, kInterruptSuccessFunctors },
    { 232, Operand::Offset, kInterruptSuccessFunctors + kFunctorListUniqueName },
    { 267, Operand::Add, kInterruptSuccessFunctors + kNamedManagerNames },
    { 295, Operand::Offset, kInterruptPropertiesFunctors },
    { 296, Operand::Offset, kInterruptPropertiesFunctors + kFunctorListUniqueName },
    { 331, Operand::Add, kInterruptPropertiesFunctors + kNamedManagerNames },
    { 358, Operand::Offset, kInterruptStack },
    { 367, Operand::Add, kInterruptCosts },
    { 370, Operand::Calls, 0, "__ZN2ls12DynamicArrayIN3eoc15action_resource18ActionResourceCostENS_15TaggedAllocatorIiEEE10ReallocateEi" },
    { 395, Operand::Offset, kInterruptContainer },
    { 404, Operand::Add, kInterruptDescription },
    { 405, Operand::Calls, 0, kDescriptionEnd },
    { 406, Operand::Offset, kInterruptName },
};

// ~InterruptPrototypeManager(): each prototype in the map's values, one
// prototype's size apart, then the map.
//     ldr x8, [x19, #0x38] ; ... ; bl ~InterruptPrototype ; ... ; add x20, x20, #0x1f0 ; ... ; add x21, x19, #0x8
constexpr Expect kInterruptManagerDestroy[] = {
    { 15, Operand::Offset, kInterruptManagerInterrupts + kHashMapValues },
    { 17, Operand::Calls, 0, "__ZN3eoc18InterruptPrototypeD1Ev" },
    { 20, Operand::Add, kInterruptSize },
    { 79, Operand::Add, kInterruptManagerInterrupts },
};

// A manager of named elements: CNamedElementManager<StatsObject, int, 0, -1>.
// GetByHandle(int): tbnz ; ldr w8, [x0, #0x58] ; cmp ; b.le ; ldr x8, [x0, #0x8] ; ldr x0, [x8, w1, uxtw #3]
constexpr Expect kNamedManagerByHandle[] = {
    { 1, Operand::Offset, kNamedManagerNextHandle },
    { 4, Operand::Offset, kNamedManagerValues },
};
// GetHandleByName(ls::FixedString const&): the bucket is the string's hash
// modulo the bucket count; the chain is followed through the next indices
// until the key is the name; the value at that index is the handle.
//     ldr w8, [x0, #0x20] ; cbz ; ... ; ldr w0, [x1] ; bl ls::FixedString::GetHash ; ldr w8, [x19, #0x20] ; udiv ; msub
//     ldr x9, [x19, #0x18] ; ldr w8, [x9, w8, sxtw #2] ; tbnz ; ldr x9, [x19, #0x38] ; ... ; ldr x11, [x19, #0x28]
//     ... ; ldr x9, [x19, #0x48] ; ldr w0, [x9, x8, lsl #2]
constexpr Expect kNamedManagerHandleByName[] = {
    { 3, Operand::Offset, kNamedManagerNames + kHashMapBucketCount },
    { 8, Operand::Calls, 0, "__ZNK2ls11FixedString7GetHashEv" },
    { 9, Operand::Offset, kNamedManagerNames + kHashMapBucketCount },
    { 12, Operand::Offset, kNamedManagerNames + kHashMapBuckets },
    { 15, Operand::Offset, kNamedManagerNames + kHashMapKeys },
    { 20, Operand::Offset, kNamedManagerNames + kHashMapNext },
    { 25, Operand::Offset, kNamedManagerNames + kHashMapValues },
};
// GetAmountOfEntries(): ldr w0, [x0, #0x14] ; ret
constexpr Expect kNamedManagerCount[] = {
    { 0, Operand::Offset, kNamedManagerValuesSize },
};
// UpdateHandles() sets every element's name to its index in the map of
// names: where the element keeps its name.
//     ldr w21, [x0, #0x14] ; ... ; add x20, x0, #0x18 ; ldr x8, [x19, #0x8] ; ldr x8, [x8, x22, lsl #3]
//     add x1, x8, #name ; ... ; bl ls::HashMap<ls::FixedString, int>::SetUniversal
constexpr Expect kObjectManagerUpdate[] = {
    { 5, Operand::Offset, kNamedManagerValuesSize },
    { 9, Operand::Add, kNamedManagerNames },
    { 10, Operand::Offset, kNamedManagerValues },
    { 12, Operand::Add, kStatsObjectName },
    { 16, Operand::Calls, 0, kNameMapSetSymbol },
};
constexpr Expect kModifierListManagerUpdate[] = {
    { 12, Operand::Add, kModifierListName },
    { 16, Operand::Calls, 0, kNameMapSetSymbol },
};
constexpr Expect kModifierManagerUpdate[] = {
    { 12, Operand::Add, kModifierName },
    { 16, Operand::Calls, 0, kNameMapSetSymbol },
};

// CRPGStats_Modifier_ValueList::Insert(ls::FixedString const&, int): the
// list's map of values follows its name.
//     ldr w8, [x1] ; ldr w9, [x0, #0x8] ; udiv ; msub ; ldr x9, [x0, #0x10] ; ...
//     ldr w10, [x9, #0x8] ; cmp ; b.ne ; str w20, [x9, #0xc] ; ... ; add x0, x19, #0x18
static_assert(kValueListName == 0);
constexpr Expect kValueListInsert[] = {
    { 10, Operand::Offset, kValueListValues + kMapBucketCount },
    { 13, Operand::Offset, kValueListValues + kMapBuckets },
    { 17, Operand::Offset, kMapNodeKey },
    { 20, Operand::Offset, kMapNodeIntValue },
    { 22, Operand::Add, kValueListValues + kMapItemCount },
};

// StatsObject::GetPosition(ls::FixedString const&) takes the object's list of
// modifiers from the stats object by the object's index of it, asks the list
// for the attribute's handle and reads the object's value at that index.
//     ldr w8, [x0, #0xe4] ; tbnz ; adrp x9, RPGStats::m_ptr ; ldr x9, [x9] ; ldr w10, [x9, #0xb8] ; cmp ; b.le
//     mov x19, x0 ; ldr x9, [x9, #0x68] ; ldr x0, [x9, x8, lsl #3] ; ... ; ldp x8, x9, [x19, #0x8] ; sub ; lsr x9, x9, #2
constexpr Expect kStatsObjectPosition[] = {
    { 3, Operand::Offset, kStatsObjectModifierList },
    { 7, Operand::Offset, kStatsModifierLists + kNamedManagerNextHandle },
    { 11, Operand::Offset, kStatsModifierLists + kNamedManagerValues },
    { 18, Operand::PairOffset, kStatsObjectProperties },
    { 20, Operand::ShiftRight, 2 },
};
bool CheckStatsObjectPosition(Code const& f, std::string& problem)
{
    uint64_t stats = f.Image.FindVA(kStatsSymbol);
    if (!stats || LoadedAddress(f, 5) != stats) {
        problem = "the object it reads is not the one RPGStats::m_ptr points to";
        return false;
    }
    return true;
}

// RPGStats::ParseDataBuffer(buffer) makes a stats object for each entry of
// the buffer: no flags, no parent, no list of modifiers, level 1. Where the
// entry is declared "using" another, the handle of that one is put in.
//     mov w8, #-0x1 ; str w8, [x20, #0xa8] ; ... ; movi.2d v0, #0xffffffffffffffff ; str d0, [x20, #0xe0]
//     mov w8, #0x1 ; str w8, [x20, #0xe8]
//     ldr x8, [sp, #0xf8] ; ldr x9, [sp, #0x10] ; str w9, [x8, #0xe0]
static_assert(kStatsObjectUsing + 4 == kStatsObjectModifierList);
constexpr Expect kStatsObjectParse[] = {
    { 226, Operand::Offset, kStatsObjectAIFlags },
    { 230, Operand::Offset, kStatsObjectUsing },
    { 231, Operand::Move, 1 },
    { 232, Operand::Offset, kStatsObjectLevel },
    { 1855, Operand::Offset, kStatsObjectUsing },
};

// CRPGStats_Treasure_Category_Manager::AddObject(name, categories, seven
// numbers) makes an item (0x24 bytes: no name, the numbers, a count of
// zero), then for each category named looks it up (the manager's virtual
// function at +0x30) or makes it (0x38 bytes: no name, the rest zero) and
// inserts it (+0x18), appends the item's pointer to the category's vector
// and adds one to the item's count.
//     mov w0, #0x24 ; bl malloc ; mov x20, x0 ; ... ; mov w8, #-0x1 ; str w8, [x20] ; stur xzr, [x20, #0xc]
//     stur xzr, [x20, #0x4] ; str wzr, [x20, #0x14] ; str d0, [x20, #0x18] ; strb wzr, [x20, #0x20]
//     stp w25, w8, [x20, #0xc] ; stp w24, w26, [x20, #0x4] ; stp w22, w21, [x20, #0x14] ; str w9, [x20, #0x1c]
//     mov w0, #0x38 ; bl malloc ; mov x24, x0 ; ... ; str w8, [x24] ; stur q0, [x24, #0x8] ; stur q0, [x24, #0x18]
//     stur q0, [x24, #0x28] ; ... ; ldp x25, x8, [x24, #0x10] ; cmp x25, x8 ; b.eq ; str x20, [x25], #0x8
//     str x25, [x24, #0x10] ; ... ; ldr x22, [x24, #0x8] ; ... ; stp x8, x9, [x24, #0x8] ; str x10, [x24, #0x18]
//     ldrb w8, [x20, #0x20] ; add w8, w8, #0x1 ; strb w8, [x20, #0x20]
constexpr Expect kTreasureCategoryAdd[] = {
    { 37, Operand::Move, kTreasureItemSize },
    { 38, Operand::CallsImport, 0, "_malloc" },
    { 50, Operand::Offset, kTreasureItemName },
    { 51, Operand::Unscaled, kTreasureItemNumbers + 8 },
    { 52, Operand::Unscaled, kTreasureItemNumbers },
    { 53, Operand::Offset, kTreasureItemNumbers + 0x10 },
    { 55, Operand::Offset, kTreasureItemNumbers + 0x14 },
    { 56, Operand::ByteOffset, kTreasureItemCategories },
    { 72, Operand::Offset, kTreasureItemName },
    { 74, Operand::PairOffset, kTreasureItemNumbers + 8 },
    { 75, Operand::PairOffset, kTreasureItemNumbers },
    { 76, Operand::PairOffset, kTreasureItemNumbers + 0x10 },
    { 77, Operand::Offset, kTreasureItemLastNumber },
    { 174, Operand::Move, kTreasureCategorySize },
    { 175, Operand::CallsImport, 0, "_malloc" },
    { 187, Operand::Offset, kTreasureCategoryName },
    { 189, Operand::Unscaled, kTreasureCategoryItems },
    { 190, Operand::Unscaled, kTreasureCategoryItems + 0x10 },
    { 191, Operand::Unscaled, kTreasureCategorySize - 0x10 },
    { 207, Operand::Offset, kTreasureCategoryName },
    { 213, Operand::PairOffset, kTreasureCategoryItems + 8 },
    { 216, Operand::Unscaled, 8 },
    { 217, Operand::Offset, kTreasureCategoryItems + 8 },
    { 219, Operand::Offset, kTreasureCategoryItems },
    { 276, Operand::PairOffset, kTreasureCategoryItems },
    { 277, Operand::Offset, kTreasureCategoryItems + 0x10 },
    { 299, Operand::ByteOffset, kTreasureItemCategories },
    { 300, Operand::Add, 1 },
    { 301, Operand::ByteOffset, kTreasureItemCategories },
};

// A category's destructor (the game's name for Norbyte's TreasureCategory is
// CRPGStats_Treasure_SubTable: it is what CRPGStats_Treasure_Category_Manager
// holds) takes one from the count of each item it lists and frees the items
// whose count is then not above zero, name first; then its two vectors and
// its own name.
//     ldp x22, x8, [x0, #0x8] ; ... ; ldr x21, [x22] ; cbz ; ldrb w8, [x21, #0x20] ; sub w8, w8, #0x1 ; sxtb w9, w8
//     strb w8, [x21, #0x20] ; cmp w9, #0x0 ; b.gt ; ldr w8, [x21] ; ... ls::gst::Map::Release ; ... ; bl free
constexpr Expect kTreasureCategoryDestroy[] = {
    { 9, Operand::PairOffset, kTreasureCategoryItems },
    { 26, Operand::CallsImport, 0, "_free" },
    { 28, Operand::Offset, kTreasureCategoryItems + 8 },
    { 34, Operand::ByteOffset, kTreasureItemCategories },
    { 35, Operand::Add, 1 },
    { 37, Operand::ByteOffset, kTreasureItemCategories },
    { 40, Operand::Offset, kTreasureItemName },
    { 47, Operand::Calls, 0, kStringReleaseSymbol },
};

// RPGStats::ParseTreasureFile(reader) makes a table (0x20 bytes: no name, no
// subtables), a subtable (0x58 bytes: zero, and no name at 0x54) and a
// subtable's category (0x28 bytes: index -1, the frequency read).
//     mov w0, #0x20 ; bl malloc ; ... ; str w8, [x20] ; stp xzr, xzr, [x20, #0x10]
//     mov w0, #0x58 ; bl malloc ; ... ; stur q0, [x20, #0x44] ; mov w8, #-0x1 ; str w8, [x20, #0x54]
//     mov w0, #0x28 ; bl malloc ; ... ; stp w8, w12, [x20]
constexpr Expect kTreasureTableParse[] = {
    { 163, Operand::Move, kTreasureTableSize },
    { 164, Operand::CallsImport, 0, "_malloc" },
    { 177, Operand::PairOffset, kTreasureTableSubTables },
    { 426, Operand::Move, kTreasureSubTableSize },
    { 427, Operand::CallsImport, 0, "_malloc" },
    { 445, Operand::Offset, kTreasureSubTableName },
    { 562, Operand::Move, kTreasureSubTableCategorySize },
    { 563, Operand::CallsImport, 0, "_malloc" },
    { 1891, Operand::Move, kTreasureSubTableCategorySize },
    { 1892, Operand::CallsImport, 0, "_malloc" },
};

// StatsObject::~StatsObject() destroys the members from the last to the first.
//     str w8, [x0, #0xe4] ; ldr x8, [x0, #0x8] ; str x8, [x0, #0x10] ; str wzr, [x0, #0xdc] ; str wzr, [x0, #0xcc]
//     ldr x20, [x0, #0xd0] ; ... ; ldr x20, [x19, #0xc0] ; ... ; ldr w8, [x19, #0xbc] ; ... ; ldr x20, [x19, #0xb0]
//     ldr w8, [x19, #0xa8] ; ... ls::gst::Map::Release ; ldr w8, [x19, #0x94] ; ldr x8, [x19, #0x98] ; (roll conditions)
//     add x21, x19, #0x68 ; ... ; ldr w8, [x19, #0x54] ; ldr x8, [x19, #0x58] ; (functors) ; add x21, x19, #0x28
//     ldr w8, [x19, #0x20] ; ... ls::gst::Map::Release ; ldr x20, [x19, #0x8]
constexpr Expect kStatsObjectDestroy[] = {
    { 12, Operand::Offset, kStatsObjectModifierList },
    { 13, Operand::Offset, kStatsObjectProperties },
    { 14, Operand::Offset, kStatsObjectProperties + 8 },
    { 15, Operand::Offset, kStatsObjectComboCategories + kArraySize },
    { 16, Operand::Offset, kStatsObjectComboProperties + kArraySize },
    { 17, Operand::Offset, kStatsObjectComboCategories },
    { 61, Operand::Offset, kStatsObjectComboProperties },
    { 105, Operand::Offset, kStatsObjectRequirements + kArraySize },
    { 109, Operand::Offset, kStatsObjectRequirements },
    { 183, Operand::Offset, kStatsObjectAIFlags },
    { 191, Operand::Calls, 0, kStringReleaseSymbol },
    { 192, Operand::Offset, kStatsObjectRollConditions + kHashMapKeysSize },
    { 197, Operand::Offset, kStatsObjectRollConditions + kHashMapValues },
    { 199, Operand::Calls, 0, "__ZN2ls12DynamicArrayI20TextKeyRollConditionNS_15TaggedAllocatorIiEEED2Ev" },
    { 226, Operand::Add, kStatsObjectRollConditions },
    { 232, Operand::Offset, kStatsObjectFunctors + kHashMapKeysSize },
    { 237, Operand::Offset, kStatsObjectFunctors + kHashMapValues },
    { 239, Operand::Calls, 0, "__ZN2ls12DynamicArrayI23TextKeyStatsFunctorListNS_15TaggedAllocatorIiEEED2Ev" },
    { 266, Operand::Add, kStatsObjectFunctors },
    { 272, Operand::Offset, kStatsObjectName },
    { 280, Operand::Calls, 0, kStringReleaseSymbol },
    { 281, Operand::Offset, kStatsObjectProperties },
};

// ---------------------------------------------------------------------------
// The client and server objects and the mod manager
// ---------------------------------------------------------------------------

constexpr char kClientSymbol[] = "__ZN3ecl9EocClient5m_ptrE";
constexpr char kServerSymbol[] = "__ZN3esv9EocServer5m_ptrE";
constexpr char kModuleDiffersSymbol[] = "__ZNK2ls6ModuleneERKS0_";
constexpr char kModuleDescriptionsAssignSymbol[] = "__ZN2ls12DynamicArrayINS_15ModuleShortDescENS_15TaggedAllocatorIiEEEaSERKS4_";

// Whether the function loads the object's address at instruction `adrp` from
// the global `symbol`: adrp x8, m_ptr@PAGE ; ldr x8, [x8, m_ptr@PAGEOFF]
bool LoadsGlobal(Code const& f, uint32_t adrp, char const* symbol, std::string& problem)
{
    uint64_t global = f.Image.FindVA(symbol);
    if (!global || LoadedAddress(f, adrp) != global) {
        problem = std::string("the object it reads is not the one ") + symbol + " points to";
        return false;
    }
    return true;
}
bool LoadsClient(Code const& f, std::string& problem) { return LoadsGlobal(f, 0, kClientSymbol, problem); }
bool LoadsServer(Code const& f, std::string& problem) { return LoadsGlobal(f, 0, kServerSymbol, problem); }

// App::IsInGame(bool) and App::IsInLobby(): adrp ; ldr x8, [m_ptr] ; ldr x8, [x8, #0x88] ; ldr x8, [x8] ; cbz ; ldr w9, [x8, #0x10]
constexpr Expect kClientStateMachineRead[] = {
    { 2, Operand::Offset, kClientGameStateMachine },
};
// Eoc::HasClient(): adrp ; ldr x8, [m_ptr] ; ldr x8, [x8, #0x90] ; cmp x8, #0 ; cset w0, ne
constexpr Expect kClientGameClientRead[] = {
    { 2, Operand::Offset, kClientGameClient },
};
// Eoc::ClientHasModule(): adrp ; ldr x8, [m_ptr] ; ldr x8, [x8, #0x138] ; add x0, x8, #0x38 ; ... ; b ls::Module::operator!=
constexpr Expect kClientModuleRead[] = {
    { 2, Operand::Offset, kClientModManager },
    { 3, Operand::Add, kModManagerBaseModule },
    { 6, Operand::JumpsTo, 0, kModuleDiffersSymbol },
};
// ecl::TLAnimation::GetWorld(): adrp ; ldr x8, [m_ptr] ; ldr x0, [x8, #0x1a0] ; ret
constexpr Expect kClientWorldRead[] = {
    { 2, Operand::Offset, kClientEntityWorld },
};
// ecl::RoomTrigger::GetUUID(): the same word is the world ecs::EntityWorld::GetComponent is called on.
//     adrp ; ldr x8, [m_ptr] ; ldr x8, [x8, #0x1a0] ; ldr x1, [x0, #0x70] ; mov x0, x8 ; b ecs::EntityWorld::GetComponent<ls::uuid::Component const, true>
constexpr char kGetUuidComponentSymbol[] =
    "__ZN3ecs11EntityWorld12GetComponentIKN2ls4uuid9ComponentELb1EEEPT_NS2_2IDINS_18EntityHandleTraitsEEENSt3__117integral_constantIbLb0EEE";
constexpr Expect kClientTriggerWorld[] = {
    { 2, Operand::Offset, kClientEntityWorld },
    { 5, Operand::JumpsTo, 0, kGetUuidComponentSymbol },
};

// Eoc::HasServer(): adrp ; ldr x8, [m_ptr] ; ldr x8, [x8, #0xa0] ; ldr w8, [x8, #0x10] ; cmp w8, #1 ; ...
constexpr Expect kServerStateMachineRead[] = {
    { 2, Operand::Offset, kServerGameStateMachine },
    { 3, Operand::Offset, kServerStateMachineState },
};
// esv::OsirisGameFunctions::GetUserCount: adrp ; ldr x8, [m_ptr] ; ldr x8, [x8, #0xa8] ; ldr w8, [x8, #0x614] ; ...
constexpr Expect kServerGameServerRead[] = {
    { 2, Operand::Offset, kServerGameServer },
    { 3, Operand::Offset, kGameServerUserIds + kArraySize },
};

// esv::GameServer::PostUpdate(ls::GameTime const&): add x0, x20, #0x3a8 ; bl ecs::sync::EntityReplicationAuthority::Sync
constexpr Expect kGameServerPostUpdate[] = {
    { 8, Operand::Add, kGameServerReplication },
    { 9, Operand::Calls, 0, "__ZN3ecs4sync26EntityReplicationAuthority4SyncERKN2ls8GameTimeE" },
};
// esv::GameServer::GetMasterUserID() const walks the peers' map: ldr w8, [x0, #0x694] ; cbz ; ldr x9, [x0, #0x698]
constexpr Expect kGameServerMasterUser[] = {
    { 0, Operand::Offset, kGameServerPeerInfo + kRefMapBucketCount },
    { 2, Operand::Offset, kGameServerPeerInfo + kRefMapBuckets },
};
// esv::GameServer::KickPeer(peer): looks the peer up among the connected ones and appends it to the kicked ones.
//     ldr x8, [x20, #0x5f8] ; ldr w10, [x20, #0x604]
constexpr Expect kGameServerKick[] = {
    { 14, Operand::Offset, kGameServerConnectedPeers },
    { 15, Operand::Offset, kGameServerConnectedPeers + kArraySize },
};
// esv::GameServer::DeactivatePeer(peer const&): ldr x8, [x20, #0x650] ; ldr w10, [x20, #0x65c]
constexpr Expect kGameServerDeactivate[] = {
    { 73, Operand::Offset, kGameServerActivePeers },
    { 74, Operand::Offset, kGameServerActivePeers + kArraySize },
};
// esv::GameServer::RequestDisconnectAll(bool) leaves out the local peer: ldr w8, [x19, #0x6b4] ; cmn w22, #1 ; ccmp w8, w22
constexpr Expect kGameServerDisconnectAll[] = {
    { 88, Operand::Offset, kGameServerLocalPeer },
};
// Eoc::ServerHasModule(): adrp ; ldr x8, [m_ptr] ; add x0, x8, #0x108 ; ... ; b ls::Module::operator!=
constexpr Expect kServerModuleRead[] = {
    { 2, Operand::Add, kServerModManager + kModManagerBaseModule },
    { 5, Operand::JumpsTo, 0, kModuleDiffersSymbol },
};
// esv::EoCDialogGameInterface::ReloadPathResolver() hands the server's mod manager to eoc::DialogPathResolver::Init(ls::ModManager*).
//     adrp ; ldr x8 ; add x0, x8, #0x38 ; adrp x8 ; ldr x8, [m_ptr] ; add x1, x8, #0xd0 ; b eoc::DialogPathResolver::Init
constexpr Expect kServerModManagerRead[] = {
    { 5, Operand::Add, kServerModManager },
    { 6, Operand::JumpsTo, 0, "__ZN3eoc18DialogPathResolver4InitEPN2ls10ModManagerE" },
};
bool LoadsServerLater(Code const& f, std::string& problem) { return LoadsGlobal(f, 3, kServerSymbol, problem); }
// esv::RoomTrigger::GetUUID(): as the client's, with the server's world at 0x288.
constexpr Expect kServerTriggerWorld[] = {
    { 2, Operand::Offset, kServerEntityWorld },
    { 5, Operand::JumpsTo, 0, kGetUuidComponentSymbol },
};

// ls::ModManager::BuildLoadOrderedModules() empties the load order and copies
// each module of the list it builds into it, member by member.
//     add x1, x19, #0x118 ; ... ; add x20, x19, #0x130 ; str wzr, [x19, #0x13c] ; ... ; mov w23, #0xf0
//     add x1, x27, #0x8 ; umaddl x8, w25, w23, x26 ; add x0, x8, #0x8 ; bl ls::ModuleInfo::operator=
//     add x1, x27, #0xc0 ; madd ; add x0, x8, #0xc0 ; bl ls::DynamicArray<ls::ModuleShortDesc>::operator=   (and 0xd0, 0xe0)
//     ldr w8, [x19, #0x13c] ; add w25, w8, #1 ; str w25, [x19, #0x13c] ; add x22, x22, #0xf0
constexpr Expect kModManagerBuildOrder[] = {
    { 12, Operand::Add, kModManagerBaseModule + kModuleAddons },
    { 16, Operand::Add, kModManagerLoadOrder },
    { 17, Operand::Offset, kModManagerLoadOrderSize },
    { 27, Operand::Move, kModuleSize },
    { 46, Operand::Add, kModuleInfo },
    { 47, Operand::Calls, 0, "__ZN2ls10ModuleInfoaSERKS0_" },
    { 51, Operand::Add, kModuleDependencies },
    { 52, Operand::Calls, 0, kModuleDescriptionsAssignSymbol },
    { 56, Operand::Add, kModuleConflicts },
    { 57, Operand::Calls, 0, kModuleDescriptionsAssignSymbol },
    { 60, Operand::Add, kModuleAddons },
    { 61, Operand::Calls, 0, kModuleDescriptionsAssignSymbol },
    { 64, Operand::Offset, kModManagerLoadOrderSize },
    { 65, Operand::Add, kModuleSize },
};

// ls::ModManager::GetDataPaths(ls::ObjectSet<ls::Path>&) makes a path of each
// module's directory, in load order.
//     ldr w8, [x20, #0x13c] ; ... ; add x23, x23, #0xf0 ; ... ; ldr x8, [x20, #0x130] ; add x8, x8, x23 ; add x2, x8, #0x68
//     ... ; bl ls::Path::Path<ls::STDString>(ls::EPathRoot, ls::STDString const&)
constexpr Expect kModManagerDataPaths[] = {
    { 17, Operand::Offset, kModManagerLoadOrderSize },
    { 42, Operand::Add, kModuleSize },
    { 45, Operand::Offset, kModManagerLoadOrder },
    { 47, Operand::Add, kModuleInfo + kModuleInfoDirectory },
    { 50, Operand::Calls, 0, "__ZN2ls4PathC2IJNS_9STDStringEEEENS_9EPathRootEDpRKT_" },
};

// ls::ModuleInfo::~ModuleInfo() destroys the members from the last to the
// first: the texts (the flag in the last byte, then the pointer), the
// scripts, the five fixed strings.
constexpr Expect kModuleInfoDestroy[] = {
    { 8, Operand::Calls, 0, "__ZN2ls14ScriptDataList10DeleteDataEv" },
    { 9, Operand::Offset, kModuleInfoDescription + kGameStringSize - 1 },
    { 12, Operand::Offset, kModuleInfoDescription },
    { 36, Operand::Offset, kModuleInfoAuthor + kGameStringSize - 1 },
    { 38, Operand::Offset, kModuleInfoAuthor },
    { 86, Operand::Offset, kModuleInfoScripts },
    { 87, Operand::Offset, kModuleInfoDirectory + kGameStringSize - 1 },
    { 89, Operand::Offset, kModuleInfoDirectory },
    { 113, Operand::Offset, kModuleInfoHash + kGameStringSize - 1 },
    { 115, Operand::Offset, kModuleInfoHash },
    { 139, Operand::Offset, kModuleInfoLevels + 0x10 },
    { 176, Operand::Offset, kModuleInfoLevels },
    { 185, Operand::Offset, kModuleInfoName + kGameStringSize - 1 },
    { 187, Operand::Offset, kModuleInfoName },
};

// ls::ModuleInfo::SetGUID(ls::FixedString const&) keeps the text and parses it into the UUID.
//     ... ; bl ls::gst::Get ; ... ; bl ls::Guid::ParseFromString ; stp x0, x1, [x19, #0x8]
constexpr Expect kModuleInfoSetGuid[] = {
    { 34, Operand::Calls, 0, "__ZN2ls4Guid15ParseFromStringERKNS_11_StringViewIcEE" },
    { 35, Operand::PairOffset, kModuleInfoGuid },
};

// ---------------------------------------------------------------------------
// The game's string, the path roots and the file reader
// ---------------------------------------------------------------------------

constexpr char kPathRootsSymbol[] = "__ZN2ls4Path7s_RootsE";

// ls::Path::Path<ls::STDString>(ls::EPathRoot, ls::STDString const&) joins a
// root's text and a string. The roots are an array of pointers to strings;
// a string's length is its last byte, or when that is negative the 32 bits
// at +8.
//     adrp x22, s_Roots@PAGE ; add x22, x22, s_Roots@PAGEOFF ; ldr x7, [x22, w1, uxtw #3]
//     ldrsb w8, [x7, #0xf] ; tbnz w8, #31 ; and w8, w8, #0xff ; b ; ldr w8, [x7, #0x8]
//     ldrb w9, [x20, #0xf] ; sxtb w10, w9 ; ldr w11, [x20, #0x8] ; cmp w10, #0 ; csel w9, w11, w9, lt
constexpr Expect kPathFromRoot[] = {
    { 11, Operand::Offset, kGameStringFlag },
    { 15, Operand::Offset, kGameStringLongLength },
    { 16, Operand::ByteOffset, kGameStringFlag },
    { 18, Operand::Offset, kGameStringLongLength },
    { 22, Operand::PairOffset, 0 },
    { 22, Operand::StoresZero },
};
bool CheckPathFromRoot(Code const& f, std::string& problem)
{
    uint64_t roots = f.Image.FindVA(kPathRootsSymbol);
    if (!roots || PageAddress(f.VA(8), f.At(8), f.At(9)) != roots) {
        problem = "the roots it indexes are not ls::Path::s_Roots";
        return false;
    }
    // ldr x7, [x22, w1, uxtw #3]: the root's number times the size of a pointer.
    if (f.At(10) != 0xf8615ac7) {
        problem = "a root is not taken as a pointer from an array of pointers";
        return false;
    }
    // ldr w8, [x7, #0x8] and ldr w11, [x20, #0x8] read 32 bits.
    if ((f.At(15) >> 30) != 2 || (f.At(18) >> 30) != 2) {
        problem = "a long string's length is not 32 bits";
        return false;
    }
    return true;
}

// ls::FileReader::Read(void*, unsigned long) copies from the next byte to
// read while the request fits before the end of the data, or reads from the
// file when the reader keeps none in memory.
//     ldr w8, [x0, #0x80] ; ... ; ldp x8, x9, [x20, #0x10] ; ldr x10, [x20, #0x8] ; sub x10, x10, x8 ; add x9, x10, x9
//     ... ; ldr x0, [x20, #0x88] ; ... ; b ls::File::Read
constexpr Expect kFileReaderReadBytes[] = {
    { 5, Operand::Offset, kFileReaderType },
    { 9, Operand::PairOffset, kFileReaderRead },
    { 10, Operand::Offset, kFileReaderData },
    { 25, Operand::Offset, kFileReaderFile },
    { 30, Operand::JumpsTo, 0, "__ZN2ls4File4ReadEPvm" },
};
static_assert(kFileReaderSize == kFileReaderRead + 8);

// ls::FileReader::Close(): stp xzr, xzr, [x0, #0x8] ; ldr x20, [x0, #0x88] ; ... ; ldr w8, [x19, #0x20] ; ...
//     str wzr, [x19, #0x20] ; str xzr, [x19, #0x18] ; strb wzr, [x19]
constexpr Expect kFileReaderClose[] = {
    { 6, Operand::PairOffset, kFileReaderData },
    { 6, Operand::StoresZero },
    { 7, Operand::Offset, kFileReaderFile },
    { 57, Operand::Offset, kFileReaderHandle },
    { 62, Operand::Offset, kFileReaderSize },
    { 63, Operand::ByteOffset, kFileReaderLoaded },
    { 63, Operand::StoresZero },
};

// ls::FileReader::LoadFile(ls::FileHandle, ls::FileReader::EType) ends a
// successful load with the byte that says so.
//     str w22, [x19, #0x80] ; stp xzr, xzr, [x19, #0x8] ; ... ; str x0, [x19, #0x18] ; ... ; strb w8, [x19]
constexpr Expect kFileReaderLoad[] = {
    { 71, Operand::Offset, kFileReaderType },
    { 72, Operand::PairOffset, kFileReaderData },
    { 81, Operand::Offset, kFileReaderSize },
    { 192, Operand::ByteOffset, kFileReaderLoaded },
};

// ---------------------------------------------------------------------------
// The game state event managers and their listeners
// ---------------------------------------------------------------------------

constexpr char kClientEventsSymbol[] = "__ZN3ecl21GameStateEventManager5m_ptrE";
constexpr char kServerEventsSymbol[] = "__ZN3esv21GameStateEventManager5m_ptrE";

// How a listener is told of a change of state is in two functions that are
// hooked or hold a hooked call by the time these proofs are made again in
// the game (esv::GameStateMachine::Update and App::Update): those facts are
// operands of the two managers' entries in the engine table, resolved before
// any hook. What is here is the array of listeners, from three functions
// nothing hooks.
//
// ecl::GameStateLoad::TickLoad(unsigned int, char const*) calls each of the
// client's listeners through the slot after the one for a change of state.
//     adrp x8, m_ptr ; ldr x8, [x8] ; ... ; ldr x19, [x8, #0x8] ; ldr w8, [x8, #0x14] ; ... ; ldr x8, [x8, #0x18] ; ... ; blr x8
constexpr Expect kClientTickLoad[] = {
    { 15, Operand::Offset, kStateEventListeners },
    { 16, Operand::Offset, kStateEventListenersSize },
    { 24, Operand::Offset, kStateListenerTickLoad },
};
bool CheckClientTickLoad(Code const& f, std::string& problem) { return LoadsGlobal(f, 9, kClientEventsSymbol, problem); }

// The destructor of a listener takes it out of the manager's array.
//     adrp x8, m_ptr ; ldr x9, [x8] ; ldr w10, [x9, #0x14] ; cbz ; ... ; ldr x8, [x9, #0x8]
constexpr Expect kStateListenerRemove[] = {
    { 6, Operand::Offset, kStateEventListenersSize },
    { 10, Operand::Offset, kStateEventListeners },
};
bool CheckClientListenerRemove(Code const& f, std::string& problem) { return LoadsGlobal(f, 4, kClientEventsSymbol, problem); }
bool CheckServerListenerRemove(Code const& f, std::string& problem) { return LoadsGlobal(f, 4, kServerEventsSymbol, problem); }

// ---------------------------------------------------------------------------
// ui::StateMachineComponent
// ---------------------------------------------------------------------------

constexpr char kLockSymbol[] = "__ZN2ls15CriticalSection4LockEv";

// ~StateMachineComponent() removes the states below the current one, clears
// the collection of state templates, and destroys the members from the last
// to the first.
//     ldr x1, [x19, #0x98] ; ... ; bl RemoveChilds
//     add x21, x19, #0x4b0 ; mov x0, x21 ; bl ls::CriticalSection::Lock        (the third lock)
//     ldr x0, [x19, #0x290] ; bl Noesis::BaseCollection::Clear
//     add x21, x19, #0x4c4 ; mov x0, x21 ; bl ls::CriticalSection::Lock        (the fourth)
//     ldr w1, [x19, #0x488] ; ... semaphore_destroy                            (the first)
//     add x21, x19, #0x478 ; ... ls::DynamicArray<ui::DeferredStateEvent>::Reallocate
//     ldrb w8, [x19, #0x277] ; ... ; ldr x0, [x19, #0x278] ; bl Noesis::Dealloc (the root state's text)
//     add x0, x19, #0x1e0 ; bl ls::HashTable<ui::State*>::~HashTable
//     add x0, x19, #0x1b0 ; bl ls::HashTable<ls::Guid>::~HashTable ; add x0, x19, #0x180 ; bl the same
//     ldr w8, [x19, #0x178] ; ... ; ldr x21, [x19, #0x170]
constexpr char kGuidTableDestroySymbol[] = "__ZN2ls9HashTableINS_4GuidENS_19HashTableOpsDefaultEED2Ev";
constexpr Expect kUiStatesDestroy[] = {
    { 77, Operand::Offset, kUiStatesState },
    { 99, Operand::Calls, 0, "__ZN2ui21StateMachineComponent12RemoveChildsEPNS_5StateERKN2ls4GuidE" },
    { 102, Operand::Add, kUiStatesLocks + 2 * sizeof(GameCriticalSection) },
    { 104, Operand::Calls, 0, kLockSymbol },
    { 165, Operand::Offset, kUiStatesStates },
    { 166, Operand::Calls, 0, "__ZN6Noesis14BaseCollection5ClearEv" },
    { 235, Operand::Add, kUiStatesLocks + 3 * sizeof(GameCriticalSection) },
    { 237, Operand::Calls, 0, kLockSymbol },
    { 487, Operand::Offset, kUiStatesLocks + offsetof(GameCriticalSection, Semaphore) },
    { 491, Operand::Add, kUiStatesDeferredEvents },
    { 494, Operand::Calls, 0, "__ZN2ls12DynamicArrayIN2ui18DeferredStateEventENS_15TaggedAllocatorIiEEE10ReallocateEi" },
    { 709, Operand::ByteOffset, kUiStatesRootState + 7 },
    { 711, Operand::Offset, kUiStatesRootState + 8 },
    { 712, Operand::Calls, 0, "__ZN6Noesis7DeallocEPv" },
    { 717, Operand::Add, kUiStatesStateSet },
    { 718, Operand::Calls, 0, "__ZN2ls9HashTableIPN2ui5StateENS_19HashTableOpsDefaultEED2Ev" },
    { 719, Operand::Add, kUiStatesExtraSets + kUiStatesSetBytes },
    { 720, Operand::Calls, 0, kGuidTableDestroySymbol },
    { 721, Operand::Add, kUiStatesExtraSets },
    { 722, Operand::Calls, 0, kGuidTableDestroySymbol },
    { 723, Operand::Offset, kUiStatesExtraArray + 8 },
    { 729, Operand::Offset, kUiStatesExtraArray },
};

// StaticFillClassType registers the component's two properties with Noesis
// by their offsets: "RootState" and "States".
//     ... ; mov w8, #0x270 ; str w8, [x20, #0x28] ; ... ; mov w8, #0x290 ; str w8, [x20, #0x28]
constexpr Expect kUiStatesProperties[] = {
    { 28, Operand::Calls, 0, "__ZN6Noesis13SymbolManager15AddStaticStringEPKc" },
    { 35, Operand::Move, kUiStatesRootState },
    { 62, Operand::Move, kUiStatesStates },
};
// The names the two offsets are registered under.
bool CheckUiStatesProperties(Code const& f, std::string& problem)
{
    auto first = f.Image.DataAt(PageAddress(f.VA(26), f.At(26), f.At(27)), sizeof("RootState"));
    auto second = f.Image.DataAt(PageAddress(f.VA(52), f.At(52), f.At(53)), sizeof("States"));
    if (!first || memcmp(first, "RootState", sizeof("RootState")) != 0 || !second || memcmp(second, "States", sizeof("States")) != 0) {
        problem = "the two properties it registers are not RootState and States";
        return false;
    }
    return true;
}

// The templates' StaticFillClassType register each member by its name and
// its offset: TypeClassCreator::Prop<T, the member's type>(name, offset)
// with the name in x1 and the offset in w2, or, for a collection, the
// property object made in place with the offset stored at +0x28.
struct NamedAt
{
    uint16_t Adrp;
    char const* Name;
};
bool NamesAt(Code const& f, std::span<NamedAt const> names, std::string& problem)
{
    for (auto const& named : names) {
        auto length = strlen(named.Name) + 1;
        auto text = f.Image.DataAt(PageAddress(f.VA(named.Adrp), f.At(named.Adrp), f.At(named.Adrp + 1)), length);
        if (!text || memcmp(text, named.Name, length) != 0) {
            problem = std::string("the member registered at instruction ") + std::to_string(named.Adrp) + " is not " + named.Name;
            return false;
        }
    }
    return true;
}

#define TEMPLATE_PROP(cls, type) "__ZN6Noesis16TypeClassCreator4PropIN2ui" cls type "EENS0_19TypePropertyCreatorEPKcMT_T0_"
#define TEMPLATE_STRING "ENS_11FixedStringILj24EEE"
constexpr char kStateTemplateString[] = TEMPLATE_PROP("13StateTemplate", TEMPLATE_STRING);
constexpr char kStateWidgetString[] = TEMPLATE_PROP("11StateWidget", TEMPLATE_STRING);
constexpr char kStateWidgetFlag[] = TEMPLATE_PROP("11StateWidget", "Eb");
constexpr char kStateWidgetLibraries[] = TEMPLATE_PROP("11StateWidget", "ENS_3PtrINS_10CollectionINS2_12StateLibraryEEEEE");
constexpr char kSubstateString[] = TEMPLATE_PROP("20StateInitialSubstate", TEMPLATE_STRING);
#undef TEMPLATE_STRING
#undef TEMPLATE_PROP

// ui::StateTemplate (his ui::UIState): thirteen strings from +0x10, the
// eighth and ninth registered as ForceSplitscreen at +0x110 and ForceVisible
// at +0xf0; three collections; ActivateOnRestore.
//     adrp x1, name ; add x1, x1, ... ; [mov x0, x19] ; mov w2, #offset ; bl Prop<StateTemplate, FixedString<24>>
constexpr Expect kUiStateTemplate[] = {
    { 9, Operand::Move, kUiTemplateFirstString }, { 10, Operand::Calls, 0, kStateTemplateString },
    { 14, Operand::Move, kUiTemplateFirstString + 1 * kUiTemplateString }, { 15, Operand::Calls, 0, kStateTemplateString },
    { 19, Operand::Move, kUiTemplateFirstString + 2 * kUiTemplateString }, { 20, Operand::Calls, 0, kStateTemplateString },
    { 24, Operand::Move, kUiTemplateFirstString + 3 * kUiTemplateString }, { 25, Operand::Calls, 0, kStateTemplateString },
    { 29, Operand::Move, kUiTemplateFirstString + 4 * kUiTemplateString }, { 30, Operand::Calls, 0, kStateTemplateString },
    { 34, Operand::Move, kUiTemplateFirstString + 5 * kUiTemplateString }, { 35, Operand::Calls, 0, kStateTemplateString },
    { 39, Operand::Move, kUiTemplateFirstString + 6 * kUiTemplateString }, { 40, Operand::Calls, 0, kStateTemplateString },
    { 44, Operand::Move, kUiTemplateForceSplitscreen }, { 45, Operand::Calls, 0, kStateTemplateString },
    { 49, Operand::Move, kUiTemplateForceVisible }, { 50, Operand::Calls, 0, kStateTemplateString },
    { 54, Operand::Move, kUiTemplateDisableSoundLayersAbove }, { 55, Operand::Calls, 0, kStateTemplateString },
    { 59, Operand::Move, kUiTemplateDisableSoundLayersAbove + 1 * kUiTemplateString }, { 60, Operand::Calls, 0, kStateTemplateString },
    { 64, Operand::Move, kUiTemplateDisableSoundLayersAbove + 2 * kUiTemplateString }, { 65, Operand::Calls, 0, kStateTemplateString },
    { 69, Operand::Move, kUiTemplateDisableSoundLayersAbove + 3 * kUiTemplateString }, { 70, Operand::Calls, 0, kStateTemplateString },
    { 100, Operand::Move, kUiTemplateInitialSubstates }, { 101, Operand::Offset, 0x28 },
    { 127, Operand::Move, kUiTemplateEvents }, { 128, Operand::Offset, 0x28 },
    { 154, Operand::Move, kUiTemplateWidgets }, { 155, Operand::Offset, 0x28 },
    { 160, Operand::Move, kUiTemplateActivateOnRestore }, { 165, Operand::JumpsTo, 0, kStateTemplateString },
};
static_assert(kUiTemplateFirstString + 7 * kUiTemplateString == kUiTemplateForceVisible
    && kUiTemplateForceVisible + kUiTemplateString == kUiTemplateForceSplitscreen
    && kUiTemplateForceSplitscreen + kUiTemplateString == kUiTemplateDisableSoundLayersAbove
    && kUiTemplateDisableSoundLayersAbove + 4 * kUiTemplateString == kUiTemplateInitialSubstates);
constexpr NamedAt kUiStateTemplateNames[] = {
    { 7, "Name" }, { 11, "Layout" }, { 16, "Owner" }, { 21, "DisableStatesBelow" }, { 26, "HideStatesBelow" }, { 31, "IsModal" },
    { 36, "IsModalCameraControlAllowed" }, { 41, "ForceSplitscreen" }, { 46, "ForceVisible" }, { 51, "DisableSoundLayersAbove" },
    { 56, "TutorialLayer" }, { 61, "ModType" }, { 66, "IsModdable" }, { 90, "InitialSubstates" }, { 117, "Events" }, { 144, "Widgets" },
    { 157, "ActivateOnRestore" },
};
bool CheckUiStateTemplate(Code const& f, std::string& problem)
{
    return NamesAt(f, kUiStateTemplateNames, problem);
}

// ui::StateWidget (his ui::UIStateWidget): three strings, then (after a
// Uri) IgnoreHitTest, BlockedLoading and AllowCreationDelay in bytes, and
// the libraries.
constexpr Expect kUiStateWidget[] = {
    { 6, Operand::Move, kUiTemplateFirstString }, { 7, Operand::Calls, 0, kStateWidgetString },
    { 11, Operand::Move, kUiTemplateFirstString + kUiTemplateString }, { 12, Operand::Calls, 0, kStateWidgetString },
    { 16, Operand::Move, kUiTemplateFirstString + 2 * kUiTemplateString }, { 17, Operand::Calls, 0, kStateWidgetString },
    { 21, Operand::Move, kUiWidgetTemplateIgnoreHitTest }, { 22, Operand::Calls, 0, kStateWidgetFlag },
    { 26, Operand::Move, kUiWidgetTemplateBlockedLoading }, { 27, Operand::Calls, 0, kStateWidgetFlag },
    { 31, Operand::Move, kUiWidgetTemplateLibraries }, { 32, Operand::Calls, 0, kStateWidgetLibraries },
    { 36, Operand::Move, kUiWidgetTemplateAllowCreationDelay }, { 39, Operand::JumpsTo, 0, kStateWidgetFlag },
};
constexpr NamedAt kUiStateWidgetNames[] = {
    { 4, "Filename" }, { 8, "Layer" }, { 13, "SoundLayer" }, { 18, "IgnoreHitTest" }, { 23, "BlockedLoading" }, { 28, "Libraries" },
    { 33, "AllowCreationDelay" },
};
bool CheckUiStateWidget(Code const& f, std::string& problem)
{
    return NamesAt(f, kUiStateWidgetNames, problem);
}

// ui::StateEvent (his ui::UIStateEvent): its name, then HandlesEvent at
// +0x30 as a bool property (the vtable of TypePropertyOffset<bool>), and its
// actions at +0x38.
constexpr Expect kUiStateEvent[] = {
    { 35, Operand::Move, kUiTemplateFirstString }, { 36, Operand::Offset, 0x28 },
    { 61, Operand::Move, kUiEventHandlesEvent }, { 62, Operand::Offset, 0x28 },
    { 88, Operand::Move, kUiEventActions },
};
constexpr NamedAt kUiStateEventNames[] = { { 26, "Name" }, { 52, "HandlesEvent" }, { 78, "Actions" } };
bool CheckUiStateEvent(Code const& f, std::string& problem)
{
    if (!NamesAt(f, kUiStateEventNames, problem)) return false;
    auto flag = f.Image.FindVA("__ZTVN6Noesis18TypePropertyOffsetIbEE");
    if (!flag || PageAddress(f.VA(58), f.At(58), f.At(59)) != flag) {
        problem = "HandlesEvent is not registered as a bool";
        return false;
    }
    return true;
}

// ui::StateInitialSubstate (his ui::UIInitialSubstate): two strings.
constexpr Expect kUiStateSubstate[] = {
    { 6, Operand::Move, kUiTemplateFirstString }, { 7, Operand::Calls, 0, kSubstateString },
    { 11, Operand::Move, kUiTemplateFirstString + kUiTemplateString }, { 14, Operand::JumpsTo, 0, kSubstateString },
};
constexpr NamedAt kUiStateSubstateNames[] = { { 4, "Name" }, { 8, "MetaData" } };
bool CheckUiStateSubstate(Code const& f, std::string& problem)
{
    return NamesAt(f, kUiStateSubstateNames, problem);
}

// AllowEventProcessing(bool) takes the second lock and walks the deferred events.
//     add x19, x0, #0x49c ; mov x0, x19 ; bl ls::CriticalSection::Lock ; ... ; ldr w8, [x20, #0x484] ; ... ; ldr x27, [x20, #0x478]
constexpr Expect kUiStatesAllowEvents[] = {
    { 10, Operand::Add, kUiStatesLocks + sizeof(GameCriticalSection) },
    { 12, Operand::Calls, 0, kLockSymbol },
    { 14, Operand::Offset, kUiStatesDeferredEvents + kArraySize },
    { 28, Operand::Offset, kUiStatesDeferredEvents },
};

// CreateState(short const&, ...) compares the player it is given with the component's.
//     ldrh w8, [x20] ; ldrh w9, [x21, #0x470] ; cmp w8, w9
constexpr Expect kUiStatesCreateState[] = {
    { 193, Operand::Offset, kUiStatesPlayer },
};

// ---------------------------------------------------------------------------
// The channels of a timeline parameter
// ---------------------------------------------------------------------------
// Channels() of a parameter's channel container takes the container alone
// and returns two addresses in it, the first channel's and the one past the
// last, in x0 and x1: add x8, x0, #first ; add x1, x0, #end ; mov x0, x8 ; ret.
// The function's shape says the rest: nothing is read from x1 and nothing
// is stored.
constexpr size_t kChannelBytes = sizeof(void*);
constexpr Expect kMaterialChannels[] = {
    { 0, Operand::Add, 0x18 },
    { 1, Operand::Add, 0x18 + 1 * kChannelBytes },
};
constexpr Expect kMaterialChannels3[] = {
    { 0, Operand::Add, 0x18 },
    { 1, Operand::Add, 0x18 + 3 * kChannelBytes },
};
constexpr Expect kTextureChannels[] = {
    { 0, Operand::Add, 0x20 },
    { 1, Operand::Add, 0x20 + 1 * kChannelBytes },
};
constexpr Expect kReflectionChannels[] = {
    { 0, Operand::Add, 0x58 },
    { 1, Operand::Add, 0x58 + 1 * kChannelBytes },
};
// mov x0, x8 ; ret
bool CheckChannelsReturn(Code const& f, std::string& problem)
{
    if (f.Count() != 4 || f.At(2) != 0xaa0803e0 || f.At(3) != 0xd65f03c0 || RegD(f.At(0)) != 8 || RegD(f.At(1)) != 1 || RegN(f.At(0)) != 0
        || RegN(f.At(1)) != 0) {
        problem = "it does not return two addresses in the container in x0 and x1";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The results of virtual functions (Layout/GameVirtualResults.h)
// ---------------------------------------------------------------------------

// The sound manager's functions that make an id return it in w0 (a bank's
// or a name's 32-bit id) or x0 (a sound object's 64-bit id): each takes the
// name in x1, after the manager, so there is no place for the id among the
// arguments and x8 is not one.
constexpr uint32_t kKeepNameInX19 = 0xaa0103f3;     // mov x19, x1
constexpr uint32_t kNoId = 0x12800000;              // mov w0, #-1
// ww::WwiseManager::LoadBank_Blocking(name): AK::SoundEngine::LoadBank's id
// read back from the stack into w0, or -1.
//     mov x19, x1 ; str wzr, [sp, #0xc] ; ... ; ldr w0, [sp, #0xc] ; ... ; mov w0, #-1
constexpr Expect kLoadBankResult[] = {
    { 15, Operand::Calls, 0, "__ZN2AK11SoundEngine11PrepareBankENS0_15PreparationTypeEPKcNS0_13AkBankContentEj" },
    { 21, Operand::Calls, 0, "__ZN2AK11SoundEngine15GetIDFromStringEPKc" },
    { 26, Operand::Calls, 0, "__ZN2AK11SoundEngine8LoadBankEPKcRjj" },
};
bool CheckLoadBankResult(Code const& f, std::string& problem)
{
    constexpr uint32_t kReadId = 0xb9400fe0;        // ldr w0, [sp, #0xc]
    if (f.At(4) != kKeepNameInX19 || f.At(18) != kReadId || f.At(29) != kNoId) {
        problem = "the bank's name is not taken from x1, or its id not returned in w0";
        return false;
    }
    return true;
}
// ww::WwiseManager::PrepareBank_Blocking(name): the name's id, or -1.
//     mov x19, x1 ; ... ; bl AK::SoundEngine::PrepareBank ; ... ; mov x0, x19 ; ... ; b AK::SoundEngine::GetIDFromString ; mov w0, #-1
constexpr Expect kPrepareBankResult[] = {
    { 7, Operand::Calls, 0, "__ZN2AK11SoundEngine11PrepareBankENS0_15PreparationTypeEPKcNS0_13AkBankContentEj" },
    { 13, Operand::JumpsTo, 0, "__ZN2AK11SoundEngine15GetIDFromStringEPKc" },
};
bool CheckPrepareBankResult(Code const& f, std::string& problem)
{
    if (f.At(3) != kKeepNameInX19 || f.At(14) != kNoId) {
        problem = "the bank's name is not taken from x1, or its id not returned in w0";
        return false;
    }
    return true;
}
// ww::WwiseManager::RegisterSoundObjectInternal(name): takes a free id,
// registers it with AK::SoundEngine::RegisterGameObj and returns it in x0.
//     mov x20, x1 ; mov x19, x0 ; ... ; ldr x21, [x23, x9, lsl #3] ; ... ; mov x0, x21 ; mov x1, x20 ; bl RegisterGameObj ; ... ; mov x0, x21 ; ... ; ret
constexpr Expect kRegisterSoundObjectResult[] = {
    { 30, Operand::Calls, 0, "__ZN2AK11SoundEngine15RegisterGameObjEyPKc" },
};
bool CheckRegisterSoundObjectResult(Code const& f, std::string& problem)
{
    constexpr uint32_t kKeepName = 0xaa0103f4, kKeepThis = 0xaa0003f3, kReturnId = 0xaa1503e0;  // mov x20, x1 ; mov x19, x0 ; mov x0, x21
    if (f.At(9) != kKeepName || f.At(10) != kKeepThis || f.At(518) != kReturnId) {
        problem = "the object's name is not taken from x1, or its id not returned in x0";
        return false;
    }
    return true;
}
// ww::WwiseManager::GetShortID(name): AK::SoundEngine::GetIDFromString of
// the name, its 32-bit result left in w0.
//     mov x0, x1 ; b AK::SoundEngine::GetIDFromString
constexpr Expect kShortIdResult[] = {
    { 1, Operand::JumpsTo, 0, "__ZN2AK11SoundEngine15GetIDFromStringEPKc" },
};
bool CheckShortIdResult(Code const& f, std::string& problem)
{
    if (f.At(0) != 0xaa0103e0) {    // mov x0, x1
        problem = "the name is not taken from x1";
        return false;
    }
    return true;
}

// eoc::DynamicStatsExpressionManager::CreateStatsExpression keeps the address
// in x8 as where its result goes, allocates the expression, starts its count
// and stores the pointer there:
//     mov x19, x8 ; ... ; mov w0, #0x28 ; bl malloc ; ... ; mov w8, #1 ; str w8, [x21, #0x20] ; str x21, [x19]
constexpr Expect kCreateStatsExpression[] = {
    { 28, Operand::Move, kStatsExpressionSize },
    { 29, Operand::CallsImport, 0, "_malloc" },
    { 42, Operand::Move, kStatsExpressionFirstCount },
    { 43, Operand::Offset, kStatsExpressionRefCount },
    { 44, Operand::Offset, 0 },
};
bool CheckCreateStatsExpression(Code const& f, std::string& problem)
{
    // mov x19, x8 before x8 is written, and the pointer stored through x19.
    constexpr uint32_t kKeepResultAddress = 0xaa0803f3;
    for (uint32_t n = 0; n < 6; n++) {
        if (RegD(f.At(n)) == 8) {
            problem = "x8 is written before it is kept";
            return false;
        }
    }
    if (f.At(6) != kKeepResultAddress || RegN(f.At(44)) != 19 || RegD(f.At(44)) != RegD(f.At(30))) {
        problem = "it does not return the expression through the address in x8";
        return false;
    }
    return true;
}

// eoc::spell_cast::BaseTarget::LEGACY_GetTargetPosition(ecs::EntityWorld&):
// the entity's position comes back from a function returning an
// ls::Optional<Vector3f> in x0 and x1, is tested by the byte after the three
// floats, and is handed on in s0, s1 and s2:
//     tst x1, #0xff00000000 ; b.eq ... ; fmov s0, w0 ; lsr x8, x0, #32 ; fmov s1, w8 ; fmov s2, w1
constexpr Expect kLegacyTargetPosition[] = {
    { 52, Operand::Mask, kOptionalPositionEngagedMask },
};
bool CheckLegacyTargetPosition(Code const& f, std::string& problem)
{
    constexpr uint32_t kFloats[] = { 0x1e270000, 0xd360fc08, 0x1e270101, 0x1e270022 };
    for (uint32_t n = 0; n < std::size(kFloats); n++) {
        if (f.At(54 + n) != kFloats[n]) {
            problem = "it does not hand the position on in s0, s1 and s2";
            return false;
        }
    }
    return true;
}

// eoc::spell_cast::BaseTarget::GetTargetPosition(..., Vector3f const*,
// ls::Optional<ESpellTargetingType>): the targeting type is tested by its
// second byte in x3, and the position comes back in s0, s1 and s2.
//     tst x3, #0xff00 ; ... ; ldp s0, s1, [x8, #0x20] ; ldr s2, [x8, #0x28]
constexpr Expect kTargetPosition[] = {
    { 5, Operand::Mask, kOptionalTargetingEngagedMask },
    { 61, Operand::Mask, kOptionalPositionEngagedMask },
};
bool CheckTargetPosition(Code const& f, std::string& problem)
{
    constexpr uint32_t kLoadFloats[] = { 0x2d440500, 0xbd402902 };
    constexpr uint32_t kFloats[] = { 0x1e270000, 0xd360fc08, 0x1e270101, 0x1e270022 };
    bool loads = f.At(9) == kLoadFloats[0] && f.At(10) == kLoadFloats[1];
    for (uint32_t n = 0; loads && n < std::size(kFloats); n++) loads = f.At(63 + n) == kFloats[n];
    if (!loads || RegN(f.At(5)) != 3) {
        problem = "it does not take the targeting type in x3 or hand the position on in s0, s1 and s2";
        return false;
    }
    return true;
}

// gn::TGenomeSet<float>::GetValueTypeHash() loads its 32 bits into w0.
//     ldr w0, [x19]
bool CheckGenomeTypeHash(Code const& f, std::string& problem)
{
    constexpr uint32_t kLoadWord = 0xb9400260;
    static_assert(kGenomeTypeHashSize == 4);
    if (f.At(10) != kLoadWord) {
        problem = "it does not return 32 bits in w0";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ww::WwiseManager (Layout/GameAudio.h)
// ---------------------------------------------------------------------------

// The constructor zeroes the 0x40 bytes after the vtable and starts the
// seven arrays of sound objects (+0x48 to +0x128) and the music object
// (+0x128) at -1.
//     str x8, [x0] ; movi.2d v1, #-1 ; mov x27, x0 ; str q1, [x27, #0x48]! ; movi.2d v0, #0 ; stur q0, [x0, #0x8]
//     stur q0, [x0, #0x18] ; stur q0, [x0, #0x28] ; stur q0, [x0, #0x38] ; stur q1, [x0, #0x58] ... stur q1, [x0, #0xf8]
//     mov x8, #-1 ; str x8, [x0, #0x128] ; stp q1, q1, [x27, #0xc0]
constexpr Expect kSoundObjectsCreate[] = {
    { 11, Operand::Offset, 0 },
    { 14, Operand::Unscaled, kSoundObjects },
    { 16, Operand::Unscaled, 0x08 },
    { 17, Operand::Unscaled, 0x18 },
    { 18, Operand::Unscaled, 0x28 },
    { 19, Operand::Unscaled, 0x38 },
    { 20, Operand::Unscaled, kSoundObjects + 0x10 },
    { 30, Operand::Unscaled, kSoundObjects + 0xb0 },
    { 32, Operand::Offset, kSoundMusic },
};
static_assert(kSoundObjects + kSoundObjectArrays * kSoundObjectArray == kSoundMusic && kSoundObjectArray == kSoundPlayers * sizeof(uint64_t)
    && kSoundListeners == kSoundObjects + kSoundObjectArray && kSoundActiveListeners == kSoundMusic + sizeof(uint64_t));
// ActivateListener(player): unless the player's listener is active (the
// byte at +0x130), it makes the music object (+0x128) listen to it for the
// first player, the HUD, ambient and first arrays' objects (+0xe8, +0xc8,
// +0x48) for each, and adds the player's listener (+0x68) as a default
// listener. DeactivateListener removes that default listener.
//     add x8, x0, x21 ; ldrb w9, [x8, #0x130] ; ... ; ldr x0, [x19, #0x128] ; add x8, x19, x21, lsl #3 ; add x1, x8, #0x68
//     bl SetListeners ; ... ; ldr x0, [x21, #0xe8] ; add x19, x21, #0x68 ; ... ; ldr x0, [x21, #0xc8] ; ... ; ldr x0, [x21, #0x68]
//     bl AddDefaultListener ; ldr x0, [x21, #0x48]
// ls::SoundManager::StopAllHUDSounds(player), StopAllCineHUDSounds and
// StopAllAmbientSounds stop the player's object of the sixth, seventh and
// fifth array through the manager's slot 29 (StopSounds), and StopAllMusicSounds
// the music object.
//     add x8, x19, w1, sxtw #3 ; ldr x1, [x8, #0xe8] ; ldr x8, [x19] ; ldr x3, [x8, #0xe8] ; ... ; br x3
constexpr Expect kStopHud[] = {
    { 7, Operand::Offset, kSoundHud },
    { 9, Operand::Offset, kStopSoundsSlot * sizeof(void*) },
};
constexpr Expect kStopCineHud[] = {
    { 7, Operand::Offset, kSoundCineHud },
    { 9, Operand::Offset, kStopSoundsSlot * sizeof(void*) },
};
constexpr Expect kStopAmbient[] = {
    { 7, Operand::Offset, kSoundAmbient },
    { 9, Operand::Offset, kStopSoundsSlot * sizeof(void*) },
};
//     ldr x1, [x0, #0x128] ; ldr x8, [x0] ; ldr x3, [x8, #0xe8] ; mov w2, #0x7d0 ; br x3
constexpr Expect kStopMusic[] = {
    { 0, Operand::Offset, kSoundMusic },
    { 2, Operand::Offset, kStopSoundsSlot * sizeof(void*) },
};
static_assert(kSoundAmbient == kSoundObjects + 4 * kSoundObjectArray && kSoundHud == kSoundObjects + 5 * kSoundObjectArray
    && kSoundCineHud == kSoundObjects + 6 * kSoundObjectArray);
constexpr Expect kSoundActivate[] = {
    { 9, Operand::ByteOffset, kSoundActiveListeners },
    { 13, Operand::Offset, kSoundMusic },
    { 14, Operand::ShiftLeft, 3 },
    { 15, Operand::Add, kSoundListeners },
    { 17, Operand::Calls, 0, "__ZN2AK11SoundEngine12SetListenersEyPKyj" },
    { 19, Operand::Offset, kSoundHud },
    { 20, Operand::Add, kSoundListeners },
    { 25, Operand::Offset, kSoundAmbient },
    { 29, Operand::Offset, kSoundListeners },
    { 30, Operand::Calls, 0, "__ZN2AK11SoundEngine18AddDefaultListenerEy" },
    { 31, Operand::Offset, kSoundObjects },
};
//     ... ; ldrb w9, [x8, #0x130] ; ... ; ldr x0, [x8, #0x68] ; bl AK::SoundEngine::RemoveDefaultListener
constexpr Expect kSoundDeactivate[] = {
    { 9, Operand::ByteOffset, kSoundActiveListeners },
    { 13, Operand::Offset, kSoundListeners },
    { 14, Operand::Calls, 0, "__ZN2AK11SoundEngine21RemoveDefaultListenerEy" },
};

// App::InitGame() registers each player's sound objects through the
// manager's slot 53 under a name made from a format, and stores the ids it
// returns from the second array's slot of the player on (+0x88 + 8 * player
// - 0x20 is the listener's): the listener, the rumble listener at +0x20
// past it, the emitter at -0x20, the controller speaker's listener at +0x40,
// the ambient, HUD and cinematic HUD objects at +0x60, +0x80, +0xa0; then the
// music object, at +0x128.
//     mov x0, x22 ; bl ls::SoundManager::HandleInitBankLoading(bool) ; ... ; add x21, x22, #0x88 ; ... ; adrp x23 ; add x23, "Listener%d" ...
//     bl lssprintf ; ldr x8, [x22] ; ldr x8, [x8, #0x1a8] ; ... ; blr x8 ; stur x0, [x21, #-0x20] ; ... ; str x0, [x21] ; ...
//     stur x0, [x21, #-0x40] ; ... ; str x0, [x21, #0x80] ; add x20, x20, #1 ; add x21, x21, #8 ; cmp x20, #4 ; ...
//     ldr x8, [x8, #0x1a8] ; adrp x1 ; add x1, "Music" ; ... ; blr x8 ; ... ; str x0, [x22, #0x128]
constexpr uint64_t kSoundObjectNamed = kSoundRumbleListeners;
constexpr Expect kSoundObjectsNamed[] = {
    { 4808, Operand::Calls, 0, "__ZN2ls12SoundManager21HandleInitBankLoadingEb" },
    { 4811, Operand::Add, kSoundObjectNamed },
    { 4833, Operand::Offset, kRegisterSoundObjectSlot * sizeof(void*) },
    { 4837, Operand::Unscaled, kSoundListeners - kSoundObjectNamed },
    { 4844, Operand::Offset, kRegisterSoundObjectSlot * sizeof(void*) },
    { 4848, Operand::Offset, kSoundRumbleListeners - kSoundObjectNamed },
    { 4855, Operand::Offset, kRegisterSoundObjectSlot * sizeof(void*) },
    { 4859, Operand::Unscaled, kSoundPlayerEmitters - kSoundObjectNamed },
    { 4866, Operand::Offset, kRegisterSoundObjectSlot * sizeof(void*) },
    { 4870, Operand::Offset, kSoundSpeakerListeners - kSoundObjectNamed },
    { 4877, Operand::Offset, kRegisterSoundObjectSlot * sizeof(void*) },
    { 4881, Operand::Offset, kSoundAmbient - kSoundObjectNamed },
    { 4888, Operand::Offset, kRegisterSoundObjectSlot * sizeof(void*) },
    { 4892, Operand::Offset, kSoundHud - kSoundObjectNamed },
    { 4899, Operand::Offset, kRegisterSoundObjectSlot * sizeof(void*) },
    { 4903, Operand::Offset, kSoundCineHud - kSoundObjectNamed },
    { 4905, Operand::Add, sizeof(uint64_t) },
    { 4909, Operand::Offset, kRegisterSoundObjectSlot * sizeof(void*) },
    { 4918, Operand::Offset, kSoundMusic },
};
static_assert(kSoundPlayerEmitters == kSoundObjects && kSoundRumbleListeners == kSoundObjects + 2 * kSoundObjectArray
    && kSoundSpeakerListeners == kSoundObjects + 3 * kSoundObjectArray);
bool CheckSoundObjectsNamed(Code const& f, std::string& problem)
{
    // Each format the loop's registrations use, in the order of the stores
    // above, and the music object's name.
    // The format each registration in the loop is given (mov x2, xN after
    // the adrp and add that form it in xN), in the order of the stores
    // above; and the music object's name.
    struct Named { uint32_t Adrp; uint32_t Move; char const* Name; };
    constexpr Named kNames[] = {
        { 4813, 4830, "Listener%d" }, { 4815, 4841, "RumbleListener%d" }, { 4817, 4852, "PlayerEmitter%d" },
        { 4819, 4863, "ControllerSpeakerListener %d" }, { 4821, 4874, "Ambient %d" }, { 4823, 4885, "HUD %d" },
        { 4825, 4896, "Cine HUD %d" }, { 4910, 0, "Music" },
    };
    constexpr uint32_t kMoveToX2 = 0xaa0003e2;
    for (auto const& named : kNames) {
        auto size = strlen(named.Name) + 1;
        auto va = AdrpAddTarget(f, named.Adrp);
        auto text = va ? f.Image.DataAt(*va, size) : nullptr;
        if (!text || memcmp(text, named.Name, size) != 0) {
            problem = std::string("the sound object registered as ") + named.Name + " is not named so";
            return false;
        }
        if (named.Move && f.At(named.Move) != (kMoveToX2 | (RegD(f.At(named.Adrp)) << 16))) {
            problem = std::string("the sound objects' formats are not passed in the order the Mac type has them, at ") + named.Name;
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// What the game's writers of the translated strings and the template cache
// do (Layout/GameGlobals.h), which Ext.Loca's and the template cache's
// writes of Norbyte's code do the same way: under the same lock, into the
// same members, with memory from malloc that the game frees with free.
// ---------------------------------------------------------------------------

// ls::TranslatedStringRepository::AddTranslatedString(handle, text, pool,
// flags) takes the repository's write lock (+0xf0), makes the text's string
// of 16 bytes with malloc and the string's constructor, appends its address
// to the pool's strings (an array: its capacity and size at +8 and +0xc,
// grown by its Reallocate), inserts the text into the pool's map, and
// leaves the lock by marking it unowned (+0xf8).
//     add x0, x1, #0xf0 ; bl RecursiveSRWLock<SRWSpinLock>::WriteLock() ; ... ; mov w0, #0x10 ; bl _malloc ; ...
//     bl STDString::STDString(StringView const&) ; ldp w9, w8, [x23, #0x8] ; ... ; bl DynamicArray<STDString*>::Reallocate(int)
//     ldr w8, [x23, #0xc] ; add w9, w8, #1 ; str w9, [x23, #0xc] ; ... ; mov x9, #-1 ; str x9, [x19, #0xf8]
constexpr uint64_t kArrayCapacity = 0x08, kArraySize = 0x0c;
constexpr Expect kTranslatedStringAdd[] = {
    { 15, Operand::Add, kTranslatedLock },
    { 16, Operand::Calls, 0, "__ZN2ls16RecursiveSRWLockINS_11SRWSpinLockEE9WriteLockEv" },
    { 261, Operand::Move, kGameStringBytes },
    { 262, Operand::CallsImport, 0, "_malloc" },
    { 276, Operand::Calls, 0, "__ZN2ls9STDStringC1ERKNS_11_StringViewIcEE" },
    { 277, Operand::PairOffset, kArrayCapacity },
    { 287, Operand::Calls, 0, "__ZN2ls12DynamicArrayIPNS_9STDStringENS_15TaggedAllocatorIiEEE10ReallocateEi" },
    { 288, Operand::Offset, kArraySize },
    { 290, Operand::Offset, kArraySize },
    { 814, Operand::Offset, kTranslatedLockOwner },
};
// The pool's strings grow by malloc and free; a string's characters, when
// they are behind its pointer, are freed with free.
//     ... ; bl _malloc ; ... ; bl _free           ... ; bl _free
constexpr Expect kTranslatedStringsGrow[] = {
    { 46, Operand::CallsImport, 0, "_malloc" },
    { 96, Operand::CallsImport, 0, "_free" },
};
constexpr Expect kGameStringFree[] = {
    { 30, Operand::CallsImport, 0, "_free" },
};

// ls::CacheTemplateManagerBase::RegisterTemplate(handle, template, ...) takes
// the cache's write lock (+0xd0), adds the template to the map by handle
// (+0x50), by name (+0x10) and its count of references, 0, to the counts
// (+0x90); the maps' arrays grow by malloc and free.
//     add x22, x0, #0xd0 ; ... ; bl SRWKernelLock::WriteLock() ; ... ; add x0, x19, #0x50 ; bl HashTable<TemplateHandle>::Ensure ; ...
//     bl _malloc ; ... ; bl _free ; add x0, x19, #0x10 ; bl HashMap<FixedString, GameObjectTemplate*>::SetUniversal ;
//     add x0, x19, #0x90 ; bl HashTable<TemplateHandle>::Ensure ; ... ; bl _malloc ; ... ; bl _free
constexpr Expect kTemplateCacheRegister[] = {
    { 11, Operand::Add, kTemplateCacheLock },
    { 22, Operand::Calls, 0, "__ZN2ls13SRWKernelLock9WriteLockEv" },
    { 35, Operand::Add, kTemplateCacheByHandle },
    { 38, Operand::Calls, 0, "__ZN2ls9HashTableINS_14TemplateHandleENS_19HashTableOpsDefaultEE6EnsureIRKS1_EENSt3__15tupleIJibEEEOT_" },
    { 72, Operand::CallsImport, 0, "_malloc" },
    { 123, Operand::CallsImport, 0, "_free" },
    { 129, Operand::Add, kTemplateCacheByName },
    { 131, Operand::Calls, 0,
        "__ZN2ls7HashMapINS_11FixedStringEPNS_18GameObjectTemplateENS_19HashTableOpsDefaultEE12SetUniversalIRKS1_RKS3_EENS_7EMapSetEOT_OT0_" },
    { 132, Operand::Add, kTemplateCacheRefCounts },
    { 135, Operand::Calls, 0, "__ZN2ls9HashTableINS_14TemplateHandleENS_19HashTableOpsDefaultEE6EnsureIRKS1_EENSt3__15tupleIJibEEEOT_" },
    { 169, Operand::CallsImport, 0, "_malloc" },
    { 220, Operand::CallsImport, 0, "_free" },
};

// ---------------------------------------------------------------------------
// The results of virtual functions declared in the Windows style
// (Layout/DeclaredVirtuals.cpp)
// ---------------------------------------------------------------------------

constexpr uint32_t kRet = 0xd65f03c0;
// esv::SurfaceAction::GetTeamID(bool) const returns the GUID in x0 and x1:
//     adrp x8, page ; add x8, x8, #off ; ldp x0, x1, [x8] ; ret
bool CheckGuidInPair(Code const& f, std::string& problem)
{
    if (f.Count() != 4 || f.At(2) != 0xa9400500 || f.At(3) != kRet) {
        problem = "it does not return the GUID in x0 and x1";
        return false;
    }
    return true;
}
// ls::Resource's and ls::GameObjectTemplate's GetLogInfo() and ToLogString()
// keep the address of the result their caller provides, x8, before anything
// writes x8:  stp ... ; mov x19, x8
bool CheckResultInMemory(Code const& f, std::string& problem)
{
    constexpr uint32_t kMoveFromX8 = 0xaa0803e0;
    for (uint32_t n = 0; n < 12 && n < f.Count(); n++) {
        uint32_t i = f.At(n);
        if ((i & ~0x1fu) == kMoveFromX8 && RegD(i) != 8) return true;
        if (RegD(i) == 8) break;
    }
    problem = "it does not take the address of its result from x8";
    return false;
}
// eoc::EoCGameObjectTemplate::GetBoundMin and GetBoundMax return a Vector3f
// in s0, s1 and s2:  ldp s0, s1, [x8, #0xc] ; ldr s2, [x8, #0x14] ; ret
bool CheckVectorInFloats(Code const& f, std::string& problem)
{
    for (uint32_t n = 0; n + 2 < f.Count(); n++) {
        uint32_t pair = f.At(n), single = f.At(n + 1);
        if ((pair & 0xffc00000) == 0x2d400000 && RegD(pair) == 0 && ((pair >> 10) & 0x1f) == 1
            && (single & 0xffc00000) == 0xbd400000 && RegD(single) == 2 && f.At(n + 2) == kRet) {
            return true;
        }
    }
    problem = "it does not return the vector in s0, s1 and s2";
    return false;
}
// GetBoundRadius and GetBoundSize return a float in s0: the last instruction
// before the final ret loads s0 (ldr s0, [x8, #0x24]) or computes it (fmul
// s0, s0, s1).
bool CheckFloatResult(Code const& f, std::string& problem)
{
    uint32_t last = f.Count() >= 2 ? f.At(f.Count() - 2) : 0;
    bool load = (last & 0xffc00000) == 0xbd400000, multiply = (last & 0xffe0fc00) == 0x1e200800;
    if (f.Count() < 2 || f.At(f.Count() - 1) != kRet || !(load || multiply) || RegD(last) != 0) {
        problem = "it does not return the float in s0";
        return false;
    }
    return true;
}
// ecl::CharacterTask::GetActionCosts() const writes its empty array into the
// memory its caller provides:  stp xzr, xzr, [x8] ; ret
bool CheckEmptyArrayInMemory(Code const& f, std::string& problem)
{
    if (f.Count() != 2 || f.At(0) != 0xa9007d1f || f.At(1) != kRet) {
        problem = "it does not write its result through x8";
        return false;
    }
    return true;
}
// ecl::CharacterTask_Spell::GetPreviewTiles() const returns an optional
// pointer in x0 and x1: the spell preview's tiles component, which the
// entity world's GetComponent left in x0, and whether there is one in w1.
//     bl GetComponent<SurfaceTilesComponent const> ; cbz x0, none ; ... ; cset w1, ne ; ... ; none: mov x1, #0
constexpr Expect kPreviewTilesResult[] = {
    { 11, Operand::Calls, 0,
        "__ZN3ecs11EntityWorld12GetComponentIKN3ecl13spell_preview21SurfaceTilesComponentELb0EEEPT_N2ls2IDINS_18EntityHandleTraitsEEENSt3__117integral_constantIbLb0EEE" },
};
bool CheckOptionalPointerInPair(Code const& f, std::string& problem)
{
    constexpr uint32_t kNoneIfNull = 0xb40000a0, kEngaged = 0x1a9f07e1, kNone = 0xd2800001;     // cbz x0, +0x14 ; cset w1, ne ; mov x1, #0
    if (f.At(12) != kNoneIfNull || f.At(15) != kEngaged || f.At(17) != kNone || f.At(20) != kRet) {
        problem = "it does not return the pointer in x0 and whether there is one in x1";
        return false;
    }
    return true;
}
// ecl::CharacterTask_Spell::GetPickupEntityPosition() const hands on the
// ls::Optional<Vector3f> its helper returns in x0 and x1, the flag in the byte
// after the three floats:
//     bl SpellCastWrapper::GetPickupEntityPosition ; and x8, x1, #0xffffff0000000000 ; and x9, x1, #0xff00000000 ; ... ; orr x1, x8, x10 ; ret
constexpr Expect kPickupPositionResult[] = {
    { 45, Operand::Calls, 0, "__ZN12_GLOBAL__N_116SpellCastWrapper23GetPickupEntityPositionEv" },
    { 47, Operand::Mask, kOptionalPositionEngagedMask },
};
bool CheckOptionalPositionInPair(Code const& f, std::string& problem)
{
    constexpr uint32_t kFlagToX1 = 0xaa0a0101;     // orr x1, x8, x10
    if (f.At(40) != kFlagToX1 || f.At(43) != kRet || RegN(f.At(47)) != 1) {
        problem = "it does not return the optional position in x0 and x1";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ls::PhysicsBox and ls::PhysicsCapsule (Layout/GameResourceManager.h)
// ---------------------------------------------------------------------------

// Visit(visitor) visits each member under its name: whether the object is
// kinematic (+8) through the visitor's slot for a bool, the mass (+0xc)
// through its slot for a float, a box's extents (+0x14) through its slot for
// a vector of three floats, a capsule's radius (+0x18) and height (+0x14)
// through the slot for a float; then the name (+0x10).
//     adrp x8, page ; ldr x1, [x8, #strKinematic] ; add x2, x0, #0x8 ; ldr x8, [x20] ; ldr x8, [x8, #0xc0] ; ...
//     adrp x8, page ; ldr x1, [x8, #strMass] ; add x2, x19, #0xc ; ldr x8, [x20] ; ldr x8, [x8, #0xf0] ; ...
//     adrp x8, page ; ldr x1, [x8, #strExtends] ; add x2, x19, #0x14 ; ... ; ldr x8, [x8, #0x128] ; ... ; ldr w21, [x19, #0x10]
constexpr uint64_t kVisitBool = 0xc0, kVisitFloat = 0xf0, kVisitVector3 = 0x128;
constexpr Expect kPhysicsBoxVisit[] = {
    { 9, Operand::Add, kPhysicsKinematic },
    { 11, Operand::Offset, kVisitBool },
    { 17, Operand::Add, kPhysicsMass },
    { 19, Operand::Offset, kVisitFloat },
    { 25, Operand::Add, kPhysicsBoxExtents },
    { 29, Operand::Offset, kVisitVector3 },
    { 33, Operand::Offset, kPhysicsName },
};
constexpr Expect kPhysicsCapsuleVisit[] = {
    { 9, Operand::Add, kPhysicsKinematic },
    { 11, Operand::Offset, kVisitBool },
    { 17, Operand::Add, kPhysicsMass },
    { 19, Operand::Offset, kVisitFloat },
    { 25, Operand::Add, kPhysicsCapsuleRadius },
    { 27, Operand::Offset, kVisitFloat },
    { 33, Operand::Add, kPhysicsCapsuleHeight },
    { 35, Operand::Offset, kVisitFloat },
    { 39, Operand::Offset, kPhysicsName },
};
static_assert(kPhysicsBoxExtents + 3 * sizeof(float) == kPhysicsObjectSize && kPhysicsCapsuleRadius + sizeof(float) <= kPhysicsObjectSize);
struct VisitedName { uint32_t Adrp; char const* Symbol; };
bool CheckVisitedNames(Code const& f, std::span<VisitedName const> names, std::string& problem)
{
    for (auto const& name : names) {
        uint64_t va = f.Image.FindVA(name.Symbol);
        if (!va || LoadedAddress(f, name.Adrp) != va) {
            problem = std::string("a member is not visited under the name ") + name.Symbol;
            return false;
        }
    }
    return true;
}
bool CheckPhysicsBoxVisit(Code const& f, std::string& problem)
{
    constexpr VisitedName kNames[] = { { 7, "__ZN2ls4FFFS12strKinematicE" }, { 15, "__ZN2ls4FFFS7strMassE" }, { 23, "__ZN2ls4FFFS10strExtendsE" } };
    return CheckVisitedNames(f, kNames, problem);
}
bool CheckPhysicsCapsuleVisit(Code const& f, std::string& problem)
{
    constexpr VisitedName kNames[] = { { 7, "__ZN2ls4FFFS12strKinematicE" }, { 15, "__ZN2ls4FFFS7strMassE" }, { 23, "__ZN2ls4FFFS9strRadiusE" },
        { 31, "__ZN2ls4FFFS9strHeightE" } };
    return CheckVisitedNames(f, kNames, problem);
}

// ls::SoundComponent::ActiveData::ActiveData() starts the sound object's id
// (+0) at -1 and the entity (+8) at the null handle from one 16-byte
// constant, zeroes +0x10 to +0x28, and gives the switches' ref map (+0x18)
// seven buckets (+0x20 and +0x1c).
//     adrp x8, page ; ldr q0, [x8, #off] ; str q0, [x0] ; stp xzr, xzr, [x0, #0x18] ; str xzr, [x0, #0x10] ; ...
//     mov w0, #0x38 ; bl _malloc ; ... ; str x20, [x19, #0x20] ; ... ; mov w8, #0x7 ; str w8, [x19, #0x1c]
constexpr Expect kActiveDataCreate[] = {
    { 7, Operand::Offset, kActiveDataObject },
    { 8, Operand::PairOffset, kActiveDataSwitches },
    { 9, Operand::Offset, 0x10 },
    { 31, Operand::Move, 7 * sizeof(void*) },
    { 32, Operand::CallsImport, 0, "_malloc" },
    { 43, Operand::Offset, kActiveDataSwitches + kRefMapBuckets },
    { 52, Operand::Move, 7 },
    { 53, Operand::Offset, kActiveDataSwitches + kRefMapBucketCount },
};
bool CheckActiveDataCreate(Code const& f, std::string& problem)
{
    auto start = static_cast<uint64_t const*>(f.Image.DataAt(LoadedAddress(f, 5), 2 * sizeof(uint64_t)));
    if (!start || start[0] != kNoSoundObject || start[1] != kNoEntity) {
        problem = "a new sound's object and entity do not start as no object and the null handle";
        return false;
    }
    return true;
}
// ecl::SoundSystem::CreateTempSound(...) makes a component's active data of
// 0x28 bytes, keeps it at +8, registers the sound object under the effect's
// name (+0x18) through slot 53 of the manager and stores the id it returns
// at +0 of the active data; then it sets flags at +0x10.
//     mov w0, #0x28 ; bl _malloc ; ... ; bl ls::SoundComponent::ActiveData::ActiveData() ; mov x8, x19 ; str x26, [x19, #0x8]
//     ldr x26, [x20, #0xc0] ; ldr w0, [x19, #0x18] ; ... ; ldr x8, [x26] ; ldr x8, [x8, #0x1a8] ; mov x0, x26 ; blr x8
//     mov x26, x19 ; ldr x8, [x19, #0x8] ; str x0, [x8] ; ldr w10, [x19, #0x10]
constexpr Expect kTempSound[] = {
    { 302, Operand::Move, kActiveDataSize },
    { 303, Operand::CallsImport, 0, "_malloc" },
    { 315, Operand::Calls, 0, "__ZN2ls14SoundComponent10ActiveDataC1Ev" },
    { 317, Operand::Offset, kSoundComponentActiveData },
    { 319, Operand::Offset, kSoundComponentEffect },
    { 328, Operand::Offset, kRegisterSoundObjectSlot * sizeof(void*) },
    { 332, Operand::Offset, kSoundComponentActiveData },
    { 333, Operand::Offset, kActiveDataObject },
    { 334, Operand::Offset, kSoundComponentFlags },
};

// ---------------------------------------------------------------------------
// ls::InputManager (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

// GetInputValue(id, player) looks each device of the binding up in the map
// of input states at +0x80: the bucket is the 16-bit id, sign-extended,
// modulo the bucket count (+0x88); the keys (+0xa0) are 16 bits each, the
// next keys at +0x90 and the states (+0xb0) pointers.
//     ldrsw x14, [x0, #0x88] ; ... ; ubfx x1, x17, #16, #16 ; sxth x1, w1 ; udiv x2, x1, x14 ; msub ; ldr x2, [x0, #0x80]
//     ... ; ldr x2, [x0, #0xa0] ; ldrh w3, [x2, x1, lsl #1] ; ... ; ldr x3, [x0, #0x90] ; ... ; ldr x2, [x0, #0xb0] ; ldr x1, [x2, x1, lsl #3]
constexpr Expect kInputValue[] = {
    { 23, Operand::Offset, kInputStates + kHashMapBucketCount },
    { 46, Operand::Offset, kInputStates + kHashMapBuckets },
    { 50, Operand::Offset, kInputStates + kHashMapKeys },
    { 54, Operand::Offset, kInputStates + kHashMapNext },
    { 58, Operand::Offset, kInputStates + kHashMapValues },
};
bool CheckInputValue(Code const& f, std::string& problem)
{
    constexpr uint32_t kSignExtend = 0x93403c21;    // sxth x1, w1
    constexpr uint32_t kKeyRead = 0x78617843;       // ldrh w3, [x2, x1, lsl #1]
    if (f.At(43) != kSignExtend || f.At(51) != kKeyRead) {
        problem = "a device's id is not hashed as a signed 16-bit number, or not kept in 16 bits";
        return false;
    }
    return true;
}
// gui::vmplayer::InputEventSystemUTHelper::GetInputDefinitions(manager)
// hands back the definitions at +0xc0.
//     add x0, x1, #0xc0 ; ret
constexpr Expect kInputDefinitionsOf[] = {
    { 0, Operand::Add, kInputDefinitions },
};
// SetPlayerDevice(player, device) links the device in the input scheme
// (+0xd0), keeps the player's device (+0x360, 32 bits a player) and id
// (+0x370, 16 bits), takes the player's colour (+0x3c8, 16 bytes a player)
// from the device's (+0x14 in its 0x28 bytes at +0x408, of twelve) when the
// device has one (+0x24), and tells the device listeners (+0x2f0).
//     add x22, x0, #0xd0 ; ... ; add x10, x20, w21, uxtb #2 ; ldr w11, [x10, #0x360] ; ... ; bl ls::InputScheme::LinkDevice
//     add x8, x20, x24, lsl #1 ; add x21, x8, #0x370 ; strh w19, [x8, #0x370] ; cmp w19, #0xb ; b.hi ; add x8, x20, #0x408
//     ... ; mov w10, #0x28 ; madd x3, x9, x10, x8 ; ldrb w10, [x3, #0x24] ; ... ; add x9, x20, x24, lsl #4 ; add x9, x9, #0x3c8
//     ldur q0, [x8, #0x14] ; str q0, [x9] ; ... ; add x0, x20, #0x2f0
constexpr Expect kInputPlayerDevice[] = {
    { 18, Operand::Add, kInputScheme },
    { 70, Operand::Offset, kInputPlayerDevices },
    { 98, Operand::Calls, 0, "__ZN2ls11InputScheme10LinkDeviceENS_14EInputDeviceIDENS_17EInputPlayerIndexE" },
    { 100, Operand::Add, kInputPlayerDeviceIds },
    { 101, Operand::Offset, kInputPlayerDeviceIds },
    { 102, Operand::Add, kInputDeviceCount - 1 },
    { 104, Operand::Add, kInputDevices },
    { 106, Operand::Move, kInputDeviceSize },
    { 108, Operand::ByteOffset, kInputDeviceHasColor },
    { 112, Operand::ShiftLeft, 4 },
    { 113, Operand::Add, kInputPlayerColors },
    { 114, Operand::Unscaled, kInputDeviceColor },
    { 119, Operand::Add, kInputDeviceListeners },
};

// ---------------------------------------------------------------------------
// eoc::BoostPrototypeManager (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

// CreateBoostPrototype(name, type, ...) hashes the name to a GUID and looks
// it up in the map at +0x10: the bucket is the GUID's two halves' exclusive
// or modulo the bucket count (+0x18), the keys (+0x30) are 16 bytes each, the
// next keys at +0x20. A new prototype is 0x28 bytes: no description, no
// type's name (+0x10), no data (+0x18), the type in the byte at +0x20 and no
// condition (+0x24); its data is parsed with the condition manager (+8). It
// is put in the map (count of keys +0x3c, the prototypes +0x40) and its
// condition appended to the handles (+0x50, size +0x5c).
//     add x22, x19, #0x10 ; ldrsw x8, [x19, #0x18] ; cbz ; eor x9, x1, x0 ; udiv x10, x9, x8 ; msub ; ldr x9, [x22]
//     ... ; ldr x9, [x19, #0x30] ; add x10, x9, x8, lsl #4 ; ... ; ldr x10, [x19, #0x20] ; ... ; mov w0, #0x28 ; bl _malloc
//     stp xzr, xzr, [x20] ; mov w8, #-0x1 ; str w8, [x20, #0x10] ; str xzr, [x20, #0x18] ; mov w9, #0x90
//     strb w9, [x20, #0x20] ; str w8, [x20, #0x24] ; ldr x25, [x19, #0x8] ; ... ; ldrsw x26, [x19, #0x3c] ; ...
//     ldr x24, [x19, #0x40] ; ... ; add x0, x19, #0x50 ; ... ; ldr w9, [x19, #0x5c] ; ... ; ldr x10, [x19, #0x50]
constexpr Expect kBoostCreate[] = {
    { 20, Operand::Add, kBoostManagerBoosts },
    { 21, Operand::Offset, kBoostManagerBoosts + kHashMapBucketCount },
    { 26, Operand::Offset, kHashMapBuckets },
    { 30, Operand::Offset, kBoostManagerBoosts + kHashMapKeys },
    { 31, Operand::ShiftLeft, 4 },
    { 36, Operand::Offset, kBoostManagerBoosts + kHashMapNext },
    { 60, Operand::Move, kBoostSize },
    { 61, Operand::CallsImport, 0, "_malloc" },
    { 72, Operand::PairOffset, 0 },
    { 74, Operand::Offset, kBoostTypeName },
    { 75, Operand::Offset, kBoostData },
    { 77, Operand::ByteOffset, kBoostType },
    { 78, Operand::Offset, kBoostCondition },
    { 79, Operand::Offset, kBoostManagerConditions },
    { 193, Operand::Offset, kBoostManagerBoosts + kHashMapKeysSize },
    { 200, Operand::Offset, kBoostManagerBoosts + kHashMapValues },
    { 302, Operand::Add, kBoostManagerHandles },
    { 304, Operand::Offset, kBoostManagerHandles + kArraySize },
    { 308, Operand::Offset, kBoostManagerHandles },
};
// The bucket of a GUID is the exclusive or of its halves, divided as 64 bits:
// Norbyte's hash of a Guid.
bool CheckBoostCreate(Code const& f, std::string& problem)
{
    constexpr uint32_t kMixHalves = 0xca000029;     // eor x9, x1, x0
    constexpr uint32_t kDivide = 0x9ac8092a;        // udiv x10, x9, x8
    if (f.At(23) != kMixHalves || f.At(24) != kDivide) {
        problem = "a boost's GUID is not hashed as the exclusive or of its halves";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The texture atlases by path (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

constexpr char kPathHash[] = "__ZN2ls4HashILNS_5EHashE1EEENS_10HashTraitsIXT_EE6ResultERKNS_7MemViewIKhEENS3_4SeedE";
constexpr char kMurmurUpdate[] = "__ZN2ls9_murmur3a6UpdateERNS_7Hash128ERNS_10HashTraitsILNS_5EHashE0EE6WindowENS_8_private10BlockedMemE";

// GetOrAdd(path, added) of the map of atlases (ls::PathHasher): the bucket is
// ls::Hash<1> of the path's bytes (a std::string's, short or long) with the
// seed 0, modulo the bucket count (+4); the buckets are at +8 and a node's
// path at +8 in it. A new node is 0x20 bytes, its atlas at +0x18, which is
// what it hands back.
//     ldr w21, [x0, #0x4] ; ldrsb w8, [x1, #0xf] ; ... ; mov x0, sp ; mov x1, #0x0 ; bl ls::Hash<1> ; udiv x8, x0, x21
//     msub ; ldr x8, [x19, #0x8] ; ... ; ldr x9, [x8, #0x8]! ; ... ; mov w0, #0x20 ; bl _malloc ; ...
//     str xzr, [x21, #0x18] ; ... ; add x0, x21, #0x18
constexpr Expect kAtlasPathFind[] = {
    { 12, Operand::Offset, kRefMapBucketCount },
    { 13, Operand::Offset, kPathSize - 1 },
    { 23, Operand::Move, kPathHashSeed },
    { 24, Operand::Calls, 0, kPathHash },
    { 27, Operand::Offset, kRefMapBuckets },
    { 39, Operand::Unscaled, kAtlasNodePath },
    { 81, Operand::Move, kAtlasNodeSize },
    { 82, Operand::CallsImport, 0, "_malloc" },
    { 105, Operand::Offset, kAtlasNodeAtlas },
    { 126, Operand::Add, kAtlasNodeAtlas },
};
// ls::Hash<1>(bytes, seed) is MurmurHash3 x64-128: both halves start at the
// seed, the blocks go through ls::_murmur3a::Update, the tail and the
// finalisation are its (the constants c1 0x87c37b91114253d5, c2
// 0x4cf5ad432745937f and fmix's 0xff51afd7ed558ccd, 0xc4ceb9fe1a85ec53), and
// it returns the first half, h1 + h2 after fmix.
//     mov x21, #0x53d5 ... ; mov x20, #0x937f ... ; stp x1, x1, [sp] ; ... ; bl Update ; ... ; mov x10, #0x8ccd ...
//     mov x11, #0xec53 ... ; add x0, x8, x9
constexpr Expect kPathHashCode[] = {
    { 6, Operand::Move, 0x53d5 },
    { 10, Operand::Move, 0x937f },
    { 21, Operand::Calls, 0, kMurmurUpdate },
    { 54, Operand::Move, 0x8ccd },
    { 60, Operand::Move, 0xec53 },
};
bool CheckPathHash(Code const& f, std::string& problem)
{
    constexpr uint32_t kSeedBoth = 0xa90007e1;      // stp x1, x1, [sp]
    constexpr uint32_t kFirstHalf = 0x8b090100;     // add x0, x8, x9
    if (f.At(14) != kSeedBoth || f.At(71) != kFirstHalf) {
        problem = "the hash does not start both halves at the seed and return the first";
        return false;
    }
    return true;
}
// Update's block step: c1, c2 and the two additions of MurmurHash3 x64-128
// (0x52dce729, 0x38495ab5), the rotations by 31 and 33 (ror 33 and 37).
//     mov x23, #0x53d5 ... ; mov x25, #0x937f ... ; mov w26, #0xe729 ; movk ; mov w27, #0x5ab5 ; movk ; ... ; ror x10, x10, #0x25 ; ... ; ror x12, x12, #0x21
constexpr Expect kMurmurBlocks[] = {
    { 13, Operand::Move, 0x53d5 },
    { 19, Operand::Move, 0x937f },
    { 23, Operand::Move, 0xe729 },
    { 25, Operand::Move, 0x5ab5 },
};

// ---------------------------------------------------------------------------
// NoesisGUI's tables, allocation and objects
// ---------------------------------------------------------------------------

// The address an adrp at `index` and the add or load after it form.
uint64_t FormedAt(Code const& f, uint32_t index)
{
    return PageAddress(f.VA(index), f.At(index), f.At(index + 1));
}
// mov x<d>, x<m>
bool MovesRegister(Code const& f, uint32_t index, uint32_t d, uint32_t m)
{
    return f.At(index) == (0xaa0003e0u | (m << 16) | d);
}
// stp x<first>, x<second>, [...]
bool StoresPair(Code const& f, uint32_t index, uint32_t first, uint32_t second)
{
    return PairOffset(f.At(index)) && RegD(f.At(index)) == first && ((f.At(index) >> 10) & 0x1f) == second;
}
// The 64 bits the image holds at `va`.
std::optional<uint64_t> DataWord(GameImage const& image, uint64_t va)
{
    auto data = image.DataAt(va, sizeof(uint64_t));
    if (!data) return {};
    uint64_t value;
    memcpy(&value, data, sizeof(value));
    return value;
}

// The index of a hash's bucket folded with the last FNV step, as the
// compiler multiplies once for both.
constexpr uint64_t kNoesisPointerIndexHash = (uint32_t)(kNoesisFnvPrime * kNoesisIndexHash);
static_assert(kNoesisPointerIndexHash == 0xca549e3b);

// Noesis::SymbolManager::AddString(char const*) hashes the string as FNV-1
// of its bytes made lower case and sign-extended, looks it up in the map
// (its bucket count at +0xc, its shift at +0x14; buckets of 16 bytes: the
// string, the hash at +8, the symbol at +0xc; a free one's string is 1);
// a new string goes at the end of the vector of strings 16 bytes before the
// map (its size at +8, its capacity in the low 24 bits of +0xc), and into
// the map with the symbol that is its index there.
constexpr Expect kNoesisSymbolAdd[] = {
    { 6, Operand::MoveWide, kNoesisFnvBasis },
    { 14, Operand::MoveWide, kNoesisFnvPrime },
    { 17, Operand::Add, 'A' },
    { 19, Operand::Add, 'a' - 'A' },
    { 20, Operand::Add, 26 },
    { 28, Operand::Offset, kNoesisMapBucketCount },
    { 29, Operand::Offset, kNoesisMapBuckets },
    { 30, Operand::ShiftLeft, 4 },
    { 33, Operand::MoveWide, kNoesisIndexHash },
    { 40, Operand::Offset, kNoesisSymbolBucketHash },
    { 41, Operand::Offset, 0 },
    { 44, Operand::Add, kNoesisEmptyPointerKey },
    { 76, Operand::Offset, kNoesisSymbolBucketValue },
    { 109, Operand::Mask, kNoesisVectorCapacityMask },
    { 114, Operand::Offset, kNoesisVectorBegin },
    { 116, Operand::Offset, kNoesisVectorSize },
    { 121, Operand::Add, kNoesisSymbolKeys - kNoesisSymbolStrings },
    { 124, Operand::Calls, 0,
        "__ZN6Noesis11HashMapImplINS_14HashBucket_KHVIPKcjNS_24CaseCharArrayHashKeyInfoEEEE6InsertIRPcJRjEEENS_4PairINS6_8IteratorEbEEOT_DpOT0_" },
};
static_assert(sizeof(uint32_t) == 4 && 1u << 4 == kNoesisSymbolBucketBytes);
bool CheckNoesisSymbolAdd(Code const& f, std::string& problem)
{
    constexpr uint32_t kSignExtendChar = 0x13001d29;    // sxtb w9, w9
    uint64_t map = FormedAt(f, 26), strings = FormedAt(f, 112);
    if (!map || strings + kNoesisSymbolKeys != map || FormedAt(f, 106) != strings + kNoesisVectorSize
        || FormedAt(f, 36) != map + kNoesisMapShift) {
        problem = "the map, its shift and the vector of strings are not where the Mac type has them";
        return false;
    }
    if (f.At(22) != kSignExtendChar) {
        problem = "the characters are not hashed sign-extended";
        return false;
    }
    return true;
}

// Noesis::Reflection::RegisterType(Symbol, creator, filler) looks the symbol
// up in the map of types (its bucket count at +0xc, its number of entries at
// +0x10, its shift at +0x14; buckets of 16 bytes: the symbol, the type at +8;
// a free one's symbol is 0xffffffff, which cmn #1 compares with), makes the
// map twice as large when its buckets are no more than twice its entries,
// and appends the symbol (4 bytes) to the vector of symbols 16 bytes before
// the map.
constexpr Expect kNoesisReflectionRegister[] = {
    { 31, Operand::Offset, kNoesisMapBuckets },
    { 32, Operand::MoveWide, kNoesisIndexHash },
    { 35, Operand::Offset, kNoesisMapShift },
    { 42, Operand::Offset, kNoesisReflectionBucketValue },
    { 50, Operand::ShiftLeft, 1 },
    { 54, Operand::Offset, 0 },
    { 55, Operand::Offset, kNoesisReflectionBucketValue },
    { 55, Operand::StoresZero },
    { 66, Operand::Mask, kNoesisVectorCapacityMask },
    { 71, Operand::Offset, kNoesisVectorBegin },
    { 74, Operand::Offset, kNoesisVectorSize },
    { 94, Operand::Add, (uint32_t)-kNoesisEmptySymbolKey },
    { 102, Operand::ShiftLeftBy, 1 },
    { 105, Operand::Calls, 0, "__ZN6Noesis11HashMapImplINS_13HashBucket_KVINS_6SymbolEPNS_4TypeENS_11HashKeyInfoIS2_EEEEE4GrowEj" },
};
bool CheckNoesisReflectionRegister(Code const& f, std::string& problem)
{
    constexpr uint32_t kStoreSymbol = 0xb828594b;       // str w11, [x10, w8, uxtw #2]
    uint64_t map = FormedAt(f, 29), names = FormedAt(f, 69);
    if (!map || names + kNoesisReflectionTypes != map || FormedAt(f, 63) != names + kNoesisVectorSize
        || FormedAt(f, 26) != map + kNoesisMapBucketCount || FormedAt(f, 48) != map + kNoesisMapUsed || FormedAt(f, 103) != map) {
        problem = "the map, its counts and the vector of symbols are not where the Mac type has them";
        return false;
    }
    if (f.At(73) != kStoreSymbol) {
        problem = "the symbols are not stored as 32-bit elements";
        return false;
    }
    return true;
}

// Noesis::SetMemoryCallbacks keeps the user data and the three functions
// Noesis::Alloc, Realloc and Dealloc call; a null function is the default,
// malloc, realloc or free.
bool CheckNoesisSetMemoryCallbacks(Code const& f, std::string& problem)
{
    auto& image = f.Image;
    uint64_t callbacks = FormedAt(f, 12), user = FormedAt(f, 3);
    if (!callbacks || !user || FormedAt(f, 1) != image.FindVA("__ZL9AnsiAllocPvm") || FormedAt(f, 7) != image.FindVA("__ZL11AnsiReallocPvS_m")
        || FormedAt(f, 15) != image.FindVA("__ZL11AnsiDeallocPvS_")) {
        problem = "the defaults are not AnsiAlloc, AnsiRealloc and AnsiDealloc";
        return false;
    }
    // The three entry points read the function and the user data there.
    struct Entry { char const* Symbol; uint32_t Function, User; uint64_t Offset; };
    constexpr Entry kEntries[] = {
        { "__ZN6Noesis5AllocEm", 1, 3, kNoesisCallbackAlloc },
        { "__ZN6Noesis7ReallocEPvm", 2, 4, kNoesisCallbackRealloc },
        { "__ZN6Noesis7DeallocEPv", 1, 3, kNoesisCallbackDealloc },
    };
    for (auto const& entry : kEntries) {
        uint64_t va = image.FindVA(entry.Symbol);
        if (!va) {
            problem = std::string(entry.Symbol) + " is not in the symbol table";
            return false;
        }
        Code g{ image, va, va + 4 * (entry.User + 3) };
        if (FormedAt(g, entry.Function) != callbacks + entry.Offset || FormedAt(g, entry.User) != user) {
            problem = std::string(entry.Symbol) + " does not call the function the callbacks keep for it";
            return false;
        }
    }
    return true;
}
constexpr Expect kNoesisSetMemoryCallbacks[] = {
    { 0, Operand::PairOffset, 0 },
    { 9, Operand::PairOffset, 0x10 },
    { 14, Operand::PairOffset, kNoesisCallbackAlloc },
    { 19, Operand::Offset, kNoesisCallbackDealloc },
};
constexpr Expect kNoesisAnsiAlloc[] = { { 1, Operand::JumpsToImport, 0, "_malloc" } };
constexpr Expect kNoesisAnsiRealloc[] = { { 2, Operand::JumpsToImport, 0, "_realloc" } };
constexpr Expect kNoesisAnsiDealloc[] = { { 1, Operand::JumpsToImport, 0, "_free" } };
// gui::GameUI::UpdateFromUI installs the UI manager's functions: on the
// stack, no user data, then NoesisAlloc, NoesisRealloc, NoesisDealloc.
constexpr Expect kNoesisInstallCallbacks[] = {
    { 52, Operand::PairOffset, 0x1b0 },
    { 53, Operand::PairOffset, 0x1c0 },
    { 57, Operand::Add, 0x1b0 },
    { 58, Operand::Calls, 0, "__ZN6Noesis18SetMemoryCallbacksERKNS_15MemoryCallbacksE" },
};
bool CheckNoesisInstallCallbacks(Code const& f, std::string& problem)
{
    auto& image = f.Image;
    if (FormedAt(f, 46) != image.FindVA("__ZN2ui15NoesisUIManager11NoesisAllocEPvm") || RegD(f.At(46)) != 8
        || FormedAt(f, 48) != image.FindVA("__ZN2ui15NoesisUIManager13NoesisReallocEPvS1_m") || RegD(f.At(48)) != 9
        || FormedAt(f, 50) != image.FindVA("__ZN2ui15NoesisUIManager13NoesisDeallocEPvS1_") || RegD(f.At(50)) != 10
        || !StoresPair(f, 52, 31, 8) || !StoresPair(f, 53, 9, 10)) {
        problem = "the callbacks installed are not the UI manager's NoesisAlloc, NoesisRealloc and NoesisDealloc";
        return false;
    }
    return true;
}
constexpr Expect kNoesisManagerAlloc[] = { { 29, Operand::CallsImport, 0, "_malloc" } };
constexpr Expect kNoesisManagerDealloc[] = { { 11, Operand::JumpsToImport, 0, "_free" } };
constexpr Expect kNoesisManagerRealloc[] = { { 3, Operand::JumpsTo, 0, "__ZN2ls13MemoryManager10ReallocateEPvmim" } };
// ls::MemoryManager::Reallocate takes the old block's size from
// malloc_size, allocates the new one with malloc, copies and frees the old.
constexpr Expect kGameReallocate[] = {
    { 14, Operand::CallsImport, 0, "_malloc_size" },
    { 21, Operand::CallsImport, 0, "_malloc" },
    { 70, Operand::CallsImport, 0, "_memcpy" },
    { 72, Operand::CallsImport, 0, "_free" },
};
// The game's operator new, which allocates the ancestors of a type class.
constexpr Expect kGameOperatorNew[] = { { 29, Operand::CallsImport, 0, "_malloc" } };

// Noesis::Type::GetTypeId() reads the symbol at +8.
constexpr Expect kNoesisTypeId[] = { { 0, Operand::Offset, kNoesisTypeName } };
// Noesis::TypeMeta::TypeMeta(Symbol) makes its metadata an empty vector at
// +0x10, pointing at its end.
constexpr Expect kNoesisTypeMeta[] = {
    { 3, Operand::Calls, 0, "__ZN6Noesis4TypeC2ENS_6SymbolE" },
    { 7, Operand::Add, kNoesisTypeMetaData + kNoesisVectorBytes },
    { 8, Operand::Offset, kNoesisTypeMetaData + kNoesisVectorBegin },
    { 11, Operand::Offset, kNoesisTypeMetaData + kNoesisVectorSize },
};
static_assert(kNoesisTypeMetaData + kNoesisVectorBytes == kNoesisTypeMetaSize);
// The size and capacity an empty vector starts with, loaded by the adrp at `adrp` and the ldr after it.
bool LoadsEmptyVector(Code const& f, uint32_t adrp, std::string& problem)
{
    if (DataWord(f.Image, FormedAt(f, adrp)) != kNoesisEmptyVectorWord) {
        problem = "an empty vector's size and capacity are not the Mac type's";
        return false;
    }
    return true;
}
bool CheckNoesisTypeMeta(Code const& f, std::string& problem)
{
    return LoadsEmptyVector(f, 9, problem);
}
// Noesis::TypeClass::TypeClass(Symbol, bool): the flag at +0x20, no base at
// +0x28, the three empty vectors at +0x30, +0x40, +0x50; the ancestors from
// operator new, not collapsed, their vector empty at +8, at +0x60.
constexpr Expect kNoesisTypeClassMembers[] = {
    { 13, Operand::ByteOffset, kNoesisClassIsInterface },
    { 14, Operand::Add, kNoesisClassAncestors },
    { 16, Operand::Unscaled, kNoesisClassEvents },
    { 18, Operand::Unscaled, kNoesisClassProperties },
    { 20, Operand::Unscaled, kNoesisClassInterfaces },
    { 21, Operand::Offset, kNoesisClassBase },
    { 21, Operand::StoresZero },
    { 24, Operand::Offset, kNoesisClassInterfaces + kNoesisVectorSize },
    { 25, Operand::Offset, kNoesisClassProperties + kNoesisVectorSize },
    { 26, Operand::Offset, kNoesisClassEvents + kNoesisVectorSize },
    { 28, Operand::Calls, 0, "__Znwm" },
    { 29, Operand::ByteOffset, kNoesisAncestorsCollapsed },
    { 29, Operand::StoresZero },
    { 32, Operand::Offset, kNoesisAncestorsVector + kNoesisVectorBegin },
    { 33, Operand::Offset, kNoesisAncestorsVector + kNoesisVectorSize },
    { 34, Operand::Offset, kNoesisClassAncestors },
};
static_assert(kNoesisClassInterfaces + kNoesisVectorBytes == kNoesisClassProperties
    && kNoesisClassProperties + kNoesisVectorBytes == kNoesisClassEvents && kNoesisClassEvents + kNoesisVectorBytes == kNoesisClassAncestors);
static_assert(kNoesisAncestorsVector + kNoesisVectorBytes == kNoesisAncestorsLock);
constexpr Expect kNoesisClassBaseRead[] = { { 0, Operand::Offset, kNoesisClassBase } };
constexpr Expect kNoesisClassInterfaceRead[] = { { 0, Operand::ByteOffset, kNoesisClassIsInterface } };
constexpr Expect kNoesisClassPropertyCount[] = { { 0, Operand::Offset, kNoesisClassProperties + kNoesisVectorSize } };
constexpr Expect kNoesisClassPropertyRead[] = { { 0, Operand::Offset, kNoesisClassProperties + kNoesisVectorBegin } };
// Noesis::TypeClass::IsDescendantOf collapses the ancestors, then searches
// their vector (16-byte ancestors, the type first) by type.
constexpr Expect kNoesisClassDescendant[] = {
    { 7, Operand::Calls, 0, "__ZNK6Noesis9TypeClass17CollapseAncestorsEv" },
    { 8, Operand::Offset, kNoesisClassAncestors },
    { 9, Operand::Offset, kNoesisAncestorsVector + kNoesisVectorBegin },
    { 10, Operand::Offset, kNoesisAncestorsVector + kNoesisVectorSize },
    { 11, Operand::ShiftLeft, 4 },
    { 15, Operand::Unscaled, kNoesisAncestorBytes },
    { 24, Operand::Offset, 0 },
};
static_assert(1u << 4 == kNoesisAncestorBytes);
// Noesis::TypeProperty::TypeProperty(Symbol, Type const*): the symbol at +8,
// the type at +0x10 and the metadata, an empty vector, at +0x18.
constexpr Expect kNoesisTypePropertyMembers[] = {
    { 3, Operand::Offset, kNoesisPropertyName },
    { 4, Operand::Add, kNoesisPropertyMetaData + kNoesisVectorBytes },
    { 5, Operand::PairOffset, kNoesisPropertyType },
    { 8, Operand::Offset, kNoesisPropertyMetaData + kNoesisVectorSize },
};
bool CheckNoesisTypeProperty(Code const& f, std::string& problem)
{
    return LoadsEmptyVector(f, 6, problem);
}
static_assert(kNoesisPropertyType + 8 == kNoesisPropertyMetaData && kNoesisPropertyMetaData + kNoesisVectorBytes == kNoesisPropertySize);
constexpr Expect kNoesisEnumValuesRead[] = {
    { 0, Operand::Offset, kNoesisEnumValues + kNoesisVectorBegin },
    { 1, Operand::Offset, kNoesisEnumValues + kNoesisVectorSize },
};
// Noesis::FrameworkElement::GetName() takes the text of the String it gets:
// at +8, inline when the byte at +7 is set.
constexpr Expect kNoesisNameText[] = {
    { 6, Operand::Unscaled, kNoesisStringText },
    { 7, Operand::Unscaled, (uint64_t)(int64_t)(kNoesisStringIsSmall - kNoesisStringText) },
};
constexpr Expect kNoesisUriStr[] = {
    { 0, Operand::ByteOffset, kNoesisUriText + kNoesisStringIsSmall },
    { 1, Operand::Add, kNoesisUriText + kNoesisStringText },
};
constexpr Expect kNoesisBoxedIntValue[] = { { 0, Operand::Add, kNoesisBoxedInt } };
constexpr Expect kNoesisCollectionCount[] = { { 0, Operand::Offset, kNoesisCollectionItems + kNoesisVectorSize } };
// Noesis::UIElementData::FindEvent takes a reference to the event and
// releases it: 32-bit atomic additions at +8.
constexpr Expect kNoesisRefCountChanged[] = {
    { 25, Operand::Add, kNoesisRefCount },
    { 27, Operand::AtomicAdd32 },
    { 29, Operand::AtomicAdd32 },
};

// Noesis::DependencyObject::GetExpression(DependencyProperty const*) finds
// the property's stored value in the object's map at +0x18 (its bucket
// count at +0x24, its shift at +0x2c; a pointer is hashed as FNV-1a of its
// two halves; buckets of 16 bytes, the value's address at +8, a free one's
// property 1), and when its flags (+9) have bit 2 the complex value's first
// member is the expression.
constexpr Expect kNoesisObjectValuesFind[] = {
    { 0, Operand::Offset, kNoesisObjectValues + kNoesisMapBucketCount },
    { 2, Operand::Offset, kNoesisObjectValues + kNoesisMapBuckets },
    { 3, Operand::MoveWide, kNoesisFnvBasis },
    { 6, Operand::MoveWide, kNoesisFnvPrime },
    { 9, Operand::ShiftRight, 32 },
    { 11, Operand::MoveWide, kNoesisPointerIndexHash },
    { 14, Operand::Offset, kNoesisObjectValues + kNoesisMapShift },
    { 16, Operand::ShiftLeftBy, 4 },
    { 22, Operand::ShiftLeft, 4 },
    { 23, Operand::Offset, kNoesisValueBucketValue },
    { 24, Operand::ByteOffset, kNoesisStoredFlags },
    { 25, Operand::TestsBit, kNoesisStoredExpression },
    { 28, Operand::Offset, 0 },
    { 29, Operand::Offset, 0 },
    { 32, Operand::Add, kNoesisEmptyPointerKey },
};
static_assert(1u << 4 == kNoesisValueBucketBytes);
// GetValueProvider reads the provider, the byte at +8 of the stored value.
constexpr Expect kNoesisObjectValueProvider[] = {
    { 26, Operand::Offset, kNoesisValueBucketValue },
    { 27, Operand::ByteOffset, kNoesisStoredProvider },
};
// Noesis::StoredValue::GetFinalValue(size): a simple value is the value
// word (or what it points at, past 8 bytes); a complex one (bit 1) is the
// coerced value (bit 4) at +0x18, the animated one (bit 3) at +0x10, or else
// the base value at +8.
constexpr Expect kNoesisStoredFinal[] = {
    { 0, Operand::ByteOffset, kNoesisStoredFlags },
    { 1, Operand::TestsBit, kNoesisStoredComplex },
    { 2, Operand::Add, kNoesisInlineValueBytes + 1 },
    { 5, Operand::TestsBit, kNoesisStoredCoerced },
    { 7, Operand::Add, kNoesisComplexBase },
    { 8, Operand::Add, kNoesisComplexAnimated },
    { 9, Operand::Mask, 1u << kNoesisStoredAnimated },
    { 15, Operand::Add, kNoesisComplexCoerced },
};
// NotifyPropertyChanged(property, stored value, ...) tests whether the
// stored value is initialized, bit 0 of its flags.
constexpr Expect kNoesisStoredInitializedTest[] = {
    { 60, Operand::ByteOffset, kNoesisStoredFlags },
    { 61, Operand::TestsBit, kNoesisStoredInitialized },
};
bool CheckNoesisStoredInitializedTest(Code const& f, std::string& problem)
{
    if (!MovesRegister(f, 12, 21, 2) || RegN(f.At(60)) != 21) {
        problem = "the flags tested are not the stored value's";
        return false;
    }
    return true;
}
// Noesis::DependencyProperty::DependencyProperty(name, manager, type, owner,
// metadata, validate, access): the symbol at +0xc, the manager and the type
// at +0x10, the owner and the validation at +0x20, whether it is read-only
// at +0x30, the map of metadata from +0x38.
constexpr Expect kNoesisDependencyPropertyMembers[] = {
    { 18, Operand::Offset, kNoesisPropertyDpName },
    { 19, Operand::PairOffset, kNoesisPropertyDpManager },
    { 20, Operand::PairOffset, kNoesisPropertyDpOwner },
    { 23, Operand::ByteOffset, kNoesisPropertyDpReadOnly },
    { 25, Operand::Unscaled, kNoesisPropertyDpMetaData },
};
static_assert(kNoesisPropertyDpManager + 8 == kNoesisPropertyDpType && kNoesisPropertyDpOwner + 8 == kNoesisPropertyDpValidate);
bool CheckNoesisDependencyPropertyMembers(Code const& f, std::string& problem)
{
    if (!MovesRegister(f, 12, 26, 1) || !MovesRegister(f, 11, 25, 2) || !MovesRegister(f, 10, 24, 3) || !MovesRegister(f, 9, 23, 4)
        || !MovesRegister(f, 7, 21, 6) || RegD(f.At(18)) != 26 || !StoresPair(f, 19, 25, 24) || !StoresPair(f, 20, 23, 21)) {
        problem = "the arguments are not stored in the members the Mac type gives them";
        return false;
    }
    return true;
}
// Noesis::RoutedEvent::RoutedEvent(name, owner, routing).
constexpr Expect kNoesisRoutedEventMembers[] = {
    { 7, Operand::Calls, 0, "__ZN6Noesis13BaseComponentC2Ev" },
    { 11, Operand::Offset, kNoesisEventName },
    { 12, Operand::Offset, kNoesisEventOwner },
    { 13, Operand::Offset, kNoesisEventRouting },
};
bool CheckNoesisRoutedEventMembers(Code const& f, std::string& problem)
{
    if (!MovesRegister(f, 6, 21, 1) || !MovesRegister(f, 5, 20, 2) || !MovesRegister(f, 4, 19, 3) || RegD(f.At(11)) != 21
        || RegD(f.At(12)) != 20 || RegD(f.At(13)) != 19) {
        problem = "the arguments are not stored in the members the Mac type gives them";
        return false;
    }
    return true;
}
// UIElementData::FindEvent and DependencyData::FindProperty: sets of
// pointers keyed by the symbol at +0xc of each, hashed as the symbol.
constexpr Expect kNoesisDataEventsFind[] = {
    { 3, Operand::Offset, kNoesisDataEvents + kNoesisMapBucketCount },
    { 5, Operand::Offset, kNoesisDataEvents + kNoesisMapBuckets },
    { 6, Operand::MoveWide, kNoesisIndexHash },
    { 9, Operand::Offset, kNoesisDataEvents + kNoesisMapShift },
    { 12, Operand::Add, kNoesisEmptyPointerKey },
    { 14, Operand::Offset, kNoesisDataKeySymbol },
};
constexpr Expect kNoesisDataPropertiesFind[] = {
    { 3, Operand::Offset, kNoesisDataProperties + kNoesisMapBucketCount },
    { 5, Operand::Offset, kNoesisDataProperties + kNoesisMapBuckets },
    { 6, Operand::MoveWide, kNoesisIndexHash },
    { 9, Operand::Offset, kNoesisDataProperties + kNoesisMapShift },
    { 12, Operand::Add, kNoesisEmptyPointerKey },
    { 14, Operand::Offset, kNoesisDataKeySymbol },
};

constexpr Expect kNoesisVisualParentRead[] = { { 0, Operand::Offset, kNoesisVisualParent } };
constexpr Expect kNoesisFrameworkParentRead[] = { { 0, Operand::Offset, kNoesisFrameworkParent } };
constexpr Expect kNoesisFrameworkTemplatedParentRead[] = { { 0, Operand::Offset, kNoesisFrameworkTemplatedParent } };
// ConnectInheritanceParent takes the logical parent's inheritance parent (or
// the parent) and stores it as the element's, handing the old one on.
//     ldr x0, [x0, #0x1b0] ; ... ; ldr x8, [x0, #0x1c0] ; ... ; ldr x8, [x19, #0x1c0] ; ... ; str x2, [x19, #0x1c0] ; ... ; bl UpdateInheritanceParentOnChildren
constexpr Expect kNoesisInheritanceParent[] = {
    { 5, Operand::Offset, kNoesisFrameworkParent },
    { 16, Operand::Offset, kNoesisFrameworkInheritanceParent },
    { 21, Operand::Offset, kNoesisFrameworkInheritanceParent },
    { 26, Operand::Offset, kNoesisFrameworkInheritanceParent },
    { 28, Operand::Calls, 0, "__ZN6Noesis16FrameworkElement33UpdateInheritanceParentOnChildrenEPS0_S1_" },
};
// Noesis::UIElement::AddHandler(event, delegate) finds the event in the map
// of handlers at +0x140 (buckets of 0x28 bytes: the event, then its
// delegate), adds a null delegate for an event it does not have (growing the
// map as the others do), then calls the delegate's Add, at +0x40 of its
// implementation's vtable.
constexpr Expect kNoesisHandlersAdd[] = {
    { 8, Operand::MoveWide, kNoesisFnvBasis },
    { 11, Operand::MoveWide, kNoesisFnvPrime },
    { 15, Operand::Offset, kNoesisElementHandlers + kNoesisMapBucketCount },
    { 17, Operand::Offset, kNoesisElementHandlers + kNoesisMapBuckets },
    { 18, Operand::MoveWide, kNoesisPointerIndexHash },
    { 21, Operand::Offset, kNoesisElementHandlers + kNoesisMapShift },
    { 23, Operand::Move, kNoesisHandlerBucketBytes },
    { 28, Operand::Add, kNoesisEmptyPointerKey },
    { 43, Operand::Offset, kNoesisElementHandlers + kNoesisMapUsed },
    { 44, Operand::ShiftLeft, 1 },
    { 52, Operand::Unscaled, kNoesisHandlerBucketDelegate },
    { 53, Operand::Offset, kNoesisDelegateAdd },
    { 65, Operand::Add, kNoesisElementHandlers },
    { 66, Operand::Calls, 0,
        "__ZN6Noesis11HashMapImplINS_13HashBucket_KVIPKNS_11RoutedEventENS_8DelegateIFvPNS_13BaseComponentERKNS_15RoutedEventArgsEEEENS_11HashKeyInfoIS4_EEEEE4GrowEj" },
};
static_assert(kNoesisHandlerBucketDelegate + kNoesisDelegateBytes == kNoesisHandlerBucketBytes);
// RemoveHandler calls the delegate's Remove, at +0x48.
constexpr Expect kNoesisHandlersRemove[] = {
    { 3, Operand::Offset, kNoesisElementHandlers + kNoesisMapBucketCount },
    { 4, Operand::Offset, kNoesisElementHandlers + kNoesisMapBuckets },
    { 17, Operand::Offset, kNoesisElementHandlers + kNoesisMapShift },
    { 19, Operand::Move, kNoesisHandlerBucketBytes },
    { 40, Operand::Unscaled, kNoesisHandlerBucketDelegate },
    { 41, Operand::Offset, kNoesisDelegateRemove },
};

// ---------------------------------------------------------------------------
// The UI library's error handler
// ---------------------------------------------------------------------------

// Noesis::InvokeErrorHandler(file, line, fatal, format, ...) reads the
// handler from (anonymous namespace)::gErrorHandler, does nothing when there
// is none, formats the message and calls the handler with the file, the
// line, the message and whether it is fatal, in that order: the handler
// Norbyte's Ext.UI.EnableErrorReporting stores there, and the null it stores
// to turn reporting off.
//     adrp x22, page ; ldr x9, [x22, #off] ; cbz x9 ; mov x8, x3 ; mov x19, x2 ; mov x20, x1 ; mov x21, x0 ; ...
//     bl _vsnprintf ; ldr x8, [x22, #off] ; add x2, sp, #0x8 ; mov x0, x21 ; mov x1, x20 ; mov x3, x19 ; blr x8
constexpr Expect kInvokeErrorHandler[] = {
    { 23, Operand::CallsImport, 0, "_vsnprintf" },
};
bool CheckInvokeErrorHandler(Code const& f, std::string& problem)
{
    constexpr uint32_t kTestHandler = 0xb4000009, kTestHandlerMask = 0xff00001f;     // cbz x9, ...
    constexpr uint32_t kKeepFatal = 0xaa0203f3, kKeepLine = 0xaa0103f4, kKeepFile = 0xaa0003f5;   // mov x19, x2 ; mov x20, x1 ; mov x21, x0
    constexpr uint32_t kReadHandler = 0xf94002c9, kReloadHandler = 0xf94002c8, kReloadHandlerMask = 0xffc003ff;  // ldr x9 and x8, [x22, #...]
    constexpr uint32_t kMessage = 0x910023e2;       // add x2, sp, #0x8
    constexpr uint32_t kFile = 0xaa1503e0, kLine = 0xaa1403e1, kFatal = 0xaa1303e3;  // mov x0, x21 ; mov x1, x20 ; mov x3, x19
    constexpr uint32_t kCall = 0xd63f0100;          // blr x8
    uint64_t handler = f.Image.FindVA("__ZN12_GLOBAL__N_113gErrorHandlerE");
    if (!handler || LoadedAddress(f, 10) != handler || (f.At(11) & kReloadHandlerMask) != kReadHandler || (f.At(12) & kTestHandlerMask) != kTestHandler) {
        problem = "the handler is not read from gErrorHandler and tested for none";
        return false;
    }
    if (f.At(14) != kKeepFatal || f.At(15) != kKeepLine || f.At(16) != kKeepFile || (f.At(24) & kReloadHandlerMask) != kReloadHandler
        || f.At(25) != kMessage || f.At(26) != kFile || f.At(27) != kLine || f.At(28) != kFatal || f.At(29) != kCall) {
        problem = "the handler is not called with the file, the line, the message and whether it is fatal";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ecl::CursorControl (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

// OnInputEvent(event): on the event that shows the cursor it sets the visible
// timer (+0x14) from the fade timer (+0x18) and, unless the cursor shows
// (+0x10), shows it.
//     ldr s0, [x19, #0x18] ; str s0, [x19, #0x14] ; ldrb w9, [x19, #0x10] ; cmp w9, #0x1 ; b.eq ; mov w9, #0x1 ; strb w9, [x19, #0x10]
constexpr Expect kCursorInput[] = {
    { 17, Operand::Offset, kCursorFadeTimer },
    { 18, Operand::Offset, kCursorVisibleTimer },
    { 19, Operand::ByteOffset, kCursorShow },
    { 23, Operand::ByteOffset, kCursorShow },
};
// RemoveForcedMouseCursor(mode) removes the mode from the array at +0x30
// (its size at +0x3c, 32 bits an element) and asks for the last one left,
// or the default, with no overlay (+0x24, +0x28).
//     ldr x8, [x0, #0x30] ; ldr w9, [x0, #0x3c] ; ... ; str w9, [x0, #0x3c] ; ... ; ldr w8, [x8, w9, sxtw #2] ; stp w8, wzr, [x0, #0x24]
constexpr Expect kCursorRemoveForced[] = {
    { 0, Operand::Offset, kCursorOverrides },
    { 1, Operand::Offset, kCursorOverrides + kArraySize },
    { 46, Operand::Offset, kCursorOverrides + kArraySize },
    { 55, Operand::PairOffset, kCursorRequested },
};
static_assert(kCursorRequestedOverlay == kCursorRequested + kCursorTypeSize);

// ---------------------------------------------------------------------------
// ls::DragDropManager (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

// GetDragEntity(player) looks the player up in the map at +0xd0 (bucket
// count +0xd4, buckets +0xd8; a node's 16-bit id at +8) and reads the first
// of the entities of the data (+0x10 in the node) when there are any.
//     ldr w8, [x0, #0xd4] ; cbz ; sxtw x9, w1 ; udiv ; msub ; ldr x9, [x0, #0xd8] ; ... ; ldrh w10, [x9, #0x8]
//     cmp w10, w1, uxth ; b.ne ; ldr x10, [x9, #0x10] ; ldr w9, [x9, #0x1c]
constexpr Expect kDragDropGet[] = {
    { 0, Operand::Offset, kDragPlayers + kRefMapBucketCount },
    { 5, Operand::Offset, kDragPlayers + kRefMapBuckets },
    { 11, Operand::Offset, kMapNodeKey },
    { 14, Operand::Offset, kDragNodeData },
    { 15, Operand::Offset, kDragNodeData + kArraySize },
};
// AddController(controller) finds the controller's player (+8) in the map
// and, when the player is not there, makes a node of 0x48 bytes: it zeroes
// the position, the flags, the active controller and the array of
// controllers, keeps the id at +8 and counts the node (+0xd0).
//     ldrsh x8, [x1, #0x8] ; and w22, w8, #0xffff ; ldr w9, [x0, #0xd4] ; udiv x10, x8, x9 ; msub ; ldr x8, [x0, #0xd8]
//     ... ; mov w0, #0x48 ; bl _malloc ; ... ; stp xzr, xzr, [x20, #0x10] ; stur wzr, [x20, #0x27] ; str xzr, [x20, #0x20]
//     stp xzr, xzr, [x20, #0x38] ; str xzr, [x20, #0x30] ; ... ; strh w22, [x20, #0x8] ; ... ; ldr w8, [x21, #0xd0]
constexpr Expect kDragDropAdd[] = {
    { 9, Operand::Offset, kDragControllerPlayer },
    { 11, Operand::Offset, kDragPlayers + kRefMapBucketCount },
    { 14, Operand::Offset, kDragPlayers + kRefMapBuckets },
    { 18, Operand::Offset, kMapNodeKey },
    { 43, Operand::Move, kDragNodeSize },
    { 44, Operand::CallsImport, 0, "_malloc" },
    { 49, Operand::Offset, kDragNodeData + kDragDataPosition },
    { 50, Operand::PairOffset, kDragNodeData + kDragDataControllers },
    { 51, Operand::Offset, kDragNodeData + kDragDataActiveController },
    { 61, Operand::Offset, kMapNodeKey },
    { 67, Operand::Offset, kDragPlayers + kRefMapItemCount },
};
// The three flags are the bytes the four-byte store at +0x27 clears after
// the position, from +0x28 in the node.
bool CheckDragDropAdd(Code const& f, std::string& problem)
{
    constexpr uint32_t kClearFlags = 0xb802729f;    // stur wzr, [x20, #0x27]
    if (f.At(48) != kClearFlags || kDragNodeData + kDragDataFlags != 0x28) {
        problem = "the drag data's flags are not cleared where they are declared";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ls::TranslatedStringKeyManager (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

// GetTranlatedStringFromKey(key, bool) looks the key up in the map at +0x18
// (bucket count +0x18, buckets +0x20; a node's key at +8) and copies the
// string that follows the key in the node (+0xc).
//     ldr w0, [x22] ; cmn w0, #0x1 ; b.eq ; ldr w8, [x21, #0x18] ; udiv ; msub ; ldr x9, [x21, #0x20]
//     add x8, x9, w8, uxtw #3 ; ldr x8, [x8] ; cbz ; ldr w9, [x8, #0x8] ; cmp ; b.ne ; add x1, x8, #0xc
//     mov x0, x19 ; bl ls::TranslatedString::operator=(ls::TranslatedString const&)
constexpr Expect kTranslatedStringKeyGet[] = {
    { 13, Operand::Offset, kTranslatedKeys + kMapBucketCount },
    { 16, Operand::Offset, kTranslatedKeys + kMapBuckets },
    { 20, Operand::Offset, kMapNodeKey },
    { 23, Operand::Add, kTranslatedKeyString },
    { 25, Operand::Calls, 0, "__ZN2ls16TranslatedStringaSERKS0_" },
};
static_assert(kTranslatedKeyString == kMapNodeIntValue);
// ls::TranslatedString::TranslatedString() makes the handle at +0 (its
// version at +4) and the argument string at +8.
//     str w20, [x19] ; strh wzr, [x19, #0x4] ; ... ; str w8, [x19, #0x8] ; ... ; str w20, [x19, #0x8]
constexpr Expect kTranslatedStringCreate[] = {
    { 12, Operand::Offset, 0 },
    { 17, Operand::Offset, kTranslatedStringArgument },
    { 33, Operand::Offset, kTranslatedStringArgument },
};

// ---------------------------------------------------------------------------
// ls::ImmutableDataHeadmaster (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

// Get<ls::TagManager>() looks the tag manager's type index up in the map at
// the start of the object: the bucket is the index, sign-extended, modulo
// the bucket count (+8); the keys (+0x20) are 4 bytes each, the next keys at
// +0x10, the count of keys at +0x2c, and the managers (+0x30) are pointers.
//     ldrsw x8, [x0, #0x8] ; ... ; ldrsw x10, [x9] ; udiv x9, x10, x8 ; msub ; ldr x9, [x0]
//     ldr x9, [x0, #0x20] ; add x11, x9, x8, lsl #2 ; ... ; ldr x11, [x0, #0x10] ; ldrsw x10, [x0, #0x2c]
//     ldr x9, [x0, #0x30] ; ldr x0, [x9, x8, lsl #3]
constexpr Expect kStaticDataGet[] = {
    { 0, Operand::Offset, kStaticDataManagers + kHashMapBucketCount },
    { 7, Operand::Offset, kStaticDataManagers + kHashMapBuckets },
    { 11, Operand::Offset, kStaticDataManagers + kHashMapKeys },
    { 12, Operand::ShiftLeft, 2 },
    { 16, Operand::Offset, kStaticDataManagers + kHashMapNext },
    { 19, Operand::Offset, kStaticDataManagers + kHashMapKeysSize },
    { 26, Operand::Offset, kStaticDataManagers + kHashMapValues },
};
static_assert(kStaticDataIndexSize == 1 << 2);
// The index is read sign-extended and divided as 64 bits, which is what
// Norbyte's hash of a 32-bit index gives.
bool CheckStaticDataGet(Code const& f, std::string& problem)
{
    constexpr uint32_t kReadIndex = 0xb980012a;     // ldrsw x10, [x9]
    constexpr uint32_t kDivide = 0x9ac80949;        // udiv x9, x10, x8
    if (f.At(4) != kReadIndex || f.At(5) != kDivide) {
        problem = "a type's index is not hashed as itself, sign-extended";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ls::CacheTemplateManagerBase (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

// The constructor (type of handle, whether to synchronise) keeps the type at
// +8 and makes the map of templates by name at +0x10 (its buckets at +0x10,
// their count at +0x18) and the one by handle at +0x50; the lock's owner
// (+0xe0) starts at -1 and the count of its write entries (+0xe8) at 0, and
// the flag is kept at +0x110.
//     strb w1, [x0, #0x8] ; str d8, [x0, #0x18] ; ... ; str x22, [x20, #0x10] ; ldr w8, [x20, #0x18]
//     str x21, [x20, #0x50] ; ldr w8, [x20, #0x58] ; ... ; str x8, [x20, #0xe0] ; str wzr, [x20, #0xe8]
//     strb w19, [x20, #0x110]
constexpr Expect kTemplateCacheCreate[] = {
    { 13, Operand::ByteOffset, kTemplateCacheType },
    { 16, Operand::Offset, kTemplateCacheByName + kHashMapBucketCount },
    { 50, Operand::Offset, kTemplateCacheByName + kHashMapBuckets },
    { 51, Operand::Offset, kTemplateCacheByName + kHashMapBucketCount },
    { 98, Operand::Offset, kTemplateCacheByHandle + kHashMapBuckets },
    { 99, Operand::Offset, kTemplateCacheByHandle + kHashMapBucketCount },
    { 117, Operand::Offset, kTemplateCacheLockOwner },
    { 118, Operand::StoresZero },
    { 118, Operand::Offset, kTemplateCacheLockWrites },
    { 120, Operand::ByteOffset, kTemplateCacheSync },
};

// DecreaseRefCount(handle) takes the lock (+0xd0) for reading unless the
// calling thread owns it (+0xe0), finds the handle's count in the map at
// +0x90, and, when it falls to nothing, takes the lock for writing, counts
// the entry (+0xe8), removes the handle from the counts and from the map of
// templates by handle (+0x50), and, when templates are synchronised
// (+0x110), takes the template out of the added ones (+0xf0) or appends it
// to the removed ones (+0x100).
//     add x21, x0, #0xd0 ; ... ; ldr x8, [x19, #0xe0] ; ... ; bl ReadLock ; ldr w8, [x19, #0x98]
//     ldr x10, [x19, #0x90] ; ldr x8, [x19, #0xb0] ; ldr x11, [x19, #0xa0] ; ldrsw x9, [x19, #0xbc]
//     ldr x9, [x19, #0xc0] ; ... ; bl WriteLock ; ... ; str x8, [x19, #0xe0] ; ldr w9, [x19, #0xe8]
//     ldr w9, [x19, #0x58] ; ldr x10, [x19, #0x50] ; ldr x12, [x19, #0x70] ; ldr x10, [x19, #0x60] ; ldrsw x9, [x19, #0x7c]
//     ldrb w8, [x19, #0x110] ; ldr w10, [x19, #0xfc] ; ldr x8, [x19, #0xf0] ; ... ; ldr w8, [x19, #0x10c] ; ldr w9, [x19, #0x108]
//     ldr x20, [x19, #0x100]
constexpr Expect kTemplateCacheRelease[] = {
    { 13, Operand::Add, kTemplateCacheLock },
    { 19, Operand::Offset, kTemplateCacheLockOwner },
    { 23, Operand::Calls, 0, "__ZN2ls13SRWKernelLock8ReadLockEv" },
    { 24, Operand::Offset, kTemplateCacheRefCounts + kHashMapBucketCount },
    { 29, Operand::Offset, kTemplateCacheRefCounts + kHashMapBuckets },
    { 32, Operand::Offset, kTemplateCacheRefCounts + kHashMapKeys },
    { 37, Operand::Offset, kTemplateCacheRefCounts + kHashMapNext },
    { 40, Operand::Offset, kTemplateCacheRefCounts + kHashMapKeysSize },
    { 56, Operand::Calls, 0, "__ZN2ls13SRWKernelLock10ReadUnlockEv" },
    { 58, Operand::Offset, kTemplateCacheRefCounts + kHashMapValues },
    { 84, Operand::Calls, 0, "__ZN2ls13SRWKernelLock9WriteLockEv" },
    { 88, Operand::Offset, kTemplateCacheLockOwner },
    { 89, Operand::Offset, kTemplateCacheLockWrites },
    { 193, Operand::Offset, kTemplateCacheByHandle + kHashMapBucketCount },
    { 198, Operand::Offset, kTemplateCacheByHandle + kHashMapBuckets },
    { 201, Operand::Offset, kTemplateCacheByHandle + kHashMapKeys },
    { 206, Operand::Offset, kTemplateCacheByHandle + kHashMapNext },
    { 209, Operand::Offset, kTemplateCacheByHandle + kHashMapKeysSize },
    { 318, Operand::ByteOffset, kTemplateCacheSync },
    { 320, Operand::Offset, kTemplateCacheAdded + kArraySize },
    { 323, Operand::Offset, kTemplateCacheAdded },
    { 341, Operand::Offset, kTemplateCacheRemoved + kArraySize },
    { 342, Operand::Offset, kTemplateCacheRemoved + kArrayCapacity },
    { 404, Operand::Offset, kTemplateCacheRemoved },
};
static_assert(kTemplateCacheLockOwner - kTemplateCacheLock == offsetof(GameRecursiveKernelLock, OwnerThread)
    && kTemplateCacheLockWrites - kTemplateCacheLock == offsetof(GameRecursiveKernelLock, WriteEnterCount));

// ---------------------------------------------------------------------------
// ls::LevelManager (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

// eoc::LevelManager's constructor keeps the local template manager at +0x88,
// starts the current level (+0x90) at none and makes the map of levels at
// +0x98 (its bucket count first, the buckets at +0xa0, the count of items
// at +0xa8).
//     stp x21, xzr, [x19, #0x88] ; ... ; str w8, [x19, #0x98] ; str wzr, [x19, #0xa8] ; ... ; str x20, [x19, #0xa0]
constexpr Expect kLevelManagerCreate[] = {
    { 14, Operand::PairOffset, kLevelManagerTemplates },
    { 16, Operand::Offset, kLevelManagerLevels + kMapBucketCount },
    { 17, Operand::Offset, kLevelManagerLevels + kMapItemCount },
    { 52, Operand::Offset, kLevelManagerLevels + kMapBuckets },
};
static_assert(kLevelManagerCurrent == kLevelManagerTemplates + sizeof(void*));
// ls::LevelManager::UnloadAllLevels() unloads each level of the map (a
// node's name at +8, its level at +0x10) and, when the one unloaded is the
// current level (+0x90), clears that; then it empties the map.
//     ldr w8, [x0, #0x98] ; ... ; ldr x9, [x19, #0xa0] ; ... ; ldr w20, [x12, #0x8] ; ... ; ldr x20, [x8, #0x10]
//     bl ls::Level::Unload ; cbz ; ldr x8, [x19, #0x90] ; cmp x20, x8 ; b.ne ; str xzr, [x19, #0x90] ; ... ; str wzr, [x19, #0xa8]
constexpr Expect kLevelManagerUnload[] = {
    { 11, Operand::Offset, kLevelManagerLevels + kMapBucketCount },
    { 17, Operand::Offset, kLevelManagerLevels + kMapBuckets },
    { 25, Operand::Offset, kMapNodeKey },
    { 51, Operand::Offset, kMapNodePointerValue },
    { 53, Operand::Calls, 0, "__ZN2ls5Level6UnloadEv" },
    { 55, Operand::Offset, kLevelManagerCurrent },
    { 58, Operand::Offset, kLevelManagerCurrent },
    { 62, Operand::Offset, kLevelManagerLevels + kMapItemCount },
};

// ---------------------------------------------------------------------------
// ls::TranslatedStringRepository (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

// TryGet(handle, identity, identity): unless the calling thread owns the lock
// (+0xf8) it takes it for reading (+0xf0); it looks in the pool of the two
// identities (+8, rows of three) by the handle's hash alone, comparing the
// handle, then the version (+4) when one is asked for; then in the versioned
// fallback pool (+0x58), then in the fallback pool (+0x50) by the hash alone.
// A pool's map: buckets +0x10 and their count +0x18, the next keys +0x20,
// the keys +0x30 (8 bytes each) and their count +0x3c, the texts +0x40 (16
// bytes each).
constexpr Expect kTranslatedStringGet[] = {
    { 30, Operand::Offset, kTranslatedLockOwner },
    { 38, Operand::Move, kTranslatedPoolRow },
    { 43, Operand::Offset, kTranslatedPools },
    { 44, Operand::Offset, kTextPoolTexts + kHashMapBucketCount },
    { 47, Operand::Calls, 0, "__ZNK2ls11FixedString7GetHashEv" },
    { 51, Operand::Offset, kTextPoolTexts + kHashMapBuckets },
    { 54, Operand::Offset, kTextPoolTexts + kHashMapKeys },
    { 56, Operand::ShiftLeft, 3 },
    { 60, Operand::Offset, kTextPoolTexts + kHashMapNext },
    { 63, Operand::Offset, kTextPoolTexts + kHashMapKeysSize },
    { 83, Operand::Offset, kTextPoolTexts + kHashMapValues },
    { 84, Operand::ShiftLeft, 4 },
    { 87, Operand::Offset, kTextHandleVersion },
    { 97, Operand::Add, kTranslatedLock },
    { 132, Operand::Offset, kTranslatedVersionedFallbackPool },
    { 213, Operand::Offset, kTranslatedFallbackPool },
    { 217, Operand::Calls, 0, "__ZNK2ls11FixedString7GetHashEv" },
};
static_assert(kTextSize == 1 << 4 && kTranslatedPoolRow * 3 == kTranslatedPoolCount * sizeof(void*));
// In the main pool and the fallback one only the handle is compared: the
// first comparison of each is of the 32 bits of the fixed string.
bool CheckTranslatedStringGet(Code const& f, std::string& problem)
{
    constexpr uint32_t kCompareHandles = 0x6b0b019f;    // cmp w12, w11
    constexpr uint32_t kCompareFallbackHandles = 0x6b0a019f;    // cmp w12, w10
    if (f.At(58) != kCompareHandles || f.At(228) != kCompareFallbackHandles || RegN(f.At(57)) != 10 || RegN(f.At(227)) != 11) {
        problem = "a pool's texts are not found by comparing the handle alone";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// eoc::EoCGlobalSwitches (Layout/GameSwitches.h)
// ---------------------------------------------------------------------------

// App::CreateGlobalSwitches allocates the object and constructs it.
//     mov w0, #0x1440 ; bl malloc ; ... ; bl eoc::EoCGlobalSwitches::EoCGlobalSwitches()
constexpr Expect kSwitchesCreate[] = {
    { 24, Operand::Move, kSwitchesSize },
    { 25, Operand::CallsImport, 0, "_malloc" },
    { 37, Operand::Calls, 0, "__ZN3eoc17EoCGlobalSwitchesC2Ev" },
};

// ls::GlobalSwitches::Reset empties the language's text, a string of 16
// bytes whose last byte says where the text is, and sets the base class's
// last members: three seconds, 25 frames, a tenth.
//     strb wzr, [x0, #0xc0] ; strb wzr, [x0, #0xcf]
//     str w8, [x0, #0x160] ; ... ; str w8, [x0, #0x180] ; ... ; str w8, [x0, #0x188] ; ret
constexpr Expect kSwitchesBaseReset[] = {
    { 54, Operand::ByteOffset, kSwitchesLanguage },
    { 55, Operand::ByteOffset, kSwitchesLanguage + kGameStringBytes - 1 },
    { 135, Operand::Offset, kSwitchesLongRestScriptWaitingTime },
    { 147, Operand::Move, 25 },
    { 148, Operand::Offset, kSwitchesBaseLastInt },
    { 153, Operand::Offset, kSwitchesBaseLast },
};

// The constructor: the base class's first; then the sound settings one by
// one, the first at 0x1a8: a byte at +0x40, a translated string at +0x44, a
// function's address at +0x58, and the next setting 0x68 bytes on. The four
// cameras' switches and the control switches are addressed before that.
//     add x25, x0, #0xe14 ; ... ; add x28, x0, #0x7c4 ; bl ls::GlobalSwitches::GlobalSwitches()
//     add x20, x0, #0x1a8 ; ... ; add x0, x0, #0x1ec ; ... ; strb wzr, [x19, #0x1e8] ; ... ; bl ls::TranslatedString::TranslatedString()
//     str x23, [x19, #0x200] ; add x8, x19, #0x210
constexpr Expect kSwitchesConstruct[] = {
    { 14, Operand::Add, kSwitchesControls },
    { 18, Operand::Add, kSwitchesCameras },
    { 19, Operand::Calls, 0, "__ZN2ls14GlobalSwitchesC2Ev" },
    { 23, Operand::Add, kSwitchesSoundSettings },
    { 29, Operand::Add, kSwitchesSoundSettings + kSoundSettingText },
    { 31, Operand::ByteOffset, kSwitchesSoundSettings + kSoundSettingFlag },
    { 33, Operand::Calls, 0, "__ZN2ls16TranslatedStringC1Ev" },
    { 36, Operand::Offset, kSwitchesSoundSettings + kSoundSettingProc },
    { 37, Operand::Add, kSwitchesSoundSettings + kSoundSettingSize },
};

// eoc::EoCGlobalSwitches::Reset: the categories the hotbar is filled with,
// 16 bits, and the first 16 bytes of the aligned array.
//     strh w8, [x19, #0x1332] ; ... ; str q0, [x19, #0xeb0]
constexpr Expect kSwitchesReset[] = {
    { 710, Operand::Offset, kSwitchesAutoFillHotbarCategories },
    { 814, Operand::Offset, kSwitchesAlignedArray },
};

// App::InitAppConfig gives the configuration the default of every name: the
// name's fixed string, and the value the object has in the member.
//     ldr x0, [x19, #0x158] ; adrp x8, <the name> ; ldr x1, [x8, ...] ; ldr x8, [x21, #0xf40] ; ldr(b) w2 / ldr s0, [x8, #member]
//     bl ls::Config::SetDefaultBoolValue / SetDefaultIntegerValue / SetDefaultFloatValue
// A sound setting's name is in the setting itself:
//     add x1, x8, #0x1b4 ; ldr s0, [x8, #0x1b0] ; bl ls::Config::SetDefaultFloatValue      (the first, the second and the twelfth)
constexpr Expect kSwitchesDefaults[] = {
    { 50, Operand::Add, kSwitchesSoundSettings + kSoundSettingKey },
    { 51, Operand::Offset, kSwitchesSoundSettings + kSoundSettingValue },
    { 55, Operand::Add, kSwitchesSoundSettings + kSoundSettingSize + kSoundSettingKey },
    { 56, Operand::Offset, kSwitchesSoundSettings + kSoundSettingSize + kSoundSettingValue },
    { 105, Operand::Add, kSwitchesSoundSettings + 11 * kSoundSettingSize + kSoundSettingKey },
    { 106, Operand::Offset, kSwitchesSoundSettings + 11 * kSoundSettingSize + kSoundSettingValue },
    { 112, Operand::Offset, kSwitchesSoundVolumeDynamicRange },
    { 118, Operand::Offset, kSwitchesPointAndClickSoundFrequency },
    { 124, Operand::ByteOffset, kSwitchesMuteSoundWhenNotFocused },
    { 130, Operand::ByteOffset, kSwitchesCentreDialogue },
    { 136, Operand::ByteOffset, kSwitchesForceMono },
    { 142, Operand::Offset, kSwitchesSoundOcclusionQuality },
    { 148, Operand::Offset, kSwitchesSoundPartyWinningThreshold },
    { 154, Operand::Offset, kSwitchesSoundPartyLosingThreshold },
    { 165, Operand::Offset, kSwitchesGameCameraSpeedMultiplier },
    { 246, Operand::Offset, kSwitchesMaxAmountDialogsInLog },
    { 257, Operand::Offset, kSwitchesControllerCharacterWalkThreshold },
    { 263, Operand::Offset, kSwitchesControllerCharacterRunThreshold },
    { 269, Operand::Offset, kSwitchesControllerRumbleStrength },
    { 325, Operand::ByteOffset, kSwitchesEscClosesAllUI },
    { 331, Operand::Offset, kSwitchesNrOfQuickSaves },
    { 337, Operand::Offset, kSwitchesNrOfAutoSaves },
    { 343, Operand::ByteOffset, kSwitchesCanAutoSave },
    { 349, Operand::ByteOffset, kSwitchesCanCrossSave },
    { 355, Operand::ByteOffset, kSwitchesFirstTimeWithCrossSave },
    { 361, Operand::ByteOffset, kSwitchesHasSeenEAMsg },
    { 367, Operand::ByteOffset, kSwitchesCanSaveDuringTimeline },
    { 373, Operand::ByteOffset, kSwitchesCanLoadDuringTimeline },
    { 379, Operand::ByteOffset, kSwitchesForceSplitscreen },
    { 390, Operand::ByteOffset, kSwitchesPeaceCharacterHighlightMode },
    { 396, Operand::ByteOffset, kSwitchesCombatCharacterHighlightMode },
    { 402, Operand::ByteOffset, kSwitchesTacticalCharacterHighlightMode },
    { 408, Operand::ByteOffset, kSwitchesUnitSystem },
    { 414, Operand::ByteOffset, kSwitchesPostCombatAssignation },
    { 430, Operand::ByteOffset, kSwitchesShowTextBackground },
    { 436, Operand::Offset, kSwitchesTextBackgroundOpacity },
    { 713, Operand::Offset, kSwitchesLongRestScriptWaitingTime },
    { 719, Operand::Offset, kSwitchesStartDay },
    { 725, Operand::Offset, kSwitchesStartYear },
    { 746, Operand::ByteOffset, kSwitchesWeightedRolls },
};

// App::CopyValuesToConfig stores every member under its name: the member is
// loaded, then the name's fixed string, whose entry of the configuration
// gets the value.
//     ldr(b) w11, [x8, #member] ; adrp x12, <the name> ; ldr w13, [x12, ...] ; ... ; str w11, [x12, #0x18]
constexpr Expect kSwitchesToConfig[] = {
    { 12, Operand::ByteOffset, kSwitchesUseLevelCache },
    { 29, Operand::ByteOffset, kSwitchesUseEndTurnFallback },
    { 63, Operand::ByteOffset, kSwitchesLoadAllEffectPools },
    { 80, Operand::ByteOffset, kSwitchesScriptLog },
    { 114, Operand::ByteOffset, kSwitchesCheckRequirements },
    { 131, Operand::ByteOffset, kSwitchesShowOriginIntroInCC },
    { 340, Operand::Offset, kSwitchesSoundVolumeDynamicRange },
    { 357, Operand::Offset, kSwitchesPointAndClickSoundFrequency },
    { 374, Operand::ByteOffset, kSwitchesMuteSoundWhenNotFocused },
    { 391, Operand::ByteOffset, kSwitchesCentreDialogue },
    { 408, Operand::ByteOffset, kSwitchesForceMono },
    { 425, Operand::Offset, kSwitchesSoundOcclusionQuality },
    { 442, Operand::Offset, kSwitchesSoundPartyWinningThreshold },
    { 459, Operand::Offset, kSwitchesSoundPartyLosingThreshold },
    { 493, Operand::Offset, kSwitchesGameCameraSpeedMultiplier },
    { 510, Operand::ByteOffset, kSwitchesGameCameraRotationLocked },
    { 527, Operand::ByteOffset, kSwitchesGameCameraEnableCloseUpDialog },
    { 544, Operand::ByteOffset, kSwitchesGameCameraEnableDynamicCombatCamera },
    { 561, Operand::ByteOffset, kSwitchesGameCameraEnableAttackCamera },
    { 578, Operand::ByteOffset, kSwitchesGameCameraEnableAttackCameraOtherPlayers },
    { 595, Operand::ByteOffset, kSwitchesGameCameraEnableFlyFollow },
    { 629, Operand::Offset, kSwitchesGameCameraAutoRotateSpeed },
    { 697, Operand::ByteOffset, kSwitchesCameraShakeEnabled },
    { 714, Operand::ByteOffset, kSwitchesMouseLock },
    { 731, Operand::ByteOffset, kSwitchesDisableEdgePanning },
    { 748, Operand::Offset, kSwitchesMaxAmountDialogsInLog },
    { 765, Operand::Offset, kSwitchesControllerCharacterWalkThreshold },
    { 782, Operand::Offset, kSwitchesControllerCharacterRunThreshold },
    { 799, Operand::Offset, kSwitchesControllerRumbleStrength },
    { 862, Operand::ByteOffset, kSwitchesEnableVoiceLogging },
    { 879, Operand::ByteOffset, kSwitchesEnableLongPressAndHold },
    { 913, Operand::ByteOffset, kSwitchesEscClosesAllUI },
    { 930, Operand::Offset, kSwitchesNrOfQuickSaves },
    { 947, Operand::Offset, kSwitchesNrOfAutoSaves },
    { 964, Operand::ByteOffset, kSwitchesCanAutoSave },
    { 981, Operand::ByteOffset, kSwitchesCanCrossSave },
    { 998, Operand::ByteOffset, kSwitchesFirstTimeWithCrossSave },
    { 1015, Operand::ByteOffset, kSwitchesHasSeenEAMsg },
    { 1032, Operand::ByteOffset, kSwitchesCanSaveDuringTimeline },
    { 1049, Operand::ByteOffset, kSwitchesCanLoadDuringTimeline },
    { 1066, Operand::ByteOffset, kSwitchesForceSplitscreen },
    { 1100, Operand::ByteOffset, kSwitchesPeaceCharacterHighlightMode },
    { 1117, Operand::ByteOffset, kSwitchesCombatCharacterHighlightMode },
    { 1134, Operand::ByteOffset, kSwitchesTacticalCharacterHighlightMode },
    { 1151, Operand::ByteOffset, kSwitchesUnitSystem },
    { 1168, Operand::ByteOffset, kSwitchesPostCombatAssignation },
    { 1185, Operand::ByteOffset, kSwitchesShowSubtitles },
    { 1202, Operand::ByteOffset, kSwitchesDisplaySpeaker },
    { 1219, Operand::ByteOffset, kSwitchesShowTextBackground },
    { 1236, Operand::Offset, kSwitchesTextBackgroundOpacity },
    { 1253, Operand::Offset, kSwitchesDialogueFontSizeModifier },
    { 1270, Operand::Offset, kSwitchesOverheadFontSizeModifier },
    { 1304, Operand::ByteOffset, kSwitchesUseRadialContextMenu },
    { 1338, Operand::ByteOffset, kSwitchesNorthFacingMinimap },
    { 1406, Operand::ByteOffset, kSwitchesHoldToEndTurn },
    { 1459, Operand::Offset, kSwitchesAutoFillHotbarCategories },
    { 1476, Operand::ByteOffset, kSwitchesAlwaysShowSplitter },
    { 1697, Operand::ByteOffset, kSwitchesAutoListenEnabled },
    { 1714, Operand::ByteOffset, kSwitchesTwitchEnableOverlay },
};

// ---------------------------------------------------------------------------
// The cursor's name and overlay (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

// EocClientSDM::Init makes the cursor control, when there is none, with
// malloc(0x40): hidden, the visible timer -1 and the fade timer 0.5 (+0x14,
// one 64-bit store), the name none (+0x1c), the overlay none and the cursor
// the default one (+0x20 and +0x24, one 64-bit store), no overlay asked for
// (+0x28) and no forced cursors (+0x30); then it keeps it in
// ecl::CursorControl::m_ptr.
//     adrp x20, page ; ldr x8, [x20, #off] ; cbnz ; ... ; mov w0, #0x40 ; bl _malloc ; ... ; strb wzr, [x19, #0x10] ; adrp ; ldr d0 ; stur d0, [x19, #0x14]
//     mov w8, #-0x1 ; str w8, [x19, #0x1c] ; adrp x8, page ; ldr d0, [x8, #off] ; str d0, [x19, #0x20] ; str wzr, [x19, #0x28] ; stp xzr, xzr, [x19, #0x30]
//     str x19, [x20, #off]
constexpr Expect kCursorCreate[] = {
    { 125, Operand::Move, kCursorSize },
    { 126, Operand::CallsImport, 0, "_malloc" },
    { 141, Operand::ByteOffset, kCursorShow },
    { 144, Operand::Unscaled, kCursorVisibleTimer },
    { 145, Operand::Move, 0xffffffff },
    { 146, Operand::Offset, kCursorName },
    { 149, Operand::Offset, kCursorOverlay },
    { 150, Operand::Offset, kCursorRequestedOverlay },
    { 151, Operand::PairOffset, kCursorOverrides },
};
static_assert(kCursorFadeTimer == kCursorVisibleTimer + sizeof(float) && kCursorRequested == kCursorOverlay + kCursorTypeSize
    && kCursorOverrides + 0x10 == kCursorSize);
bool CheckCursorCreate(Code const& f, std::string& problem)
{
    uint64_t control = f.Image.FindVA("__ZN3ecl13CursorControl5m_ptrE");
    if (!control || LoadedAddress(f, 102) != control) {
        problem = "it does not test ecl::CursorControl::m_ptr before it makes the control";
        return false;
    }
    constexpr uint32_t kKeep = 0xf9000293, kKeepMask = 0xffc003ff;     // str x19, [x20, #...]
    if ((f.At(152) & kKeepMask) != kKeep || LoadStoreOffset(f.At(152)) != (uint32_t)(control & 0xfff)) {
        problem = "the control it makes is not kept in ecl::CursorControl::m_ptr";
        return false;
    }
    // The overlay and the cursor: none (0) and the default one (1).
    uint64_t initial = LoadedAddress(f, 147);
    auto words = initial ? f.Image.DataAt(initial, 8) : nullptr;
    uint32_t values[2]{ 1, 0 };
    if (words) memcpy(values, words, sizeof(values));
    if (!words || values[0] != kCursorNoOverlay || values[1] != kCursorDefault) {
        problem = "the overlay and the cursor are not made none and the default one";
        return false;
    }
    return true;
}
// ~CursorControl() lets the name (+0x1c) go, when it has one.
//     ldr w8, [x19, #0x1c] ; cmn w8, #0x1 ; b.eq ; str w8, [sp, #0x8] ; ... ; bl ls::gst::Map::Release
constexpr Expect kCursorDestroy[] = {
    { 87, Operand::Offset, kCursorName },
    { 88, Operand::Add, 1 },
    { 96, Operand::Calls, 0, "__ZN2ls3gst3Map7ReleaseERKNS0_12NodePoolDataE" },
};

// ---------------------------------------------------------------------------
// The levels' template managers (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

// GetOrCreateLevel(name) finds the name's level data through the local
// template manager (+0x88 in the level manager), its level data manager
// (+0x40) and that manager's map of levels (+8, a hash map: the name's hash
// from ls::FixedString::GetHash modulo the bucket count, in 32 bits), then
// the level in the map of levels (+0x98).
//     ldr x8, [x0, #0x88] ; ldr x21, [x8, #0x40] ; ldr w8, [x21, #0x10] ; cbz ; ... ; ldr w0, [x1] ; bl ls::FixedString::GetHash()
//     ldr w8, [x21, #0x10] ; udiv w9, w0, w8 ; msub ; ldr x9, [x21, #0x8] ; ... ; ldr x10, [x21, #0x28] ; ... ; ldr x11, [x21, #0x18] ; ...
//     ldr x10, [x21, #0x38] ; ldr x1, [x10, x9, lsl #3] ; ... ; ldr w9, [x19, #0x98]
constexpr Expect kLevelDataFind[] = {
    { 6, Operand::Offset, kLevelManagerTemplates },
    { 7, Operand::Offset, kTemplatesLevelData },
    { 8, Operand::Offset, kLevelDataLevels + kHashMapBucketCount },
    { 13, Operand::Calls, 0, "__ZNK2ls11FixedString7GetHashEv" },
    { 17, Operand::Offset, kLevelDataLevels + kHashMapBuckets },
    { 20, Operand::Offset, kLevelDataLevels + kHashMapKeys },
    { 25, Operand::Offset, kLevelDataLevels + kHashMapNext },
    { 29, Operand::Offset, kLevelDataLevels + kHashMapValues },
    { 32, Operand::Offset, kLevelManagerLevels + kMapBucketCount },
};
// ls::LevelData::Unload(name, keep) looks the name up in the templates the
// level's data tracks (+0x18, a hash map hashed as the map of levels) and
// empties the array it finds there: a value is 16 bytes (an array of
// templates, whose size it reads and clears; the not-found error is
// ls::Result<ls::DynamicArray<ls::GameObjectTemplate*>&>).
//     ldr w8, [x20, #0x20] ; ... ; bl ls::FixedString::GetHash() ; ... ; ldr x9, [x20, #0x18] ; ... ; ldr x9, [x20, #0x38]
//     ... ; ldr x11, [x20, #0x28] ; ... ; ldr x9, [x20, #0x48] ; add x21, x9, x8, lsl #4 ; ... ; ldr w8, [x21, #0xc]
constexpr Expect kLevelTrackedTemplates[] = {
    { 271, Operand::Offset, kLevelDataTrackedTemplates + kHashMapBucketCount },
    { 274, Operand::Calls, 0, "__ZNK2ls11FixedString7GetHashEv" },
    { 278, Operand::Offset, kLevelDataTrackedTemplates + kHashMapBuckets },
    { 281, Operand::Offset, kLevelDataTrackedTemplates + kHashMapKeys },
    { 286, Operand::Offset, kLevelDataTrackedTemplates + kHashMapNext },
    { 293, Operand::Calls, 0, "__ZNK2ls6ResultIRNS_12DynamicArrayIPNS_18GameObjectTemplateENS_15TaggedAllocatorIiEEEENS_5ErrorEJEE5RaiseEv" },
    { 295, Operand::Offset, kLevelDataTrackedTemplates + kHashMapValues },
    { 296, Operand::ShiftLeft, 4 },
    { 298, Operand::Offset, kArraySize },
};
bool CheckLevelDataHash(Code const& f, std::string& problem)
{
    constexpr uint32_t kDivide = 0x1ac80809;        // udiv w9, w0, w8
    if (f.At(15) != kDivide) {
        problem = "a name's hash is not taken modulo the bucket count in 32 bits";
        return false;
    }
    return true;
}
// esv::LevelManager::CreateLevel(data) makes the level with malloc(0xf0),
// keeps the manager's local template manager (+0x88) at +0x20 in it, and
// makes its template cache: malloc(0x118), a CacheTemplateManagerBase of
// level templates kept in sync, the level's name at +0x114; kept at +0xe8.
//     mov w0, #0xf0 ; bl _malloc ; ... ; ldr x8, [x21, #0x88] ; ... ; str x8, [x19, #0x20] ; ... ; mov w0, #0x118 ; bl _malloc ; ...
//     mov w1, #0x5 ; mov w2, #0x1 ; bl ls::CacheTemplateManagerBase::CacheTemplateManagerBase ; ... ; str w20, [x21, #0x114] ; ...
//     stp x9, x21, [x19, #0xe0]
constexpr Expect kServerLevelCreate[] = {
    { 30, Operand::Move, kServerLevelSize },
    { 31, Operand::CallsImport, 0, "_malloc" },
    { 42, Operand::Offset, kLevelManagerTemplates },
    { 46, Operand::Offset, kLevelTemplates },
    { 339, Operand::Move, kLevelTemplateCacheSize },
    { 340, Operand::CallsImport, 0, "_malloc" },
    { 353, Operand::Move, kLevelTemplateCacheType },
    { 354, Operand::Move, 1 },
    { 355, Operand::Calls, 0, "__ZN2ls24CacheTemplateManagerBaseC2ENS_19ETemplateHandleTypeEb" },
    { 364, Operand::Offset, kLevelTemplateCacheName },
    { 375, Operand::PairOffset, kServerLevelTemplateCache - sizeof(void*) },
};
bool CheckServerLevelCache(Code const& f, std::string& problem)
{
    constexpr uint32_t kSecond = 21;                // stp x9, x21: the cache is the second register
    if (((f.At(375) >> 10) & 31) != kSecond || f.At(341) != 0xaa0003f5) {     // mov x21, x0
        problem = "the template cache it makes is not the one it keeps";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The rest of ls::InputManager (Layout/GameGlobals.h)
// ---------------------------------------------------------------------------

constexpr uint64_t kSpinCountOffset = offsetof(GameCriticalSection, SpinCount);
constexpr uint64_t kRecursionOffset = offsetof(GameCriticalSection, RecursionCount);
constexpr uint64_t kOwnerOffset = offsetof(GameCriticalSection, OwnerThread);
constexpr uint64_t kLockCountOffset = offsetof(GameCriticalSection, LockCount);
static_assert(kInputAfterLocks == kInputInjectLock + sizeof(GameCriticalSection) && kInputInjectLock == kInputLock + sizeof(GameCriticalSection));

// CoreLibSDM::Init makes the input manager, when there is none, with
// malloc(0x5f8): both critical sections free (the first 16 bytes of a free
// one, then its spin count), zero from +0x28 on, the definitions' 313
// buckets at +0xc8 (their count at +0xc4); the modifiers, the time and both
// flags zero, the three listener groups empty, the raw inputs empty, the
// remap's listener none and its binding none (-1, no modifiers, player
// 0xff), the events made with room for eight, the players' devices -1, the
// translator the default one and the eight bytes after it zero; then it
// keeps it in ls::InputManager::m_ptr.
//     adrp x26, page ; ldr x8, [x26, #off] ; cbnz ; ... ; mov w0, #0x5f8 ; bl _malloc ; ... ; ldr q0, [sp, #0x60] ; str q0, [x19]
//     mov w8, #0xfa0 ; str w8, [x19, #0x10] ; mov x22, x19 ; str q0, [x22, #0x14]! ; str w8, [x19, #0x24] ; movi.2d v0, #0 ; stur q0, [x19, #0x28] ...
//     ... ; str x20, [x19, #0xc8] ; ... ; mov w8, #0x139 ; str w8, [x19, #0xc4] ; ...
//     strb wzr, [x19, #0x2a8] ; str xzr, [x19, #0x2b0] ; strh wzr, [x19, #0x2b8] ; ... ; str wzr, [x19, #0x2c0] ; strb wzr, [x19, #0x2e8] ; ...
//     str q0, [x19, #0x350] ; str xzr, [x19, #0x378] ; ... ; str w8, [x19, #0x380] ; strb wzr, [x19, #0x384] ; ... ; strb w8, [x19, #0x386]
//     ... ; mov w0, #0x1c0 ; bl _malloc ; ... ; str x20, [x19, #0x388] ; ... ; str wzr, [x19, #0x3c4] ; ... ; str x8, [x19, #0x408] ; ...
//     adrp x9, page ; add x9, x9, #off ; str x9, [x19, #0x5e8] ; ... ; str d0, [x19, #0x5f0] ; ... ; str x8, [x19, #0x360] ; ... ; str x19, [x27, #off]
constexpr Expect kInputCreate[] = {
    { 5486, Operand::Move, kInputSize },
    { 5487, Operand::CallsImport, 0, "_malloc" },
    { 5499, Operand::Offset, kInputLock },
    { 5500, Operand::Move, (uint32_t)kFreeCriticalSection.SpinCount },
    { 5501, Operand::Offset, kInputLock + kSpinCountOffset },
    { 5503, Operand::Unscaled, kInputInjectLock },
    { 5504, Operand::Offset, kInputInjectLock + kSpinCountOffset },
    { 5506, Operand::Unscaled, kInputAfterLocks },
    { 5548, Operand::Offset, kInputDefinitions + kRefMapBuckets },
    { 5558, Operand::Offset, kInputDefinitions + kRefMapBucketCount },
    { 6595, Operand::ByteOffset, kInputModifiers },
    { 6596, Operand::Offset, kInputUpdateTime },
    { 6597, Operand::Offset, kInputFlags },
    { 6599, Operand::Offset, kInputListeners },
    { 6600, Operand::ByteOffset, kInputListeners + kListenerGroupCompact },
    { 6605, Operand::Offset, kInputDeviceListeners },
    { 6606, Operand::ByteOffset, kInputDeviceListeners + kListenerGroupCompact },
    { 6610, Operand::Offset, kInputNotifyUpListeners },
    { 6611, Operand::ByteOffset, kInputNotifyUpListeners + kListenerGroupCompact },
    { 6614, Operand::Offset, kInputRawInputs },
    { 6615, Operand::Offset, kInputRemap },
    { 6617, Operand::Offset, kInputRemapBinding },
    { 6618, Operand::ByteOffset, kInputRemapBinding + kBindingModifiers },
    { 6620, Operand::ByteOffset, kInputRemapBinding + kBindingPlayer },
    { 6645, Operand::Move, kInputEventsMade * kInputEventSize },
    { 6646, Operand::CallsImport, 0, "_malloc" },
    { 6658, Operand::Offset, kInputEvents },
    { 6660, Operand::Offset, kInputDeviceEvents + kArraySize },
    { 6666, Operand::Offset, kInputDevices },
    { 6746, Operand::Offset, kInputTranslator },
    { 6748, Operand::Offset, kInputTranslator + sizeof(void*) },
    { 6751, Operand::Offset, kInputPlayerDevices },
};
static_assert(kInputTranslator + 2 * sizeof(void*) == kInputSize && kInputDeviceListeners == kInputListeners + kListenerGroupSize
    && kInputNotifyUpListeners == kInputDeviceListeners + kListenerGroupSize && kInputRawInputs == kInputNotifyUpListeners + kListenerGroupSize);
bool CheckInputCreate(Code const& f, std::string& problem)
{
    uint64_t manager = f.Image.FindVA("__ZN2ls12InputManager5m_ptrE");
    uint64_t translator = f.Image.FindVA("__ZN2ls24KeyboardLayoutTranslator9s_DefaultE");
    if (!manager || LoadedAddress(f, 5463) != manager) {
        problem = "it does not test ls::InputManager::m_ptr before it makes the manager";
        return false;
    }
    constexpr uint32_t kKeep = 0xf9000013, kKeepMask = 0xffc0001f;   // str x19, [x?, #...]
    if ((f.At(6762) & kKeepMask) != kKeep || LoadStoreOffset(f.At(6762)) != (uint32_t)(manager & 0xfff)) {
        problem = "the manager it makes is not kept in ls::InputManager::m_ptr";
        return false;
    }
    if (!translator || PageAddress(f.VA(6744), f.At(6744), f.At(6745)) != translator) {
        problem = "the translator is not the default one";
        return false;
    }
    return true;
}

// InjectInput(change, raw) appends the change to the injected inputs (+0x3a8:
// capacity +0x3b0, size +0x3b4; 0x14 bytes each, `raw` at +0x10) under the
// second critical section (+0x14), which it leaves inline: its entries at
// +0x1c, its owner at +0x20, its count at +0x18.
//     add x19, x0, #0x14 ; mov x0, x19 ; bl ls::CriticalSection::Lock() ; ldr w8, [x20, #0x3b0] ; ldr w26, [x20, #0x3b4] ; ...
//     ldr x22, [x20, #0x3a8] ; ... ; mov w9, #0x14 ; smaddl x8, w26, w9, x8 ; ... ; strb w21, [x8, #0x10]
//     ldr w8, [x20, #0x1c] ; subs ; str w8, [x20, #0x1c] ; ... ; str wzr, [x20, #0x20] ; add x8, x20, #0x18 ; ldaddal
constexpr Expect kInputInjectRaw[] = {
    { 19, Operand::Add, kInputInjectLock },
    { 21, Operand::Calls, 0, "__ZN2ls15CriticalSection4LockEv" },
    { 22, Operand::Offset, kInputInjects + kArrayCapacity },
    { 23, Operand::Offset, kInputInjects + kArraySize },
    { 36, Operand::Offset, kInputInjects },
    { 176, Operand::Move, kInjectSize },
    { 187, Operand::ByteOffset, kInjectRaw },
    { 189, Operand::Offset, kInputInjectLock + kRecursionOffset },
    { 197, Operand::Offset, kInputInjectLock + kOwnerOffset },
    { 198, Operand::Add, kInputInjectLock + kLockCountOffset },
};
// InjectInput(data) reads the pressed modifiers (+0x2a8) and the device's
// states (+0x80), adds to the modifiers, and tells the listeners of the
// group at +0x2c0 (its listeners +0x2c8, their count +0x2d4), compacting
// them afterwards when the group says so (+0x2e8).
//     ldrb w9, [x0, #0x2a8] ; ... ; add x0, x0, #0x80 ; ... ; ldrb w9, [x19, #0x2a8] ; orr ; strb w21, [x19, #0x2a8] ; add x22, x19, #0x2c0
//     bl ls::InputListenerGroup::StartTraversingListeners() ; ldr x20, [x19, #0x2c8] ; ... ; ldr w8, [x19, #0x2d4] ; ... ; ldrb w8, [x19, #0x2e8]
constexpr Expect kInputInjectData[] = {
    { 14, Operand::ByteOffset, kInputModifiers },
    { 16, Operand::Add, kInputStates },
    { 172, Operand::ByteOffset, kInputModifiers },
    { 173, Operand::Add, kInputListeners },
    { 175, Operand::Calls, 0, "__ZN2ls18InputListenerGroup24StartTraversingListenersEv" },
    { 176, Operand::Offset, kInputListeners + kListenerGroupListeners },
    { 180, Operand::Offset, kInputListeners + kListenerGroupListeners + kArraySize },
    { 208, Operand::ByteOffset, kInputListeners + kListenerGroupCompact },
};
// FireInputEvents(events, ...) reads the first flag (+0x2b8), tells the
// listeners at +0x2c0 and empties the events (+0x388, their size +0x394).
//     ldrb w11, [x20, #0x2b8] ; ... ; add x19, x20, #0x2c0 ; ... ; bl StartTraversingListeners ; ldr x22, [x20, #0x2c8] ; ... ; add x19, x20, #0x388
//     ... ; str wzr, [x20, #0x394]
constexpr Expect kInputFire[] = {
    { 125, Operand::ByteOffset, kInputFlags },
    { 193, Operand::Add, kInputListeners },
    { 196, Operand::Calls, 0, "__ZN2ls18InputListenerGroup24StartTraversingListenersEv" },
    { 198, Operand::Offset, kInputListeners + kListenerGroupListeners },
    { 207, Operand::Add, kInputEvents },
    { 236, Operand::Offset, kInputEvents + kArraySize },
};
// InjectPress(event, ...) finds the event's player (+8 in it) in the
// repeats (+0x40: the keys' capacity +0x68 and size +0x6c, the values +0x70
// and their capacity +0x78, 0x60 bytes each) and reads the time of the last
// update (+0x2b0).
//     ldr x25, [x1], #0x8 ; add x0, x0, #0x40 ; ldrsw x21, [x20, #0x6c] ; bl HashTable<EInputPlayerIndex>::Ensure ; ... ; ldr w1, [x20, #0x68]
//     ldr w8, [x20, #0x78] ; ... ; ldr x8, [x0, #0x70]! ; ... ; mov w9, #0x60 ; ... ; ldr d8, [x20, #0x2b0]
constexpr char const kPlayerIndexEnsure[] =
    "__ZN2ls9HashTableINS_17EInputPlayerIndexENS_19HashTableOpsDefaultEE6EnsureIRKS1_EENSt3__15tupleIJibEEEOT_";
constexpr Expect kInputPress[] = {
    { 13, Operand::Add, kInputRepeats },
    { 14, Operand::Offset, kInputRepeats + kHashMapKeysSize },
    { 15, Operand::Calls, 0, kPlayerIndexEnsure },
    { 17, Operand::Offset, kInputRepeats + kHashMapKeys + kArrayCapacity },
    { 18, Operand::Offset, kInputRepeats + kHashMapValues + kArrayCapacity },
    { 22, Operand::Unscaled, kInputRepeats + kHashMapValues },
    { 24, Operand::Move, kInputRepeatSize },
    { 95, Operand::Offset, kInputUpdateTime },
};
// The repeats' key: HashTable<EInputPlayerIndex>::Ensure hashes the one-byte
// index sign-extended, compares it a byte at a time and keeps it in a byte.
//     ldr w9, [x0, #0x8] ; ... ; ldrsb x8, [x20] ; udiv x10, x8, x9 ; msub ; ... ; ldr x9, [x19, #0x20] ; ldrb w10, [x9, x0] ; ... ; strb w10, [x9, w8, sxtw]
constexpr Expect kPlayerIndexKey[] = {
    { 6, Operand::Offset, kHashMapBucketCount },
    { 16, Operand::Offset, kHashMapKeys },
};
bool CheckPlayerIndexKey(Code const& f, std::string& problem)
{
    constexpr uint32_t kSignExtend = 0x39800288;    // ldrsb x8, [x20]
    constexpr uint32_t kDivide = 0x9ac9090a;        // udiv x10, x8, x9
    constexpr uint32_t kKeyRead = 0x3860692a;       // ldrb w10, [x9, x0]
    constexpr uint32_t kKeyWrite = 0x3828c92a;      // strb w10, [x9, w8, sxtw]
    if (f.At(9) != kSignExtend || f.At(10) != kDivide || f.At(17) != kKeyRead || f.At(43) != kKeyWrite) {
        problem = "a player's index is not hashed sign-extended, or not kept in a byte";
        return false;
    }
    return true;
}
// RegisterDevice(device) appends a device event to +0x3b8 (capacity +0x3c0,
// size +0x3c4; eight bytes each, the event's id zero and the device at +4).
//     ldr w9, [x19, #0x3c0] ; ldr w8, [x19, #0x3c4] ; ... ; add x0, x19, #0x3b8 ; bl DynamicArray<InjectDeviceEvent>::Reallocate ; ...
//     str w9, [x19, #0x3c4] ; ldr x9, [x19, #0x3b8] ; add x8, x9, w8, sxtw #3 ; str wzr, [x8] ; ldrh w9, [x20] ; strh w9, [x8, #0x4]
constexpr Expect kInputRegisterDevice[] = {
    { 266, Operand::Offset, kInputDeviceEvents + kArrayCapacity },
    { 267, Operand::Offset, kInputDeviceEvents + kArraySize },
    { 276, Operand::Add, kInputDeviceEvents },
    { 277, Operand::Calls, 0, "__ZN2ls12DynamicArrayINS_5input17InjectDeviceEventENS_15TaggedAllocatorIiEEE10ReallocateEi" },
    { 280, Operand::Offset, kInputDeviceEvents + kArraySize },
    { 281, Operand::Offset, kInputDeviceEvents },
    { 285, Operand::Offset, kDeviceEventDevice },
};
bool CheckDeviceEventSize(Code const& f, std::string& problem)
{
    constexpr uint32_t kElement = 0x8b28cd28;       // add x8, x9, w8, sxtw #3
    static_assert(kDeviceEventSize == 1u << 3);
    if (f.At(282) != kElement) {
        problem = "a device event is not eight bytes";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// esv::Character (Layout/GameCharacter.h)
// ---------------------------------------------------------------------------

constexpr char const kServerCharacterOf[] =
    "__ZN3ecs11EntityWorld12GetComponentIKN3esv9CharacterELb1EEEPT_N2ls2IDINS_18EntityHandleTraitsEEENSt3__117integral_constantIbLb0EEE";
constexpr char const kServerCharacterOfMutable[] =
    "__ZN3ecs11EntityWorld12GetComponentIN3esv9CharacterELb1EEEPT_N2ls2IDINS_18EntityHandleTraitsEEENSt3__117integral_constantIbLb0EEE";

// The 64-bit value a `movi dN, #imm` writes: each bit of its eight-bit
// immediate a whole byte.
constexpr std::optional<uint64_t> MoviD(uint32_t instruction)
{
    if ((instruction & 0xfff8fc00) != 0x2f00e400) return {};
    uint32_t imm = ((instruction >> 16) & 7) << 5 | ((instruction >> 5) & 0x1f);
    uint64_t value = 0;
    for (int bit = 0; bit < 8; bit++) {
        if (imm & (1u << bit)) value |= 0xffull << (8 * bit);
    }
    return value;
}
static_assert(MoviD(0x2f06e580) == ((uint64_t)kUnassignedUser << 32 | kUnassignedUser));

// Character(template) makes the character: the four entity handles from the
// owner (+0x158) on all the null handle, the base visual (+0x180) none, both
// users (+0x184, +0x188) unassigned, the speed multiplier (+0x18c) 1; then it
// keeps the template at +0xc8 and +0xd0.
//     add x8, x0, #0x158 ; ... ; mov x9, #-0x40000000000000 ; ... ; dup.2d v0, x9 ; stp q0, q0, [x8] ; ...
//     mov w9, #-0x1 ; str w9, [x0, #0x180] ; movi d0, #0xffff0000ffff0000 ; stur d0, [x8, #0x2c] ; mov w8, #0x3f800000 ; str w8, [x0, #0x18c]
//     ... ; bl esv::IncreaseTemplateRefCount ; ldr x0, [x19, #0xd0] ; str x20, [x19, #0xc8]
constexpr Expect kCharacterCreate[] = {
    { 12, Operand::Add, kCharacterOwner },
    { 14, Operand::Move, 0xffc0000000000000ull },
    { 41, Operand::Offset, kCharacterBaseVisual },
    { 43, Operand::Unscaled, kCharacterOwnerUser - kCharacterOwner },
    { 45, Operand::Offset, kCharacterSpeedMultiplier },
    { 56, Operand::Calls, 0, "__ZN3esv24IncreaseTemplateRefCountEPKN2ls18GameObjectTemplateE" },
    { 57, Operand::Offset, kCharacterOriginalTemplate },
    { 58, Operand::Offset, kCharacterTemplate },
};
static_assert(kCharacterReservedUser == kCharacterOwnerUser + 4 && kCharacterEnemy == kCharacterOwner + 8 * (kCharacterHandles - 1));
bool CheckCharacterCreate(Code const& f, std::string& problem)
{
    constexpr uint32_t kNullHandles = 0x4e080d20;   // dup.2d v0, x9
    constexpr uint32_t kFourHandles = 0xad000100;   // stp q0, q0, [x8]: the four handles from the owner on
    constexpr uint32_t kUsers = 0xfc000100, kUsersMask = 0xffe00fff;   // stur d0, [x8, #...]: both users at once
    if (f.At(36) != kNullHandles || f.At(37) != kFourHandles) {
        problem = "the entity handles from the owner on are not made null together";
        return false;
    }
    if (MoviD(f.At(42)) != ((uint64_t)kUnassignedUser << 32 | kUnassignedUser) || (f.At(43) & kUsersMask) != kUsers) {
        problem = "the users are not made unassigned together";
        return false;
    }
    if (MoveValue(f.At(40)) != (uint64_t)0xffffffff || MoveValue(f.At(44)) != (uint64_t)0x3f800000) {
        problem = "the base visual is not made none, or the speed multiplier not 1";
        return false;
    }
    return true;
}

// GetOwnerUserID(followOwner) follows the owners (+0x158) through
// GetComponent<esv::Character const> while there is one and gives the last
// one's user (+0x184); GetReservedForUserID does the same with +0x188.
//     ldr x1, [x0, #0x158] ; ... ; bl GetComponent<esv::Character const, true> ; ... ; ldr x1, [x0, #0x158] ; ... ; ldr w0, [x19, #0x184]
constexpr Expect kCharacterOwnerUserRead[] = {
    { 5, Operand::Offset, kCharacterOwner },
    { 14, Operand::Calls, 0, kServerCharacterOf },
    { 18, Operand::Offset, kCharacterOwner },
    { 25, Operand::Calls, 0, kServerCharacterOf },
    { 27, Operand::Offset, kCharacterOwnerUser },
};
constexpr Expect kCharacterReservedUserRead[] = {
    { 6, Operand::Offset, kCharacterOwner },
    { 15, Operand::Calls, 0, kServerCharacterOf },
    { 19, Operand::Offset, kCharacterOwner },
    { 26, Operand::Calls, 0, kServerCharacterOf },
    { 28, Operand::Offset, kCharacterReservedUser },
};
// SetReservedForUserID(user, ...) does nothing when the user is the one at
// +0x188 already.
//     ldr w8, [x0, #0x188] ; ldr w9, [x1] ; cmp w8, w9 ; b.eq
constexpr Expect kCharacterSetReservedUser[] = {
    { 15, Operand::Offset, kCharacterReservedUser },
    { 16, Operand::Offset, 0 },
};
// SetFollowCharacter(target) keeps the target at +0x160.
//     ... ; str x19, [x20, #0x160]
constexpr Expect kCharacterSetFollow[] = {
    { 20, Operand::Offset, kCharacterFollow },
};
// SetEnemyCharacter(target) takes the character out of the old enemy's
// (+0x170) array and keeps the target at +0x170, or the null handle when the
// target is no character.
//     ldr x1, [x19, #0x170] ; bl GetComponent<esv::Character, true> ; ... ; str x20, [x19, #0x170] ; ... ; str x8, [x19, #0x170]
constexpr Expect kCharacterSetEnemy[] = {
    { 10, Operand::Offset, kCharacterEnemy },
    { 11, Operand::Calls, 0, kServerCharacterOfMutable },
    { 41, Operand::Offset, kCharacterEnemy },
    { 45, Operand::Calls, 0, kServerCharacterOfMutable },
    { 54, Operand::Offset, kCharacterEnemy },
};
// CreatePlayerData() makes the player data, when there is none at +0x150,
// and keeps it there.
//     ldr x8, [x0, #0x150] ; cbz ; ... ; bl esv::PlayerData::PlayerData() ; str x20, [x19, #0x150]
constexpr Expect kCharacterCreatePlayerData[] = {
    { 3, Operand::Offset, kCharacterPlayerData },
    { 43, Operand::Calls, 0, "__ZN3esv10PlayerDataC1Ev" },
    { 44, Operand::Offset, kCharacterPlayerData },
};

// ---------------------------------------------------------------------------
// ui::Widget and ui::State
// ---------------------------------------------------------------------------
// Widget() makes NoesisGUI's control, then its own members: the lock (a free
// one, and the spin count), the vector's pointer to its own end, the state's
// GUID, the words from the parent canvas on, the last event, the three bytes
// of flags and the player's id.
//     bl Noesis::ContentControl::ContentControl() ; ... ; add x8, x0, #0x26c ; ... ; str w8, [x0, #0x27c] ; add x8, x0, #0x2b0 ; ... ;
//     str x8, [x0, #0x2a0] ; ... ; add x8, x0, #0x2e8 ; ... ; str xzr, [x0, #0x2e0] ; ... ; add x8, x0, #0x2f8 ; ... ;
//     str w8, [x0, #0x364] ; strh wzr, [x0, #0x368] ; ldrb w8, [x0, #0x36a] ; and w8, w8, #0xf0 ; strb w8, [x0, #0x36a] ; strh wzr, [x0, #0x36c]
constexpr Expect kUiWidgetCreate[] = {
    { 11, Operand::Calls, 0, "__ZN6Noesis14ContentControlC2Ev" },
    { 19, Operand::Add, kUiWidgetLock },
    { 25, Operand::Offset, kUiWidgetLock + offsetof(GameCriticalSection, SpinCount) },
    { 26, Operand::Add, kUiWidgetVector + 0x10 },
    { 29, Operand::Offset, kUiWidgetVector },
    { 33, Operand::Add, kUiWidgetStateId },
    { 36, Operand::Offset, kUiWidgetInputManager },
    { 40, Operand::Add, kUiWidgetParentCanvas },
    { 46, Operand::Offset, kUiWidgetMoveFocusInputState },
    { 47, Operand::Offset, kUiWidgetFlags },
    { 48, Operand::ByteOffset, kUiWidgetFlags + 2 },
    { 50, Operand::ByteOffset, kUiWidgetFlags + 2 },
    { 51, Operand::Offset, kUiWidgetPlayer },
};
static_assert(kUiWidgetLock + sizeof(GameCriticalSection) == kUiWidgetInputChildren && kUiWidgetInputChildren + 2 * 0x10 == kUiWidgetVector
    && kUiWidgetVector + 0x10 == kUiWidgetContextMenuManager && kUiWidgetInputManager + 8 == kUiWidgetStateId
    && kUiWidgetStateId + 0x10 == kUiWidgetParentCanvas && kUiWidgetFileName + kGameStringBytes == kUiWidgetContextMenu
    && kUiWidgetContextMenu + 5 * 8 == kUiWidgetEvents && kUiWidgetEvents + 9 * 4 == kUiWidgetMoveFocusInputState
    && kUiWidgetMoveFocusInputState + 4 == kUiWidgetFlags);

// GetFilename() gives the file name's characters: the string itself, or what
// its first word points to when the string's last byte is negative.
//     mov x8, x0 ; add x0, x0, #0x308 ; ldrsb w8, [x8, #0x317] ; tbnz w8, #31, long ; ret ; long: ldr x0, [x0] ; ret
constexpr Expect kUiWidgetFilename[] = {
    { 1, Operand::Add, kUiWidgetFileName },
    { 2, Operand::Offset, kUiWidgetFileName + kGameStringFlag },
    { 3, Operand::TestsBit, 31 },
};

// WindowManager::InternalWidgetLoad(Uri const&, StateWidget*, State*,
// ECanvasLayout, Widget*) puts the widget in its state and fills it: under
// the state's lock it appends the widget to the state's widgets and finds
// the last index in the widget's layer; then the widget gets the window
// manager's state machine, the widget template, the input manager, the
// context menu and tooltip managers, its file name, the state's GUID and
// player, its layer, the window manager and its index in the layer.
//     mov x19, x3 ; mov x20, x2 ; ... ; mov x22, x5 ; ... ; add x26, x19, #0x2c ; mov x0, x26 ; bl ls::CriticalSection::Lock ;
//     ldp w9, w8, [x19, #0x8] ; ... ; mov x0, x19 ; bl ls::DynamicArray<Noesis::Ptr<ui::Widget>>::Reallocate ; ... ; str w9, [x19, #0xc] ; ... ;
//     ldr w11, [x10, #0x300] ; cmp w11, w24 ; ... ; ldr w10, [x10, #0x304] ; ... ; str x8, [x22, #0x2d0] ; str x20, [x22, #0x2d8] ; ... ;
//     str x9, [x22, #0x2e0] ; ... ; str x8, [x22, #0x2b0] ; ... ; str x8, [x22, #0x2c0] ; ldrh w8, [x19, #0x40] ; ... ; bl ui::Widget::SetVisible ;
//     ... ; add x0, x22, #0x308 ; ... ; add x8, x22, #0x2e8 ; ldr q0, [x19, #0xb0] ; str q0, [x8] ; ldrh w26, [x19, #0x28] ; ... ;
//     strh w26, [x22, #0x36c] ; ... ; ldrb w8, [x19, #0x43] ; ... ; str w24, [x22, #0x300] ; str x21, [x22, #0x2c8] ; ... ; str w8, [x22, #0x304]
constexpr char kWidgetsReallocate[] = "__ZN2ls12DynamicArrayIN6Noesis3PtrIN2ui6WidgetEEENS_15TaggedAllocatorIiEEE10ReallocateEi";
constexpr Expect kUiWidgetLoad[] = {
    { 25, Operand::Offset, kUiInstancePlayer },
    { 146, Operand::Add, kUiInstanceLock },
    { 148, Operand::Calls, 0, kLockSymbol },
    { 149, Operand::PairOffset, kUiInstanceWidgets + kArrayCapacity },
    { 159, Operand::Calls, 0, kWidgetsReallocate },
    { 162, Operand::Offset, kUiInstanceWidgets + kArraySize },
    { 184, Operand::Offset, kUiWidgetLayerIndex },
    { 187, Operand::Offset, kUiWidgetIndexInLayer },
    { 223, Operand::Offset, kUiWidgetStateMachine },
    { 224, Operand::Offset, kUiWidgetData },
    { 226, Operand::Offset, kUiWidgetInputManager },
    { 288, Operand::Offset, kUiWidgetContextMenuManager },
    { 290, Operand::Offset, kUiWidgetTooltipManager },
    { 291, Operand::Offset, kUiInstanceFlags },
    { 294, Operand::Calls, 0, "__ZN2ui6Widget10SetVisibleEb" },
    { 313, Operand::Add, kUiWidgetFileName },
    { 365, Operand::Add, kUiWidgetStateId },
    { 368, Operand::Offset, kUiInstancePlayer },
    { 398, Operand::Offset, kUiWidgetPlayer },
    { 402, Operand::ByteOffset, kUiInstanceOwner },
    { 430, Operand::Offset, kUiWidgetLayerIndex },
    { 431, Operand::Offset, kUiWidgetWindowManager },
    { 440, Operand::Offset, kUiWidgetIndexInLayer },
};
// A 128-bit load or store at a register and an offset that is a multiple of 16.
constexpr uint32_t LoadsQuad(unsigned base, size_t offset) { return 0x3dc00000 | (uint32_t)(offset / 16) << 10 | base << 5; }
constexpr uint32_t StoresQuad(unsigned base, size_t offset) { return 0x3d800000 | (uint32_t)(offset / 16) << 10 | base << 5; }
bool CheckUiWidgetLoad(Code const& f, std::string& problem)
{
    // x19 is the state, x20 the widget template, x22 the widget; the widget
    // template is what is stored at the widget's data, the state's GUID
    // what is copied to the widget's state id.
    if (f.At(8) != 0xaa0303f3 || f.At(9) != 0xaa0203f4 || f.At(21) != 0xaa0503f6) {
        problem = "the state, the widget template and the widget are not kept in x19, x20 and x22";
        return false;
    }
    if (RegD(f.At(224)) != 20 || RegN(f.At(224)) != 22 || RegN(f.At(365)) != 22 || RegD(f.At(365)) != 8
        || f.At(366) != LoadsQuad(19, kUiInstanceGuid) || f.At(367) != StoresQuad(8, 0)
        || RegN(f.At(368)) != 19 || RegN(f.At(398)) != 22 || RegD(f.At(398)) != RegD(f.At(368))) {
        problem = "the widget does not get the widget template, the state's GUID and the state's player";
        return false;
    }
    return true;
}

// SetVisible(bool) reads the third flag byte, keeps the element whose focus
// to refresh, sets the second byte and a bit of the first; hiding, it has
// the context menu manager close the player's menu.
//     ldrb w8, [x19, #0x36a] ; ... ; str x20, [x19, #0x320] ; mov w8, #1 ; strb w8, [x19, #0x369] ; ldrb w8, [x19, #0x368] ;
//     orr w8, w8, #0x2 ; strb w8, [x19, #0x368] ; ... ; ldr x8, [x19, #0x318] ; cbz ; ldr x0, [x19, #0x2b0] ; ldrh w8, [x19, #0x36c]
constexpr Expect kUiWidgetSetVisible[] = {
    { 24, Operand::ByteOffset, kUiWidgetFlags + 2 },
    { 41, Operand::Offset, kUiWidgetFocusRefresh },
    { 43, Operand::ByteOffset, kUiWidgetFlags + 1 },
    { 44, Operand::ByteOffset, kUiWidgetFlags },
    { 46, Operand::ByteOffset, kUiWidgetFlags },
    { 48, Operand::Offset, kUiWidgetContextMenu },
    { 50, Operand::Offset, kUiWidgetContextMenuManager },
    { 51, Operand::Offset, kUiWidgetPlayer },
};

// SetHasInputEnabled(bool) sets or clears one bit of the third flag byte.
//     ldrb w8, [x0, #0x36a] ; mov w9, #0x8 ; cmp w1, #0 ; csel w9, w9, wzr, ne ; and w8, w8, #0xfffffff7 ; orr w8, w8, w9 ; strb w8, [x0, #0x36a]
constexpr Expect kUiWidgetSetInput[] = {
    { 4, Operand::ByteOffset, kUiWidgetFlags + 2 },
    { 5, Operand::Move, kUiWidgetHasInput },
    { 8, Operand::Mask, (uint32_t)~kUiWidgetHasInput },
    { 10, Operand::ByteOffset, kUiWidgetFlags + 2 },
};

// State() makes a free lock, the flags, the layout and owner bytes, a null
// fixed string, three empty strings, and a collection for the widget
// templates.
//     str w8, [x0, #0x3c] ; ldrh w8, [x0, #0x40] ; ... ; strh w8, [x0, #0x40] ; mov w8, #0x100 ; strh w8, [x0, #0x42] ; strb wzr, [x0, #0x44] ;
//     mov w8, #-1 ; str w8, [x0, #0x48] ; ... ; strb wzr, [x0, #0x58] ; ... ; strb wzr, [x0, #0x78] ; ... ; strb wzr, [x0, #0x98] ; ... ;
//     stp xzr, xzr, [x0, #0xd0] ; str xzr, [x0, #0xe0] ; ... ; str x20, [x19, #0xe0]
constexpr Expect kUiInstanceCreate[] = {
    { 14, Operand::Offset, kUiInstanceLock + offsetof(GameCriticalSection, SpinCount) },
    { 15, Operand::Offset, kUiInstanceFlags },
    { 19, Operand::Offset, kUiInstanceFlags },
    { 21, Operand::Offset, kUiInstanceLayout },
    { 22, Operand::ByteOffset, kUiInstanceByte },
    { 23, Operand::Move, 0xffffffffull },
    { 24, Operand::Offset, kUiInstanceName },
    { 28, Operand::ByteOffset, kUiInstanceStrings + 8 },
    { 30, Operand::ByteOffset, kUiInstanceStrings + kUiTemplateString + 8 },
    { 32, Operand::ByteOffset, kUiInstanceStrings + 2 * kUiTemplateString + 8 },
    { 36, Operand::PairOffset, kUiInstanceExtra },
    { 37, Operand::Offset, kUiInstanceWidgetTemplates },
    { 63, Operand::Offset, kUiInstanceWidgetTemplates },
};
static_assert(kUiInstanceLock + sizeof(GameCriticalSection) == kUiInstanceFlags && kUiInstanceStrings + 3 * kUiTemplateString == kUiInstanceGuid
    && kUiInstanceStackGuid + 0x10 == kUiInstanceExtra && kUiInstanceWidgetTemplates + 8 == kUiInstanceSize);

// ~State() releases the widget templates, frees the three strings, releases
// the fixed string, destroys the lock's semaphore, releases the state
// template, frees the stacks and destroys the widgets.
//     ldr x0, [x0, #0xe0] ; ... ; ldrb w8, [x19, #0x97] ; cbnz ; ldr x0, [x19, #0x98] ; bl Noesis::Dealloc ; (0x77, 0x78; 0x57, 0x58) ;
//     ldr w8, [x19, #0x48] ; ... ; bl ls::gst::Map::Release ; ... ; ldr w1, [x19, #0x2c] ; ... ; bl semaphore_destroy ; ... ;
//     ldr x0, [x19, #0x20] ; ... ; ldr w8, [x19, #0x18] ; ... ; ldr w8, [x19, #0x1c] ; ... ; ldr x20, [x19, #0x10] ; ... ;
//     mov x0, x19 ; ... ; b ls::DynamicArray<Noesis::Ptr<ui::Widget>>::~DynamicArray
constexpr Expect kUiInstanceDestroy[] = {
    { 6, Operand::Offset, kUiInstanceWidgetTemplates },
    { 11, Operand::ByteOffset, kUiInstanceStrings + 2 * kUiTemplateString + 7 },
    { 13, Operand::Offset, kUiInstanceStrings + 2 * kUiTemplateString + 8 },
    { 15, Operand::ByteOffset, kUiInstanceStrings + kUiTemplateString + 7 },
    { 17, Operand::Offset, kUiInstanceStrings + kUiTemplateString + 8 },
    { 19, Operand::ByteOffset, kUiInstanceStrings + 7 },
    { 21, Operand::Offset, kUiInstanceStrings + 8 },
    { 23, Operand::Offset, kUiInstanceName },
    { 32, Operand::Calls, 0, "__ZN2ls3gst3Map7ReleaseERKNS0_12NodePoolDataE" },
    { 36, Operand::Offset, kUiInstanceLock + offsetof(GameCriticalSection, Semaphore) },
    { 40, Operand::CallsImport, 0, "_semaphore_destroy" },
    { 42, Operand::Offset, kUiInstanceTemplate },
    { 52, Operand::Offset, kUiInstanceStacks + kArrayCapacity },
    { 55, Operand::Offset, kUiInstanceStacks + kArraySize },
    { 59, Operand::Offset, kUiInstanceStacks },
    { 138, Operand::JumpsTo, 0, "__ZN2ls12DynamicArrayIN6Noesis3PtrIN2ui6WidgetEEENS_15TaggedAllocatorIiEEED2Ev" },
};

// StateMachineComponent::AddChild(State* parent, State* child) makes a stack
// with a new GUID and the parent's, puts the child in it, enters the stack
// in the component's map by its GUID, writes that GUID to the child and
// appends the stack to the parent's stacks.
//     mov w0, #0x30 ; bl malloc ; ... ; add x24, x21, #0x10 ; ... ; stp x23, x0, [x21, #0x10] ; ldr q0, [x20, #0xb0] ; str q0, [x21, #0x20] ;
//     ldp w9, w8, [x21, #0x8] ; ... ; bl ls::DynamicArray<ui::State*>::Reallocate ; ... ; add x0, x19, #0xb0 ; ... ;
//     bl ls::HashMap<ls::Guid, ui::StateStack*>::SetUniversal ; ldr q0, [x24] ; str q0, [x22, #0xc0] ; ldp w9, w8, [x20, #0x18] ; ... ;
//     add x0, x20, #0x10 ; bl ls::DynamicArray<ui::StateStack*>::Reallocate ; ... ; str w9, [x20, #0x1c] ; ldr x9, [x20, #0x10]
constexpr size_t kUiStatesStackMap = 0xb0;              // in the component: the stacks by their GUID
constexpr Expect kUiInstanceAddChild[] = {
    { 30, Operand::Move, kUiStackSize },
    { 31, Operand::CallsImport, 0, "_malloc" },
    { 43, Operand::Add, kUiStackGuid },
    { 58, Operand::PairOffset, kUiStackGuid },
    { 61, Operand::PairOffset, kUiStackStates + kArrayCapacity },
    { 71, Operand::Calls, 0, "__ZN2ls12DynamicArrayIPN2ui5StateENS_15TaggedAllocatorIiEEE10ReallocateEi" },
    { 80, Operand::Add, kUiStatesStackMap },
    { 83, Operand::Calls, 0, "__ZN2ls7HashMapINS_4GuidEPN2ui10StateStackENS_19HashTableOpsDefaultEE12SetUniversalIRKS1_RKS4_EENS_7EMapSetEOT_OT0_" },
    { 86, Operand::PairOffset, kUiInstanceStacks + kArrayCapacity },
    { 95, Operand::Add, kUiInstanceStacks },
    { 96, Operand::Calls, 0, "__ZN2ls12DynamicArrayIPN2ui10StateStackENS_15TaggedAllocatorIiEEE10ReallocateEi" },
    { 99, Operand::Offset, kUiInstanceStacks + kArraySize },
    { 100, Operand::Offset, kUiInstanceStacks },
};
bool CheckUiInstanceAddChild(Code const& f, std::string& problem)
{
    // x20 is the parent, x22 the child, x21 the stack, x24 the stack's GUID.
    if (f.At(59) != LoadsQuad(20, kUiInstanceGuid) || f.At(60) != StoresQuad(21, kUiStackParent) || f.At(84) != LoadsQuad(24, 0)
        || f.At(85) != StoresQuad(22, kUiInstanceStackGuid) || RegN(f.At(43)) != 21 || RegD(f.At(43)) != 24) {
        problem = "the stack does not get the parent's GUID, or the child the stack's";
        return false;
    }
    return true;
}

// StateMachineComponent::CreateState(short const& player, name, ...) makes a
// state: allocates it, gives it a new GUID, the state template it found by
// name, the player, a fixed string and a name, the owner, the layout its
// template's string names, the flags, and its template's tutorial layer, and
// enters it in the component's map by its GUID.
//     mov w0, #0xe8 ; bl malloc ; ... ; bl ui::State::State() ; ... ; stp x19, x0, [x21, #0xb0] ; ... ; str x22, [x21, #0x20] ; ldrh w8, [x20] ;
//     strh w8, [x21, #0x28] ; ... ; str w19, [x21, #0x48] ; add x26, x21, #0x50 ; ... ; strb w25, [x21, #0x43] ; ... ; ldr w8, [x0, #0x30]! ; cbz ;
//     bl StateLayoutFromString ; strb w0, [x21, #0x42] ; ... ; add x8, x22, #0x150 ; add x24, x21, #0x90 ; ... ; add x1, x21, #0xb0 ; ... ;
//     bl ls::HashMap<ls::Guid, ui::State*>::SetUniversal
constexpr Expect kUiInstanceCreateState[] = {
    { 18, Operand::Calls, 0, "__ZN2ui21StateMachineComponent17FindStateTemplateERKN6Noesis11FixedStringILj24EEE" },
    { 217, Operand::Move, kUiInstanceSize },
    { 218, Operand::CallsImport, 0, "_malloc" },
    { 230, Operand::Calls, 0, "__ZN2ui5StateC1Ev" },
    { 242, Operand::PairOffset, kUiInstanceGuid },
    { 259, Operand::Offset, kUiInstanceTemplate },
    { 261, Operand::Offset, kUiInstancePlayer },
    { 280, Operand::Offset, kUiInstanceName },
    { 281, Operand::Add, kUiInstanceStrings },
    { 324, Operand::ByteOffset, kUiInstanceOwner },
    { 344, Operand::Unscaled, kUiTemplateFirstString + kUiTemplateString },
    { 346, Operand::Calls, 0, "__ZN2ui21StateMachineComponent21StateLayoutFromStringERKN6Noesis11FixedStringILj24EEE" },
    { 347, Operand::ByteOffset, kUiInstanceLayout },
    { 534, Operand::Add, kUiTemplateDisableSoundLayersAbove + kUiTemplateString },
    { 535, Operand::Add, kUiInstanceStrings + 2 * kUiTemplateString },
    { 584, Operand::Add, kUiInstanceGuid },
    { 586, Operand::Calls, 0, "__ZN2ls7HashMapINS_4GuidEPN2ui5StateENS_19HashTableOpsDefaultEE12SetUniversalIRKS1_RKS4_EENS_7EMapSetEOT_OT0_" },
};
bool CheckUiInstanceCreateState(Code const& f, std::string& problem)
{
    // x22 is the state template found, x21 the new state: the template is
    // what the state keeps, the layout what StateLayoutFromString returns.
    if (f.At(20) != 0xaa0003f6 || f.At(219) != 0xaa0003f5 || RegD(f.At(259)) != 22 || RegN(f.At(259)) != 21 || RegD(f.At(347)) != 0
        || RegN(f.At(347)) != 21 || RegN(f.At(534)) != 22 || RegN(f.At(535)) != 21) {
        problem = "the state does not get the state template it found, or the layout its string names";
        return false;
    }
    return true;
}

// StateMachineComponent::OnWidgetCreated(Guid const&, State*, Widget*) takes
// the widget's template out of the state's collection of templates to load.
//     ldr x0, [x23, #0xe0] ; ldr x1, [x21, #0x2d8] ; bl Noesis::BaseCollection::Remove
constexpr Expect kUiInstanceWidgetCreated[] = {
    { 16, Operand::Offset, kUiInstanceWidgetTemplates },
    { 17, Operand::Offset, kUiWidgetData },
    { 18, Operand::Calls, 0, "__ZN6Noesis14BaseCollection6RemoveEPKNS_13BaseComponentE" },
};

// ---------------------------------------------------------------------------
// esv::Item
// ---------------------------------------------------------------------------
// Item(GameObjectTemplate const*) makes the two vtable pointers, a null
// entity, a null level, the item's type, then the cold data from +0x38: a
// null handle after a word, three null fixed strings, eight zero bytes, the
// flag byte; and keeps the template as both of its templates.
//     stp x8, x9, [x0] ; ... ; stp x23, xzr, [x0, #0x20] ; mov w24, #-1 ; str w24, [x0, #0x30] ; ... ; mov x21, x19 ; str q0, [x21, #0x38]! ;
//     stur w22, [x21, #-0x4] ; ... ; stp xzr, x23, [x21, #0x50] ; ... ; str d0, [x21, #0x60] ; str w24, [x21, #0x68] ; stur xzr, [x21, #0x6c] ;
//     ldrb w8, [x21, #0x74] ; and w8, w8, #0xc0 ; strb w8, [x21, #0x74] ; ... ; ldr x0, [x19, #0x50] ; str x20, [x19, #0x48] ; ... ; str x20, [x19, #0x50]
constexpr char kServerTemplateAdd[] = "__ZN3esv24IncreaseTemplateRefCountEPKN2ls18GameObjectTemplateE";
constexpr char kServerTemplateRelease[] = "__ZN3esv24DecreaseTemplateRefCountEPKN2ls18GameObjectTemplateE";
constexpr Expect kItemCreate[] = {
    { 16, Operand::PairOffset, kItemEntity },
    { 18, Operand::Offset, kItemLevel },
    { 28, Operand::Unscaled, (uint64_t)(int64_t)((int64_t)kItemType - (int64_t)kItemColdData) },
    { 31, Operand::PairOffset, kItemWord - kItemColdData },
    { 34, Operand::Offset, kItemPreviousLevel - kItemColdData },
    { 35, Operand::Unscaled, kItemTreasureLevel - kItemColdData },
    { 36, Operand::ByteOffset, kItemFlags2 - kItemColdData },
    { 38, Operand::ByteOffset, kItemFlags2 - kItemColdData },
    { 41, Operand::Calls, 0, kServerTemplateAdd },
    { 42, Operand::Offset, kItemOriginalTemplate },
    { 43, Operand::Offset, kItemTemplate },
    { 51, Operand::Offset, kItemOriginalTemplate },
};
bool CheckItemCreate(Code const& f, std::string& problem)
{
    // The cold data's address is made with the store of its first sixteen
    // bytes (str q0, [x21, #0x38]!), and the two fixed strings before the
    // previous level are one store of eight bytes (str d0, [x21, #0x60]).
    if (f.At(27) != (0x3c800c00u | (uint32_t)kItemColdData << 12 | 21u << 5) || f.At(33) != (0xfd000000u | (uint32_t)((kItemNames - kItemColdData) / 8) << 10 | 21u << 5)) {
        problem = "the cold data is not at the offset the Mac type has, or the names are not";
        return false;
    }
    return true;
}
static_assert(kItemType + 4 == kItemColdData && kItemPeers == kItemColdData && kItemNames + 4 == kItemStats && kItemStats + 4 == kItemPreviousLevel
    && kItemPreviousLevel + 4 == kItemTreasureLevel && kItemTreasureLevel + 8 == kItemFlags2);

// The accessors: the object handle, the entity, the flags, the level, the
// template, whether the item is global, whether a peer is in its peers.
//     GetEntityObjectHandle: ldr x0, [x0, #0x10] ; ret        GetEntityRef: add x0, x0, #0x20 ; ret
//     IsFlag: ldr x8, [x0, #0x18] ; tst x8, x1 ; ...          GetCurrentLevel: add x0, x0, #0x30 ; ret
//     GetTemplate: ldr x0, [x0, #0x48] ; ret                  IsGlobal: ldrb w8, [x0, #0xac] ; ubfx w0, w8, #1, #1 ; ret
//     HasPeer: ldr x8, [x0, #0x38] ; ldr w10, [x0, #0x44] ; ...
constexpr Expect kItemObjectHandleRead[] = { { 0, Operand::Offset, kItemObjectHandle } };
constexpr Expect kItemEntityRead[] = { { 0, Operand::Add, kItemEntity } };
constexpr Expect kItemFlagRead[] = { { 0, Operand::Offset, kItemFlags } };
constexpr Expect kItemLevelRead[] = { { 0, Operand::Add, kItemLevel } };
constexpr Expect kItemTemplateRead[] = { { 0, Operand::Offset, kItemTemplate } };
constexpr Expect kItemGlobalRead[] = { { 0, Operand::ByteOffset, kItemFlags2 }, { 1, Operand::ShiftRight, 1 } };
constexpr Expect kItemPeerRead[] = { { 0, Operand::Offset, kItemPeers }, { 1, Operand::Offset, kItemPeers + kArraySize } };
static_assert(kItemGlobal == 1 << 1);

// SetTemplate and SetOriginalTemplate release the template held and keep the new one.
//     ldr x0, [x0, #0x48] ; ... ; bl esv::DecreaseTemplateRefCount ; str xzr, [x20, #0x48] ; ... ; bl esv::IncreaseTemplateRefCount ; str x19, [x20, #0x48]
constexpr Expect kItemSetTemplate[] = {
    { 4, Operand::Offset, kItemTemplate }, { 9, Operand::Calls, 0, kServerTemplateRelease }, { 10, Operand::Offset, kItemTemplate },
    { 13, Operand::Calls, 0, kServerTemplateAdd }, { 14, Operand::Offset, kItemTemplate },
};
constexpr Expect kItemSetOriginalTemplate[] = {
    { 4, Operand::Offset, kItemOriginalTemplate }, { 9, Operand::Calls, 0, kServerTemplateRelease }, { 10, Operand::Offset, kItemOriginalTemplate },
    { 13, Operand::Calls, 0, kServerTemplateAdd }, { 14, Operand::Offset, kItemOriginalTemplate },
};

// SetStatsId(FixedString const&) releases the stats id held, keeps the new
// one, and keeps the stats object the stats give for it.
//     ldr w8, [x19, #0x9c] ; ... ; bl ls::gst::Map::Release ; str w21, [x19, #0x9c] ; ... ; blr x8 ; str x0, [x19, #0x78] ; ldr w8, [x19, #0x34]
constexpr Expect kItemSetStats[] = {
    { 16, Operand::Offset, kItemStats },
    { 25, Operand::Calls, 0, "__ZN2ls3gst3Map7ReleaseERKNS0_12NodePoolDataE" },
    { 26, Operand::Offset, kItemStats },
    { 39, Operand::Offset, kItemStatsObject },
    { 40, Operand::Offset, kItemType },
};

// ---------------------------------------------------------------------------
// ecl::Character
// ---------------------------------------------------------------------------
// Character(GameObjectTemplate const*) makes the two vtable pointers, a null
// level and entity, an empty array, two network ids of -1, null handles for
// the owner and the character using it and for the four effects, the two
// users unassigned, the speed multiplier 1, the physics flags 0xffff, and
// keeps the template as both of its templates.
//     add x8, x0, #0x108 ; ... ; str w9, [x0, #0x20] ; ... ; str q0, [x21, #0x58]! ; mov x9, #-0x40000000000000 ; stp x9, xzr, [x0, #0x28] ;
//     stp xzr, xzr, [x0, #0x38] ; mov x10, #-1 ; stp x10, x10, [x0, #0x48] ; ... ; dup.2d v0, x9 ; stur q0, [x0, #0xe8] ; stp xzr, xzr, [x0, #0xf8] ;
//     stp q0, q0, [x8] ; movi d0, #0xffff0000ffff0000 ; str d0, [x0, #0x128] ; stp xzr, xzr, [x0, #0x130] ; mov w9, #0x3f800000 ;
//     stp xzr, x9, [x0, #0x140] ; mov w9, #0xffff ; strh w9, [x0, #0x150] ; strb wzr, [x0, #0x152] ; ldurh w9, [x8, #0x4b] ; ... ;
//     sturh w9, [x8, #0x4b] ; cbz ; str x20, [x19, #0x60]
constexpr char kClientTemplateAdd[] = "__ZN3ecl24IncreaseTemplateRefCountEPKN2ls18GameObjectTemplateE";
constexpr char kClientTemplateRelease[] = "__ZN3ecl24DecreaseTemplateRefCountEPKN2ls18GameObjectTemplateE";
constexpr Expect kClientCharacterCreate[] = {
    { 13, Operand::Add, kClientCharacterEffects },
    { 16, Operand::Offset, kClientCharacterLevel },
    { 21, Operand::PairOffset, kClientCharacterEntity },
    { 22, Operand::PairOffset, kClientCharacterPathInfluences },
    { 24, Operand::PairOffset, kClientCharacterRequests },
    { 35, Operand::PairOffset, kClientCharacterCircles },
    { 39, Operand::PairOffset, kClientCharacterCircleColors },
    { 40, Operand::Move, 0x3f800000 },
    { 41, Operand::PairOffset, kClientCharacterLastTick },
    { 42, Operand::Move, 0xffff },
    { 43, Operand::Offset, kClientCharacterPhysicsFlags },
    { 44, Operand::ByteOffset, kClientCharacterFeetStatus },
    { 45, Operand::Unscaled, kClientCharacterFlags2 - kClientCharacterEffects },
    { 48, Operand::Unscaled, kClientCharacterFlags2 - kClientCharacterEffects },
    { 50, Operand::Offset, kClientCharacterOriginalTemplate },
    { 52, Operand::Calls, 0, kClientTemplateAdd },
};
bool CheckClientCharacterCreate(Code const& f, std::string& problem)
{
    // The template's address is made with the store of the two templates
    // (str q0, [x21, #0x58]!); the owner and the character using it are
    // one store of two null handles (stur q0, [x0, #0xe8]); the two users
    // one store of eight bytes (str d0, [x0, #0x128]) of the value the
    // movi before it makes; the speed multiplier is the second word of the
    // pair stored at the last tick.
    if (f.At(19) != (0x3c800c00u | (uint32_t)kClientCharacterTemplate << 12 | 21u << 5) || f.At(34) != (0x3c800000u | (uint32_t)kClientCharacterOwner << 12)
        || f.At(37) != 0x2f06e580 || f.At(38) != (0xfd000000u | (uint32_t)(kClientCharacterOwnerUser / 8) << 10)
        || (f.At(41) & 0x7c1f) != (9u << 10 | 31u)) {
        problem = "the templates, the owner, the users or the speed multiplier are not where the Mac type has them";
        return false;
    }
    return true;
}
static_assert(kClientCharacterLastTick + 8 == kClientCharacterSpeedMultiplier && kClientCharacterEffects + 4 * 8 == kClientCharacterOwnerUser
    && kClientCharacterOwner + 2 * 8 == kClientCharacterCircles && kClientCharacterRequests + 2 * 8 == kClientCharacterTemplate
    && kClientCharacterFeetStatus + 1 == kClientCharacterFlags2);

// The accessors, and CreatePlayerData and GetOwnerCharacter.
//     GetEntityObjectHandle: ldr x0, [x0, #0x10] ; ret        GetEntityRef: add x0, x0, #0x28 ; ret
//     IsFlag: ldr x8, [x0, #0x18] ; tst x8, x1 ; ...          GetCurrentLevel: add x0, x0, #0x20 ; ret
//     GetTemplate: ldr x0, [x0, #0x58] ; ret
//     SetTemplate: ldr x0, [x0, #0x58] ; ... ; bl ecl::DecreaseTemplateRefCount ; str x19, [x20, #0x58] ; ... ; b ecl::IncreaseTemplateRefCount
//     GetOwnerCharacter: ldr x20, [x19, #0x28] ; ... ; ldr x1, [x19, #0xe8] ; ... ; ldr x23, [x19, #0x30]
//     CreatePlayerData: ldr x8, [x0, #0xc8] ; ... ; bl ecl::PlayerData::PlayerData() ; str x20, [x19, #0xc8]
constexpr Expect kClientCharacterObjectHandleRead[] = { { 0, Operand::Offset, kClientCharacterObjectHandle } };
constexpr Expect kClientCharacterEntityRead[] = { { 0, Operand::Add, kClientCharacterEntity } };
constexpr Expect kClientCharacterFlagRead[] = { { 0, Operand::Offset, kClientCharacterFlags } };
constexpr Expect kClientCharacterLevelRead[] = { { 0, Operand::Add, kClientCharacterLevel } };
constexpr Expect kClientCharacterTemplateRead[] = { { 0, Operand::Offset, kClientCharacterTemplate } };
constexpr Expect kClientCharacterSetTemplate[] = {
    { 4, Operand::Offset, kClientCharacterTemplate }, { 8, Operand::Calls, 0, kClientTemplateRelease }, { 9, Operand::Offset, kClientCharacterTemplate },
    { 13, Operand::JumpsTo, 0, kClientTemplateAdd },
};
constexpr Expect kClientCharacterSetOriginalTemplate[] = {
    { 4, Operand::Offset, kClientCharacterOriginalTemplate }, { 8, Operand::Calls, 0, kClientTemplateRelease },
    { 9, Operand::Offset, kClientCharacterOriginalTemplate }, { 13, Operand::JumpsTo, 0, kClientTemplateAdd },
};
constexpr Expect kClientCharacterOwnerRead[] = {
    { 7, Operand::Offset, kClientCharacterEntity },
    { 16, Operand::Offset, kClientCharacterOwner },
    { 22, Operand::Offset, kClientCharacterEntity + 8 },
};
// ecl::GameInput::OnControllerModeChanged() gives each player's character's
// input controller (+0xa0) the game's controller mode.
//     bl ecl::GameInput::GetInputCharacter ; cbz x0 ; ldr x0, [x0, #0xa0] ; cbz x0 ; ldrb w1, [x20, ...]
//     bl ecl::InputController::SetControllerMode
constexpr Expect kClientCharacterControllerMode[] = {
    { 27, Operand::Calls, 0, "__ZN3ecl9GameInput17GetInputCharacterEs" },
    { 29, Operand::Offset, kClientCharacterInputController },
    { 32, Operand::Calls, 0, "__ZN3ecl15InputController17SetControllerModeEb" },
};
// ecl::InputController::SetControllerMode(bool) keeps the mode in the lowest
// bit of the flags (+0x48), and does nothing when it is the mode already.
//     ldrb w8, [x0, #0x48] ; tst w8, #0x1 ; cset w9, eq ; eor w9, w9, w1 ; tbnz w9, #0 ; mov x19, x0
//     and w8, w8, #0xfe ; orr w8, w8, w1 ; strb w8, [x0, #0x48]
constexpr Expect kClientInputSetControllerMode[] = {
    { 11, Operand::ByteOffset, kClientInputFlags },
    { 12, Operand::Mask, kClientInputControllerMode },
    { 17, Operand::Mask, (uint8_t)~kClientInputControllerMode },
    { 19, Operand::ByteOffset, kClientInputFlags },
};
constexpr Expect kClientCharacterCreatePlayerData[] = {
    { 3, Operand::Offset, kClientCharacterPlayerData },
    { 40, Operand::Calls, 0, "__ZN3ecl10PlayerDataC1Ev" },
    { 41, Operand::Offset, kClientCharacterPlayerData },
};

// ---------------------------------------------------------------------------
// esv::Status
// ---------------------------------------------------------------------------
// Status() makes every member: the word of -1, the story action, a new GUID
// for the originator and its three null strings and set flag, the double,
// a null id, the times, the cause bytes, null handles, the owner, the
// conditions of removal, the flags, the source spell with its source type,
// the stack id, the difficulty status and the last flags.
//     add x21, x0, #0x100 ; ... ; stp xzr, x8, [x0, #0x10] ; str wzr, [x0, #0x20] ; ... ; stp x20, x0, [x19, #0x28] ; ... ; str d0, [x19, #0x38] ;
//     mov w8, #-1 ; str w8, [x19, #0x40] ; mov w9, #1 ; strb w9, [x19, #0x44] ; str xzr, [x19, #0x48] ; str w8, [x19, #0x50] ; ... ;
//     stur d0, [x19, #0x54] ; mov w9, #0xbf800000 ; str w9, [x19, #0x5c] ; str xzr, [x19, #0x60] ; strh wzr, [x19, #0x68] ; ... ;
//     stp xzr, xzr, [x19, #0x98] ; str x9, [x19, #0x90] ; ... ; stp xzr, xzr, [x19, #0xb8] ; ... ; stp xzr, x9, [x19, #0xd8] ;
//     stp xzr, xzr, [x19, #0xe8] ; ... ; str d0, [x19, #0xf8] ; ldrb w9, [x19, #0x102] ; ... ; strh w10, [x19, #0x100] ; ... ;
//     strb w9, [x19, #0x102] ; add x9, x19, #0x103 ; ... ; add x9, x19, #0x108 ; str w8, [x19, #0x108] ; mov w10, #0x17 ; strb w10, [x19, #0x110] ;
//     ... ; str w8, [x19, #0x138] ; ... ; strb wzr, [x19, #0x158] ; ... ; str w20, [x19, #0x15c] ; ... ; strb wzr, [x19, #0x180] ;
//     ldurh w8, [x21, #0x81] ; ... ; sturh w8, [x21, #0x81]
constexpr Expect kStatusCreate[] = {
    { 6, Operand::Add, kStatusFlags },
    { 11, Operand::PairOffset, kStatusWord - 8 },
    { 12, Operand::Offset, kStatusStoryAction },
    { 23, Operand::PairOffset, kStatusOriginator },
    { 27, Operand::Offset, kStatusOriginatorStrings + 8 },
    { 28, Operand::Move, 1 },
    { 29, Operand::ByteOffset, kStatusOriginatorStrings + 12 },
    { 30, Operand::Offset, kStatusDouble },
    { 31, Operand::Offset, kStatusId },
    { 35, Operand::Move, 0xbf800000 },
    { 36, Operand::Offset, kStatusLifeTime + 4 },
    { 37, Operand::Offset, kStatusTurnTimer },
    { 38, Operand::Offset, kStatusCauseType },
    { 42, Operand::PairOffset, kStatusOwners },
    { 43, Operand::Offset, kStatusOwner },
    { 48, Operand::PairOffset, kStatusCauseGuid },
    { 50, Operand::PairOffset, kStatusRootCause + 8 },
    { 51, Operand::PairOffset, kStatusGuid },
    { 54, Operand::ByteOffset, kStatusFlags + 2 },
    { 58, Operand::Offset, kStatusFlags },
    { 60, Operand::ByteOffset, kStatusFlags + 2 },
    { 61, Operand::Add, kStatusFreezeDuration },
    { 64, Operand::Add, kStatusSourceSpell },
    { 65, Operand::Offset, kStatusSourceSpell },
    { 66, Operand::Move, 0x17 },
    { 67, Operand::ByteOffset, kStatusSpellSourceType },
    { 70, Operand::Offset, kStatusSpellPrototype },
    { 75, Operand::ByteOffset, kStatusRecoverable },
    { 83, Operand::Offset, kStatusDifficultyStatus },
    { 85, Operand::ByteOffset, kStatusTickType },
    { 86, Operand::Unscaled, kStatusFlags4 - kStatusFlags },
    { 89, Operand::Unscaled, kStatusFlags4 - kStatusFlags },
};
bool CheckStatusCreate(Code const& f, std::string& problem)
{
    // The stores of 64 and 128 bits from vector registers: the first two
    // strings of the originator, the two floats before the current life
    // time, the removal conditions and events, the GUID of the spell cast
    // and the stack id with its priority.
    auto stores64 = [](size_t offset) { return 0xfd000000u | (uint32_t)(offset / 8) << 10 | 19u << 5; };
    if (f.At(25) != stores64(kStatusOriginatorStrings) || f.At(34) != (0xfc000000u | (uint32_t)(kStatusId + 4) << 12 | 19u << 5)
        || f.At(53) != stores64(kStatusRemoveConditions) || f.At(73) != (StoresQuad(19, kStatusSpellCastSource) | 1u)
        || f.At(74) != stores64(kStatusStackId)) {
        problem = "the originator's strings, the times, the removal conditions, the spell cast's GUID or the stack id are not where the Mac type has them";
        return false;
    }
    return true;
}
static_assert(kStatusOriginatorStrings == kStatusOriginator + 0x10 && kStatusId + 8 == kStatusLifeTime && kStatusSpellCastSource + 0x10 == kStatusStackId
    && kStatusStackedSpellCastSource + 0x20 == kStatusTickType);

// GetHandle and SetHandle, GetTarget (the owner), and SetLifeTime, which
// stores the life time and the current one and enters the entity the status
// is synchronised through in a set.
//     GetHandle: ldr x0, [x0, #0x80] ; ret      SetHandle: str x1, [x0, #0x80] ; ret      GetTarget: add x0, x0, #0x90 ; ret
//     SetLifeTime: add x19, x0, #0x100 ; ldrb w8, [x0, #0x102] ; ldrh w9, [x0, #0x100] ; ... ; strh w10, [x0, #0x100] ; ... ;
//     stp s0, s1, [x0, #0x58] ; ... ; add x1, x0, #0x88
constexpr Expect kStatusHandleRead[] = { { 0, Operand::Offset, kStatusHandle } };
constexpr Expect kStatusOwnerRead[] = { { 0, Operand::Add, kStatusOwner } };
constexpr Expect kStatusSetLifeTime[] = {
    { 3, Operand::Add, kStatusFlags },
    { 4, Operand::ByteOffset, kStatusFlags + 2 },
    { 5, Operand::Offset, kStatusFlags },
    { 8, Operand::Offset, kStatusFlags },
    { 24, Operand::Add, kStatusSyncEntity },
};
bool CheckStatusSetLifeTime(Code const& f, std::string& problem)
{
    // stp s0, s1, [x0, #0x58]: the life time it is given, and the current one.
    if (f.At(12) != (0x2d000000u | (uint32_t)(kStatusLifeTime / 4) << 15 | 1u << 10 | 0u << 5 | 0u)) {
        problem = "the life time and the current life time are not stored where the Mac type has them";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// rf::CameraController and rf::Camera (Layout/GameCamera.h)
// ---------------------------------------------------------------------------

// ldp and stp of two q or two s registers at a signed offset, which the
// operand list does not read: the instructions the proofs below compare
// whole.
constexpr uint32_t StoresQuads(unsigned rt, unsigned rt2, unsigned base, size_t offset)
{
    return 0xad000000 | (uint32_t)(offset / 16) << 15 | rt2 << 10 | base << 5 | rt;
}
constexpr uint32_t LoadsQuads(unsigned rt, unsigned rt2, unsigned base, size_t offset)
{
    return 0xad400000 | (uint32_t)(offset / 16) << 15 | rt2 << 10 | base << 5 | rt;
}
constexpr uint32_t LoadsFloats(unsigned rt, unsigned rt2, unsigned base, size_t offset)
{
    return 0x2d400000 | (uint32_t)(offset / 4) << 15 | rt2 << 10 | base << 5 | rt;
}
struct CameraWord
{
    uint16_t Index;
    uint32_t Word;
};
bool CameraWordsHold(Code const& f, std::span<CameraWord const> words, char const* what, std::string& problem)
{
    for (auto const& w : words) {
        if (f.At(w.Index) != w.Word) {
            problem = std::string("instruction ") + std::to_string(w.Index) + " does not " + what + " at the Mac type's offset";
            return false;
        }
    }
    return true;
}

// CameraController() sets the view and projection dirty and the other two
// flags clear in one word; stores the field of view, the planes and the
// aspect ratio from one constant; the rotation, translation, scale, the
// point looked at and the vector after it in four 16-byte stores; clears the
// orthographic bounds; takes the null string for the ID; clears the camera
// from its product matrices through its planes; makes the camera's four
// matrices the identity, the first float of its view matrix first; clears
// the frustum points, which end the object; and computes them.
//     mov w8, #0x101 ; str w8, [x0, #0x8] ; ... ; stur q0, [x0, #0xc] ; ... ; stur q0, [x0, #0x1c] ; stur q0, [x0, #0x2c] ; ... ;
//     stur q0, [x0, #0x3c] ; ... ; stur q0, [x0, #0x4c] ; stp xzr, xzr, [x0, #0x60] ; add x20, x0, #0x70 ; ... ; str w21, [x19, #0x70] ;
//     movi.2d v0, #0 ; stp q0, q0, [x19, #0x180] ; ... ; stp q0, q0, [x19, #0x280] ; mov w8, #0x3f800000 ; mov x9, x19 ;
//     str w8, [x9, #0x80]! ; ... ; stp q0, q0, [x19, #0x2a0] ; stp q0, q0, [x19, #0x2c0] ; stp q0, q0, [x19, #0x2e0] ;
//     mov x0, x19 ; bl rf::CameraController::CalculateFrustumPoints()
constexpr Expect kCameraCreate[] = {
    { 8, Operand::Move, 0x101 },
    { 9, Operand::Offset, kCameraViewDirty },
    { 12, Operand::Unscaled, kCameraFov },
    { 15, Operand::Unscaled, kCameraWorldTransform },
    { 16, Operand::Unscaled, kCameraWorldTransform + 0x10 },
    { 19, Operand::Unscaled, kCameraWorldTransform + 0x20 },
    { 22, Operand::Unscaled, kCameraLookAt + 8 },
    { 23, Operand::PairOffset, kCameraOrtho },
    { 23, Operand::StoresZero },
    { 24, Operand::Add, kCameraId },
    { 31, Operand::Offset, kCameraId },
    { 44, Operand::Unscaled, kCameraCamera + kCameraView },
    { 82, Operand::Calls, 0, "__ZN2rf16CameraController22CalculateFrustumPointsEv" },
};
static_assert(kCameraFov + 0x10 == kCameraWorldTransform && kCameraWorldTransform + kTransformSize == kCameraLookAt
    && kCameraLookAt + 8 + 0x10 == kCameraVector + 0xc && kCameraVector + 0xc + 4 == kCameraOrtho);
constexpr CameraWord kCameraCreateWords[] = {
    { 33, StoresQuads(0, 0, 19, kCameraCamera + kCameraViewProjection) },
    { 41, StoresQuads(0, 0, 19, kCameraCamera + kCameraSize - 0x20) },
    { 78, StoresQuads(0, 0, 19, kCameraFrustumPoints) },
    { 80, StoresQuads(0, 0, 19, kCameraControllerSize - 0x20) },
};
bool CheckCameraCreate(Code const& f, std::string& problem)
{
    return CameraWordsHold(f, kCameraCreateWords, "clear the camera's products and planes, or the frustum points,", problem);
}

// ~CameraController() releases the ID.
//     ldr w8, [x0, #0x70] ; cmn w8, #1 ; ... ; bl ls::gst::Map::Release(ls::gst::NodePoolData const&)
constexpr Expect kCameraDestroy[] = {
    { 8, Operand::Offset, kCameraId },
    { 17, Operand::Calls, 0, "__ZN2ls3gst3Map7ReleaseERKNS0_12NodePoolDataE" },
};

// GetWorldTransform hands out the transform, GetWorldTranslate its translation.
//     add x0, x0, #0x1c ; ret      add x0, x0, #0x2c ; ret
constexpr Expect kCameraTransformRead[] = { { 0, Operand::Add, kCameraWorldTransform } };
constexpr Expect kCameraTranslateRead[] = { { 0, Operand::Add, kCameraWorldTransform + kTransformTranslate } };

// SetWorldTransform(transform) copies the 0x28 bytes, sets the view dirty,
// and puts the point looked at one unit along the rotated forward axis from
// the translation.
//     ldr x8, [x1, #0x20] ; ldp q1, q0, [x1] ; stur q1, [x0, #0x1c] ; stur q0, [x0, #0x2c] ; stur x8, [x0, #0x3c] ;
//     mov w8, #1 ; strb w8, [x0, #0x8] ; ldr s0, [x0, #0x1c] ; ldr s1, [x0, #0x28] ; ldr s2, [x0, #0x34] ; ... ;
//     ldur d1, [x0, #0x2c] ; ... ; stur d1, [x0, #0x44] ; str s0, [x0, #0x4c] ; ret
constexpr Expect kCameraSetTransform[] = {
    { 0, Operand::Offset, kTransformSize - 8 },
    { 2, Operand::Unscaled, kCameraWorldTransform },
    { 3, Operand::Unscaled, kCameraWorldTransform + 0x10 },
    { 4, Operand::Unscaled, kCameraWorldTransform + kTransformSize - 8 },
    { 6, Operand::ByteOffset, kCameraViewDirty },
    { 7, Operand::Offset, kCameraWorldTransform },
    { 8, Operand::Offset, kCameraWorldTransform + 0xc },
    { 9, Operand::Offset, kCameraWorldTransform + kTransformTranslate + 8 },
    { 43, Operand::Unscaled, kCameraWorldTransform + kTransformTranslate },
    { 46, Operand::Unscaled, kCameraLookAt },
    { 47, Operand::Offset, kCameraLookAt + 8 },
};

// UpdateDirtyComponents(): with the view dirty, sets the camera's view
// matrix and clears the flag; with the projection dirty, makes the
// projection from the orthographic bounds (off centre or not) or from the
// field of view and the aspect ratio, with the near and far planes, sets the
// camera's projection matrix and clears that flag; computes the frustum
// points after each.
//     ldrb w8, [x0, #0x8] ; ... ; add x0, x19, #0x80 ; add x1, sp, #0x50 ; bl rf::Camera::SetViewMatrix ; strb wzr, [x19, #0x8] ; ... ;
//     ldrb w8, [x19, #0x9] ; ... ; ldrb w8, [x19, #0xa] ; cbz ; ldrb w8, [x19, #0xb] ; cbz ; ldp s1, s3, [x19, #0x10] ; ... ;
//     ldr s2, [x19, #0x60] ; ldur d5, [x19, #0x64] ; ldr s6, [x19, #0x6c] ; ... ; ldr s0, [x19, #0x18] ; ... ; ldp s10, s11, [x19, #0x10] ;
//     ... ; ldp s1, s0, [x19, #0x60] ; ... ; ldp s1, s2, [x19, #0x68] ; ... ; ldp s2, s4, [x19, #0x10] ; ... ;
//     add x0, x19, #0x80 ; add x1, sp, #0x50 ; bl rf::Camera::SetProjectionMatrix ; strb wzr, [x19, #0x9]
constexpr Expect kCameraUpdate[] = {
    { 8, Operand::ByteOffset, kCameraViewDirty },
    { 136, Operand::Add, kCameraCamera },
    { 138, Operand::Calls, 0, "__ZN2rf6Camera13SetViewMatrixERK10Matrix4x4f" },
    { 139, Operand::ByteOffset, kCameraViewDirty },
    { 139, Operand::StoresZero },
    { 141, Operand::Calls, 0, "__ZN2rf16CameraController22CalculateFrustumPointsEv" },
    { 142, Operand::ByteOffset, kCameraProjectionDirty },
    { 147, Operand::ByteOffset, kCameraOrthographic },
    { 149, Operand::ByteOffset, kCameraOffCenter },
    { 155, Operand::Offset, kCameraOrtho },
    { 156, Operand::Unscaled, kCameraOrtho + 4 },
    { 157, Operand::Offset, kCameraOrtho + 0xc },
    { 175, Operand::Offset, kCameraAspectRatio },
    { 232, Operand::Add, kCameraCamera },
    { 234, Operand::Calls, 0, "__ZN2rf6Camera19SetProjectionMatrixERK10Matrix4x4f" },
    { 235, Operand::ByteOffset, kCameraProjectionDirty },
    { 235, Operand::StoresZero },
    { 237, Operand::Calls, 0, "__ZN2rf16CameraController22CalculateFrustumPointsEv" },
};
static_assert(kCameraNearPlane + 4 == kCameraFarPlane);
constexpr CameraWord kCameraUpdateWords[] = {
    { 151, LoadsFloats(1, 3, 19, kCameraNearPlane) },
    { 189, LoadsFloats(10, 11, 19, kCameraNearPlane) },
    { 207, LoadsFloats(1, 0, 19, kCameraOrtho) },
    { 209, LoadsFloats(1, 2, 19, kCameraOrtho + 8) },
    { 211, LoadsFloats(2, 4, 19, kCameraNearPlane) },
};
bool CheckCameraUpdate(Code const& f, std::string& problem)
{
    return CameraWordsHold(f, kCameraUpdateWords, "read the near and far planes or the orthographic bounds", problem);
}

// CalculateFrustumPoints() reads the planes, the projection's first two
// scales from the camera in the orthographic case or the field of view and
// the aspect ratio otherwise, writes the eight points (the last float ends
// the object) and the four planes after the six the camera computes (the
// last float ends the camera).
//     ldp s12, s10, [x19, #0x10] ; ... ; ldrb w8, [x19, #0xa] ; cbz ; mov x0, x19 ; bl UpdateDirtyComponents ;
//     ldr s0, [x19, #0x100] ; ... ; ldr s2, [x19, #0x114] ; ... ; str s17, [x19, #0x2a0] ; ... ; str s0, [x19, #0x2fc] ; ... ;
//     ldr s0, [x19, #0xc] ; ... ; ldr s13, [x19, #0x18] ; ... ; str s14, [x19, #0x260] ; ... ; str s1, [x19, #0x29c]
constexpr Expect kCameraFrustumPointsCalc[] = {
    { 136, Operand::ByteOffset, kCameraOrthographic },
    { 139, Operand::Calls, 0, "__ZN2rf16CameraController21UpdateDirtyComponentsEv" },
    { 140, Operand::Offset, kCameraCamera + kCameraProjection },
    { 143, Operand::Offset, kCameraCamera + kCameraProjection + 0x14 },
    { 162, Operand::Offset, kCameraFrustumPoints },
    { 221, Operand::Offset, kCameraControllerSize - 4 },
    { 223, Operand::Offset, kCameraFov },
    { 225, Operand::Offset, kCameraAspectRatio },
    { 314, Operand::Offset, kCameraCamera + kCameraCullingPlanes + 0x10 * kCameraComputedPlanes },
    { 341, Operand::Offset, kCameraCamera + kCameraSize - 4 },
};
static_assert(kCameraCamera + kCameraSize == kCameraFrustumPoints && kCameraFrustumPoints + 8 * 0xc == kCameraControllerSize
    && kCameraCullingPlanes + 10 * 0x10 == kCameraSize);
constexpr CameraWord kCameraFrustumPointsWords[] = {
    { 117, LoadsFloats(12, 10, 19, kCameraNearPlane) },
};
bool CheckCameraFrustumPoints(Code const& f, std::string& problem)
{
    return CameraWordsHold(f, kCameraFrustumPointsWords, "read the near and far planes", problem);
}

// rf::Camera::SetViewMatrix(view) keeps the view and its inverse, reads the
// projection, keeps their product and that of the projection with the
// view's rotation, and computes the six planes from the product.
//     ldp q0, q1, [x1] ; ldp q2, q3, [x1, #0x20] ; stp q2, q3, [x0, #0x20] ; stp q0, q1, [x0] ; ... ; stp q5, q7, [x0, #0x40] ;
//     stp q6, q4, [x0, #0x60] ; add x8, x0, #0x100 ; stp q0, q1, [x0, #0x100] ; stp q2, q3, [x0, #0x120] ; ldr s2, [x0, #0x100] ;
//     ldp q0, q1, [x0, #0x80] ; ... ; stp q4, q7, [x0, #0x140] ; ... ; stp q6, q0, [x0, #0x160] ; add x9, x0, #0x180 ; add x10, x0, #0x1e0 ;
//     stp x9, x10, [sp] ; mov x1, sp ; mov x0, x8 ; bl rf::Camera::CalculateCullingPlanes
constexpr Expect kCameraSetView[] = {
    { 93, Operand::Add, kCameraViewProjection },
    { 180, Operand::Add, kCameraCullingPlanes },
    { 181, Operand::Add, kCameraCullingPlanes + 0x10 * kCameraComputedPlanes },
    { 185, Operand::Calls, 0, "__ZN2rf6Camera22CalculateCullingPlanesERK10Matrix4x4fRN2ls4SpanI8Vector4fEE" },
};
constexpr CameraWord kCameraSetViewWords[] = {
    { 5, StoresQuads(2, 3, 0, kCameraView + 0x20) },
    { 6, StoresQuads(0, 1, 0, kCameraView) },
    { 91, StoresQuads(5, 7, 0, kCameraInvView) },
    { 92, StoresQuads(6, 4, 0, kCameraInvView + 0x20) },
    { 94, StoresQuads(0, 1, 0, kCameraViewProjection) },
    { 97, LoadsQuads(0, 1, 0, kCameraProjection) },
    { 177, StoresQuads(4, 7, 0, kCameraOffsetViewProjection) },
    { 179, StoresQuads(6, 0, 0, kCameraOffsetViewProjection + 0x20) },
};
bool CheckCameraSetView(Code const& f, std::string& problem)
{
    return CameraWordsHold(f, kCameraSetViewWords, "store or read a matrix", problem);
}

// rf::Camera::SetProjectionMatrix(projection) keeps the projection and its
// inverse, reads the view, and keeps and computes the same products and
// planes.
//     ldp q0, q1, [x1] ; ldp q2, q3, [x1, #0x20] ; stp q2, q3, [x0, #0xa0] ; stp q0, q1, [x0, #0x80] ; ... ; stp q1, q3, [x0, #0xc0] ;
//     stp q4, q2, [x0, #0xe0] ; add x8, x0, #0x100 ; ldp q0, q1, [x0] ; stp q0, q1, [x0, #0x100] ; ... ; stp q4, q7, [x0, #0x140] ; ... ;
//     add x9, x0, #0x180 ; add x10, x0, #0x1e0 ; ... ; bl rf::Camera::CalculateCullingPlanes
constexpr Expect kCameraSetProjection[] = {
    { 92, Operand::Add, kCameraViewProjection },
    { 181, Operand::Add, kCameraCullingPlanes },
    { 182, Operand::Add, kCameraCullingPlanes + 0x10 * kCameraComputedPlanes },
    { 186, Operand::Calls, 0, "__ZN2rf6Camera22CalculateCullingPlanesERK10Matrix4x4fRN2ls4SpanI8Vector4fEE" },
};
constexpr CameraWord kCameraSetProjectionWords[] = {
    { 5, StoresQuads(2, 3, 0, kCameraProjection + 0x20) },
    { 6, StoresQuads(0, 1, 0, kCameraProjection) },
    { 90, StoresQuads(1, 3, 0, kCameraInvProjection) },
    { 91, StoresQuads(4, 2, 0, kCameraInvProjection + 0x20) },
    { 93, LoadsQuads(0, 1, 0, kCameraView) },
    { 94, StoresQuads(0, 1, 0, kCameraViewProjection) },
    { 178, StoresQuads(4, 7, 0, kCameraOffsetViewProjection) },
};
bool CheckCameraSetProjection(Code const& f, std::string& problem)
{
    return CameraWordsHold(f, kCameraSetProjectionWords, "store or read a matrix", problem);
}
static_assert(kCameraView + 0x40 == kCameraInvView && kCameraInvView + 0x40 == kCameraProjection
    && kCameraProjection + 0x40 == kCameraInvProjection && kCameraInvProjection + 0x40 == kCameraViewProjection
    && kCameraViewProjection + 0x40 == kCameraOffsetViewProjection && kCameraOffsetViewProjection + 0x40 == kCameraCullingPlanes);

// ls::CameraComponent's constructor, inline where a camera is added to an
// entity with its controller and whether it accepts input: it clears one
// byte for the type of the master behaviour, and the next thing it writes is
// the controller, eight bytes on.
//     mov w1, #0xb0 ; bl ecs::ComponentFrameStorageAllocRaw ; mov x26, x0 ; ldrb w8, [x23] ; strb wzr, [x0] ; str x22, [x0, #0x8] ;
//     str wzr, [x0, #0x10] ; strb wzr, [x0, #0x14] ; strb w8, [x0, #0x15]
constexpr Expect kCameraComponentCreate[] = {
    { 92, Operand::Move, kCameraComponentSize },
    { 93, Operand::Calls, 0, "__ZN3ecs29ComponentFrameStorageAllocRawERNS_21ComponentFrameStorageEiRNS_26ComponentFrameStorageIndexE" },
    { 96, Operand::ByteOffset, kCameraComponentBehaviorType },
    { 96, Operand::StoresZero },
    { 97, Operand::Offset, kCameraComponentController },
    { 98, Operand::Offset, kCameraComponentController + 8 },
    { 99, Operand::ByteOffset, kCameraComponentAcceptsInput - 1 },
    { 100, Operand::ByteOffset, kCameraComponentAcceptsInput },
};
// The stores above, whole: each through x0, the storage just allocated; the
// controller from x22, which holds the third argument, and the flag from
// what the fourth points to.
constexpr CameraWord kCameraComponentCreateWords[] = {
    { 8, 0xaa0303f7 },      // mov x23, x3
    { 9, 0xaa0203f6 },      // mov x22, x2
    { 95, 0x394002e8 },     // ldrb w8, [x23]
    { 96, 0x3900001f },     // strb wzr, [x0]
    { 97, 0xf9000416 },     // str x22, [x0, #0x8]
    { 100, 0x39005408 },    // strb w8, [x0, #0x15]
};
static_assert(kCameraComponentBehaviorType == 0 && kCameraComponentController == 8 && kCameraComponentAcceptsInput == 0x15);
bool CheckCameraComponentCreate(Code const& f, std::string& problem)
{
    return CameraWordsHold(f, kCameraComponentCreateWords, "keep the controller or the flag, or clear one byte for the behaviour's type,", problem);
}

// ls::CameraSystem::OnInputEvent(entity, event) gives the event to the
// camera's master behaviour: of a component that accepts input it reads the
// type as one byte, and switches on the nine types from 1.
//     bl ecs::EntityWorld::GetComponent<ls::CameraComponent, true> ; cbz x0, ... ; ldrb w8, [x0, #0x15] ; cbz w8, ... ; mov w8, #0x0 ;
//     ldrb w9, [x0] ; sub w9, w9, #0x1 ; cmp w9, #0x8 ; b.hi ...
constexpr Expect kCameraComponentInput[] = {
    { 93, Operand::Calls, 0, "__ZN3ecs11EntityWorld12GetComponentIN2ls15CameraComponentELb1EEEPT_NS2_2IDINS_18EntityHandleTraitsEEENSt3__117integral_constantIbLb0EEE" },
    { 95, Operand::ByteOffset, kCameraComponentAcceptsInput },
    { 98, Operand::ByteOffset, kCameraComponentBehaviorType },
    { 99, Operand::Add, 1 },
    { 100, Operand::Add, 8 },
};
constexpr CameraWord kCameraComponentInputWords[] = {
    { 95, 0x39405408 },     // ldrb w8, [x0, #0x15]
    { 98, 0x39400009 },     // ldrb w9, [x0]
    { 99, 0x51000529 },     // sub w9, w9, #0x1
    { 100, 0x7100213f },    // cmp w9, #0x8
};
bool CheckCameraComponentInput(Code const& f, std::string& problem)
{
    return CameraWordsHold(f, kCameraComponentInputWords, "read the flag or the behaviour's type as one byte", problem);
}

// ---------------------------------------------------------------------------

constexpr Proof kProofs[] = {
    { GameShape::CriticalSection, "ls::CriticalSection::Lock()", "__ZN2ls15CriticalSection4LockEv",
        { 100, 0xeaeb8c0c6a089a8eull }, kCriticalSectionLock },
    { GameShape::CriticalSection, "ls::CriticalSection::Unlock()", "__ZN2ls15CriticalSection6UnlockEv",
        { 47, 0x3b143ca5ab820b72ull }, kCriticalSectionUnlock },
    { GameShape::CriticalSection, "ls::CriticalSection::~CriticalSection()", "__ZN2ls15CriticalSectionD1Ev",
        { 18, 0x5abb125cced7deabull }, kCriticalSectionDestroy },
    { GameShape::CriticalSection, "ls::Dispatcher::Dispatcher(unsigned)", "__ZN2ls10DispatcherC2Ej",
        { 115, 0x5ef0f733d39d6822ull }, {}, CheckDispatcherConstructor },

    { GameShape::SRWKernelLock, "ls::SRWKernelLock::ReadLock()", "__ZN2ls13SRWKernelLock8ReadLockEv",
        { 148, 0x4d906a959d303d45ull }, kKernelReadLock },
    { GameShape::SRWKernelLock, "ls::SRWKernelLock::ReadUnlock()", kReadUnlockSymbol,
        { 62, 0xf4aa5acc9a1ffef2ull }, kKernelReadUnlock },
    { GameShape::SRWKernelLock, "ls::SRWKernelLock::WriteLock()", kWriteLockSymbol,
        { 72, 0xf3dd37014d88c3cbull }, kKernelWriteLock },
    { GameShape::SRWKernelLock, "ls::SRWKernelLock::WriteUnlock()", "__ZN2ls13SRWKernelLock11WriteUnlockEv",
        { 21, 0x495cc7086f11e06bull }, kKernelWriteUnlock },
    { GameShape::SRWKernelLock, "srwlock_leave_exclusive(_RTL_SRWLOCK*, unsigned)", kLeaveExclusiveSymbol,
        { 63, 0x54ef19bba1dea87bull }, kKernelLeaveExclusive },
    { GameShape::SRWKernelLock, "ls::SRWKernelLock::~SRWKernelLock()", "__ZN2ls13SRWKernelLockD1Ev",
        { 17, 0xd7ecf2e0b138d2acull }, kKernelDestroy },
    { GameShape::SRWKernelLock, "vt::GraniteManager::KeepWarm(signed char, ls::FixedString const&, unsigned char)",
        "__ZN2vt14GraniteManager8KeepWarmEaRKN2ls11FixedStringEh", { 245, 0xad556ea4617ffed2ull }, kKeepWarm, CheckKeepWarm },

    { GameShape::RecursiveKernelLock, "ls::ScopedWriteLock<ls::RecursiveSRWLock<ls::SRWKernelLock>>::~ScopedWriteLock()",
        "__ZN2ls15ScopedWriteLockINS_16RecursiveSRWLockINS_13SRWKernelLockEEEED1Ev", { 38, 0x6435362f0906d43full }, kRecursiveKernelWriteUnlock },
    { GameShape::RecursiveKernelLock, "ls::ScopedReadLock<ls::RecursiveSRWLock<ls::SRWKernelLock>>::~ScopedReadLock()",
        "__ZN2ls14ScopedReadLockINS_16RecursiveSRWLockINS_13SRWKernelLockEEEvED1Ev", { 22, 0x53f1db6414260c11ull }, kRecursiveKernelReadUnlock },

    { GameShape::RecursiveSpinLock, "ls::RecursiveSRWLock<ls::SRWSpinLock>::WriteLock()",
        "__ZN2ls16RecursiveSRWLockINS_11SRWSpinLockEE9WriteLockEv", { 77, 0x7197706eecf9204eull }, kRecursiveSpinWriteLock },
    { GameShape::RecursiveSpinLock, "ls::ScopedReadLock<ls::RecursiveSRWLock<ls::SRWSpinLock>>::~ScopedReadLock()",
        "__ZN2ls14ScopedReadLockINS_16RecursiveSRWLockINS_11SRWSpinLockEEEvED1Ev", { 22, 0xe67f09d5e2891e92ull }, kRecursiveSpinReadUnlock },

    { GameShape::Variant, "ls::Variant<ls::STDString, int, double, bool, Vector3f>::~Variant()",
        "__ZN2ls7VariantIJNS_9STDStringEidb8Vector3fEED1Ev", { 38, 0x0901740f7af8d999ull }, kStringVariantDestroy },
    { GameShape::Variant, "ls::Variant<eoc::ruleset::NoneValue, int, float, ls::FixedString, bool>::Storage::Assign(Storage&&)",
        "__ZN2ls7VariantIJN3eoc7ruleset9NoneValueEifNS_11FixedStringEbEE7Storage6AssignEOS6_", { 72, 0xc6d3482e4b72b344ull }, kRulesetVariantAssign },
    { GameShape::Variant, "scr::CreatePuddleAt(..., ls::Variant<ls::ID<ecs::EntityHandleTraits>, Vector3f> const&, ...)",
        "__ZN3scr14CreatePuddleAtERN3ecs11EntityWorldERKN3esv12LevelManagerERNS3_20SurfaceActionFactoryERKN2ls7VariantIJNS9_2IDINS0_"
        "18EntityHandleTraitsEEE8Vector3fEEERKSD_N3eoc12ESurfaceTypeEiiiiffRKNSK_17ClassDescriptionsE",
        { 241, 0xd526bcfea2725c88ull }, kTargetVariantRead },

    { GameShape::Queues, "ls::MPMCQueueBounded<short>::Grow(unsigned long)", "__ZN2ls16MPMCQueueBoundedIsE4GrowEm",
        { 151, 0x29191bafb50643feull }, kQueueGrow },
    { GameShape::Queues, "ls::MPMCQueueUnbounded<int>::Emplace<int&>(int&)", "__ZN2ls18MPMCQueueUnboundedIiE7EmplaceIJRiEEEvDpOT_",
        { 96, 0x63639df21de87488ull }, kLockedQueueEmplace },
    { GameShape::Queues, "ecs::EntityHandleGenerator::Create()", "__ZN3ecs21EntityHandleGenerator6CreateEv",
        { 188, 0x0d848593d01e6165ull }, kHandleGeneratorCreate },

    { GameShape::NoesisLocks, "Noesis::Reflection::RegisterType(Noesis::Symbol, creator, filler)",
        "__ZN6Noesis10Reflection12RegisterTypeENS_6SymbolEPFPNS_4TypeES1_EPFvS3_E", { 144, 0xdf2bb314641db4eeull }, kNoesisRegisterType },
    { GameShape::NoesisLocks, "Noesis::SymbolManager::AddString(char const*)", "__ZN6Noesis13SymbolManager9AddStringEPKc",
        { 152, 0x9239fd638c932129ull }, kNoesisAddString },
    { GameShape::NoesisLocks, "Noesis::TypeClass::TypeClass(Noesis::Symbol, bool)", "__ZN6Noesis9TypeClassC2ENS_6SymbolEb",
        { 53, 0xeb2bef4824a95acbull }, kNoesisTypeClass },
    { GameShape::NoesisLocks, "Noesis::TypeClass::CollapseAncestors() const", "__ZNK6Noesis9TypeClass17CollapseAncestorsEv",
        { 209, 0xf5eab73ec3a46a8aull }, kNoesisCollapseAncestors },

    { GameShape::EntityWorld, "ecs::_private::ComponentRegistrationHelper::RegisterComponentType<navcloud::AgentComponent>(ecs::EntityWorld&)",
        "__ZN3ecs8_private27ComponentRegistrationHelper21RegisterComponentTypeIN8navcloud14AgentComponentEEEvRNS_11EntityWorldE",
        { 180, 0x4c043d36398a01d5ull }, kRegisterComponentType, CheckRegisterComponentType },
    { GameShape::EntityWorld, "ecs::core::ComponentTypeEntry::~ComponentTypeEntry()", "__ZN3ecs4core18ComponentTypeEntryD2Ev",
        { 175, 0x797afc2f2c14e5ccull }, kComponentTypeEntryDestroy },
    { GameShape::EntityWorld, "ecs::EntityWorld::IsEntityAlive(ls::ID<ecs::EntityHandleTraits> const&) const",
        "__ZNK3ecs11EntityWorld13IsEntityAliveERKN2ls2IDINS_18EntityHandleTraitsEEE", { 28, 0xf8b158967e0a11eaull }, kWorldIsEntityAlive },
    { GameShape::EntityWorld, "ecs::ComponentOps<eoc::HealthComponent>::AddImmediateDefaultComponent(...)",
        "__ZN3ecs12ComponentOpsIN3eoc15HealthComponentEE28AddImmediateDefaultComponentEN2ls2IDINS_18EntityHandleTraitsEEEi",
        { 21, 0xe0d95f37576662b0ull }, kWorldAddImmediate },
    { GameShape::EntityWorld, "ecs::EntityWorld::GetComponent<eoc::SyncedTimelineActorControlComponent const, true>(...)",
        "__ZN3ecs11EntityWorld12GetComponentIKN3eoc35SyncedTimelineActorControlComponentELb1EEEPT_N2ls2IDINS_18EntityHandleTraitsEEENSt3__117integral_constantIbLb0EEE",
        { 119, 0x2744939b8c49abbcull }, kWorldGetComponent },
    { GameShape::EntityWorld, "ecs::ComponentOps<eoc::SyncedTimelineActorControlComponent>::SendComponentAttachedSignal(...)",
        "__ZN3ecs12ComponentOpsIN3eoc35SyncedTimelineActorControlComponentEE27SendComponentAttachedSignalERKN2ls18PagedArrayIteratorIKNS_19ComponentSignalDataEEESA_",
        { 58, 0x0b329051ba5e15b2ull }, kSignalAttached },
    { GameShape::EntityWorld, "ecs::ComponentOps<eoc::SyncedTimelineActorControlComponent>::SendComponentDetachedSignal(...)",
        "__ZN3ecs12ComponentOpsIN3eoc35SyncedTimelineActorControlComponentEE27SendComponentDetachedSignalERKN2ls18PagedArrayIteratorIKNS_19ComponentSignalDataEEESA_",
        { 58, 0xebeb300b9e3f0a36ull }, kSignalDetached },
    { GameShape::EntityWorld, "ls::DynamicBitSet<ls::TaggedAllocator<int>>::Ensure(int, bool)",
        "__ZN2ls13DynamicBitSetINS_15TaggedAllocatorIiEEE6EnsureEib", { 37, 0x861b3c9fed265fbeull }, kBitSetEnsure },
    { GameShape::EntityWorld, "ls::DynamicBitSet<ls::TaggedAllocator<int>>::~DynamicBitSet()",
        "__ZN2ls13DynamicBitSetINS_15TaggedAllocatorIiEEED1Ev", { 41, 0xc3bf7180967ac865ull }, kBitSetDestroy },
    { GameShape::EntityWorld, "ecs::EntityWorld::GetComponent<eoc::HealthComponent, true>(...)",
        "__ZN3ecs11EntityWorld12GetComponentIN3eoc15HealthComponentELb1EEEPT_N2ls2IDINS_18EntityHandleTraitsEEENSt3__117integral_constantIbLb0EEE",
        { 166, 0xb865eb92af8d0559ull }, kWorldMarkChanged },
    { GameShape::EntityWorld, "ecs::EntityStorageData::~EntityStorageData()", "__ZN3ecs17EntityStorageDataD1Ev", { 641, 0x141040ab383ed4a8ull }, kStorageDataDestroy },
    { GameShape::EntityWorld, "ecs::EntityStorageClearFrameData(ecs::EntityStorageData&, ...)",
        "__ZN3ecs27EntityStorageClearFrameDataERNS_17EntityStorageDataERKN2ls11SparseArrayINS_4core18ComponentTypeEntryENS2_16DefaultAllocatorEEE",
        { 431, 0xc58be50205c3cd02ull }, kStorageClearFrameData },
    { GameShape::EntityWorld, "ecs::EntityStoragePurgeAll(ecs::EntityStorageData&, ...)",
        "__ZN3ecs21EntityStoragePurgeAllERNS_17EntityStorageDataERKN2ls11SparseArrayINS_4core18ComponentTypeEntryENS2_16DefaultAllocatorEEE",
        { 710, 0x324a8924f5a6f20eull }, kStoragePurgeAll },

    { GameShape::EntityCommands, "ecs::EntityCommandBuffer::CreateEntity()", "__ZN3ecs19EntityCommandBuffer12CreateEntityEv", { 20, 0xbc95002777447f05ull }, kCommandsCreateEntity },
    { GameShape::EntityCommands, "ecs::EntityCommandBuffer::DestroyEntity(entity)",
        "__ZN3ecs19EntityCommandBuffer13DestroyEntityEN2ls2IDINS_18EntityHandleTraitsEEE", { 20, 0xde73b89145483c2bull }, kCommandsDestroyEntity },
    { GameShape::EntityCommands, "ecs::EntityCommandBuffer::RemoveComponentUnchecked(type, entity)",
        "__ZN3ecs19EntityCommandBuffer24RemoveComponentUncheckedENS_11ComponentIdEN2ls2IDINS_18EntityHandleTraitsEEE", { 38, 0xdd1b205bdcef5fa4ull }, kCommandsRemoveComponent },
    { GameShape::EntityCommands, "ls::PagedHashMap<entity, ecs::ECBEntityChange>::EnsureUniversal(entity, allocator)",
        kEnsureEntityChange, { 65, 0xf30da62698f7050aull }, kEnsureEntityChangeCode },
    { GameShape::EntityCommands, "ecs::EntityCommandBuffer::AccessAddStorage<navcloud::AgentComponent>()",
        "__ZN3ecs19EntityCommandBuffer16AccessAddStorageIN8navcloud14AgentComponentEEERNS_21ComponentFrameStorageEv", { 76, 0xef95f4b084412759ull }, kCommandsAddStorage },
    { GameShape::EntityCommands, "ecs::EntityCommandBuffer::AddComponentUnchecked<navcloud::AgentComponent>(entity)",
        "__ZN3ecs19EntityCommandBuffer21AddComponentUncheckedIN8navcloud14AgentComponentEJEEERT_N2ls2IDINS_18EntityHandleTraitsEEEDpOT0_",
        { 50, 0xa082b5c2246f9e9eull }, kCommandsAddComponent },
    { GameShape::EntityCommands, "ecs::EntityCommandBuffer::AddComponentUnchecked<eoc::splatter::ValueChangedOneFrameComponent>(entity)",
        "__ZN3ecs19EntityCommandBuffer21AddComponentUncheckedIN3eoc8splatter29ValueChangedOneFrameComponentEJEEERT_N2ls2IDINS_18EntityHandleTraitsEEEDpOT0_",
        { 114, 0x101934131a72ea00ull }, kCommandsAddOneFrame },
    { GameShape::EntityCommands, "ecs::EntityWorld::FlushECBs()", "__ZN3ecs11EntityWorld9FlushECBsEv", { 1174, 0x6ca6d4fcd7e44c52ull }, kFlushCommands },
    { GameShape::EntityCommands, "ecs::ComponentFrameStorageAllocRaw(storage, size, index)", kFrameStorageAllocRaw,
        { 160, 0x6fc459e844d646deull }, kFrameStorageAlloc, CheckFrameStorageTables },
    { GameShape::EntityCommands, "ecs::FrameAllocator::Allocate(unsigned short)", kFrameAllocate, { 142, 0x482d07a09eb30e73ull }, kFrameAllocatorAllocate,
        CheckFramePageHeader },
    { GameShape::EntityCommands, "ecs::FrameAllocator::~FrameAllocator()", "__ZN3ecs14FrameAllocatorD1Ev", { 271, 0x621c11495254eebcull }, kFrameAllocatorDestroy },
    { GameShape::EntityCommands, "ecs::legacy::ImmediateWorldCache::GetChange(entity, type) const",
        "__ZNK3ecs6legacy19ImmediateWorldCache9GetChangeEN2ls2IDINS_18EntityHandleTraitsEEENS_11ComponentIdE", { 100, 0xb11d4d6160f4b91eull }, kCacheGetChange },
    { GameShape::EntityCommands, "ecs::legacy::ImmediateWorldCache::AddComponent<navcloud::PathRequestComponent>(entity)",
        "__ZN3ecs6legacy19ImmediateWorldCache12AddComponentIN8navcloud20PathRequestComponentEJEEERT_N2ls2IDINS_18EntityHandleTraitsEEEDpOT0_",
        { 271, 0x564d6dd3cd736d1dull }, kCacheAddComponent },
    { GameShape::EntityCommands, "ecs::EntityWorld::EntityWorld(ecs::EntityWorldSettings)", "__ZN3ecs11EntityWorldC1ENS_19EntityWorldSettingsE",
        { 2235, 0x9395d0ca19039a77ull }, kWorldCreateCache, CheckWorldCreateCache },

    { GameShape::EntityQueries, "ecs::QueryDescription::Init(counts...)", "__ZN3ecs16QueryDescription4InitEhhhhhhhhhh", { 121, 0x120dedfbc3834001ull }, kQueryInit },
    { GameShape::EntityQueries, "ecs::QueryRegistry::RegisterQueryDescription<...ls::uuid::ToHandleMappingComponent...>(registry)",
        "__ZN3ecs13QueryRegistry24RegisterQueryDescriptionINS_5query4spec4SpecIN2ls8TypeListIJNS5_4uuid24ToHandleMappingComponentEEEENS6_IJEEESA_SA_SA_SA_NS_22QueryTypePersistentTagENS_17QueryTypeAliveTagEEEEEvRS0_",
        { 226, 0x0d79bca43d2cc30bull }, kRegisterQuery },
    { GameShape::EntityQueries, "ecs::QueryRegistry::RegisterQueryDescription<...ecl::Item..., Added, Alive>(registry)",
        "__ZN3ecs13QueryRegistry24RegisterQueryDescriptionINS_5query4spec4SpecIN2ls8TypeListIJN3ecl4ItemEEEENS6_IJN3eoc6action28ActionUseConditionsComponentEEEENS6_IJEEESE_S9_SE_NS_17QueryTypeAddedTagENS_17QueryTypeAliveTagEEEEEvRS0_",
        { 245, 0xb70312f642c41641ull }, kRegisterAddedQuery },
    { GameShape::EntityQueries, "ecs::QueryRegistry::RegisterQueryDescription<...ecl::attack_range::EffectsComponent..., Removed, Alive>(registry)",
        "__ZN3ecs13QueryRegistry24RegisterQueryDescriptionINS_5query4spec4SpecIN2ls8TypeListIJN3ecl12attack_range16EffectsComponentEEEENS6_IJEEESB_SB_NS6_IJNS8_16ChangedComponentEEEESB_NS_19QueryTypeRemovedTagENS_17QueryTypeAliveTagEEEEEvRS0_",
        { 236, 0xf015c543f7ab594cull }, kRegisterRemovedQuery },
    { GameShape::EntityQueries, "ecs::QueryRegistry::RegisterQueryDescription<...ecl::attack_range::EffectsComponent..., Persistent, Dead>(registry)",
        "__ZN3ecs13QueryRegistry24RegisterQueryDescriptionINS_5query4spec4SpecIN2ls8TypeListIJN3ecl12attack_range16EffectsComponentEEEENS6_IJEEESB_SB_SB_SB_NS_22QueryTypePersistentTagENS_16QueryTypeDeadTagEEEEEvRS0_",
        { 216, 0x05da8d533aa51a6dull }, kRegisterDeadQuery },
    { GameShape::EntityQueries, "ecs::_private::QueryIterator<Persistent, Alive, true>::MoveToNextStorage(int)",
        "__ZN3ecs8_private13QueryIteratorINS_22QueryTypePersistentTagENS_17QueryTypeAliveTagELb1EE17MoveToNextStorageEi", { 178, 0x5e329d8503b10d67ull }, kQueryNextStorage },
    { GameShape::EntityQueries, "ecs::EntityStorageIterator<0u>::FetchPage()", "__ZN3ecs21EntityStorageIteratorILj0EE9FetchPageEv",
        { 74, 0xbeca2445669a802bull }, kStorageFetchPage },
    { GameShape::EntityQueries, "ecs::_private::SystemRegistrationHelper::SystemUpdate<ecl::splatter::StateSystem>(...)",
        "__ZN3ecs8_private24SystemRegistrationHelper12SystemUpdateIN3ecl8splatter11StateSystemEEEvRNS_6SystemERNS_11EntityWorldERKN2ls8GameTimeE",
        { 555, 0x46722c9b905a2574ull }, kSplatterUpdate },
    { GameShape::EntityQueries, "ecs::core::SystemDependencyExecutor::ExecuteWTKernel()", "__ZN3ecs4core24SystemDependencyExecutor15ExecuteWTKernelEv",
        { 108, 0xccfb1bd907a393ecull }, kExecuteSystem },
    { GameShape::EntityQueries, "ecs::EntityWorld::ActivateSystemInternal(ecs::core::SystemTypeEntry&)",
        "__ZN3ecs11EntityWorld22ActivateSystemInternalERNS_4core15SystemTypeEntryE", { 226, 0xbb36aa84730e0d2full }, kActivateSystem },
    { GameShape::EntityQueries, "ecs::EntityWorld::EndRegisterPhase()", "__ZN3ecs11EntityWorld16EndRegisterPhaseEv", { 3571, 0x5abf363f9181a58aull }, kEndRegisterPhase },

    { GameShape::EntityReplication, "ecs::sync::EntitiesDirtyFieldsBuffer::MarkDirty(entity, type, fields)",
        "__ZN3ecs4sync25EntitiesDirtyFieldsBuffer9MarkDirtyEN2ls2IDINS_18EntityHandleTraitsEEEiONS2_13DynamicBitSetINS2_15TaggedAllocatorIiEEEE",
        { 249, 0x388ac31524c23f63ull }, kMarkDirty },
    { GameShape::EntityReplication, "ecs::sync::SyncedData<eoc::splatter::StateComponent>::SyncedData(view, entity, buffers)",
        kSyncedDataCreate, { 8, 0xa2a5c9c6a45465c1ull }, kSyncedDataCreateCode },
    { GameShape::EntityReplication, "ecs::sync::SyncedData<eoc::splatter::StateComponent>::~SyncedData()",
        "__ZN3ecs4sync10SyncedDataIN3eoc8splatter14StateComponentEED1Ev", { 71, 0xf4eabb634a00e8eaull }, kSyncedDataDestroy },
    { GameShape::EntityReplication, "ecl::GameClient::~GameClient()", "__ZN3ecl10GameClientD2Ev", { 491, 0x6ba710da9d6e193bull }, kClientDestroy },
    { GameShape::EntityReplication, "ecs::sync::ComponentDeserializer<eoc::splatter::StateComponent>::DispatchChanges()",
        "__ZN3ecs4sync21ComponentDeserializerIN3eoc8splatter14StateComponentEE15DispatchChangesEv", { 96, 0x954b35336d5489cbull }, kDeserializerDispatch },
    { GameShape::EntityReplication, "ecs::sync::_private::FieldTracker<...splatter::StateComponent::m_SweatAmount>::Dispatch()",
        "__ZN3ecs4sync8_private12FieldTrackerIN2ls9FieldMetaIMN3eoc8splatter14StateComponentEfXadL_ZNS7_13m_SweatAmountEEEEEE8DispatchEv",
        { 52, 0x184f5ec146962c14ull }, kFieldTrackerDispatch },
    { GameShape::EntityReplication, "ecs::sync::EntityReplicationPeer::GetEntity(id) const",
        "__ZNK3ecs4sync21EntityReplicationPeer9GetEntityEN2ls2IDIN3net11NetIDTraitsEEE", { 20, 0x25246a1845051e28ull }, kPeerGetEntity },
    { GameShape::EntityReplication, "ecs::sync::EntityReplicationAuthority::GetEntity(id) const",
        "__ZNK3ecs4sync26EntityReplicationAuthority9GetEntityERKN2ls2IDIN3net11NetIDTraitsEEE", { 20, 0xeee08be1b723fe70ull }, kAuthorityGetEntity },
    { GameShape::EntityReplication, "esv::GameServer::PostUpdate(ls::GameTime const&)", "__ZN3esv10GameServer10PostUpdateERKN2ls8GameTimeE",
        { 15, 0xdca1fd8eaf35344aull }, kServerSync },
    { GameShape::EntityReplication, "ecl::CharacterTask_PickUp::Enter()", "__ZN3ecl20CharacterTask_PickUp5EnterEv", { 405, 0xfb7c48084a3ea140ull }, kClientPeerGetEntity },

    { GameShape::EffectsManager, "ls::RegisterClientSystems(ecs::EntityWorld&)", "__ZN2ls21RegisterClientSystemsERN3ecs11EntityWorldE",
        { 14516, 0x33cd58c14af1bf33ull }, kEffectsManagerCreate, CheckEffectsManagerCreate },
    { GameShape::EffectsManager, "ls::EffectsManager::~EffectsManager()", "__ZN2ls14EffectsManagerD2Ev",
        { 711, 0xa0547b36904e7d8eull }, kEffectsManagerDestroy },

    { GameShape::ResourceManager, "GameEngineSDM::Init()", "__ZN13GameEngineSDM4InitEv", { 7342, 0x37186864ebf7683dull }, kResourceManagerCreate },
    { GameShape::ResourceManager, "GameEngineSDM::Shutdown()", "__ZN13GameEngineSDM8ShutdownEv", { 4471, 0x3ce7867ad1c0812eull }, kResourceManagerShutdown },
    { GameShape::ResourceManager, "ls::ResourceManager::Destroy()", "__ZN2ls15ResourceManager7DestroyEv",
        { 2014, 0xf6335862ae68ebc8ull }, kResourceManagerDestroy },
    { GameShape::ResourceManager, "ls::ResourceManager::RemoveTexture(rf::Texture*)", "__ZN2ls15ResourceManager13RemoveTextureEPN2rf7TextureE",
        { 81, 0xeaa4e8c659f5a20eull }, kResourceManagerRemoveTexture },

    { GameShape::StringTable, "ls::gst::NodePool::NodePool(int, ls::gst::PoolSpec const&)", "__ZN2ls3gst8NodePoolC1EiRKNS0_8PoolSpecE",
        { 250, 0x67df9c2196a9f664ull }, kStringPoolCreate, CheckStringPoolLock },
    { GameShape::StringTable, "ls::gst::Get(unsigned int)", "__ZN2ls3gst3GetEj", { 44, 0xbefcfe73e0706563ull }, kStringGet, CheckStringGet },
    { GameShape::StringTable, "ls::gst::Acquire(unsigned int)", "__ZN2ls3gst7AcquireEj",
        { 57, 0xb8cc640495cdc4a2ull }, kStringAcquire, CheckStringAcquire },
    { GameShape::StringTable, "ls::gst::Map::Release(ls::gst::NodePoolData const&)", kStringReleaseSymbol,
        { 191, 0x6744be094df769b1ull }, kStringRelease, CheckStringRelease },
    { GameShape::StringTable, "ls::FixedString::~FixedString()", "__ZN2ls11FixedStringD1Ev",
        { 21, 0xbf0aee187e1501f7ull }, kFixedStringDestroy, CheckFixedStringDestroy },

    { GameShape::GameUi, "gui::GameUI::GameUI()", "__ZN3gui6GameUIC2Ev", { 4110, 0x6ed3aff6a12e263cull }, kGameUiCreate },
    { GameShape::GameUi, "gui::GameUI::~GameUI()", "__ZN3gui6GameUID2Ev", { 1661, 0x139feeb40a487df2ull }, kGameUiDestroy },
    { GameShape::GameUi, "gui::TwitchOverlayManager::~TwitchOverlayManager()", "__ZN3gui20TwitchOverlayManagerD1Ev",
        { 197, 0x481a7cc7ffc6a360ull }, kTwitchManagerDestroy },

    { GameShape::Systems, "gn::GenomeManager::GenomeManager(...)", "__ZN2gn13GenomeManagerC1EON2ls11_StringViewIcEES4_RKNS1_7VersionES7_m",
        { 619, 0x35265c49247a46f9ull }, kGenomeManagerCreate },
    { GameShape::Systems, "ls::level::LevelInstanceLoadSystem::~LevelInstanceLoadSystem()", "__ZN2ls5level23LevelInstanceLoadSystemD1Ev",
        { 64, 0x46f2f62bc3685e1aull }, kLevelLoadSystemDestroy },
    { GameShape::Systems, "ecl::RegisterSystems(ecs::EntityWorld&, ecl::SystemDependencies...)", "__ZN3ecl15RegisterSystemsERN3ecs11EntityWorldERNS_18SystemDependenciesE", { 120164, 0x23d7f7e6917ad451ull }, kLevelLoadSystemCreate },
    { GameShape::Systems, "esv::RegisterSystems(ecs::EntityWorld&, esv::SystemDependencies...)", "__ZN3esv15RegisterSystemsERN3ecs11EntityWorldERNS_18SystemDependenciesE", { 173366, 0xb1fbc98c38a0ec2dull }, kProgressionSystemCreate },
    { GameShape::Systems, "esv::DialogSystem::~DialogSystem()", "__ZN3esv12DialogSystemD2Ev", { 159, 0x35a22d1b5bbec5e4ull }, kDialogSystemDestroy },
    { GameShape::Systems, "eoc::SharedDialogGameInterface::OnManagerStart(LSDialogs::DialogManager*)",
        "__ZN3eoc25SharedDialogGameInterface14OnManagerStartEPN9LSDialogs13DialogManagerE", { 2, 0xe5aa8ef55bd5b367ull }, kDialogManagerStart },
    { GameShape::Systems, "eoc::SharedDialogGameInterface::SharedDialogGameInterface()", "__ZN3eoc25SharedDialogGameInterfaceC2Ev",
        { 3551, 0xcc5752d1a0e59c96ull }, kDialogInterfaceCreate },
    { GameShape::Systems, "LSDialogs::DialogManager::DialogManager(ls::ModManager const&, LSDialogs::INodeLogicBase*)",
        "__ZN9LSDialogs13DialogManagerC1ERKN2ls10ModManagerEPNS_14INodeLogicBaseE", { 3177, 0x852cdbcb2ac1c969ull }, kDialogManagerCreate },
    { GameShape::Systems, "LSDialogs::DialogManager::~DialogManager()", "__ZN9LSDialogs13DialogManagerD1Ev", { 1560, 0xee8341c7ad754c79ull }, kDialogManagerDestroy },

    { GameShape::Components, "esv::SoundVolumeTrigger::GetEntityObjectHandle() const", "__ZNK3esv18SoundVolumeTrigger21GetEntityObjectHandleEv",
        { 2, 0x485bec490648c09eull }, kTriggerEntityRead },
    { GameShape::Components, "esv::TimelineSceneTrigger::GetEntityObjectHandle() const", "__ZNK3esv20TimelineSceneTrigger21GetEntityObjectHandleEv",
        { 2, 0x485bec490648c09eull }, kTriggerEntityRead },
    { GameShape::Components, "ecl::TimelineSceneTrigger::GetEntityObjectHandle() const", "__ZNK3ecl20TimelineSceneTrigger21GetEntityObjectHandleEv",
        { 2, 0x485bec490648c09eull }, kTriggerEntityRead },
    { GameShape::Components, "esv::Projectile::Projectile()", "__ZN3esv10ProjectileC1Ev", { 143, 0x234fe457d8605122ull }, kProjectileCreate },
    { GameShape::Components, "esv::TimelineSystem::RegisterActor(...)", kRegisterActorSymbol, { 858, 0x885c580738cbadb5ull }, kActorControlCreate, CheckActorControlDefaults },
    { GameShape::Components, "ecs::sync::ComponentSerializer<eoc::SyncedTimelineActorControlComponent>::SerializeComponent(...)",
        "__ZNK3ecs4sync19ComponentSerializerIN3eoc35SyncedTimelineActorControlComponentEE18SerializeComponentERNS0_13NetworkWriterERKNS_11EntityWorldERKN2ls4SpanIKNSA_2IDINS_18EntityHandleTraitsEEEEERKNSB_IKiEE",
        { 49, 0xc07d80b1dd03f94full }, kActorControlSerialize },
    { GameShape::Components, "ecs::sync::ComponentSerializer<eoc::character_creation::CompanionDefinitionComponent>::SerializeComponent(...)",
        "__ZNK3ecs4sync19ComponentSerializerIN3eoc18character_creation28CompanionDefinitionComponentEE18SerializeComponentERNS0_13NetworkWriterERKNS_11EntityWorldERKN2ls4SpanIKNSB_2IDINS_18EntityHandleTraitsEEEEERKNSC_IKiEE",
        { 43, 0x5bb6def52ab74d6cull }, kCompanionSerialize, CheckCompanionSizes },
    { GameShape::Components, "ls::TLShot::TLShot()", "__ZN2ls6TLShotC1Ev", { 40, 0xfbac5a2776abe9b6ull }, kShotCreate, CheckShotEnd },
    { GameShape::Components, "ls::Construction::Construction(...)",
        "__ZN2ls12ConstructionC1ERKN3ecs13EntityRefViewIJKNS_22LevelInstanceComponentEKNS_27LevelInstanceStateComponentEKNS_18TransformComponentEEEEPKNS_20ConstructionTemplateE",
        { 83, 0x21cb33d9f2745a43ull }, kConstructionCreate },

    { GameShape::NetPeer, "net::AbstractPeer::ProcessMsg(...)", "__ZN3net12AbstractPeer10ProcessMsgERKN2ls8GameTimeEPKNS_14MessageContextEPNS_7MessageE",
        { 40, 0x1011e42e923d8310ull }, kPeerProcessMsg },
    { GameShape::NetPeer, "net::AbstractPeer::GetProtocol<esv::LoadProtocol>()", "__ZN3net12AbstractPeer11GetProtocolIN3esv12LoadProtocolEEERT_v",
        { 19, 0x921aa1c412621593ull }, kPeerGetProtocol },
    { GameShape::NetPeer, "net::MessageFactory::GetFreeMessage(int)", "__ZN3net14MessageFactory14GetFreeMessageEi", { 206, 0x30dd0ab12fd3b134ull }, kFactoryGetFree },
    { GameShape::NetPeer, "net::MessageFactory::RegisterMessage(...)", "__ZN3net14MessageFactory15RegisterMessageEiPNS_7MessageEiPKc",
        { 260, 0x09daa7ac50ed8143ull }, kFactoryRegister },
    { GameShape::NetPeer, "eocnet::ClientConnectMessage::CreateInstance() const", "__ZNK6eocnet20ClientConnectMessage14CreateInstanceEv",
        { 63, 0x046c959c1259df75ull }, kMessageCreate, CheckMessageDefaults },
    { GameShape::NetPeer, "net::Message::Reset()", "__ZN3net7Message5ResetEv", { 2, 0xfe4503b2bf895640ull }, kMessageReset },
    { GameShape::NetPeer, "net::MessageVisitor::Visit(ls::ID<net::NetIDTraits>&)", "__ZN3net14MessageVisitor5VisitERN2ls2IDINS_11NetIDTraitsEEE",
        { 9, 0xbcfdde554dd2d4bdull }, kVisitorVisitId, CheckVisitorSelects },
    { GameShape::NetPeer, "net::BitStreamSerializer::WriteInternal(void const*, unsigned long)", "__ZN3net19BitStreamSerializer13WriteInternalEPKvm",
        { 2, 0xefce015e8c6df646ull }, kSerializerWrite },

    { GameShape::NetworkSession, "ecl::JoinProtocol::SendClientConnectMsg()", "__ZN3ecl12JoinProtocol20SendClientConnectMsgEv",
        { 156, 0x90fd08e2eae5d58bull }, kJoinSendConnect, CheckConnectStrings },
    { GameShape::NetworkSession, "ecl::GameClient::OnConnectionRequestAccepted(...)",
        "__ZN3ecl10GameClient27OnConnectionRequestAcceptedERKN3net30ConnectionRequestAcceptedEventE", { 623, 0x1af827b20e3ce04bull },
        kClientAccepted, CheckClientAcceptedStatus },
    { GameShape::NetworkSession, "eocnet::ClientConnectMessage::Visit(net::MessageVisitor*)",
        "__ZN6eocnet20ClientConnectMessage5VisitEPN3net14MessageVisitorE", { 239, 0x815c0f5a19b2b309ull }, kConnectVisit },
    { GameShape::NetworkSession, "esv::GameServer::GetPeerInfo(peer const&)",
        "__ZN3esv10GameServer11GetPeerInfoERKN2ls8TypeWrapIiN3net15PeerIDClassnameELb1EEE", { 79, 0x02bc6c29f31592bcull }, kServerPeerInfo },
    { GameShape::NetworkSession, "esv::GamePeerInfo::GetPlayerInfo(short const&)", kPeerInfoGetPlayerSymbol,
        { 80, 0x7b1c0fc4f5919b68ull }, kPeerInfoPlayerInfo, CheckPlayerKeys },
    { GameShape::NetworkSession, "esv::GameServer::GetPlayerInfo(net::UserID const&)", "__ZN3esv10GameServer13GetPlayerInfoERKN3net6UserIDE",
        { 87, 0x0cf6fcfc69b8f057ull }, kServerPlayerInfo, CheckUserParts },
    { GameShape::NetworkSession, "esv::ChatProtocol::ProcessMsg(...)",
        "__ZN3esv12ChatProtocol10ProcessMsgERKN2ls8GameTimeEPKN3net14MessageContextEPNS5_7MessageE", { 688, 0xd41b9b90e79e2847ull },
        kChatContext, CheckContextUser },

    { GameShape::Stats, "RPGStats::Destroy()", "__ZN8RPGStats7DestroyEv", { 1417, 0x3a72a1559acc7d20ull }, kStatsDestroy },
    { GameShape::Stats, "RPGStats::InsertDataBuffer(...)", "__ZN8RPGStats16InsertDataBufferEN2ls9UniquePtrI19RPGStats_DataBuffervEENS0_11FixedStringE",
        { 263, 0x82309c2e6b11e2b3ull }, kStatsInsertBuffer },
    { GameShape::Stats, "RPGStats::FinishAndCleanCurrentParsedType()", "__ZN8RPGStats31FinishAndCleanCurrentParsedTypeEv",
        { 337, 0x6fb586ff857e3495ull }, kStatsFinishType },
    { GameShape::Stats, "RPGStats::AddCondition(ls::STDString const&)", "__ZN8RPGStats12AddConditionERKN2ls9STDStringE",
        { 89, 0x2e03440facfe86cfull }, kStatsAddCondition },
    { GameShape::Stats, "EocSharedSDM::Init()", "__ZN12EocSharedSDM4InitEv", { 10918, 0x755c263d9ef5abaeull }, kStatsCreate },
    { GameShape::Stats, "CNamedElementManager<StatsObject, int, 0, -1>::GetByHandle(int) const",
        "__ZNK20CNamedElementManagerI11StatsObjectiLi0ELin1EE11GetByHandleEi", { 9, 0x681b05a7ae2590bcull }, kNamedManagerByHandle },
    { GameShape::Stats, "CNamedElementManager<StatsObject, int, 0, -1>::GetHandleByName(ls::FixedString const&) const",
        "__ZNK20CNamedElementManagerI11StatsObjectiLi0ELin1EE15GetHandleByNameERKN2ls11FixedStringE", { 30, 0xb68a07b0638e4797ull }, kNamedManagerHandleByName },
    { GameShape::Stats, "CNamedElementManager<StatsObject, int, 0, -1>::GetAmountOfEntries() const",
        "__ZNK20CNamedElementManagerI11StatsObjectiLi0ELin1EE18GetAmountOfEntriesEv", { 2, 0x71d7c8a014d03d1eull }, kNamedManagerCount },
    { GameShape::Stats, "CNamedElementManager<StatsObject, int, 0, -1>::UpdateHandles()",
        "__ZN20CNamedElementManagerI11StatsObjectiLi0ELin1EE13UpdateHandlesEv", { 25, 0x46876e7d55b19a6cull }, kObjectManagerUpdate },
    { GameShape::Stats, "CNamedElementManager<CRPGStats_Modifier_List, int, 0, -1>::UpdateHandles()",
        "__ZN20CNamedElementManagerI23CRPGStats_Modifier_ListiLi0ELin1EE13UpdateHandlesEv", { 25, 0xd12c350cbdd9b455ull }, kModifierListManagerUpdate },
    { GameShape::Stats, "CNamedElementManager<CRPGStats_Modifier, int, 0, -1>::UpdateHandles()",
        "__ZN20CNamedElementManagerI18CRPGStats_ModifieriLi0ELin1EE13UpdateHandlesEv", { 25, 0xa41cff6bdda3161cull }, kModifierManagerUpdate },
    { GameShape::Stats, "CRPGStats_Modifier_ValueList::Insert(ls::FixedString const&, int)",
        "__ZN28CRPGStats_Modifier_ValueList6InsertERKN2ls11FixedStringEi", { 60, 0x2d0e160ad9ddc11full }, kValueListInsert },
    { GameShape::Stats, "StatsObject::GetPosition(ls::FixedString const&) const", "__ZNK11StatsObject11GetPositionERKN2ls11FixedStringE",
        { 29, 0x7bfd20b0d46039f7ull }, kStatsObjectPosition, CheckStatsObjectPosition },
    { GameShape::Stats, "StatsObject::~StatsObject()", "__ZN11StatsObjectD2Ev", { 325, 0xcbeb22c1e8a98e43ull }, kStatsObjectDestroy },
    { GameShape::Stats, "RPGStats::ParseDataBuffer(...)", "__ZN8RPGStats15ParseDataBufferEN2ls9UniquePtrI19RPGStats_DataBuffervEE",
        { 2567, 0x848021d1465610d6ull }, kStatsObjectParse },
    { GameShape::Stats, "CRPGStats_Treasure_Category_Manager::AddObject(...)",
        "__ZN35CRPGStats_Treasure_Category_Manager9AddObjectERKN2ls11FixedStringES3_iiiiiii", { 357, 0xdaac09edf0adc307ull }, kTreasureCategoryAdd },
    { GameShape::Stats, "CRPGStats_Treasure_SubTable::~CRPGStats_Treasure_SubTable()", "__ZN27CRPGStats_Treasure_SubTableD1Ev",
        { 139, 0xdcfc5339bb5c8672ull }, kTreasureCategoryDestroy },
    { GameShape::Stats, "RPGStats::ParseTreasureFile(ls::FileReader&)", "__ZN8RPGStats17ParseTreasureFileERN2ls10FileReaderE",
        { 2572, 0xf6f291823d6b0dc7ull }, kTreasureTableParse },
    { GameShape::Stats, "ls::LEGACY_Set<ls::FixedString, 0, ...>::Realloc(unsigned long)",
        "__ZN2ls10LEGACY_SetINS_11FixedStringELm0ENS_12NewInterfaceIS1_EENS_17DefaultComparatorIS1_EEE7ReallocEm", { 167, 0xbd87c385df7e17dfull }, kFixedStringSetRealloc },
    { GameShape::Stats, "ls::LEGACY_Set<int, 4, ...>::Clone(...)",
        "__ZN2ls10LEGACY_SetIiLm4ENS_12NewInterfaceIiEENS_17DefaultComparatorIiEEE5CloneERKS5_", { 234, 0x6d081c289b796523ull }, kIntSetClone },

    { GameShape::Prototypes, "eoc::SpellAnimations::~SpellAnimations()", kSpellAnimationsEnd, { 319, 0x8fe7f39fe3a26c26ull }, kSpellAnimationsDestroy },
    { GameShape::Prototypes, "eoc::SpellPrototype::ParseSpellAnimations(eoc::SpellAnimations&, ls::FixedString const&)",
        kParseSpellAnimations, { 499, 0x7a0d81f495a0d7ffull }, kSpellAnimationsParse },
    { GameShape::Prototypes, "ls::LEGACY_Set<ls::AnimationSimpleNamesSet, 0, ...>::Realloc(unsigned long)",
        "__ZN2ls10LEGACY_SetINS_23AnimationSimpleNamesSetELm0ENS_12NewInterfaceIS1_EENS_17DefaultComparatorIS1_EEE7ReallocEm",
        { 149, 0xeb50072b29f079b4ull }, kAnimationNamesRealloc },
    { GameShape::Prototypes, "ls::NewInterface<ls::AnimationSimpleNamesSet>::Allocate(unsigned long)",
        "__ZN2ls12NewInterfaceINS_23AnimationSimpleNamesSetEE8AllocateEm", { 95, 0xaff997cd86e6def4ull }, kAnimationNamesAllocate },
    { GameShape::Prototypes, "eoc::SpellPrototype::Init(ls::FixedString const&)", "__ZN3eoc14SpellPrototype4InitERKN2ls11FixedStringE",
        { 4446, 0x6bf6520cf6f46c25ull }, kSpellPrototypeInit },
    { GameShape::Prototypes, "eoc::SpellPrototypeManager::Terminate()", "__ZN3eoc21SpellPrototypeManager9TerminateEv",
        { 539, 0x3ddb6cb726eca041ull }, kSpellManagerTerminate },
    { GameShape::Prototypes, "eoc::SpellPrototypeManager::~SpellPrototypeManager()", "__ZN3eoc21SpellPrototypeManagerD2Ev",
        { 71, 0x115cba0f7d5ac692ull }, kSpellManagerDestroy },
    { GameShape::Prototypes, "eoc::StatusPrototypeManager::Terminate()", "__ZN3eoc22StatusPrototypeManager9TerminateEv",
        { 246, 0x5ccfa5bd72dd630aull }, kStatusManagerTerminate },
    { GameShape::Prototypes, "eoc::StatusPrototypeManager::~StatusPrototypeManager()", "__ZN3eoc22StatusPrototypeManagerD0Ev",
        { 65, 0xb77859b7d8d9345bull }, kStatusManagerDestroy },
    { GameShape::Prototypes, "eoc::PassivePrototype::~PassivePrototype()", "__ZN3eoc16PassivePrototypeD1Ev", { 357, 0x74600fc3f377a583ull }, kPassiveDestroy },
    { GameShape::Prototypes, "eoc::Passives::~Passives()", "__ZN3eoc8PassivesD1Ev", { 88, 0x4166880b7b6ef7a4ull }, kPassivesDestroy },
    { GameShape::Prototypes, "eoc::InterruptPrototype::~InterruptPrototype()", "__ZN3eoc18InterruptPrototypeD1Ev", { 439, 0x3f0a32ad85d0465cull }, kInterruptDestroy },
    { GameShape::Prototypes, "eoc::InterruptPrototypeManager::~InterruptPrototypeManager()", "__ZN3eoc25InterruptPrototypeManagerD2Ev",
        { 93, 0x4f7b144b0d45cdcfull }, kInterruptManagerDestroy },

    { GameShape::Client, "App::IsInGame(bool) const", "__ZNK3App8IsInGameEb", { 23, 0x0a8c77b054ff275eull }, kClientStateMachineRead, LoadsClient },
    { GameShape::Client, "App::IsInLobby() const", "__ZNK3App9IsInLobbyEv", { 11, 0x72b1e7c398100300ull }, kClientStateMachineRead, LoadsClient },
    { GameShape::Client, "Eoc::HasClient() const", "__ZNK3Eoc9HasClientEv", { 6, 0x29e369da2fa7510aull }, kClientGameClientRead, LoadsClient },
    { GameShape::Client, "Eoc::ClientHasModule() const", "__ZNK3Eoc15ClientHasModuleEv", { 7, 0x120c4b03f13e2c54ull }, kClientModuleRead, LoadsClient },
    { GameShape::Client, "ecl::TLAnimation::GetWorld() const", "__ZNK3ecl11TLAnimation8GetWorldEv", { 4, 0x3b35376fe1cf5055ull }, kClientWorldRead, LoadsClient },
    { GameShape::Client, "ecl::RoomTrigger::GetUUID() const", "__ZNK3ecl11RoomTrigger7GetUUIDEv", { 6, 0x85e54ebf619cd748ull }, kClientTriggerWorld, LoadsClient },

    { GameShape::Server, "Eoc::HasServer() const", "__ZNK3Eoc9HasServerEv", { 8, 0x4c445a971e2804bbull }, kServerStateMachineRead, LoadsServer },
    { GameShape::Server, "esv::OsirisGameFunctions::GetUserCount(osi::ExecutionParameter*)",
        "__ZN3esv19OsirisGameFunctions12GetUserCountEPN3osi18ExecutionParameterE", { 7, 0x42299db3ba29f092ull }, kServerGameServerRead, LoadsServer },
    { GameShape::Server, "Eoc::ServerHasModule() const", "__ZNK3Eoc15ServerHasModuleEv", { 6, 0x5b417be308c1e184ull }, kServerModuleRead, LoadsServer },
    { GameShape::Server, "esv::EoCDialogGameInterface::ReloadPathResolver()", "__ZN3esv22EoCDialogGameInterface18ReloadPathResolverEv",
        { 7, 0xdcd0d8807a57e543ull }, kServerModManagerRead, LoadsServerLater },
    { GameShape::Server, "esv::RoomTrigger::GetUUID() const", "__ZNK3esv11RoomTrigger7GetUUIDEv", { 6, 0x6ea356acce4f67c1ull }, kServerTriggerWorld, LoadsServer },
    { GameShape::Server, "esv::GameServer::PostUpdate(ls::GameTime const&)", "__ZN3esv10GameServer10PostUpdateERKN2ls8GameTimeE",
        { 15, 0xdca1fd8eaf35344aull }, kGameServerPostUpdate },
    { GameShape::Server, "esv::GameServer::GetMasterUserID() const", "__ZNK3esv10GameServer15GetMasterUserIDEv", { 53, 0x0e5f3d9c3e7b7021ull }, kGameServerMasterUser },
    { GameShape::Server, "esv::GameServer::KickPeer(...)", "__ZN3esv10GameServer8KickPeerEN2ls8TypeWrapIiN3net15PeerIDClassnameELb1EEE",
        { 201, 0x73257508c99892b5ull }, kGameServerKick },
    { GameShape::Server, "esv::GameServer::DeactivatePeer(...)", "__ZN3esv10GameServer14DeactivatePeerERKN2ls8TypeWrapIiN3net15PeerIDClassnameELb1EEE",
        { 200, 0x6abb05dc0bff3ceaull }, kGameServerDeactivate },
    { GameShape::Server, "esv::GameServer::RequestDisconnectAll(bool)", "__ZN3esv10GameServer20RequestDisconnectAllEb",
        { 155, 0x089c8dbb26abdae3ull }, kGameServerDisconnectAll },

    { GameShape::Modules, "ls::ModManager::BuildLoadOrderedModules()", "__ZN2ls10ModManager23BuildLoadOrderedModulesEv",
        { 151, 0x8fc229a7bd13e1b3ull }, kModManagerBuildOrder },
    { GameShape::Modules, "ls::ModManager::GetDataPaths(ls::ObjectSet<ls::Path>&) const",
        "__ZNK2ls10ModManager12GetDataPathsERNS_9ObjectSetINS_4PathELm0ENS_17DefaultComparatorIS2_EEEE", { 98, 0xed94bdff17eb4564ull }, kModManagerDataPaths },
    { GameShape::Modules, "ls::ModuleInfo::~ModuleInfo()", "__ZN2ls10ModuleInfoD1Ev", { 238, 0x87bce80cbc65c479ull }, kModuleInfoDestroy },
    { GameShape::Modules, "ls::ModuleInfo::SetGUID(ls::FixedString const&)", "__ZN2ls10ModuleInfo7SetGUIDERKNS_11FixedStringE",
        { 42, 0xfcafcc86cb901d5eull }, kModuleInfoSetGuid },

    { GameShape::Files, "ls::Path::Path<ls::STDString>(ls::EPathRoot, ls::STDString const&)", "__ZN2ls4PathC2IJNS_9STDStringEEEENS_9EPathRootEDpRKT_",
        { 155, 0xab98b5ac0997fc7dull }, kPathFromRoot, CheckPathFromRoot },
    { GameShape::Files, "ls::FileReader::Read(void*, unsigned long)", "__ZN2ls10FileReader4ReadEPvm", { 36, 0xd4a7640e9a01d768ull }, kFileReaderReadBytes },
    { GameShape::Files, "ls::FileReader::Close()", "__ZN2ls10FileReader5CloseEv", { 71, 0x5ab5cb0c31d3bd9cull }, kFileReaderClose },
    { GameShape::Files, "ls::FileReader::LoadFile(ls::FileHandle, ls::FileReader::EType)", "__ZN2ls10FileReader8LoadFileENS_10FileHandleENS0_5ETypeE",
        { 259, 0xe65953c2a2f9f0d1ull }, kFileReaderLoad },

    { GameShape::StateEvents, "ecl::GameStateLoad::TickLoad(unsigned int, char const*)", "__ZN3ecl13GameStateLoad8TickLoadEjPKc",
        { 33, 0x675245b7adfdf180ull }, kClientTickLoad, CheckClientTickLoad },
    { GameShape::StateEvents, "ls::EventListener_Deprecated<ecl::GameStateEventManager>::~EventListener_Deprecated()",
        "__ZN2ls24EventListener_DeprecatedIN3ecl21GameStateEventManagerEED1Ev", { 45, 0x17520cc2f3fe5e91ull }, kStateListenerRemove, CheckClientListenerRemove },
    { GameShape::StateEvents, "ls::EventListener_Deprecated<esv::GameStateEventManager>::~EventListener_Deprecated()",
        "__ZN2ls24EventListener_DeprecatedIN3esv21GameStateEventManagerEED1Ev", { 45, 0x17520cc2f3fe5e91ull }, kStateListenerRemove, CheckServerListenerRemove },

    { GameShape::UiStates, "ui::StateMachineComponent::~StateMachineComponent()", "__ZN2ui21StateMachineComponentD2Ev", { 932, 0x4c2530e9069f2bd7ull }, kUiStatesDestroy },
    { GameShape::UiStates, "ui::StateMachineComponent::StaticFillClassType(Noesis::TypeClassCreator&)",
        "__ZN2ui21StateMachineComponent19StaticFillClassTypeERN6Noesis16TypeClassCreatorE", { 70, 0xe8094da58006feb0ull }, kUiStatesProperties, CheckUiStatesProperties },
    { GameShape::UiStates, "ui::StateTemplate::StaticFillClassType(Noesis::TypeClassCreator&)",
        "__ZN2ui13StateTemplate19StaticFillClassTypeERN6Noesis16TypeClassCreatorE", { 170, 0x2918927e84c95bd7ull }, kUiStateTemplate, CheckUiStateTemplate },
    { GameShape::UiStates, "ui::StateWidget::StaticFillClassType(Noesis::TypeClassCreator&)",
        "__ZN2ui11StateWidget19StaticFillClassTypeERN6Noesis16TypeClassCreatorE", { 40, 0x31d185ecf166048bull }, kUiStateWidget, CheckUiStateWidget },
    { GameShape::UiStates, "ui::StateEvent::StaticFillClassType(Noesis::TypeClassCreator&)",
        "__ZN2ui10StateEvent19StaticFillClassTypeERN6Noesis16TypeClassCreatorE", { 97, 0x289e85facf341038ull }, kUiStateEvent, CheckUiStateEvent },
    { GameShape::UiStates, "ui::StateInitialSubstate::StaticFillClassType(Noesis::TypeClassCreator&)",
        "__ZN2ui20StateInitialSubstate19StaticFillClassTypeERN6Noesis16TypeClassCreatorE", { 15, 0x0e3329c67e3b86c5ull }, kUiStateSubstate, CheckUiStateSubstate },
    { GameShape::UiStates, "ui::StateMachineComponent::AllowEventProcessing(bool)", "__ZN2ui21StateMachineComponent20AllowEventProcessingEb",
        { 97, 0x588a5362ca2e853cull }, kUiStatesAllowEvents },
    { GameShape::UiStates, "ui::StateMachineComponent::CreateState(...)",
        "__ZN2ui21StateMachineComponent11CreateStateERKsRKN6Noesis11FixedStringILj24EEERKN2ls11FixedStringES7_", { 1607, 0xae6de495ed22d178ull }, kUiStatesCreateState },

    { GameShape::TimelineChannels, "ls::TLMaterial::ParameterChannelContainer<float>::Channels()",
        "__ZN2ls10TLMaterial25ParameterChannelContainerIfE8ChannelsEv", { 4, 0x78de37d1fbe57b3full }, kMaterialChannels, CheckChannelsReturn },
    { GameShape::TimelineChannels, "ls::TLMaterial::ParameterChannelContainer<Vector3f>::Channels()",
        "__ZN2ls10TLMaterial25ParameterChannelContainerI8Vector3fE8ChannelsEv", { 4, 0x1a311c0cdbba7bffull }, kMaterialChannels3, CheckChannelsReturn },
    { GameShape::TimelineChannels, "ls::TLMaterial::TextureParameterChannelContainer::Channels()",
        "__ZN2ls10TLMaterial32TextureParameterChannelContainer8ChannelsEv", { 4, 0x389d5e5fe0da703full }, kTextureChannels, CheckChannelsReturn },
    { GameShape::TimelineChannels, "ls::TimelineActorPropertiesReflection::ParameterChannelContainer<float>::Channels()",
        "__ZN2ls33TimelineActorPropertiesReflection25ParameterChannelContainerIfE8ChannelsEv", { 4, 0xd89a0aa9fcce79a3ull }, kReflectionChannels, CheckChannelsReturn },

    { GameShape::Switches, "App::CreateGlobalSwitches()", "__ZN3App20CreateGlobalSwitchesEv", { 48, 0x274ffad3e5367c3full }, kSwitchesCreate },
    { GameShape::Switches, "ls::GlobalSwitches::Reset()", "__ZN2ls14GlobalSwitches5ResetEv", { 155, 0xbb8c64592425438dull }, kSwitchesBaseReset },
    { GameShape::Switches, "eoc::EoCGlobalSwitches::EoCGlobalSwitches()", "__ZN3eoc17EoCGlobalSwitchesC2Ev", { 2015, 0x4d1ab7c501207585ull }, kSwitchesConstruct },
    { GameShape::Switches, "eoc::EoCGlobalSwitches::Reset()", "__ZN3eoc17EoCGlobalSwitches5ResetEv", { 1016, 0xa53167a9c8142975ull }, kSwitchesReset },
    { GameShape::Switches, "App::InitAppConfig()", "__ZN3App13InitAppConfigEv", { 768, 0xe50cf6ec2e290a60ull }, kSwitchesDefaults },
    { GameShape::Switches, "App::CopyValuesToConfig()", "__ZN3App18CopyValuesToConfigEv", { 2080, 0xac0411b4696e9cacull }, kSwitchesToConfig },

    { GameShape::Levels, "eoc::LevelManager::LevelManager(char const*, unsigned long, ls::LocalTemplateManager*)",
        "__ZN3eoc12LevelManagerC2EPKcmPN2ls20LocalTemplateManagerE", { 101, 0x9c142089a34c26c8ull }, kLevelManagerCreate },
    { GameShape::Levels, "ls::LevelManager::UnloadAllLevels()", "__ZN2ls12LevelManager15UnloadAllLevelsEv", { 147, 0x44f13fb03816d843ull }, kLevelManagerUnload },
    { GameShape::TranslatedStrings, "ls::TranslatedStringRepository::TryGet(ls::RuntimeStringHandle const&, EIdentity, EIdentity) const",
        "__ZNK2ls26TranslatedStringRepository6TryGetERKNS_19RuntimeStringHandleENS_8identity9EIdentityES5_", { 253, 0x9e9418a5253a0a7bull },
        kTranslatedStringGet, CheckTranslatedStringGet },
    { GameShape::Audio, "ww::WwiseManager::WwiseManager()", "__ZN2ww12WwiseManagerC1Ev", { 2397, 0x1ce62205442ee284ull }, kSoundObjectsCreate },
    { GameShape::Audio, "ww::WwiseManager::ActivateListener(ls::EInputPlayerIndex)",
        "__ZN2ww12WwiseManager16ActivateListenerEN2ls17EInputPlayerIndexE", { 44, 0xeec20b40724e7629ull }, kSoundActivate },
    { GameShape::Audio, "ww::WwiseManager::DeactivateListener(ls::EInputPlayerIndex)",
        "__ZN2ww12WwiseManager18DeactivateListenerEN2ls17EInputPlayerIndexE", { 60, 0x547ef0585c0d2870ull }, kSoundDeactivate },
    { GameShape::Audio, "ls::SoundManager::StopAllHUDSounds(ls::EInputPlayerIndex)",
        "__ZN2ls12SoundManager16StopAllHUDSoundsENS_17EInputPlayerIndexE", { 32, 0x14a02d74aa57f62dull }, kStopHud },
    { GameShape::Audio, "ls::SoundManager::StopAllCineHUDSounds(ls::EInputPlayerIndex)",
        "__ZN2ls12SoundManager20StopAllCineHUDSoundsENS_17EInputPlayerIndexE", { 32, 0xc4f237d7e84ba16dull }, kStopCineHud },
    { GameShape::Audio, "ls::SoundManager::StopAllAmbientSounds(ls::EInputPlayerIndex)",
        "__ZN2ls12SoundManager20StopAllAmbientSoundsENS_17EInputPlayerIndexE", { 32, 0xaaf3a6b310c923edull }, kStopAmbient },
    { GameShape::Audio, "ls::SoundManager::StopAllMusicSounds()", "__ZN2ls12SoundManager18StopAllMusicSoundsEv", { 5, 0x92a20a3ef7616210ull }, kStopMusic },
    { GameShape::Audio, "App::InitGame()", "__ZN3App8InitGameEv", { 6325, 0x0a9d33da84d2a5d1ull }, kSoundObjectsNamed, CheckSoundObjectsNamed },
    { GameShape::Audio, "ls::SoundComponent::ActiveData::ActiveData()", "__ZN2ls14SoundComponent10ActiveDataC1Ev", { 79, 0x590b2328bc435e39ull },
        kActiveDataCreate, CheckActiveDataCreate },
    { GameShape::Audio, "ecl::SoundSystem::CreateTempSound(...)",
        "__ZN3ecl11SoundSystem15CreateTempSoundERKN2ls11FixedStringES4_RK8Vector3ffb", { 743, 0x74f090aa6a91bd31ull }, kTempSound },
    { GameShape::Input, "ls::InputManager::GetInputValue(unsigned int const&, ls::EInputPlayerIndex) const",
        "__ZNK2ls12InputManager13GetInputValueERKjNS_17EInputPlayerIndexE", { 73, 0x8a117bd1847391a7ull }, kInputValue, CheckInputValue },
    { GameShape::Input, "gui::vmplayer::InputEventSystemUTHelper::GetInputDefinitions(ls::InputManager const&) const",
        "__ZNK3gui8vmplayer24InputEventSystemUTHelper19GetInputDefinitionsERKN2ls12InputManagerE", { 2, 0xba997f6a318c34f1ull }, kInputDefinitionsOf },
    { GameShape::Input, "ls::InputManager::SetPlayerDevice(ls::EInputPlayerIndex, ls::EInputDeviceID)",
        "__ZN2ls12InputManager15SetPlayerDeviceENS_17EInputPlayerIndexENS_14EInputDeviceIDE", { 130, 0xb003394ab295e49bull }, kInputPlayerDevice },
    { GameShape::Boosts, "eoc::BoostPrototypeManager::CreateBoostPrototype(ls::_StringView<char> const&, ls::FixedString const&, ...)",
        "__ZN3eoc21BoostPrototypeManager20CreateBoostPrototypeERKN2ls11_StringViewIcEERKNS1_11FixedStringES5_S5_", { 359, 0x14c74188f1a25cbfull },
        kBoostCreate, CheckBoostCreate },
    { GameShape::AtlasPaths, "ls::RefMap<ls::Path, ls::TextureAtlas*, ls::PathHasher>::GetOrAdd(ls::Path const&, bool&)",
        "__ZN2ls21DEPRECATED_RefMapImplINS_12RefMapConfigINS_15ObjectAllocatorENS_4PathENS_17ResetResetFunctorIS3_EEPNS_12TextureAtlasENS_17DummyResetFunctorIS7_EENS_10PathHasherESA_NS_15MallocInterfaceIcEEEEE8GetOrAddERKS3_Rb",
        { 141, 0xf3ffe1382c202d16ull }, kAtlasPathFind },
    { GameShape::AtlasPaths, "ls::Hash<1>(ls::MemView<unsigned char const> const&, ls::HashTraits<1>::Seed)", kPathHash, { 77, 0xf945fee03eca6b30ull }, kPathHashCode, CheckPathHash },
    { GameShape::AtlasPaths, "ls::_murmur3a::Update(...)", kMurmurUpdate, { 120, 0x93b915ebb1128236ull }, kMurmurBlocks },
    { GameShape::UiErrorHandler, "Noesis::InvokeErrorHandler(char const*, unsigned int, bool, char const*, ...)",
        "__ZN6Noesis18InvokeErrorHandlerEPKcjbS1_z", { 43, 0x3d9368ce173e4614ull }, kInvokeErrorHandler, CheckInvokeErrorHandler },
    { GameShape::Cursor, "ecl::CursorControl::OnInputEvent(ls::InputEvent const&)", "__ZN3ecl13CursorControl12OnInputEventERKN2ls10InputEventE",
        { 49, 0x9c8642436a33f1d6ull }, kCursorInput },
    { GameShape::Cursor, "ecl::CursorControl::RemoveForcedMouseCursor(ecl::EMouseCursorMode)",
        "__ZN3ecl13CursorControl23RemoveForcedMouseCursorENS_16EMouseCursorModeE", { 57, 0x240fa0f0b767528cull }, kCursorRemoveForced },
    { GameShape::DragDrop, "ls::DragDropManager::GetDragEntity(short) const", "__ZNK2ls15DragDropManager13GetDragEntityEs", { 25, 0x6e24effbc07e6c9full }, kDragDropGet },
    { GameShape::DragDrop, "ls::DragDropManager::AddController(ls::DragController*)", "__ZN2ls15DragDropManager13AddControllerEPNS_14DragControllerE",
        { 226, 0x3ef214c59b6ee8aaull }, kDragDropAdd, CheckDragDropAdd },
    { GameShape::TranslatedStringKeys, "ls::TranslatedStringKeyManager::GetTranlatedStringFromKey(ls::FixedString const&, bool) const",
        "__ZNK2ls26TranslatedStringKeyManager25GetTranlatedStringFromKeyERKNS_11FixedStringEb", { 52, 0xedd8daf4678c53fdull }, kTranslatedStringKeyGet },
    { GameShape::TranslatedStringKeys, "ls::TranslatedString::TranslatedString()", "__ZN2ls16TranslatedStringC1Ev", { 43, 0xf76ea88b385b87c8ull }, kTranslatedStringCreate },
    { GameShape::StaticData, "ls::ImmutableDataHeadmaster::Get<ls::TagManager>() const",
        "__ZNK2ls23ImmutableDataHeadmaster3GetINS_10TagManagerEEEPKT_v", { 33, 0xb3b166153e6ed131ull }, kStaticDataGet, CheckStaticDataGet },
    { GameShape::TemplateCache, "ls::CacheTemplateManagerBase::CacheTemplateManagerBase(ls::ETemplateHandleType, bool)",
        "__ZN2ls24CacheTemplateManagerBaseC2ENS_19ETemplateHandleTypeEb", { 133, 0x2c33c54d039165acull }, kTemplateCacheCreate },
    { GameShape::TemplateCache, "ls::CacheTemplateManagerBase::DecreaseRefCount(ls::TemplateHandle const&)",
        "__ZN2ls24CacheTemplateManagerBase16DecreaseRefCountERKNS_14TemplateHandleE", { 574, 0xb61c78b870b5a418ull }, kTemplateCacheRelease },
    { GameShape::VirtualResults, "ww::WwiseManager::LoadBank_Blocking(char const*)", "__ZN2ww12WwiseManager17LoadBank_BlockingEPKc",
        { 34, 0x9ad1993afa100756ull }, kLoadBankResult, CheckLoadBankResult },
    { GameShape::VirtualResults, "ww::WwiseManager::PrepareBank_Blocking(char const*)", "__ZN2ww12WwiseManager20PrepareBank_BlockingEPKc",
        { 18, 0x8f67c79b626b9b61ull }, kPrepareBankResult, CheckPrepareBankResult },
    { GameShape::VirtualResults, "ww::WwiseManager::RegisterSoundObjectInternal(char const*)",
        "__ZN2ww12WwiseManager27RegisterSoundObjectInternalEPKc", { 528, 0xe9c6f9af9ee794d4ull }, kRegisterSoundObjectResult, CheckRegisterSoundObjectResult },
    { GameShape::VirtualResults, "ww::WwiseManager::GetShortID(char const*) const", "__ZNK2ww12WwiseManager10GetShortIDEPKc",
        { 2, 0x51b92bd5133cad13ull }, kShortIdResult, CheckShortIdResult },
    { GameShape::VirtualResults, "eoc::DynamicStatsExpressionManager::CreateStatsExpression(ls::Guid const&, ls::_StringView<char> const&)",
        "__ZN3eoc29DynamicStatsExpressionManager21CreateStatsExpressionERKN2ls4GuidERKNS1_11_StringViewIcEE", { 69, 0x8b7e835c84f2c23cull },
        kCreateStatsExpression, CheckCreateStatsExpression },
    { GameShape::VirtualResults, "eoc::spell_cast::BaseTarget::LEGACY_GetTargetPosition(ecs::EntityWorld&)",
        "__ZNK3eoc10spell_cast10BaseTarget24LEGACY_GetTargetPositionERN3ecs11EntityWorldE", { 59, 0x71117e2310a94d57ull }, kLegacyTargetPosition, CheckLegacyTargetPosition },
    { GameShape::VirtualResults, "eoc::spell_cast::BaseTarget::GetTargetPosition(...)",
        "__ZNK3eoc10spell_cast10BaseTarget17GetTargetPositionERN3ecs9WorldViewIJKN2ls18TransformComponentEKNS4_16PhysicsComponentEKNS_6repose14StateComponentEEEEPK8Vector3fNS4_8OptionalINS0_19ESpellTargetingTypeEEE",
        { 68, 0x76a3b5fdc0fb44b3ull }, kTargetPosition, CheckTargetPosition },
    { GameShape::VirtualResults, "gn::TGenomeSet<float>::GetValueTypeHash()", "__ZNK2gn10TGenomeSetIfE16GetValueTypeHashEv", { 40, 0xc810e96eea05a133ull }, {}, CheckGenomeTypeHash },
    { GameShape::Input, "CoreLibSDM::Init(ls::ArgumentString const&)", "__ZN10CoreLibSDM4InitERKN2ls14ArgumentStringE", { 9610, 0x1707f22144cbe40eull }, kInputCreate, CheckInputCreate },
    { GameShape::Input, "ls::InputManager::InjectInput(ls::InputRawChange const&, bool)", "__ZN2ls12InputManager11InjectInputERKNS_14InputRawChangeEb",
        { 240, 0x7c233f7413b04932ull }, kInputInjectRaw },
    { GameShape::Input, "ls::InputManager::InjectInput(ls::input::InjectInputData const&)", "__ZN2ls12InputManager11InjectInputERKNS_5input15InjectInputDataE",
        { 1398, 0x9dfbc93e2b312210ull }, kInputInjectData },
    { GameShape::Input, "ls::InputManager::FireInputEvents(...)", "__ZN2ls12InputManager15FireInputEventsERKNS_4SpanIKNS_5input13FireEventDescEEEb",
        { 565, 0x1cba660c87ad9acdull }, kInputFire },
    { GameShape::Input, "ls::InputManager::InjectPress(...)", "__ZN2ls12InputManager11InjectPressERNS_5input13FireEventDescERKNS_10InputValueES6_",
        { 139, 0xefdf56be9205537cull }, kInputPress },
    { GameShape::Input, "ls::HashTable<ls::EInputPlayerIndex>::Ensure(ls::EInputPlayerIndex const&)", kPlayerIndexEnsure, { 85, 0x9a16dacc3320ee3aull }, kPlayerIndexKey,
        CheckPlayerIndexKey },
    { GameShape::Input, "ls::InputManager::RegisterDevice(ls::InputDevice const&)", "__ZN2ls12InputManager14RegisterDeviceERKNS_11InputDeviceE",
        { 293, 0x9434c943bd1163e3ull }, kInputRegisterDevice, CheckDeviceEventSize },
    { GameShape::Cursor, "EocClientSDM::Init()", "__ZN12EocClientSDM4InitEv", { 5934, 0xa0fe43cb4359cbbbull }, kCursorCreate, CheckCursorCreate },
    { GameShape::Cursor, "ecl::CursorControl::~CursorControl()", "__ZN3ecl13CursorControlD2Ev", { 106, 0xacea2506e9a977a0ull }, kCursorDestroy },
    { GameShape::Levels, "ls::LevelManager::GetOrCreateLevel(ls::FixedString const&)", "__ZN2ls12LevelManager16GetOrCreateLevelERKNS_11FixedStringE",
        { 138, 0xb8bf3cd655f16b7cull }, kLevelDataFind, CheckLevelDataHash },
    { GameShape::Levels, "ls::LevelData::Unload(ls::FixedString const&, bool)", "__ZN2ls9LevelData6UnloadERKNS_11FixedStringEb",
        { 387, 0xf660698974717075ull }, kLevelTrackedTemplates },
    { GameShape::Levels, "esv::LevelManager::CreateLevel(ls::LevelData*)", "__ZN3esv12LevelManager11CreateLevelEPN2ls9LevelDataE", { 481, 0x96e3536ef186da91ull },
        kServerLevelCreate, CheckServerLevelCache },
    { GameShape::Characters, "esv::Character::Character(ls::GameObjectTemplate const*)", "__ZN3esv9CharacterC1EPKN2ls18GameObjectTemplateE",
        { 100, 0x4e245ebae6d302d5ull }, kCharacterCreate, CheckCharacterCreate },
    { GameShape::Characters, "esv::Character::GetOwnerUserID(bool) const", "__ZNK3esv9Character14GetOwnerUserIDEb", { 32, 0x52023bb804f7f230ull }, kCharacterOwnerUserRead },
    { GameShape::Characters, "esv::Character::GetReservedForUserID(bool) const", "__ZNK3esv9Character20GetReservedForUserIDEb",
        { 33, 0x86d3f6c11e942f2bull }, kCharacterReservedUserRead },
    { GameShape::Characters, "esv::Character::SetReservedForUserID(net::UserID const&, bool, bool, bool, bool)",
        "__ZN3esv9Character20SetReservedForUserIDERKN3net6UserIDEbbbb", { 812, 0x5bacee03ea7395a9ull }, kCharacterSetReservedUser },
    { GameShape::Characters, "esv::Character::SetFollowCharacter(ls::ID<ecs::EntityHandleTraits>)",
        "__ZN3esv9Character18SetFollowCharacterEN2ls2IDIN3ecs18EntityHandleTraitsEEE", { 71, 0x06de7f979378cde1ull }, kCharacterSetFollow },
    { GameShape::Characters, "esv::Character::SetEnemyCharacter(ls::ID<ecs::EntityHandleTraits>)",
        "__ZN3esv9Character17SetEnemyCharacterEN2ls2IDIN3ecs18EntityHandleTraitsEEE", { 60, 0x5f549b75a40e8e13ull }, kCharacterSetEnemy },
    { GameShape::Characters, "esv::Character::CreatePlayerData()", "__ZN3esv9Character16CreatePlayerDataEv", { 53, 0x61aa78c186d824c2ull }, kCharacterCreatePlayerData },
    { GameShape::VirtualResults, "esv::SurfaceAction::GetTeamID(bool) const", "__ZNK3esv13SurfaceAction9GetTeamIDEb", { 4, 0x0f05dd0e7533e921ull }, {}, CheckGuidInPair },
    { GameShape::VirtualResults, "ls::Resource::GetLogInfo() const", "__ZNK2ls8Resource10GetLogInfoEv", { 54, 0x8a13774f2c1629aaull }, {}, CheckResultInMemory },
    { GameShape::VirtualResults, "ls::Resource::ToLogString() const", "__ZNK2ls8Resource11ToLogStringEv", { 154, 0x71347a6389457bdcull }, {}, CheckResultInMemory },
    { GameShape::VirtualResults, "ls::GameObjectTemplate::GetLogInfo() const", "__ZNK2ls18GameObjectTemplate10GetLogInfoEv", { 61, 0x872414044d716725ull }, {},
        CheckResultInMemory },
    { GameShape::VirtualResults, "ls::GameObjectTemplate::ToLogString() const", "__ZNK2ls18GameObjectTemplate11ToLogStringEv", { 197, 0xe207294604f185bfull }, {},
        CheckResultInMemory },
    { GameShape::VirtualResults, "eoc::EoCGameObjectTemplate::GetBoundMin(eoc::EBoundType) const",
        "__ZNK3eoc21EoCGameObjectTemplate11GetBoundMinENS_10EBoundTypeE", { 45, 0x7f68e0ade9d6023cull }, {}, CheckVectorInFloats },
    { GameShape::VirtualResults, "eoc::EoCGameObjectTemplate::GetBoundMax(eoc::EBoundType) const",
        "__ZNK3eoc21EoCGameObjectTemplate11GetBoundMaxENS_10EBoundTypeE", { 45, 0xbc682c7286037e0eull }, {}, CheckVectorInFloats },
    { GameShape::VirtualResults, "eoc::EoCGameObjectTemplate::GetBoundRadius(eoc::EBoundType) const",
        "__ZNK3eoc21EoCGameObjectTemplate14GetBoundRadiusENS_10EBoundTypeE", { 39, 0x52071fe5bab9afc9ull }, {}, CheckFloatResult },
    { GameShape::VirtualResults, "eoc::EoCGameObjectTemplate::GetBoundSize(eoc::EBoundType) const",
        "__ZNK3eoc21EoCGameObjectTemplate12GetBoundSizeENS_10EBoundTypeE", { 58, 0x687f3e639d8dfbd9ull }, {}, CheckFloatResult },
    { GameShape::VirtualResults, "rf::CameraController::GetCurrentFov() const", "__ZNK2rf16CameraController13GetCurrentFovEv", { 2, 0x5437c815866a01e2ull }, {},
        CheckFloatResult },
    { GameShape::VirtualResults, "rf::CameraController::GetCurrentAspectRatio() const", "__ZNK2rf16CameraController21GetCurrentAspectRatioEv",
        { 2, 0x181126f61b427f26ull }, {}, CheckFloatResult },
    { GameShape::VirtualResults, "ecl::CharacterTask::GetActionCosts() const", "__ZNK3ecl13CharacterTask14GetActionCostsEv", { 2, 0xa1f6c1b641461f86ull }, {},
        CheckEmptyArrayInMemory },
    { GameShape::VirtualResults, "ecl::CharacterTask_Loot::GetDescription() const", "__ZNK3ecl18CharacterTask_Loot14GetDescriptionEv", { 31, 0x91f888e40bb9ad99ull }, {},
        CheckResultInMemory },
    { GameShape::VirtualResults, "ecl::CharacterTask::GetDescriptionFormatted() const", "__ZNK3ecl13CharacterTask23GetDescriptionFormattedEv",
        { 25, 0x2c289843575b5d50ull }, {}, CheckResultInMemory },
    { GameShape::VirtualResults, "ecl::CharacterTask_Spell::GetPreviewTiles() const", "__ZNK3ecl19CharacterTask_Spell15GetPreviewTilesEv", { 21, 0xeb910378788df922ull },
        kPreviewTilesResult, CheckOptionalPointerInPair },
    { GameShape::VirtualResults, "ecl::CharacterTask_Spell::GetPickupEntityPosition() const", "__ZNK3ecl19CharacterTask_Spell23GetPickupEntityPositionEv",
        { 50, 0x1b58ce7e92dbbd49ull }, kPickupPositionResult, CheckOptionalPositionInPair },
    { GameShape::TranslatedStrings,
        "ls::TranslatedStringRepository::AddTranslatedString(ls::RuntimeStringHandle, ls::_StringView<char>, ls::TranslatedMapRepo<...>*, ls::EnumFlags<...>)",
        "__ZN2ls26TranslatedStringRepository19AddTranslatedStringENS_19RuntimeStringHandleENS_11_StringViewIcEEPNS_17TranslatedMapRepoINS_29RuntimeStringNoVersionHashOpsEEENS_9EnumFlagsINS_9EAddFlagsEEE",
        { 874, 0xf673526226ca9878ull }, kTranslatedStringAdd },
    { GameShape::TranslatedStrings, "ls::DynamicArray<ls::STDString*, ls::TaggedAllocator<int>>::Reallocate(int)",
        "__ZN2ls12DynamicArrayIPNS_9STDStringENS_15TaggedAllocatorIiEEE10ReallocateEi", { 151, 0x96b20c72adfc181dull }, kTranslatedStringsGrow },
    { GameShape::TranslatedStrings, "ls::STDString::~STDString()", "__ZN2ls9STDStringD1Ev", { 36, 0x0492b774a70ff077ull }, kGameStringFree },
    { GameShape::TemplateCache, "ls::CacheTemplateManagerBase::RegisterTemplate(ls::TemplateHandle const&, ls::GameObjectTemplate*, bool)",
        "__ZN2ls24CacheTemplateManagerBase16RegisterTemplateERKNS_14TemplateHandleEPNS_18GameObjectTemplateEb", { 267, 0xbf609c1198cb3361ull }, kTemplateCacheRegister },
    { GameShape::PhysicsObjects, "ls::PhysicsBox::Visit(ls::ObjectVisitor*)", "__ZN2ls10PhysicsBox5VisitEPNS_13ObjectVisitorE", { 102, 0x5f2e60da0d3b1b8aull },
        kPhysicsBoxVisit, CheckPhysicsBoxVisit },
    { GameShape::PhysicsObjects, "ls::PhysicsCapsule::Visit(ls::ObjectVisitor*)", "__ZN2ls14PhysicsCapsule5VisitEPNS_13ObjectVisitorE", { 108, 0xb2c18f2a2681e6fdull },
        kPhysicsCapsuleVisit, CheckPhysicsCapsuleVisit },
    { GameShape::NoesisTables, "Noesis::SymbolManager::AddString(char const*)", "__ZN6Noesis13SymbolManager9AddStringEPKc",
        { 152, 0x9239fd638c932129ull }, kNoesisSymbolAdd, CheckNoesisSymbolAdd },
    { GameShape::NoesisTables, "Noesis::Reflection::RegisterType(Noesis::Symbol, creator, filler)",
        "__ZN6Noesis10Reflection12RegisterTypeENS_6SymbolEPFPNS_4TypeES1_EPFvS3_E", { 144, 0xdf2bb314641db4eeull }, kNoesisReflectionRegister,
        CheckNoesisReflectionRegister },
    { GameShape::NoesisMemory, "Noesis::SetMemoryCallbacks(Noesis::MemoryCallbacks const&)", "__ZN6Noesis18SetMemoryCallbacksERKNS_15MemoryCallbacksE",
        { 21, 0x7a48291066246b72ull }, kNoesisSetMemoryCallbacks, CheckNoesisSetMemoryCallbacks },
    { GameShape::NoesisMemory, "AnsiAlloc(void*, unsigned long)", "__ZL9AnsiAllocPvm", { 2, 0x51b92bd5133cad13ull }, kNoesisAnsiAlloc },
    { GameShape::NoesisMemory, "AnsiRealloc(void*, void*, unsigned long)", "__ZL11AnsiReallocPvS_m", { 3, 0x5dceafbbfc2972efull }, kNoesisAnsiRealloc },
    { GameShape::NoesisMemory, "AnsiDealloc(void*, void*)", "__ZL11AnsiDeallocPvS_", { 2, 0x51b92bd5133cad13ull }, kNoesisAnsiDealloc },
    { GameShape::NoesisMemory, "gui::GameUI::UpdateFromUI(ls::GameTime const&)", "__ZN3gui6GameUI12UpdateFromUIERKN2ls8GameTimeE",
        { 8049, 0x991dc2523447937eull }, kNoesisInstallCallbacks, CheckNoesisInstallCallbacks },
    { GameShape::NoesisMemory, "ui::NoesisUIManager::NoesisAlloc(void*, unsigned long)", "__ZN2ui15NoesisUIManager11NoesisAllocEPvm",
        { 43, 0x4a411e157654ea4full }, kNoesisManagerAlloc },
    { GameShape::NoesisMemory, "ui::NoesisUIManager::NoesisRealloc(void*, void*, unsigned long)", "__ZN2ui15NoesisUIManager13NoesisReallocEPvS1_m",
        { 4, 0xbb0a134b64cd3ba7ull }, kNoesisManagerRealloc },
    { GameShape::NoesisMemory, "ui::NoesisUIManager::NoesisDealloc(void*, void*)", "__ZN2ui15NoesisUIManager13NoesisDeallocEPvS1_",
        { 34, 0xc1aeec5b65c0b6f4ull }, kNoesisManagerDealloc },
    { GameShape::NoesisMemory, "ls::MemoryManager::Reallocate(void*, unsigned long, int, unsigned long)", "__ZN2ls13MemoryManager10ReallocateEPvmim",
        { 94, 0x901fc46c10d69495ull }, kGameReallocate },
    { GameShape::NoesisMemory, "operator new(unsigned long)", "__Znwm", { 43, 0xedae0f25e9d30b5aull }, kGameOperatorNew },
    { GameShape::NoesisTypes, "Noesis::Type::GetTypeId() const", "__ZNK6Noesis4Type9GetTypeIdEv", { 2, 0xba87cdd4c17b5902ull }, kNoesisTypeId },
    { GameShape::NoesisTypes, "Noesis::TypeMeta::TypeMeta(Noesis::Symbol)", "__ZN6Noesis8TypeMetaC2ENS_6SymbolE", { 14, 0x5f1e8fc426061a24ull }, kNoesisTypeMeta,
        CheckNoesisTypeMeta },
    { GameShape::NoesisTypes, "Noesis::TypeClass::TypeClass(Noesis::Symbol, bool)", "__ZN6Noesis9TypeClassC2ENS_6SymbolEb",
        { 53, 0xeb2bef4824a95acbull }, kNoesisTypeClassMembers },
    { GameShape::NoesisTypes, "Noesis::TypeClass::GetBase() const", "__ZNK6Noesis9TypeClass7GetBaseEv", { 2, 0x471679d5a4a6a0deull }, kNoesisClassBaseRead },
    { GameShape::NoesisTypes, "Noesis::TypeClass::IsInterface() const", "__ZNK6Noesis9TypeClass11IsInterfaceEv", { 2, 0xcbace36a1e98d05aull }, kNoesisClassInterfaceRead },
    { GameShape::NoesisTypes, "Noesis::TypeClass::GetNumProperties() const", "__ZNK6Noesis9TypeClass16GetNumPropertiesEv", { 2, 0xacbd88e25ed59942ull },
        kNoesisClassPropertyCount },
    { GameShape::NoesisTypes, "Noesis::TypeClass::GetProperty(unsigned int) const", "__ZNK6Noesis9TypeClass11GetPropertyEj", { 3, 0x15c2053ea171b940ull },
        kNoesisClassPropertyRead },
    { GameShape::NoesisTypes, "Noesis::TypeClass::IsDescendantOf(Noesis::TypeClass const*, Noesis::TypeClass::AncestorInfo const**) const",
        "__ZNK6Noesis9TypeClass14IsDescendantOfEPKS0_PPKNS0_12AncestorInfoE", { 39, 0x16dc43bf8d4a3b43ull }, kNoesisClassDescendant },
    { GameShape::NoesisTypes, "Noesis::TypeProperty::TypeProperty(Noesis::Symbol, Noesis::Type const*)",
        "__ZN6Noesis12TypePropertyC2ENS_6SymbolEPKNS_4TypeE", { 10, 0x8bfce164ad88b8fcull }, kNoesisTypePropertyMembers, CheckNoesisTypeProperty },
    { GameShape::NoesisTypes, "Noesis::TypeEnum::GetValues() const", "__ZNK6Noesis8TypeEnum9GetValuesEv", { 4, 0x96c59b50c1eaa4f7ull }, kNoesisEnumValuesRead },
    { GameShape::NoesisTypes, "Noesis::FrameworkElement::GetName() const", "__ZNK6Noesis16FrameworkElement7GetNameEv", { 12, 0x0a090111de916ba2ull }, kNoesisNameText },
    { GameShape::NoesisTypes, "Noesis::Uri::Str() const", "__ZNK6Noesis3Uri3StrEv", { 5, 0xaf97cf55f0bef65dull }, kNoesisUriStr },
    { GameShape::NoesisTypes, "Noesis::Boxed<int>::GetValuePtr() const", "__ZNK6Noesis5BoxedIiE11GetValuePtrEv", { 2, 0x0b08947db14944c2ull }, kNoesisBoxedIntValue },
    { GameShape::NoesisTypes, "Noesis::BaseCollection::Count() const", "__ZNK6Noesis14BaseCollection5CountEv", { 2, 0xa0b179bb3d1acefaull }, kNoesisCollectionCount },
    { GameShape::NoesisTypes, "Noesis::UIElementData::FindEvent(Noesis::Symbol) const", "__ZNK6Noesis13UIElementData9FindEventENS_6SymbolE",
        { 55, 0xf67a260bbaf5ecd6ull }, kNoesisRefCountChanged },
    { GameShape::NoesisValues, "Noesis::DependencyObject::GetExpression(Noesis::DependencyProperty const*) const",
        "__ZNK6Noesis16DependencyObject13GetExpressionEPKNS_18DependencyPropertyE", { 41, 0x5be4b566391075b8ull }, kNoesisObjectValuesFind },
    { GameShape::NoesisValues, "Noesis::DependencyObject::GetValueProvider(Noesis::DependencyProperty const*) const",
        "__ZNK6Noesis16DependencyObject16GetValueProviderEPKNS_18DependencyPropertyE", { 39, 0xa55c229a6652794aull }, kNoesisObjectValueProvider },
    { GameShape::NoesisValues, "Noesis::StoredValue::GetFinalValue(unsigned int) const", "__ZNK6Noesis11StoredValue13GetFinalValueEj", { 20, 0xc25c20c53e55a716ull },
        kNoesisStoredFinal },
    { GameShape::NoesisValues, "Noesis::DependencyObject::NotifyPropertyChanged(property, stored value, ...)",
        "__ZN6Noesis16DependencyObject21NotifyPropertyChangedEPKNS_18DependencyPropertyEPNS_11StoredValueEPKvS7_bbPKNS_16PropertyMetadataE",
        { 128, 0x1a49515409bc9adfull }, kNoesisStoredInitializedTest, CheckNoesisStoredInitializedTest },
    { GameShape::NoesisValues, "Noesis::DependencyProperty::DependencyProperty(name, manager, type, owner, metadata, validate, access)",
        "__ZN6Noesis18DependencyPropertyC1ENS_6SymbolEPNS_19ValueStorageManagerEPKNS_4TypeEPKNS_9TypeClassEPNS_16PropertyMetadataEPFbPKvENS_14PropertyAccessE",
        { 45, 0x86bfa1a8fdca149eull }, kNoesisDependencyPropertyMembers, CheckNoesisDependencyPropertyMembers },
    { GameShape::NoesisValues, "Noesis::RoutedEvent::RoutedEvent(Noesis::Symbol, Noesis::TypeClass const*, Noesis::RoutingStrategy)",
        "__ZN6Noesis11RoutedEventC1ENS_6SymbolEPKNS_9TypeClassENS_15RoutingStrategyE", { 18, 0xd09266e3eb8efba8ull }, kNoesisRoutedEventMembers,
        CheckNoesisRoutedEventMembers },
    { GameShape::NoesisValues, "Noesis::UIElementData::FindEvent(Noesis::Symbol) const", "__ZNK6Noesis13UIElementData9FindEventENS_6SymbolE",
        { 55, 0xf67a260bbaf5ecd6ull }, kNoesisDataEventsFind },
    { GameShape::NoesisValues, "Noesis::DependencyData::FindProperty(Noesis::Symbol) const", "__ZNK6Noesis14DependencyData12FindPropertyENS_6SymbolE",
        { 55, 0xbe85582315ce499aull }, kNoesisDataPropertiesFind },
    { GameShape::NoesisTree, "Noesis::Visual::GetVisualParent() const", "__ZNK6Noesis6Visual15GetVisualParentEv", { 2, 0xdcb77add4fbcaa8aull }, kNoesisVisualParentRead },
    { GameShape::NoesisTree, "Noesis::FrameworkElement::GetParent() const", "__ZNK6Noesis16FrameworkElement9GetParentEv", { 2, 0x7c99d6cb04a60072ull },
        kNoesisFrameworkParentRead },
    { GameShape::NoesisTree, "Noesis::FrameworkElement::GetTemplatedParent() const", "__ZNK6Noesis16FrameworkElement18GetTemplatedParentEv", { 2, 0xe5934c61d60eac06ull },
        kNoesisFrameworkTemplatedParentRead },
    { GameShape::NoesisTree, "Noesis::FrameworkElement::ConnectInheritanceParent()", "__ZN6Noesis16FrameworkElement24ConnectInheritanceParentEv",
        { 62, 0xdf3a87206de5c10cull }, kNoesisInheritanceParent },
    { GameShape::NoesisTree, "Noesis::UIElement::AddHandler(Noesis::RoutedEvent const*, delegate)",
        "__ZN6Noesis9UIElement10AddHandlerEPKNS_11RoutedEventERKNS_8DelegateIFvPNS_13BaseComponentERKNS_15RoutedEventArgsEEEE", { 76, 0x6a5be2d4ef277114ull },
        kNoesisHandlersAdd },
    { GameShape::NoesisTree, "Noesis::UIElement::RemoveHandler(Noesis::RoutedEvent const*, delegate)",
        "__ZN6Noesis9UIElement13RemoveHandlerEPKNS_11RoutedEventERKNS_8DelegateIFvPNS_13BaseComponentERKNS_15RoutedEventArgsEEEE", { 59, 0x84f3139d2d8b8213ull },
        kNoesisHandlersRemove },

    { GameShape::UiWidgets, "ui::Widget::Widget()", "__ZN2ui6WidgetC1Ev", { 652, 0xadbe213d095b0072ull }, kUiWidgetCreate },
    { GameShape::UiWidgets, "ui::Widget::GetFilename() const", "__ZNK2ui6Widget11GetFilenameEv", { 7, 0x6a84666ab8ccf177ull }, kUiWidgetFilename },
    { GameShape::UiWidgets, "ui::WindowManager::InternalWidgetLoad(...)",
        "__ZN2ui13WindowManager18InternalWidgetLoadERKN6Noesis3UriEPNS_11StateWidgetEPNS_5StateENS_13ECanvasLayoutEPNS_6WidgetE",
        { 865, 0xc79d5b8485bb5dfdull }, kUiWidgetLoad, CheckUiWidgetLoad },
    { GameShape::UiWidgets, "ui::Widget::SetVisible(bool)", "__ZN2ui6Widget10SetVisibleEb", { 65, 0x7f6b25f0a01159e8ull }, kUiWidgetSetVisible },
    { GameShape::UiWidgets, "ui::Widget::SetHasInputEnabled(bool)", "__ZN2ui6Widget18SetHasInputEnabledEb", { 47, 0x9921c386a53a00d5ull },
        kUiWidgetSetInput },
    { GameShape::UiWidgets, "ui::State::State()", "__ZN2ui5StateC1Ev", { 228, 0x9a8ffac3ca230923ull }, kUiInstanceCreate },
    { GameShape::UiWidgets, "ui::State::~State()", "__ZN2ui5StateD2Ev", { 148, 0x2ceb7c95777bf3aaull }, kUiInstanceDestroy },
    { GameShape::UiWidgets, "ui::StateMachineComponent::AddChild(ui::State*, ui::State*)", "__ZN2ui21StateMachineComponent8AddChildEPNS_5StateES2_",
        { 151, 0xd9b0a4a557a7bb58ull }, kUiInstanceAddChild, CheckUiInstanceAddChild },
    { GameShape::UiWidgets, "ui::StateMachineComponent::CreateState(...), the state it makes",
        "__ZN2ui21StateMachineComponent11CreateStateERKsRKN6Noesis11FixedStringILj24EEERKN2ls11FixedStringES7_", { 1607, 0xae6de495ed22d178ull },
        kUiInstanceCreateState, CheckUiInstanceCreateState },
    { GameShape::UiWidgets, "ui::StateMachineComponent::OnWidgetCreated(ls::Guid const&, ui::State*, ui::Widget*)",
        "__ZN2ui21StateMachineComponent15OnWidgetCreatedERKN2ls4GuidEPNS_5StateEPNS_6WidgetE", { 218, 0xb8533d33e99ac25bull },
        kUiInstanceWidgetCreated },

    { GameShape::Items, "esv::Item::Item(ls::GameObjectTemplate const*)", "__ZN3esv4ItemC1EPKN2ls18GameObjectTemplateE", { 86, 0x7bd39f44029b10d9ull },
        kItemCreate, CheckItemCreate },
    { GameShape::Items, "esv::Item::GetEntityObjectHandle() const", "__ZNK3esv4Item21GetEntityObjectHandleEv", { 2, 0xe1e31c9f2ec1cc42ull },
        kItemObjectHandleRead },
    { GameShape::Items, "esv::Item::GetEntityRef() const", "__ZNK3esv4Item12GetEntityRefEv", { 2, 0x71e0d9b63650a972ull }, kItemEntityRead },
    { GameShape::Items, "esv::Item::IsFlag(unsigned long long) const", "__ZNK3esv4Item6IsFlagEy", { 4, 0x59a52a48b4ecb325ull }, kItemFlagRead },
    { GameShape::Items, "esv::Item::GetCurrentLevel() const", "__ZNK3esv4Item15GetCurrentLevelEv", { 2, 0x72ba487958824f32ull }, kItemLevelRead },
    { GameShape::Items, "esv::Item::GetTemplate() const", "__ZNK3esv4Item11GetTemplateEv", { 2, 0xd419e921ddaecb8eull }, kItemTemplateRead },
    { GameShape::Items, "esv::Item::IsGlobal() const", "__ZNK3esv4Item8IsGlobalEv", { 3, 0xb093943594dcac73ull }, kItemGlobalRead },
    { GameShape::Items, "esv::Item::HasPeer(peer) const", "__ZNK3esv4Item7HasPeerERKN2ls8TypeWrapIiN3net15PeerIDClassnameELb1EEE",
        { 24, 0x0925a88f38780db5ull }, kItemPeerRead },
    { GameShape::Items, "esv::Item::SetTemplate(ls::GameObjectTemplate const*)", "__ZN3esv4Item11SetTemplateEPKN2ls18GameObjectTemplateE",
        { 18, 0x66991ccd95fcaa0cull }, kItemSetTemplate },
    { GameShape::Items, "esv::Item::SetOriginalTemplate(ls::GameObjectTemplate const*)",
        "__ZN3esv4Item19SetOriginalTemplateEPKN2ls18GameObjectTemplateE", { 18, 0x8461a1c933573228ull }, kItemSetOriginalTemplate },
    { GameShape::Items, "esv::Item::SetStatsId(ls::FixedString const&)", "__ZN3esv4Item10SetStatsIdERKN2ls11FixedStringE",
        { 173, 0x6918424cf9585b6cull }, kItemSetStats },

    { GameShape::ClientCharacters, "ecl::Character::Character(ls::GameObjectTemplate const*)", "__ZN3ecl9CharacterC1EPKN2ls18GameObjectTemplateE",
        { 86, 0x040c883251b781b8ull }, kClientCharacterCreate, CheckClientCharacterCreate },
    { GameShape::ClientCharacters, "ecl::Character::GetEntityObjectHandle() const", "__ZNK3ecl9Character21GetEntityObjectHandleEv",
        { 2, 0xe1e31c9f2ec1cc42ull }, kClientCharacterObjectHandleRead },
    { GameShape::ClientCharacters, "ecl::Character::GetEntityRef() const", "__ZNK3ecl9Character12GetEntityRefEv", { 2, 0x1ed946d555eb0952ull },
        kClientCharacterEntityRead },
    { GameShape::ClientCharacters, "ecl::Character::IsFlag(unsigned long long) const", "__ZNK3ecl9Character6IsFlagEy", { 4, 0x59a52a48b4ecb325ull },
        kClientCharacterFlagRead },
    { GameShape::ClientCharacters, "ecl::Character::GetCurrentLevel() const", "__ZNK3ecl9Character15GetCurrentLevelEv", { 2, 0x71e0d9b63650a972ull },
        kClientCharacterLevelRead },
    { GameShape::ClientCharacters, "ecl::Character::GetTemplate() const", "__ZNK3ecl9Character11GetTemplateEv", { 2, 0x2e1a45bc20fff0f6ull },
        kClientCharacterTemplateRead },
    { GameShape::ClientCharacters, "ecl::Character::SetTemplate(ls::GameObjectTemplate const*)",
        "__ZN3ecl9Character11SetTemplateEPKN2ls18GameObjectTemplateE", { 17, 0x3444de5dc3aa1c50ull }, kClientCharacterSetTemplate },
    { GameShape::ClientCharacters, "ecl::Character::SetOriginalTemplate(ls::GameObjectTemplate const*)",
        "__ZN3ecl9Character19SetOriginalTemplateEPKN2ls18GameObjectTemplateE", { 17, 0xa24e96784d56b030ull }, kClientCharacterSetOriginalTemplate },
    { GameShape::ClientCharacters, "ecl::Character::GetOwnerCharacter(bool) const", "__ZNK3ecl9Character17GetOwnerCharacterEb",
        { 68, 0x7014bdc6529e8147ull }, kClientCharacterOwnerRead },
    { GameShape::ClientCharacters, "ecl::Character::CreatePlayerData()", "__ZN3ecl9Character16CreatePlayerDataEv", { 45, 0x11a6c8a4baf6df7full },
        kClientCharacterCreatePlayerData },
    { GameShape::ClientCharacters, "ecl::GameInput::OnControllerModeChanged()", "__ZN3ecl9GameInput23OnControllerModeChangedEv",
        { 46, 0x54ad06b1a35553c7ull }, kClientCharacterControllerMode },
    { GameShape::ClientCharacters, "ecl::InputController::SetControllerMode(bool)", "__ZN3ecl15InputController17SetControllerModeEb",
        { 133, 0x34b62a6543113c17ull }, kClientInputSetControllerMode },

    { GameShape::Statuses, "esv::Status::Status()", "__ZN3esv6StatusC2Ev", { 96, 0xb201fe47b9823115ull }, kStatusCreate, CheckStatusCreate },
    { GameShape::Statuses, "esv::Status::GetHandle() const", "__ZNK3esv6Status9GetHandleEv", { 2, 0xe95547a9bdbeb1daull }, kStatusHandleRead },
    { GameShape::Statuses, "esv::Status::SetHandle(ls::ObjectHandle)", "__ZN3esv6Status9SetHandleEN2ls12ObjectHandleE", { 2, 0x1b0505156e35fe2bull },
        kStatusHandleRead },
    { GameShape::Statuses, "esv::Status::GetTarget() const", "__ZNK3esv6Status9GetTargetEv", { 2, 0xf4c232334c7daf50ull }, kStatusOwnerRead },
    { GameShape::Statuses, "esv::Status::SetLifeTime(float)", "__ZN3esv6Status11SetLifeTimeEf", { 33, 0x8346e6a5b3a2f9beull }, kStatusSetLifeTime,
        CheckStatusSetLifeTime },

    { GameShape::Dialogs, "LSDialogs::DialogManager::PerformDialogLoad(ls::DialogResource const*, ...)",
        "__ZN9LSDialogs13DialogManager17PerformDialogLoadEPKN2ls14DialogResourceEPFbRNS1_13ObjectVisitorERNS_10DialogNodeEPvES9_",
        { 650, 0xca0fc027314bdfa9ull }, kDialogLoad },
    { GameShape::Dialogs, "LSDialogs::DialogManager::Update(ls::GameTime const&)", "__ZN9LSDialogs13DialogManager6UpdateERKN2ls8GameTimeE",
        { 622, 0xe771f8c4b7ba94d6ull }, kDialogUnload },
    { GameShape::Dialogs, "LSDialogs::Dialog::Visit(LSDialogs::DialogVisitor&)", "__ZN9LSDialogs6Dialog5VisitERNS_13DialogVisitorE",
        { 3041, 0xee97111c2be4bb64ull }, kDialogVisit, CheckDialogVisit },
    { GameShape::Dialogs, "ecl::DialogSystem::SystemLoad()", "__ZN3ecl12DialogSystem10SystemLoadEv", { 252, 0x8e8a7608b8715cc2ull },
        kClientDialogManagerCreate },
    { GameShape::Dialogs, "ecl::TimelineSystem::TimelineSystem(ecl::SoundSystem&, ...)",
        "__ZN3ecl14TimelineSystemC1ERNS_11SoundSystemERN2ls18SoundRoutingSystemERNS3_15ResourceManagerERNS3_24AnimationBlueprintSystemERNS3_15TransformSystemERNS3_12VisualSystemERNS_21SoundAttachmentSystemERNS_14SpeakerManagerERNS_12MusicManagerE",
        { 4072, 0x855bfb46a0ed149dull }, kClientTimelineCreate },
    { GameShape::Dialogs, "ecl::TimelineSystem::OnLevelUnload(ecl::details::GameDataEvent<...> const&)",
        "__ZN3ecl14TimelineSystem13OnLevelUnloadERKNS_7details13GameDataEventILNS_12EGameEventIDE1EN2ls11FixedStringEEE", { 13, 0xe5306119d5bf3eb5ull },
        kClientTimelineLevelUnload },
    { GameShape::Dialogs, "ecl::TimelineSystem::OnClientSessionLoaded(ecl::details::GameDataEvent<...> const&)",
        "__ZN3ecl14TimelineSystem21OnClientSessionLoadedERKNS_7details13GameDataEventILNS_12EGameEventIDE6EN2ls11FixedStringEEE",
        { 5, 0x6c1aad9832c674acull }, kClientTimelineSessionLoadedSet },
    { GameShape::Dialogs, "ecl::TimelineSystem::OnLevelDeactivate(ecl::details::GameDataEvent<...> const&)",
        "__ZN3ecl14TimelineSystem17OnLevelDeactivateERKNS_7details13GameDataEventILNS_12EGameEventIDE3EN2ls11FixedStringEEE",
        { 5, 0xf63ee5bf73445939ull }, kClientTimelineLevelDeactivatedSet },
    { GameShape::Dialogs, "ecl::TimelineSystem::OnGameStateChanged(ecl::GameStateChangedEvent const&)",
        "__ZN3ecl14TimelineSystem18OnGameStateChangedERKNS_21GameStateChangedEventE", { 18, 0xb418539ffb59f846ull }, kClientTimelineStateChanged },
    { GameShape::Dialogs, "ecl::TimelineSystem::HasAnyInteractiveTimelineActive() const",
        "__ZNK3ecl14TimelineSystem31HasAnyInteractiveTimelineActiveEv", { 39, 0xc6c0ca3269922983ull }, kClientDialogManagerRead },
    { GameShape::Cameras, "rf::CameraController::CameraController()", "__ZN2rf16CameraControllerC2Ev", { 93, 0xe18688c4a82c8f59ull }, kCameraCreate, CheckCameraCreate },
    { GameShape::Cameras, "rf::CameraController::~CameraController()", "__ZN2rf16CameraControllerD1Ev", { 24, 0xc13bb1d69564eb84ull }, kCameraDestroy },
    { GameShape::Cameras, "rf::CameraController::GetWorldTransform() const", "__ZNK2rf16CameraController17GetWorldTransformEv", { 2, 0xbac72dbe22e8b382ull },
        kCameraTransformRead },
    { GameShape::Cameras, "rf::CameraController::GetWorldTranslate() const", "__ZNK2rf16CameraController17GetWorldTranslateEv", { 2, 0xad94ad2dadb3b842ull },
        kCameraTranslateRead },
    { GameShape::Cameras, "rf::CameraController::SetWorldTransform(ls::Transform const&)",
        "__ZN2rf16CameraController17SetWorldTransformERKN2ls9TransformE", { 49, 0x435d4413c7320816ull }, kCameraSetTransform },
    { GameShape::Cameras, "rf::CameraController::UpdateDirtyComponents()", "__ZN2rf16CameraController21UpdateDirtyComponentsEv", { 245, 0x8265bd07e5a92b3bull },
        kCameraUpdate, CheckCameraUpdate },
    { GameShape::Cameras, "rf::CameraController::CalculateFrustumPoints()", "__ZN2rf16CameraController22CalculateFrustumPointsEv", { 368, 0x321238c35de7d876ull },
        kCameraFrustumPointsCalc, CheckCameraFrustumPoints },
    { GameShape::Cameras, "rf::Camera::SetViewMatrix(Matrix4x4f const&)", "__ZN2rf6Camera13SetViewMatrixERK10Matrix4x4f", { 189, 0x8e714784d9f6478aull },
        kCameraSetView, CheckCameraSetView },
    { GameShape::Cameras, "rf::Camera::SetProjectionMatrix(Matrix4x4f const&)", "__ZN2rf6Camera19SetProjectionMatrixERK10Matrix4x4f", { 190, 0xa2a19bffae555ea1ull },
        kCameraSetProjection, CheckCameraSetProjection },
    { GameShape::Cameras, "ecs::legacy::ImmediateWorldCache::AddComponent<ls::CameraComponent, ls::DefaultCameraController&, bool&>(...)",
        "__ZN3ecs6legacy19ImmediateWorldCache12AddComponentIN2ls15CameraComponentEJRNS3_23DefaultCameraControllerERbEEERT_NS3_2IDINS_18EntityHandleTraitsEEEDpOT0_",
        { 442, 0x2c59453290ff8499ull }, kCameraComponentCreate, CheckCameraComponentCreate },
    { GameShape::Cameras, "ls::CameraSystem::OnInputEvent(ecs::EntityRef const&, ls::InputEvent const&)",
        "__ZN2ls12CameraSystem12OnInputEventERKN3ecs9EntityRefERKNS_10InputEventE", { 298, 0xe116af4c3ac0a1aeull },
        kCameraComponentInput, CheckCameraComponentInput },
};

}

char const* Describe(GameShape shape)
{
    switch (shape) {
    case GameShape::CriticalSection: return "critical section";
    case GameShape::SRWKernelLock: return "kernel reader-writer lock";
    case GameShape::RecursiveKernelLock: return "recursive kernel lock";
    case GameShape::RecursiveSpinLock: return "recursive spin lock";
    case GameShape::Variant: return "variant";
    case GameShape::Queues: return "lock-free queues";
    case GameShape::NoesisLocks: return "Noesis locks";
    case GameShape::EntityWorld: return "entity world";
    case GameShape::EntityCommands: return "entity command buffers and immediate cache";
    case GameShape::EntityQueries: return "entity queries and systems";
    case GameShape::EntityReplication: return "entity replication";
    case GameShape::EffectsManager: return "effects manager";
    case GameShape::ResourceManager: return "resource manager";
    case GameShape::StringTable: return "fixed-string table";
    case GameShape::GameUi: return "game UI";
    case GameShape::Systems: return "genome manager, three systems and the dialog interface and manager";
    case GameShape::Components: return "component members";
    case GameShape::NetPeer: return "network peer";
    case GameShape::Stats: return "stats object";
    case GameShape::Prototypes: return "stats prototypes";
    case GameShape::Client: return "client object";
    case GameShape::Server: return "server object";
    case GameShape::Modules: return "mod manager";
    case GameShape::Files: return "strings, path roots, file reader";
    case GameShape::StateEvents: return "game state listeners";
    case GameShape::UiStates: return "UI state machine";
    case GameShape::TimelineChannels: return "timeline parameter channels";
    case GameShape::Switches: return "global switches";
    case GameShape::VirtualResults: return "results of virtual functions";
    case GameShape::TranslatedStrings: return "translated string repository";
    case GameShape::TemplateCache: return "template cache";
    case GameShape::StaticData: return "GUID resource manager";
    case GameShape::TranslatedStringKeys: return "translated string key manager";
    case GameShape::DragDrop: return "drag and drop manager";
    case GameShape::Cursor: return "cursor control";
    case GameShape::UiErrorHandler: return "UI library's call of its error handler";
    case GameShape::AtlasPaths: return "texture atlases by path";
    case GameShape::Boosts: return "boost prototype manager";
    case GameShape::Input: return "input manager";
    case GameShape::Audio: return "sound manager's sound objects";
    case GameShape::Levels: return "level managers";
    case GameShape::NetworkSession: return "network session";
    case GameShape::Characters: return "server character";
    case GameShape::PhysicsObjects: return "physics objects";
    case GameShape::NoesisTables: return "Noesis symbol table and type registry";
    case GameShape::NoesisMemory: return "Noesis allocation";
    case GameShape::NoesisTypes: return "Noesis types and references";
    case GameShape::NoesisValues: return "Noesis dependency values and events";
    case GameShape::NoesisTree: return "Noesis element tree and handlers";
    case GameShape::UiWidgets: return "UI widgets and state instances";
    case GameShape::Items: return "server item";
    case GameShape::ClientCharacters: return "client character";
    case GameShape::Statuses: return "server status";
    case GameShape::Cameras: return "camera controller";
    case GameShape::Dialogs: return "dialogue, and the client's timeline system with its dialog manager";
    case GameShape::Count: break;
    }
    return "?";
}

bool GameShapes::Confirmed(GameShape shape) const
{
    bool any = false;
    for (auto const& proof : Proofs) {
        if (proof.Shape != shape) continue;
        if (!proof.Problem.empty()) return false;
        any = true;
    }
    return any;
}

bool GameShapes::AllConfirmed() const
{
    for (uint8_t shape = 0; shape < (uint8_t)GameShape::Count; shape++) {
        if (!Confirmed((GameShape)shape)) return false;
    }
    return true;
}

GameShapes CheckGameShapes(GameImage const& image)
{
    constexpr size_t count = std::size(kProofs);
    char const* names[count];
    uint64_t vas[count], ends[count];
    for (size_t n = 0; n < count; n++) names[n] = kProofs[n].Symbol;
    image.FindVAs(names, vas);
    image.NextSymbolVAs(vas, ends);

    GameShapes shapes;
    for (size_t n = 0; n < count; n++) {
        auto const& proof = kProofs[n];
        FunctionProof result;
        result.Shape = proof.Shape;
        result.Function = proof.Function;
        if (!vas[n] || vas[n] < image.TextBegin() || ends[n] <= vas[n] || ends[n] > image.TextEnd()) {
            result.Problem = "not in the symbol table under the expected signature";
            shapes.Proofs.push_back(std::move(result));
            continue;
        }
        Code f{ image, vas[n], ends[n] };
        result.Found = true;
        result.Code = ShapeOfRange(image.CodeAt(f.Begin), f.Count());
        if (result.Code != proof.Expected) {
            result.Problem = "its code changed";
        } else {
            for (auto const& operand : proof.Operands) {
                if (!CheckOperand(f, operand, result.Problem)) break;
            }
            if (result.Problem.empty() && proof.Extra) proof.Extra(f, result.Problem);
        }
        shapes.Proofs.push_back(std::move(result));
    }
    return shapes;
}

void LogUnconfirmedShapes(GameShapes const& shapes)
{
    for (uint8_t n = 0; n < (uint8_t)GameShape::Count; n++) {
        auto shape = (GameShape)n;
        if (shapes.Confirmed(shape)) continue;
        for (auto const& proof : shapes.Proofs) {
            if (proof.Shape != shape || proof.Problem.empty()) continue;
            Log(LogLevel::Error, "[Layout] the game's %s is not confirmed: %s: %s (%u/%016llx)", Describe(shape), proof.Function,
                proof.Problem.c_str(), proof.Code.Instructions, (unsigned long long)proof.Code.Hash);
            break;
        }
    }
}

}
