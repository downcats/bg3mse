#include <Crash/CrashReporter.h>
#include <Crash/ThreadList.h>

#include <Platform/FaultCapture.h>
#include <Platform/FaultGuard.h>
#include <Platform/FaultObserver.h>
#include <Platform/FunctionExtents.h>
#include <Platform/Log.h>
#include <Release/Version.h>

#include <CoreFoundation/CoreFoundation.h>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cxxabi.h>
#include <exception>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <mach/vm_statistics.h>
#include <pthread.h>
#include <string>
#include <typeinfo>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <unwind.h>

extern "C" {

// Crash/GuardedBlockCall.S
void bg3mse_guarded_block_call(void (*fn)(void*), void* arg);
void bg3mse_crash_capture_state(arm_thread_state64_t* out);

__attribute__((visibility("hidden")))
_Unwind_Reason_Code bg3mse_guarded_block_personality(int version, _Unwind_Action actions, uint64_t exceptionClass,
    _Unwind_Exception* exception, _Unwind_Context* context);

}

// What bg3mse_crash_capture_state writes.
static_assert(offsetof(arm_thread_state64_t, __x) == 0 && offsetof(arm_thread_state64_t, __fp) == 232);
static_assert(offsetof(arm_thread_state64_t, __lr) == 240 && offsetof(arm_thread_state64_t, __sp) == 248);
static_assert(offsetof(arm_thread_state64_t, __pc) == 256 && offsetof(arm_thread_state64_t, __cpsr) == 264);
static_assert(sizeof(arm_thread_state64_t) == 272);

namespace bg3se {

std::atomic<uint32_t> gDisableCrashReportingCount{ 0 };

std::unordered_set<void const*> gRegisteredTrampolines;

}

namespace bg3se::mac::crash {

namespace {

// Norbyte's BacktraceDumper::MaxBacktraceLines and MaxLuaFullBacktraceLines.
constexpr unsigned kMaxBacktraceLines = 12;
// The set of registered functions is read while other threads may be adding
// to it: a walk that goes round in circles ends here.
constexpr size_t kMaxTrampolines = 1 << 16;
constexpr size_t kMaxExcludedFunctions = 64;
constexpr size_t kErrorLogLimit = 0x10000;

enum class Cause : uint8_t
{
    Fault,
    ExceptionLeftGuardedBlock,
    // std::terminate on a thread outside any guarded block: an exception
    // nothing caught, or a call of std::terminate itself.
    Terminate,
};

char const* CauseName(Cause cause)
{
    switch (cause) {
    case Cause::Fault: return "fault";
    case Cause::ExceptionLeftGuardedBlock: return "c++ exception left a guarded block";
    case Cause::Terminate: return "std::terminate";
    }
    return "?";
}

struct Paths
{
    char Directory[PATH_MAX];
    char ReportTemp[PATH_MAX];
    char BacktraceTemp[PATH_MAX];
    char StackTemp[PATH_MAX];
    // Made when a report is written: they carry the time of the crash.
    char Report[PATH_MAX];
    char Backtrace[PATH_MAX];
    char Stack[PATH_MAX];
};

struct Lock
{
    explicit Lock(pthread_mutex_t& mutex) : Mutex(mutex) { pthread_mutex_lock(&Mutex); }
    ~Lock() { pthread_mutex_unlock(&Mutex); }
    pthread_mutex_t& Mutex;
};

// Start, Stop and Prepare, none of which runs at the time of a crash.
constinit pthread_mutex_t gSetupMutex = PTHREAD_MUTEX_INITIALIZER;
constinit std::atomic<bool> gPrepared{ false };
constinit std::atomic<bool> gStarted{ false };
// The reporter has been started in this process: its observer is added, and
// stays (see Stop in the header).
constinit std::atomic<bool> gObserving{ false };

// Set by Prepare and not changed after.
constinit Paths gPaths{};
// Local time's distance from UTC, in seconds, asked when the reporter is
// prepared: at the time of a crash the C library's time zone code, which
// locks and allocates, cannot be asked.
constinit long gUtcOffset = 0;
constinit char gSystem[2048]{};
constinit size_t gSystemSize = 0;
constinit FunctionTable const* gFunctions = nullptr;      // of this image

// One report is written at a time; a second fault meanwhile is not reported,
// and waits before it ends the process (BeginWriting). Everything below
// belongs to whoever holds this.
constinit std::atomic<bool> gWriting{ false };
constinit char gTrace[32 * 1024]{};
constinit char gLua[64 * 1024]{};
constinit char gScratch[512 * 1024]{};
alignas(8) constinit uint8_t gCommands[32 * 1024]{};
ImageTable gImages;
FaultCapture gCapture;
FaultRecord gRecord;
ThreadList gThreads;
constinit char gThreadText[64 * 1024]{};
// How long the other threads' frames may take to name, in mach_absolute_time
// units: the faulting thread waits for the reporter for at most 30 seconds
// (Platform/FaultObserver.h), and the report is renamed into place last.
constexpr uint64_t kThreadNamingSeconds = 8;
constinit uint64_t gThreadNamingTicks = 0;

// The process has written its report. It writes one: the same fault comes by
// again (an outer block's filter, the observer, the abort after an
// exception), and a fault after it is one the directory has no room for.
constinit std::atomic<bool> gReported{ false };

// The fault last written to the log, by its thread and pc: the same fault
// comes by more than once (the filter of each block it is in, then the
// observer), and is one line.
constinit std::atomic<uint64_t> gSaidThread{ 0 };
constinit std::atomic<uint64_t> gSaidPC{ 0 };

constinit char gErrorLog[kErrorLogLimit + 4096]{};
constinit std::atomic<size_t> gErrorLogSize{ 0 };

bool Force()
{
    return gHost.ForceCrashReporting && gHost.ForceCrashReporting();
}

// A report is being written from here to EndWriting. False when another is:
// the caller's fault is not reported. The hold keeps a fault that would end
// the process meanwhile, a second thread's or the abort that follows this
// one, waiting until the report has its final name (Platform/FaultObserver.h).
bool BeginWriting()
{
    if (gWriting.exchange(true, std::memory_order_acquire)) return false;
    HoldFatalFaults();
    return true;
}

void EndWriting()
{
    ReleaseFatalFaults();
    gWriting.store(false, std::memory_order_release);
}

// --- Norbyte's rule -------------------------------------------------------------

bool InFunctionAt(void const* start, uint64_t address)
{
    FunctionExtent extent = gFunctions->At(reinterpret_cast<uint64_t>(start));
    return extent.Ok() && address >= extent.Start && address < extent.End;
}

// Whether `address` is in one of the functions whose frames do not count. The
// walk of gRegisteredTrampolines is guarded: another thread may be changing
// the set, and the faulting thread may have been.
bool InExcludedFunction(uint64_t address)
{
    if (!gFunctions) return false;
    bool excluded = false;
    GuardedCall([&] {
        if (gHost.ExcludedFunctions) {
            void const* fixed[kMaxExcludedFunctions];
            size_t count = gHost.ExcludedFunctions(fixed, kMaxExcludedFunctions);
            for (size_t i = 0; i < count && i < kMaxExcludedFunctions; i++) {
                if (InFunctionAt(fixed[i], address)) {
                    excluded = true;
                    return;
                }
            }
        }
        size_t seen = 0;
        for (void const* trampoline : gRegisteredTrampolines) {
            if (++seen > kMaxTrampolines) break;
            if (InFunctionAt(trampoline, address)) {
                excluded = true;
                return;
            }
        }
    });
    return excluded;
}

// What Norbyte's rule makes of a crash: his reporter reports it, or leaves it
// to the game's for one of three reasons.
enum class Rule : uint8_t
{
    Reports,
    NativeApiUsed,
    StackOverflow,
    ReportingDisabled,
    NoFrameOfTheExtender,
};

char const* RuleName(Rule rule)
{
    switch (rule) {
    case Rule::Reports: return "reports";
    case Rule::NativeApiUsed: return "a native module uses the extender's API";
    case Rule::StackOverflow: return "a stack overflow";
    case Rule::ReportingDisabled: return "crash reporting disabled, as around a call into the game";
    case Rule::NoFrameOfTheExtender: return "no frame of the extender";
    }
    return "?";
}

// CrashReporter::IsExtensionRelatedCrash.
//
// `first`: the frames before it are the reporter's own, on a thread that
// reports on itself, and are not looked at.
Rule RuleOn(FaultRecord const& fault, bool inGuardedBlock, uint32_t first = 0)
{
    // ShouldReportCrashes, which his filter asks before anything else.
    if (gHost.NativeApiUsed && gHost.NativeApiUsed()) return Rule::NativeApiUsed;
    bool force = Force();
    // "Osiris is riddled with various stack overflow issues, skip reporting those"
    if (fault.Kind == FaultKind::StackOverflow && !force) return Rule::StackOverflow;
    if (gDisableCrashReportingCount.load(std::memory_order_relaxed) > 0 && !force) return Rule::ReportingDisabled;
    if (force) return Rule::Reports;
    // On Windows the block's filter is itself a frame of the extender at the
    // top of the stack the reporter walks.
    if (inGuardedBlock) return Rule::Reports;

    for (uint32_t i = first; i < fault.FrameCount; i++) {
        auto const& frame = fault.Frames[i];
        if (!(frame.Flags & FrameInThisImage)) continue;
        // "Only exclude crashes where the IP itself is not in a trampoline".
        // The others are return addresses, which can be the address after
        // the function's last instruction: the call is one before.
        if (i > first && InExcludedFunction(frame.PC - 1)) continue;
        return Rule::Reports;
    }
    return Rule::NoFrameOfTheExtender;
}

// --- The C++ exception of a std::terminate -----------------------------------------
//
// What CrashReporter::OnTerminate cannot say on Windows, where the minidump
// carries the exception: its type and what(), for MFLT. Taken on the thread
// that ends, which is running (not stopped by another): the C++ runtime's
// own terminate handler, which runs next, demangles the same name and calls
// what() there. The name is kept as the runtime has it before it is
// demangled, which allocates; what() is an exception's own code, and is
// called in a guarded call. Held by whoever holds gWriting.

struct ExceptionNote
{
    bool Present;       // a C++ exception was the thread's current one
    bool HasWhat;       // it is a std::exception
    char Type[512];
    char What[2048];
};
constinit ExceptionNote gException{};

// Text for one "key=value" line: what does not fit is dropped, and a line
// break or another control character becomes a blank.
void CopyLine(char* out, size_t size, char const* text)
{
    size_t i = 0;
    for (; text[i] && i + 1 < size; i++) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        out[i] = c < 0x20 || c == 0x7f ? ' ' : static_cast<char>(c);
    }
    out[i] = '\0';
}

void NoteCurrentException()
{
    gException = {};
    // Null for no exception, and for one that is not C++'s.
    std::type_info const* type = abi::__cxa_current_exception_type();
    if (!type) return;
    gException.Present = true;
    char const* mangled = type->name();
    GuardedCall([&] { CopyLine(gException.Type, sizeof(gException.Type), mangled); });
    GuardedCall([&] {
        int status = -1;
        char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
        if (status == 0 && demangled) CopyLine(gException.Type, sizeof(gException.Type), demangled);
        free(demangled);
    });
    // The current exception is this thread's caught one: thrown again and
    // caught here, it stays the current one.
    std::exception const* standard = nullptr;
    try {
        throw;
    } catch (std::exception const& e) {
        standard = &e;
    } catch (...) {
    }
    if (standard) {
        gException.HasWhat = GuardedCall([&] {
            char const* what = standard->what();
            CopyLine(gException.What, sizeof(gException.What), what ? what : "");
        });
    }
}

// --- The report -----------------------------------------------------------------

struct ImageDetails
{
    uint64_t Slide;
    uint64_t TextSize;
    bool HasUuid;
    uint8_t Uuid[16];
};

// An image's slide, the size of its __TEXT and its LC_UUID, from its header
// and load commands read with checked reads.
bool ReadImageDetails(uint64_t base, ImageDetails& out)
{
    out = {};
    auto mh = reinterpret_cast<mach_header_64*>(gCommands);
    if (!ReadMemory(base, mh, sizeof(*mh)) || mh->magic != MH_MAGIC_64) return false;
    if (mh->sizeofcmds > sizeof(gCommands) - sizeof(*mh)) return false;
    if (!ReadMemory(base + sizeof(*mh), mh + 1, mh->sizeofcmds)) return false;

    bool haveText = false;
    auto p = reinterpret_cast<uint8_t const*>(mh + 1);
    auto end = p + mh->sizeofcmds;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        auto lc = reinterpret_cast<load_command const*>(p);
        if (p + sizeof(*lc) > end || lc->cmdsize < sizeof(*lc) || p + lc->cmdsize > end) break;
        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(segment_command_64)) {
            auto seg = reinterpret_cast<segment_command_64 const*>(lc);
            if (!haveText && strncmp(seg->segname, SEG_TEXT, sizeof(seg->segname)) == 0) {
                out.Slide = base - seg->vmaddr;
                out.TextSize = seg->vmsize;
                haveText = true;
            }
        } else if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(uuid_command)) {
            memcpy(out.Uuid, reinterpret_cast<uuid_command const*>(lc)->uuid, sizeof(out.Uuid));
            out.HasUuid = true;
        }
        p += lc->cmdsize;
    }
    return haveText;
}

