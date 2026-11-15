// Norbyte's spell prototype animations (Stats/Prototype.h), compiled as his
// sources are, cleared as his SyncStat clears them (Stats/Stats.cpp) when
// the game has filled their set of names: the set's block is the one the
// game's NewInterface<AnimationSimpleNamesSet>::Allocate makes, its element
// size and count before the first element (Layout/GameStats.h), and it must
// be released at its start.

#include <stdafx.h>
#include <GameDefinitions/Stats/Prototype.h>

#include <Layout/GameStats.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int gFailures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            gFailures++; \
        } \
    } while (0)

std::vector<void*> gBlocks;        // what may be freed
std::vector<void*> gFreed;
std::vector<uint32_t> gReleased;   // fixed strings whose reference was dropped

}

// What his code takes from the extender, which the test does not link: the
// game's allocator, a fixed string's references and the function his
// assertions fail through.
BEGIN_SE()

void* GameAllocRaw(std::size_t size)
{
    auto block = malloc(size);
    gBlocks.push_back(block);
    return block;
}

void GameFree(void* ptr) noexcept
{
    gFreed.push_back(ptr);
    for (auto& block : gBlocks) {
        if (block == ptr) {
            block = nullptr;
            free(ptr);
            return;
        }
    }
}

void FixedStringBase::IncRef()
{
}

void FixedStringBase::DecRef()
{
    if (Index != NullIndex) {
        gReleased.push_back(Index);
    }
}

void AssertionFailed(char const* expr)
{
    fprintf(stderr, "assertion of Norbyte's code failed: %s\n", expr);
    abort();
}

END_SE()

BEGIN_NS(mac::test)

// The game's block for `count` names, as its Allocate leaves it: the element
// size, the count, then the elements, every fixed string given an index.
void* GameNames(uint32_t count, uint32_t firstIndex)
{
    auto block = static_cast<uint8_t*>(malloc(mac::kSetHeaderBytes + count * mac::kAnimationNamesSize));
    gBlocks.push_back(block);
    uint64_t header[2] = { mac::kAnimationNamesSize, count };
    memcpy(block, header, sizeof(header));
    for (uint32_t i = 0; i < count * 3; i++) {
        uint32_t index = firstIndex + i;
        memcpy(block + mac::kSetHeaderBytes + i * sizeof(uint32_t), &index, sizeof(index));
    }
    return block;
}

void TestClearFilledNames()
{
    stats::SpellPrototypeAnimationData animation;
    auto block = GameNames(2, 100);
    auto names = reinterpret_cast<uint8_t*>(&animation) + mac::kSpellAnimationsNames;
    void* elements = static_cast<uint8_t*>(block) + mac::kSetHeaderBytes;
    uint32_t two = 2;
    memcpy(names, &elements, sizeof(elements));
    memcpy(names + mac::kSetCapacity, &two, sizeof(two));
    memcpy(names + mac::kSetSize, &two, sizeof(two));
    uint32_t seven = 7;
    memcpy(&animation.Part0[0], &seven, sizeof(seven));

    animation = stats::SpellPrototypeAnimationData{};

    CHECK(gFreed.size() == 1);
    CHECK(!gFreed.empty() && gFreed[0] == block);
    CHECK(animation.Part2.Buf == nullptr && animation.Part2.Size == 0 && animation.Part2.Capacity == 0);
    // The six fixed strings of the two names, and the one before the set.
    CHECK(gReleased.size() == 7);
    for (uint32_t index = 100; index < 106; index++) {
        CHECK(::std::count(gReleased.begin(), gReleased.end(), index) == 1);
    }
    CHECK(::std::count(gReleased.begin(), gReleased.end(), 7u) == 1);
}

void TestClearEmptyNames()
{
    gFreed.clear();
    gReleased.clear();
    stats::SpellPrototypeAnimationData animation;
    animation = stats::SpellPrototypeAnimationData{};
    CHECK(gFreed.empty() && gReleased.empty());
}

END_NS()

int main()
{
    bg3se::mac::test::TestClearFilledNames();
    bg3se::mac::test::TestClearEmptyNames();
    if (gFailures) {
        printf("%d checks failed\n", gFailures);
        return 1;
    }
    printf("prototype tests passed\n");
    return 0;
}
