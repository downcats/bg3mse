#pragma once

#include <cstddef>
#include <string_view>

// For Norbyte's LibraryManager (GameHooks/DataLibraries.cpp), outside MSVC:
// what its functions hand over to. This header is the one his file includes,
// and includes nothing of bg3mse's.
//
// FindLibraries: the macOS game is not searched for byte patterns.
// StaticSymbols is filled from the engine table as it resolved on the running
// game (Mac/Binding/StaticSymbolBinding.h), which the start-up sequence
// (Mac/Binding/Startup.h) resolved before any hook went on.
//
// ApplyCodePatch, UndoCodePatch and WasPatchApplied: a patch is instructions
// of the macOS game, found from the same resolution (Mac/Binding/CodePatches.h).

namespace bg3se::mac::binding {

struct LibrariesBound
{
    // What LibraryManager keeps as CriticalInitFailed and InitFailed.
    bool CriticalFailed{ true };
    bool Failed{ true };
};

// Fills the StaticSymbols at `staticSymbols` and logs every member left null
// with the reason. Without a resolution (the start-up sequence did not run, or
// refused) nothing is bound and both failures are reported.
LibrariesBound BindLibraries(void* staticSymbols);

// For LibraryManager::PostStartupFindLibraries, which runs once the game's
// core library is initialised: fills the type index maps of the StaticSymbols
// at `staticSymbols` (Mac/Binding/TypeIndexBinding.h) and logs every type
// left unknown with the reason. It is not part of BindLibraries because the
// game gives its types their indices in CoreLibSDM::Init, after that runs.
struct TypeIndicesBound
{
    size_t Contexts{ 0 };       // of the five, those the game has numbered
    size_t Types{ 0 };          // index variables now in the maps
    size_t Unknown{ 0 };        // types left out: names of Norbyte's without a variable, and variables that failed their check
};
TypeIndicesBound BindTypeIndices(void* staticSymbols);

// Norbyte's three functions, by the mapping's name in BinaryMappings.xml.
// ApplyCodePatch is true when the patch is in, whether it went in now or
// before, as his is; nothing is written unless the whole patch can be.
bool ApplyCodePatch(std::string_view mapping);
bool UndoCodePatch(std::string_view mapping);
bool WasCodePatchApplied(std::string_view mapping);

}
