#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Binding/OsirisLibrary.h>
#include <Binding/StaticSymbolBinding.h>
#include <Engine/EngineResolver.h>
#include <Layout/OsirisShapes.h>

// Binding Norbyte's story layer (BG3Extender/GameHooks/OsirisWrappers.cpp,
// BG3Extender/Osiris) to the Mac game's libOsiris.dylib.
//
// On Windows OsirisWrappers::Initialize loads osiris.dll, finds its
// functions by their MSVC names, its globals and RuleAction::Call by x86
// scans, and hooks CreateFileW and CloseHandle to see the story files being
// written. Here everything comes from the engine table's story entries
// (Engine/Entries/Osiris.cpp), through rows as for StaticSymbols:
//
//   globals   the fifteen members of OsirisStaticGlobals: the ten objects,
//             the logging flags, and the four string table functions
//   hooks     what Initialize gives its Wrap calls: the library function
//             itself where a detour of Norbyte's prototype passes what the
//             function takes, a bridge (Binding/HookBridges.h) where it does
//             not, and the two bridges that stand for the Windows API
//   vtables   the nine node classes, each the address a node's vtable
//             pointer holds, for OsirisWrappers::VMTs
//
// The gate. The story layer walks the library's structures on every event
// and reads its globals without a test, and Norbyte's Initialize ends the
// process when anything is missing. Here nothing is handed over unless all
// of it is: every row marked critical bound, every hook of this layer that
// such a row needs attached, and every shape of Layout/OsirisShapes.h
// confirmed on the library's code. Otherwise the story layer stays off: no
// hook of Norbyte's goes on the library, his globals point at nothing, and
// the reason is in the report. The two rows for the story files are not
// critical: without them the story header is not extended, and nothing else
// changes.
//
// The node vtables are decided on their own, each class by its entry: the
// slots Norbyte's code calls or replaces must be where NodeVMT has them. His
// wrappers need all nine classes (a node of a class they do not know ends the
// process), so when one is refused none is handed over, and ResolveNodeVMTs
// answers false: the node hooks stay off and the rest of the layer works.

namespace bg3se::mac::binding {

// The rows. `Offset` is in OsirisStaticGlobals for a global row and in
// OsirisHookTargets for a hook row.
std::span<MemberRow const> OsirisGlobalRows();
std::span<MemberRow const> OsirisHookRows();
size_t OsirisGlobalsSize();

// A slot of a node's vtable that Norbyte's code calls or replaces.
struct NodeSlot
{
    char const* Name;       // the table's name for the method, which is NodeVMT's
    uint32_t InNodeVMT;     // offsetof(NodeVMT, Name)
    // Where a call through Norbyte's class Node finds the function: the
    // offset his virtual function has in the vtable this build gives the
    // class.
    int32_t InNode;
    uint32_t Expected;      // Layout/OsirisLayout.h
    bool QueryOnly;         // only the three query classes have it
};
std::span<NodeSlot const> OsirisNodeSlots();

struct NodeClassRow
{
    uint8_t Type;           // NodeType
    char const* Entry;      // the vtable's entry
    bool Query;
};
std::span<NodeClassRow const> OsirisNodeClasses();

// Empty when the rows hold together with the table: every byte of
// OsirisStaticGlobals and of OsirisHookTargets belongs to one row, every row
// can take its entry, every node slot is where the table has it for every
// class, and the offsets the story file hooks read are the ones the table
// checks in the game's code. Otherwise every finding, one to a line.
std::string CheckOsirisRows(std::span<engine::Area const> table);

// For CheckRows (Binding/StaticSymbolBinding.h): whether the story binding
// uses the entry, and whether it has a row of that kind for the member.
bool OsirisUsesEntry(std::string_view entry);
bool OsirisHasRow(std::string_view member, Bound how);

struct NodeClassResult
{
    NodeClassRow const* Row{ nullptr };
    void* Vtable{ nullptr };
    std::string Reason;     // why not, when null
};

struct OsirisPlan
{
    std::vector<MemberResult> Globals;      // in the rows' order
    std::vector<MemberResult> Hooks;
    std::vector<NodeClassResult> Classes;
    osiris::Shapes Shapes;

    // The gate is open: everything critical is bound and the shapes are
    // confirmed. Otherwise `Reason` says what is missing, one to a line.
    bool Available{ false };
    std::string Reason;
    // Every node class is proven: OsirisWrappers::VMTs can be filled.
    bool NodeHooks{ false };
    // Where the generated story's path is from its writer; 0 when not known.
    ptrdiff_t StoryPathFromWriter{ 0 };
};

// What BindOsiris (Binding/OsirisLibrary.h) would decide, without storing
// anything. On a resolution of binaries mapped from disk the values are
// link-time addresses. BindOsiris decides the same from the resolution of the
// running game, then fills Norbyte's globals, gives the adapter its target
// and the story file hooks their offset.
OsirisPlan PlanOsiris(engine::Resolution const& resolution, bool hooksAttached);

}
