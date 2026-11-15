#pragma once

#include <Crash/CrashReport.h>
#include <Platform/FaultRecord.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

// The Mac side of Norbyte's guarded blocks and crash reporter
// (GameDefinitions/Base/Base.h, Extender/Shared/CrashReporter.cpp), on the
// platform layer's guarded calls and fatal-fault observers.
//
// What Norbyte's code does on Windows. BEGIN_GUARDED() ... END_GUARDED() is a
// __try block whose filter is HandleGuardedException and whose handler is
// empty. The filter hands the exception to the crash reporter when the
// reporter is initialised, and a C++ exception in any case; anything else
// goes on to the next handler. The reporter (also the process's top-level
// exception filter and terminate handler while it is initialised) decides
// whether the crash involves the extender:
//
//   - a stack overflow does not, and neither does anything while
//     gDisableCrashReportingCount is raised (it is, around every call into
//     the game's own function from a hook), unless the ForceCrashReporting
//     setting is on, which makes every crash count;
//   - otherwise it does when a frame of the backtrace is in the extender's
//     module, not counting return addresses in the functions the extender
//     registered as hook entry points (gRegisteredTrampolines) or in a fixed
//     list of Osiris wrappers: game code runs below those.
//
// A crash that involves the extender is written out (a minidump, a report
// file and a backtrace text in the temporary directory), CrashReporter.exe is
// started on it, and the process exits; nothing is logged and nothing
// continues. One that does not is given to the top-level filter the game had
// installed. So a guarded block recovers from nothing: it is there so that
// the reporter sees an exception before a handler of the game's can swallow it.
//
// Here. RunGuardedBlock runs the block in a guarded call whose filter hands
// the fault to the reporter, on bg3mse's exception thread while the faulting
// thread is stopped, and in every case answers ContinueSearch, so the fault
// goes on as it would have without bg3mse: to the game's own crash reporter
// and then to the system's. Nothing is recovered and no frame is abandoned. A
// fault outside any guarded block reaches the reporter through a fatal-fault
// observer, before the game's crash reporter gets the signal; a fault already
// reported from a block is not reported twice, the process writing one report
// (see below).
//
// Where this reporter goes beyond Norbyte's. His rule decides whether a crash
// is reported at all, because his reports are sent to him and a crash of the
// game's own is not his to collect. A report here goes nowhere (see
// LaunchCrashReporter), and the rule left the first crash of a play session
// without a word: a handler of the extender's faulted while the game's update
// ran inside ScriptExtender::OnECSUpdate, which disables crash reporting
// around the game's function as every hook does, so the rule did not count
// it. So here the rule decides what the user is told, as on Windows, and
// what the report says of itself, but not whether it is written:
//
//   - every fault that reaches the reporter is written to the log, one line
//     with its kind, address, the function it was in and its thread, from a
//     guarded block whether or not the reporter was ever started;
//   - the process's first such fault is written out as a report, once the
//     reporter has been started;
//   - the report's fault section says what the rule makes of the crash
//     (extender_related, norbytes_rule) and what crash reporting's count was
//     (crash_reporting_disabled), and the log's line says so when the rule
//     would have left the crash to the game;
//   - LaunchCrashReporter, and with it the notice at the next start, is only
//     for a crash the rule reports, as CrashReporter.exe is only started for
//     one; the log says where the report of another crash is.
//
// What still leaves nothing: a fault outside any guarded block while a
// handler installed after bg3mse's is in front of it (the game's, until the
// reporter is next started; Platform/FaultObserver.h), a fault on a thread
// whose stack has run out outside any guarded block, and a fault on one of
// bg3mse's exception threads. A fault that something recovers after the
// reporter has seen it (a guarded call around the block, a handler bg3mse's
// passes the signal to) is written down as well, and takes the process's one
// report.
//
// An exception that nothing catches, outside any guarded block, ends in
// std::terminate, as on Windows, where the reporter is the terminate handler
// (CrashReporter::OnTerminate). Here too, from the first Start on: the report
// is written from the thread that ends, with the thrower's frames (nothing
// is unwound before std::terminate), and names the exception's type and
// what(); then the handler that was there before runs, which ends the
// process. Like the fault observer, the handler is put in front again at
// every Start and stays after Stop.
//
// A C++ exception about to leave a guarded block is what HandleGuardedException
// gets as STATUS_CPP_EH_EXCEPTION. The block is called through a frame whose
// personality routine is asked while the unwinder still searches for a
// handler, before any frame has been unwound: when no native module has
// used the extender's API (ShouldReportCrashes) and the count is not raised
// (or reporting is forced), the exception is caught there, as the C++ runtime
// catches one leaving a noexcept function, the report is written with the
// throwing frames still on the stack and naming the exception, and
// std::terminate ends the process, whether or not the
// reporter is started, as on Windows. Otherwise the search goes on and the
// exception propagates as if the block were not there: this part of the rule
// stays a decision, since such an exception is not a crash until nobody
// catches it. An exception caught
// inside the block never gets that far.
//
// Which frames are a registered function's. Norbyte's rule takes the 0x120
// bytes after a registered address (0x180 for some of the fixed list): sizes
// of x64 code. Here a return address is in a registered function when the
// function it is in starts at the registered address, by the image's table of
// function starts (Platform/FunctionExtents.h). The faulting pc itself is
// never excluded.
//
// Where the report is written. Norbyte's reporter writes into the temporary
// directory, one minidump with a report and a backtrace text beside it, each
// replacing the one of the crash before, for the program it starts to pick
// up. Here nothing picks a report up (see LaunchCrashReporter), so reports
// are kept: Host::ReportDirectory gives a directory, and a report is its
// files there, named for the time of the crash (Crash/CrashReport.h). In
// place of the minidump, the owner's decision: the faulting thread's stack
// memory, from its stack pointer up, in a file of its own beside the report,
// which names it; no core dump of the process. Each file is written under a
// temporary name and renamed. The directory keeps the newest ten reports: when the reporter is
// prepared it removes all but the newest nine, and a process writes one
// report, for the first of its faults. On Windows that is all there can be,
// the reporter ending the process itself. Here the
// fault goes on after the report is written, and something may yet recover
// it (a guard around the block, a handler of the game's): what the process
// runs into after that is not reported. Then LaunchCrashReporter is called.

