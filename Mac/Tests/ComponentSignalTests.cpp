// The component signals (Mac/Binding/ComponentSignals.h): Norbyte's handlers
// called as the macOS game calls a subscriber, his own sending of a signal as
// the game's subscribers take it, and the check of the game's code that both
// rest on.
//
//     bg3mse_component_signal_tests               the tests
//     bg3mse_component_signal_tests <Game.app>    the check, on every component type of a game binary

#include <Binding/ComponentSignalCheck.h>
#include <Platform/Log.h>
#include <Platform/Symbols.h>

#include "ComponentSignalsNorbyte.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace bg3se::mac;
using namespace bg3se::mac::binding;
using namespace bg3se::mac::test;

extern "C" {
// Mac/Tests/ComponentSignalTargets.S
void cst_send_attached(void const* ops, void const* begin, void const* end);
void cst_send_detached(void const* ops, void const* begin, void const* end);
void cst_send_attached_boxed(void const* ops, void const* begin, void const* end);
void cst_send_detached_boxed(void const* ops, void const* begin, void const* end);
void cst_send_attached_by_address(void const* ops, void const* begin, void const* end);
void cst_record();
uint64_t cst_seen[4];       // x0 to x3 as cst_record received them
uint64_t cst_calls;
}

namespace {

int gFailures = 0;
char const* gTest = "";
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; name(); } static void name()

bool Contains(std::string const& text, char const* part) { return text.find(part) != std::string::npos; }

using Sender = void (void const* ops, void const* begin, void const* end);
void const* Code(Sender* sender) { return reinterpret_cast<void const*>(sender); }

// The game's objects as the senders read them.

// ecs::ComponentSignalData: an entity and its component (or, for a component
// kept outside the entity's storage, where its address is).
struct Entry
{
    uint64_t Handle;
    void* Component;
};

// ls::PagedArrayIterator<ecs::ComponentSignalData const>
struct Iterator
{
    Entry const* const* Pages;
    uint32_t Index;
    uint16_t Shift;
};
static_assert(offsetof(Iterator, Index) == 8 && offsetof(Iterator, Shift) == 0xc);

// ecs::ComponentOps<T>
struct Ops
{
    void const* VTable;
    void* Registry;
    void* Callbacks;
    void* Storage;
    void* World;
    uint16_t Type;
};
static_assert(offsetof(Ops, Callbacks) == kComponentOpsCallbacks && offsetof(Ops, World) == kComponentOpsWorld);

void NotCalled() { abort(); }

// Virtual tables of ecs::ComponentOps<T>: the two destructors, then the senders.
void (*const kInlineTable[])() = { NotCalled, NotCalled, reinterpret_cast<void (*)()>(cst_send_attached),
    reinterpret_cast<void (*)()>(cst_send_detached) };
void (*const kBoxedTable[])() = { NotCalled, NotCalled, reinterpret_cast<void (*)()>(cst_send_attached_boxed),
    reinterpret_cast<void (*)()>(cst_send_detached_boxed) };
// Tables of a game that is not the one that was read.
void (*const kByAddressTable[])() = { NotCalled, NotCalled, reinterpret_cast<void (*)()>(cst_send_attached_by_address),
    reinterpret_cast<void (*)()>(cst_send_detached) };
void (*const kAttachedTwiceTable[])() = { NotCalled, NotCalled, reinterpret_cast<void (*)()>(cst_send_attached),
    reinterpret_cast<void (*)()>(cst_send_attached) };
// The senders one slot further on, as with one virtual function more before them.
void (*const kShiftedTable[])() = { NotCalled, NotCalled, NotCalled, reinterpret_cast<void (*)()>(cst_send_attached),
    reinterpret_cast<void (*)()>(cst_send_detached) };
void (*const kDataTable[])() = { NotCalled, NotCalled, reinterpret_cast<void (*)()>(cst_seen),
    reinterpret_cast<void (*)()>(cst_send_detached) };

