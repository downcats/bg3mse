// Tests of the platform layer's code writing. No game needed: the code that is
// written is this executable's own and pages mapped here.

#include <Platform/CodeWriter.h>
#include <Platform/Config.h>
#include <Platform/EntryHook.h>
#include <Platform/FunctionExtents.h>
#include <Platform/HookPlanner.h>
#include <Platform/Islands.h>
#include <Platform/Log.h>
#include <Platform/Process.h>
#include <Platform/Symbols.h>

#include <atomic>
#include <clocale>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/fat.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "FakeBinary.h"

using namespace bg3se::mac;

// A function of this executable's own __text: mov w0, #1; ret.
extern "C" __attribute__((naked, noinline)) int WriterVictim()
{
    __asm__("mov w0, #1\n ret\n");
}

// HookTargets.S
extern "C" {
int hook_adrp_first();
int hook_cbz_first(long);
uint64_t hook_literal_first();
long hook_loop_head(long);
int hook_stress_target();
extern char const hook_adrp_first_end[], hook_cbz_first_end[], hook_literal_first_end[], hook_loop_head_end[], hook_stress_target_end[];
extern int hook_data;
}

namespace {

int gFailures = 0;
char const* gTest = "";

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; name(); } static void name()

constexpr uint32_t kRet = 0xd65f03c0;

constexpr uint32_t MovW0(uint32_t value) { return 0x52800000 | (value << 5); }

using IntFn = int (*)();

// Pages mapped for a test, r-x unless asked otherwise, filled with `ret`.
struct CodePages
{
    uint8_t* Base{ nullptr };
    size_t Bytes{ 0 };

    explicit CodePages(size_t pages, vm_prot_t prot = VM_PROT_READ | VM_PROT_EXECUTE)
    {
        Bytes = pages * CodePageSize();
        mach_vm_address_t at = 0;
        if (mach_vm_allocate(mach_task_self(), &at, Bytes, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return;
        Base = reinterpret_cast<uint8_t*>(at);
        for (size_t i = 0; i < Bytes / 4; i++) reinterpret_cast<uint32_t*>(Base)[i] = kRet;
        mach_vm_protect(mach_task_self(), at, Bytes, FALSE, prot);
    }
    ~CodePages()
    {
        if (Base) mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)Base, Bytes);
    }
    uint32_t* Word(size_t offset) const { return reinterpret_cast<uint32_t*>(Base + offset); }
    IntFn Fn(size_t offset) const { return reinterpret_cast<IntFn>(Base + offset); }
};

struct RegionFacts
{
    vm_prot_t Protection{ VM_PROT_NONE };
    vm_inherit_t Inheritance{ VM_INHERIT_DEFAULT };
};

RegionFacts FactsAt(void const* p)
{
    mach_vm_address_t start = (mach_vm_address_t)p;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info{};
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object) != KERN_SUCCESS ||
        start > (mach_vm_address_t)p) {
        return {};
    }
    return { info.protection, info.inheritance };
}

// --- the code writer ------------------------------------------------------------------

TEST(writer_writes_one_word_and_a_block)
{
    CodePages code(1);
    CHECK(code.Base != nullptr);
    if (!code.Base) return;
    mach_vm_inherit(mach_task_self(), (mach_vm_address_t)code.Base, code.Bytes, VM_INHERIT_NONE);

    CHECK(WriteCodeWord(code.Word(0x100), MovW0(5)).empty());
    CHECK(*code.Word(0x100) == MovW0(5) && *code.Word(0x104) == kRet);
    CHECK(code.Fn(0x100)() == 5);

    uint32_t block[] = { MovW0(7), kRet };
    CHECK(WriteCodeBlock(code.Word(0x200), block, BlockContract::Unreachable).empty());
    CHECK(code.Fn(0x200)() == 7);
    CHECK(code.Fn(0x100)() == 5);       // the rest of the page is as it was

    // Several single words on one page, in one swap.
    WordPatch words[] = { { code.Word(0x100), MovW0(8) }, { code.Word(0x200), MovW0(9) } };
    CHECK(WriteCodeWords(words).empty());
    CHECK(code.Fn(0x100)() == 8 && code.Fn(0x200)() == 9);

    // The swapped page has the protection and inheritance the original had.
    auto facts = FactsAt(code.Base);
    CHECK(facts.Protection == (VM_PROT_READ | VM_PROT_EXECUTE));
    CHECK(facts.Inheritance == VM_INHERIT_NONE);
    auto page = QueryPage((uintptr_t)code.Base);
    CHECK(page.Mapped && page.Readable && page.Executable && !page.Writable);
}

TEST(writer_patches_this_executables_own_code)
{
    IntFn volatile victim = &WriterVictim;
    void* at = reinterpret_cast<void*>(&WriterVictim);
    CHECK(victim() == 1);
    auto before = FactsAt(at);
    CHECK(WriteCodeWord(at, MovW0(3)).empty());
    CHECK(victim() == 3);
    CHECK(WriteCodeWord(at, MovW0(1)).empty());
    CHECK(victim() == 1);
    auto after = FactsAt(at);
    CHECK(after.Protection == before.Protection && after.Protection == (VM_PROT_READ | VM_PROT_EXECUTE));
    CHECK(after.Inheritance == before.Inheritance);
}

TEST(writer_refuses_a_write_across_two_pages)
{
    CodePages code(2);
    if (!code.Base) return;
    size_t end = CodePageSize();
    uint32_t block[] = { MovW0(1), MovW0(2) };
    CHECK(!WriteCodeBlock(code.Word(end - 4), block, BlockContract::Unreachable).empty());
    CHECK(*code.Word(end - 4) == kRet && *code.Word(end) == kRet);
    WordPatch words[] = { { code.Word(end - 4), MovW0(1) }, { code.Word(end), MovW0(2) } };
    CHECK(!WriteCodeWords(words).empty());
    CHECK(*code.Word(end - 4) == kRet && *code.Word(end) == kRet);
    // Up to the last word of a page is fine.
    CHECK(WriteCodeBlock(code.Word(end - 8), block, BlockContract::Unreachable).empty());
    CHECK(*code.Word(end - 4) == MovW0(2) && *code.Word(end) == kRet);
}

