#pragma once

#include <cstddef>
#include <cstdint>

// Where the game's NoesisGUI keeps the members that Norbyte's UI code reads
// and writes, as the game's own code shows them (Layout/GameShapes.cpp says
// which code). NoesisGUI is linked into the game executable. His code is
// compiled against the SDK's headers, whose classes and inline code are
// asserted to give the same offsets, sizes and constants
// (Layout/DefinitionChecks.cpp, Tests/NoesisTests.cpp).

namespace bg3se::mac {

// Noesis::BaseRefCounted: after the vtable, the count of references, 32
// bits, changed with atomic additions.
constexpr size_t kNoesisRefCount = 0x08;
constexpr size_t kNoesisRefCountedSize = 0x10;

// Noesis::Vector<T, 0>: the elements' address, their number (32 bits), then
// the capacity in the low 24 bits of a word whose top byte says whether the
// elements are the inline ones; an empty vector points at its own end.
constexpr size_t kNoesisVectorBegin = 0x00;
constexpr size_t kNoesisVectorSize = 0x08;
constexpr size_t kNoesisVectorCapacity = 0x0c;
constexpr uint32_t kNoesisVectorCapacityMask = 0xffffff;
constexpr uint64_t kNoesisEmptyVectorWord = 1ull << 56;  // size 0, capacity 0, small
constexpr size_t kNoesisVectorBytes = 0x10;

// Noesis::HashMap: the buckets' address, then the number of buckets at +0xc,
// the number of entries at +0x10 and the shift of the index at +0x14; the
// bucket of a hash is (hash * 2654435769) >> shift, probed linearly, and the
// map grows (to twice the buckets) once the buckets are no more than twice
// the entries.
constexpr size_t kNoesisMapBuckets = 0x00;
constexpr size_t kNoesisMapBucketCount = 0x0c;
constexpr size_t kNoesisMapUsed = 0x10;
constexpr size_t kNoesisMapShift = 0x14;
constexpr size_t kNoesisMapBytes = 0x18;
constexpr uint32_t kNoesisIndexHash = 2654435769u;
// FNV: the offset basis and the prime.
constexpr uint32_t kNoesisFnvBasis = 2166136261u;
constexpr uint32_t kNoesisFnvPrime = 16777619u;

// Noesis::SymbolManager: the vector of the symbols' strings, then the map
// from a string (without case) to its symbol. A bucket is 16 bytes: the
// string, its hash at +8 and the symbol at +0xc; a free bucket's string is 1.
// A string's hash is FNV-1 of its bytes, each made lower case (A-Z only) and
// sign-extended.
constexpr size_t kNoesisSymbolStrings = 0x00;
constexpr size_t kNoesisSymbolKeys = 0x10;
constexpr size_t kNoesisSymbolBucketHash = 0x08;
constexpr size_t kNoesisSymbolBucketValue = 0x0c;
constexpr size_t kNoesisSymbolBucketBytes = 0x10;
constexpr uint64_t kNoesisEmptyPointerKey = 1;

// Noesis::Reflection: the vector of the types' symbols, then the map from a
// symbol to its type. A bucket is 16 bytes: the symbol (32 bits) and the type
// at +8; a free bucket's symbol is 0xffffffff. A symbol's hash is the symbol.
constexpr size_t kNoesisReflectionNames = 0x00;
constexpr size_t kNoesisReflectionTypes = 0x10;
constexpr size_t kNoesisReflectionBucketValue = 0x08;
constexpr size_t kNoesisReflectionBucketBytes = 0x10;
constexpr uint32_t kNoesisEmptySymbolKey = 0xffffffffu;

// Noesis::Type: its symbol at +8. Noesis::TypeMeta: its metadata, a vector,
// at +0x10; 0x20 bytes. Noesis::TypeClass: whether it is an interface in the
// byte at +0x20, its base at +0x28, the vectors of its interfaces, properties
// and events at +0x30, +0x40 and +0x50, its ancestors at +0x60. The ancestors
// (Layout/GameLocks.h: whether they are collapsed, the vector of them sorted
// by type, the lock) are allocated with operator new; an ancestor is its type
// and its offset (+8), 16 bytes.
constexpr size_t kNoesisTypeName = 0x08;
constexpr size_t kNoesisTypeMetaData = 0x10;
constexpr size_t kNoesisTypeMetaSize = 0x20;
constexpr size_t kNoesisClassIsInterface = 0x20;
constexpr size_t kNoesisClassBase = 0x28;
constexpr size_t kNoesisClassInterfaces = 0x30;
constexpr size_t kNoesisClassProperties = 0x40;
constexpr size_t kNoesisClassEvents = 0x50;
constexpr size_t kNoesisClassAncestors = 0x60;
constexpr size_t kNoesisAncestorOffset = 0x08;
constexpr size_t kNoesisAncestorBytes = 0x10;
// Noesis::TypeProperty: its symbol at +8, its type at +0x10, its metadata (a
// vector) at +0x18; 0x28 bytes. Noesis::TypeEnum: its values (a vector) at +0x20.
constexpr size_t kNoesisPropertyName = 0x08;
constexpr size_t kNoesisPropertyType = 0x10;
constexpr size_t kNoesisPropertyMetaData = 0x18;
constexpr size_t kNoesisPropertySize = 0x28;
constexpr size_t kNoesisEnumValues = 0x20;

// Noesis::String (a FixedString<24>) and FixedString<N>: the length, then the
// capacity in 24 bits and whether the text is inline in the byte at +7; the
// text's address, or the text itself, at +8. Noesis::Uri starts with its text.
constexpr size_t kNoesisStringIsSmall = 0x07;
constexpr size_t kNoesisStringText = 0x08;
constexpr size_t kNoesisStringBytes = 0x20;
constexpr size_t kNoesisUriText = 0x00;

// Noesis::DependencyObject: the map of its stored values at +0x18, from a
// dependency property to its stored value; a bucket is the property and the
// value's address (+8), 16 bytes, a free bucket's property 1; the hash of a
// pointer is FNV-1a of its two halves.
constexpr size_t kNoesisObjectValues = 0x18;
constexpr size_t kNoesisValueBucketValue = 0x08;
constexpr size_t kNoesisValueBucketBytes = 0x10;
// Noesis::StoredValue: the value (or the address of the complex value) at
// +0, the provider at +8, the flags at +9: initialized (bit 0), complex (1),
// expression (2), animated (3), coerced (4). A complex value: the expression,
// then the base value at +8, the animated one at +0x10, the coerced one at
// +0x18. A value of more than 8 bytes is kept by its address.
constexpr size_t kNoesisStoredProvider = 0x08;
constexpr size_t kNoesisStoredFlags = 0x09;
constexpr unsigned kNoesisStoredInitialized = 0, kNoesisStoredComplex = 1, kNoesisStoredExpression = 2,
    kNoesisStoredAnimated = 3, kNoesisStoredCoerced = 4;
constexpr size_t kNoesisComplexBase = 0x08;
constexpr size_t kNoesisComplexAnimated = 0x10;
constexpr size_t kNoesisComplexCoerced = 0x18;
constexpr uint32_t kNoesisInlineValueBytes = 8;
// Noesis::DependencyProperty: its symbol at +0xc, its value manager at +0x10,
// its type at +0x18, its owner's type at +0x20, its validation at +0x28,
// whether it is read-only in the byte at +0x30, its metadata by type (a map)
// at +0x38.
constexpr size_t kNoesisPropertyDpName = 0x0c;
constexpr size_t kNoesisPropertyDpManager = 0x10;
constexpr size_t kNoesisPropertyDpType = 0x18;
constexpr size_t kNoesisPropertyDpOwner = 0x20;
constexpr size_t kNoesisPropertyDpValidate = 0x28;
constexpr size_t kNoesisPropertyDpReadOnly = 0x30;
constexpr size_t kNoesisPropertyDpMetaData = 0x38;
// Noesis::RoutedEvent: its symbol at +0xc, its owner's type at +0x10, its
// routing (32 bits) at +0x18.
constexpr size_t kNoesisEventName = 0x0c;
constexpr size_t kNoesisEventOwner = 0x10;
constexpr size_t kNoesisEventRouting = 0x18;
// Noesis::DependencyData: the map of its properties at +0x18.
// Noesis::UIElementData: the map of its routed events at +0x30. Both are
// sets of pointers keyed by the symbol the object keeps at +0xc, hashed as
// the symbol.
constexpr size_t kNoesisDataProperties = 0x18;
constexpr size_t kNoesisDataEvents = 0x30;
constexpr size_t kNoesisDataKeySymbol = 0x0c;

// Noesis::Visual: its visual parent at +0xa0. Noesis::UIElement: the map of
// its routed event handlers at +0x140, a bucket the event and a delegate
// (0x28 bytes), a free bucket's event 1. Noesis::FrameworkElement: its
// logical parent at +0x1b0, its templated parent at +0x1b8, the element it
// inherits properties from at +0x1c0.
constexpr size_t kNoesisVisualParent = 0xa0;
constexpr size_t kNoesisElementHandlers = 0x140;
constexpr size_t kNoesisHandlerBucketDelegate = 0x08;
constexpr size_t kNoesisHandlerBucketBytes = 0x28;
constexpr size_t kNoesisFrameworkParent = 0x1b0;
constexpr size_t kNoesisFrameworkTemplatedParent = 0x1b8;
constexpr size_t kNoesisFrameworkInheritanceParent = 0x1c0;
// Noesis::Delegate: 0x20 bytes, an implementation object in place whose
// vtable has, after the two destructors, GetType, Size, Equal, Invoke,
// GetInvoker, Copy, Add (+0x40) and Remove (+0x48).
constexpr size_t kNoesisDelegateBytes = 0x20;
constexpr size_t kNoesisDelegateInvoke = 0x28;
constexpr size_t kNoesisDelegateAdd = 0x40;
constexpr size_t kNoesisDelegateRemove = 0x48;

// Noesis::BaseCollection: its items, a vector of references, at +0x18.
// Noesis::Boxed<int>: the value at +0xc.
constexpr size_t kNoesisCollectionItems = 0x18;
constexpr size_t kNoesisBoxedInt = 0x0c;

// What NoesisGUI allocates with: Noesis::Alloc, Realloc and Dealloc call the
// three functions of the memory callbacks (their user data the first
// argument), which hold malloc, realloc and free until the game UI installs
// its own: malloc, ls::MemoryManager::Reallocate and free.
constexpr size_t kNoesisCallbackAlloc = 0x00;
constexpr size_t kNoesisCallbackRealloc = 0x08;
constexpr size_t kNoesisCallbackDealloc = 0x10;

}
