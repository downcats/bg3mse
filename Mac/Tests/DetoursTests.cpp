// Tests of the Detours API (Mac/Shim/detours.h). No game needed: the
// functions hooked are this executable's own and a test dylib's.

#include <detours.h>

#include <Platform/CodeWriter.h>
#include <Platform/EntryHook.h>
#include <Platform/FunctionExtents.h>
#include <Platform/HookPlanner.h>
#include <Platform/Islands.h>
#include <Platform/Log.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace bg3se::mac;

// DetoursTargets.S
extern "C" {
int dt_add_ten(int);
int dt_twenty();
int dt_twenty_one();
int dt_thirty();
int dt_thirty_one();
int dt_thirty_two();
int dt_calls_first();
int dt_stress();
int dt_forty();
int dt_forty_one();
}

namespace {

int gFailures = 0;
char const* gTest = "";

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; name(); } static void name()

using IntFn = int (*)();

template <class F>
F Load(PVOID const& pointer)
{
    return reinterpret_cast<F>(__atomic_load_n(&pointer, __ATOMIC_ACQUIRE));
}

// The log, kept in a file so that tests can look for a reason in it.
FILE* gLog = nullptr;

std::string LogText()
{
    fflush(gLog);
    std::string text;
    rewind(gLog);
    char buffer[4096];
    while (size_t n = fread(buffer, 1, sizeof(buffer), gLog)) text.append(buffer, n);
    return text;
}

void ClearLog()
{
    fflush(gLog);
    ftruncate(fileno(gLog), 0);
    rewind(gLog);
}

// --- attach, call through the trampoline, commit, detach ---------------------------------------

PVOID gAddTen = (PVOID)&dt_add_ten;
int AddTenDoubled(int x) { return Load<int (*)(int)>(gAddTen)(x) * 2; }
int AddTenPlus100(int x) { return Load<int (*)(int)>(gAddTen)(x) + 100; }

TEST(attach_commit_detach_and_attach_again)
{
    int (*volatile fn)(int) = &dt_add_ten;
    CHECK(fn(1) == 11);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourUpdateThread(nullptr) == NO_ERROR);
    PDETOUR_TRAMPOLINE trampoline = nullptr;
    PVOID realTarget = nullptr, realDetour = nullptr;
    CHECK(DetourAttachEx(&gAddTen, (PVOID)&AddTenDoubled, &trampoline, &realTarget, &realDetour) == NO_ERROR);
    auto original = reinterpret_cast<int (*)(int)>(trampoline);
    // Queued: the trampoline runs the original already; nothing else changed.
    CHECK(trampoline != nullptr && realTarget == (PVOID)&dt_add_ten && realDetour == (PVOID)&AddTenDoubled);
    CHECK(gAddTen == (PVOID)&dt_add_ten && fn(1) == 11 && original(1) == 11);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(gAddTen == (PVOID)trampoline);
    CHECK(fn(1) == 22 && original(1) == 11);

    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(&gAddTen, (PVOID)&AddTenDoubled) == NO_ERROR);
    CHECK(fn(1) == 22);             // until commit
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(gAddTen == (PVOID)&dt_add_ten && fn(1) == 11 && original(1) == 11);

    // Attached again with another detour: the same trampoline, the new detour.
    PDETOUR_TRAMPOLINE again = nullptr;
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttachEx(&gAddTen, (PVOID)&AddTenPlus100, &again, nullptr, nullptr) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(again == trampoline && gAddTen == (PVOID)trampoline && fn(1) == 111);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(&gAddTen, (PVOID)&AddTenPlus100) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(gAddTen == (PVOID)&dt_add_ten && fn(1) == 11);
}

// --- abort ----------------------------------------------------------------------------------

PVOID gThirty = (PVOID)&dt_thirty;
int ThirtyPlusOne() { return Load<IntFn>(gThirty)() + 1; }
int ThirtyPlusTwo() { return Load<IntFn>(gThirty)() + 2; }