TEST(writer_refuses_data_unmapped_and_misaligned_addresses)
{
    CodePages data(1, VM_PROT_READ | VM_PROT_WRITE);
    if (!data.Base) return;
    CHECK(!WriteCodeWord(data.Word(0x40), MovW0(1)).empty());
    CHECK(*data.Word(0x40) == kRet);
    CodePages readOnly(1, VM_PROT_READ);
    CHECK(!WriteCodeWord(readOnly.Word(0x40), MovW0(1)).empty());

    mach_vm_address_t gone = 0;
    CHECK(mach_vm_allocate(mach_task_self(), &gone, CodePageSize(), VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    mach_vm_deallocate(mach_task_self(), gone, CodePageSize());
    CHECK(!WriteCodeWord(reinterpret_cast<void*>(gone + 0x40), MovW0(1)).empty());

    CodePages code(1);
    CHECK(!WriteCodeWord(code.Base + 2, MovW0(1)).empty());
    CHECK(!WriteCodeWord(nullptr, MovW0(1)).empty());
    CHECK(*code.Word(0) == kRet);
}

TEST(writer_writes_a_jit_page_through_the_write_gate)
{
    size_t size = CodePageSize();
    void* jit = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    CHECK(jit != MAP_FAILED);
    if (jit == MAP_FAILED) return;
    auto words = static_cast<uint32_t*>(jit);
    uint32_t function[] = { MovW0(11), kRet };
    CHECK(WriteCodeBlock(words + 8, function, BlockContract::Unreachable).empty());
    CHECK(reinterpret_cast<IntFn>(words + 8)() == 11);
    CHECK(WriteCodeWord(words + 8, MovW0(12)).empty());
    CHECK(reinterpret_cast<IntFn>(words + 8)() == 12);
    // An aligned doubleword (a far jump's literal) is stored whole.
    WordPatch literal[] = { { words + 16, 0x55667788 }, { words + 17, 0x11223344 } };
    CHECK(WriteCodeWords(literal).empty());
    CHECK(*reinterpret_cast<uint64_t*>(words + 16) == 0x1122334455667788ull);
    munmap(jit, size);
}

TEST(pointer_stores_into_read_only_data_keep_its_protection)
{
    size_t page = CodePageSize();
    auto data = static_cast<void**>(mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    CHECK(data != MAP_FAILED);
    if (data == MAP_FAILED) return;
    data[3] = (void*)0x1111;
    CHECK(mprotect(data, page, PROT_READ) == 0);

    CHECK(StorePointer(&data[3], (void*)0x2222).empty());
    CHECK(data[3] == (void*)0x2222);
    CHECK(FactsAt(data).Protection == VM_PROT_READ);    // read-only again, as it was

    CHECK(!StorePointer(reinterpret_cast<char*>(&data[3]) + 1, nullptr).empty());     // misaligned
    CHECK(!StorePointer((void*)&WriterVictim, nullptr).empty());                       // code
    munmap(data, page);
    CHECK(!StorePointer(&data[3], nullptr).empty());                                   // unmapped
}


// --- islands ------------------------------------------------------------------------

// A Mach-O header whose __TEXT has one section `firstSection` bytes in.
void WriteFakeHeader(uint8_t* at, uint64_t firstSection)
{
    auto mh = reinterpret_cast<mach_header_64*>(at);
    auto seg = reinterpret_cast<segment_command_64*>(mh + 1);
    auto sect = reinterpret_cast<section_64*>(seg + 1);
    mh->magic = MH_MAGIC_64;
    mh->ncmds = 1;
    mh->sizeofcmds = sizeof(*seg) + sizeof(*sect);
    seg->cmd = LC_SEGMENT_64;
    seg->cmdsize = mh->sizeofcmds;
    strcpy(seg->segname, "__TEXT");
    seg->vmaddr = 0x100000000;
    seg->nsects = 1;
    strcpy(sect->sectname, "__text");
    sect->addr = 0x100000000 + firstSection;
    sect->size = 0x100;
}

constexpr uintptr_t kFakeCommandsEnd = sizeof(mach_header_64) + sizeof(segment_command_64) + sizeof(section_64);

TEST(header_padding_runs_from_the_commands_to_the_first_section)
{
    alignas(16) static uint8_t image[0x4000];
    WriteFakeHeader(image, 0x3010);
    auto padding = HeaderPaddingOf(image);
    CHECK(padding.has_value());
    if (!padding) return;
    uintptr_t base = (uintptr_t)image;
    CHECK(padding->Low >= base + kFakeCommandsEnd + 0x100 && padding->Low % 16 == 0 && padding->Low < base + kFakeCommandsEnd + 0x110);
    CHECK(padding->High == base + 0x3010);      // up to the section itself, not the page holding it

    // A section on the header's page still leaves the bytes before it.
    WriteFakeHeader(image, 0x0f00);
    CHECK(HeaderPaddingOf(image) && HeaderPaddingOf(image)->High == base + 0x0f00);
    // No room past the margin: no padding.
    WriteFakeHeader(image, kFakeCommandsEnd + 0x80);
    CHECK(!HeaderPaddingOf(image));
    image[0] = 0;
    CHECK(!HeaderPaddingOf(image));             // not a Mach-O header
}

// A two-page r-x image whose first section starts 0x40 into its second page,
// so its padding crosses a page boundary. Never unmapped: islands are not freed.
uint8_t* FakeCodeImage()
{
    size_t page = CodePageSize();
    mach_vm_address_t at = 0;
    if (mach_vm_allocate(mach_task_self(), &at, 2 * page, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return nullptr;
    WriteFakeHeader(reinterpret_cast<uint8_t*>(at), page + 0x40);
    mach_vm_protect(mach_task_self(), at, 2 * page, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    return reinterpret_cast<uint8_t*>(at);
}

bool IsFill(void const* block, size_t bytes)
{
    auto words = static_cast<uint32_t const*>(block);
    for (size_t i = 0; i < bytes / 4; i++) {
        if (words[i] != kIslandFill) return false;
    }
    return true;
}

std::string RefuseAt(uintptr_t at, std::span<uint32_t>, void* context)
{
    return at == *static_cast<uintptr_t*>(context) ? "not here" : "";
}

TEST(islands_come_from_zero_bytes_and_never_cross_a_page)
{
    uint8_t* image = FakeCodeImage();
    CHECK(image != nullptr);
    if (!image) return;
    auto padding = *HeaderPaddingOf(image);
    uintptr_t near = padding.High;
    size_t page = CodePageSize();
    std::lock_guard lock(CodeLock());
    std::string problem;

    // The bottom first, written non-zero in the swap that hands it out.
    auto a = (uintptr_t)ReserveIslandLocked(near, image, 5, nullptr, nullptr, problem, kIslandFromPadding);
    CHECK(a == padding.Low && problem.empty());
    CHECK(IsFill((void*)a, 32));

    // A block that is not zero is never handed out.
    CHECK(EditCodeLocked((void*)(a + 32 + 8), 4, [](uint8_t* copy, void*) { memset(copy, 0xee, 4); return std::string(); }, nullptr).empty());
    auto b = (uintptr_t)ReserveIslandLocked(near, image, 4, nullptr, nullptr, problem, kIslandFromPadding);
    CHECK(b == a + 48);

    // A block the builder refuses stays for the next caller.
    uintptr_t refused = b + 16;
    auto c = (uintptr_t)ReserveIslandLocked(near, image, 4, RefuseAt, &refused, problem, kIslandFromPadding);
    CHECK(c == b + 32);
    auto d = (uintptr_t)ReserveIslandLocked(near, image, 4, nullptr, nullptr, problem, kIslandFromPadding);
    CHECK(d == refused);

    // Out of branch range of the caller: refused, and the padding is untouched.
    uint32_t next[4];
    memcpy(next, (void*)(c + 16), sizeof(next));
    CHECK(ReserveIslandLocked(near + 0x10000000, image, 4, nullptr, nullptr, problem, kIslandFromPadding) == nullptr);
    CHECK(!problem.empty());
    CHECK(memcmp(next, (void*)(c + 16), sizeof(next)) == 0);

    // Up to the first section, never across the page boundary.
    uintptr_t last = 0;
    bool startsPage = false;
    while (auto block = (uintptr_t)ReserveIslandLocked(near, image, 12, nullptr, nullptr, problem, kIslandFromPadding)) {
        CHECK(block / page == (block + 47) / page);
        CHECK(block + 48 <= padding.High && IsFill((void*)block, 48));
        startsPage = startsPage || block % page == 0;
        last = block;
    }
    CHECK(startsPage);
    CHECK(last && last + 48 > padding.High - 48);
    auto facts = QueryPage((uintptr_t)image);
    CHECK(facts.Readable && facts.Executable && !facts.Writable);
}

int NearTarget()
{
    return 21;
}

TEST(islands_fall_back_to_a_page_mapped_near_the_code)
{
    // As when the image's padding has no room: a page in a free gap in range.
    auto near = (uintptr_t)&NearTarget;
    std::string problem;
    void* first;
    void* second;
    {
        std::lock_guard lock(CodeLock());
        first = ReserveIslandLocked(near, nullptr, 4, nullptr, nullptr, problem, kIslandFromNearPage);
        second = ReserveIslandLocked(near, nullptr, 4, nullptr, nullptr, problem, kIslandFromNearPage);
    }
    CHECK(first && second);
    if (!first || !second) return;
    CHECK(InBranchRange(near, (uintptr_t)first, 16) && InBranchRange(near, (uintptr_t)second, 16));
    CHECK((uintptr_t)second == (uintptr_t)first + 16);       // the same page, next block
    CHECK(IsFill(first, 16) && IsFill(second, 16));
    auto facts = QueryPage((uintptr_t)first);
    CHECK(facts.Readable && facts.Executable && !facts.Writable);
    CHECK(ImageHeaderOf(first) == nullptr);
}

std::string BuildReturn33(uintptr_t, std::span<uint32_t> words, void*)
{
    words[0] = MovW0(33);
    words[1] = kRet;
    return {};
}

TEST(islands_fall_back_to_a_jit_page)
{
    auto near = (uintptr_t)&NearTarget;
    std::string problem;
    void* island;
    {
        std::lock_guard lock(CodeLock());
        island = ReserveIslandLocked(near, nullptr, 2, BuildReturn33, nullptr, problem, kIslandFromNearJitPage);
    }
    CHECK(island != nullptr);
    if (!island) return;
    CHECK(InBranchRange(near, (uintptr_t)island, 16));
    auto facts = QueryPage((uintptr_t)island);
    CHECK(facts.Readable && facts.Writable && facts.Executable);
    CHECK(reinterpret_cast<IntFn>(island)() == 33);
    CHECK(static_cast<uint32_t*>(island)[2] == kIslandFill);
}

TEST(jump_pad_reaches_its_target)
{
    std::string problem;
    void* pad = MakeJumpPad((void*)&WriterVictim, (void*)&NearTarget, problem);
    CHECK(pad != nullptr && problem.empty());
    if (!pad) return;
    CHECK(InBranchRange((uintptr_t)&WriterVictim, (uintptr_t)pad, 16));
    CHECK(reinterpret_cast<IntFn>(pad)() == 21);
    CHECK(static_cast<uint32_t*>(pad)[0] == 0x58000050 && static_cast<uint32_t*>(pad)[1] == 0xd61f0200);
}

// --- the entry hook planner ----------------------------------------------------------------

// The function at A, the trampoline at T: 32 MB below, so every relocation is re-encoded.
constexpr uint64_t kA = 0x104000000, kT = 0x102000010;

struct PlanCase
{
    char const* Name;
    std::vector<uint32_t> Function;
    std::vector<uint32_t> Trampoline;
};

TEST(planner_relocates_each_kind_of_first_instruction)
{
    // Expected words from an encoder written separately from the planner's.
    PlanCase const cases[] = {
        { "copy", { 0xa9bf7bfd }, { 0xa9bf7bfd, 0x147ffffc } },                                 // stp x29, x30, [sp, #-16]!
        { "adrp", { 0xf0000908 }, { 0xf0010908, 0x147ffffc } },                                 // adrp x8, A+0x123000
        { "adr far", { 0x10002001 }, { 0x90010001, 0x91100021, 0x147ffffb } },                  // adr x1, A+0x400
        { "b", { 0x14000400 }, { 0x148003fc } },                                                // b A+0x1000
        { "b.ne", { 0x54000201 }, { 0x54000041, 0x147ffffc, 0x1480000a } },                     // b.ne A+0x40
        { "cbz", { 0xb4000403 }, { 0xb4000043, 0x147ffffc, 0x1480001a } },                      // cbz x3, A+0x80
        { "tbnz", { 0x37180105 }, { 0x37180045, 0x147ffffc, 0x14800002 } },                     // tbnz w5, #3, A+0x20
        { "tbz high bit", { 0xb647ff05 }, { 0xb6400045, 0x147ffffc, 0x147ffff2 } },             // tbz x5, #40, A-0x20
        { "ldr x unaligned", { 0x58008022 }, { 0xb0010002, 0x91001042, 0xf9400042, 0x147ffffa } },  // ldr x2, A+0x1004
        { "ldr x", { 0x58008042 }, { 0xb0010002, 0xf9400442, 0x147ffffb } },                    // ldr x2, A+0x1008
        { "ldr w", { 0x18008062 }, { 0xb0010002, 0xb9400c42, 0x147ffffb } },                    // ldr w2, A+0x100c
        { "ldrsw", { 0x98008062 }, { 0xb0010002, 0xb9800c42, 0x147ffffb } },                    // ldrsw x2, A+0x100c
        { "paciasp", { 0xd503233f }, { 0xd503233f, 0x147ffffc } },
        { "mrs", { 0xd53bd060 }, { 0xd53bd060, 0x147ffffc } },                                  // mrs x0, tpidrro_el0
        { "ret", { 0xd65f03c0 }, { 0xd65f03c0, 0x147ffffc } },
        { "ldr register", { 0xf9400020 }, { 0xf9400020, 0x147ffffc } },
        { "add register", { 0x8b020020 }, { 0x8b020020, 0x147ffffc } },
        { "fadd", { 0x1e222820 }, { 0x1e222820, 0x147ffffc } },
    };
    for (auto const& c : cases) {
        auto plan = PlanTrampoline(c.Function, kA, kT);
        bool same = plan.Ok() && plan.Count == c.Trampoline.size() && std::equal(c.Trampoline.begin(), c.Trampoline.end(), plan.Words);
        if (!same) printf("  case %s: %s\n", c.Name, Describe(plan.Refusal));
        CHECK(same);
    }
    // adr within its own range of the trampoline stays an adr.
    auto near = PlanTrampoline(std::vector<uint32_t>{ 0x10002001 }, kA, kA - 0x1000);
    CHECK(near.Ok() && near.Count == 2 && near.Words[0] == 0x1000a001 && near.Words[1] == 0x14000400);
}

TEST(planner_refuses_what_it_cannot_move)
{
    struct Refusal
    {
        char const* Name;
        std::vector<uint32_t> Function;
        HookRefusal Expected;
    } const cases[] = {
        { "ldr s0 literal", { 0x1c000800 }, HookRefusal::LiteralIntoFpSimd },
        { "ldr q0 literal", { 0x9c000800 }, HookRefusal::LiteralIntoFpSimd },
        { "prfm literal", { 0xd8000800 }, HookRefusal::PrefetchLiteral },
        { "ldr xzr literal", { 0x5800081f }, HookRefusal::ZeroRegister },
        { "adr xzr out of its range", { 0x1000201f }, HookRefusal::ZeroRegister },
        { "bl", { 0x94000800 }, HookRefusal::Call },                                // bl A+0x2000
        { "blr", { 0xd63f0100 }, HookRefusal::Call },                               // blr x8
        { "brk", { 0xd4200000 }, HookRefusal::Trap },
        { "udf", { 0x00000000 }, HookRefusal::Trap },
        { "hlt", { 0xd4400000 }, HookRefusal::Unrecognised },
        { "sve", { 0x04603000 }, HookRefusal::Unrecognised },
        { "bc.ne", { 0x54000211 }, HookRefusal::Unrecognised },
        { "braa", { 0xd71f0800 }, HookRefusal::Unrecognised },
        { "retaa", { 0xd65f0bff }, HookRefusal::Unrecognised },
        { "unallocated", { 0x02000000 }, HookRefusal::Unrecognised },
        { "loop head", { 0xd503201f, 0xb5ffffe0 }, HookRefusal::BranchesToItsEntry },  // nop; cbnz x0, <entry>
        { "tail call to itself", { 0xd503201f, 0xd503201f, 0x17fffffe }, HookRefusal::BranchesToItsEntry },
        { "b to itself", { 0x14000000 }, HookRefusal::BranchesToItsEntry },
        { "b.eq to itself", { 0x54000000 }, HookRefusal::BranchesToItsEntry },
        // What the hook's far jump changes, read before it is written.
        { "mov x0, x16", { 0xaa1003e0 }, HookRefusal::ReadsScratchRegisters },
        { "svc takes x16", { 0xd4001001 }, HookRefusal::ReadsScratchRegisters },
        { "x17 after a branch", { 0xb4000040, 0xd503201f, 0x8b110000 }, HookRefusal::ReadsScratchRegisters },  // cbz x0, +8; nop; add x0, x0, x17
        { "x16 after a jump table", { 0xd503201f, 0xd61f0120, 0xaa1003e0 }, HookRefusal::ReadsScratchRegisters },   // nop; br x9; mov x0, x16
        { "blr x16", { 0xd503201f, 0xd63f0200 }, HookRefusal::ReadsScratchRegisters },
        { "movk x16", { 0xf2a00030 }, HookRefusal::ReadsScratchRegisters },
        { "empty", {}, HookRefusal::NoFunction },
    };
    for (auto const& c : cases) {
        auto plan = PlanTrampoline(c.Function, kA, kT);
        if (plan.Refusal != c.Expected) printf("  case %s: %s\n", c.Name, Describe(plan.Refusal));
        CHECK(plan.Refusal == c.Expected && plan.Count == 0);
    }
    // Written before it is read, ended by a call, or 16 only as an immediate: no refusal.
    std::vector<uint32_t> const scratchFree[] = {
        { 0xd2800030, 0xd4001001 },                 // mov x16, #1; svc #0x80
        { 0xd503201f, 0x94000010, 0xaa1003e0 },     // nop; bl; mov x0, x16
        { 0x910043ff, 0xf9404020, 0xd65f03c0 },     // add sp, sp, #0x10; ldr x0, [x1, #0x80]; ret
        { 0xd65f03c0, 0xaa1003e0 },                 // ret; mov x0, x16 (not reached)
        { 0x9e660010, 0x1e220200 },                 // fmov x16, d0; scvtf s0, w16
        { 0x4e010e10, 0xaa1003e0 },                 // dup v16.16b, w16 is a read: below
    };
    for (size_t k = 0; k + 1 < std::size(scratchFree); k++) {
        auto plan = PlanTrampoline(scratchFree[k], kA, kT);
        if (!plan.Ok()) printf("  scratch case %zu: %s\n", k, Describe(plan.Refusal));
        CHECK(plan.Ok());
    }
    CHECK(PlanTrampoline(scratchFree[std::size(scratchFree) - 1], kA, kT).Refusal == HookRefusal::ReadsScratchRegisters);
    // A call to itself is not a branch to its entry.
    auto recursive = PlanTrampoline(std::vector<uint32_t>{ 0xd503201f, 0xd503201f, 0x97fffffe }, kA, kT);
    CHECK(recursive.Ok());
    CHECK(PlanTrampoline(std::vector<uint32_t>{ 0xd503201f }, kA + 2, kT).Refusal == HookRefusal::Misaligned);
    CHECK(PlanTrampoline(std::vector<uint32_t>{ 0xd503201f }, kA, kT + 2).Refusal == HookRefusal::Misaligned);
}

TEST(planner_respects_branch_range)
{
    // b to the farthest address the trampoline reaches, and one word beyond.
    auto farthest = PlanTrampoline(std::vector<uint32_t>{ 0x15800003 }, kA, kT);
    CHECK(farthest.Ok() && farthest.Count == 1 && farthest.Words[0] == 0x15ffffff);
    CHECK(PlanTrampoline(std::vector<uint32_t>{ 0x15800004 }, kA, kT).Refusal == HookRefusal::OutOfRange);
    // The way back to the second instruction must be in range too.
    CHECK(PlanTrampoline(std::vector<uint32_t>{ 0xa9bf7bfd }, kA, kA - (128ull << 20) + 4).Ok());
    CHECK(PlanTrampoline(std::vector<uint32_t>{ 0xa9bf7bfd }, kA, kA - (128ull << 20)).Refusal == HookRefusal::OutOfRange);
    CHECK(PlanTrampoline(std::vector<uint32_t>{ 0xa9bf7bfd }, kA, kA + (128ull << 20)).Ok());
    CHECK(PlanTrampoline(std::vector<uint32_t>{ 0xa9bf7bfd }, kA, kA + (128ull << 20) + 4).Refusal == HookRefusal::OutOfRange);
    // b.ne A+0x40 with the trampoline 128 MB - 8 below: the way back reaches, the taken branch does not.
    CHECK(PlanTrampoline(std::vector<uint32_t>{ 0x54000201 }, kA, kA - (128ull << 20) + 8).Refusal == HookRefusal::OutOfRange);
    CHECK(PlanTrampoline(std::vector<uint32_t>{ 0x54000201 }, kA, kA - (128ull << 20) + 0x48).Ok());
}

// --- entry hooks on real functions ------------------------------------------------------

size_t Extent(void const* start, char const* end)
{
    return (size_t)(reinterpret_cast<uintptr_t>(end) - reinterpret_cast<uintptr_t>(start));
}

int (*gAdrpOriginal)() = nullptr;
int AdrpReplacement() { return gAdrpOriginal() + 1; }
int AdrpOtherReplacement() { return gAdrpOriginal() + 2; }

TEST(entry_hook_on_a_function_starting_with_adrp)
{
    int (*volatile fn)() = &hook_adrp_first;
    CHECK(fn() == 4242);
    std::string problem;
    auto hook = PrepareEntryHook((void*)&hook_adrp_first, Extent((void*)&hook_adrp_first, hook_adrp_first_end), (void*)&AdrpReplacement,
        reinterpret_cast<void**>(&gAdrpOriginal), problem);
    CHECK(hook != nullptr && problem.empty());
    if (!hook) {
        printf("  %s\n", problem.c_str());
        return;
    }
    // Prepared: the trampoline runs the original; the function is untouched.
    CHECK(gAdrpOriginal == hook->Trampoline() && gAdrpOriginal() == 4242 && fn() == 4242 && !hook->Committed());
    CHECK(FindEntryHook((void*)&hook_adrp_first) == hook);
    CHECK(InBranchRange((uintptr_t)&hook_adrp_first, (uintptr_t)hook->Island(), 32));
    CHECK(CommitEntryHook(hook).empty() && hook->Committed());
    CHECK(fn() == 4243);
    hook_data = 100;
    CHECK(fn() == 101 && gAdrpOriginal() == 100);
    hook_data = 4242;

    // One hook per function: a committed one is not prepared again.
    void* again = nullptr;
    CHECK(PrepareEntryHook((void*)&hook_adrp_first, 12, (void*)&AdrpReplacement, &again, problem) == nullptr);
    CHECK(problem.find(Describe(HookRefusal::AlreadyHooked)) != std::string::npos && again == nullptr);

    CHECK(RemoveEntryHook(hook).empty() && !hook->Committed());
    CHECK(fn() == 4242);
    // Committed again, with the island and trampoline it had.
    CHECK(CommitEntryHook(hook).empty() && fn() == 4243);
    CHECK(RemoveEntryHook(hook).empty() && fn() == 4242);

    // Removed, then prepared again with another replacement: the same hook, re-targeted.
    gAdrpOriginal = nullptr;
    CHECK(PrepareEntryHook((void*)&hook_adrp_first, 12, (void*)&AdrpOtherReplacement, reinterpret_cast<void**>(&gAdrpOriginal), problem) == hook);
    CHECK(problem.empty() && gAdrpOriginal == hook->Trampoline() && fn() == 4242);
    CHECK(CommitEntryHook(hook).empty() && fn() == 4244);
    CHECK(RemoveEntryHook(hook).empty() && fn() == 4242);
}

int (*gCbzOriginal)(long) = nullptr;
int CbzReplacement(long x) { return gCbzOriginal(x) * 10; }

TEST(entry_hook_on_a_function_starting_with_cbz)
{
    int (*volatile fn)(long) = &hook_cbz_first;
    std::string problem;
    size_t extent = Extent((void*)&hook_cbz_first, hook_cbz_first_end);
    // Prepared and left uncommitted, as an install whose commit failed leaves
    // it: the install can be retried and goes in with the same hook.
    auto prepared = PrepareEntryHook((void*)&hook_cbz_first, extent, (void*)&CbzReplacement, reinterpret_cast<void**>(&gCbzOriginal), problem);
    CHECK(prepared != nullptr && fn(0) == 3);
    auto hook = InstallEntryHook((void*)&hook_cbz_first, extent, (void*)&CbzReplacement, reinterpret_cast<void**>(&gCbzOriginal), problem);
    CHECK(hook != nullptr && hook == prepared && hook->Committed());
    if (!hook) {
        printf("  %s\n", problem.c_str());
        return;
    }
    CHECK(fn(0) == 30 && fn(5) == 20);              // both ways out of the moved cbz
    CHECK(gCbzOriginal(0) == 3 && gCbzOriginal(5) == 2);
    CHECK(RemoveEntryHook(hook).empty());
    CHECK(fn(0) == 3 && fn(5) == 2);
}

uint64_t (*gLiteralOriginal)() = nullptr;
uint64_t LiteralReplacement() { return gLiteralOriginal() ^ 0xff; }

TEST(entry_hook_on_a_function_starting_with_a_literal_load)
{
    uint64_t (*volatile fn)() = &hook_literal_first;
    std::string problem;
    auto hook = InstallEntryHook((void*)&hook_literal_first, Extent((void*)&hook_literal_first, hook_literal_first_end),
        (void*)&LiteralReplacement, reinterpret_cast<void**>(&gLiteralOriginal), problem);
    CHECK(hook != nullptr);
    if (!hook) {
        printf("  %s\n", problem.c_str());
        return;
    }
    CHECK(fn() == (0x1122334455667788ull ^ 0xff) && gLiteralOriginal() == 0x1122334455667788ull);
    CHECK(RemoveEntryHook(hook).empty() && fn() == 0x1122334455667788ull);
}

long LoopReplacement(long) { return -1; }

TEST(entry_hook_refuses_a_loop_head_and_changes_nothing)
{
    uint32_t before[3];
    memcpy(before, (void*)&hook_loop_head, sizeof(before));
    void* original = nullptr;
    std::string problem;
    CHECK(PrepareEntryHook((void*)&hook_loop_head, Extent((void*)&hook_loop_head, hook_loop_head_end), (void*)&LoopReplacement, &original, problem) == nullptr);
    CHECK(problem.find(Describe(HookRefusal::BranchesToItsEntry)) != std::string::npos);
    CHECK(original == nullptr && memcmp(before, (void*)&hook_loop_head, sizeof(before)) == 0);
    CHECK(hook_loop_head(5) == 0);
    CHECK(PrepareEntryHook((void*)((uintptr_t)&hook_loop_head + 2), 8, (void*)&LoopReplacement, &original, problem) == nullptr);
    CHECK(PrepareEntryHook((void*)&hook_loop_head, 0, (void*)&LoopReplacement, &original, problem) == nullptr);
    uint64_t data = 0;
    CHECK(PrepareEntryHook(&data, 8, (void*)&LoopReplacement, &original, problem) == nullptr);       // not code
    CHECK(problem.find(Describe(HookRefusal::NotExecutable)) != std::string::npos);
}

// Enough locals and a call that the compiler emits a frame-building prologue.
extern "C" __attribute__((noinline)) int HookFrameTarget(int a, int b)
{
    volatile int scratch[8];
    for (int i = 0; i < 8; i++) scratch[i] = a + i;
    printf("%s", "");
    return scratch[3] * b;
}

int (*gFrameOriginal)(int, int) = nullptr;
int gFrameCalls = 0;
int FrameReplacement(int a, int b)
{
    gFrameCalls++;
    return gFrameOriginal(a, b) + 1000;
}

TEST(entry_hook_on_a_compiled_function)
{
    int (*volatile fn)(int, int) = &HookFrameTarget;
    CHECK(fn(4, 5) == 35);
    // Its extent from this executable's own symbol table.
    auto image = GameImage::Current();
    CHECK(image != nullptr);
    if (!image) return;
    uint64_t va = image->FindVA("_HookFrameTarget");
    uint64_t end = va ? image->NextSymbolVA(va) : 0;
    CHECK(va && end > va && image->CodeAt(va) == (void const*)&HookFrameTarget);
    if (!va || end <= va) return;
    CHECK(CheckEntryHook(*image, va, end) == HookRefusal::None);

    std::string problem;
    auto hook = InstallEntryHook((void*)&HookFrameTarget, end - va, (void*)&FrameReplacement, reinterpret_cast<void**>(&gFrameOriginal), problem);
    CHECK(hook != nullptr);
    if (!hook) {
        printf("  %s\n", problem.c_str());
        return;
    }
    CHECK(fn(4, 5) == 1035 && gFrameCalls == 1);
    CHECK(RemoveEntryHook(hook).empty() && fn(4, 5) == 35 && gFrameCalls == 1);
}

int (*gPageOriginal[2])() = {};
int PageReplacement0() { return gPageOriginal[0]() + 100; }
int PageReplacement1() { return gPageOriginal[1]() + 200; }

TEST(entry_hooks_outside_any_image_use_a_page_mapped_near_them)
{
    // Two functions on a page of no image, so no header padding: the islands
    // must come from a page mapped near them. Both go in with one swap.
    static CodePages code(1);
    CHECK(code.Base != nullptr);
    if (!code.Base) return;
    uint32_t f0[] = { 0xd2800028, MovW0(70), kRet };    // mov x8, #1; mov w0, #70; ret
    uint32_t f1[] = { 0xd2800028, MovW0(71), kRet };
    CHECK(WriteCodeBlock(code.Word(0x100), f0, BlockContract::Unreachable).empty());
    CHECK(WriteCodeBlock(code.Word(0x200), f1, BlockContract::Unreachable).empty());
    std::string problem;
    EntryHook* hooks[2] = {
        PrepareEntryHook(code.Word(0x100), sizeof(f0), (void*)&PageReplacement0, reinterpret_cast<void**>(&gPageOriginal[0]), problem),
        PrepareEntryHook(code.Word(0x200), sizeof(f1), (void*)&PageReplacement1, reinterpret_cast<void**>(&gPageOriginal[1]), problem),
    };
    CHECK(hooks[0] && hooks[1]);
    if (!hooks[0] || !hooks[1]) {
        printf("  %s\n", problem.c_str());
        return;
    }
    for (auto h : hooks) {
        CHECK(ImageHeaderOf(h->Island()) == nullptr);
        CHECK(InBranchRange((uintptr_t)h->Target(), (uintptr_t)h->Island(), 32));
    }
    CHECK(code.Fn(0x100)() == 70 && code.Fn(0x200)() == 71);
    CHECK(CommitEntryHooks(hooks).empty());
    CHECK(code.Fn(0x100)() == 170 && code.Fn(0x200)() == 271);
    CHECK(RemoveEntryHook(hooks[0]).empty() && RemoveEntryHook(hooks[1]).empty());
    CHECK(code.Fn(0x100)() == 70 && code.Fn(0x200)() == 71);

    // A function whose first instruction branches to a far jump carries
    // another copy's hook.
    void* pad = MakeJumpPad(code.Word(0x300), (void*)&NearTarget, problem);
    CHECK(pad != nullptr);
    uint32_t hooked[] = { EncodeBranch((uintptr_t)code.Word(0x300), (uintptr_t)pad, false), kRet };
    CHECK(hooked[0] && WriteCodeBlock(code.Word(0x300), hooked, BlockContract::Unreachable).empty());
    void* original = nullptr;
    CHECK(PrepareEntryHook(code.Word(0x300), sizeof(hooked), (void*)&PageReplacement0, &original, problem) == nullptr);
    CHECK(problem.find(Describe(HookRefusal::AlreadyHooked)) != std::string::npos);
}

// --- function extents ------------------------------------------------------------------

// The test dylib's functions, from lib_add_one: lib_add_one (2 words),
// lib_local (2, no symbol in the copy without local symbols), lib_twice (2),
// lib_add_local (7), then the end of __text.
constexpr uint64_t kLocal = 8, kTwice = 16, kAddLocal = 24, kTextEnd = 52;

void const* gUnloadedHeader = nullptr;

TEST(function_extents_from_the_symbol_table_of_an_image_without_function_starts)
{
    void* lib = dlopen(BG3MSE_TEST_FUNCTIONS_SYMBOLS, RTLD_NOW | RTLD_LOCAL);
    CHECK(lib != nullptr);
    if (!lib) return;
    auto addOne = reinterpret_cast<uint64_t>(dlsym(lib, "lib_add_one"));
    CHECK(addOne && reinterpret_cast<uint64_t>(dlsym(lib, "lib_twice")) == addOne + kTwice);
    auto e = FindLoadedFunction((void*)addOne);
    CHECK(e.Ok() && e.Source == FunctionSource::SymbolTable);
    // lib_local has no symbol here, so it is counted in lib_add_one's extent.
    CHECK(e.Start == addOne && e.End == addOne + kTwice);
    CHECK(FindLoadedFunction((void*)(addOne + kLocal)).Refusal == ExtentRefusal::NotAStart);
    CHECK(FindLoadedFunction((void*)(addOne + 4)).Refusal == ExtentRefusal::NotAStart);
    auto last = FindLoadedFunction((void*)(addOne + kAddLocal));
    CHECK(last.Ok() && last.End == addOne + kTextEnd);       // the last function ends with its section
    CHECK(FindLoadedFunction(dlsym(lib, "lib_data")).Refusal == ExtentRefusal::NotCode);

    Dl_info info{};
    CHECK(dladdr((void*)addOne, &info) && info.dli_fbase);
    gUnloadedHeader = info.dli_fbase;
    // Unloaded: nothing of this copy has a hook, so nothing points into it.
    CHECK(dlclose(lib) == 0);
    CHECK(!dladdr((void*)addOne, &info));
    CHECK(FindLoadedFunction((void*)addOne).Refusal == ExtentRefusal::NotInImage);
}

TEST(function_extents_from_the_function_starts_of_a_loaded_dylib)
{
    // Never closed: the Detours tests' islands may sit in its header padding.
    void* lib = dlopen(BG3MSE_TEST_FUNCTIONS, RTLD_NOW | RTLD_LOCAL);
    CHECK(lib != nullptr);
    if (!lib) return;
    auto addOne = reinterpret_cast<uint64_t>(dlsym(lib, "lib_add_one"));
    CHECK(addOne != 0);
    Dl_info info{};
    CHECK(dladdr((void*)addOne, &info));
    // When dyld loads it where the copy unloaded above was, the cached table
    // of that copy must not be used.
    if (info.dli_fbase == gUnloadedHeader) printf("  loaded where the unloaded copy was\n");

    struct Expected
    {
        uint64_t Offset, End;
    } const functions[] = { { 0, kLocal }, { kLocal, kTwice }, { kTwice, kAddLocal }, { kAddLocal, kTextEnd } };
    for (auto const& f : functions) {
        auto e = FindLoadedFunction((void*)(addOne + f.Offset));
        CHECK(e.Ok() && e.Source == FunctionSource::FunctionStarts && e.Start == addOne + f.Offset && e.End == addOne + f.End);
    }
    CHECK(FindLoadedFunction((void*)(addOne + 4)).Refusal == ExtentRefusal::NotAStart);
    CHECK(FindLoadedFunction((void*)(addOne + kAddLocal + 8)).Refusal == ExtentRefusal::NotAStart);
    CHECK(FindLoadedFunction(dlsym(lib, "lib_data")).Refusal == ExtentRefusal::NotCode);
    CHECK(FindLoadedFunction(info.dli_fbase).Refusal == ExtentRefusal::NotCode);       // the header
    // Again, from the cache.
    auto again = FindLoadedFunction((void*)addOne);
    CHECK(again.Ok() && again.End == addOne + kLocal);
}

TEST(function_extents_of_this_executable)
{
    auto e = FindLoadedFunction((void*)&HookFrameTarget);
    CHECK(e.Ok() && e.Source == FunctionSource::FunctionStarts && e.Start == (uint64_t)&HookFrameTarget);
    // Against the symbol table: the next named address, or an unnamed start before it.
    auto image = GameImage::Current();
    CHECK(image != nullptr);
    if (image) {
        uint64_t va = image->FindVA("_HookFrameTarget");
        uint64_t slide = (uint64_t)&HookFrameTarget - va;
        CHECK(va && e.End > e.Start && e.End - slide <= image->NextSymbolVA(va));
    }
    // Functions of an assembly file without .subsections_via_symbols.
    auto adrp = FindLoadedFunction((void*)&hook_adrp_first);
    CHECK(adrp.Ok() && adrp.End == (uint64_t)hook_adrp_first_end);
    auto stress = FindLoadedFunction((void*)&hook_stress_target);
    CHECK(stress.Ok() && stress.End == (uint64_t)hook_stress_target_end);

    Dl_info info{};
    CHECK(dladdr((void*)&HookFrameTarget, &info));
    unsigned long size = 0;
    auto stubs = getsectiondata(static_cast<mach_header_64 const*>(info.dli_fbase), "__TEXT", "__stubs", &size);
    CHECK(stubs && size);
    if (stubs) CHECK(FindLoadedFunction(stubs).Refusal == ExtentRefusal::Stub);
    CHECK(FindLoadedFunction((void*)((uintptr_t)&HookFrameTarget + 4)).Refusal == ExtentRefusal::NotAStart);
    CHECK(FindLoadedFunction(nullptr).Refusal == ExtentRefusal::NotInImage);
    CodePages code(1);
    CHECK(FindLoadedFunction(code.Base).Refusal == ExtentRefusal::NotInImage);
}

std::vector<uint8_t> ReadFile(char const* path)
{
    std::vector<uint8_t> bytes;
    int fd = open(path, O_RDONLY);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0) close(fd);
        return bytes;
    }
    bytes.resize((size_t)st.st_size);
    if (read(fd, bytes.data(), bytes.size()) != (ssize_t)bytes.size()) bytes.clear();
    close(fd);
    return bytes;
}

// The file wrapped in a fat file as its arm64 slice, at a 16 KB boundary.
std::vector<uint8_t> FatFile(std::vector<uint8_t> const& thin)
{
    constexpr uint32_t kSlice = 0x4000;
    std::vector<uint8_t> fat(kSlice + thin.size(), 0);
    auto be = [&](size_t at, uint32_t value) { OSWriteBigInt32(fat.data(), at, value); };
    be(0, FAT_MAGIC);
    be(4, 2);
    // An x86_64 slice entry first, which must be passed over.
    be(8, CPU_TYPE_X86_64);
    be(16, kSlice);
    be(20, 0);
    be(28, CPU_TYPE_ARM64);
    be(36, kSlice);
    be(40, (uint32_t)thin.size());
    be(44, 14);
    memcpy(fat.data() + kSlice, thin.data(), thin.size());
    return fat;
}

TEST(function_extents_from_a_file_agree_with_the_loaded_image)
{
    void* lib = dlopen(BG3MSE_TEST_FUNCTIONS, RTLD_NOW | RTLD_LOCAL);
    CHECK(lib != nullptr);
    if (!lib) return;
    auto addOne = reinterpret_cast<uint64_t>(dlsym(lib, "lib_add_one"));
    for (char const* path : { BG3MSE_TEST_FUNCTIONS, BG3MSE_TEST_FUNCTIONS_SYMBOLS }) {
        auto thin = ReadFile(path);
        CHECK(!thin.empty());
        GameImage symbols;
        CHECK(symbols.OpenFile(thin));
        uint64_t va = symbols.FindVA("_lib_add_one");
        CHECK(va != 0);
        bool withStarts = path == std::string_view(BG3MSE_TEST_FUNCTIONS);
        for (auto const& file : { thin, FatFile(thin) }) {
            FunctionTable table;
            std::string problem;
            CHECK(table.OpenFile(file, problem) && problem.empty());
            CHECK(table.Source() == (withStarts ? FunctionSource::FunctionStarts : FunctionSource::SymbolTable));
            for (uint64_t offset : { 0ull, 4ull, kLocal, kTwice, kAddLocal, kTextEnd }) {
                auto fromFile = table.At(va + offset);
                if (withStarts) {
                    // The same answer as for the loaded image, in link-time addresses.
                    auto loaded = FindLoadedFunction((void*)(addOne + offset));
                    CHECK(fromFile.Refusal == loaded.Refusal);
                    CHECK(!loaded.Ok() || (fromFile.Start == va + offset && fromFile.End == va + (loaded.End - addOne)));
                } else {
                    bool start = offset == 0 || offset == kTwice || offset == kAddLocal;
                    CHECK(fromFile.Ok() == start);
                }
            }
        }
        // Not a Mach-O file, and one cut short inside its load commands.
        FunctionTable table;
        std::string problem;
        std::vector<uint8_t> junk(4096, 0xab);
        CHECK(!table.OpenFile(junk, problem) && !problem.empty() && table.At(va).Refusal == ExtentRefusal::NoTable);
        CHECK(!table.OpenFile(std::span(thin).first(64), problem) && !problem.empty());
    }
}

// --- hooking while other threads run the function ----------------------------------------------

int (*gStressOriginal)() = nullptr;
int StressReplacement() { return gStressOriginal() + 10000; }

std::atomic<bool> gStressStop{ false };
std::atomic<uint64_t> gStressCalls{ 0 }, gStressBad{ 0 }, gStressReplaced{ 0 };

// On the page of hook_stress_target, so the callers execute the page that is swapped.
extern "C" __attribute__((noinline, section("__TEXT,__hookstress,regular,pure_instructions"))) void StressCaller()
{
    uint64_t calls = 0, bad = 0, replaced = 0;
    while (!gStressStop.load(std::memory_order_relaxed)) {
        int value = hook_stress_target();
        if (value == 11234) replaced++;
        else if (value != 1234) bad++;
        calls++;
    }
    gStressCalls += calls;
    gStressBad += bad;
    gStressReplaced += replaced;
}

TEST(entry_hook_goes_in_and_out_while_threads_run_the_function)
{
    auto page = [](void const* p) { return (uintptr_t)p / CodePageSize(); };
    CHECK(page((void*)&StressCaller) == page((void*)&hook_stress_target));
    std::string problem;
    auto hook = PrepareEntryHook((void*)&hook_stress_target, Extent((void*)&hook_stress_target, hook_stress_target_end),
        (void*)&StressReplacement, reinterpret_cast<void**>(&gStressOriginal), problem);
    CHECK(hook != nullptr);
    if (!hook) {
        printf("  %s\n", problem.c_str());
        return;
    }
    std::vector<std::thread> threads;
    for (int i = 0; i < 6; i++) threads.emplace_back(StressCaller);

    // A fixed number of rounds (about a second's worth on an idle machine),
    // however long this machine takes over them: the two minutes only end a
    // run that is getting nowhere.
    constexpr int kRounds = 8000;
    int rounds = 0, failures = 0;
    auto until = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    while (rounds < kRounds && std::chrono::steady_clock::now() < until) {
        if (!CommitEntryHook(hook).empty()) failures++;
        if (!RemoveEntryHook(hook).empty()) failures++;
        rounds++;
    }
    gStressStop = true;
    for (auto& t : threads) t.join();
    printf("  %d rounds, %llu calls, %llu replaced\n", rounds, (unsigned long long)gStressCalls.load(), (unsigned long long)gStressReplaced.load());
    CHECK(failures == 0);
    CHECK(rounds == kRounds);
    CHECK(gStressBad == 0);
    CHECK(gStressCalls > 0 && gStressReplaced > 0 && gStressReplaced < gStressCalls);
    CHECK(hook_stress_target() == 1234);
}
}

// --- symbol lookups ---------------------------------------------------------------------------

TEST(lookups_tell_missing_from_ambiguous)
{
    FakeBinary binary;
    GameImage image;
    CHECK(image.OpenFile(binary.File));
    char const* names[] = { "_first", "_second", "_missing" };
    GameImage::Symbol found[3];
    image.Lookup(names, found);
    CHECK(found[0].Status == GameImage::SymbolStatus::Found && found[0].VA == FakeBinary::Text);
    CHECK(found[1].Status == GameImage::SymbolStatus::Found && found[1].VA == FakeBinary::Text + 0x40);
    CHECK(found[2].Status == GameImage::SymbolStatus::Missing && found[2].VA == 0);

    std::string_view prefixes[] = { "_fir", "_", "_none", "_af" };
    GameImage::Symbol byPrefix[4];
    image.LookupByPrefix(prefixes, byPrefix);
    CHECK(byPrefix[0].Status == GameImage::SymbolStatus::Found && byPrefix[0].VA == FakeBinary::Text);
    CHECK(byPrefix[1].Status == GameImage::SymbolStatus::Ambiguous && byPrefix[1].VA == 0);
    CHECK(byPrefix[2].Status == GameImage::SymbolStatus::Missing);
    CHECK(byPrefix[3].Status == GameImage::SymbolStatus::Found && byPrefix[3].VA == FakeBinary::Data + 0x30);

    // The second symbol given the first one's name: one name at two addresses.
    FakeBinary twice;
    twice.Symbol(1)->n_un.n_strx = twice.Symbol(0)->n_un.n_strx;
    GameImage twiceImage;
    CHECK(twiceImage.OpenFile(twice.File));
    GameImage::Symbol again[3];
    twiceImage.Lookup(names, again);
    CHECK(again[0].Status == GameImage::SymbolStatus::Ambiguous && again[0].VA == 0);
    CHECK(again[1].Status == GameImage::SymbolStatus::Missing);
}

TEST(lookups_list_symbols_and_sections)
{
    FakeBinary binary(false, {
        { "_table_one_end", FakeBinary::Data + 0x40, N_SECT, 3 },
        { "_table_two_end", FakeBinary::Data + 0x48, N_SECT, 3 },
        { "_table_two_other", FakeBinary::Data + 0x50, N_SECT, 3 },
    });
    struct { GameImage Image; } fake;
    CHECK(fake.Image.OpenFile(binary.File));
    auto named = fake.Image.SymbolsNamed("_table_", "_end");
    CHECK(named.size() == 2 && named[0].VA == FakeBinary::Data + 0x40 && named[0].Name == "_table_one_end" && named[1].Name == "_table_two_end");
    CHECK(fake.Image.SymbolsNamed("_nothing", "").empty());

    auto text = fake.Image.SectionAt(FakeBinary::Text + 4);
    CHECK(text && std::string_view(text->Name) == "__text" && std::string_view(text->Segment) == "__TEXT" && text->Begin == FakeBinary::Text);
    auto data = fake.Image.SectionAt(FakeBinary::Data + 0x30);
    CHECK(data && std::string_view(data->Name) == "__const" && !data->ZeroFill && !data->Code);
    CHECK(!fake.Image.SectionAt(FakeBinary::Data + 0x900));
    CHECK(fake.Image.FromFile());
    CHECK(fake.Image.GotSlotOf("_longjmp") == 0);       // the file has no pointer slots
    CHECK(fake.Image.StubOf("_setjmp") == FakeBinary::Stubs && fake.Image.StubOf("_longjmp") == FakeBinary::Stubs + 12);
    CHECK(fake.Image.StubOf("_first") == 0);
}

TEST(lookups_work_on_a_loaded_dylib)
{
    void* lib = dlopen(BG3MSE_TEST_FUNCTIONS, RTLD_NOW | RTLD_LOCAL);
    CHECK(lib != nullptr);
    if (!lib) return;
    char const* path = BG3MSE_TEST_FUNCTIONS;
    char const* leaf = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    void const* header = FindLoadedImage(leaf);
    CHECK(header != nullptr);
    CHECK(FindLoadedImage("no such image.dylib") == nullptr);
    GameImage image;
    CHECK(header && image.OpenLoaded(header));
    CHECK(!image.FromFile());
    int unrelated = 0;
    GameImage none;
    CHECK(!none.OpenLoaded(&unrelated));
    if (!header) return;

    // A local symbol, which the dynamic loader does not give out, and an exported one.
    char const* names[] = { "_lib_local", "_lib_twice" };
    GameImage::Symbol found[2];
    image.Lookup(names, found);
    CHECK(found[0].Status == GameImage::SymbolStatus::Found && found[1].Status == GameImage::SymbolStatus::Found);
    CHECK(image.LoadedAt(found[1].VA) == dlsym(lib, "lib_twice"));
    auto section = image.SectionAt(found[0].VA);
    CHECK(section && section->Code);
}

// The game's rule, read in -[AppDelegate applicationDidFinishLaunching:]: any
// argument, the program's name included, equal to "-crash" in any case.
TEST(a_command_line_with_the_crash_argument_is_the_games_crash_reporter)
{
    auto reporter = [](std::vector<char const*> argv) {
        return IsCrashReporterCommandLine(static_cast<int>(argv.size()), argv.data());
    };
    char const* game = "/Applications/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3";
    CHECK(!reporter({ game }));
    CHECK(reporter({ game, "-crash" }));
    CHECK(reporter({ game, "-CRASH" }) && reporter({ game, "-Crash" }) && reporter({ game, "-cRaSh" }));
    CHECK(reporter({ game, "-modded", "-crash" }) && reporter({ game, "-crash", "-modded" }));
    CHECK(reporter({ "-crash" }));
    // Only the whole argument.
    CHECK(!reporter({ game, "-crashes" }) && !reporter({ game, "crash" }) && !reporter({ game, "--crash" }));
    CHECK(!reporter({ game, "-crash " }) && !reporter({ game, " -crash" }) && !reporter({ game, "-crash=1" }) && !reporter({ game, "" }));
    CHECK(!reporter({ game, "-modded", "-stats", "-storylog" }));
    // No arguments at all, and a list with a hole in it.
    CHECK(!reporter({}));
    CHECK(!IsCrashReporterCommandLine(3, nullptr));
    CHECK(!reporter({ game, nullptr }) && reporter({ game, nullptr, "-crash" }));
    // This test is not the game, whatever it was started with.
    CHECK(!IsGameProcess());
}

// The log's file, which the log keeps open: found by its path among this
// process's descriptors.
TEST(the_log_file_is_closed_in_a_program_the_process_starts)
{
    char const* tmp = getenv("TMPDIR");
    std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/bg3mse-platform-log.XXXXXX";
    CHECK(mkdtemp(dir.data()) != nullptr);
    std::string path = OpenLogFile(dir, "platform", 2);
    CHECK(!path.empty());
    char wanted[PATH_MAX];
    CHECK(realpath(path.c_str(), wanted) != nullptr);

    int descriptor = -1;
    for (int fd = 0; fd < 1024 && descriptor < 0; fd++) {
        char open[PATH_MAX];
        if (fcntl(fd, F_GETPATH, open) == 0 && strcmp(open, wanted) == 0) descriptor = fd;
    }
    CHECK(descriptor >= 0);
    CHECK(descriptor >= 0 && (fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0);

    // The tests go on without a log.
    SetLogStream(nullptr);
    if (descriptor >= 0) close(descriptor);
    unlink(path.c_str());
    unlink((dir + "/platform_latest.log").c_str());
    rmdir(dir.c_str());
}

// Only the path is asked for: nothing is made or read there.
TEST(the_data_directory_is_in_the_users_home_with_or_without_HOME)
{
    char const* was = getenv("HOME");
    std::string saved = was ? was : "";
    constexpr char kTail[] = "/Library/Application Support/bg3mse";

    setenv("HOME", "/Users/somebody", 1);
    CHECK(DataDirectory() == std::string("/Users/somebody") + kTail);

    // Without HOME, and with one that is no path: the user's home as the
    // system has it, not the machine's /Library.
    passwd* user = getpwuid(getuid());
    CHECK(user && user->pw_dir && user->pw_dir[0] == '/');
    std::string expected = std::string(user && user->pw_dir ? user->pw_dir : "") + kTail;
    for (char const* home : { static_cast<char const*>(nullptr), "", "relative/home" }) {
        if (home) {
            setenv("HOME", home, 1);
        } else {
            unsetenv("HOME");
        }
        std::string directory = DataDirectory();
        CHECK(directory == expected);
        CHECK(directory != kTail && directory.rfind("/Library/", 0) != 0);
    }

    if (was) {
        setenv("HOME", saved.c_str(), 1);
    } else {
        unsetenv("HOME");
    }
}


// The sink is given each line's level and text, once; the echo writes each
// line to stderr as well.
std::vector<std::pair<LogLevel, std::string>> gSunk;

TEST(the_log_gives_every_line_to_its_sink_and_echoes_it_to_stderr)
{
    char const* tmp = getenv("TMPDIR");
    std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/bg3mse-platform-sink.XXXXXX";
    CHECK(mkdtemp(dir.data()) != nullptr);
    std::string file = dir + "/stream", echoed = dir + "/stderr";
    FILE* stream = fopen(file.c_str(), "w");
    SetLogStream(stream);
    gSunk.clear();
    SetLogSink([](LogLevel level, char const* text) {
        gSunk.emplace_back(level, text);
        Log(LogLevel::Info, "from the sink");    // written, and not given back
    });
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    int to = open(echoed.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(to, STDERR_FILENO);
    SetLogEcho(true);
    Log(LogLevel::Warning, "one %d", 1);
    SetLogEcho(false);
    Log(LogLevel::Debug, "dropped");
    Log(LogLevel::Error, "%s", std::string(2000, 'x').c_str());
    SetLogSink(nullptr);
    Log(LogLevel::Info, "after");
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    close(to);
    SetLogStream(nullptr);
    fclose(stream);

    CHECK(gSunk.size() == 2);
    CHECK(gSunk.size() == 2 && gSunk[0].first == LogLevel::Warning && gSunk[0].second == "one 1");
    CHECK(gSunk.size() == 2 && gSunk[1].first == LogLevel::Error && gSunk[1].second == std::string(2000, 'x'));
    auto read = [](std::string const& path) {
        std::string text;
        if (FILE* f = fopen(path.c_str(), "r")) {
            char buffer[4096];
            size_t n;
            while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, n);
            fclose(f);
        }
        return text;
    };
    std::string written = read(file), copy = read(echoed);
    CHECK(written.find("[WARN ] one 1\n") != std::string::npos && written.find("[INFO ] from the sink\n") != std::string::npos);
    CHECK(written.find("dropped") == std::string::npos && written.find("[INFO ] after\n") != std::string::npos);
    // Only what was logged while the echo was on.
    CHECK(copy.find("[WARN ] one 1\n") != std::string::npos && copy.find("xxxx") == std::string::npos);
    unlink(file.c_str());
    unlink(echoed.c_str());
    rmdir(dir.c_str());
}

// Under the locale a German Mac gives the game, a number is logged with a
// point, as Norbyte's lines are on Windows ("took %.2f ms").
TEST(the_log_writes_numbers_in_the_c_locale)
{
    gSunk.clear();
    SetLogSink([](LogLevel level, char const* text) { gSunk.emplace_back(level, text); });
    std::string before = setlocale(LC_ALL, nullptr);
    CHECK(setlocale(LC_ALL, "de_DE.UTF-8") != nullptr);
    Log(LogLevel::Info, "took %.2f ms", 1.5);
    setlocale(LC_ALL, before.c_str());
    SetLogSink(nullptr);
    CHECK(gSunk.size() == 1 && gSunk[0].second == "took 1.50 ms");
}

// The updater opens the extender with dlopen; the game's load command and
// DYLD_INSERT_LIBRARIES load it without the updater, which then loads
// nothing (ScriptExtender::Initialize warns, as WarnIfOffline does).
extern "C" int bg3mse_test_image_function(void);

TEST(an_image_is_loaded_through_the_updater_only_when_the_updater_opened_it)
{
    // Named by this executable: never through the updater.
    void const* named = reinterpret_cast<void const*>(&bg3mse_test_image_function);
    CHECK(!LoadedThroughUpdater(named));

    void* opened = dlopen(BG3MSE_TEST_OPENED_IMAGE, RTLD_NOW | RTLD_LOCAL);
    CHECK(opened != nullptr);
    void const* inOpened = opened ? dlsym(opened, "bg3mse_test_image_function") : nullptr;
    CHECK(inOpened != nullptr && inOpened != named);

    // Opened, but no updater is loaded.
    CHECK(!LoadedThroughUpdater(inOpened));

    void* updater = dlopen(BG3MSE_TEST_UPDATER_IMAGE, RTLD_NOW | RTLD_LOCAL);
    CHECK(updater != nullptr);
    CHECK(LoadedThroughUpdater(inOpened));
    CHECK(!LoadedThroughUpdater(named));

    // Named in DYLD_INSERT_LIBRARIES (as the launch option names it).
    char const* was = getenv("DYLD_INSERT_LIBRARIES");
    std::string saved = was ? was : "";
    setenv("DYLD_INSERT_LIBRARIES", (std::string("/nowhere/libx.dylib:") + BG3MSE_TEST_OPENED_IMAGE).c_str(), 1);
    CHECK(!LoadedThroughUpdater(inOpened));
    if (was) {
        setenv("DYLD_INSERT_LIBRARIES", saved.c_str(), 1);
    } else {
        unsetenv("DYLD_INSERT_LIBRARIES");
    }
    CHECK(LoadedThroughUpdater(inOpened));

    // An address in no image.
    CHECK(!LoadedThroughUpdater(nullptr));
}

int main()
{
    SetLogStream(nullptr);
    void (*tests[])() = {
        writer_writes_one_word_and_a_block_run,
        writer_patches_this_executables_own_code_run,
        writer_refuses_a_write_across_two_pages_run,
        writer_refuses_data_unmapped_and_misaligned_addresses_run,
        writer_writes_a_jit_page_through_the_write_gate_run,
        pointer_stores_into_read_only_data_keep_its_protection_run,
        header_padding_runs_from_the_commands_to_the_first_section_run,
        islands_come_from_zero_bytes_and_never_cross_a_page_run,
        islands_fall_back_to_a_page_mapped_near_the_code_run,
        islands_fall_back_to_a_jit_page_run,
        jump_pad_reaches_its_target_run,
        planner_relocates_each_kind_of_first_instruction_run,
        planner_refuses_what_it_cannot_move_run,
        planner_respects_branch_range_run,
        entry_hook_on_a_function_starting_with_adrp_run,
        entry_hook_on_a_function_starting_with_cbz_run,
        entry_hook_on_a_function_starting_with_a_literal_load_run,
        entry_hook_refuses_a_loop_head_and_changes_nothing_run,
        entry_hook_on_a_compiled_function_run,
        entry_hooks_outside_any_image_use_a_page_mapped_near_them_run,
        entry_hook_goes_in_and_out_while_threads_run_the_function_run,
        function_extents_from_the_symbol_table_of_an_image_without_function_starts_run,
        function_extents_from_the_function_starts_of_a_loaded_dylib_run,
        function_extents_of_this_executable_run,
        function_extents_from_a_file_agree_with_the_loaded_image_run,
        lookups_tell_missing_from_ambiguous_run,
        lookups_list_symbols_and_sections_run,
        lookups_work_on_a_loaded_dylib_run,
        a_command_line_with_the_crash_argument_is_the_games_crash_reporter_run,
        the_log_file_is_closed_in_a_program_the_process_starts_run,
        the_data_directory_is_in_the_users_home_with_or_without_HOME_run,
        an_image_is_loaded_through_the_updater_only_when_the_updater_opened_it_run,
        the_log_gives_every_line_to_its_sink_and_echoes_it_to_stderr_run,
        the_log_writes_numbers_in_the_c_locale_run,
    };
    for (auto test : tests) test();
    printf("%zu tests, %d failures\n", std::size(tests), gFailures);
    return gFailures ? 1 : 0;
}
