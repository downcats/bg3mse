// Tests of the platform layer's fault handling. No game needed.

#include <Platform/FaultCapture.h>
#include <Platform/FaultGuard.h>
#include <Platform/FaultObserver.h>
#include <Platform/FaultRecord.h>
#include <Platform/FaultServer.h>
#include <Platform/Log.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_images.h>
#include <spawn.h>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <memory>
#include <pthread.h>
#include <csetjmp>
#include <string_view>
#include <unistd.h>
#include <vector>

using namespace bg3se::mac;

extern char** environ;

namespace {

int gFailures = 0;
char const* gTest = "";

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; name(); } static void name()

bool SymbolIs(FaultFrame const& frame, std::string_view name)
{
    FrameSymbol symbol{};
    return SymbolizeFrame(frame, symbol) && std::string_view(symbol.Name) == name;
}

// A thread stopped three calls deep, for capturing a record of it.
std::atomic<int> gChainReady{ 0 };
std::atomic<int> gChainRelease{ 0 };

}

extern "C" __attribute__((noinline)) void FaultTestChainC()
{
    gChainReady = 1;
    while (!gChainRelease) {
        usleep(1000);
    }
    __asm__ volatile("");
}

extern "C" __attribute__((noinline)) void FaultTestChainB()
{
    FaultTestChainC();
    __asm__ volatile("");
}

extern "C" __attribute__((noinline)) void FaultTestChainA()
{
    FaultTestChainB();
    __asm__ volatile("");
}

namespace {

void* ChainThread(void*)
{
    FaultTestChainA();
    return nullptr;
}

TEST(read_memory_fails_on_unmapped_memory_instead_of_faulting)
{
    uint64_t value = 0x1234;
    uint64_t copy = 0;
    CHECK(ReadMemory((uint64_t)&value, &copy, sizeof(copy)) && copy == 0x1234);
    CHECK(!ReadMemory(0, &copy, sizeof(copy)));
    CHECK(!ReadMemory(8, &copy, sizeof(copy)));
}

TEST(image_table_lists_this_executable_and_the_system_libraries)
{
    auto table = std::make_unique<ImageTable>();
    CHECK(table->Read());
    CHECK(table->Count() > 10);
    auto self = table->Find((uint64_t)&FaultTestChainA);
    CHECK(self && self->MainExecutable && self->Base == ThisImageBase());
    CHECK(self && self->Path && std::string_view(self->Path).ends_with("bg3mse_fault_tests"));
    auto libc = table->Find((uint64_t)&getpid);
    CHECK(libc && !libc->MainExecutable && libc->Path && std::string_view(libc->Path).starts_with("/usr/lib/"));
    CHECK(!table->Find(0));
    // dyld itself, which dyld's own list leaves out.
    Dl_info info{};
    CHECK(dladdr(reinterpret_cast<void const*>(&dlopen), &info));
    task_dyld_info_data_t dyldInfo{};
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    CHECK(task_info(mach_task_self(), TASK_DYLD_INFO, reinterpret_cast<task_info_t>(&dyldInfo), &count) == KERN_SUCCESS);
    auto all = reinterpret_cast<dyld_all_image_infos const*>(dyldInfo.all_image_info_addr);
    auto dyld = table->Find(reinterpret_cast<uint64_t>(all->dyldImageLoadAddress) + 0x1000);
    CHECK(dyld && dyld->Base == reinterpret_cast<uint64_t>(all->dyldImageLoadAddress));
    CHECK(dyld && dyld->Path && std::string_view(dyld->Path) == "/usr/lib/dyld");
}

TEST(capture_of_a_stopped_thread_walks_its_frames_and_names_their_images)
{
    gChainReady = 0;
    gChainRelease = 0;
    pthread_t thread;
    CHECK(pthread_create(&thread, nullptr, ChainThread, nullptr) == 0);
    while (!gChainReady) {
        usleep(1000);
    }
    mach_port_t port = pthread_mach_thread_np(thread);
    CHECK(thread_suspend(port) == KERN_SUCCESS);
    arm_thread_state64_t state{};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    CHECK(thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&state, &count) == KERN_SUCCESS);

    FaultSource source{};
    source.MachException = EXC_BAD_ACCESS;
    source.Code[0] = KERN_INVALID_ADDRESS;
    source.Code[1] = 0x10;
    source.State = &state;
    source.Thread = port;
    source.ThreadId = 77;
    source.Origin = FaultOrigin::Unguarded;
    auto capture = std::make_unique<FaultCapture>();
    auto record = std::make_unique<FaultRecord>();
    capture->Capture(source, *record);
    CHECK(thread_resume(port) == KERN_SUCCESS);
    gChainRelease = 1;
    pthread_join(thread, nullptr);

    CHECK(record->Kind == FaultKind::BadAccess);
    CHECK(record->Signal == SIGSEGV);
    CHECK(record->Address == 0x10);
    CHECK(record->PC == arm_thread_state64_get_pc(state));
    CHECK(record->ThreadId == 77 && record->Thread == port);
    CHECK(record->FrameCount >= 4);

    // The thread is in FaultTestChainC or a system call under it (the last
    // frame named FaultTestChainC is its saved return address); its callers
    // follow in order.
    int c = -1, b = -1, a = -1;
    for (uint32_t i = 0; i < record->FrameCount; i++) {
        auto const& frame = record->Frames[i];
        if (SymbolIs(frame, "_FaultTestChainC")) c = (int)i;
        if (SymbolIs(frame, "_FaultTestChainB")) b = (int)i;
        if (SymbolIs(frame, "_FaultTestChainA")) a = (int)i;
    }
    CHECK(c >= 0 && b == c + 1 && a == b + 1);
    for (int i : { a, b, c }) {
        if (i < 0) continue;
        CHECK(record->Frames[i].Flags & FrameInMainExecutable);
        CHECK(record->Frames[i].Flags & FrameInThisImage);
        CHECK(record->Frames[i].ImageBase == ThisImageBase());
    }
    if (!record->Frames[0].ImageBase) {
        Dl_info info{};
        dladdr(reinterpret_cast<void const*>(record->PC), &info);
        printf("  frame 0 at 0x%llx is in no image; dladdr says %s %s\n", (unsigned long long)record->PC,
            info.dli_fname ? info.dli_fname : "-", info.dli_sname ? info.dli_sname : "-");
    }
    CHECK(record->Frames[0].ImageBase != 0);
}

TEST(a_bad_access_below_a_stack_with_the_stack_pointer_at_its_bottom_is_a_stack_overflow)
{
    arm_thread_state64_t state{};
    uint64_t low = 0x170000000, high = 0x170080000;
    arm_thread_state64_set_sp(state, low + 0x40);
    FaultSource source{};
    source.MachException = EXC_BAD_ACCESS;
    source.Code[0] = KERN_PROTECTION_FAILURE;
    source.Code[1] = (int64_t)(low - 0x20);
    source.State = &state;
    source.StackLow = low;
    source.StackHigh = high;
    auto capture = std::make_unique<FaultCapture>();
    auto record = std::make_unique<FaultRecord>();
    capture->Capture(source, *record);
    CHECK(record->Kind == FaultKind::StackOverflow);
    CHECK(record->Signal == SIGBUS);
    CHECK(record->StackLow == low && record->StackHigh == high);

    // The same address with the stack pointer high up is a wild access.
    arm_thread_state64_set_sp(state, high - 0x100);
    capture->Capture(source, *record);
    CHECK(record->Kind == FaultKind::BadAccess);
}

TEST(signals_map_to_kinds_and_addresses)
{
    arm_thread_state64_t state{};
    FaultSource source{};
    source.State = &state;
    source.Signal = SIGILL;
    source.SignalCode = ILL_ILLOPC;
    auto capture = std::make_unique<FaultCapture>();
    auto record = std::make_unique<FaultRecord>();
    capture->Capture(source, *record);
    CHECK(record->Kind == FaultKind::IllegalInstruction && record->Signal == SIGILL && record->MachException == 0);
    CHECK(record->Code[0] == ILL_ILLOPC);
    source.Signal = SIGABRT;
    capture->Capture(source, *record);
    CHECK(record->Kind == FaultKind::Abort);
    source.Signal = SIGBUS;
    source.SignalAddress = 0x4000;
    capture->Capture(source, *record);
    CHECK(record->Kind == FaultKind::BadAccess && record->Address == 0x4000);
}

TEST(symbols_come_from_each_images_own_table)
{
    FaultFrame frame{ (uint64_t)&FaultTestChainB + 8, ThisImageBase(), nullptr, 0 };
    FrameSymbol symbol{};
    CHECK(SymbolizeFrame(frame, symbol));
    CHECK(std::string_view(symbol.Name) == "_FaultTestChainB" && symbol.Offset == 8);

    auto table = std::make_unique<ImageTable>();
    CHECK(table->Read());
    auto libc = table->Find((uint64_t)&getpid);
    CHECK(libc != nullptr);
    if (libc) {
        FaultFrame system{ (uint64_t)&getpid, libc->Base, libc->Path, 0 };
        // The same function dladdr names (without the leading underscore).
        Dl_info info{};
        CHECK(dladdr((void const*)&getpid, &info) && info.dli_sname);
        CHECK(SymbolizeFrame(system, symbol));
        CHECK(info.dli_sname && std::string_view(symbol.Name).substr(1) == info.dli_sname && symbol.Offset == 0);
    }

    FaultFrame nowhere{ 0x10, 0, nullptr, 0 };
    CHECK(!SymbolizeFrame(nowhere, symbol));
}

// SymbolizeFrames names many frames as SymbolizeFrame names each: addresses
// all through this image's code and through two of the system's, in no
// order, one outside every image, and with a lookup address for some.
TEST(many_frames_are_named_as_each_one_is)
{
    auto table = std::make_unique<ImageTable>();
    CHECK(table->Read());
    std::vector<FaultFrame> frames;
    auto add = [&](uint64_t pc) {
        auto image = table->Find(pc);
        frames.push_back({ pc, image ? image->Base : 0, image ? image->Path : nullptr, 0 });
    };
    for (auto base : { (uint64_t)&FaultTestChainB, (uint64_t)&getpid, (uint64_t)&pthread_create }) {
        auto image = table->Find(base);
        CHECK(image != nullptr);
        if (!image) continue;
        uint64_t step = (image->End - image->Base) / 300;
        for (uint64_t pc = image->End - 4; pc > image->Base && pc - image->Base > step; pc -= step) add(pc & ~3ull);
        add(base);
    }
    add(0x10);
    std::vector<uint64_t> lookup(frames.size());
    for (size_t i = 0; i < frames.size(); i++) lookup[i] = i % 3 == 0 ? frames[i].PC - 1 : frames[i].PC;

    for (bool withLookup : { false, true }) {
        std::vector<FrameSymbol> symbols(frames.size());
        std::vector<uint32_t> order(frames.size());
        std::vector<void const*> best(frames.size());
        auto found = std::make_unique<bool[]>(frames.size());
        size_t named = SymbolizeFrames(frames.data(), withLookup ? lookup.data() : nullptr, (uint32_t)frames.size(), symbols.data(),
            found.get(), order.data(), best.data(), 0);
        size_t expectedNamed = 0, differ = 0;
        for (size_t i = 0; i < frames.size(); i++) {
            FaultFrame looked = frames[i];
            if (withLookup) looked.PC = lookup[i];
            FrameSymbol one{};
            bool ok = SymbolizeFrame(looked, one);
            expectedNamed += ok;
            if (ok) one.Offset += frames[i].PC - looked.PC;
            if (ok != found[i] || (ok && (strcmp(one.Name, symbols[i].Name) != 0 || one.Offset != symbols[i].Offset))) differ++;
        }
        CHECK(differ == 0 && named == expectedNamed && expectedNamed > 600);
    }
}

}

