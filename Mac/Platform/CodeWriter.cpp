#include <Platform/CodeWriter.h>

#include <algorithm>
#include <cstring>
#include <vector>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>

namespace bg3se::mac {

namespace {

// Constant-initialised: a dylib constructor may write code before this file's
// dynamic initialisers have run.
constinit std::mutex gCodeLock;

struct Region
{
    mach_vm_address_t Start{ 0 };
    mach_vm_size_t Size{ 0 };
    vm_region_basic_info_data_64_t Info{};
};

// The region that holds `address`; false when `address` is not mapped.
bool RegionAt(uintptr_t address, Region& out)
{
    mach_vm_address_t start = address;
    mach_vm_size_t size = 0;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64,
        reinterpret_cast<vm_region_info_t>(&out.Info), &count, &object);
    if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
    // For an unmapped address the kernel answers with the next region above it.
    if (kr != KERN_SUCCESS || start > address || address - start >= size) return false;
    out.Start = start;
    out.Size = size;
    return true;
}

std::string Describe(char const* what, void const* at, kern_return_t kr)
{
    char text[160];
    snprintf(text, sizeof(text), "%s at %p failed (%s)", what, at, mach_error_string(kr));
    return text;
}

std::string Describe(char const* what, void const* at)
{
    char text[160];
    snprintf(text, sizeof(text), "%s at %p", what, at);
    return text;
}

// A MAP_JIT page: written in place, with this thread's JIT write gate open.
// Other threads keep their execute view of the page throughout. Only what
// changes is stored: each aligned 8 bytes of the write as one store, a word
// left over at either end as one store, so no reader sees a value half written.
std::string EditJitPage(uintptr_t page, uintptr_t at, size_t bytes, PageEdit edit, void* context)
{
    uintptr_t pageSize = CodePageSize();
    mach_vm_address_t scratch = 0;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &scratch, pageSize, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) return Describe("allocating a scratch page", (void const*)at, kr);
    auto copy = reinterpret_cast<uint8_t*>(scratch);
    memcpy(copy, reinterpret_cast<void const*>(page), pageSize);
    std::string problem = edit(copy + (at - page), context);
    if (problem.empty()) {
        pthread_jit_write_protect_np(0);
        for (uintptr_t word = at; word < at + bytes;) {
            if (!(word & 7) && word + 8 <= at + bytes) {
                uint64_t value;
                memcpy(&value, copy + (word - page), sizeof(value));
                auto live = reinterpret_cast<uint64_t*>(word);
                if (__atomic_load_n(live, __ATOMIC_RELAXED) != value) __atomic_store_n(live, value, __ATOMIC_RELEASE);
                word += 8;
                continue;
            }
            uint32_t value;
            memcpy(&value, copy + (word - page), sizeof(value));
            auto live = reinterpret_cast<uint32_t*>(word);
            if (__atomic_load_n(live, __ATOMIC_RELAXED) != value) __atomic_store_n(live, value, __ATOMIC_RELEASE);
            word += 4;
        }
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(reinterpret_cast<void*>(at), bytes);
    }
    mach_vm_deallocate(mach_task_self(), scratch, pageSize);
    return problem;
}

// An r-x page: a patched copy is mapped over it.
std::string SwapPage(uintptr_t page, Region const& original, uintptr_t at, size_t bytes, PageEdit edit, void* context)
{
    uintptr_t pageSize = CodePageSize();
    auto const& info = original.Info;
    mach_vm_address_t scratch = 0;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &scratch, pageSize, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) return Describe("allocating a copy of the page", (void const*)at, kr);
    auto release = [&] { mach_vm_deallocate(mach_task_self(), scratch, pageSize); };

    auto copy = reinterpret_cast<uint8_t*>(scratch);
    memcpy(copy, reinterpret_cast<void const*>(page), pageSize);
    if (std::string problem = edit(copy + (at - page), context); !problem.empty()) {
        release();
        return problem;
    }
    std::vector<uint8_t> expected(copy + (at - page), copy + (at - page) + bytes);
    // The copy gets the original's maximum and current protection. A fresh
    // allocation's maximum is already everything; asking for that again is
    // refused, so it is only ever lowered.
    kr = KERN_SUCCESS;
    if (info.max_protection != VM_PROT_ALL) kr = mach_vm_protect(mach_task_self(), scratch, pageSize, TRUE, info.max_protection);
    if (kr == KERN_SUCCESS) kr = mach_vm_protect(mach_task_self(), scratch, pageSize, FALSE, info.protection);
    if (kr != KERN_SUCCESS) {
        release();
        return Describe("protecting the copy of the page", (void const*)at, kr);
    }
    sys_icache_invalidate(copy, pageSize);

    mach_vm_address_t target = page;
    vm_prot_t current = VM_PROT_NONE, maximum = VM_PROT_NONE;
    kr = mach_vm_remap(mach_task_self(), &target, pageSize, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), scratch,
        FALSE, &current, &maximum, info.inheritance);
    release();
    if (kr != KERN_SUCCESS) return Describe("mapping the patched page", (void const*)at, kr);
    sys_icache_invalidate(reinterpret_cast<void*>(page), pageSize);

    Region swapped;
    if (target != page || current != info.protection || !RegionAt(page, swapped) || swapped.Info.protection != info.protection ||
        swapped.Info.max_protection != info.max_protection || swapped.Info.inheritance != info.inheritance) {
        return Describe("the patched page does not have the original's protection and inheritance", (void const*)at);
    }
    if (memcmp(reinterpret_cast<void const*>(at), expected.data(), bytes) != 0) {
        return Describe("the patched page does not read back as written", (void const*)at);
    }
    return {};
}