TEST(abort_leaves_the_code_and_the_pointer_as_they_were)
{
    IntFn volatile fn = &dt_thirty;
    uint32_t before;
    memcpy(&before, (void*)&dt_thirty, sizeof(before));
    CHECK(DetourTransactionBegin() == NO_ERROR);
    PDETOUR_TRAMPOLINE trampoline = nullptr;
    CHECK(DetourAttachEx(&gThirty, (PVOID)&ThirtyPlusOne, &trampoline, nullptr, nullptr) == NO_ERROR);
    CHECK(DetourTransactionAbort() == NO_ERROR);
    uint32_t after;
    memcpy(&after, (void*)&dt_thirty, sizeof(after));
    CHECK(before == after && gThirty == (PVOID)&dt_thirty && fn() == 30);
    CHECK(reinterpret_cast<IntFn>(trampoline)() == 30);     // still callable
    // The transaction is closed.
    CHECK(DetourTransactionAbort() == ERROR_INVALID_OPERATION);
    CHECK(DetourTransactionCommit() == ERROR_INVALID_OPERATION);
    CHECK(DetourAttach(&gThirty, (PVOID)&ThirtyPlusOne) == ERROR_INVALID_OPERATION);

    // Attached afterwards with another detour: that detour runs, not the aborted one.
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&gThirty, (PVOID)&ThirtyPlusTwo) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(fn() == 32 && gThirty == (PVOID)trampoline);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(&gThirty, (PVOID)&ThirtyPlusTwo) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(fn() == 30 && gThirty == (PVOID)&dt_thirty);
}

// --- two functions on one page ----------------------------------------------------------------

PVOID gTwenty = (PVOID)&dt_twenty;
PVOID gTwentyOne = (PVOID)&dt_twenty_one;
int Twenty() { return Load<IntFn>(gTwenty)() + 100; }
int TwentyOne() { return Load<IntFn>(gTwentyOne)() + 200; }

TEST(two_attaches_on_one_page_in_one_transaction)
{
    IntFn volatile a = &dt_twenty;
    IntFn volatile b = &dt_twenty_one;
    uintptr_t page = CodePageSize();
    CHECK((uintptr_t)&dt_twenty / page == (uintptr_t)&dt_twenty_one / page);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&gTwenty, (PVOID)&Twenty) == NO_ERROR);
    CHECK(DetourAttach(&gTwentyOne, (PVOID)&TwentyOne) == NO_ERROR);
    CHECK(a() == 20 && b() == 21);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(a() == 120 && b() == 221);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(&gTwenty, (PVOID)&Twenty) == NO_ERROR);
    CHECK(DetourDetach(&gTwentyOne, (PVOID)&TwentyOne) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(a() == 20 && b() == 21 && gTwenty == (PVOID)&dt_twenty && gTwentyOne == (PVOID)&dt_twenty_one);
}

// --- a function of another image ----------------------------------------------------------------

PVOID gLibAddOne = nullptr;
long LibAddOne(long x) { return Load<long (*)(long)>(gLibAddOne)(x) * 1000; }
PVOID gLibAddLocal = nullptr;
long LibAddLocal(long x) { return -Load<long (*)(long)>(gLibAddLocal)(x); }

TEST(attach_on_functions_of_a_loaded_dylib)
{
    // Never closed: islands may lie in their header padding.
    void* withStarts = dlopen(BG3MSE_TEST_FUNCTIONS, RTLD_NOW | RTLD_LOCAL);
    void* symbolsOnly = dlopen(BG3MSE_TEST_FUNCTIONS_SYMBOLS, RTLD_NOW | RTLD_LOCAL);
    CHECK(withStarts && symbolsOnly);
    if (!withStarts || !symbolsOnly) return;
    auto addOne = reinterpret_cast<long (*)(long)>(dlsym(withStarts, "lib_add_one"));
    auto addLocal = reinterpret_cast<long (*)(long)>(dlsym(symbolsOnly, "lib_add_local"));
    CHECK(addOne && addLocal && addOne(1) == 2 && addLocal(1) == 8);
    gLibAddOne = (PVOID)addOne;
    gLibAddLocal = (PVOID)addLocal;
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&gLibAddOne, (PVOID)&LibAddOne) == NO_ERROR);         // its extent from LC_FUNCTION_STARTS
    CHECK(DetourAttach(&gLibAddLocal, (PVOID)&LibAddLocal) == NO_ERROR);     // from the symbol table
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(addOne(1) == 2000 && addLocal(1) == -8);

    // The dylib with header padding holds its own island; the other's comes from a page mapped near it.
    EntryHook* hook = FindEntryHook((void*)addOne);
    CHECK(hook && ImageHeaderOf(hook->Island()) == ImageHeaderOf((void*)addOne));
    EntryHook* other = FindEntryHook((void*)addLocal);
    CHECK(other && InBranchRange((uintptr_t)addLocal, (uintptr_t)other->Island(), 32));

    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(&gLibAddOne, (PVOID)&LibAddOne) == NO_ERROR);
    CHECK(DetourDetach(&gLibAddLocal, (PVOID)&LibAddLocal) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(addOne(1) == 2 && addLocal(1) == 8);
}

// --- refusals --------------------------------------------------------------------------------

int Nothing() { return -1; }