// Guarded calls. Every case that faults runs in a child process (this
// executable started again with "--child <case>"), so that a guard that fails
// to recover takes down only the child; the parent checks how the child ended
// and what it wrote to fd 3.

extern "C" {
uint64_t fault_test_read_null();
void fault_test_write(void* address);
void fault_test_undefined();
void fault_test_trap();
void fault_test_recurse();
}

namespace {

constexpr exception_mask_t kFaultMask = EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC | EXC_MASK_BREAKPOINT;

// What a child writes for its parent.
void Say(char const* fmt, ...) __attribute__((format(printf, 1, 2)));
void Say(char const* fmt, ...)
{
    char line[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((size_t)n > sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    write(3, line, (size_t)n);
}

struct ChildResult
{
    bool Ran{ false };
    bool Hung{ false };
    int Status{ 0 };
    std::string Output;

    bool Exited(int code) const { return Ran && !Hung && WIFEXITED(Status) && WEXITSTATUS(Status) == code; }
    bool KilledBy(int signal) const { return Ran && !Hung && WIFSIGNALED(Status) && WTERMSIG(Status) == signal; }
    bool Said(std::string_view line) const { return Output.find(std::string(line) + "\n") != std::string::npos; }
    int Count(std::string_view line) const
    {
        int count = 0;
        std::string wanted = std::string(line) + "\n";
        for (size_t at = Output.find(wanted); at != std::string::npos; at = Output.find(wanted, at + 1)) count++;
        return count;
    }
};

std::string gSelf;

ChildResult RunChild(char const* name)
{
    ChildResult result;
    int fds[2];
    if (pipe(fds) != 0) return result;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // The read end is closed first: it may be fd 3 itself.
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 3);
    char* argv[] = { gSelf.data(), const_cast<char*>("--child"), const_cast<char*>(name), nullptr };
    pid_t pid = 0;
    int rc = posix_spawn(&pid, gSelf.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return result;
    }
    result.Ran = true;
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        char buffer[4096];
        ssize_t n;
        while ((n = read(fds[0], buffer, sizeof(buffer))) > 0) result.Output.append(buffer, (size_t)n);
        pid_t done = waitpid(pid, &result.Status, WNOHANG);
        if (done == pid) break;
        if (std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &result.Status, 0);
            result.Hung = true;
            break;
        }
        usleep(5000);
    }
    char buffer[4096];
    ssize_t n;
    while ((n = read(fds[0], buffer, sizeof(buffer))) > 0) result.Output.append(buffer, (size_t)n);
    close(fds[0]);
    if (result.Hung) printf("  child %s hung\n", name);
    return result;
}

// The thread-level port for kFaultMask, or MACH_PORT_NULL.
mach_port_t ThreadPort()
{
    exception_mask_t masks[EXC_TYPES_COUNT];
    mach_msg_type_number_t count = EXC_TYPES_COUNT;
    exception_handler_t ports[EXC_TYPES_COUNT];
    exception_behavior_t behaviors[EXC_TYPES_COUNT];
    thread_state_flavor_t flavors[EXC_TYPES_COUNT];
    if (thread_get_exception_ports(pthread_mach_thread_np(pthread_self()), kFaultMask, masks, &count, ports, behaviors, flavors) != KERN_SUCCESS) {
        return MACH_PORT_DEAD;
    }
    mach_port_t found = MACH_PORT_NULL;
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        if (MACH_PORT_VALID(ports[i])) {
            found = ports[i];
            mach_port_deallocate(mach_task_self(), ports[i]);
        }
    }
    return found;
}

size_t PortNameCount()
{
    mach_port_name_array_t names = nullptr;
    mach_msg_type_number_t nameCount = 0;
    mach_port_type_array_t types = nullptr;
    mach_msg_type_number_t typeCount = 0;
    if (mach_port_names(mach_task_self(), &names, &nameCount, &types, &typeCount) != KERN_SUCCESS) return 0;
    vm_deallocate(mach_task_self(), (vm_address_t)names, nameCount * sizeof(*names));
    vm_deallocate(mach_task_self(), (vm_address_t)types, typeCount * sizeof(*types));
    return nameCount;
}

FaultDecision Decline(FaultRecord const&, void*)
{
    return FaultDecision::ContinueSearch;
}

FaultDecision CountAndRecover(FaultRecord const&, void* context)
{
    ++*static_cast<int*>(context);
    return FaultDecision::Recover;
}

FaultDecision CountAndDecline(FaultRecord const&, void* context)
{
    ++*static_cast<int*>(context);
    return FaultDecision::ContinueSearch;
}