char const* BaseName(char const* path)
{
    char const* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// The function a frame is in, by the nearest symbol before it
// (SymbolizeFrame). In this image, whose local symbols a release strips
// (Mac/Release/Symbols.cmake), the nearest symbol left may be another
// function's: it names the frame only when the image's function starts say
// that a function starts at the symbol and runs to the frame's address (or
// to just before it, for a return address after a call that does not return).
bool NamesFrame(FaultFrame const& frame, FrameSymbol const& symbol)
{
    // Before the reporter is prepared there is no table to ask (a fault said
    // in the log from a guarded block): the symbol is taken as it is.
    if (!(frame.Flags & FrameInThisImage) || !gFunctions) return true;
    uint64_t start = frame.PC - symbol.Offset;
    FunctionExtent extent = gFunctions->At(start);
    return extent.Ok() && extent.Start == start && frame.PC <= extent.End;
}

// `returnAddress`: the frame is not the first, and its address is the
// instruction after a call. It is looked up one byte before, so that a call
// that does not return, the last instruction of its function, is named by
// that function and not by the one after it; the offset is from the address.
bool NameFrame(FaultFrame const& frame, FrameSymbol& symbol, bool returnAddress)
{
    FaultFrame looked = frame;
    if (returnAddress && looked.PC) looked.PC--;
    if (!SymbolizeFrame(looked, symbol)) return false;
    symbol.Offset += frame.PC - looked.PC;
    return NamesFrame(frame, symbol);
}

// A frame as Norbyte's BacktraceDumper::TryGetFunctionName writes it: by name
// in the extender and in the game, whose symbols he reads from symbol tables
// shipped beside the DLL and which here are in the images themselves; by
// module and address in the file elsewhere.
//
// `found`: the frame's symbol when it was looked for already (another
// thread's frame, ThreadList), or nullptr to look for it here.
void AppendTraceLine(TextBuffer& text, FaultFrame const& frame, bool returnAddress, FrameSymbol const* found = nullptr,
    bool lookedFor = false)
{
    if (frame.Flags & (FrameInThisImage | FrameInMainExecutable)) {
        FrameSymbol symbol{};
        bool named = lookedFor ? found && NamesFrame(frame, *found) : NameFrame(frame, symbol, returnAddress);
        if (named) {
            if (lookedFor) symbol = *found;
            text.Appendf("%s+0x%llx\n", symbol.Name, (unsigned long long)symbol.Offset);
            return;
        }
    }
    ImageDetails details;
    if (frame.ImageBase && frame.ImagePath && ReadImageDetails(frame.ImageBase, details)) {
        text.Appendf("%s!0x%llx\n", BaseName(frame.ImagePath), (unsigned long long)(frame.PC - details.Slide));
        return;
    }
    text.Appendf("Unknown!0x%016llx\n", (unsigned long long)frame.PC);
}

// The system's lowest layer, where a thread is stopped by a signal it sends
// itself (abort: __pthread_kill, pthread_kill): what ntdll.dll and
// kernelbase.dll are to Norbyte's backtrace, whose first frames in them he
// leaves out.
bool InSystemsLowestLayer(FaultFrame const& frame)
{
    if (!frame.ImagePath) return false;
    char const* name = BaseName(frame.ImagePath);
    return strcmp(name, "libsystem_kernel.dylib") == 0 || strcmp(name, "libsystem_pthread.dylib") == 0;
}

// BacktraceDumper::DumpBacktrace: the first lines of the backtrace, for the
// TRAC section and for the text shown to the user. "Remove ntdll/kernelbase
// error handler frames from the top of the stack": here the system's lowest
// layer, until a frame is written.
void BuildTrace(FaultRecord const& fault, TextBuffer& text)
{
    unsigned lines = 0;
    for (uint32_t i = 0; i < fault.FrameCount; i++) {
        if (lines == 0 && InSystemsLowestLayer(fault.Frames[i])) continue;
        AppendTraceLine(text, fault.Frames[i], i > 0);
        if (lines++ >= kMaxBacktraceLines) {
            text.Append("(...)\n");
            break;
        }
    }
}

char const* OriginName(FaultOrigin origin)
{
    switch (origin) {
    case FaultOrigin::Guarded: return "guarded";
    case FaultOrigin::Unguarded: return "unguarded";
    case FaultOrigin::DeclinedByGuards: return "declined by guards";
    }
    return "?";
}

// `stack`: the stack file's header, when the file was written.
void BuildFault(FaultRecord const& fault, Cause cause, Rule rule, StackFileHeader const* stack, TextBuffer& text)
{
    text.Appendf("cause=%s\n", CauseName(cause));
    text.Appendf("extender_related=%s\n", rule == Rule::Reports ? "yes" : "no");
    text.Appendf("norbytes_rule=%s\n", RuleName(rule));
    text.Appendf("crash_reporting_disabled=%u\n", gDisableCrashReportingCount.load(std::memory_order_relaxed));
    text.Appendf("kind=%s\n", FaultKindName(fault.Kind));
    text.Appendf("origin=%s\n", OriginName(fault.Origin));
    text.Appendf("signal=%d\n", fault.Signal);
    text.Appendf("mach_exception=%u\n", fault.MachException);
    text.Appendf("code=0x%llx,0x%llx\n", (unsigned long long)fault.Code[0], (unsigned long long)fault.Code[1]);
    text.Appendf("address=0x%llx\n", (unsigned long long)fault.Address);
    text.Appendf("pc=0x%llx\n", (unsigned long long)fault.PC);
    text.Appendf("thread_id=%llu\n", (unsigned long long)fault.ThreadId);
    text.Appendf("thread_port=0x%x\n", fault.Thread);
    text.Appendf("stack_low=0x%llx\n", (unsigned long long)fault.StackLow);
    text.Appendf("stack_high=0x%llx\n", (unsigned long long)fault.StackHigh);
    if (stack) {
        text.Appendf("stack_file=%s\n", BaseName(gPaths.Stack));
        text.Appendf("stack_base=0x%llx\n", (unsigned long long)stack->Base);
        text.Appendf("stack_length=0x%llx\n", (unsigned long long)stack->Length);
        text.Appendf("stack_flags=%s%s%s%s\n", stack->Flags & StackBoundsKnown ? "B" : "-", stack->Flags & StackCutAtLimit ? "C" : "-",
            stack->Flags & StackEndsUnreadable ? "U" : "-", stack->Flags & StackStartsLater ? "S" : "-");
    } else {
        text.Append("stack_file=none\n");
    }
    // The exception, for a thread that ends in std::terminate: its type and,
    // for a std::exception, what(); "none" when there was no C++ exception.
    if (cause != Cause::Fault) {
        text.Appendf("exception=%s\n", gException.Present ? gException.Type : "none");
        if (gException.HasWhat) text.Appendf("exception_what=%s\n", gException.What);
    }
}

void BuildRegisters(FaultRecord const& fault, TextBuffer& text)
{
    auto const& state = fault.Registers;
    for (int i = 0; i < 29; i++) text.Appendf("x%d=0x%016llx\n", i, (unsigned long long)state.__x[i]);
    text.Appendf("fp=0x%016llx\n", (unsigned long long)arm_thread_state64_get_fp(state));
    text.Appendf("lr=0x%016llx\n", (unsigned long long)arm_thread_state64_get_lr(state));
    text.Appendf("sp=0x%016llx\n", (unsigned long long)arm_thread_state64_get_sp(state));
    text.Appendf("pc=0x%016llx\n", (unsigned long long)arm_thread_state64_get_pc(state));
    text.Appendf("cpsr=0x%08x\n", state.__cpsr);
    if (fault.HasExceptionState) {
        text.Appendf("far=0x%016llx\n", (unsigned long long)fault.ExceptionState.__far);
        text.Appendf("esr=0x%08x\n", fault.ExceptionState.__esr);
    }

    // The SIMD and floating-point registers, as a minidump's context has
    // them, when they are still the fault's: the faulting thread is another
    // one, stopped in its Mach exception. A fault seen as a signal has its
    // thread in the handler by now, and a thread reporting on itself has
    // used them since.
    if (fault.MachException == 0 || fault.HasExceptionState || fault.Thread == pthread_mach_thread_np(pthread_self())) return;
    arm_neon_state64_t neon{};
    mach_msg_type_number_t count = ARM_NEON_STATE64_COUNT;
    if (thread_get_state(fault.Thread, ARM_NEON_STATE64, reinterpret_cast<thread_state_t>(&neon), &count) != KERN_SUCCESS) return;
    for (int i = 0; i < 32; i++) {
        text.Appendf("q%d=0x%016llx%016llx\n", i, (unsigned long long)(neon.__v[i] >> 64), (unsigned long long)neon.__v[i]);
    }
    text.Appendf("fpsr=0x%08x\n", neon.__fpsr);
    text.Appendf("fpcr=0x%08x\n", neon.__fpcr);
}

void BuildBacktrace(FaultRecord const& fault, TextBuffer& text)
{
    for (uint32_t i = 0; i < fault.FrameCount; i++) {
        auto const& frame = fault.Frames[i];
        text.Appendf("frame=%u pc=0x%llx image=0x%llx flags=%c%c%c symbol=", i, (unsigned long long)frame.PC,
            (unsigned long long)frame.ImageBase,
            frame.Flags & FrameFromLinkRegister ? 'L' : '-',
            frame.Flags & FrameInMainExecutable ? 'M' : '-',
            frame.Flags & FrameInThisImage ? 'E' : '-');
        FrameSymbol symbol{};
        if (NameFrame(frame, symbol, i > 0)) {
            text.Appendf("%s+0x%llx\n", symbol.Name, (unsigned long long)symbol.Offset);
        } else {
            text.Append("-\n");
        }
    }
}

// --- The other threads (ThreadList) -------------------------------------------------

void AppendThreadLine(TextBuffer& text, uint32_t index, ThreadEntry const& thread)
{
    text.Appendf("thread=%u id=%llu port=0x%x role=%s captured=%s run_state=%d suspend_count=%d frames=%u name=%s\n", index,
        (unsigned long long)thread.Id, thread.Port, ThreadRoleName(thread.Role), thread.Captured ? "yes" : "no", thread.RunState,
        thread.SuspendCount, thread.FrameCount, thread.HasName ? thread.Name : "");
}

// One thread's lines of MTHR.
void BuildThread(TextBuffer& text, uint32_t index)
{
    auto const& thread = gThreads.At(index);
    AppendThreadLine(text, index, thread);
    if (thread.QueueAddress) {
        text.Appendf("queue thread=%u address=0x%llx label=%s\n", index, (unsigned long long)thread.QueueAddress,
            thread.HasQueue ? thread.Queue : "");
    }
    if (!thread.Captured) return;
    auto const& state = thread.Registers;
    text.Appendf("registers thread=%u", index);
    for (int i = 0; i < 29; i++) text.Appendf(" x%d=0x%016llx", i, (unsigned long long)state.__x[i]);
    text.Appendf(" fp=0x%016llx lr=0x%016llx sp=0x%016llx pc=0x%016llx cpsr=0x%08x\n",
        (unsigned long long)arm_thread_state64_get_fp(state), (unsigned long long)arm_thread_state64_get_lr(state),
        (unsigned long long)arm_thread_state64_get_sp(state), (unsigned long long)arm_thread_state64_get_pc(state), state.__cpsr);
    for (uint32_t k = 0; k < thread.FrameCount; k++) {
        auto const& frame = gThreads.Frame(thread, k);
        text.Appendf("frame thread=%u index=%u pc=0x%llx image=0x%llx flags=%c%c%c symbol=", index, k, (unsigned long long)frame.PC,
            (unsigned long long)frame.ImageBase,
            frame.Flags & FrameFromLinkRegister ? 'L' : '-',
            frame.Flags & FrameInMainExecutable ? 'M' : '-',
            frame.Flags & FrameInThisImage ? 'E' : '-');
        FrameSymbol symbol{};
        if (gThreads.Symbol(thread, k, symbol) && NamesFrame(frame, symbol)) {
            text.Appendf("%s+0x%llx\n", symbol.Name, (unsigned long long)symbol.Offset);
        } else {
            text.Append("-\n");
        }
    }
}

// One thread's part of the backtrace text: a line naming it, then its
// frames as the crashing thread's are written, as many as BuildTrace writes.
void BuildThreadTrace(TextBuffer& text, uint32_t index)
{
    auto const& thread = gThreads.At(index);
    text.Appendf("\nThread %u", index);
    if (thread.HasName) text.Appendf(" \"%s\"", thread.Name);
    if (thread.HasQueue) text.Appendf(", queue \"%s\"", thread.Queue);
    text.Appendf(", id %llu", (unsigned long long)thread.Id);
    switch (thread.Role) {
    case ThreadRole::Crashed: text.Append(": the crashing thread, above\n"); return;
    case ThreadRole::Reporter: text.Append(": the thread that wrote this report\n"); return;
    case ThreadRole::FaultHandling: text.Append(": bg3mse's fault handling\n"); return;
    case ThreadRole::Other: break;
    }
    if (!thread.Captured) {
        text.Append(": not read\n");
        return;
    }
    text.Append(":\n");
    for (uint32_t k = 0; k < thread.FrameCount; k++) {
        if (k > kMaxBacktraceLines) {
            text.Append("(...)\n");
            break;
        }
        FrameSymbol symbol{};
        bool found = gThreads.Symbol(thread, k, symbol);
        AppendTraceLine(text, gThreads.Frame(thread, k), k > 0, found ? &symbol : nullptr, true);
    }
}

void BuildImages(TextBuffer& text)
{
    if (!gImages.Read()) return;
    uint64_t self = ThisImageBase();
    for (size_t i = 0; i < gImages.Count(); i++) {
        auto const& image = gImages.At(i);
        ImageDetails details;
        if (!ReadImageDetails(image.Base, details)) continue;
        text.Appendf("base=0x%llx slide=0x%llx size=0x%llx uuid=", (unsigned long long)image.Base,
            (unsigned long long)details.Slide, (unsigned long long)details.TextSize);
        if (details.HasUuid) {
            auto u = details.Uuid;
            text.Appendf("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        } else {
            text.Append("-");
        }
        text.Appendf(" flags=%c%c path=%s\n", image.MainExecutable ? 'M' : '-', image.Base == self ? 'E' : '-',
            image.Path ? image.Path : "");
    }
}

// --- The memory map ------------------------------------------------------------------
//
// What a minidump has of the process's memory beyond the stack: here, the
// region each address of the fault is in (the faulting address, pc, lr, sp,
// fp, and the general registers that point into mapped memory), and the
// whole address space summed up by the tag the system gives each region (as
// vmmap's summary does). Asked of the kernel, which reads the task's map; it
// allocates nothing.

char const* TagName(unsigned tag)
{
    switch (tag) {
    case 0: return "none";
#define BG3MSE_TAG(name) case VM_MEMORY_##name: return #name;
    BG3MSE_TAG(MALLOC) BG3MSE_TAG(MALLOC_SMALL) BG3MSE_TAG(MALLOC_LARGE) BG3MSE_TAG(MALLOC_HUGE) BG3MSE_TAG(SBRK)
    BG3MSE_TAG(REALLOC) BG3MSE_TAG(MALLOC_TINY) BG3MSE_TAG(MALLOC_LARGE_REUSABLE) BG3MSE_TAG(MALLOC_LARGE_REUSED)
    BG3MSE_TAG(ANALYSIS_TOOL) BG3MSE_TAG(MALLOC_NANO) BG3MSE_TAG(MALLOC_MEDIUM) BG3MSE_TAG(MACH_MSG) BG3MSE_TAG(IOKIT)
    BG3MSE_TAG(STACK) BG3MSE_TAG(GUARD) BG3MSE_TAG(SHARED_PMAP) BG3MSE_TAG(DYLIB) BG3MSE_TAG(OBJC_DISPATCHERS)
    BG3MSE_TAG(UNSHARED_PMAP) BG3MSE_TAG(APPKIT) BG3MSE_TAG(FOUNDATION) BG3MSE_TAG(COREGRAPHICS) BG3MSE_TAG(CORESERVICES)
    BG3MSE_TAG(JAVA) BG3MSE_TAG(COREDATA) BG3MSE_TAG(ATS) BG3MSE_TAG(LAYERKIT) BG3MSE_TAG(CGIMAGE) BG3MSE_TAG(TCMALLOC)
    BG3MSE_TAG(COREGRAPHICS_DATA) BG3MSE_TAG(COREGRAPHICS_SHARED) BG3MSE_TAG(COREGRAPHICS_FRAMEBUFFERS)
    BG3MSE_TAG(COREGRAPHICS_BACKINGSTORES) BG3MSE_TAG(DYLD) BG3MSE_TAG(DYLD_MALLOC) BG3MSE_TAG(SQLITE) BG3MSE_TAG(GLSL)
    BG3MSE_TAG(OPENCL) BG3MSE_TAG(COREIMAGE) BG3MSE_TAG(IMAGEIO) BG3MSE_TAG(COREPROFILE) BG3MSE_TAG(ASSETSD)
    BG3MSE_TAG(OS_ALLOC_ONCE) BG3MSE_TAG(LIBDISPATCH) BG3MSE_TAG(ACCELERATE) BG3MSE_TAG(COREUI) BG3MSE_TAG(COREUIFILE)
    BG3MSE_TAG(CORPSEINFO) BG3MSE_TAG(ASL) BG3MSE_TAG(SWIFT_RUNTIME) BG3MSE_TAG(SWIFT_METADATA) BG3MSE_TAG(IOSURFACE)
    BG3MSE_TAG(LIBNETWORK) BG3MSE_TAG(AUDIO) BG3MSE_TAG(IOACCELERATOR) BG3MSE_TAG(COLORSYNC)
#undef BG3MSE_TAG
    }
    return nullptr;
}

char const* ShareName(unsigned char mode)
{
    switch (mode) {
    case SM_COW: return "cow";
    case SM_PRIVATE: return "private";
    case SM_EMPTY: return "empty";
    case SM_SHARED: return "shared";
    case SM_TRUESHARED: return "trueshared";
    case SM_PRIVATE_ALIASED: return "private_aliased";
    case SM_SHARED_ALIASED: return "shared_aliased";
    case SM_LARGE_PAGE: return "large_page";
    }
    return "?";
}

void AppendTag(TextBuffer& text, unsigned tag)
{
    if (char const* name = TagName(tag)) {
        text.Appendf("tag=%u(%s)", tag, name);
    } else {
        text.Appendf("tag=%u", tag);
    }
}

void AppendProtection(TextBuffer& text, vm_prot_t prot)
{
    text.Appendf("%c%c%c", prot & VM_PROT_READ ? 'r' : '-', prot & VM_PROT_WRITE ? 'w' : '-', prot & VM_PROT_EXECUTE ? 'x' : '-');
}

// The region at `address`, or the first after it. False when there is none.
bool RegionFrom(mach_vm_address_t& start, mach_vm_size_t& size, vm_region_submap_info_data_64_t& info)
{
    mach_vm_address_t from = start;
    natural_t depth = 0;
    for (int tries = 0; tries < 8; tries++) {
        start = from;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &start, &size, &depth, reinterpret_cast<vm_region_recurse_info_t>(&info),
                &count) != KERN_SUCCESS) {
            return false;
        }
        // A submap (the shared cache) holding the address: the region is in it.
        if (!info.is_submap || start > from) return true;
        depth++;
    }
    return true;
}

