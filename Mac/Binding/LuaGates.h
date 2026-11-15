#pragma once

// For Norbyte's ModuleRegistry (Lua/Shared/LuaModule.inl), outside MSVC:
// the Lua functions of his libraries that read through something the binding
// may leave null without a test of their own.
//
// After a game update a proof of a layout or of a virtual slot can fail, or
// an entry of the engine table that is not critical can stop resolving. The
// binding then leaves the member of StaticSymbols that leads there null, or
// answers VirtualsConfirmed with false (Binding/Layouts.h), and the extender
// starts without it. Most of his code tests for null and gives nil; the
// functions listed in Mac/Binding/LuaGates.cpp do not, and would fault. Each
// of them is put in the module's table behind a gate that, while what the
// function reads is missing, raises a Lua error naming the function and why,
// and otherwise calls his function. The first time a gate is shut its reason
// is logged once.
//
// This header is the one his file includes, and includes nothing of bg3mse's.

struct lua_State;

namespace bg3se::mac::binding {

using LuaFunction = int (*)(lua_State*);

// What his MakeLuaFunctionTable puts in the table for `function` of the
// module `table` (and `subTable`, or nullptr) of `role` (lua::ModuleRole's
// value): `original` itself, or a gate in front of it.
LuaFunction GateLuaFunction(char const* table, char const* subTable, char const* function, int role, LuaFunction original);

}