void* MapPage(int prot)
{
    void* page = mmap(nullptr, (size_t)getpagesize(), PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (page == MAP_FAILED) return nullptr;
    memset(page, 0, (size_t)getpagesize());
    mprotect(page, (size_t)getpagesize(), prot);
    return page;
}

// --- Cases run in a child. Exit 0 means every check in it held. ---

int gChildFailures = 0;
#define CHILD_CHECK(cond) do { if (!(cond)) { Say("FAIL %s (line %d)", #cond, __LINE__); gChildFailures++; } } while (0)

int ChildNullRead()
{
    auto fault = std::make_unique<FaultRecord>();
    bool completed = GuardedCall([] { fault_test_read_null(); }, nullptr, nullptr, fault.get());
    CHILD_CHECK(!completed);
    CHILD_CHECK(fault->Kind == FaultKind::BadAccess && fault->Signal == SIGSEGV && fault->MachException == EXC_BAD_ACCESS);
    CHILD_CHECK(fault->Code[0] == KERN_INVALID_ADDRESS && fault->Address == 0x10);
    CHILD_CHECK(fault->Origin == FaultOrigin::Guarded);
    uint64_t self = 0;
    pthread_threadid_np(nullptr, &self);
    CHILD_CHECK(fault->ThreadId == self && fault->Thread == pthread_mach_thread_np(pthread_self()));
    CHILD_CHECK(fault->PC == (uint64_t)&fault_test_read_null + 4);
    CHILD_CHECK(fault->FrameCount >= 3 && SymbolIs(fault->Frames[0], "_fault_test_read_null"));
    CHILD_CHECK(fault->Frames[0].Flags & FrameInMainExecutable);
    CHILD_CHECK(fault->Frames[1].Flags & FrameFromLinkRegister);
    CHILD_CHECK(ThreadPort() == MACH_PORT_NULL);
    // The thread carries on, and guards keep working.
    CHILD_CHECK(!GuardedCall([] { fault_test_read_null(); }));
    CHILD_CHECK(GuardedCall([] {}));
    return gChildFailures;
}

int ChildReadOnlyWrite()
{
    void* page = MapPage(PROT_READ);
    auto fault = std::make_unique<FaultRecord>();
    CHILD_CHECK(!GuardedCall([&] { fault_test_write(page); }, nullptr, nullptr, fault.get()));
    CHILD_CHECK(fault->Kind == FaultKind::BadAccess && fault->Signal == SIGBUS);
    CHILD_CHECK(fault->Code[0] == KERN_PROTECTION_FAILURE && fault->Address == (uint64_t)page);
    CHILD_CHECK(SymbolIs(fault->Frames[0], "_fault_test_write"));
    return gChildFailures;
}

int ChildJumpToData()
{
    void* page = MapPage(PROT_READ | PROT_WRITE);
    auto fault = std::make_unique<FaultRecord>();
    auto jump = reinterpret_cast<void (*)()>(page);
    CHILD_CHECK(!GuardedCall([&] { jump(); }, nullptr, nullptr, fault.get()));
    CHILD_CHECK(fault->Kind == FaultKind::BadAccess);
    CHILD_CHECK(fault->PC == (uint64_t)page && fault->Address == (uint64_t)page);
    // The jump's caller is known only from the link register.
    CHILD_CHECK(fault->FrameCount >= 2 && fault->Frames[0].ImageBase == 0);
    CHILD_CHECK((fault->Frames[1].Flags & FrameFromLinkRegister) && (fault->Frames[1].Flags & FrameInThisImage));
    return gChildFailures;
}

int ChildNullCall()
{
    // A call through a null function pointer: pc 0, address 0.
    void (*volatile null)() = nullptr;
    auto fault = std::make_unique<FaultRecord>();
    CHILD_CHECK(!GuardedCall([&] { null(); }, nullptr, nullptr, fault.get()));
    CHILD_CHECK(fault->Kind == FaultKind::BadAccess && fault->PC == 0 && fault->Address == 0);
    CHILD_CHECK(fault->FrameCount >= 2 && (fault->Frames[1].Flags & FrameFromLinkRegister) && (fault->Frames[1].Flags & FrameInThisImage));
    // Again, after a recovery, and inside another guard.
    CHILD_CHECK(!GuardedCall([&] { null(); }));
    int outer = 0;
    CHILD_CHECK(GuardedCall([&] { CHILD_CHECK(!GuardedCall([&] { null(); })); }, CountAndRecover, &outer));
    CHILD_CHECK(outer == 0);
    return gChildFailures;
}

int ChildIllegalInstruction()
{
    auto fault = std::make_unique<FaultRecord>();
    CHILD_CHECK(!GuardedCall([] { fault_test_undefined(); }, nullptr, nullptr, fault.get()));
    CHILD_CHECK(fault->Kind == FaultKind::IllegalInstruction && fault->Signal == SIGILL && fault->MachException == EXC_BAD_INSTRUCTION);
    CHILD_CHECK(fault->PC == (uint64_t)&fault_test_undefined && fault->Address == fault->PC);
    return gChildFailures;
}

int ChildBreakpointTrap()
{
    auto fault = std::make_unique<FaultRecord>();
    CHILD_CHECK(!GuardedCall([] { fault_test_trap(); }, nullptr, nullptr, fault.get()));
    CHILD_CHECK(fault->Kind == FaultKind::Breakpoint && fault->Signal == SIGTRAP && fault->MachException == EXC_BREAKPOINT);
    CHILD_CHECK(fault->PC == (uint64_t)&fault_test_trap);
    return gChildFailures;
}

void* OverflowThread(void* out)
{
    auto fault = static_cast<FaultRecord*>(out);
    bool completed = GuardedCall([] { fault_test_recurse(); }, nullptr, nullptr, fault);
    // Deep recursion again, now that the guard has the stack back.
    bool again = GuardedCall([] { fault_test_recurse(); });
    return reinterpret_cast<void*>((uintptr_t)(!completed && !again));
}

int ChildStackOverflow()
{
    auto fault = std::make_unique<FaultRecord>();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t thread;
    CHILD_CHECK(pthread_create(&thread, &attr, OverflowThread, fault.get()) == 0);
    void* recovered = nullptr;
    pthread_join(thread, &recovered);
    CHILD_CHECK(recovered != nullptr);
    CHILD_CHECK(fault->Kind == FaultKind::StackOverflow);
    CHILD_CHECK(fault->FrameCount == FaultRecord::MaxFrames && SymbolIs(fault->Frames[0], "_fault_test_recurse"));
    // The thread's own stack (its size rounded up by the system), with the
    // stack pointer at its bottom: it may have gone past it, into the guard.
    uint64_t sp = arm_thread_state64_get_sp(fault->Registers);
    CHILD_CHECK(fault->StackHigh - fault->StackLow >= 256 * 1024 && fault->StackHigh - fault->StackLow < 512 * 1024);
    CHILD_CHECK(sp + 4096 > fault->StackLow && sp < fault->StackLow + 64 * 1024);
    return gChildFailures;
}

int ChildMainThreadStackOverflow()
{
    // The main thread's stack bounds come from the kernel's stack limit, not
    // from a size pthread was given.
    auto fault = std::make_unique<FaultRecord>();
    CHILD_CHECK(!GuardedCall([] { fault_test_recurse(); }, nullptr, nullptr, fault.get()));
    CHILD_CHECK(fault->Kind == FaultKind::StackOverflow);
    CHILD_CHECK(!GuardedCall([] { fault_test_recurse(); }));
    return gChildFailures;
}

int ChildNestedGuards()
{
    int inner = 0, outer = 0;
    auto fault = std::make_unique<FaultRecord>();
    // The inner guard recovers; the outer one carries on and completes.
    bool innerCompleted = true;
    CHILD_CHECK(GuardedCall([&] { innerCompleted = GuardedCall([] { fault_test_read_null(); }, CountAndRecover, &inner); },
        CountAndRecover, &outer));
    CHILD_CHECK(!innerCompleted && inner == 1 && outer == 0);
    // The inner filter declines: the outer one is asked and recovers.
    bool reached = false;
    CHILD_CHECK(!GuardedCall([&] {
        GuardedCall([] { fault_test_read_null(); }, CountAndDecline, &inner);
        reached = true;
    }, CountAndRecover, &outer, fault.get()));
    CHILD_CHECK(!reached && inner == 2 && outer == 1);
    CHILD_CHECK(ThreadPort() == MACH_PORT_NULL);
    // Three deep, the middle one recovers past the innermost.
    int middle = 0;
    CHILD_CHECK(GuardedCall([&] {
        CHILD_CHECK(!GuardedCall([&] {
            GuardedCall([] { fault_test_undefined(); }, CountAndDecline, &inner);
        }, CountAndRecover, &middle));
    }, CountAndRecover, &outer));
    CHILD_CHECK(inner == 3 && middle == 1 && outer == 1);
    return gChildFailures;
}

int ChildRepeatedRecoveryKeepsNoPorts()
{
    CHILD_CHECK(!GuardedCall([] { fault_test_read_null(); }));
    size_t before = PortNameCount();
    for (int i = 0; i < 2000; i++) {
        if (GuardedCall([] { fault_test_read_null(); })) {
            CHILD_CHECK(false);
            break;
        }
    }
    size_t after = PortNameCount();
    CHILD_CHECK(after <= before + 2);
    CHILD_CHECK(ThreadPort() == MACH_PORT_NULL);
    return gChildFailures;
}

int ChildDeclined()
{
    GuardedCall([] { fault_test_read_null(); }, Decline, nullptr);
    Say("survived");
    return 0;
}

int ChildUnguardedNullRead()
{
    fault_test_read_null();
    return 0;
}

FaultDecision FaultingFilter(FaultRecord const&, void*)
{
    fault_test_read_null();
    return FaultDecision::Recover;
}

int ChildFaultInFilter()
{
    GuardedCall([] { fault_test_read_null(); }, FaultingFilter, nullptr);
    Say("survived");
    return 0;
}

FaultDecision GuardingFilter(FaultRecord const&, void* context)
{
    // As a crash reporter's filter guards its own risky reads.
    *static_cast<bool*>(context) = !GuardedCall([] { fault_test_read_null(); });
    return FaultDecision::Recover;
}

int ChildGuardInFilter()
{
    bool innerRecovered = false;
    CHILD_CHECK(!GuardedCall([] { fault_test_undefined(); }, GuardingFilter, &innerRecovered));
    CHILD_CHECK(innerRecovered);
    return gChildFailures;
}

// The thread that must not allocate just now: a filter's, while the faulting
// thread is stopped. calloc below, which is the calloc of everything linked
// into this executable, says so when that thread calls it.
std::atomic<pthread_t> gMustNotAllocate{ nullptr };

FaultDecision GuardingFilterThatMustNotAllocate(FaultRecord const&, void* context)
{
    gMustNotAllocate = pthread_self();
    *static_cast<bool*>(context) = !GuardedCall([] { fault_test_read_null(); });
    gMustNotAllocate = nullptr;
    return FaultDecision::Recover;
}

int ChildGuardInFilterAllocatesNothing()
{
    bool innerRecovered = false;
    CHILD_CHECK(!GuardedCall([] { fault_test_undefined(); }, GuardingFilterThatMustNotAllocate, &innerRecovered));
    CHILD_CHECK(innerRecovered);
    return gChildFailures;
}

std::atomic<int> gGo{ 0 };
std::atomic<int> gRecovered{ 0 };

void* FaultLoopThread(void* count)
{
    while (!gGo) {
    }
    for (int i = 0; i < (int)(intptr_t)count; i++) {
        if (!GuardedCall([] { fault_test_read_null(); })) gRecovered++;
    }
    return nullptr;
}

int ChildThreadsFaultAtOnce()
{
    constexpr int kThreads = 8, kFaults = 200;
    pthread_t threads[kThreads];
    for (auto& thread : threads) CHILD_CHECK(pthread_create(&thread, nullptr, FaultLoopThread, (void*)(intptr_t)kFaults) == 0);
    gGo = 1;
    for (auto& thread : threads) pthread_join(thread, nullptr);
    CHILD_CHECK(gRecovered == kThreads * kFaults);
    return gChildFailures;
}

int ChildUnguardedFaultBesideAGuard()
{
    pthread_t thread;
    CHILD_CHECK(pthread_create(&thread, nullptr, FaultLoopThread, (void*)(intptr_t)1000000) == 0);
    gGo = 1;
    while (gRecovered < 50) {
    }
    Say("guarded thread recovered");
    fault_test_read_null();
    return 0;
}

int ChildCppExceptionLeavesTheGuard()
{
    bool caught = false;
    try {
        GuardedCall([] { GuardedCall([] { throw 7; }); });
    } catch (int value) {
        caught = value == 7;
    }
    CHILD_CHECK(caught);
    CHILD_CHECK(ThreadPort() == MACH_PORT_NULL);
    // The thread is in no guard: a guard now is the outermost again, and recovers.
    int outer = 0;
    CHILD_CHECK(!GuardedCall([] { fault_test_read_null(); }, CountAndRecover, &outer));
    CHILD_CHECK(outer == 1 && ThreadPort() == MACH_PORT_NULL);
    return gChildFailures;
}

// A thread-level or task-level exception port of the test's own, standing in
// for one the game or a library installs. It counts what it receives, says
// so on fd 3, and answers as told.
struct ForeignPort
{
    mach_port_t Port{ MACH_PORT_NULL };
    std::atomic<int> Received{ 0 };
    bool SkipFaultingInstruction{ false };
};

void* ForeignPortMain(void* arg)
{
    auto foreign = static_cast<ForeignPort*>(arg);
    for (;;) {
        union
        {
            mach_msg_header_t Head;
            uint8_t Room[1024];
        } request;
        if (mach_msg(&request.Head, MACH_RCV_MSG, 0, sizeof(request), foreign->Port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != MACH_MSG_SUCCESS) {
            continue;
        }
        // mach_exception_raise (msgh_id 2405), EXCEPTION_DEFAULT with 64-bit codes.
#pragma pack(push, 4)
        struct Raise
        {
            mach_msg_header_t Head;
            mach_msg_body_t Body;
            mach_msg_port_descriptor_t Thread;
            mach_msg_port_descriptor_t Task;
            NDR_record_t NDR;
            exception_type_t Exception;
            mach_msg_type_number_t CodeCount;
            int64_t Code[2];
        };
#pragma pack(pop)
        auto raise = reinterpret_cast<Raise*>(&request);
        foreign->Received++;
        Say("foreign port: exception %d", raise->Exception);
        kern_return_t answer = KERN_FAILURE;
        if (foreign->SkipFaultingInstruction && request.Head.msgh_id == 2405) {
            mach_port_t thread = raise->Thread.name;
            arm_thread_state64_t state;
            mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
            thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, &count);
            arm_thread_state64_set_pc_fptr(state, (void*)(arm_thread_state64_get_pc(state) + 4));
            thread_set_state(thread, ARM_THREAD_STATE64, (thread_state_t)&state, count);
            answer = KERN_SUCCESS;
        }
        mach_port_deallocate(mach_task_self(), raise->Thread.name);
        mach_port_deallocate(mach_task_self(), raise->Task.name);
        struct
        {
            mach_msg_header_t Head;
            NDR_record_t NDR;
            kern_return_t RetCode;
        } reply{};
        reply.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request.Head.msgh_bits), 0);
        reply.Head.msgh_remote_port = request.Head.msgh_remote_port;
        reply.Head.msgh_size = sizeof(reply);
        reply.Head.msgh_id = request.Head.msgh_id + 100;
        reply.NDR = NDR_record;
        reply.RetCode = answer;
        mach_msg(&reply.Head, MACH_SEND_MSG, sizeof(reply), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    return nullptr;
}

ForeignPort* StartForeignPort()
{
    auto foreign = new ForeignPort();
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &foreign->Port);
    mach_port_insert_right(mach_task_self(), foreign->Port, foreign->Port, MACH_MSG_TYPE_MAKE_SEND);
    pthread_t thread;
    pthread_create(&thread, nullptr, ForeignPortMain, foreign);
    return foreign;
}

constexpr exception_behavior_t kForeignBehavior = EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES;