void DescribeAddress(TextBuffer& text, char const* what, uint64_t value, bool always)
{
    mach_vm_address_t start = value;
    mach_vm_size_t size = 0;
    vm_region_submap_info_data_64_t info{};
    bool found = RegionFrom(start, size, info);
    bool inside = found && value >= start && value - start < size && !info.is_submap;
    if (!inside) {
        if (!always) return;
        text.Appendf("where=%s value=0x%llx region=none", what, (unsigned long long)value);
        if (found) text.Appendf(" next=0x%llx", (unsigned long long)start);
        text.Append("\n");
        return;
    }
    text.Appendf("where=%s value=0x%llx region=0x%llx-0x%llx prot=", what, (unsigned long long)value, (unsigned long long)start,
        (unsigned long long)(start + size));
    AppendProtection(text, info.protection);
    text.Append("/");
    AppendProtection(text, info.max_protection);
    text.Appendf(" share=%s ", ShareName(info.share_mode));
    AppendTag(text, info.user_tag);
    if (auto image = gImages.Find(value)) {
        text.Appendf(" image=%s", image->Path ? BaseName(image->Path) : "?");
    }
    text.Append("\n");
}

struct TagTotal
{
    uint32_t Regions;
    uint64_t Virtual;
    uint64_t Resident;
};
constinit TagTotal gTagTotals[256]{};

