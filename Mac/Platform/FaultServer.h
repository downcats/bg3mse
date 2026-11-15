#pragma once

#include <Platform/FaultCapture.h>

#include <cstddef>
#include <cstdint>
#include <mach/message.h>
#include <pthread.h>

// Between the exception threads (Platform/FaultGuard.cpp) and the
// fatal-fault observers (Platform/FaultObserver.cpp). The exception threads
// receive from one port set: the guarded threads' ports and the observer
// port, to which the observers' signal handler sends a faulting thread's
// state.

namespace bg3se::mac {

// Starts the exception threads if they are not running (as a guarded call does).
bool EnsureFaultServer();

// Called by FaultGuard.cpp while it starts the exception threads, holding its
// start lock: adds the observer port to the set.
bool StartObserverPort(mach_port_t portSet);
// In a forked child, where the parent's ports and threads are gone.
void ResetObserverAfterFork();

// Room for the largest observer message, for the exception threads' receive buffer.
constexpr size_t kObserverMessageMax = 1024;
// When `message` came in on the observer port: tells the observers and
// replies, then returns true. Runs on an exception thread.
bool HandleObserverMessage(mach_msg_header_t& message, FaultCapture& capture, FaultRecord& record);

// The exception threads, so that the signal handler does not send a fault of
// one of them to itself.
void NoteServerThread();
bool IsServerThread();

// For the signal handler, on the faulting thread: whether this thread is in
// a guard whose filters declined a fault at `pc`.
bool GuardsDeclinedFaultAt(uint64_t pc);

// The watchdog has given up on the filter this exception thread is running:
// a hold that thread took (Platform/FaultObserver.h) holds nobody any more.
void VoidFatalFaultHoldOf(pthread_t thread);

// The time limit: how long a fault is kept from going on by what bg3mse runs
// for it, a guard's filters or the observers. 30 seconds; the tests set less.
uint32_t FaultTimeLimitMs();
void SetFaultTimeLimitMs(uint32_t milliseconds);
// The moment (mach_absolute_time) the limit runs out for something that
// starts now, and the milliseconds left until such a moment, rounded up; 0
// when it is past. System calls only.
uint64_t FaultDeadlineFromNow();
uint32_t MillisecondsUntil(uint64_t deadline);

}