int ChildTaskPortInstalledLater()
{
    CHILD_CHECK(StartFaultHandling());
    CHILD_CHECK(!GuardedCall([] { fault_test_read_null(); }));
    auto foreign = StartForeignPort();
    CHILD_CHECK(task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, foreign->Port, kForeignBehavior, THREAD_STATE_NONE) == KERN_SUCCESS);
    CHILD_CHECK(!GuardedCall([] { fault_test_read_null(); }));
    CHILD_CHECK(foreign->Received == 0);
    if (gChildFailures) return gChildFailures;
    Say("guarded fault recovered");
    // Unguarded: the task port gets it, declines, and the signal ends the process.
    fault_test_read_null();
    return 0;
}

void LaterSignalHandler(int, siginfo_t*, void*)
{
    Say("later signal handler");
    _exit(42);
}

int ChildSignalHandlerInstalledLater()
{
    CHILD_CHECK(!GuardedCall([] { fault_test_read_null(); }));
    struct sigaction action{};
    action.sa_sigaction = LaterSignalHandler;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
    CHILD_CHECK(!GuardedCall([] { fault_test_read_null(); }));
    if (gChildFailures) return gChildFailures;
    Say("guarded fault recovered");
    fault_test_read_null();
    return 0;
}

int ChildDeclinedToTheReplacedThreadPort()
{
    auto foreign = StartForeignPort();
    mach_port_t thread = pthread_mach_thread_np(pthread_self());
    CHILD_CHECK(thread_set_exception_ports(thread, EXC_MASK_BAD_ACCESS, foreign->Port, kForeignBehavior, THREAD_STATE_NONE) == KERN_SUCCESS);
    // Inside the guard the port is the guard's; after it, the test's again.
    mach_port_t inside = MACH_PORT_NULL;
    CHILD_CHECK(GuardedCall([&] { inside = ThreadPort(); }));
    CHILD_CHECK(inside != MACH_PORT_NULL && inside != foreign->Port);
    CHILD_CHECK(ThreadPort() == foreign->Port);

    // A recovered fault never reaches it.
    CHILD_CHECK(!GuardedCall([] { fault_test_read_null(); }));
    CHILD_CHECK(foreign->Received == 0);

    // A declined one does; when it handles the fault (by skipping the
    // instruction), the guarded call goes on and completes.
    foreign->SkipFaultingInstruction = true;
    bool after = false;
    CHILD_CHECK(GuardedCall([&] { fault_test_read_null(); after = true; }, Decline, nullptr));
    CHILD_CHECK(after && foreign->Received == 1);
    CHILD_CHECK(ThreadPort() == foreign->Port);

    // When it declines too, the fault goes on to the signal.
    foreign->SkipFaultingInstruction = false;
    if (gChildFailures) return gChildFailures;
    Say("handled by the replaced port");
    GuardedCall([] { fault_test_read_null(); }, Decline, nullptr);
    return 0;
}

// fork() inside a guard, and the guard left in the child. The guard saved the
// thread's port, a name of the parent's. In the child that name is given to a
// port of the child's own: leaving the guard must not touch it.
int ChildForkInsideAGuard()
{
    // A name far from the ones a new process hands out.
    constexpr mach_port_name_t kName = (2000u << 8) | 3;
    CHILD_CHECK(mach_port_allocate_name(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, kName) == KERN_SUCCESS);
    CHILD_CHECK(mach_port_insert_right(mach_task_self(), kName, kName, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    mach_port_t thread = pthread_mach_thread_np(pthread_self());
    CHILD_CHECK(thread_set_exception_ports(thread, EXC_MASK_BAD_ACCESS, kName, kForeignBehavior, THREAD_STATE_NONE) == KERN_SUCCESS);
    if (gChildFailures) return gChildFailures;

    bool forked = false;
    int ended = -1;
    GuardedCall([&] {
        pid_t pid = fork();
        if (pid == 0) {
            forked = true;
            if (mach_port_allocate_name(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, kName) != KERN_SUCCESS) _exit(50);
            if (mach_port_insert_right(mach_task_self(), kName, kName, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) _exit(50);
            return;
        }
        int status = 0;
        waitpid(pid, &status, 0);
        ended = WIFEXITED(status) ? WEXITSTATUS(status) : 100 + WTERMSIG(status);
    });
    if (forked) {
        // The child's port still has its one send right, and the thread no port from the guard.
        mach_port_urefs_t refs = 0;
        if (mach_port_get_refs(mach_task_self(), kName, MACH_PORT_RIGHT_SEND, &refs) != KERN_SUCCESS || refs != 1) _exit(51);
        if (ThreadPort() != MACH_PORT_NULL) _exit(52);
        // And guards work in the child.
        if (GuardedCall([] { fault_test_read_null(); })) _exit(53);
        _exit(0);
    }
    Say("forked child ended with %d", ended);
    CHILD_CHECK(ended == 0);
    // In the parent the guard put the thread's port back.
    CHILD_CHECK(ThreadPort() == kName);
    return gChildFailures;
}

// The exception threads are started by whatever thread enters a guard
// first: here one that has a fault's signal blocked and takes SIGUSR1.
FaultDecision SayMaskAndRecover(FaultRecord const&, void*)
{
    sigset_t mask;
    pthread_sigmask(SIG_SETMASK, nullptr, &mask);
    Say("filter: SIGSEGV %s, SIGABRT %s, SIGUSR1 %s, SIGCHLD %s", sigismember(&mask, SIGSEGV) ? "blocked" : "open",
        sigismember(&mask, SIGABRT) ? "blocked" : "open", sigismember(&mask, SIGUSR1) ? "blocked" : "open",
        sigismember(&mask, SIGCHLD) ? "blocked" : "open");
    return FaultDecision::Recover;
}

void BlockFaultSignals()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGSEGV);
    sigaddset(&mask, SIGBUS);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
}

int ChildExceptionThreadsMask()
{
    BlockFaultSignals();
    CHILD_CHECK(!GuardedCall([] { fault_test_read_null(); }, SayMaskAndRecover, nullptr));
    return gChildFailures;
}

int ChildFaultInFilterStartedWithFaultsBlocked()
{
    BlockFaultSignals();
    GuardedCall([] { fault_test_undefined(); }, FaultingFilter, nullptr);
    Say("survived");
    return 0;
}

// --- The time limit ---

// A filter that waits for what it will never get.
FaultDecision NeverAnswers(FaultRecord const&, void*)
{
    Say("filter entered");
    for (;;) pause();
}

int ChildFilterNeverAnswers()
{
    SetFaultTimeLimitMs(300);
    GuardedCall([] { fault_test_read_null(); }, NeverAnswers, nullptr);
    Say("survived");
    return 0;
}

std::atomic<int> gLateFilterAnswered{ 0 };

FaultDecision AnswersLate(FaultRecord const&, void*)
{
    usleep(1500 * 1000);
    gLateFilterAnswered = 1;
    return FaultDecision::Recover;
}

// Stands in for whatever takes a fault the guards let go: it steps over the
// faulting instruction, and the thread carries on.
void SkipTheInstruction(int, siginfo_t*, void* context)
{
    auto uc = static_cast<ucontext_t*>(context);
    arm_thread_state64_set_pc_fptr(uc->uc_mcontext->__ss, (void*)(arm_thread_state64_get_pc(uc->uc_mcontext->__ss) + 4));
    Say("signal handler stepped over the instruction");
}

int ChildFilterAnswersLate()
{
    SetFaultTimeLimitMs(300);
    struct sigaction action{};
    action.sa_sigaction = SkipTheInstruction;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, nullptr);

    // The filter is given up on; the fault goes on to the signal, whose
    // handler steps over it: the guarded call completes.
    bool after = false;
    auto fault = std::make_unique<FaultRecord>();
    fault->PC = 1;
    bool completed = GuardedCall([&] { fault_test_read_null(); after = true; }, AnswersLate, nullptr, fault.get());
    CHILD_CHECK(completed && after);
    CHILD_CHECK(gLateFilterAnswered == 0);

    // The filter answers "recover" a second later. Nobody is listening: the
    // thread is in no guard, it is not thrown back into one, and its record
    // is not written.
    while (!gLateFilterAnswered) usleep(10 * 1000);
    usleep(200 * 1000);
    CHILD_CHECK(ThreadPort() == MACH_PORT_NULL);
    CHILD_CHECK(fault->PC == 1);
    // Both exception threads serve again.
    SetFaultTimeLimitMs(30 * 1000);
    for (int i = 0; i < 20; i++) CHILD_CHECK(!GuardedCall([] { fault_test_read_null(); }));
    CHILD_CHECK(!GuardedCall([] { fault_test_undefined(); }, GuardingFilter, &after));
    return gChildFailures;
}

void ObserverNeverReturns(FaultRecord const&, void*)
{
    Say("observer entered");
    for (;;) pause();
}

int ChildObserverNeverReturns()
{
    SetFaultTimeLimitMs(300);
    CHILD_CHECK(AddFatalFaultObserver(ObserverNeverReturns, nullptr));
    fault_test_read_null();
    return 0;
}

// mach_port_set_context as the platform layer calls it (see the definition
// at the end of this file): the system's, failing while this counts down.
kern_return_t (*gSystemSetContext)(ipc_space_t, mach_port_name_t, mach_port_context_t) = nullptr;
std::atomic<int> gFailSetContext{ 0 };

int ChildPortSetupFails()
{
    CHILD_CHECK(StartFaultHandling());
    // The exception threads set their own ports up as they start: first.
    usleep(100 * 1000);
    // This thread's port for guards cannot be set up: its guarded calls run
    // unguarded, each trying again, and none leaves a port name behind.
    constexpr int kAttempts = 200;
    size_t before = PortNameCount();
    gFailSetContext = kAttempts;
    int ran = 0;
    for (int i = 0; i < kAttempts; i++) CHILD_CHECK(GuardedCall([&] { ran++; }));
    size_t after = PortNameCount();
    CHILD_CHECK(ran == kAttempts && gFailSetContext == 0);
    Say("port names: %zu before, %zu after", before, after);
    CHILD_CHECK(after <= before + 2);
    // Once it can be, guards work.
    CHILD_CHECK(!GuardedCall([] { fault_test_read_null(); }));
    return gChildFailures;
}

// --- Fatal-fault observers ---

std::atomic<uint64_t> gExpectedThread{ 0 };

char const* OriginName(FaultOrigin origin)
{
    switch (origin) {
    case FaultOrigin::Guarded: return "guarded";
    case FaultOrigin::Unguarded: return "unguarded";
    case FaultOrigin::DeclinedByGuards: return "declined";
    }
    return "?";
}

// Writes what it was told to fd 3, formatting into a stack buffer as an
// observer must.
void SayRecord(FaultRecord const& fault, void*)
{
    FrameSymbol symbol{};
    bool named = fault.FrameCount > 0 && SymbolizeFrame(fault.Frames[0], symbol);
    uint64_t self = 0;
    pthread_threadid_np(nullptr, &self);
    Say("observed %s, signal %d, %s", FaultKindName(fault.Kind), fault.Signal, OriginName(fault.Origin));
    Say("observed frame 0 in %s", named ? symbol.Name : "?");
    Say("observed on the faulting thread: %s", fault.ThreadId == gExpectedThread ? "yes" : "no");
    Say("observer runs on another thread: %s", self != fault.ThreadId ? "yes" : "no");
}