void* const kWorld = reinterpret_cast<void*>(uintptr_t(0x761dd30000));

uint64_t LinkTimeOf(GameImage const& image, void const* loaded)
{
    return reinterpret_cast<uint64_t>(loaded) - reinterpret_cast<uint64_t>(image.LoadedAt(image.BaseVA())) + image.BaseVA();
}
constexpr uint64_t kHandle = 0x02000001000000adull;

// Sends `entries` through `sender`, the entries two to a page.
void Send(Sender* sender, void* callbacks, std::vector<Entry> const& entries)
{
    std::vector<Entry const*> pages;
    for (size_t i = 0; i < entries.size(); i += 2) pages.push_back(&entries[i]);
    Iterator begin{ pages.data(), 0, 1 };
    Iterator end{ pages.data(), (uint32_t)entries.size(), 1 };
    Ops ops{ kInlineTable, nullptr, callbacks, nullptr, kWorld, 0 };
    sender(&ops, &begin, &end);
}

TEST(the_stand_in_senders_are_the_games)
{
    CHECK(IsComponentSignalSender(kSendComponentAttached, Code(cst_send_attached), 58));
    CHECK(IsComponentSignalSender(kSendComponentDetached, Code(cst_send_detached), 58));
    CHECK(IsComponentSignalSender(kSendComponentAttached, Code(cst_send_attached_boxed), 59));
    CHECK(IsComponentSignalSender(kSendComponentDetached, Code(cst_send_detached_boxed), 59));

    // One is not the other, and a sender is the whole of it.
    CHECK(!IsComponentSignalSender(kSendComponentAttached, Code(cst_send_detached), 58));
    CHECK(!IsComponentSignalSender(kSendComponentDetached, Code(cst_send_attached), 58));
    CHECK(!IsComponentSignalSender(kSendComponentAttached, Code(cst_send_attached), 57));
    CHECK(!IsComponentSignalSender(kSendComponentAttached, Code(cst_send_attached_boxed), 58));
    CHECK(!IsComponentSignalSender(kSendComponentAttached, Code(cst_send_attached_by_address), 64));
}

TEST(a_component_types_operations_are_checked_through_their_virtual_table)
{
    auto image = GameImage::Current();
    CHECK(image != nullptr);
    if (!image) return;

    std::string problem;
    int callbacks = 0, otherCallbacks = 0;
    Ops kept{ kInlineTable, nullptr, &callbacks, nullptr, kWorld, 0 };
    CHECK(ChecksComponentSignals(*image, &kept, &callbacks, kWorld, problem) && problem.empty());
    Ops boxed{ kBoxedTable, nullptr, &callbacks, nullptr, kWorld, 0 };
    CHECK(ChecksComponentSignals(*image, &boxed, &callbacks, kWorld, problem) && problem.empty());
    // The table by itself, as a tool checks the tables of a game binary.
    CHECK(ChecksComponentSignalTable(*image, LinkTimeOf(*image, kInlineTable), problem) && problem.empty());

    CHECK(ComponentSignalsConfirmed(&kept, &callbacks, kWorld));
    CHECK(ComponentSignalsConfirmed(&boxed, &callbacks, kWorld));
    CHECK(ComponentSignalsConfirmed(&kept, &callbacks, kWorld));

    // The operations object must be the one that calls these callbacks' subscribers, in this world.
    CHECK(!ChecksComponentSignals(*image, &kept, &otherCallbacks, kWorld, problem));
    CHECK(Contains(problem, "callbacks are not the component type's"));
    CHECK(!ChecksComponentSignals(*image, &kept, &callbacks, &otherCallbacks, problem));
    CHECK(Contains(problem, "world is not the entity world"));
    CHECK(!ChecksComponentSignals(*image, &kept, nullptr, kWorld, problem));
    // Also once the table is known. (The refusals are logged; not here.)
    SetLogStream(nullptr);
    CHECK(!ComponentSignalsConfirmed(&kept, &otherCallbacks, kWorld));
    CHECK(!ComponentSignalsConfirmed(&kept, &callbacks, nullptr));
    SetLogStream(stdout);
    CHECK(ComponentSignalsConfirmed(&kept, &callbacks, kWorld));
}