TEST(refusals_change_nothing_and_fail_the_transaction)
{
    IntFn volatile thirtyOne = &dt_thirty_one;
    PVOID good = (PVOID)&dt_thirty_one;
    PVOID inside = (PVOID)((uintptr_t)&dt_thirty_one + 4);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&good, (PVOID)&Nothing) == NO_ERROR);
    ClearLog();
    CHECK(DetourAttach(&inside, (PVOID)&Nothing) == ERROR_INVALID_BLOCK);
    CHECK(LogText().find("not at its start") != std::string::npos);
    // The transaction has failed: what follows returns that error, and commit makes nothing.
    PVOID other = (PVOID)&dt_thirty_two;
    CHECK(DetourAttach(&other, (PVOID)&Nothing) == ERROR_INVALID_BLOCK);
    CHECK(DetourUpdateThread(nullptr) == ERROR_INVALID_BLOCK);
    PVOID* failed = nullptr;
    CHECK(DetourTransactionCommitEx(&failed) == ERROR_INVALID_BLOCK);
    CHECK(failed == &inside);
    CHECK(good == (PVOID)&dt_thirty_one && thirtyOne() == 31);

    struct Case
    {
        char const* Name;
        PVOID Function;
        LONG Expected;
        char const* Reason;
    };
    void* page = mmap(nullptr, CodePageSize(), PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    Case const cases[] = {
        { "a call first", (PVOID)&dt_calls_first, ERROR_INVALID_BLOCK, Describe(HookRefusal::Call) },
        { "no function", nullptr, ERROR_INVALID_HANDLE, "no function" },
        { "outside any image", page, ERROR_INVALID_BLOCK, Describe(ExtentRefusal::NotInImage) },
        { "data", (PVOID)&gFailures, ERROR_INVALID_BLOCK, Describe(ExtentRefusal::NotCode) },
    };
    for (auto const& c : cases) {
        PVOID pointer = c.Function;
        ClearLog();
        CHECK(DetourTransactionBegin() == NO_ERROR);
        LONG result = DetourAttach(&pointer, (PVOID)&Nothing);
        bool logged = LogText().find(c.Reason) != std::string::npos;
        if (result != c.Expected || !logged) printf("  case %s: %ld\n%s", c.Name, (long)result, LogText().c_str());
        CHECK(result == c.Expected && logged);
        CHECK(DetourTransactionCommit() == c.Expected && pointer == c.Function);
    }
    munmap(page, CodePageSize());
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(nullptr, (PVOID)&Nothing) == ERROR_INVALID_HANDLE);
    CHECK(DetourTransactionAbort() == NO_ERROR);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&good, nullptr) == ERROR_INVALID_HANDLE);
    CHECK(DetourTransactionAbort() == NO_ERROR);
}

PVOID gTwentyAgain = (PVOID)&dt_twenty;
int TwentyOther() { return 0; }

TEST(one_hook_per_function)
{
    IntFn volatile fn = &dt_twenty;
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&gTwenty, (PVOID)&Twenty) == NO_ERROR);
    // Twice in one transaction.
    CHECK(DetourAttach(&gTwentyAgain, (PVOID)&TwentyOther) == ERROR_INVALID_BLOCK);
    CHECK(DetourTransactionCommit() == ERROR_INVALID_BLOCK);
    CHECK(fn() == 20 && gTwenty == (PVOID)&dt_twenty);

    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&gTwenty, (PVOID)&Twenty) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(fn() == 120);
    // A function that carries a committed hook.
    ClearLog();
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&gTwentyAgain, (PVOID)&TwentyOther) == ERROR_INVALID_BLOCK);
    CHECK(LogText().find(Describe(HookRefusal::AlreadyHooked)) != std::string::npos);
    CHECK(DetourTransactionAbort() == NO_ERROR);
    // Detach, and attach in the same transaction.
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(&gTwenty, (PVOID)&Twenty) == NO_ERROR);
    PVOID target = (PVOID)&dt_twenty;
    CHECK(DetourAttach(&target, (PVOID)&TwentyOther) == ERROR_INVALID_BLOCK);
    CHECK(DetourTransactionAbort() == NO_ERROR);
    // Detach with another detour than the one attached, or of something that is not a trampoline.
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(&gTwenty, (PVOID)&TwentyOther) == ERROR_INVALID_BLOCK);
    CHECK(DetourTransactionAbort() == NO_ERROR);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(&target, (PVOID)&Twenty) == ERROR_INVALID_BLOCK);
    CHECK(DetourTransactionAbort() == NO_ERROR);
    CHECK(fn() == 120);

    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(&gTwenty, (PVOID)&Twenty) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(fn() == 20 && gTwenty == (PVOID)&dt_twenty);
}

