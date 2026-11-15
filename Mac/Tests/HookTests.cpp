// Tests of the hooks Norbyte's code puts directly on the game's and the story
// library's own functions: his detour, of his prototype, on a stand-in that
// records what reaches it, called with the registers and the stack block the
// engine table says the game passes. Each argument must reach his handler as
// the parameter of his the table names, and the function as the game passes
// it, whatever the caller left in the bytes the argument does not fill.
#include <Engine/EngineTable.h>
#include <Platform/Log.h>

#include "HookTestsNorbyte.h"

#include <cstdio>
#include <cstring>
#include <string_view>

using namespace bg3se::mac;
using engine::Passing;
using engine::Place;
using engine::Returns;

extern "C" {
uint64_t ht_seen[9];            // x0 to x8 as the stand-in received them
uint64_t ht_stack[12];          // the 0x60 bytes at its [sp]
uint64_t ht_calls;
uint64_t ht_return;             // what it returns in x0
void ht_record();
void ht_call(void const* target, uint64_t const* registers, uint64_t const* stack, uint64_t* results);
}

namespace {

int gFailures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf(" (%s, line %d)\n", #cond, __LINE__); gFailures++; } } while (0)

engine::Entry const* TableEntry(std::string_view name)
{
    for (auto const& area : engine::EngineTable()) {
        for (auto const& entry : area.Entries) {
            if (name == entry.Name) return &entry;
        }
    }
    return nullptr;
}

uint64_t Mask(unsigned bytes)
{
    return bytes >= 8 ? ~0ull : (1ull << (8 * bytes)) - 1;
}

// The value the test passes as argument `index`: bytes from 0x10 to 0x6f, so
// that a value narrower than its register reads the same whether it is
// extended with zeros or with its sign, and whose first byte differs from
// every other argument's.
uint64_t Sentinel(unsigned index, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned j = 0; j < bytes; j++) {
        value |= static_cast<uint64_t>(0x10 + (index * 7 + j * 3) % 0x60) << (8 * j);
    }
    return value;
}

constexpr uint64_t kRegisterJunk = 0xdead5a5a00000000ull;
constexpr uint8_t kStackJunk = 0xcd;

// One hooked function: called as the game calls it, through Norbyte's detour.
void CheckProbe(test::HookProbe const& probe)
{
    auto entry = TableEntry(probe.Entry);
    CHECK(entry != nullptr, "%s: no engine table entry", probe.Entry);
    if (!entry) return;
    auto const& call = entry->Call;
    CHECK(call.Count > 0 || probe.Parameters == 0, "%s: the entry states no arguments", probe.Entry);

    uint64_t registers[9];
    for (unsigned i = 0; i < 9; i++) registers[i] = kRegisterJunk | (0x1000u + i);
    uint8_t stack[sizeof(ht_stack)];
    memset(stack, kStackJunk, sizeof(stack));

    for (unsigned k = 0; k < call.Count; k++) {
        auto const& arg = call.Args[k];
        uint64_t value = Sentinel(k, arg.Bytes);
        if (arg.At == Place::Stack) {
            CHECK(arg.StackOffset + arg.Bytes <= sizeof(stack), "%s: argument %u lies beyond the stack block the test passes", probe.Entry, k);
            if (arg.StackOffset + arg.Bytes <= sizeof(stack)) memcpy(stack + arg.StackOffset, &value, arg.Bytes);
        } else {
            // A narrow value fills its register up to 32 bits, as Apple's
            // caller extends it; the bits above are the caller's.
            unsigned filled = arg.Bytes < 4 ? 4 : arg.Bytes;
            auto& r = registers[static_cast<unsigned>(arg.At)];
            r = (r & ~Mask(filled)) | value;
        }
    }

    bool producesResult = call.Result == Returns::W0 || call.Result == Returns::X0;
    ht_return = call.Result == Returns::W0 ? 0xbadc0de500000001ull : 0x0000600012345678ull;
    ht_calls = 0;
    memset(ht_seen, 0, sizeof(ht_seen));
    memset(ht_stack, 0, sizeof(ht_stack));

    CHECK(probe.Attach(reinterpret_cast<void*>(&ht_record)), "%s: Norbyte's detour does not go on the stand-in", probe.Entry);
    uint64_t results[1]{};
    ht_call(reinterpret_cast<void const*>(&ht_record), registers, reinterpret_cast<uint64_t const*>(stack), results);
    probe.Detach();

    CHECK(*probe.Calls == 1, "%s: the handler ran %u times", probe.Entry, *probe.Calls);
    CHECK(ht_calls == 1, "%s: the function ran %llu times", probe.Entry, (unsigned long long)ht_calls);

    for (unsigned k = 0; k < call.Count; k++) {
        auto const& arg = call.Args[k];
        uint64_t value = Sentinel(k, arg.Bytes);
        uint64_t mask = Mask(arg.Bytes);
        char where[24];
        if (arg.At == Place::Stack) snprintf(where, sizeof(where), "[sp+0x%x]", arg.StackOffset);
        else snprintf(where, sizeof(where), "x%u", static_cast<unsigned>(arg.At));

        if (arg.Norbyte >= 0) {
            CHECK(static_cast<unsigned>(arg.Norbyte) < probe.Parameters, "%s: %s is Norbyte's parameter %d, his prototype has %u",
                probe.Entry, where, arg.Norbyte, probe.Parameters);
            if (static_cast<unsigned>(arg.Norbyte) < probe.Parameters) {
                uint64_t got = probe.Seen[arg.Norbyte] & mask;
                CHECK(got == value, "%s: Norbyte's parameter %d (from %s, %s) reached his handler as 0x%llx, the game passed 0x%llx",
                    probe.Entry, arg.Norbyte, where, arg.Type ? arg.Type : "", (unsigned long long)got, (unsigned long long)value);
            }
        }
        if (arg.How == Passing::Unread) continue;
        uint64_t arrived = 0;
        if (arg.At == Place::Stack) {
            memcpy(&arrived, reinterpret_cast<uint8_t const*>(ht_stack) + arg.StackOffset, arg.Bytes);
        } else {
            arrived = ht_seen[static_cast<unsigned>(arg.At)];
        }
        CHECK((arrived & mask) == value, "%s: %s (%s) reached the function as 0x%llx, the game passed 0x%llx", probe.Entry, where,
            arg.Type ? arg.Type : "", (unsigned long long)(arrived & mask), (unsigned long long)value);
    }

    if (producesResult) {
        uint64_t mask = call.Result == Returns::W0 ? Mask(4) : Mask(8);
        CHECK((results[0] & mask) == (ht_return & mask), "%s: the caller got 0x%llx back, the function returned 0x%llx", probe.Entry,
            (unsigned long long)(results[0] & mask), (unsigned long long)(ht_return & mask));
    }
}

void hooks_hand_the_games_arguments_to_norbytes_handlers_and_on()
{
    unsigned checked = 0;
    for (auto const& probe : test::HookProbes()) {
        int before = gFailures;
        CheckProbe(probe);
        if (gFailures == before) checked++;
    }
    printf("%u of %zu hooked functions: every argument reaches Norbyte's handler and the function as the game passes it\n", checked,
        test::HookProbes().size());
}

}

int main()
{
    SetLogStream(stdout);

    hooks_hand_the_games_arguments_to_norbytes_handlers_and_on();

    if (gFailures) {
        printf("%d check(s) failed\n", gFailures);
        return 1;
    }
    printf("hook tests passed\n");
    return 0;
}