TEST(what_is_not_the_sender_that_was_read_is_refused)
{
    auto image = GameImage::Current();
    if (!image) return;

    std::string problem;
    int callbacks = 0;
    Ops byAddress{ kByAddressTable, nullptr, &callbacks, nullptr, kWorld, 0 };
    CHECK(!ChecksComponentSignals(*image, &byAddress, &callbacks, kWorld, problem));
    CHECK(Contains(problem, "SendComponentAttachedSignal is not the code that was read"));

    // The senders in the slots they are called through, not just somewhere.
    Ops attachedTwice{ kAttachedTwiceTable, nullptr, &callbacks, nullptr, kWorld, 0 };
    CHECK(!ChecksComponentSignals(*image, &attachedTwice, &callbacks, kWorld, problem));
    CHECK(Contains(problem, "SendComponentDetachedSignal is not the code that was read"));
    Ops shifted{ kShiftedTable, nullptr, &callbacks, nullptr, kWorld, 0 };
    CHECK(!ChecksComponentSignals(*image, &shifted, &callbacks, kWorld, problem));
    CHECK(Contains(problem, "SendComponentAttachedSignal is not the code that was read"));

    // A function that is not code of the image, and a table that is not data of it.
    Ops data{ kDataTable, nullptr, &callbacks, nullptr, kWorld, 0 };
    CHECK(!ChecksComponentSignals(*image, &data, &callbacks, kWorld, problem));
    CHECK(Contains(problem, "SendComponentAttachedSignal is not in the game's code"));

    auto heapTable = static_cast<void const**>(malloc(sizeof(kInlineTable)));
    memcpy(heapTable, kInlineTable, sizeof(kInlineTable));
    Ops heap{ heapTable, nullptr, &callbacks, nullptr, kWorld, 0 };
    CHECK(!ChecksComponentSignals(*image, &heap, &callbacks, kWorld, problem));
    CHECK(Contains(problem, "virtual table is not in the game's image"));

    Ops none{ nullptr, nullptr, &callbacks, nullptr, kWorld, 0 };
    CHECK(!ChecksComponentSignals(*image, &none, &callbacks, kWorld, problem));
    CHECK(!ChecksComponentSignals(*image, nullptr, &callbacks, kWorld, problem));
    CHECK(Contains(problem, "no operations object"));

    // In the game: refused each time, and said once for an operations object.
    char text[4096]{};
    FILE* log = fmemopen(text, sizeof(text) - 1, "w");
    SetLogStream(log);
    CHECK(!ComponentSignalsConfirmed(&byAddress, &callbacks, kWorld));
    CHECK(!ComponentSignalsConfirmed(&byAddress, &callbacks, kWorld));
    SetLogStream(stdout);
    fclose(log);
    std::string said = text;
    auto first = said.find("component events are withheld");
    CHECK(first != std::string::npos && said.find("component events are withheld", first + 1) == std::string::npos);
    CHECK(Contains(said, "SendComponentAttachedSignal is not the code that was read"));
    SetLogStream(nullptr);
    CHECK(!ComponentSignalsConfirmed(nullptr, &callbacks, kWorld));
    SetLogStream(stdout);
    free(heapTable);
}