TEST(one_transaction_at_a_time_owned_by_one_thread)
{
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourTransactionBegin() == ERROR_INVALID_OPERATION);
    LONG begin = NO_ERROR, attach = NO_ERROR, commit = NO_ERROR, abort = NO_ERROR;
    std::thread other([&] {
        PVOID p = (PVOID)&dt_thirty;
        begin = DetourTransactionBegin();
        attach = DetourAttach(&p, (PVOID)&Nothing);
        commit = DetourTransactionCommit();
        abort = DetourTransactionAbort();
    });
    other.join();
    CHECK(begin == ERROR_INVALID_OPERATION && attach == ERROR_INVALID_OPERATION);
    CHECK(commit == ERROR_INVALID_OPERATION && abort == ERROR_INVALID_OPERATION);
    CHECK(DetourTransactionCommit() == NO_ERROR);       // still ours, and empty
    // Closed: another thread may open the next one.
    std::thread next([&] {
        begin = DetourTransactionBegin();
        abort = DetourTransactionAbort();
    });
    next.join();
    CHECK(begin == NO_ERROR && abort == NO_ERROR);
}

// --- commits that find the code changed ---------------------------------------------------------

PVOID gThirtyTwo = (PVOID)&dt_thirty_two;
int ThirtyTwo() { return Load<IntFn>(gThirtyTwo)() + 1; }
int ThirtyTwoElsewhere() { return -32; }

TEST(commit_refuses_a_function_changed_under_the_transaction)
{
    IntFn volatile fn = &dt_thirty_two;
    uint32_t first;
    memcpy(&first, (void*)&dt_thirty_two, sizeof(first));

    // Its first instruction changed between attach and commit.
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&gThirtyTwo, (PVOID)&ThirtyTwo) == NO_ERROR);
    CHECK(WriteCodeWord((void*)&dt_thirty_two, 0x52800420).empty());        // mov w0, #33
    PVOID* failed = nullptr;
    CHECK(DetourTransactionCommitEx(&failed) == ERROR_INVALID_DATA);
    CHECK(failed == &gThirtyTwo && gThirtyTwo == (PVOID)&dt_thirty_two && fn() == 33);
    CHECK(WriteCodeWord((void*)&dt_thirty_two, first).empty());
    CHECK(fn() == 32);

    // Its hook re-targeted by other code of this process between attach and commit.
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&gThirtyTwo, (PVOID)&ThirtyTwo) == NO_ERROR);
    void* elsewhere = nullptr;
    std::string problem;
    CHECK(PrepareEntryHook((void*)&dt_thirty_two, 8, (void const*)&ThirtyTwoElsewhere, &elsewhere, problem) != nullptr);
    CHECK(DetourTransactionCommit() == ERROR_INVALID_DATA);
    CHECK(gThirtyTwo == (PVOID)&dt_thirty_two && fn() == 32);

    // And then attached properly.
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&gThirtyTwo, (PVOID)&ThirtyTwo) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(fn() == 33);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(&gThirtyTwo, (PVOID)&ThirtyTwo) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(fn() == 32);
}

PVOID gForty = (PVOID)&dt_forty;
PVOID gFortyOne = (PVOID)&dt_forty_one;
int Forty() { return Load<IntFn>(gForty)() + 100; }
int FortyOne() { return Load<IntFn>(gFortyOne)() + 100; }

