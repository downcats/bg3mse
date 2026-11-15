#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

// Metal functions of our own, found by the game under their names.
//
// The game makes a shader from a stub by asking its two Metal libraries for
// the functions the stub names (rf::GPUDevice::CreateShader: the library of
// the engine first, then the materials', -newFunctionWithName: on each). A
// translated shader's functions are in neither. Once installed, a library
// that does not have a function whose name starts with bg3mse_ answers with
// the function of that name compiled here, if there is one; every other
// request is answered as before.
//
// A function is compiled from its source when it is added, or, for a source
// that is known to compile (KeepTranslatedSources), when the game first asks
// for it or a thread in the background reaches it, whichever is first.

namespace bg3se::mac {

// Puts the answer in place. False, with `why`, when it cannot be.
bool InstallTranslatedFunctions(std::string& why);

// Compiles `sources` (one library each) and keeps every function of theirs
// whose name starts with bg3mse_. False, with `why`, when one does not compile.
// `material`, when given, is whose they are: the first time the game takes
// one of them the log names it.
bool AddTranslatedFunctions(std::span<std::string const> sources, std::string& why, std::string_view material = {});

// One function of ours and the source of the library it is in.
struct TranslatedSource
{
    std::string Name;
    std::string Source;
};

// The same for functions whose names are known beforehand: false as well when
// a source compiles without the function it is named for.
bool AddTranslatedFunctions(std::span<TranslatedSource const> sources, std::string& why, std::string_view material = {});

// Keeps the sources of functions that compiled on this system before, without
// compiling them: nothing waits for the compiler. Each is compiled when the
// game asks for its function, or before that in the background. Should one
// not compile after all, `failed` is called, once; it is tried again when the
// game asks for it, twice then, and when it still does not compile what the
// game draws with that function is missing, and the log says so. A source
// whose name is not text is left out.
void KeepTranslatedSources(std::span<TranslatedSource const> sources, std::string_view material = {}, std::function<void()> failed = {});

// Returns when the background has gone through every source kept so far.
void WaitForTranslatedSources();

struct TranslatedFunctionCounts
{
    uint32_t Kept{ 0 };         // functions compiled here
    uint32_t Waiting{ 0 };      // sources kept and not compiled yet
    uint64_t Asked{ 0 };        // requests for a bg3mse_ name the libraries did not have
    uint64_t Answered{ 0 };     // of those, answered with a function
};
TranslatedFunctionCounts CountTranslatedFunctions();

}
