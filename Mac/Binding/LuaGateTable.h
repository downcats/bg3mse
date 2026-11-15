#pragma once

#include <span>
#include <string>

#include <Binding/LuaGates.h>

// The gates of Binding/LuaGates.h, for the start-up summary and the tests.

namespace bg3se::mac::binding {

struct LuaGate
{
    int Role;                   // lua::ModuleRole: 0 client, 1 server, 2 both
    char const* Table;          // the module, as DECLARE_MODULE names it
    char const* Function;
    // Members of StaticSymbols his function reads through without a test:
    // each must be bound.
    char const* Members[8]{};
    // A class whose virtual functions or members his function uses: its
    // slots must be confirmed (VirtualsConfirmed).
    char const* Class{ nullptr };
    // Anything else his function reads without a test; empty when present.
    std::string (*Other)(){ nullptr };
};

std::span<LuaGate const> LuaGates();

// Why the gate is shut now; empty while it is open.
std::string ShutBecause(LuaGate const& gate);

// "Ext.<Table>.<Function>", as a script names it.
std::string LuaName(LuaGate const& gate);

// One block in the log: what the binding keeps back on this game, and the
// Lua functions that are unavailable for it, by library. Logged once, the
// first time a Lua state's libraries are made; later calls log again only
// when `again` is set.
void LogWhatIsUnavailable(bool again = false);

}
