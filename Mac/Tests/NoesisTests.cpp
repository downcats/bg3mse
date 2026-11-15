// Norbyte's UI code against NoesisGUI's layouts as the game's code has them
// (Layout/GameNoesis.h, proven in Layout/GameShapes.cpp): the private members
// his code reaches through the SDK's accessors, and the symbol table, the
// type registry, the stored values and the handlers his code fills and
// reads, laid out and hashed as the game's code reads them.

#include <Layout/GameNoesis.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "NoesisNorbyte.h"

namespace bg3se {

// What his NoesisGUI allocates with (GameDefinitions/UI.h): the game's
// allocator, malloc and free on macOS, as the game's UI also allocates.
void* GameAllocRaw(std::size_t size) { return malloc(size); }
void GameFree(void* ptr) noexcept { free(ptr); }

}

namespace {

using namespace bg3se::mac;
using namespace bg3se::mac::test;

int gFailures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            gFailures++; \
        } \
    } while (0)

template <class T> T Read(void const* base, size_t offset)
{
    T value;
    memcpy(&value, static_cast<uint8_t const*>(base) + offset, sizeof(T));
    return value;
}

template <class T> void Write(void* base, size_t offset, T value)
{
    memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof(T));
}

// The game's hashes (Layout/GameNoesis.h).
uint32_t TextHash(char const* text)
{
    uint32_t h = kNoesisFnvBasis;
    for (auto p = text; *p; p++) h = (h * kNoesisFnvPrime) ^ (uint32_t)(int32_t)(int8_t)tolower((unsigned char)*p);
    return h;
}

uint32_t PointerHash(void const* key)
{
    auto value = (uint64_t)key;
    uint32_t h = kNoesisFnvBasis;
    h = (h ^ (uint32_t)value) * kNoesisFnvPrime;
    h = (h ^ (uint32_t)(value >> 32)) * kNoesisFnvPrime;
    return h;
}

struct Map
{
    uint8_t const* Buckets;
    uint32_t Count, Used, Shift;
};

Map MapAt(void const* map)
{
    return { Read<uint8_t const*>(map, kNoesisMapBuckets), Read<uint32_t>(map, kNoesisMapBucketCount),
        Read<uint32_t>(map, kNoesisMapUsed), Read<uint32_t>(map, kNoesisMapShift) };
}

// The bucket the game's code finds `key` in: from (hash * 2654435769) >>
// shift, probed linearly; `bytes` a bucket's size; the key is the first word,
// `empty` a free bucket's.
template <class Key>
uint8_t const* GameFind(Map const& m, uint32_t hash, Key key, Key empty, size_t bytes)
{
    if (!m.Count) return nullptr;
    uint32_t index = (uint32_t)(hash * kNoesisIndexHash) >> m.Shift;
    for (uint32_t probe = 0; probe < m.Count; probe++) {
        auto bucket = m.Buckets + bytes * ((index + probe) & (m.Count - 1));
        auto stored = Read<Key>(bucket, 0);
        if (stored == key) return bucket;
        if (stored == empty) return nullptr;
    }
    return nullptr;
}

void TestPrivateMembers()
{
    alignas(16) uint8_t object[0x60]{};
    auto type = reinterpret_cast<void*>(0x1000);
    auto owner = reinterpret_cast<void*>(0x2000);
    auto manager = reinterpret_cast<void*>(0x3000);
    Write<uint32_t>(object, kNoesisTypeName, 0x1234);
    CHECK(NorbyteTypeSymbol(object) == 0x1234);

    memset(object, 0, sizeof(object));
    Write<uint32_t>(object, kNoesisPropertyDpName, 0x55);
    Write(object, kNoesisPropertyDpManager, manager);
    Write(object, kNoesisPropertyDpType, type);
    Write(object, kNoesisPropertyDpOwner, owner);
    Write<uint8_t>(object, kNoesisPropertyDpReadOnly, 1);
    CHECK(NorbyteDependencyPropertySymbol(object) == 0x55);
    CHECK(NorbyteDependencyPropertyManager(object) == manager);
    CHECK(NorbyteDependencyPropertyType(object) == type);
    CHECK(NorbyteDependencyPropertyOwner(object) == owner);
    CHECK(NorbyteDependencyPropertyReadOnly(object));

    memset(object, 0, sizeof(object));
    Write<uint32_t>(object, kNoesisEventName, 0x66);
    Write(object, kNoesisEventOwner, owner);
    CHECK(NorbyteRoutedEventSymbol(object) == 0x66);
    CHECK(NorbyteRoutedEventOwner(object) == owner);

    memset(object, 0, sizeof(object));
    Write<int32_t>(object, kNoesisRefCount, 3);
    CHECK(NorbyteReferenceCount(object) == 3);
    CHECK(NorbyteAddReference(object) == 4);
    CHECK(Read<int32_t>(object, kNoesisRefCount) == 4);
}