TEST(a_commit_whose_second_page_cannot_be_written_takes_the_first_out_again)
{
    IntFn volatile a = &dt_forty;
    IntFn volatile b = &dt_forty_one;
    uintptr_t pageSize = CodePageSize();
    auto forty = reinterpret_cast<uintptr_t>(&dt_forty);
    auto fortyOne = reinterpret_cast<uintptr_t>(&dt_forty_one);
    uintptr_t pageB = fortyOne & ~(pageSize - 1);
    CHECK(forty < pageB);
    CHECK(fortyOne == pageB);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&gForty, (PVOID)&Forty) == NO_ERROR);
    CHECK(DetourAttach(&gFortyOne, (PVOID)&FortyOne) == NO_ERROR);
    // The hooks go in page by page, lowest first: the first page's goes in, the
    // second page, no longer executable, is refused, and the first is taken out again.
    CHECK(mprotect((void*)pageB, pageSize, PROT_READ) == 0);
    ClearLog();
    PVOID* failed = nullptr;
    LONG result = DetourTransactionCommitEx(&failed);
    CHECK(mprotect((void*)pageB, pageSize, PROT_READ | PROT_EXEC) == 0);
    std::string log = LogText();
    CHECK(result == ERROR_NOT_ENOUGH_MEMORY && failed == &gFortyOne);
    CHECK(a() == 40 && b() == 41 && gForty == (PVOID)&dt_forty && gFortyOne == (PVOID)&dt_forty_one);
    char line[128];
    for (void const* f : { (void const*)&dt_forty, (void const*)&dt_forty_one }) {
        snprintf(line, sizeof(line), "attach %p: as it was", f);
        CHECK(log.find(line) != std::string::npos);
    }
    if (result != ERROR_NOT_ENOUGH_MEMORY) printf("%s", log.c_str());
    EntryHook* first = FindEntryHook((void*)&dt_forty);
    CHECK(first && !first->Committed());

    // Both go in once the page is executable again.
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&gForty, (PVOID)&Forty) == NO_ERROR);
    CHECK(DetourAttach(&gFortyOne, (PVOID)&FortyOne) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(a() == 140 && b() == 141);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(&gForty, (PVOID)&Forty) == NO_ERROR);
    CHECK(DetourDetach(&gFortyOne, (PVOID)&FortyOne) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(a() == 40 && b() == 41);
}

// --- attaching while other threads call the function ---------------------------------------------

PVOID gStress = (PVOID)&dt_stress;
int StressDetour() { return Load<IntFn>(gStress)() + 10000; }

std::atomic<bool> gStressStop{ false };
std::atomic<uint64_t> gStressCalls{ 0 }, gStressBad{ 0 }, gStressReplaced{ 0 };

// On the page of dt_stress, so the callers execute the page that is swapped:
// it starts a page, and DetoursTargets.S's functions follow it in the section.
extern "C" __attribute__((noinline, aligned(16384), section("__TEXT,__detours,regular,pure_instructions"))) void DetoursStressCaller()
{
    uint64_t calls = 0, bad = 0, replaced = 0;
    while (!gStressStop.load(std::memory_order_relaxed)) {
        int value = dt_stress();
        if (value == 11234) replaced++;
        else if (value != 1234) bad++;
        calls++;
    }
    gStressCalls += calls;
    gStressBad += bad;
    gStressReplaced += replaced;
}

TEST(attach_and_detach_while_threads_call_the_function)
{
    uintptr_t page = CodePageSize();
    CHECK((uintptr_t)&DetoursStressCaller / page == (uintptr_t)&dt_stress / page);
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; i++) threads.emplace_back(DetoursStressCaller);

    // A second of rounds, at most 10000.
    int rounds = 0, failures = 0;
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (rounds < 10000 && std::chrono::steady_clock::now() < until) {
        if (DetourTransactionBegin() != NO_ERROR) failures++;
        if (DetourAttach(&gStress, (PVOID)&StressDetour) != NO_ERROR) failures++;
        if (DetourTransactionCommit() != NO_ERROR) failures++;
        if (DetourTransactionBegin() != NO_ERROR) failures++;
        if (DetourDetach(&gStress, (PVOID)&StressDetour) != NO_ERROR) failures++;
        if (DetourTransactionCommit() != NO_ERROR) failures++;
        rounds++;
    }
    gStressStop = true;
    for (auto& t : threads) t.join();
    printf("  %d rounds, %llu calls, %llu replaced\n", rounds, (unsigned long long)gStressCalls.load(), (unsigned long long)gStressReplaced.load());
    CHECK(failures == 0);
    CHECK(rounds >= 500);
    CHECK(gStressBad == 0);
    CHECK(gStressCalls > 0 && gStressReplaced > 0 && gStressReplaced < gStressCalls);
    CHECK(dt_stress() == 1234 && gStress == (PVOID)&dt_stress);
}

}

int main()
{
    gLog = tmpfile();
    SetLogStream(gLog);
    void (*tests[])() = {
        attach_commit_detach_and_attach_again_run,
        abort_leaves_the_code_and_the_pointer_as_they_were_run,
        two_attaches_on_one_page_in_one_transaction_run,
        attach_on_functions_of_a_loaded_dylib_run,
        refusals_change_nothing_and_fail_the_transaction_run,
        one_hook_per_function_run,
        one_transaction_at_a_time_owned_by_one_thread_run,
        commit_refuses_a_function_changed_under_the_transaction_run,
        a_commit_whose_second_page_cannot_be_written_takes_the_first_out_again_run,
        attach_and_detach_while_threads_call_the_function_run,
    };
    for (auto test : tests) test();
    printf("%zu tests, %d failures\n", std::size(tests), gFailures);
    return gFailures ? 1 : 0;
}
