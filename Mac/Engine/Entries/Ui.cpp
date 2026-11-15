#include <Engine/Entries/Areas.h>

// The user interface: input, the UI library's reflection data and locks, and
// the UI library's functions Norbyte calls.
//
// The UI library's functions are exported from the game executable under
// their own names. Its reflection and symbol tables have no symbols of their
// own (they lie in blocks the linker merged), so each is found from the code
// of a named function that uses it, at the call that takes it as an argument.

namespace bg3se::mac::engine {

namespace {

constexpr char kRegisterType[] = "__ZN6Noesis10Reflection12RegisterTypeENS_6SymbolEPFPNS_4TypeES1_EPFvS3_E";
constexpr char kAddString[] = "__ZN6Noesis13SymbolManager9AddStringEPKc";
constexpr char kUpdateFromUI[] = "__ZN3gui6GameUI12UpdateFromUIERKN2ls8GameTimeE";
constexpr char kUpdateFromUIName[] = "gui::GameUI::UpdateFromUI";

constexpr CodeShape kRegisterTypeShape{ 144, 0xdf2bb314641db4eeull };     // 4.1.1.7631656
constexpr CodeShape kAddStringShape{ 152, 0x9239fd638c932129ull };        // 4.1.1.7631656

// adrp x0, page ; add x0, x0, #offset ; bl <callee>: the callee's first argument.
constexpr MaskedWord kFirstArgument[] = {
    { 0x90000000, Any::Adrp },
    { 0x91000000, Any::Imm12 },
    { 0x94000000, Any::Branch },
};

// Noesis::Reflection::RegisterType compares the owner of its lock with this thread:
//     bl pthread_mach_thread_np ; adrp x23, page ; ldr w8, [x23, #offset] ; cmp w0, w8
constexpr MaskedWord kOwnerTest[] = {
    { 0x94000000, Any::Branch },
    { 0x90000017, Any::Adrp },
    { 0xb94002e8, Any::Imm12 },
    { 0x6b08001f },
};
constexpr Derivation kReflectionOwner{
    .Host = Exact(kRegisterType),
    .HostShape = kRegisterTypeShape,
    .Anchor = CallsImport("_pthread_mach_thread_np"),
    .Pattern = kOwnerTest,
    .AnchorAt = 0,
    .What = Derivation::Take::PageAddress,
    .At = 1,
};
constexpr Derivation kReflectionLock{
    .Host = Exact(kRegisterType),
    .HostShape = kRegisterTypeShape,
    .Anchor = CallsImport("_OSSpinLockLock"),
    .Pattern = kFirstArgument,
    .AnchorAt = 2,
    .What = Derivation::Take::PageAddress,
    .At = 0,
};
// ... and counts how often it holds it:
//     adrp x22, page ; ldr w8, [x22, #offset] ; add w9, w8, #1 ; str w9, [x22, #offset]
constexpr MaskedWord kDepthIncrement[] = {
    { 0x90000016, Any::Adrp },
    { 0xb94002c8, Any::Imm12 },
    { 0x11000509 },
    { 0xb90002c9, Any::Imm12 },
};
constexpr Derivation kReflectionDepth{
    .Host = Exact(kRegisterType),
    .HostShape = kRegisterTypeShape,
    .Pattern = kDepthIncrement,
    .What = Derivation::Take::PageAddress,
    .At = 0,
};
constexpr Derivation kReflectionMap{
    .Host = Exact(kRegisterType),
    .HostShape = kRegisterTypeShape,
    .Anchor = Calls(Exact("__ZN6Noesis11HashMapImplINS_13HashBucket_KVINS_6SymbolEPNS_4TypeENS_11HashKeyInfoIS2_EEEEE4GrowEj")),
    .Pattern = kFirstArgument,
    .AnchorAt = 2,
    .What = Derivation::Take::PageAddress,
    .At = 0,
};

constexpr Derivation kSymbolLock{
    .Host = Exact(kAddString),
    .HostShape = kAddStringShape,
    .Anchor = CallsImport("_OSSpinLockLock"),
    .Pattern = kFirstArgument,
    .AnchorAt = 2,
    .What = Derivation::Take::PageAddress,
    .At = 0,
};
// Noesis::SymbolManager::AddString looks the string up in its map:
//     adrp x8, page ; add x8, x8, #offset ; ldr w10, [x8, #0xc] ; ldr x9, [x8]
constexpr MaskedWord kSymbolMapLookup[] = {
    { 0x90000008, Any::Adrp },
    { 0x91000108, Any::Imm12 },
    { 0xb9400d0a },
    { 0xf9400109 },
};
constexpr Derivation kSymbolMap{
    .Host = Exact(kAddString),
    .HostShape = kAddStringShape,
    .Pattern = kSymbolMapLookup,
    .What = Derivation::Take::PageAddress,
    .At = 0,
};

// Noesis::SetErrorHandler exchanges the handler: adrp x9, page ; ldr x8, [x9, #offset] ; ...
constexpr MaskedWord kLoadHandler[] = { { 0x90000009, Any::Adrp }, { 0xf9400128, Any::Imm12 } };
constexpr Operand kErrorHandlerOperands[] = {
    {
        .What = "read as a pointer",
        .In = Exact("__ZN6Noesis15SetErrorHandlerEPFvPKcjS1_bE"),
        .Pattern = kLoadHandler,
        .How = Operand::Read::PointerLoad,
        .Target = Exact("__ZN12_GLOBAL__N_113gErrorHandlerE"),
    },
};

// Noesis::VisualTreeHelper::HitTest(Visual*, Point const&) is one jump.
constexpr MaskedWord kAnyJump[] = { { 0x14000000, Any::Branch } };
constexpr Operand kHitTestOperands[] = {
    { .What = "its jump", .Pattern = kAnyJump, .How = Operand::Read::BranchTarget, .Target = Exact("__ZN6Noesis6Visual7HitTestERKNS_5PointE") },
};

constexpr Operand kUpdateFromUIOperands[] = {
    { .What = "refreshes filters", .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact("__ZN9NoesisApp15FilterPredicate7RefreshEv") },
    { .What = "refreshes sorters", .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact("__ZN9NoesisApp12SortComparer7RefreshEv") },
};

constexpr Named kUpdateFromUIHosts[] = { Exact(kUpdateFromUI) };
constexpr Named kGameUIUpdateHosts[] = { Exact("__ZN3gui6GameUI6UpdateERKN2ls7AnyViewERKNS1_8GameTimeE") };
constexpr char const* kPredicatesReplacements[] = { kUpdateFromUIName };

constexpr char kSpinLockCaveat[] =
    "the lock is an OSSpinLock (32 bits), taken with OSSpinLockLock and released with OSSpinLockUnlock. Norbyte types it SRWLOCK "
    "and takes it with the Windows calls: neither applies here";

constexpr Entry kEntries[] = {
    {
        .Name = "ls__gInputManager",
        .What = Kind::GlobalPointer,
        .Mac = "ls::InputManager::m_ptr",
        .Use = "Lua/Libs/ClientInput.inl: the input manager",
        .Where = Exact("__ZN2ls12InputManager5m_ptrE"),
    },
    {
        .Name = "ecl__gCursorControl",
        .What = Kind::GlobalPointer,
        .Mac = "ecl::CursorControl::m_ptr",
        .Use = "Lua/Libs/ClientUI/Module.inl: the cursor",
        .Where = Exact("__ZN3ecl13CursorControl5m_ptrE"),
    },
    {
        .Name = "ls__gDragDropManager",
        .What = Kind::GlobalPointer,
        .Mac = "ls::DragDropManager::m_ptr",
        .Use = "Lua/Libs/ClientUI/Module.inl: drag and drop",
        .Where = Exact("__ZN2ls15DragDropManager5m_ptrE"),
    },

    {
        .Name = "Noesis__Reflection__LockedByThreadId",
        .What = Kind::GlobalObject,
        .Mac = "the owner of the reflection lock, as Noesis::Reflection::RegisterType compares it (its symbol is the C name gThreadID)",
        .Use = "Lua/Libs/ClientUI/Builtins.inl: take the reflection lock as the UI library does",
        .Derived = &kReflectionOwner,
        .Bytes = 4,
        .Agrees = Exact("_gThreadID"),
        .Caveat = "32 bits holding the owner's Mach thread port (pthread_mach_thread_np(pthread_self())), 0xffffffff when the lock "
                  "is free: not a Windows thread id",
    },
    {
        .Name = "Noesis__Reflection__Lock",
        .What = Kind::GlobalObject,
        .Mac = "the reflection lock Noesis::Reflection::RegisterType takes (its symbol is the C name gMutex)",
        .Use = "GameDefinitions/UI.h and Lua/Libs/ClientUI/Builtins.inl: take the reflection lock",
        .Derived = &kReflectionLock,
        .Bytes = 4,
        .Agrees = Exact("_gMutex"),
        .Caveat = kSpinLockCaveat,
    },
    {
        .Name = "Noesis__Reflection__NumLocks",
        .What = Kind::GlobalObject,
        .Mac = "how often the owner holds the reflection lock, 32 bits (its symbol is the C name gRecursiveDepth)",
        .Use = "Lua/Libs/ClientUI/Builtins.inl: take the reflection lock as the UI library does",
        .Derived = &kReflectionDepth,
        .Bytes = 4,
        .Agrees = Exact("_gRecursiveDepth"),
    },
    {
        .Name = "Noesis__Reflection__Data",
        .What = Kind::GlobalObject,
        .Mac = "the reflection's map from a type's symbol to its type, the first argument of the map's Grow in "
               "Noesis::Reflection::RegisterType; the vector of the types' symbols is the 16 bytes before it",
        .Use = "GameDefinitions/UI.h: the types the UI library knows (Norbyte's struct starts at the vector before the map)",
        .Derived = &kReflectionMap,
    },
    {
        .Name = "Noesis__SymbolManager__Lock",
        .What = Kind::GlobalObject,
        .Mac = "the symbol table's lock, the argument of the one OSSpinLockLock in Noesis::SymbolManager::AddString",
        .Use = "GameDefinitions/UI.h and Lua/Libs/ClientUI/Builtins.inl: take the symbol table's lock",
        .Derived = &kSymbolLock,
        .Caveat = kSpinLockCaveat,
    },
    {
        .Name = "Noesis__SymbolManager__Data",
        .What = Kind::GlobalObject,
        .Mac = "the symbol table's map from a string to its symbol, as Noesis::SymbolManager::AddString looks a string up in it; "
               "the vector of the strings is the 16 bytes before it",
        .Use = "GameDefinitions/UI.h: the strings of the UI library's symbols (Norbyte's struct starts at the vector before the map)",
        .Derived = &kSymbolMap,
    },
    {
        .Name = "Noesis__gErrorHandler",
        .What = Kind::GlobalPointer,
        .Mac = "(anonymous namespace)::gErrorHandler: the variable holding the UI library's error handler function",
        .Use = "Lua/Libs/ClientUI/Module.inl: replace the UI library's error handler",
        .Where = Exact("__ZN12_GLOBAL__N_113gErrorHandlerE"),
        .Operands = kErrorHandlerOperands,
    },
    {
        .Name = "Noesis__GUI__LoadXaml",
        .What = Kind::FunctionToCall,
        .Mac = "Noesis::GUI::LoadXaml(char const*)",
        .Use = "Lua/Libs/ClientUI/NsHelpers.inl: load a XAML file",
        .Where = Exact("__ZN6Noesis3GUI8LoadXamlEPKc"),
        .Call = Takes({ Pointer(Place::X0, 1, "char const: the path") }, Returns::IndirectX8, "Noesis::Ptr<Noesis::BaseComponent>",
            "Norbyte declares (Ptr<BaseComponent>& result, char const* path), the Windows form of a result returned in memory; here "
            "the path is the only argument and the result's address goes in x8, as a function declared to return the Ptr by value does it"),
        .Shape = { 42, 0xa8f31665e95b7636ull },     // 4.1.1.7631656
    },
    {
        .Name = "Noesis__Visual__AddVisualChild",
        .What = Kind::FunctionToCall,
        .Mac = "Noesis::Visual::AddVisualChild(Noesis::Visual*)",
        .Use = "Lua/Libs/ClientUI/Builtins.inl: add a child to a visual",
        .Where = Exact("__ZN6Noesis6Visual14AddVisualChildEPS0_"),
        .Call = Takes({ Pointer(Place::X0, 0, "Noesis::Visual"), Pointer(Place::X1, 1, "Noesis::Visual: the child") }),
        .Shape = { 53, 0x88f307a5fdf5aef6ull },     // 4.1.1.7631656
    },
    {
        .Name = "Noesis__Visual__RemoveVisualChild",
        .What = Kind::FunctionToCall,
        .Mac = "Noesis::Visual::RemoveVisualChild(Noesis::Visual*)",
        .Use = "Lua/Libs/ClientUI/Builtins.inl: remove a child from a visual",
        .Where = Exact("__ZN6Noesis6Visual17RemoveVisualChildEPS0_"),
        .Call = Takes({ Pointer(Place::X0, 0, "Noesis::Visual"), Pointer(Place::X1, 1, "Noesis::Visual: the child") }),
        .Shape = { 38, 0xcb6d2e5c45f60bffull },     // 4.1.1.7631656
    },
    {
        .Name = "Noesis__Visual__PointFromScreen",
        .What = Kind::FunctionToCall,
        .Mac = "Noesis::Visual::PointFromScreen(Noesis::Point const&) const",
        .Use = "Lua/Libs/ClientUI/NsHelpers.inl: a screen position in a visual's coordinates",
        .Where = Exact("__ZNK6Noesis6Visual15PointFromScreenERKNS_5PointE"),
        .Call = Takes({ Pointer(Place::X0, 0, "Noesis::Visual const"), Pointer(Place::X1, 1, "Noesis::Point const") },
            Returns::V0V1, "Noesis::Point: two floats"),
        .Shape = { 172, 0x37791579de81478aull },     // 4.1.1.7631656
    },
    {
        .Name = "Noesis__Visual__PointToScreen",
        .What = Kind::FunctionToCall,
        .Mac = "Noesis::Visual::PointToScreen(Noesis::Point const&) const",
        .Use = "Lua/Libs/ClientUI/NsHelpers.inl: a position of a visual on the screen",
        .Where = Exact("__ZNK6Noesis6Visual13PointToScreenERKNS_5PointE"),
        .Call = Takes({ Pointer(Place::X0, 0, "Noesis::Visual const"), Pointer(Place::X1, 1, "Noesis::Point const") },
            Returns::V0V1, "Noesis::Point: two floats"),
        .Shape = { 12, 0x9edb0d737d3d6d31ull },     // 4.1.1.7631656
    },
    {
        .Name = "Noesis__VisualTreeHelper__HitTest",
        .What = Kind::FunctionToCall,
        .Mac = "Noesis::VisualTreeHelper::HitTest(Noesis::Visual*, Noesis::Point const&): a jump to Noesis::Visual::HitTest(Noesis::Point const&)",
        .Use = "Lua/Libs/ClientUI/NsHelpers.inl: the visual at a position",
        .Where = Exact("__ZN6Noesis16VisualTreeHelper7HitTestEPNS_6VisualERKNS_5PointE"),
        .Call = Takes({ Pointer(Place::X0, 0, "Noesis::Visual"), Pointer(Place::X1, 1, "Noesis::Point const") },
            Returns::X0, "Noesis::HitTestResult: one pointer, the visual hit or null"),
        .Shape = { 1, 0x4d25627f9dcdf1f9ull },     // 4.1.1.7631656
        .Operands = kHitTestOperands,
    },

    {
        .Name = "ui__DataContextProvider__ExecuteCommandQueues",
        .What = Kind::NoCallableCode,
        .Use = "GameHooks/EngineHooks.inl: wrapped, and no handler is ever attached to it",
        .Gone = {
            .Fate = "inlined into gui::GameUI::Update(ls::AnyView const&, ls::GameTime const&), which calls the function for one queue, "
                    "ui::CommandProcessor::ExecuteCommandQueue(ls::AnyView const&), twice",
            .NoSymbol = "__ZN2ui19DataContextProvider20ExecuteCommandQueues",
            .Hosts = kGameUIUpdateHosts,
        },
    },
    {
        .Name = "ui__deferred__PredicatesManager__UpdateFromUI",
        .What = Kind::NoCallableCode,
        .Use = "Extender/Client/ScriptExtenderClient.cpp: a pre-hook, once for each UI update on the UI thread, that runs the Lua "
               "callbacks queued for that thread",
        .Gone = {
            .Fate = "inlined into gui::GameUI::UpdateFromUI(ls::GameTime const&): the loop that refreshes each queued filter predicate "
                    "and sort comparer holds that function's one call of NoesisApp::FilterPredicate::Refresh and of "
                    "NoesisApp::SortComparer::Refresh",
            .NoSymbol = "__ZN2ui8deferred17PredicatesManager12UpdateFromUI",
            .Hosts = kUpdateFromUIHosts,
            .Replacements = kPredicatesReplacements,
        },
    },
    {
        .Name = kUpdateFromUIName,
        .What = Kind::FunctionToHook,
        .Mac = "gui::GameUI::UpdateFromUI(ls::GameTime const&)",
        .Use = "takes the place of the predicates manager's pre-hook: its entry is before the refresh, on the same thread",
        .Where = Exact(kUpdateFromUI),
        .Call = Takes({ Pointer(Place::X0, -1, "gui::GameUI"), Pointer(Place::X1, -1, "ls::GameTime const") }, Returns::Nothing, nullptr,
            "it stands in for a function declared void (DeferredPredicatesManager*): here the arguments are the game UI and the "
            "time (Norbyte's handler uses neither)"),
        .Shape = { 8049, 0x991dc2523447937eull },     // 4.1.1.7631656
        .Operands = kUpdateFromUIOperands,
        .Caveat = "the hook runs at the function's entry, earlier than the Windows hook by everything the function does before the "
                  "refresh; and which thread calls the function was not established from the code",
    },
};

}

std::span<Entry const> UiEntries()
{
    return kEntries;
}

}