void ExpectThisThread()
{
    uint64_t self = 0;
    pthread_threadid_np(nullptr, &self);
    gExpectedThread = self;
}

int ChildObserveNullRead()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    ExpectThisThread();
    fault_test_read_null();
    return 0;
}

int ChildObserveIllegalInstruction()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    ExpectThisThread();
    fault_test_undefined();
    return 0;
}

int ChildUnobservedIllegalInstruction()
{
    fault_test_undefined();
    return 0;
}

int ChildObserveTrap()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    ExpectThisThread();
    fault_test_trap();
    return 0;
}

int ChildUnobservedTrap()
{
    fault_test_trap();
    return 0;
}

int ChildObserveAbort()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    ExpectThisThread();
    abort();
}

int ChildUnobservedAbort()
{
    abort();
}

int ChildObserveDeclined()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    ExpectThisThread();
    GuardedCall([] { fault_test_read_null(); }, Decline, nullptr);
    return 0;
}

int ChildObserveNothingRecovered()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    for (int i = 0; i < 10; i++) CHILD_CHECK(!GuardedCall([] { fault_test_read_null(); }));
    Say("done");
    return gChildFailures;
}

void ObserverRemovedLater(FaultRecord const&, void*)
{
    Say("removed observer told");
}

int ChildObserveAfterRemove()
{
    CHILD_CHECK(AddFatalFaultObserver(ObserverRemovedLater, nullptr));
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    RemoveFatalFaultObserver(ObserverRemovedLater, nullptr);
    ExpectThisThread();
    fault_test_read_null();
    return 0;
}

struct sigaction gBeforeLater{};

// As the game's crash reporter does: report, then pass the signal to the
// handler that was installed before.
void ChainingHandler(int signal, siginfo_t* info, void* context)
{
    Say("later handler");
    if (gBeforeLater.sa_flags & SA_SIGINFO) gBeforeLater.sa_sigaction(signal, info, context);
}

int ChildObserveBehindALaterHandler()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    struct sigaction action{};
    action.sa_sigaction = ChainingHandler;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, &gBeforeLater);
    ExpectThisThread();
    fault_test_read_null();
    return 0;
}

// Whether a signal's action is a handler (bg3mse's, or one installed after it).
bool HasAHandler(int signal)
{
    struct sigaction now{};
    return sigaction(signal, nullptr, &now) == 0 && now.sa_handler != SIG_DFL && now.sa_handler != SIG_IGN;
}

// A signal the process ignored before bg3mse's handler was installed, as a
// program does with SIGTRAP so that its debugger breaks are harmless.
int ChildIgnoredBefore()
{
    signal(SIGTRAP, SIG_IGN);
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    CHILD_CHECK(HasAHandler(SIGTRAP));
    raise(SIGTRAP);
    // Nothing happened, and the handler is still the signal's.
    CHILD_CHECK(HasAHandler(SIGTRAP));
    raise(SIGTRAP);
    Say("survived");
    return gChildFailures;
}

int ChildIgnoredBeforeBehindALaterHandler()
{
    signal(SIGTRAP, SIG_IGN);
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    struct sigaction action{};
    action.sa_sigaction = ChainingHandler;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &action, &gBeforeLater);
    raise(SIGTRAP);
    raise(SIGTRAP);
    struct sigaction now{};
    sigaction(SIGTRAP, nullptr, &now);
    CHILD_CHECK(now.sa_sigaction == ChainingHandler);
    Say("survived");
    return gChildFailures;
}

void* UnguardedOverflow(void* altStack)
{
    if (altStack) {
        stack_t stack{};
        stack.ss_sp = altStack;
        stack.ss_size = 64 * 1024;
        sigaltstack(&stack, nullptr);
    }
    ExpectThisThread();
    fault_test_recurse();
    return nullptr;
}

int OverflowOnAThread(bool withAltStack)
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t thread;
    void* altStack = withAltStack ? mmap(nullptr, 64 * 1024, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0) : nullptr;
    pthread_create(&thread, &attr, UnguardedOverflow, altStack);
    pthread_join(thread, nullptr);
    return 0;
}

int ChildObserveOverflowWithoutAltStack()
{
    return OverflowOnAThread(false);
}

int ChildObserveOverflowWithAltStack()
{
    return OverflowOnAThread(true);
}

void* UnguardedNullReadThread(void*)
{
    while (!gGo) {
    }
    fault_test_read_null();
    return nullptr;
}

int ChildObserveTwoThreadsAtOnce()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    pthread_t a, b;
    pthread_create(&a, nullptr, UnguardedNullReadThread, nullptr);
    pthread_create(&b, nullptr, UnguardedNullReadThread, nullptr);
    gGo = 1;
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
    return 0;
}

void FaultingObserver(FaultRecord const&, void*)
{
    Say("faulting observer called");
    fault_test_read_null();
}

int ChildObserveFaultInObserver()
{
    CHILD_CHECK(AddFatalFaultObserver(FaultingObserver, nullptr));
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    fault_test_undefined();
    return 0;
}

int ChildObserveFaultInFilter()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    GuardedCall([] { fault_test_read_null(); }, FaultingFilter, nullptr);
    Say("survived");
    return 0;
}

// Signals that are sent, not raised by a fault: to the handler they look
// like faults (the same si_code), and the process must end by them all the same.
int SendAfterObserving(int signal, int how)
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    ExpectThisThread();
    if (how == 0) raise(signal);
    if (how == 1) kill(getpid(), signal);
    if (how == 2) pthread_kill(pthread_self(), signal);
    // A signal sent to the process may be delivered a moment later.
    usleep(200 * 1000);
    Say("survived");
    return 0;
}

int ChildSentSegvByRaise() { return SendAfterObserving(SIGSEGV, 0); }
int ChildSentBusByKill() { return SendAfterObserving(SIGBUS, 1); }
int ChildSentIllByPthreadKill() { return SendAfterObserving(SIGILL, 2); }
int ChildSentTrapByRaise() { return SendAfterObserving(SIGTRAP, 0); }

int ChildSentUnobserved()
{
    raise(SIGSEGV);
    Say("survived");
    return 0;
}

int ChildSentBehindALaterHandler()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    struct sigaction action{};
    action.sa_sigaction = ChainingHandler;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, &gBeforeLater);
    ExpectThisThread();
    raise(SIGSEGV);
    Say("survived");
    return 0;
}

// --- In front of other handlers ---

// A handler installed after bg3mse's that behaves as the game's does (read
// in the game's code): it puts the fatal signals back to their defaults,
// does its work, calls nothing that was installed before it, and raises the
// signal again.
void GameLikeHandler(int signal, siginfo_t* info, void* context)
{
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    for (int s : { SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGTRAP }) sigaction(s, &action, nullptr);
    auto uc = static_cast<ucontext_t*>(context);
    uint64_t pc = arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    Say("game handler: signal %d at %p, pc %s", signal, info->si_addr,
        pc == (uint64_t)&fault_test_read_null + 4 ? "at the fault" : "elsewhere");
    raise(signal);
}

void InstallGameLikeHandler()
{
    struct sigaction action{};
    action.sa_sigaction = GameLikeHandler;
    action.sa_flags = SA_SIGINFO;
    for (int s : { SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGTRAP }) sigaction(s, &action, nullptr);
}

int ChildBehindAHandlerThatPassesNothingOn()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    InstallGameLikeHandler();
    ExpectThisThread();
    fault_test_read_null();
    Say("survived");
    return 0;
}

int ChildPutInFront()
{
    // Nothing to put in front, and nothing happens.
    CHILD_CHECK(PutFatalFaultObserversInFront());
    CHILD_CHECK(!HasAHandler(SIGSEGV));
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    InstallGameLikeHandler();
    CHILD_CHECK(PutFatalFaultObserversInFront());
    // Again changes nothing: the handler is not put in front of itself.
    CHILD_CHECK(PutFatalFaultObserversInFront());
    ExpectThisThread();
    fault_test_read_null();
    Say("survived");
    return 0;
}

// In front of a handler that does pass the signal on, to the handler it
// found when it was installed: bg3mse's.
int ChildPutInFrontOfAHandlerThatPassesOn()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    struct sigaction action{};
    action.sa_sigaction = ChainingHandler;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, &gBeforeLater);
    CHILD_CHECK(PutFatalFaultObserversInFront());
    ExpectThisThread();
    fault_test_read_null();
    Say("survived");
    return 0;
}

// Both, one after the other: the signal goes through bg3mse's handler three
// times, each time on to what was displaced one step earlier.
int ChildPutInFrontTwice()
{
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    InstallGameLikeHandler();
    CHILD_CHECK(PutFatalFaultObserversInFront());
    struct sigaction action{};
    action.sa_sigaction = ChainingHandler;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, &gBeforeLater);
    CHILD_CHECK(PutFatalFaultObserversInFront());
    ExpectThisThread();
    fault_test_read_null();
    Say("survived");
    return 0;
}

// A handler that was there before bg3mse's and leaves by a jump, so that
// bg3mse's handler never returns: its frame must not be taken for one that
// is still passing a signal on when the next signal comes.
sigjmp_buf gJumpOut;

void JumpsOut(int, siginfo_t*, void*)
{
    Say("earlier handler jumps out");
    siglongjmp(gJumpOut, 1);
}

__attribute__((noinline)) void RaiseTrapAtDepth(int depth)
{
    volatile char room[200];
    room[0] = static_cast<char>(depth);
    if (depth > 0) {
        RaiseTrapAtDepth(depth - 1);
    } else {
        raise(SIGTRAP);
    }
    room[1] = room[0];
}

int ChildHandlerBelowJumpsOut()
{
    struct sigaction action{};
    action.sa_sigaction = JumpsOut;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &action, nullptr);
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    // More often than the handler has room to keep frames in mind, at the
    // same depth of the stack and at others.
    // (Volatile: it is read again after a jump back to sigsetjmp.)
    for (volatile int i = 0; i < 100; i = i + 1) {
        if (!sigsetjmp(gJumpOut, 1)) RaiseTrapAtDepth(i % 3 == 0 ? 0 : i % 7);
    }
    Say("jumped out a hundred times");

    // The frames that were jumped out of are forgotten, and there is room to
    // keep a live one in mind: with a handler in between that passes the
    // signal on to bg3mse's, the signal still goes on to the handler that
    // was there first, once.
    struct sigaction between{};
    between.sa_sigaction = ChainingHandler;
    between.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &between, &gBeforeLater);
    CHILD_CHECK(PutFatalFaultObserversInFront());
    if (!sigsetjmp(gJumpOut, 1)) raise(SIGTRAP);
    Say("survived");
    return gChildFailures;
}