void TestSizes()
{
    CHECK(NorbyteSize("Symbols.Keys") == kNoesisSymbolKeys);
    CHECK(NorbyteSize("Symbols") == kNoesisSymbolKeys + kNoesisMapBytes);
    CHECK(NorbyteSize("Types.NameToType") == kNoesisReflectionTypes);
    CHECK(NorbyteSize("Types") == kNoesisReflectionTypes + kNoesisMapBytes);
    CHECK(NorbyteSize("Values") == kNoesisMapBytes);
    CHECK(NorbyteSize("Handlers") == kNoesisMapBytes);
    CHECK(NorbyteSize("Delegate") == kNoesisDelegateBytes);
    CHECK(NorbyteSize("String") == kNoesisStringBytes);
    CHECK(NorbyteSize("StoredValue") == 0x10);
}

// The vector of `count` elements at `vector`, as the game reads it.
bool VectorHolds(void const* vector, uint32_t count)
{
    auto size = Read<uint32_t>(vector, kNoesisVectorSize);
    auto capacity = Read<uint32_t>(vector, kNoesisVectorCapacity) & kNoesisVectorCapacityMask;
    return size == count && capacity >= count && Read<void const*>(vector, kNoesisVectorBegin) != nullptr;
}

// His AddString fills the table the game's AddString reads: the vector of
// strings, and the map from the string, by the hash of its text without
// case, to its symbol; the map grows as the game's does, before an
// insertion that finds no more than twice as many buckets as entries.
void TestSymbols()
{
    auto symbols = NorbyteSymbolsMake();
    CHECK(Read<uint64_t>(symbols, kNoesisSymbolStrings + kNoesisVectorSize) == kNoesisEmptyVectorWord);
    char names[64][16];
    for (int i = 0; i < 64; i++) {
        snprintf(names[i], sizeof(names[i]), "Name%dX", i);
        NorbyteSymbolsAdd(symbols, names[i]);
    }
    CHECK(VectorHolds((uint8_t*)symbols + kNoesisSymbolStrings, 64));
    auto strings = Read<char const* const*>(symbols, kNoesisSymbolStrings + kNoesisVectorBegin);
    auto map = MapAt((uint8_t*)symbols + kNoesisSymbolKeys);
    CHECK(map.Used == 64);
    CHECK(map.Count >= 2 * map.Used && (map.Count & (map.Count - 1)) == 0);
    CHECK((1u << (32 - map.Shift)) == map.Count);
    for (uint32_t i = 0; i < 64; i++) {
        auto text = strings[i];
        CHECK(strcmp(text, names[i]) == 0);
        // The game finds a string by the address it keeps, here the same one.
        auto bucket = GameFind<char const*>(map, TextHash(text), text, (char const*)kNoesisEmptyPointerKey, kNoesisSymbolBucketBytes);
        CHECK(bucket != nullptr);
        if (!bucket) continue;
        CHECK(Read<uint32_t>(bucket, kNoesisSymbolBucketHash) == TextHash(text));
        CHECK(Read<uint32_t>(bucket, kNoesisSymbolBucketValue) == i);
    }
    // Without case, as the game hashes and compares.
    CHECK(NorbyteSymbolsFind(symbols, "name5x") == 5);
    CHECK(TextHash("name5x") == TextHash("NAME5X"));
    uint32_t free = 0;
    for (uint32_t i = 0; i < map.Count; i++) {
        free += Read<uint64_t>(map.Buckets + kNoesisSymbolBucketBytes * i, 0) == kNoesisEmptyPointerKey;
    }
    CHECK(free == map.Count - map.Used);
    NorbyteSymbolsFree(symbols);
}

