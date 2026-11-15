#pragma once

#include <Platform/FaultRecord.h>

// Fatal-fault observers: told of a fault no guarded call recovered, on any
// thread, before the process goes down, so that a crash report can be written.
//
//     AddFatalFaultObserver(WriteCrashReport, &reporter);
//
// When. The first observer installs a signal handler for SIGSEGV, SIGBUS,
// SIGILL, SIGFPE, SIGTRAP and SIGABRT (abort(), std::terminate), keeping the
// action each had. Whoever installs a handler after that is in front of
// bg3mse's and decides whether it ever runs: one that recovers the fault, or
// ends the process without passing it on, means the observers are not told.
//
// The game's is such a handler. It installs PLCrashReporter's when it has
// finished launching (-[AppDelegate enableCrashReporting]), and that handler
// passes nothing on. Read in the game's code (4.1.1.7631656): the callback it
// calls first puts the six signals back to their defaults, writes the game's
// report, starts the crash reporter's process and returns "not handled"
// without calling what comes after it in its list, which is where the handler
// installed before would have been called; the signal handler then raises
// the signal again. The system's report of the first crash with bg3mse in the
// game shows the process ended that way.
//
// So bg3mse puts its handler in front again (PutFatalFaultObserversInFront):
// the handler it finds there becomes what the signal is passed on to. That
// is done whenever an observer is added, by whoever knows of a moment after
// which a handler may have appeared (the crash reporter each time Norbyte's
// code starts it, as he starts it again at every change of the game's state;
// the start-up when the game's core library has been initialised, which is
// after the game installed its handler), and it changes nothing where bg3mse's
// handler is still in front. Observers are then told first, and the game's
// crash reporter runs after them with the signal as it came. A handler
// installed later still is in front until the next such moment, so a fault a
// library recovers from on purpose under a handler it installs for the
// purpose (the game's OpenSSL probes CPU features by taking SIGILL) is not
// reported. A fault inside a guarded call reaches the observers only when
// every filter declined it (FaultRecord::Origin is then DeclinedByGuards).
//
// Then. After the observers return, the signal goes on to the action bg3mse's
// handler displaced: what the signal was for before the handler was first
// installed, or the handler it was last put in front of. A handler is
// called, with the signal mask its own action asked for and the signal's
// information and context as they came. Should that handler pass the signal
// on to the one it found when it was installed, which is bg3mse's, the signal
// goes on to the action displaced before it, without the observers being
// told again. When the action is the default one, it is put back and the
// signal is sent to the faulting thread again, which ends the process as soon
// as the handlers have returned: with the signal it would have ended with,
// the registers of the fault, and in the system's crash report the fault's
// exception, codes and address. This holds for a fault and for a signal that
// kill, raise, pthread_kill or abort() sent alike; the handler cannot tell
// them apart and does not try. A signal that was ignored before is left
// ignored: the handler returns, and observers are not told of it.
//
// Where. Observers run on a bg3mse exception thread (Platform/FaultGuard.h),
// on its own 1 MiB stack, while the faulting thread waits in the signal
// handler. The rules for guard filters apply: the faulting thread may hold the
// malloc, dyld, stdio or log lock, so an observer must not wait for anything
// it may hold. What is safe: the record, SymbolizeFrame, reading memory
// through the record's registers carefully, and the file system calls
// themselves (open, write, close, rename) with buffers the observer
// formatted itself without allocating (snprintf into a stack buffer is fine;
// std::string, printf to a FILE, Log() and new are not). Observers are called
// one after another, in the order they were added; faults on two threads at
// once are told one after the other, each on one of the two exception
// threads. A faulting thread waits at most 30 seconds in all, for an
// exception thread to take its fault and for the observers to return, then
// goes on regardless.
//
// What is never told. A fault on a thread whose stack has run out and that
// has no alternate signal stack: the kernel cannot run any signal handler on
// it and ends the process (with SIGSEGV when no handler is installed, SIGILL
// when one is). A fault on a bg3mse exception thread (in a filter or an
// observer) goes on without the observers. A forked child's observers are
// told nothing until the child has entered a guard or added an observer,
// which restarts the exception threads there.
//
// Two copies. Each copy of the platform layer (libbg3mse.dylib,
// libbg3mse-camera.dylib) that has observers installs its own handler; the
// later one runs first and passes the signal on to the earlier, so both
// copies' observers are told, the later copy's first. Only one copy may keep
// putting itself in front: two that both did would displace each other at
// every call, each list of displaced handlers one longer, until a list is
// full (eight entries; logged) and that copy stays behind the other. Today
// libbg3mse.dylib's is the only copy with observers.

// Holds. Work that the end of the process would cut short, which is a crash
// report being written, takes a hold while it lasts. While a hold is taken, a
// thread whose signal bg3mse's handler is about to pass on, after which the
// process ends, waits for the hold to be released: on its own thread, in the
// handler, after the observers have been told, and for at most the 30
// seconds. So a second thread's fault, or the abort() that follows the first,
// does not end the process under a report that is half written. The thread
// that took the hold does not wait for itself, and an exception thread waits
// for nothing. A hold taken by a guard's filter that is then given up on
// (Platform/FaultGuard.h, the time limit) holds nobody any more.

namespace bg3se::mac {

using FatalFaultObserver = void (*)(FaultRecord const& fault, void* context);

// At most four observers per copy of the platform layer. False when there is
// no room, or the handler or exception threads cannot be set up (logged).
bool AddFatalFaultObserver(FatalFaultObserver observer, void* context);

// The signal handler stays installed (a handler installed later may pass
// signals to it); with no observers it only passes them on. A call already in
// progress is not waited for.
void RemoveFatalFaultObserver(FatalFaultObserver observer, void* context);

// Makes bg3mse's handler the first to get each of the six signals again,
// where another handler has been installed since; what it displaces is what
// the signals are passed on to from then on. Nothing happens while there are
// no observers. False when it could not be done for every signal (logged).
bool PutFatalFaultObserversInFront();

// Takes and releases a hold. One holder at a time: a second hold while one is
// taken counts, but the first holder is no longer exempt from waiting.
void HoldFatalFaults();
void ReleaseFatalFaults();
// For a thread that is about to end the process by other means than a signal
// passing through bg3mse's handler (std::terminate): waits as that handler does.
void WaitForFatalFaultHold();

}