// A handler that was there before bg3mse's: it says which signals are
// blocked while it runs, and takes the signal as handled.
void EarlierHandler(int signal, siginfo_t*, void*)
{
    sigset_t mask;
    pthread_sigmask(SIG_SETMASK, nullptr, &mask);
    Say("earlier handler: signal %d %s, SIGUSR1 %s, SIGUSR2 %s", signal, sigismember(&mask, signal) ? "blocked" : "open",
        sigismember(&mask, SIGUSR1) ? "blocked" : "open", sigismember(&mask, SIGUSR2) ? "blocked" : "open");
}

void InstallEarlier(int signal, int flags)
{
    struct sigaction action{};
    action.sa_sigaction = EarlierHandler;
    action.sa_flags = SA_SIGINFO | flags;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR1);
    sigaction(signal, &action, nullptr);
}

void SayMaskAfter()
{
    sigset_t mask;
    pthread_sigmask(SIG_SETMASK, nullptr, &mask);
    Say("after: SIGTRAP %s, SIGFPE %s, SIGUSR1 %s", sigismember(&mask, SIGTRAP) ? "blocked" : "open",
        sigismember(&mask, SIGFPE) ? "blocked" : "open", sigismember(&mask, SIGUSR1) ? "blocked" : "open");
}

int ChildEarlierHandlersMask()
{
    InstallEarlier(SIGTRAP, 0);
    InstallEarlier(SIGFPE, SA_NODEFER);
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    raise(SIGTRAP);
    raise(SIGFPE);
    SayMaskAfter();
    return gChildFailures;
}

// sigaction as the platform layer calls it (see the definition at the end
// of this file): the system's, and failing once for one signal when told to.
int (*gSystemSigaction)(int, struct sigaction const*, struct sigaction*) = nullptr;
std::atomic<int> gFailInstallingFor{ 0 };

int ChildInstallFailsPartway()
{
    // The third of the six signals cannot be given the handler: the first
    // two have it, and the observer is not added.
    gFailInstallingFor = SIGILL;
    CHILD_CHECK(!AddFatalFaultObserver(SayRecord, nullptr));
    CHILD_CHECK(gFailInstallingFor == 0);
    // The next attempt installs what is missing, and leaves alone what is there.
    CHILD_CHECK(AddFatalFaultObserver(SayRecord, nullptr));
    ExpectThisThread();
    fault_test_read_null();
    return 0;
}

struct ChildCase
{
    char const* Name;
    int (*Run)();
};

ChildCase const kChildCases[] = {
    { "null-read", ChildNullRead },
    { "read-only-write", ChildReadOnlyWrite },
    { "jump-to-data", ChildJumpToData },
    { "null-call", ChildNullCall },
    { "illegal-instruction", ChildIllegalInstruction },
    { "breakpoint-trap", ChildBreakpointTrap },
    { "stack-overflow", ChildStackOverflow },
    { "main-thread-stack-overflow", ChildMainThreadStackOverflow },
    { "nested-guards", ChildNestedGuards },
    { "repeated-recovery", ChildRepeatedRecoveryKeepsNoPorts },
    { "declined", ChildDeclined },
    { "unguarded-null-read", ChildUnguardedNullRead },
    { "fault-in-filter", ChildFaultInFilter },
    { "guard-in-filter", ChildGuardInFilter },
    { "guard-in-filter-allocates-nothing", ChildGuardInFilterAllocatesNothing },
    { "threads-fault-at-once", ChildThreadsFaultAtOnce },
    { "unguarded-beside-guard", ChildUnguardedFaultBesideAGuard },
    { "cpp-exception", ChildCppExceptionLeavesTheGuard },
    { "task-port-later", ChildTaskPortInstalledLater },
    { "signal-handler-later", ChildSignalHandlerInstalledLater },
    { "declined-to-thread-port", ChildDeclinedToTheReplacedThreadPort },
    { "fork-inside-a-guard", ChildForkInsideAGuard },
    { "port-setup-fails", ChildPortSetupFails },
    { "exception-threads-mask", ChildExceptionThreadsMask },
    { "filter-never-answers", ChildFilterNeverAnswers },
    { "filter-answers-late", ChildFilterAnswersLate },
    { "observer-never-returns", ChildObserverNeverReturns },
    { "fault-in-filter-started-with-faults-blocked", ChildFaultInFilterStartedWithFaultsBlocked },
    { "observe-null-read", ChildObserveNullRead },
    { "observe-illegal-instruction", ChildObserveIllegalInstruction },
    { "unobserved-illegal-instruction", ChildUnobservedIllegalInstruction },
    { "observe-trap", ChildObserveTrap },
    { "unobserved-trap", ChildUnobservedTrap },
    { "observe-abort", ChildObserveAbort },
    { "unobserved-abort", ChildUnobservedAbort },
    { "observe-declined", ChildObserveDeclined },
    { "observe-nothing-recovered", ChildObserveNothingRecovered },
    { "observe-after-remove", ChildObserveAfterRemove },
    { "observe-behind-later-handler", ChildObserveBehindALaterHandler },
    { "observe-overflow-without-altstack", ChildObserveOverflowWithoutAltStack },
    { "observe-overflow-with-altstack", ChildObserveOverflowWithAltStack },
    { "observe-two-threads", ChildObserveTwoThreadsAtOnce },
    { "observe-fault-in-filter", ChildObserveFaultInFilter },
    { "observe-fault-in-observer", ChildObserveFaultInObserver },
    { "install-fails-partway", ChildInstallFailsPartway },
    { "sent-segv-by-raise", ChildSentSegvByRaise },
    { "sent-bus-by-kill", ChildSentBusByKill },
    { "sent-ill-by-pthread-kill", ChildSentIllByPthreadKill },
    { "sent-trap-by-raise", ChildSentTrapByRaise },
    { "sent-unobserved", ChildSentUnobserved },
    { "sent-behind-later-handler", ChildSentBehindALaterHandler },
    { "earlier-handlers-mask", ChildEarlierHandlersMask },
    { "behind-a-handler-that-passes-nothing-on", ChildBehindAHandlerThatPassesNothingOn },
    { "put-in-front", ChildPutInFront },
    { "put-in-front-of-a-handler-that-passes-on", ChildPutInFrontOfAHandlerThatPassesOn },
    { "put-in-front-twice", ChildPutInFrontTwice },
    { "handler-below-jumps-out", ChildHandlerBelowJumpsOut },
    { "ignored-before", ChildIgnoredBefore },
    { "ignored-before-behind-later-handler", ChildIgnoredBeforeBehindALaterHandler },
};

void ExpectClean(char const* name)
{
    ChildResult child = RunChild(name);
    if (!child.Exited(0)) printf("  child %s: status 0x%x, said:\n%s", name, child.Status, child.Output.c_str());
    CHECK(child.Exited(0));
}

TEST(a_guard_recovers_from_a_null_read)
{
    ExpectClean("null-read");
}

TEST(a_guard_recovers_from_a_write_to_read_only_memory)
{
    ExpectClean("read-only-write");
}

TEST(a_guard_recovers_from_a_jump_to_memory_that_is_not_executable)
{
    ExpectClean("jump-to-data");
}

TEST(a_guard_recovers_from_a_call_through_a_null_pointer)
{
    ExpectClean("null-call");
}

TEST(a_guard_recovers_from_an_illegal_instruction)
{
    ExpectClean("illegal-instruction");
}

TEST(a_guard_recovers_from_a_breakpoint_trap)
{
    ExpectClean("breakpoint-trap");
}

TEST(a_guard_recovers_from_a_stack_overflow)
{
    ExpectClean("stack-overflow");
}

TEST(a_guard_recovers_from_a_stack_overflow_on_the_main_thread)
{
    ExpectClean("main-thread-stack-overflow");
}

TEST(guards_nest_and_filters_are_asked_innermost_first)
{
    ExpectClean("nested-guards");
}

TEST(recovering_again_and_again_keeps_no_ports)
{
    ExpectClean("repeated-recovery");
}

TEST(a_declined_fault_ends_the_process_as_it_would_without_a_guard)
{
    ChildResult declined = RunChild("declined");
    ChildResult control = RunChild("unguarded-null-read");
    CHECK(control.KilledBy(SIGSEGV));
    CHECK(declined.KilledBy(SIGSEGV) && declined.Status == control.Status);
    CHECK(!declined.Said("survived"));
}

TEST(a_fault_in_a_filter_ends_the_process)
{
    ChildResult child = RunChild("fault-in-filter");
    CHECK(child.KilledBy(SIGSEGV));
    CHECK(!child.Said("survived"));
}

TEST(a_filter_may_use_guarded_calls)
{
    ExpectClean("guard-in-filter");
}

TEST(a_filters_guarded_call_allocates_nothing)
{
    ChildResult child = RunChild("guard-in-filter-allocates-nothing");
    CHECK(child.Exited(0));
    CHECK(!child.Said("allocated on a thread that must not"));
}

TEST(threads_faulting_at_once_all_recover)
{
    ExpectClean("threads-fault-at-once");
}

TEST(a_fault_outside_any_guard_ends_the_process_while_another_thread_is_guarded)
{
    ChildResult child = RunChild("unguarded-beside-guard");
    CHECK(child.Said("guarded thread recovered"));
    CHECK(child.KilledBy(SIGSEGV));
}

TEST(a_cpp_exception_leaves_the_guard_and_its_port)
{
    ExpectClean("cpp-exception");
}

TEST(a_task_port_installed_later_sees_only_unguarded_faults)
{
    ChildResult child = RunChild("task-port-later");
    CHECK(child.Said("guarded fault recovered"));
    CHECK(child.Said("foreign port: exception 1"));
    CHECK(child.KilledBy(SIGSEGV));
}

TEST(a_signal_handler_installed_later_sees_only_unguarded_faults)
{
    ChildResult child = RunChild("signal-handler-later");
    CHECK(child.Said("guarded fault recovered"));
    CHECK(child.Said("later signal handler"));
    CHECK(child.Exited(42));
}

TEST(a_declined_fault_goes_to_the_thread_port_the_guard_replaced)
{
    ChildResult child = RunChild("declined-to-thread-port");
    if (!child.Said("handled by the replaced port")) printf("%s", child.Output.c_str());
    CHECK(child.Said("handled by the replaced port"));
    CHECK(child.KilledBy(SIGSEGV));
}

TEST(a_guard_without_a_fault_leaves_the_thread_port_as_it_was)
{
    CHECK(ThreadPort() == MACH_PORT_NULL);
    mach_port_t inside = MACH_PORT_NULL, nested = MACH_PORT_NULL;
    int value = 0;
    CHECK(GuardedCall([&] {
        inside = ThreadPort();
        GuardedCall([&] { nested = ThreadPort(); value = 5; });
    }));
    CHECK(value == 5 && inside != MACH_PORT_NULL && nested == inside);
    CHECK(ThreadPort() == MACH_PORT_NULL);
}

