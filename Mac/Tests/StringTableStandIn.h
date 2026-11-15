// The game's table of fixed strings, stood in for a test that runs
// Norbyte's code without a game (Mac/Tests/StartupTests.cpp,
// Mac/Tests/VirtualTextureTests.cpp). Included inside the test's own
// namespace, after its Failed(std::string const&) and after <stdafx.h>,
// <Layout/GameStringTable.h>, <mutex>, <string> and <unordered_map> at file
// scope. The test's start-up puts StringCreate, StringGet, StringIncRef,
// StringDecRef and gStrings.Table where LibraryManager::FindLibraries puts
// the game's.

#pragma once

// An object of Norbyte's type, so that what his code reads of the table
// (FixedStringBase::FindEntry, IsValid, GetMetadata) reads this one as it
// reads the game's.
struct StringTableStandIn
{
    // Bytes per entry of each pool: the header, the text and its terminator.
    static constexpr uint32_t kEntrySizes[mac::kStringPools] = { 0x28, 0x38, 0x48, 0x68, 0x98, 0xd8, 0x118, 0x218, 0x418, 0x818, 0x1018 };
    static constexpr uint32_t kBucketLimit = 0x10000;

    GlobalStringTable* Table{ nullptr };
    std::mutex Lock;
    std::unordered_map<std::string_view, uint32_t> Ids;
    uint32_t Used[mac::kStringPools]{};

    void Make()
    {
        void* block = nullptr;
        if (posix_memalign(&block, mac::kStringLine, sizeof(GlobalStringTable)) != 0) abort();
        memset(block, 0, sizeof(GlobalStringTable));
        Table = static_cast<GlobalStringTable*>(block);
        for (uint32_t i = 0; i < mac::kStringPools; i++) {
            auto& pool = Table->SubTables[i];
            pool.TableIndex = (int32_t)i;
            pool.EntrySize = kEntrySizes[i];
            pool.EntriesPerBucket = (uint32_t)(mac::kStringBucketBytes / kEntrySizes[i]);
            pool.NumBuckets = 0;
            pool.Buckets = static_cast<uint8_t**>(calloc(kBucketLimit, sizeof(uint8_t*)));
        }
        Table->Main.StringTable = Table;
    }

    // The entry an index names, as his FixedStringBase::FindEntry finds it.
    FixedStringBase::Header* Entry(uint32_t id)
    {
        uint32_t poolIndex = id & mac::kStringIndexPoolMask;
        if (id == FixedStringId::NullIndex || poolIndex >= mac::kStringPools) return nullptr;
        auto& pool = Table->SubTables[poolIndex];
        uint32_t bucket = (id >> 4) & 0xffff, entry = id >> mac::kStringIndexEntryShift;
        if (bucket >= pool.NumBuckets || entry >= pool.EntriesPerBucket || bucket * pool.EntriesPerBucket + entry >= Used[poolIndex]) return nullptr;
        return reinterpret_cast<FixedStringBase::Header*>(pool.Buckets[bucket] + (size_t)entry * pool.EntrySize);
    }

    uint32_t Acquire(char const* text, uint32_t length)
    {
        std::lock_guard _(Lock);
        if (auto it = Ids.find(std::string_view(text, length)); it != Ids.end()) {
            Entry(it->second)->RefCount++;
            return it->second;
        }

        uint32_t poolIndex = 0;
        while (poolIndex < mac::kStringPools && kEntrySizes[poolIndex] < sizeof(FixedStringBase::Header) + length + 1) poolIndex++;
        if (poolIndex == mac::kStringPools) {
            Failed("a fixed string of " + std::to_string(length) + " bytes: longer than the game's pools hold");
            return FixedStringId::NullIndex;
        }
        auto& pool = Table->SubTables[poolIndex];
        uint32_t slot = Used[poolIndex]++;
        uint32_t bucket = slot / pool.EntriesPerBucket, entry = slot % pool.EntriesPerBucket;
        if (bucket >= kBucketLimit) abort();
        if (bucket >= pool.NumBuckets) {
            pool.Buckets[bucket] = static_cast<uint8_t*>(calloc(1, mac::kStringBucketBytes));
            pool.NumBuckets = bucket + 1;
        }
        pool.field_1100 = Used[poolIndex];

        uint32_t id = poolIndex | (bucket << 4) | (entry << mac::kStringIndexEntryShift);
        auto header = reinterpret_cast<FixedStringBase::Header*>(pool.Buckets[bucket] + (size_t)entry * pool.EntrySize);
        uint32_t hash = 2166136261u;
        for (uint32_t i = 0; i < length; i++) hash = (hash ^ (uint8_t)text[i]) * 16777619u;
        header->Hash = hash;
        header->RefCount = 1;
        header->Length = length;
        header->Id = id;
        auto stored = reinterpret_cast<char*>(header) + mac::kStringEntryText;
        memcpy(stored, text, length);
        stored[length] = 0;
        Ids.emplace(std::string_view(stored, length), id);
        return id;
    }
};

static_assert(sizeof(FixedStringBase::Header) == mac::kStringEntryText);

StringTableStandIn gStrings;

FixedString* StringCreate(GlobalStringTable::MainTable* self, uint32_t* id, LSStringView* text)
{
    if (self != &gStrings.Table->Main) Failed("a fixed string was made through a map that is not the table's");
    *id = gStrings.Acquire(text->data(), (uint32_t)text->size());
    return reinterpret_cast<FixedString*>(id);
}

LSStringView* StringGet(FixedStringBase const* self, LSStringView& text)
{
    auto header = gStrings.Entry(self->Index);
    if (!header) {
        Failed("the text of fixed string " + std::to_string(self->Index) + " was asked for: there is no such string");
        text = LSStringView();
        return &text;
    }
    text = LSStringView(reinterpret_cast<char const*>(header) + mac::kStringEntryText, header->Length);
    return &text;
}

void StringIncRef(uint32_t id)
{
    auto header = gStrings.Entry(id);
    if (!header) {
        Failed("a reference was added to fixed string " + std::to_string(id) + ": there is no such string");
        return;
    }
    std::lock_guard _(gStrings.Lock);
    header->RefCount++;
}

void StringDecRef(GlobalStringTable::MainTable* self, uint32_t* id)
{
    if (self != &gStrings.Table->Main) Failed("a fixed string was released through a map that is not the table's");
    auto header = gStrings.Entry(*id);
    if (!header) {
        Failed("fixed string " + std::to_string(*id) + " was released: there is no such string");
        return;
    }
    std::lock_guard _(gStrings.Lock);
    if (header->RefCount == 0) {
        Failed(std::string("fixed string \"") + (reinterpret_cast<char const*>(header) + mac::kStringEntryText)
            + "\" was released more often than it was acquired");
        return;
    }
    header->RefCount--;
}
