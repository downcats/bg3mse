#include <stdafx.h>
#include <GameDefinitions/Osiris.h>

#include "StoryContainersNorbyte.h"

#include <list>
#include <map>
#include <set>
#include <strings.h>

// Every check here looks at one container through two types: the standard
// library's, as libOsiris.dylib is compiled, and Norbyte's, as the extender
// is. In the game no compiler sees both. This file is therefore compiled
// without type-based alias analysis (CMakeLists.txt): with it, the compiler
// may assume the two views are different memory.

BEGIN_NS(mac::test)

namespace {

std::string gFindings;

#define HOLDS(cond) do { if (!(cond)) { gFindings += #cond; gFindings += " (line " + std::to_string(__LINE__) + ")\n"; } } while (0)

// The library's own containers have the size Norbyte's views have: a list
// whole, a map with the size that each of his structures declares after it.
static_assert(sizeof(std::list<int>) == sizeof(List<int>));
static_assert(sizeof(std::map<uint32_t, void*>) == sizeof(TMap<uint32_t, void*>) + sizeof(uint64_t));

struct NoCaseLess
{
    bool operator ()(std::string const& a, std::string const& b) const
    {
        return strcasecmp(a.c_str(), b.c_str()) < 0;
    }
};

void ReadLists()
{
    // A list the library made, walked as Norbyte's code walks every list.
    std::list<int> made{ 3, 1, 4, 1, 5, 9, 2, 6 };
    auto view = reinterpret_cast<List<int> const*>(&made);
    std::vector<int> seen;
    auto head = view->Head;
    for (auto node = head->Next; node != head; node = node->Next) seen.push_back(node->Item);
    HOLDS(seen == (std::vector<int>{ 3, 1, 4, 1, 5, 9, 2, 6 }));
    HOLDS(view->Size == made.size());
    // The node before the end node is the last, as the library links them.
    HOLDS(head->Head->Item == 6 && head->Next->Head == head);

    std::list<int> empty;
    auto none = reinterpret_cast<List<int> const*>(&empty);
    HOLDS(none->Head->Next == none->Head && none->Size == 0);
}

void BuildLists()
{
    // Built as OsiFunction::OsiInsert builds the tuple it inserts: nodes from
    // a pool, the first of which a Windows list would use as its end node.
    int values[3]{ 10, 20, 30 };
    ListNode<int*> pool[4];
    List<int*> built;
    built.Init(&pool[0]);
    auto prev = built.Head;
    for (int i = 0; i < 3; i++) {
        built.Insert(&values[i], &pool[i + 1], prev);
        prev = &pool[i + 1];
    }
    // The library reads it as a list of its own: forwards, backwards, by size.
    auto const& asMade = *reinterpret_cast<std::list<int*> const*>(&built);
    HOLDS(asMade.size() == 3);
    std::vector<int> forwards, backwards;
    for (auto item : asMade) forwards.push_back(*item);
    for (auto it = asMade.rbegin(); it != asMade.rend(); ++it) backwards.push_back(**it);
    HOLDS(forwards == (std::vector<int>{ 10, 20, 30 }) && backwards == (std::vector<int>{ 30, 20, 10 }));
    HOLDS(asMade.front() == &values[0] && asMade.back() == &values[2]);
    // A list of nothing is still a list.
    List<int*> nothing;
    nothing.Init(&pool[0]);
    HOLDS(reinterpret_cast<std::list<int*> const*>(&nothing)->empty());

    // Built as the story debugger builds one: every node allocated.
    List<int> allocated;
    allocated.Init();
    auto last = allocated.Head;
    for (int i = 1; i <= 4; i++) {
        last = allocated.Insert(last);
        last->Item = i * i;
    }
    auto const& squares = *reinterpret_cast<std::list<int> const*>(&allocated);
    HOLDS(squares.size() == 4 && squares.front() == 1 && squares.back() == 16);
    HOLDS(allocated.Size == 4 && allocated.First->Item == 1 && allocated.Last->Item == 16);
    int sum = 0;
    for (auto value : squares) sum += value;
    HOLDS(sum == 30);
    for (auto node = allocated.Head->Next; node != allocated.Head;) {
        auto next = node->Next;
        delete node;
        node = next;
    }
}

void SearchMaps()
{
    // A map the library made, searched and walked through Norbyte's TMap.
    std::map<uint32_t, void*> made;
    std::set<uint32_t> keys;
    for (uint32_t n = 0; n < 200; n++) {
        uint32_t key = (n * 2654435761u) % 5000 + 10;
        made[key] = reinterpret_cast<void*>(static_cast<uintptr_t>(key) * 8);
        keys.insert(key);
    }
    auto view = reinterpret_cast<TMap<uint32_t, void*>*>(&made);
    for (auto key : keys) {
        auto found = view->Find(key);
        HOLDS(found != nullptr && *found == reinterpret_cast<void*>(static_cast<uintptr_t>(key) * 8));
    }
    // Below the least, above the greatest, and between two keys.
    HOLDS(view->Find(0) == nullptr && view->Find(6000) == nullptr);
    for (uint32_t key = 10; key < 5010; key++) {
        if (!keys.count(key)) HOLDS(view->Find(key) == nullptr);
    }
    std::set<uint32_t> visited;
    size_t visits = 0;
    view->Iterate([&](uint32_t const& key, void*& value) {
        visits++;
        visited.insert(key);
        HOLDS(value == reinterpret_cast<void*>(static_cast<uintptr_t>(key) * 8));
    });
    HOLDS(visits == keys.size() && visited == keys);

    std::map<uint32_t, void*> empty;
    auto none = reinterpret_cast<TMap<uint32_t, void*>*>(&empty);
    size_t noVisits = 0;
    none->Iterate([&](uint32_t const&, void*&) { noVisits++; });
    HOLDS(none->Find(1) == nullptr && noVisits == 0);

    // Names, as the function table keeps them: a string before the value,
    // compared without regard to case.
    std::map<std::string, int, NoCaseLess> named{ { "DB_Players/1", 1 }, { "PROC_Start/0", 2 }, { "QRY_IsTagged/2", 3 },
        { "a name that is longer than a string holds in place/3", 4 } };
    auto names = reinterpret_cast<TMap<std::string, int, NoCaseLess>*>(&named);
    auto hit = names->Find("db_players/1");
    HOLDS(hit != nullptr && *hit == 1);
    hit = names->Find("A NAME THAT IS LONGER THAN A STRING HOLDS IN PLACE/3");
    HOLDS(hit != nullptr && *hit == 4);
    HOLDS(names->Find("DB_Players/2") == nullptr);
    std::set<std::string> walked;
    names->Iterate([&](std::string const& key, int&) { walked.insert(key); });
    HOLDS(walked.size() == 4 && walked.count("QRY_IsTagged/2") == 1);
}

void SearchFunctionTable()
{
    // A function table with no function: every bucket an empty map.
    static uint64_t storage[(sizeof(FunctionDb) + 7) / 8];
    memset(storage, 0, sizeof(storage));
    auto table = reinterpret_cast<FunctionDb*>(storage);
    HOLDS(table->FindById(77) == nullptr);
    size_t visits = 0;
    table->Iterate([&](OsiString const&, OsiFunctionDef*&) { visits++; });
    HOLDS(visits == 0);

    // One map of the library's in the bucket of its ids: id modulo 1023.
    uint32_t const first = 5 + 1023 * 3, second = 5 + 1023 * 40, elsewhere = 6 + 1023 * 3;
    std::map<uint32_t, OsiFunctionDef*> bucket;
    bucket[first] = reinterpret_cast<OsiFunctionDef*>(storage + 1);
    bucket[second] = reinterpret_cast<OsiFunctionDef*>(storage + 2);
    memcpy(&table->FunctionIdHash[first % 0x3FF], &bucket, sizeof(bucket));
    auto found = table->FindById(first);
    HOLDS(found != nullptr && *found == reinterpret_cast<OsiFunctionDef*>(storage + 1));
    found = table->FindById(second);
    HOLDS(found != nullptr && *found == reinterpret_cast<OsiFunctionDef*>(storage + 2));
    HOLDS(table->FindById(elsewhere) == nullptr && table->FindById(5) == nullptr);
    memset(storage, 0, sizeof(storage));
}

// The bucket of a name, as LookupOsiFunction chooses it: Norbyte's
// FunctionNameHash over the name, plus the number of parameters.
uint32_t BucketOf(char const* name, uint32_t arity)
{
    uint32_t hash = 0;
    for (; *name; name++) hash = (uint32_t)(*name | 0x20) + 129 * (hash % 4294967);
    return (hash + arity) % 0x3FF;
}

void SearchFunctionTableByName()
{
    // What a server Lua state does with the function table: one name and
    // arity looked up (a listener registered, LookupOsiFunction), and every
    // function visited (the cache of Osi.*, RebuildCacheIfNecessary). The
    // table is the library's: 1023 buckets, each a map of its own from
    // "name/arity" to the function, compared without regard to case.
    static uint64_t storage[(sizeof(FunctionDb) + 7) / 8];
    memset(storage, 0, sizeof(storage));
    auto table = reinterpret_cast<FunctionDb*>(storage);

    struct Entry { char const* Name; uint32_t Arity; };
    static constexpr Entry entries[] = { { "LeveledUp", 1 }, { "DB_Players", 1 }, { "DB_PartyFollowers", 1 }, { "CharacterJoinedParty", 1 },
        { "LevelGameplayStarted", 2 }, { "QRY_IsTagged", 2 }, { "QRY_IsTagged", 3 } };
    using Bucket = std::map<std::string, OsiFunctionDef*, NoCaseLess>;
    // The size the library keeps after the tree is the slot's last word.
    static_assert(sizeof(Bucket) == sizeof(FunctionDb::HashSlot));
    std::map<uint32_t, Bucket> buckets;
    for (size_t i = 0; i < std::size(entries); i++) {
        auto key = std::string(entries[i].Name) + "/" + std::to_string(entries[i].Arity);
        buckets[BucketOf(entries[i].Name, entries[i].Arity)][key] = reinterpret_cast<OsiFunctionDef*>(storage + 1 + i);
    }
    for (auto& bucket : buckets) memcpy(&table->Hash[bucket.first], &bucket.second, sizeof(Bucket));

    auto lookup = [&](char const* name, uint32_t arity) -> OsiFunctionDef* {
        // The key is a string of the game's allocator in the extender; its
        // bytes are the standard string's.
        static_assert(sizeof(OsiString) == sizeof(std::string));
        auto sig = std::string(name) + "/" + std::to_string(arity);
        uint32_t hash = 0;
        for (auto c = name; *c; c++) hash = (uint32_t)(*c | 0x20) + 129 * (hash % 4294967);
        auto found = table->Find(hash + arity, reinterpret_cast<OsiString const&>(sig));
        return found ? *found : nullptr;
    };
    for (size_t i = 0; i < std::size(entries); i++) {
        HOLDS(lookup(entries[i].Name, entries[i].Arity) == reinterpret_cast<OsiFunctionDef*>(storage + 1 + i));
    }
    // The name in another case, as the story compares names.
    HOLDS(lookup("db_players", 1) == reinterpret_cast<OsiFunctionDef*>(storage + 2));
    // A name the story has with another number of parameters, and one it
    // does not have: not found, in a bucket with functions and in an empty one.
    HOLDS(lookup("LeveledUp", 2) == nullptr && lookup("DB_PartyFollowers", 2) == nullptr && lookup("QRY_IsTagged", 1) == nullptr);
    HOLDS(lookup("NoSuchFunction", 0) == nullptr);

    std::set<std::string> visited;
    table->Iterate([&](OsiString const& key, OsiFunctionDef*& function) {
        if (function >= reinterpret_cast<OsiFunctionDef*>(storage + 1) && function <= reinterpret_cast<OsiFunctionDef*>(storage + std::size(entries))) {
            visited.insert(std::string(key.c_str()));
        }
    });
    HOLDS(visited.size() == std::size(entries) && visited.count("QRY_IsTagged/3") == 1 && visited.count("LeveledUp/1") == 1);
    memset(storage, 0, sizeof(storage));
}

}

std::string CheckStoryContainers()
{
    gFindings.clear();
    ReadLists();
    BuildLists();
    SearchMaps();
    SearchFunctionTable();
    SearchFunctionTableByName();
    return gFindings;
}

END_NS()
