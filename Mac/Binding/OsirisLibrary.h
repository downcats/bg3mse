#pragma once

#include <cstddef>
#include <span>
#include <string>

// What Norbyte's story layer receives on macOS, where on Windows
// OsirisWrappers::Initialize finds everything in osiris.dll itself
// (BG3Extender/GameHooks/OsirisWrappers.cpp includes this file; how the
// answers are made is in Binding/OsirisBinding.h).

namespace bg3se::mac::binding {

// What OsirisWrappers::Initialize wraps, in the order of its Wrap calls.
// null: not to be wrapped.
struct OsirisHookTargets
{
    void* RegisterDivFunctions{ nullptr };
    void* InitGame{ nullptr };
    void* DeleteAllData{ nullptr };
    void* GetFunctionMappings{ nullptr };
    void* OpenLogFile{ nullptr };
    void* CloseLogFile{ nullptr };
    void* Load{ nullptr };
    void* Compile{ nullptr };
    void* Merge{ nullptr };
    void* Event{ nullptr };
    void* RuleActionCall{ nullptr };
    void* CreateFileW{ nullptr };
    void* CloseHandle{ nullptr };
};

// Norbyte's NodeType: 1 to 9; 0 is no type.
inline constexpr size_t kNodeTypes = 10;

struct OsirisBound
{
    // Everything the story layer needs is found and proven: the globals are
    // filled and the functions below are the ones to wrap. Otherwise nothing
    // was filled and nothing may be wrapped.
    bool Available{ false };
    OsirisHookTargets Hooks{};
    // The node classes' vtables by NodeType, each the address a node's
    // vtable pointer holds; all null unless every class is proven.
    void* Vtables[kNodeTypes]{};
};

// Decides from the resolution the start-up sequence made; when everything is
// there, fills the OsirisStaticGlobals at `globals` and returns what to wrap.
// The outcome is logged, every refusal with its reason.
OsirisBound BindOsiris(void* globals);

// One slot of a node's vtable to be replaced: the slot, the function to put
// there, and where the function it held is kept.
struct VtableSlotWrap
{
    void** Slot;
    void* Replacement;
    void** Original;
};

// Puts each replacement in its slot, after keeping what the slot held. The
// vtables are read-only data: each slot is one pointer stored on its own
// (Platform/CodeWriter.h, StorePointer), which other threads may read at any
// time. All or none: when a store is refused, the slots already replaced get
// their functions back and the reason is returned.
std::string WrapVtableSlots(std::span<VtableSlotWrap const> slots);
// Gives every slot that still holds its replacement the function it held
// before. Returns the first refusal; the other slots are restored all the same.
std::string UnwrapVtableSlots(std::span<VtableSlotWrap const> slots);

}