void BuildMemoryMap(FaultRecord const& fault, TextBuffer& text)
{
    gImages.Read();
    auto const& state = fault.Registers;
    DescribeAddress(text, "address", fault.Address, true);
    DescribeAddress(text, "pc", arm_thread_state64_get_pc(state), true);
    DescribeAddress(text, "lr", arm_thread_state64_get_lr(state), true);
    DescribeAddress(text, "sp", arm_thread_state64_get_sp(state), true);
    DescribeAddress(text, "fp", arm_thread_state64_get_fp(state), true);
    for (int i = 0; i < 29; i++) {
        char name[8];
        snprintf(name, sizeof(name), "x%d", i);
        DescribeAddress(text, name, state.__x[i], false);
    }

    memset(gTagTotals, 0, sizeof(gTagTotals));
    TagTotal total{};
    mach_vm_address_t address = 0;
    natural_t depth = 0;
    // Bounded: a map that changes while it is walked could go round.
    for (unsigned regions = 0; regions < (1u << 20); regions++) {
        mach_vm_size_t size = 0;
        vm_region_submap_info_data_64_t info{};
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth, reinterpret_cast<vm_region_recurse_info_t>(&info),
                &count) != KERN_SUCCESS) {
            break;
        }
        if (info.is_submap) {
            depth++;
            continue;
        }
        auto& entry = gTagTotals[info.user_tag & 0xff];
        uint64_t resident = (uint64_t)info.pages_resident * vm_page_size;
        entry.Regions++;
        entry.Virtual += size;
        entry.Resident += resident;
        total.Regions++;
        total.Virtual += size;
        total.Resident += resident;
        if (address + size <= address) break;
        address += size;
    }
    for (unsigned tag = 0; tag < 256; tag++) {
        auto const& entry = gTagTotals[tag];
        if (!entry.Regions) continue;
        text.Append("summary ");
        AppendTag(text, tag);
        text.Appendf(" regions=%u virtual=0x%llx resident=0x%llx\n", entry.Regions, (unsigned long long)entry.Virtual,
            (unsigned long long)entry.Resident);
    }
    text.Appendf("total regions=%u virtual=0x%llx resident=0x%llx\n", total.Regions, (unsigned long long)total.Virtual,
        (unsigned long long)total.Resident);
}

