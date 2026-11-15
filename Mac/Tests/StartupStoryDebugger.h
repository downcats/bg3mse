#pragma once

#include <string>
#include <vector>

// The story debugger test of the start-up harness
// (Mac/Tests/StartupStoryDebugger.cpp, bg3mse_startup_tests --story-debugger).

namespace bg3se::mac::test {

// Norbyte's story debugger (Osiris/Debugger) from its server's start to its
// end, with a story of the test's own in place of the game's and a frontend
// on another thread that speaks osidebug.proto over loopback. Run where the
// game loads a story: on the server's thread, once the extender has its
// server. Returns everything that did not hold, one to a line, and says what
// was seen in `summary`.
std::vector<std::string> RunStoryDebuggerTest(std::string& summary);

}
