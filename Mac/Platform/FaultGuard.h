#pragma once

#include <Platform/FaultRecord.h>

#include <memory>
#include <type_traits>

// Guarded calls: a call that survives a hardware fault taken inside it, the
// way a __try / __except block lets Windows code survive one.
//
//     FaultRecord fault;
//     bool completed = GuardedCall([&] { RunModCode(); }, MyFilter, &state, &fault);
//
// If the calling thread takes a bad access, illegal instruction, arithmetic
// trap or breakpoint trap (FaultRecord.h, FaultKind) inside the callable, the
// filters of the guards the thread is in are asked, innermost first, as SEH
// asks __except filters. The first that answers Recover gets control back:
// its GuardedCall returns false with the fault's record in `fault`, and the
// thread carries on from there. When every filter answers ContinueSearch the
// fault takes the course it would have taken without bg3mse. A null filter
// always recovers.
//
// How. Inside its outermost guard a thread's Mach exception port for those
// four exceptions is bg3mse's (thread_swap_exception_ports on entry, the
// previous ports put back on exit). The kernel consults a thread's ports
// before the task's and before turning the exception into a signal, so a
// fault inside a guard reaches bg3mse first whatever signal handlers or
// task-level ports the game or its libraries install, before or after. The
// faulting thread stays stopped in the kernel while one of bg3mse's two
// exception threads records the fault and runs the filters. To recover, the
// exception thread rewrites the stopped thread's registers so that it resumes
// in the guard with the state it had on entry. To decline, it passes the
// exception to the thread-level port the outermost guard replaced, if there
// was one, and otherwise answers that it was not handled, after which the
// kernel tries the task's port and then raises the signal: the same order and
// the same exception as without bg3mse. A stack overflow inside a guard is
// recovered like any other bad access (the exception thread needs no stack of
// the faulting thread's), and is reported as FaultKind::StackOverflow.
//
// What a recovered fault abandons. Control jumps from the faulting
// instruction straight back into the guard: nothing of the frames in between
// runs, exactly as with SEH. Their destructors do not run, so whatever they
// owned stays as it was at the fault: heap memory is leaked, locks they took
// stay taken, reference counts and "scope" counters they raised stay raised,
// data structures they were changing stay half changed. So a caller must not
// rely on the callable to release anything it acquires, and must assume that
// any lock the callable could have taken may now be held forever.
//
// What a caller must not hold across a guarded call. Filters run on the
// exception thread while the faulting thread is stopped holding everything it
// held at the fault, including what the caller held when it entered the
// guard. A filter that waits for something the faulting thread holds
// deadlocks the exception thread; the faulting thread stays stopped, and
// faults elsewhere then wait for the other exception thread. So a caller must
// not hold, across a guarded call, a lock that the filters of that call (or of
// any guard around it) take.
//
// Filters. A filter runs on a bg3mse exception thread, not on the faulting
// thread: thread-local data and pthread_self() are the exception thread's,
// and FaultRecord::ThreadId / Thread say which thread faulted. It has that
// thread's stack (1 MiB) to itself, so it may do real work (format and write
// a report file, read the faulting thread's memory through the record's
// registers), but the faulting thread may be stopped inside malloc, dyld,
// stdio or the log, holding their locks: anything that may take one of them
// can deadlock as above. A filter may use guarded calls itself (an exception
// thread has what a guarded call needs from the start, so that one allocates
// nothing); their faults are handled by the other exception thread. A fault
// in a filter outside any guard is a fault on that exception thread like any
// other: it takes its normal course, which ends the process. Filters of
// faults on different threads may run at the same time, one on each
// exception thread.
//
// The time limit. The filters of one fault have 30 seconds between them. When
// they have not answered by then (one of them waits for something it will
// never get), a third thread of this layer answers the kernel for them: the
// fault goes on as if every filter had declined it, the log says so, and the
// process ends as a crashed one does instead of standing still. The exception
// thread stays where it is; if its filter returns after all, what it answered
// is dropped. The same limit holds for the handler a declined fault is
// forwarded to.
//
// When nothing serves a fault. A guarded fault in a filter is served by the
// other exception thread; when both are inside filters whose own guarded
// calls fault, nothing is left to serve the second, and those threads wait
// for good. So a filter's guarded calls must not run filters that fault in
// guarded calls themselves.
//
// C++ exceptions are not faults: one thrown out of the callable leaves the
// guard (and its thread port is put back) and propagates to the caller.
// Nothing may longjmp (or siglongjmp) out of the callable past the guard:
// the guard would stay on the thread's list, pointing into dead stack, and
// the thread's next fault would be handed to it.
//
// Breakpoints. While a debugger is attached, breakpoint traps (the
// debugger's own breakpoints and single steps included) are passed on as
// declined without asking filters.
//
// A thread-level exception port that code inside a guarded call installs on
// its own thread is replaced, when the outermost guard leaves, by the one the
// guard replaced. A previous handler registered with a behaviour this layer
// cannot forward (anything but EXCEPTION_DEFAULT, EXCEPTION_STATE and
// EXCEPTION_STATE_IDENTITY) is skipped: the kernel goes on to the task's port.
//
// Cost. Entering and leaving the outermost guard of a thread is two kernel
// calls, a few microseconds together (about 7 µs on the M-series Mac the
// fault tests were last run on, which print the figure); a guard inside
// another costs no kernel call (about 30 ns).
//
// Two copies. libbg3mse.dylib and libbg3mse-camera.dylib each link their own
// copy of this layer. Each copy that uses guards has its own exception
// threads and ports. Guards of the two copies nest like any other: a guard of
// one copy inside a guard of the other replaces and puts back the other's
// thread port, and a fault its filters decline is passed to the other copy's
// port, whose guards are asked next.
//
// fork(). In a child process every guard the forking thread was in is
// forgotten: leaving one there puts no port back, and a fault inside one is
// nobody's. The child's first guarded call starts new exception threads.

namespace bg3se::mac {

enum class FaultDecision
{
    Recover,            // control returns to this guard
    ContinueSearch,     // ask the next guard out; after the outermost, the fault goes on
};

using FaultFilter = FaultDecision (*)(FaultRecord const& fault, void* context);

// Runs fn(arg) guarded. True when fn returned (or threw), false when a fault
// inside it was recovered by this guard; `fault`, when given, then holds the
// record. When fault handling cannot be set up (the exception threads or the
// thread's port cannot be created: logged once), fn runs unguarded.
bool GuardedInvoke(void (*fn)(void*), void* arg, FaultFilter filter, void* context, FaultRecord* fault);

template <class F>
bool GuardedCall(F&& fn, FaultFilter filter = nullptr, void* context = nullptr, FaultRecord* fault = nullptr)
{
    using Fn = std::remove_reference_t<F>;
    return GuardedInvoke([](void* p) { (*static_cast<Fn*>(p))(); },
        const_cast<void*>(static_cast<void const*>(std::addressof(fn))), filter, context, fault);
}

// Starts the exception threads, which otherwise start at the first guarded
// call. False when they cannot be started (logged).
bool StartFaultHandling();

// Whether `thread` (a port name in this task) is one of this copy's exception
// threads or its watchdog: a thread that must keep running while a filter or
// observer works, since it serves the faults of their guarded calls. It asks
// nothing that locks (Platform/FaultObserver.h's rules for observers hold).
bool IsFaultHandlingThread(mach_port_t thread);

}