TEST(a_guard_left_in_a_forked_child_touches_none_of_the_childs_ports)
{
    ChildResult child = RunChild("fork-inside-a-guard");
    if (!child.Exited(0)) printf("  child fork-inside-a-guard: status 0x%x, said:\n%s", child.Status, child.Output.c_str());
    CHECK(child.Exited(0) && child.Said("forked child ended with 0"));
}

TEST(a_thread_whose_port_cannot_be_set_up_leaves_no_port_names_behind)
{
    ChildResult child = RunChild("port-setup-fails");
    if (!child.Exited(0)) printf("  child port-setup-fails: status 0x%x, said:\n%s", child.Status, child.Output.c_str());
    CHECK(child.Exited(0));
}

TEST(the_exception_threads_have_a_signal_mask_of_their_own)
{
    ChildResult child = RunChild("exception-threads-mask");
    if (!child.Exited(0)) printf("  child exception-threads-mask: status 0x%x, said:\n%s", child.Status, child.Output.c_str());
    CHECK(child.Exited(0));
    CHECK(child.Said("filter: SIGSEGV open, SIGABRT open, SIGUSR1 blocked, SIGCHLD blocked"));

    // So a fault in a filter ends the process whoever started the threads.
    ChildResult fault = RunChild("fault-in-filter-started-with-faults-blocked");
    CHECK(fault.KilledBy(SIGSEGV) && !fault.Said("survived"));
}

TEST(a_filter_that_does_not_answer_within_the_time_limit_is_given_up_on)
{
    // The limit is 300 ms in the child: it ends as a crashed process does,
    // and is not found hung after a minute. (How long a crashed process takes
    // to be gone is the system's affair: with many crashing at once, as these
    // tests' children do, it has been seen to take twenty seconds.)
    ChildResult child = RunChild("filter-never-answers");
    if (!child.KilledBy(SIGSEGV)) printf("  child filter-never-answers: status 0x%x, hung %d, said:\n%s", child.Status, child.Hung, child.Output.c_str());
    CHECK(child.KilledBy(SIGSEGV) && child.Said("filter entered") && !child.Said("survived"));

    ChildResult late = RunChild("filter-answers-late");
    if (!late.Exited(0)) printf("  child filter-answers-late: status 0x%x, hung %d, said:\n%s", late.Status, late.Hung, late.Output.c_str());
    CHECK(late.Exited(0) && late.Count("signal handler stepped over the instruction") == 1);

    // The observers' limit is the same one.
    ChildResult observer = RunChild("observer-never-returns");
    CHECK(observer.KilledBy(SIGSEGV) && observer.Said("observer entered"));
}

// The observer was told, on its own thread, of the faulting thread's fault,
// and the child died with the same status as one without bg3mse.
void ExpectObserved(char const* name, char const* control, int signal, char const* what)
{
    ChildResult child = RunChild(name);
    ChildResult without = RunChild(control);
    CHECK(without.KilledBy(signal));
    CHECK(child.KilledBy(signal) && child.Status == without.Status);
    if (!child.Said(what)) printf("  child %s said:\n%s", name, child.Output.c_str());
    CHECK(child.Said(what));
    CHECK(child.Said("observed on the faulting thread: yes"));
    CHECK(child.Said("observer runs on another thread: yes"));
}

TEST(an_observer_is_told_of_a_fatal_bad_access_and_the_process_dies_as_without_it)
{
    ExpectObserved("observe-null-read", "unguarded-null-read", SIGSEGV, "observed bad access, signal 11, unguarded");
    ChildResult child = RunChild("observe-null-read");
    CHECK(child.Said("observed frame 0 in _fault_test_read_null"));
}

TEST(an_observer_is_told_of_a_fatal_illegal_instruction)
{
    ExpectObserved("observe-illegal-instruction", "unobserved-illegal-instruction", SIGILL, "observed illegal instruction, signal 4, unguarded");
}

TEST(an_observer_is_told_of_a_fatal_breakpoint_trap)
{
    ExpectObserved("observe-trap", "unobserved-trap", SIGTRAP, "observed breakpoint trap, signal 5, unguarded");
}

TEST(an_observer_is_told_of_abort)
{
    ExpectObserved("observe-abort", "unobserved-abort", SIGABRT, "observed abort, signal 6, unguarded");
}

TEST(an_observer_is_told_of_a_fault_every_guard_declined)
{
    ExpectObserved("observe-declined", "unguarded-null-read", SIGSEGV, "observed bad access, signal 11, declined");
}

TEST(an_observer_is_not_told_of_recovered_faults)
{
    ChildResult child = RunChild("observe-nothing-recovered");
    CHECK(child.Exited(0) && child.Said("done"));
    CHECK(child.Output.find("observed") == std::string::npos);
}

TEST(a_removed_observer_is_not_told)
{
    ChildResult child = RunChild("observe-after-remove");
    CHECK(child.KilledBy(SIGSEGV));
    CHECK(child.Said("observed bad access, signal 11, unguarded"));
    CHECK(!child.Said("removed observer told"));
}

TEST(a_handler_installed_later_that_passes_signals_on_runs_before_the_observers)
{
    ChildResult child = RunChild("observe-behind-later-handler");
    CHECK(child.KilledBy(SIGSEGV));
    size_t later = child.Output.find("later handler\n");
    size_t observed = child.Output.find("observed bad access, signal 11, unguarded\n");
    CHECK(later != std::string::npos && observed != std::string::npos && later < observed);
}

TEST(a_stack_overflow_outside_any_guard_is_told_only_on_a_thread_with_an_alternate_signal_stack)
{
    ChildResult without = RunChild("observe-overflow-without-altstack");
    printf("  unguarded stack overflow, no alternate signal stack: killed by signal %d\n", WIFSIGNALED(without.Status) ? WTERMSIG(without.Status) : 0);
    CHECK(without.Ran && !without.Hung && WIFSIGNALED(without.Status));
    CHECK(without.Output.find("observed") == std::string::npos);

    ChildResult with = RunChild("observe-overflow-with-altstack");
    if (!with.Said("observed on the faulting thread: yes")) printf("%s", with.Output.c_str());
    CHECK(with.Output.find("observed stack overflow, signal ") != std::string::npos);
    CHECK(with.Said("observed frame 0 in _fault_test_recurse"));
    CHECK(with.Said("observed on the faulting thread: yes"));
    int told = 0;
    sscanf(with.Output.c_str() + with.Output.find("observed stack overflow, signal "), "observed stack overflow, signal %d", &told);
    CHECK(with.KilledBy(told));
}

TEST(two_threads_faulting_at_once_outside_guards_end_the_process_once_told)
{
    ChildResult child = RunChild("observe-two-threads");
    CHECK(child.KilledBy(SIGSEGV));
    CHECK(child.Said("observed bad access, signal 11, unguarded"));
}

TEST(a_fault_in_a_filter_is_not_sent_to_the_observers_and_ends_the_process)
{
    ChildResult child = RunChild("observe-fault-in-filter");
    CHECK(child.KilledBy(SIGSEGV));
    CHECK(child.Output.find("observed") == std::string::npos && !child.Said("survived"));
}

TEST(a_fault_in_an_observer_ends_the_process_as_a_fault_of_its_own)
{
    // The observer's fault, on the exception thread, takes its course: the
    // process ends with its signal before the illegal instruction's.
    ChildResult child = RunChild("observe-fault-in-observer");
    CHECK(child.Said("faulting observer called"));
    CHECK(child.KilledBy(SIGSEGV));
    CHECK(child.Output.find("observed") == std::string::npos);
}

TEST(a_handler_that_could_not_be_installed_for_every_signal_is_completed_later)
{
    ChildResult child = RunChild("install-fails-partway");
    if (!child.KilledBy(SIGSEGV)) printf("  child install-fails-partway: status 0x%x, hung %d, said:\n%s", child.Status, child.Hung, child.Output.c_str());
    CHECK(child.KilledBy(SIGSEGV) && !child.Said("FAIL"));
    // Told once: the handler was not saved as the one before itself.
    CHECK(child.Count("observed bad access, signal 11, unguarded") == 1);
}

TEST(a_sent_signal_ends_the_process_as_it_would_without_bg3mse)
{
    ChildResult control = RunChild("sent-unobserved");
    CHECK(control.KilledBy(SIGSEGV) && !control.Said("survived"));

    struct
    {
        char const* Name;
        int Signal;
        char const* Told;
    } const cases[] = {
        { "sent-segv-by-raise", SIGSEGV, "observed bad access, signal 11, unguarded" },
        { "sent-bus-by-kill", SIGBUS, "observed bad access, signal 10, unguarded" },
        { "sent-ill-by-pthread-kill", SIGILL, "observed illegal instruction, signal 4, unguarded" },
        { "sent-trap-by-raise", SIGTRAP, "observed breakpoint trap, signal 5, unguarded" },
    };
    for (auto const& sent : cases) {
        ChildResult child = RunChild(sent.Name);
        if (!child.KilledBy(sent.Signal)) printf("  child %s: status 0x%x, said:\n%s", sent.Name, child.Status, child.Output.c_str());
        CHECK(child.KilledBy(sent.Signal) && !child.Said("survived") && !child.Said("FAIL"));
        CHECK(child.Count(sent.Told) == 1);
        if (sent.Signal == SIGSEGV) CHECK(child.Status == control.Status);
    }

    // Called by a handler installed later, as the game's calls it: the
    // process still ends, and nobody is called a second time.
    ChildResult later = RunChild("sent-behind-later-handler");
    if (!later.KilledBy(SIGSEGV)) printf("  child sent-behind-later-handler: status 0x%x, hung %d, said:\n%s", later.Status, later.Hung, later.Output.c_str());
    CHECK(later.KilledBy(SIGSEGV) && !later.Said("survived"));
    CHECK(later.Count("later handler") == 1);
    CHECK(later.Count("observed bad access, signal 11, unguarded") == 1);
}

TEST(the_handler_installed_before_is_called_as_the_kernel_would_call_it)
{
    ChildResult mask = RunChild("earlier-handlers-mask");
    if (!mask.Exited(0)) printf("  child earlier-handlers-mask: status 0x%x, said:\n%s", mask.Status, mask.Output.c_str());
    CHECK(mask.Exited(0));
    // Its sa_mask is blocked, and the signal itself unless it said SA_NODEFER.
    CHECK(mask.Said("earlier handler: signal 5 blocked, SIGUSR1 blocked, SIGUSR2 open"));
    CHECK(mask.Said("earlier handler: signal 8 open, SIGUSR1 blocked, SIGUSR2 open"));
    // Afterwards the thread has the mask it had.
    CHECK(mask.Said("after: SIGTRAP open, SIGFPE open, SIGUSR1 open"));
    // The observers were told of both signals: sent, and not ignored.
    CHECK(mask.Count("observed breakpoint trap, signal 5, unguarded") == 1);
}

