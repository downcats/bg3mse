#pragma once

// Where the messages of CoreLib's logging go in this build.
//
// CoreLib's entry points are Debug and DebugLocal (behind the DEBUG / INFO /
// WARN / ERR macros). Norbyte's definitions of them (CoreLib/Utils.cpp,
// compiled for MSVC only) format a message and hand it to the extender's
// console object, which prints it when a console was created, keeps warnings
// and errors for the crash report, and passes it to the Lua debugger; other
// parts of the extender hand the console object their messages directly (Lua
// and Osiris errors, a failed assertion). Without that object a message goes
// nowhere.
//
// Here every message is a line of bg3mse's log, exactly once:
//
//   - Debug and DebugLocal are defined by Platform/DebugLog.cpp. While no
//     console object takes messages (the extender is not started, or this is
//     a tool or a test), they write the line to the log themselves.
//   - Once the extender is started, they hand the message to its console
//     object as Norbyte's do (the console sink, set by Mac/Binding/Startup.cpp),
//     and the console object writes every message it is given to the log
//     (LogConsoleMessage, called from Console::LocalPrint outside MSVC)
//     before it does anything else with it.

namespace bg3se::mac {

// Hands a formatted message to the extender's console object. `type` is the
// DebugMessageType; `local` tells DebugLocal (the console's LocalPrint) from
// Debug (its Print). False when there is no console object to take it.
using DebugConsoleSink = bool(int type, char const* text, bool local);

// nullptr takes the sink away. May be called from any thread.
void SetDebugConsoleSink(DebugConsoleSink* sink);

// A message the console object was given, as a line of the log at the level
// of its DebugMessageType.
void LogConsoleMessage(int type, char const* text);

}