// His RegisterType fills the registry the game's Reflection::GetType reads:
// the map from a symbol, hashed as itself, to the type at +8.
void TestTypes()
{
    auto types = NorbyteTypesMake();
    for (uint32_t s = 1; s <= 40; s++) NorbyteTypesAdd(types, s * 7, reinterpret_cast<void*>(0x10000ull * s));
    CHECK(VectorHolds((uint8_t*)types + kNoesisReflectionNames, 40));
    auto map = MapAt((uint8_t*)types + kNoesisReflectionTypes);
    CHECK(map.Used == 40 && map.Count >= 2 * map.Used);
    for (uint32_t s = 1; s <= 40; s++) {
        auto bucket = GameFind<uint32_t>(map, s * 7, s * 7, kNoesisEmptySymbolKey, kNoesisReflectionBucketBytes);
        CHECK(bucket && Read<void*>(bucket, kNoesisReflectionBucketValue) == reinterpret_cast<void*>(0x10000ull * s));
    }
    NorbyteTypesFree(types);
}

// The SDK's map of stored values, keyed by the property's address and hashed
// as the game hashes a pointer; and a UIElement's handlers, 0x28 bytes a
// bucket with the delegate at +8.
void TestValues()
{
    static uint8_t properties[30][0x40];
    static uint8_t stored[30][0x10];
    auto values = NorbyteValuesMake();
    for (int i = 0; i < 30; i++) NorbyteValuesAdd(values, properties[i], stored[i]);
    auto map = MapAt(values);
    CHECK(map.Used == 30 && map.Count >= 2 * map.Used);
    for (int i = 0; i < 30; i++) {
        auto bucket = GameFind<void const*>(map, PointerHash(properties[i]), properties[i], (void const*)kNoesisEmptyPointerKey,
            kNoesisValueBucketBytes);
        CHECK(bucket && Read<void*>(bucket, kNoesisValueBucketValue) == stored[i]);
        CHECK(NorbyteValuesFind(values, properties[i]) == stored[i]);
    }
    NorbyteValuesFree(values);

    static uint8_t events[20][0x20];
    auto handlers = NorbyteHandlersMake();
    for (int i = 0; i < 20; i++) NorbyteHandlersAdd(handlers, events[i]);
    map = MapAt(handlers);
    CHECK(map.Used == 20);
    for (int i = 0; i < 20; i++) {
        auto bucket = GameFind<void const*>(map, PointerHash(events[i]), events[i], (void const*)kNoesisEmptyPointerKey,
            kNoesisHandlerBucketBytes);
        // The delegate in place: its implementation's vtable first.
        CHECK(bucket && Read<void const*>(bucket, kNoesisHandlerBucketDelegate) != nullptr);
    }
    NorbyteHandlersFree(handlers);
}

// A Noesis::String: a short text in place (the byte at +7 is 1), a long one
// behind the pointer.
void TestString()
{
    alignas(8) uint8_t bytes[kNoesisStringBytes];
    NorbyteStringBytes("short", bytes);
    CHECK(Read<uint8_t>(bytes, kNoesisStringIsSmall) == 1);
    CHECK(strcmp((char const*)bytes + kNoesisStringText, "short") == 0);
    NorbyteStringFree(bytes);
    char const* long_text = "a text longer than the twenty-four characters in place";
    NorbyteStringBytes(long_text, bytes);
    CHECK(Read<uint8_t>(bytes, kNoesisStringIsSmall) == 0);
    CHECK(strcmp(Read<char const*>(bytes, kNoesisStringText), long_text) == 0);
    CHECK(Read<uint32_t>(bytes, 0) == strlen(long_text));
    NorbyteStringFree(bytes);
}

}

int main()
{
    TestPrivateMembers();
    TestSizes();
    TestSymbols();
    TestTypes();
    TestValues();
    TestString();
    if (gFailures) {
        printf("%d checks failed\n", gFailures);
        return 1;
    }
    printf("noesis tests passed\n");
    return 0;
}
