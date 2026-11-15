#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The debug adapter test of the start-up harness
// (Mac/Tests/StartupLuaDebugAdapter.cpp, bg3mse_startup_tests --lua-debug-adapter PROGRAM).

namespace bg3se::mac::test {

// Runs Norbyte's Lua debug adapter (the program `adapter`) as an editor does
// and takes it through a debugging session with the Lua debugger's server of
// this process, which listens on `port`. Run on the server's Lua thread.
// Returns everything that did not hold, one to a line, and says what was
// seen in `summary`.
std::vector<std::string> RunLuaDebugAdapterTest(char const* adapter, uint16_t port, std::string& summary);

}