std::string CheckWordAddress(void* at)
{
    if (!at || (reinterpret_cast<uintptr_t>(at) & 3)) return Describe("an instruction word must be aligned: refused", at);
    return {};
}

struct WordsEdit
{
    std::span<WordPatch const> Patches;
    uintptr_t First;
};

std::string ApplyWords(uint8_t* copyOfFirst, void* context)
{
    auto const& e = *static_cast<WordsEdit const*>(context);
    for (auto const& p : e.Patches) {
        memcpy(copyOfFirst + (reinterpret_cast<uintptr_t>(p.At) - e.First), &p.Word, sizeof(p.Word));
    }
    return {};
}

std::string ApplyBlock(uint8_t* copyOfAt, void* context)
{
    auto const& words = *static_cast<std::span<uint32_t const> const*>(context);
    memcpy(copyOfAt, words.data(), words.size_bytes());
    return {};
}

}

std::mutex& CodeLock()
{
    return gCodeLock;
}

uintptr_t CodePageSize()
{
    return vm_page_size;
}

PageInfo QueryPage(uintptr_t address)
{
    Region region;
    if (!RegionAt(address, region)) return {};
    auto prot = region.Info.protection;
    return { true, (prot & VM_PROT_READ) != 0, (prot & VM_PROT_WRITE) != 0, (prot & VM_PROT_EXECUTE) != 0 };
}

std::string EditCodeLocked(void* at, size_t bytes, PageEdit edit, void* context)
{
    auto start = reinterpret_cast<uintptr_t>(at);
    if (!at || !bytes || (start & 3) || (bytes & 3)) return Describe("a code write must cover whole aligned words: refused", at);
    uintptr_t page = start & ~(CodePageSize() - 1);
    if (bytes > CodePageSize() || ((start + bytes - 1) & ~(CodePageSize() - 1)) != page) {
        return Describe("a code write spans two pages: refused", at);
    }
    Region region;
    if (!RegionAt(page, region)) return Describe("no memory is mapped", at);
    auto prot = region.Info.protection;
    if ((prot & (VM_PROT_READ | VM_PROT_EXECUTE)) != (VM_PROT_READ | VM_PROT_EXECUTE)) {
        return Describe("the memory is not readable and executable code", at);
    }
    if (prot & VM_PROT_WRITE) return EditJitPage(page, start, bytes, edit, context);
    return SwapPage(page, region, start, bytes, edit, context);
}

std::string WriteCodeWord(void* at, uint32_t word)
{
    WordPatch patch{ at, word };
    return WriteCodeWords({ &patch, 1 });
}

std::string WriteCodeWords(std::span<WordPatch const> patches)
{
    std::lock_guard lock(CodeLock());
    return WriteCodeWordsLocked(patches);
}

std::string WriteCodeWordsLocked(std::span<WordPatch const> patches)
{
    if (patches.empty()) return {};
    uintptr_t first = UINTPTR_MAX, last = 0;
    for (auto const& p : patches) {
        if (auto problem = CheckWordAddress(p.At); !problem.empty()) return problem;
        auto a = reinterpret_cast<uintptr_t>(p.At);
        first = std::min(first, a);
        last = std::max(last, a);
    }
    WordsEdit edit{ patches, first };
    return EditCodeLocked(reinterpret_cast<void*>(first), last + 4 - first, ApplyWords, &edit);
}

std::string WriteCodeBlock(void* at, std::span<uint32_t const> words, BlockContract)
{
    if (words.empty()) return {};
    std::lock_guard lock(CodeLock());
    return EditCodeLocked(at, words.size_bytes(), ApplyBlock, &words);
}

std::string StorePointer(void* at, void const* value)
{
    auto address = reinterpret_cast<uintptr_t>(at);
    if (!at || (address & (sizeof(void*) - 1))) return Describe("a pointer store must be aligned: refused", at);
    std::lock_guard lock(CodeLock());
    Region region;
    if (!RegionAt(address, region)) return Describe("no memory is mapped", at);
    if (region.Info.protection & VM_PROT_EXECUTE) return Describe("the memory is code, not data", at);

    uintptr_t page = address & ~(CodePageSize() - 1);
    kern_return_t kr = mach_vm_protect(mach_task_self(), page, CodePageSize(), FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) return Describe("making the page writable", at, kr);
    __atomic_store_n(static_cast<void const**>(at), value, __ATOMIC_RELEASE);
    kr = mach_vm_protect(mach_task_self(), page, CodePageSize(), FALSE, region.Info.protection);
    if (kr != KERN_SUCCESS) return Describe("giving the page back its protection", at, kr);
    return {};
}

}
