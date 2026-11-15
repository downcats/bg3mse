#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Binding/Libraries.h>
#include <Engine/EngineResolver.h>
#include <Platform/CodeSearch.h>

// The code patches of Norbyte's extender, for LibraryManager::ApplyCodePatch,
// UndoCodePatch and WasPatchApplied (GameHooks/DataLibraries.cpp), outside MSVC.
//
// On Windows a patch is a run of x86 bytes at a place a byte pattern finds
// (the <Patch> elements of GameHooks/BinaryMappings.xml), written after the
// pages are made writable. Here a mapping's patch is one or more code sites
// of the engine table (Engine/Entries/Patches.cpp): one instruction each,
// found from the code of a named function and written through the code
// writer, which swaps the page and never makes code writable. Where the
// Windows patch changes a function that this game has inline in its callers,
// the mapping's sites are all of those callers: together they do what the
// Windows patch does.
//
// A patch goes in whole or not at all. Every site of the mapping must have
// resolved, and must, at the moment of writing, still hold the instruction
// the table's pattern states for it; otherwise nothing is written and the
// reason is logged. Taking a patch out writes back the words that were there,
// where the site still holds what was put in.

namespace bg3se::mac::binding {

// A mapping of BinaryMappings.xml that has a patch, and the table entries
// (code sites) that are its patch here.
struct CodePatchMapping
{
    char const* Mapping;
    std::span<char const* const> Sites;
};

std::span<CodePatchMapping const> CodePatchMappings();

// Empty when every site of the mapping resolved; otherwise why its patch
// would not go in. Works on a resolution of binaries mapped from disk.
std::string CodePatchRefusal(std::string_view mapping, engine::Resolution const& resolution);

// Norbyte's three functions, ApplyCodePatch, UndoCodePatch and
// WasCodePatchApplied, are declared in Binding/Libraries.h, which his file
// includes. They work on the resolution the start-up sequence handed over
// (SetResolutionForLibraries).

// --- the writing, apart from the table ---------------------------------------

struct CodeSiteWrite
{
    void* At;
    MaskedWord Was;         // what the instruction must be before it is replaced
    uint32_t Becomes;
};

// Replaces every site's instruction, or none: empty and `originals` filled
// (the words that were there, in order) when all are written. A site that
// does not hold its instruction refuses the whole before anything is
// written; a write that fails takes out the ones already made.
std::string WriteCodeSites(std::span<CodeSiteWrite const> sites, std::vector<uint32_t>& originals);

// Puts back `originals`. A site that no longer holds what was written is left
// as it is and named in the result; the others are put back.
std::string RestoreCodeSites(std::span<CodeSiteWrite const> sites, std::span<uint32_t const> originals);

}