// bg3mse's build: its version, the commit, the version of Norbyte's
// extender and the flavour (Release/Version.h), and the UUID of the image
// the reporter is in, which its dSYM carries.
void BuildBuild(TextBuffer& text)
{
    text.Appendf("version=%s\n", version::kVersion);
    text.Appendf("commit=%s\n", version::kCommit);
    text.Appendf("extender_version=%s\n", version::kExtenderVersion);
    text.Appendf("channel=%s\n", version::kChannel);
    ImageDetails details;
    if (ReadImageDetails(ThisImageBase(), details) && details.HasUuid) {
        auto u = details.Uuid;
        text.Appendf("uuid=%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X\n",
            u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    }
}

// Writes the report and the backtrace text for `fault`. The caller holds gWriting.
bool WriteFiles(FaultRecord const& fault, Cause cause, Rule rule)
{
    TextBuffer trace(gTrace), lua(gLua), scratch(gScratch), threadText(gThreadText);
    trace.Clear();
    lua.Clear();

    // The other threads first, nearest the time of the crash. Not in a
    // guarded call as a whole: a thread it has suspended is resumed by code
    // that has to run (ThreadList.h); what in it may fault is guarded inside.
    bool haveThreads = gThreads.Take(fault.Thread, gImages, mach_absolute_time() + gThreadNamingTicks);

    // As Norbyte's reporter guards its Lua and mod walks with __try: each
    // part that reads what the fault may have left broken runs in a guarded
    // call, and a fault in it loses that part only.
    GuardedCall([&] { BuildTrace(fault, trace); });
    bool haveLua = false;
    if (gHost.LuaBacktrace) {
        haveLua = GuardedCall([&] { gHost.LuaBacktrace(fault, lua); });
        if (!haveLua) lua.Clear();
    }

    // The three files' names, for this crash.
    char time[kReportTimeLength + 1];
    FormatReportTime(static_cast<int64_t>(::time(nullptr)), gUtcOffset, time);
    snprintf(gPaths.Report, sizeof(gPaths.Report), "%s/%s%s%s", gPaths.Directory, kReportPrefix, time, kReportSuffix);
    snprintf(gPaths.Backtrace, sizeof(gPaths.Backtrace), "%s/%s%s%s", gPaths.Directory, kReportPrefix, time, kBacktraceSuffix);
    snprintf(gPaths.Stack, sizeof(gPaths.Stack), "%s/%s%s%s", gPaths.Directory, kReportPrefix, time, kStackSuffix);

    // The stack memory first, so that the report can say what of it was written.
    StackFileHeader stack{};
    bool haveStack = false;
    ReportWriter stackFile;
    if (stackFile.Open(gPaths.StackTemp)) {
        bool read = false;
        GuardedCall([&] { read = WriteStackFile(stackFile, fault, stack); });
        if (stackFile.Close() && read && rename(gPaths.StackTemp, gPaths.Stack) == 0) {
            haveStack = true;
        } else {
            unlink(gPaths.StackTemp);
        }
    }

    bool written = false;
    ReportWriter report;
    if (report.Open(gPaths.ReportTemp)) {
        report.AddHeader();
        if (gHost.BuildSections) GuardedCall([&] { gHost.BuildSections(report); });
        report.AddSection(tag::StackTrace, trace.View());
        if (haveLua) report.AddSection(tag::LuaStackTrace, lua.View());
        if (gHost.ModuleList) {
            scratch.Clear();
            if (GuardedCall([&] { gHost.ModuleList(scratch); })) report.AddSection(tag::LoadedModules, scratch.View());
        }
        size_t logged = gErrorLogSize.load(std::memory_order_acquire);
        if (logged > sizeof(gErrorLog)) logged = sizeof(gErrorLog);
        if (logged > 0) report.AddSection(tag::ErrorLog, { gErrorLog, logged });
        if (gHost.StateSections) GuardedCall([&] { gHost.StateSections(report); });

        scratch.Clear();
        BuildFault(fault, cause, rule, haveStack ? &stack : nullptr, scratch);
        report.AddSection(tag::MacFault, scratch.View());
        scratch.Clear();
        BuildRegisters(fault, scratch);
        report.AddSection(tag::MacRegisters, scratch.View());
        scratch.Clear();
        GuardedCall([&] { BuildBacktrace(fault, scratch); });
        report.AddSection(tag::MacBacktrace, scratch.View());
        scratch.Clear();
        GuardedCall([&] { BuildImages(scratch); });
        report.AddSection(tag::MacImages, scratch.View());
        report.AddSection(tag::MacSystem, { gSystem, gSystemSize });
        scratch.Clear();
        GuardedCall([&] { BuildBuild(scratch); });
        report.AddSection(tag::MacBuild, scratch.View());
        scratch.Clear();
        GuardedCall([&] { BuildMemoryMap(fault, scratch); });
        report.AddSection(tag::MacMemory, scratch.View());

        // The other threads, one at a time: together they need not fit any buffer.
        if (haveThreads) {
            report.BeginSection(tag::MacThreads);
            threadText.Clear();
            threadText.Appendf("threads total=%u listed=%u max_frames=%u\n", gThreads.Total(), gThreads.Count(), ThreadList::kMaxFrames);
            report.Write(threadText.View());
            for (uint32_t i = 0; i < gThreads.Count(); i++) {
                threadText.Clear();
                GuardedCall([&] { BuildThread(threadText, i); });
                report.Write(threadText.View());
            }
            report.EndSection();
        }

        if (report.Close() && rename(gPaths.ReportTemp, gPaths.Report) == 0) {
            written = true;
        } else {
            unlink(gPaths.ReportTemp);
        }
    }

    // CrashReporter::CreateBacktraceFile.
    ReportWriter text;
    if (text.Open(gPaths.BacktraceTemp)) {
        text.Write(trace.View());
        if (!lua.Empty()) {
            text.Write("\r\n\r\nLua Backtrace:\r\n");
            text.Write(lua.View());
        }
        // Beyond Norbyte's text: every other thread, as a minidump has them.
        if (haveThreads) {
            threadText.Clear();
            threadText.Appendf("\r\n\r\nThreads (%u", gThreads.Total());
            if (gThreads.Count() < gThreads.Total()) threadText.Appendf(", the first %u", gThreads.Count());
            threadText.Append("):\n");
            text.Write(threadText.View());
            for (uint32_t i = 0; i < gThreads.Count(); i++) {
                threadText.Clear();
                GuardedCall([&] { BuildThreadTrace(threadText, i); });
                text.Write(threadText.View());
            }
        }
        if (!text.Close() || rename(gPaths.BacktraceTemp, gPaths.Backtrace) != 0) unlink(gPaths.BacktraceTemp);
    }

    // CrashReporterThread: only for a crash whose dump was written, and that
    // his rule reports. Another crash's report stays in the directory, and
    // the log says where; nobody is told of it at the next start.
    if (written) {
        if (rule == Rule::Reports) {
            LaunchCrashReporter(gPaths.Report);
        } else {
            char line[1024];
            snprintf(line, sizeof(line), "crash report written to %s (not the extender's crash: no notice)", gPaths.Report);
            LogFromFaultHandler(LogLevel::Error, line);
        }
    }
    return written;
}

// One line of the log for a fault that is going on to end the process: what
// it was and where. Formatted here and written with write(2): the faulting
// thread may hold the log's lock.
void SayFault(FaultRecord const& fault, bool inGuardedBlock, Rule rule)
{
    char where[512] = "";
    if (fault.FrameCount > 0) {
        auto const& frame = fault.Frames[0];
        // The symbol table is the image's own, read as it is in memory.
        GuardedCall([&] {
            char const* image = frame.ImagePath ? BaseName(frame.ImagePath) : "no image";
            FrameSymbol symbol{};
            if (NameFrame(frame, symbol, false)) {
                snprintf(where, sizeof(where), ", in %s+0x%llx (%s)", symbol.Name, (unsigned long long)symbol.Offset, image);
            } else {
                snprintf(where, sizeof(where), ", in %s", image);
            }
        });
    }

    char line[900];
    snprintf(line, sizeof(line), "fatal fault: %s at 0x%llx, pc 0x%llx%s, thread %llu%s%s%s%s", FaultKindName(fault.Kind),
        (unsigned long long)fault.Address, (unsigned long long)fault.PC, where, (unsigned long long)fault.ThreadId,
        inGuardedBlock ? ", in a guarded block" : "",
        rule == Rule::Reports ? "" : "; Norbyte's reporter would leave it to the game's (", rule == Rule::Reports ? "" : RuleName(rule),
        rule == Rule::Reports ? "" : ")");
    LogFromFaultHandler(LogLevel::Error, line);
}

// CrashReporter::OnUnhandledException, as far as it reports, and beyond it:
// a fault that comes by here is written to the log, and the process's first
// is written out as a report, whatever Norbyte's rule makes of it. What the
// rule makes of it is in both (see the header).
void ReportFault(FaultRecord const& fault, bool inGuardedBlock)
{
    // The same fault again: said, and reported if it could be.
    if (gSaidThread.load(std::memory_order_acquire) == fault.ThreadId && gSaidPC.load(std::memory_order_acquire) == fault.PC) return;
    gSaidThread.store(fault.ThreadId, std::memory_order_release);
    gSaidPC.store(fault.PC, std::memory_order_release);

    Rule rule = RuleOn(fault, inGuardedBlock);
    SayFault(fault, inGuardedBlock, rule);

    if (!gPrepared.load(std::memory_order_acquire) || gReported.load(std::memory_order_acquire)) return;
    if (!BeginWriting()) return;
    // Another thread may have written the report since the look above.
    if (!gReported.load(std::memory_order_acquire)) {
        WriteFiles(fault, Cause::Fault, rule);
        gReported.store(true, std::memory_order_release);
    }
    EndWriting();
}

// HandleGuardedException for a hardware fault. Runs on an exception thread.
// Whatever is decided, the fault goes on: on Windows the reporter ends the
// process itself or hands the exception to the game's top-level filter. The
// fault is said in the log whether or not the reporter was ever started; the
// report needs the reporter to have been started once in this process (Stop
// does not end that; see the header).
FaultDecision GuardedBlockFilter(FaultRecord const& fault, void*)
{
    ReportFault(fault, true);
    return FaultDecision::ContinueSearch;
}

// The reporter as the process's top-level handler: a fault outside any
// guarded block, or abort(). Runs on an exception thread, before the handlers
// bg3mse's was put in front of (the game's crash reporter) get the signal.
// It is the reporter's observer from the first Start to the end of the process.
void OnFatalFault(FaultRecord const& fault, void*)
{
    ReportFault(fault, false);
}

// --- Setting up -----------------------------------------------------------------

void DescribeSystem()
{
    TextBuffer text(gSystem);
    char value[512];
    auto fromSysctl = [&](char const* name, char const* key) {
        size_t size = sizeof(value) - 1;
        if (sysctlbyname(name, value, &size, nullptr, 0) != 0) return;
        value[size] = 0;
        text.Appendf("%s=%s\n", key, value);
    };
    fromSysctl("kern.osproductversion", "os_version");
    fromSysctl("kern.osversion", "os_build");
    fromSysctl("hw.model", "model");

    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) text.Appendf("executable=%s\n", path);

    // The bundle the process runs from: in the game, the game's, whose
    // Info.plist carries its version.
    if (CFBundleRef bundle = CFBundleGetMainBundle()) {
        auto fromBundle = [&](CFStringRef infoKey, char const* key) {
            CFTypeRef found = CFBundleGetValueForInfoDictionaryKey(bundle, infoKey);
            if (!found || CFGetTypeID(found) != CFStringGetTypeID()) return;
            if (!CFStringGetCString(static_cast<CFStringRef>(found), value, sizeof(value), kCFStringEncodingUTF8)) return;
            text.Appendf("%s=%s\n", key, value);
        };
        fromBundle(kCFBundleIdentifierKey, "bundle_identifier");
        fromBundle(CFSTR("CFBundleShortVersionString"), "bundle_short_version");
        fromBundle(kCFBundleVersionKey, "bundle_version");
    }

    // The machine, beyond its model.
    fromSysctl("machdep.cpu.brand_string", "cpu");
    auto numberFromSysctl = [&](char const* name, char const* key) {
        uint64_t number = 0;
        size_t size = sizeof(number);
        if (sysctlbyname(name, &number, &size, nullptr, 0) != 0 || (size != 4 && size != 8)) return;
        if (size == 4) number = static_cast<uint32_t>(number);
        text.Appendf("%s=%llu\n", key, (unsigned long long)number);
    };
    numberFromSysctl("hw.ncpu", "cpus");
    numberFromSysctl("hw.perflevel0.physicalcpu", "performance_cpus");
    numberFromSysctl("hw.perflevel1.physicalcpu", "efficiency_cpus");
    numberFromSysctl("hw.memsize", "memory");
    text.Appendf("pid=%d\n", getpid());
    gSystemSize = text.View().size();
}