TEST(norbytes_handler_is_given_what_the_game_passes)
{
    auto callbacks = MakeComponentCallbacks();
    SignalSeen attached{}, detached{};
    SubscribeHandler(callbacks, true, 0x123, &attached);
    SubscribeHandler(callbacks, false, 0x124, &detached);

    int components[3];
    std::vector<Entry> entries{ { 0x0200000100000001ull, &components[0] }, { 0x0200000100000002ull, &components[1] }, { kHandle, &components[2] } };

    Send(cst_send_attached, callbacks, entries);
    CHECK(attached.Calls == 3 && detached.Calls == 0);
    CHECK(attached.Handle == kHandle && attached.World == kWorld && attached.Component == &components[2] && attached.Type == 0x123);
    // The entity at an address, as his handler takes it; the handle is not one.
    CHECK(attached.Entity != nullptr && reinterpret_cast<uint64_t>(attached.Entity) != kHandle);

    entries.pop_back();
    Send(cst_send_detached, callbacks, entries);
    CHECK(attached.Calls == 3 && detached.Calls == 2);
    CHECK(detached.Handle == 0x0200000100000002ull && detached.World == kWorld && detached.Component == &components[1] && detached.Type == 0x124);

    // A component kept outside the entity's storage: the entry says where its address is.
    void* box = &components[0];
    Send(cst_send_attached_boxed, callbacks, { { kHandle, &box } });
    CHECK(attached.Calls == 4 && attached.Handle == kHandle && attached.Component == &components[0]);
    Send(cst_send_detached_boxed, callbacks, { { kHandle, &box } });
    CHECK(detached.Calls == 3 && detached.Handle == kHandle && detached.Component == &components[0]);

    // Nothing to send, nobody called.
    Send(cst_send_attached, callbacks, {});
    CHECK(attached.Calls == 4);
}

TEST(every_subscriber_is_called_and_a_removed_one_is_not)
{
    auto callbacks = MakeComponentCallbacks();
    constexpr unsigned count = 9;       // more than his array's first allocation holds: connections are moved
    SignalSeen seen[count]{};
    uint64_t registrants[count];
    for (unsigned i = 0; i < count; i++) registrants[i] = SubscribeHandler(callbacks, true, (uint16_t)(0x200 + i), &seen[i]);
    CHECK(Subscribers(callbacks, true) == count && Subscribers(callbacks, false) == 0);

    int component;
    Send(cst_send_attached, callbacks, { { kHandle, &component } });
    for (unsigned i = 0; i < count; i++) {
        CHECK(seen[i].Calls == 1 && seen[i].Handle == kHandle && seen[i].World == kWorld && seen[i].Component == &component
            && seen[i].Type == 0x200 + i);
    }

    CHECK(Unsubscribe(callbacks, true, registrants[4]));
    CHECK(!Unsubscribe(callbacks, true, registrants[4]));
    CHECK(Subscribers(callbacks, true) == count - 1);
    Send(cst_send_attached, callbacks, { { kHandle, &component } });
    for (unsigned i = 0; i < count; i++) CHECK(seen[i].Calls == (i == 4 ? 1u : 2u));
}

TEST(a_signal_norbytes_code_sends_reaches_the_games_subscribers_by_value)
{
    auto callbacks = MakeComponentCallbacks();
    SignalSeen own{};
    SubscribeHandler(callbacks, false, 0x300, &own);
    ConnectGameSubscriber(callbacks, false, cst_record);

    int component;
    memset(cst_seen, 0xee, sizeof(cst_seen));
    cst_calls = 0;
    SendAsNorbyte(callbacks, false, kHandle, kWorld, &component);

    // The game's subscriber: its storage, which starts with what was called,
    // then the handle, the world and the component in a register each.
    CHECK(cst_calls == 1);
    void (*called)();
    memcpy(&called, reinterpret_cast<void const*>(cst_seen[0]), sizeof(called));
    CHECK(called == cst_record);
    CHECK(cst_seen[1] == kHandle && cst_seen[2] == reinterpret_cast<uint64_t>(kWorld) && cst_seen[3] == reinterpret_cast<uint64_t>(&component));
    // And his own, in the same signal.
    CHECK(own.Calls == 1 && own.Handle == kHandle && own.World == kWorld && own.Component == &component && own.Type == 0x300);

    // The same subscriber called by the game's sender sees the same registers.
    memset(cst_seen, 0xee, sizeof(cst_seen));
    Send(cst_send_detached, callbacks, { { kHandle, &component } });
    CHECK(cst_calls == 2 && own.Calls == 2);
    CHECK(cst_seen[1] == kHandle && cst_seen[2] == reinterpret_cast<uint64_t>(kWorld) && cst_seen[3] == reinterpret_cast<uint64_t>(&component));
}