TEST(the_handler_is_put_in_front_of_one_that_passes_nothing_on)
{
    auto show = [](char const* name, ChildResult const& child) {
        printf("  child %s: status 0x%x, hung %d, said:\n%s", name, child.Status, child.Hung, child.Output.c_str());
    };
    ChildResult control = RunChild("unguarded-null-read");

    // As in the game: a handler installed after bg3mse's that passes nothing
    // on. Behind it the observers are told nothing.
    ChildResult behind = RunChild("behind-a-handler-that-passes-nothing-on");
    CHECK(behind.KilledBy(SIGSEGV) && !behind.Said("survived"));
    CHECK(behind.Said("game handler: signal 11 at 0x10, pc at the fault"));
    CHECK(behind.Output.find("observed") == std::string::npos);

    // In front of it they are told first, and the handler gets the signal
    // as it came and ends the process its own way.
    ChildResult front = RunChild("put-in-front");
    if (!front.KilledBy(SIGSEGV) || front.Said("FAIL")) show("put-in-front", front);
    CHECK(front.KilledBy(SIGSEGV) && front.Status == control.Status && !front.Said("survived") && !front.Said("FAIL"));
    size_t observed = front.Output.find("observed bad access, signal 11, unguarded\n");
    size_t game = front.Output.find("game handler: signal 11 at 0x10, pc at the fault\n");
    CHECK(observed != std::string::npos && game != std::string::npos && observed < game);
    CHECK(front.Count("observed bad access, signal 11, unguarded") == 1 && front.Said("observed frame 0 in _fault_test_read_null"));
    CHECK(front.Count("game handler: signal 11 at 0x10, pc at the fault") == 1);

    // A handler that passes the signal on to bg3mse's does not send it round
    // and round: the second time through, it goes on to what was there first.
    ChildResult passes = RunChild("put-in-front-of-a-handler-that-passes-on");
    if (!passes.KilledBy(SIGSEGV)) show("put-in-front-of-a-handler-that-passes-on", passes);
    CHECK(passes.KilledBy(SIGSEGV) && !passes.Said("survived") && !passes.Said("FAIL"));
    CHECK(passes.Count("observed bad access, signal 11, unguarded") == 1 && passes.Count("later handler") == 1);

    ChildResult twice = RunChild("put-in-front-twice");
    if (!twice.KilledBy(SIGSEGV)) show("put-in-front-twice", twice);
    CHECK(twice.KilledBy(SIGSEGV) && !twice.Said("survived") && !twice.Said("FAIL"));
    CHECK(twice.Count("observed bad access, signal 11, unguarded") == 1 && twice.Count("later handler") == 1);
    CHECK(twice.Count("game handler: signal 11 at 0x10, pc at the fault") == 1);
    size_t later = twice.Output.find("later handler\n");
    game = twice.Output.find("game handler: ");
    observed = twice.Output.find("observed bad access");
    CHECK(observed < later && later < game);
}

TEST(a_handler_frame_that_was_jumped_out_of_is_not_taken_for_a_live_one)
{
    ChildResult child = RunChild("handler-below-jumps-out");
    if (!child.Exited(0)) printf("  child handler-below-jumps-out: status 0x%x, hung %d\n", child.Status, child.Hung);
    CHECK(child.Exited(0) && child.Said("jumped out a hundred times") && child.Said("survived"));
    CHECK(child.Count("earlier handler jumps out") == 101);
    CHECK(child.Count("observed breakpoint trap, signal 5, unguarded") == 101);
    CHECK(child.Count("later handler") == 1);
}

TEST(a_signal_that_was_ignored_before_stays_ignored_and_keeps_its_handler)
{
    ChildResult child = RunChild("ignored-before");
    if (!child.Exited(0)) printf("  child ignored-before: status 0x%x, said:\n%s", child.Status, child.Output.c_str());
    CHECK(child.Exited(0) && child.Said("survived"));
    CHECK(child.Output.find("observed") == std::string::npos);

    // A handler installed after bg3mse's is not taken out either.
    ChildResult later = RunChild("ignored-before-behind-later-handler");
    if (!later.Exited(0)) printf("  child ignored-before-behind-later-handler: status 0x%x, said:\n%s", later.Status, later.Output.c_str());
    CHECK(later.Exited(0) && later.Said("survived"));
    CHECK(later.Count("later handler") == 2);
    CHECK(later.Output.find("observed") == std::string::npos);
}

TEST(guard_cost)
{
    constexpr int kOuter = 20000, kNested = 1000000;
    int sink = 0;
    GuardedCall([&] { sink++; });
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kOuter; i++) GuardedCall([&] { sink++; });
    auto t1 = std::chrono::steady_clock::now();
    GuardedCall([&] {
        for (int i = 0; i < kNested; i++) GuardedCall([&] { sink++; });
    });
    auto t2 = std::chrono::steady_clock::now();
    double outer = std::chrono::duration<double, std::nano>(t1 - t0).count() / kOuter;
    double nested = std::chrono::duration<double, std::nano>(t2 - t1).count() / kNested;
    printf("guard cost: %.0f ns to enter and leave a thread's outermost guard, %.1f ns for a nested one\n", outer, nested);
    CHECK(sink == 1 + kOuter + kNested);
    CHECK(outer < 50000 && nested < 1000);
}

}

// sigaction for everything linked into this executable (the platform layer
// included; the system's libraries keep their own).
extern "C" int sigaction(int signal, struct sigaction const* action, struct sigaction* old)
{
    if (action && signal == gFailInstallingFor.load(std::memory_order_relaxed)) {
        gFailInstallingFor = 0;
        errno = EINVAL;
        return -1;
    }
    return gSystemSigaction(signal, action, old);
}

// mach_port_set_context for everything linked into this executable.
extern "C" kern_return_t mach_port_set_context(ipc_space_t task, mach_port_name_t name, mach_port_context_t context)
{
    int left = gFailSetContext.load(std::memory_order_relaxed);
    while (left > 0) {
        if (gFailSetContext.compare_exchange_weak(left, left - 1)) return KERN_FAILURE;
    }
    return gSystemSetContext(task, name, context);
}

// calloc for everything linked into this executable (the platform layer
// included; the system's libraries keep their own).
extern "C" void* calloc(size_t count, size_t size)
{
    pthread_t forbidden = gMustNotAllocate.load(std::memory_order_relaxed);
    if (forbidden && forbidden == pthread_self()) Say("allocated on a thread that must not");
    return malloc_zone_calloc(malloc_default_zone(), count, size);
}

int main(int argc, char** argv)
{
    gSystemSigaction = reinterpret_cast<decltype(gSystemSigaction)>(dlsym(RTLD_NEXT, "sigaction"));
    gSystemSetContext = reinterpret_cast<decltype(gSystemSetContext)>(dlsym(RTLD_NEXT, "mach_port_set_context"));
    if (!gSystemSigaction || !gSystemSetContext) return 1;
    SetLogStream(nullptr);
    if (argc == 3 && std::string_view(argv[1]) == "--child") {
        for (auto const& child : kChildCases) {
            if (std::string_view(argv[2]) == child.Name) return child.Run();
        }
        return 100;
    }

    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0) return 1;
    gSelf = path;

    void (*tests[])() = {
        read_memory_fails_on_unmapped_memory_instead_of_faulting_run,
        image_table_lists_this_executable_and_the_system_libraries_run,
        capture_of_a_stopped_thread_walks_its_frames_and_names_their_images_run,
        a_bad_access_below_a_stack_with_the_stack_pointer_at_its_bottom_is_a_stack_overflow_run,
        signals_map_to_kinds_and_addresses_run,
        symbols_come_from_each_images_own_table_run,
        many_frames_are_named_as_each_one_is_run,
        a_guard_recovers_from_a_null_read_run,
        a_guard_recovers_from_a_write_to_read_only_memory_run,
        a_guard_recovers_from_a_jump_to_memory_that_is_not_executable_run,
        a_guard_recovers_from_a_call_through_a_null_pointer_run,
        a_guard_recovers_from_an_illegal_instruction_run,
        a_guard_recovers_from_a_breakpoint_trap_run,
        a_guard_recovers_from_a_stack_overflow_run,
        a_guard_recovers_from_a_stack_overflow_on_the_main_thread_run,
        guards_nest_and_filters_are_asked_innermost_first_run,
        recovering_again_and_again_keeps_no_ports_run,
        a_declined_fault_ends_the_process_as_it_would_without_a_guard_run,
        a_fault_in_a_filter_ends_the_process_run,
        a_filter_may_use_guarded_calls_run,
        a_filters_guarded_call_allocates_nothing_run,
        threads_faulting_at_once_all_recover_run,
        a_fault_outside_any_guard_ends_the_process_while_another_thread_is_guarded_run,
        a_cpp_exception_leaves_the_guard_and_its_port_run,
        a_task_port_installed_later_sees_only_unguarded_faults_run,
        a_signal_handler_installed_later_sees_only_unguarded_faults_run,
        a_declined_fault_goes_to_the_thread_port_the_guard_replaced_run,
        a_guard_without_a_fault_leaves_the_thread_port_as_it_was_run,
        a_guard_left_in_a_forked_child_touches_none_of_the_childs_ports_run,
        a_thread_whose_port_cannot_be_set_up_leaves_no_port_names_behind_run,
        the_exception_threads_have_a_signal_mask_of_their_own_run,
        a_filter_that_does_not_answer_within_the_time_limit_is_given_up_on_run,
        an_observer_is_told_of_a_fatal_bad_access_and_the_process_dies_as_without_it_run,
        an_observer_is_told_of_a_fatal_illegal_instruction_run,
        an_observer_is_told_of_a_fatal_breakpoint_trap_run,
        an_observer_is_told_of_abort_run,
        an_observer_is_told_of_a_fault_every_guard_declined_run,
        an_observer_is_not_told_of_recovered_faults_run,
        a_removed_observer_is_not_told_run,
        a_handler_installed_later_that_passes_signals_on_runs_before_the_observers_run,
        a_stack_overflow_outside_any_guard_is_told_only_on_a_thread_with_an_alternate_signal_stack_run,
        two_threads_faulting_at_once_outside_guards_end_the_process_once_told_run,
        a_fault_in_a_filter_is_not_sent_to_the_observers_and_ends_the_process_run,
        a_fault_in_an_observer_ends_the_process_as_a_fault_of_its_own_run,
        a_handler_that_could_not_be_installed_for_every_signal_is_completed_later_run,
        a_sent_signal_ends_the_process_as_it_would_without_bg3mse_run,
        the_handler_installed_before_is_called_as_the_kernel_would_call_it_run,
        the_handler_is_put_in_front_of_one_that_passes_nothing_on_run,
        a_handler_frame_that_was_jumped_out_of_is_not_taken_for_a_live_one_run,
        a_signal_that_was_ignored_before_stays_ignored_and_keeps_its_handler_run,
        guard_cost_run,
    };
    for (auto test : tests) test();
    printf("%zu tests, %d failures\n", std::size(tests), gFailures);
    return gFailures ? 1 : 0;
}