// Everything a report needs that cannot be made at the time of a crash. The
// caller holds gSetupMutex.
bool PrepareLocked()
{
    if (gPrepared.load(std::memory_order_relaxed)) return true;

    // Room for a file's name after the directory.
    char directory[PATH_MAX - 64];
    size_t length = gHost.ReportDirectory ? gHost.ReportDirectory(directory, sizeof(directory)) : 0;
    if (length == 0 || length >= sizeof(directory)) {
        Log(LogLevel::Error, "crash reporter: there is no directory to write a report to; crashes will not be reported");
        return false;
    }
    directory[length] = '\0';
    struct stat found{};
    if (mkdir(directory, 0700) != 0 && (errno != EEXIST || stat(directory, &found) != 0 || !S_ISDIR(found.st_mode))) {
        Log(LogLevel::Error, "crash reporter: %s cannot be made or is no directory (%s); crashes will not be reported", directory,
            strerror(errno));
        return false;
    }
    snprintf(gPaths.Directory, sizeof(gPaths.Directory), "%s", directory);
    snprintf(gPaths.ReportTemp, sizeof(gPaths.ReportTemp), "%s/%sreport.tmp", directory, kReportPrefix);
    snprintf(gPaths.BacktraceTemp, sizeof(gPaths.BacktraceTemp), "%s/%sbacktrace.tmp", directory, kReportPrefix);
    snprintf(gPaths.StackTemp, sizeof(gPaths.StackTemp), "%s/%sstack.tmp", directory, kReportPrefix);
    // Room for this process's report: it writes one.
    PruneReports(directory, kReportsKept - 1);

    time_t now = ::time(nullptr);
    tm local{};
    if (localtime_r(&now, &local)) gUtcOffset = local.tm_gmtoff;

    // Without this image's function starts nothing says which frames are a
    // registered function's, and the rule cannot be applied.
    auto functions = new FunctionTable();
    std::string problem;
    if (!functions->OpenLoaded(reinterpret_cast<void const*>(ThisImageBase()), problem)) {
        Log(LogLevel::Error, "crash reporter: the functions of this image cannot be listed (%s); crashes will not be reported",
            problem.c_str());
        delete functions;
        return false;
    }
    gFunctions = functions;

    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) == KERN_SUCCESS && timebase.numer) {
        gThreadNamingTicks = kThreadNamingSeconds * 1000000000ull * timebase.denom / timebase.numer;
    }

    DescribeSystem();
    gPrepared.store(true, std::memory_order_release);
    return true;
}

