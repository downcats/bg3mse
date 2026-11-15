#pragma once

#include <functional>
#include <string>
#include <vector>

// The story's queries called from Lua, a test of the start-up harness
// (Mac/Tests/StartupStoryQueries.cpp).

namespace bg3se::esv::lua { class OsirisBinding; }

namespace bg3se::mac::test {

// Osi.<query>(...) in the server's Lua state for two user queries of a story
// of the test's own: one with the node the game's story compiler gives a
// query that a rule calls, one without, as the compiler leaves a query that
// no rule calls. `binding` is the server's Lua state's story layer; `run`
// runs a chunk in that state and gives its error, empty when it ran. Run on
// the server's thread once the server has its Lua state; the story is the
// extender's only while this runs. Returns everything that did not hold,
// one to a line.
std::vector<std::string> RunStoryQueryTest(esv::lua::OsirisBinding& binding, std::function<std::string(char const* chunk)> const& run);

}