// --- on a game binary ------------------------------------------------------------

int CheckGame(std::string const& app)
{
    std::string path = app + "/Contents/MacOS/Baldur's Gate 3";
    int fd = open(path.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) {
        fprintf(stderr, "cannot open %s\n", path.c_str());
        return 1;
    }
    void* data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        fprintf(stderr, "cannot map %s\n", path.c_str());
        return 1;
    }
    GameImage image;
    if (!image.OpenFile({ static_cast<uint8_t const*>(data), (size_t)st.st_size })) {
        fprintf(stderr, "%s: no readable arm64 symbol table\n", path.c_str());
        return 1;
    }

    // ecs::ComponentOps<T>::SendComponent{Attached,Detached}Signal(...), for every T.
    unsigned refused = 0;
    unsigned total[2]{};
    ComponentSignalSender const* senders[2] = { &kSendComponentAttached, &kSendComponentDetached };
    for (auto const& symbol : image.SymbolsNamed("__ZN3ecs12ComponentOpsI", "")) {
        for (unsigned which = 0; which < 2; which++) {
            std::string member = "E27" + std::string(senders[which]->Name) + "E";
            if (symbol.Name.find(member) == std::string_view::npos) continue;
            total[which]++;
            size_t available = (size_t)((image.TextEnd() - symbol.VA) / 4);
            bool inCode = symbol.VA >= image.TextBegin() && symbol.VA < image.TextEnd();
            if (!inCode || !IsComponentSignalSender(*senders[which], image.CodeAt(symbol.VA), available)) {
                if (refused++ < 10) printf("not the code that was read: %.*s\n", (int)symbol.Name.size(), symbol.Name.data());
            }
        }
    }

    // And every type's virtual table, as the check reads it in the game: the
    // two senders in the slots they are called through. A table's first slot
    // is two words after its symbol.
    unsigned tables = 0, tablesRefused = 0;
    for (auto const& symbol : image.SymbolsNamed("__ZTVN3ecs12ComponentOpsI", "")) {
        tables++;
        std::string problem;
        if (!ChecksComponentSignalTable(image, symbol.VA + 2 * sizeof(uint64_t), problem)) {
            if (tablesRefused++ < 10) printf("%.*s: %s\n", (int)symbol.Name.size(), symbol.Name.data(), problem.c_str());
        }
    }
    printf("%u virtual tables of component operations; %u do not name the two senders in slots %zu and %zu\n", tables, tablesRefused,
        kComponentOpsSendAttached, kComponentOpsSendDetached);
    refused += tablesRefused;
    if (tables != total[0]) refused++;

    printf("%u component types send the attached signal and %u the detached one; %u of these functions are not the code that was read\n",
        total[0], total[1], refused);
    bool ok = refused == 0 && total[0] > 0 && total[0] == total[1];
    printf("%s\n", ok ? "every component type calls its subscribers as the handlers are written for"
                       : "COMPONENT EVENTS WOULD BE WITHHELD FOR SOME OR ALL COMPONENT TYPES");
    return ok ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    SetLogStream(stdout);
    if (argc == 2) return CheckGame(argv[1]);

    the_stand_in_senders_are_the_games_run();
    a_component_types_operations_are_checked_through_their_virtual_table_run();
    what_is_not_the_sender_that_was_read_is_refused_run();
    norbytes_handler_is_given_what_the_game_passes_run();
    every_subscriber_is_called_and_a_removed_one_is_not_run();
    a_signal_norbytes_code_sends_reaches_the_games_subscribers_by_value_run();

    if (gFailures) {
        printf("%d check(s) failed\n", gFailures);
        return 1;
    }
    printf("component signal tests passed\n");
    return 0;
}