void OnTerminate();

// The reporter's own frames at the top of a thread that reports on itself:
// this function and the terminate handler, which are not the crash's.
__attribute__((noinline)) void ReportThisThread(Cause cause);

uint32_t OwnFrames(FaultRecord const& record)
{
    void const* own[] = { reinterpret_cast<void const*>(&ReportThisThread), reinterpret_cast<void const*>(&OnTerminate) };
    uint32_t count = 0;
    for (; count < record.FrameCount; count++) {
        auto const& frame = record.Frames[count];
        if (!gFunctions || !(frame.Flags & FrameInThisImage)) break;
        uint64_t pc = count == 0 ? frame.PC : frame.PC - 1;
        bool mine = false;
        for (void const* function : own) mine = mine || InFunctionAt(function, pc);
        if (!mine) break;
    }
    return count;
}

// One line of the log for the exception a thread ends with, formatted here
// and written with write(2), as SayFault's.
void SayException(FaultRecord const& fault, Cause cause, Rule rule)
{
    char line[3200];
    snprintf(line, sizeof(line), "%s: %s%s%s%s, thread %llu%s%s%s", cause == Cause::Terminate ? "std::terminate" : "c++ exception left a guarded block",
        gException.Present ? "c++ exception " : "no c++ exception", gException.Present ? gException.Type : "",
        gException.HasWhat ? ": " : "", gException.HasWhat ? gException.What : "", (unsigned long long)fault.ThreadId,
        rule == Rule::Reports ? "" : "; Norbyte's reporter would leave it to the game's (", rule == Rule::Reports ? "" : RuleName(rule),
        rule == Rule::Reports ? "" : ")");
    LogFromFaultHandler(LogLevel::Error, line);
}