namespace bg3se {

// Norbyte's two variables (Extender/Shared/Utils.h, CoreLib/Wrappers.h),
// defined here on Mac as his crash reporter defines them on Windows.
extern std::atomic<uint32_t> gDisableCrashReportingCount;
extern std::unordered_set<void const*> gRegisteredTrampolines;

}

namespace bg3se::mac {

// Runs fn(arg) as a guarded block (Mac/Shim/windows.h, GuardedBlock).
void RunGuardedBlock(void (*fn)(void*), void* arg);

}

namespace bg3se::mac::crash {

// What the reporter asks of the extender it reports for. Every function but
// ReportDirectory is called at the time of a crash, on a thread that must not
// allocate or take a lock while the faulting thread is stopped holding
// whatever it held; the ones that write are run inside a guarded call, and a
// fault in one loses only what it had not written yet. A null entry is
// skipped.
struct Host
{
    // The ForceCrashReporting setting.
    bool (*ForceCrashReporting)();
    // Whether a native module has used the extender's API
    // (APIHelpers::WasActivated): Norbyte's reporter then reports no crash,
    // whatever the setting says (CrashReporter::ShouldReportCrashes).
    bool (*NativeApiUsed)();
    // The directory reports are written to, as UTF-8 and without a separator
    // at its end: the path's length, or 0 when there is none or it does not
    // fit. Called once, when the reporter is prepared; the reporter makes the
    // directory when it is not there (its parent must be).
    size_t (*ReportDirectory)(char* out, size_t size);
    // The fixed list of functions whose frames do not count: their start
    // addresses, at most `max`; the number written.
    size_t (*ExcludedFunctions)(void const** out, size_t max);
    // SVER, GVER, SDAT, CHAN, BLDT and CERT.
    void (*BuildSections)(ReportWriter& report);
    // The Lua backtrace of the state the faulting thread owns, if it owns one.
    void (*LuaBacktrace)(FaultRecord const& fault, TextBuffer& text);
    // The loaded mods.
    void (*ModuleList)(TextBuffer& text);
    // CSTA, SSTA and SLEN.
    void (*StateSections)(ReportWriter& report);
};

// Defined by what links the reporter: Crash/ExtenderCrashReporting.cpp for
// Norbyte's extender, a test for itself. Constant-initialised, so that it is
// there before any code of the image runs.
extern Host const gHost;

// What InitCrashReporting and ShutdownCrashReporting do. Start prepares the
// reporter (the reports' directory, this image's function starts, the
// system's description) and adds its fatal-fault observer; false when any of that
// cannot be done (logged), and nothing is reported then. Both may be called
// again and again.
//
// Start may be called again and again, and Norbyte's code does: at the
// beginning of its start-up and at every change of the game's state, "to
// initialize the crash reporter after the game engine has started, otherwise
// the game will overwrite the top level exception filter". Here the game's
// handler does not replace bg3mse's; it is installed in front of it, when the
// game has finished launching, and passes nothing on
// (Platform/FaultObserver.h). So every Start puts bg3mse's handler in front
// again, and the start-up does it once more when the game's core library has
// been initialised (Mac/Binding/HookBridges.cpp), which is where Norbyte's
// start-up goes on (ScriptExtender::PostStartup) before any state has changed.
//
// Stop ends what Started() says, which the extender's capture of its log goes
// by, and nothing else: a reporter that was started reports until the process
// ends. On Windows ShutdownCrashReporting takes the reporter out as the
// process's top-level exception filter, and Norbyte's start-up calls it at its
// end to make room for the filter the game sets; a crash of the extender
// between that and the first change of state goes unreported there. Here
// there is nothing to make room for: the observer stays, and so does the
// reporting from guarded blocks.
bool Start();
void Stop();
bool Started();

// A warning or error the extender logged, for the ERRL section: kept, each
// followed by "\r\n", until there are 64 KiB of them.
void NoteLogMessage(char const* message);

// CrashReporter::LaunchCrashReporter. On Windows it starts CrashReporter.exe,
// beside the DLL, which shows the user the backtrace, asks whether to send
// the report, and uploads it to Norbyte's server.
//
// The owner's decision for macOS: a report stays on disk, and its path is
// written to the log. Nothing is started and nothing is sent anywhere. What
// the user is shown instead of CrashReporter.exe's dialog is a notice the
// next time the game starts (TakeCrashNotice): the report's path is noted for
// it in the reports' directory. The definition for Norbyte's extender is
// Crash/CrashDelivery.cpp; the reporter's tests define their own, to see the
// call.
//
// Called once the report of a crash Norbyte's rule reports is written (not
// for another crash's), with the report's path, on the thread
// that wrote it and under the rules for Host's functions: for a fault that is
// an exception thread, with the faulting thread still stopped. When it
// returns the fault goes on as it would have without bg3mse, to the game's
// own crash reporter.
void LaunchCrashReporter(char const* reportPath);

// The file in the reports' directory that names the last report written, until
// the next start of the game has told the user of it.
inline constexpr char kCrashNoticeFile[] = "last-crash";

// What CrashReporter.exe tells a Windows user after a crash of the extender
// ("The game has unexpectedly crashed.", then the backtrace), told at the
// next start of the game: that it crashed, where the report is, and the
// report's backtrace. Taken from the note LaunchCrashReporter left in
// `directory` (the reports' directory), which goes with it: told once. Nothing
// when there is no note, or when the report it names is gone.
std::optional<std::string> TakeCrashNotice(std::string const& directory);

// Norbyte's rule, as described above: whether his reporter would report the
// crash. `inGuardedBlock`: the fault was given to a guarded block's filter,
// whose function is a frame of the extender wherever the walk of the stack
// ended.
bool IsExtensionRelatedCrash(FaultRecord const& fault, bool inGuardedBlock);

}