// A thread reporting on itself: a C++ exception is about to leave a guarded
// block it is in, or the thread is in std::terminate. Not at the time of a
// fault: this is ordinary code on a thread that is running.
__attribute__((noinline)) void ReportThisThread(Cause cause)
{
    arm_thread_state64_t state;
    bg3mse_crash_capture_state(&state);
    // The exception may come out of Start itself, called inside a block on
    // this thread: then the lock is this thread's, and what is prepared is
    // what there is.
    if (pthread_mutex_trylock(&gSetupMutex) == 0) {
        PrepareLocked();
        pthread_mutex_unlock(&gSetupMutex);
    }
    if (!gPrepared.load(std::memory_order_acquire)) return;
    if (!BeginWriting()) {
        // Another thread's report is being written, and this thread is about
        // to end the process: not before that report is finished.
        WaitForFatalFaultHold();
        return;
    }
    if (!gReported.load(std::memory_order_acquire)) {
        pthread_t self = pthread_self();
        FaultSource source{};
        source.Signal = SIGABRT;            // what std::terminate ends in
        source.State = &state;
        pthread_threadid_np(self, &source.ThreadId);
        source.Thread = pthread_mach_thread_np(self);
        source.StackHigh = reinterpret_cast<uint64_t>(pthread_get_stackaddr_np(self));
        source.StackLow = source.StackHigh - pthread_get_stacksize_np(self);
        source.Origin = cause == Cause::ExceptionLeftGuardedBlock ? FaultOrigin::Guarded : FaultOrigin::Unguarded;
        gCapture.Capture(source, gRecord);
        NoteCurrentException();
        // A guarded block's own function is a frame of the extender (see
        // ExceptionLeavingGuardedBlock); for std::terminate the rule looks
        // at the thread's frames below the reporter's.
        Rule rule = cause == Cause::Terminate ? RuleOn(gRecord, false, OwnFrames(gRecord)) : Rule::Reports;
        SayException(gRecord, cause, rule);
        WriteFiles(gRecord, cause, rule);
        gReported.store(true, std::memory_order_release);
    }
    EndWriting();
}

// HandleGuardedException for STATUS_CPP_EH_EXCEPTION, which the reporter is
// asked about whether or not it is initialised. Returns when the exception
// is not the extender's to report, and the search for its handler goes on.
void ExceptionLeavingGuardedBlock(_Unwind_Exception* exception)
{
    // OnUnhandledException asks ShouldReportCrashes first, as RuleOn does;
    // then IsExtensionRelatedCrash for this case: no stack overflow, and the
    // block's own function is a frame of the extender.
    if (gHost.NativeApiUsed && gHost.NativeApiUsed()) return;
    if (gDisableCrashReportingCount.load(std::memory_order_relaxed) > 0 && !Force()) return;
    // From here the exception goes no further. It is caught here, as the C++
    // runtime catches one that would leave a noexcept function before it
    // calls std::terminate: it becomes the thread's current exception, which
    // the report names, and so does the runtime's terminate handler.
    abi::__cxa_begin_catch(exception);
    ReportThisThread(Cause::ExceptionLeftGuardedBlock);
    // On Windows the reporter exits the process. Here it ends as an uncaught
    // exception ends it, with the throwing frames still on the stack.
    std::terminate();
}

// CrashReporter::OnTerminate: the process's terminate handler from the first
// Start on. The report is written from the thread that ends, with its frames
// (for an exception nothing caught, the thrower's: the C++ runtime calls
// std::terminate before it unwinds anything), and then the handler that was
// there before is called, which is the runtime's own unless the process set
// another; it ends the process. A handler installed after this one that
// calls the one it replaced brings the thread back here: then it aborts.
constinit std::atomic<std::terminate_handler> gPreviousTerminate{ nullptr };
constinit std::atomic<bool> gTerminating{ false };

void OnTerminate()
{
    if (gTerminating.exchange(true, std::memory_order_acq_rel)) abort();
    ReportThisThread(Cause::Terminate);
    if (auto previous = gPreviousTerminate.load(std::memory_order_acquire)) previous();
    abort();
}

void InstallTerminateHandler()
{
    if (std::get_terminate() == &OnTerminate) return;
    gPreviousTerminate.store(std::set_terminate(&OnTerminate), std::memory_order_release);
}

}

bool IsExtensionRelatedCrash(FaultRecord const& fault, bool inGuardedBlock)
{
    return RuleOn(fault, inGuardedBlock) == Rule::Reports;
}

bool Start()
{
    Lock lock(gSetupMutex);
    if (!PrepareLocked()) return false;
    if (!gObserving.load(std::memory_order_relaxed)) {
        if (!AddFatalFaultObserver(OnFatalFault, nullptr)) return false;
        gObserving.store(true, std::memory_order_release);
    } else {
        // Each time: the game may have installed a handler of its own since
        // the last (see the header).
        PutFatalFaultObserversInFront();
    }
    // The same for the terminate handler.
    InstallTerminateHandler();
    gStarted.store(true, std::memory_order_release);
    return true;
}

void Stop()
{
    Lock lock(gSetupMutex);
    gStarted.store(false, std::memory_order_release);
}

bool Started()
{
    return gStarted.load(std::memory_order_acquire);
}

void NoteLogMessage(char const* message)
{
    size_t length = strlen(message);
    size_t size = gErrorLogSize.load(std::memory_order_relaxed);
    size_t taken;
    do {
        // CrashReporter::OnLogMessage: nothing more once the log is past its limit.
        if (size > kErrorLogLimit) return;
        taken = length + 2;
        if (taken > sizeof(gErrorLog) - size) taken = sizeof(gErrorLog) - size;
    } while (!gErrorLogSize.compare_exchange_weak(size, size + taken, std::memory_order_acq_rel, std::memory_order_relaxed));

    // The room is this call's; a report written meanwhile may see it not
    // filled in yet.
    char* out = gErrorLog + size;
    if (taken >= length + 2) {
        memcpy(out, message, length);
        memcpy(out + length, "\r\n", 2);
    } else if (taken > 2) {
        memcpy(out, message, taken - 2);
        memcpy(out + taken - 2, "\r\n", 2);
    } else {
        memcpy(out, "\r\n", taken);
    }
}

}

namespace bg3se::mac {

void RunGuardedBlock(void (*fn)(void*), void* arg)
{
    struct Block
    {
        void (*Fn)(void*);
        void* Arg;
    } block{ fn, arg };
    GuardedInvoke([](void* p) {
        auto b = static_cast<Block*>(p);
        bg3mse_guarded_block_call(b->Fn, b->Arg);
    }, &block, crash::GuardedBlockFilter, nullptr, nullptr);
}

}

_Unwind_Reason_Code bg3mse_guarded_block_personality(int version, _Unwind_Action actions, uint64_t exceptionClass,
    _Unwind_Exception* exception, _Unwind_Context* context)
{
    // The search phase: an exception has found no handler inside the block
    // and nothing is unwound yet. A forced unwind (a thread being cancelled)
    // has no search phase and passes through.
    if (version == 1 && (actions & _UA_SEARCH_PHASE)) bg3se::mac::crash::ExceptionLeavingGuardedBlock(exception);
    return _URC_CONTINUE_UNWIND;
}
