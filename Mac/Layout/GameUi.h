#pragma once

#include <cstddef>

// Where the Mac game keeps the members of gui::GameUI that its own code
// shows (Layout/GameShapes.cpp says which code). Norbyte's declaration
// (ui::GameUI, GameDefinitions/UI.h) is asserted to give the same offsets
// (Layout/DefinitionChecks.cpp).
//
// The object differs from the Windows one in three places. It begins with
// the vtable pointers of its five bases one after another, where Windows has
// a word of data after two of them. Its command processor (two flag bytes
// and a lock-free queue) is aligned to the queue's 128-byte line. And
// gui::TwitchOverlayManager holds its request manager as a pointer at the
// 8-byte boundary after its critical section.

namespace bg3se::mac {

constexpr size_t kGameUiVtables = 5;                    // +0 .. +0x28
constexpr size_t kGameUiSignal = 0x28;
constexpr size_t kGameUiNoesisManager = 0x80;
constexpr size_t kGameUiBrushManager = 0x240;
constexpr size_t kGameUiBrushManagerLock = 0x18;        // in the brush manager
constexpr size_t kGameUiCommandProcessor = 0x300;        // two flag bytes, then the queue on the next 128-byte line
constexpr size_t kGameUiCommandQueue = 0x380;
constexpr size_t kGameUiWindowManager = 0x540;
constexpr size_t kGameUiStateMachine = 0x800;
constexpr size_t kGameUiViewModelProvider = 0x8a8;
constexpr size_t kGameUiTwitchManager = 0x32e0;
constexpr size_t kGameUiTutorialManager = 0x33e8;
constexpr size_t kGameUiEventsLock = 0x38f8;
constexpr size_t kGameUiUpdateLock = 0x390c;
constexpr size_t kGameUiRenderLock = 0x3920;
constexpr size_t kGameUiRenderOffscreenLock = 0x3934;

// ui::StateMachineComponent: Norbyte's ui::UIStateMachine. As the Windows
// one up to 0x170; then an array and two sets of GUIDs the Windows one does
// not have, which put every member up to the player's id 0x70 further in;
// after the id an array of deferred events and four critical sections, and
// three lock-free queues to the object's end.
constexpr size_t kUiStatesState = 0x98;                 // ui::State*
constexpr size_t kUiStatesExtraArray = 0x170;           // pointer, two 32-bit words
constexpr size_t kUiStatesExtraSets = 0x180;            // two ls::HashTable<ls::Guid>, 0x30 bytes each
constexpr size_t kUiStatesStateSet = 0x1e0;             // ls::HashTable<ui::State*>
constexpr size_t kUiStatesRootState = 0x270;            // Noesis::FixedString<24>: 0x20 bytes
constexpr size_t kUiStatesStates = 0x290;               // the collection of state templates
constexpr size_t kUiStatesPlayer = 0x470;               // 16 bits
constexpr size_t kUiStatesDeferredEvents = 0x478;       // array
constexpr size_t kUiStatesLocks = 0x488;                // four ls::CriticalSection
constexpr size_t kUiStatesLockCount = 4;
constexpr size_t kUiStatesSetBytes = 0x30;

// The templates the state machine is made from, as their StaticFillClassType
// registers each member with NoesisGUI by its offset (Norbyte's ui::UIState,
// UIStateWidget, UIStateEvent, UIInitialSubstate). After the object's 0x10
// bytes, NoesisGUI strings of 0x20 bytes: ui::StateTemplate's thirteen, with
// ForceVisible before ForceSplitscreen, then three collections and the
// string ActivateOnRestore; ui::StateWidget's three, a Uri, three flags and
// its libraries; ui::StateEvent's name, then whether it handles the event in
// a byte and its actions; ui::StateInitialSubstate's two.
constexpr size_t kUiTemplateFirstString = 0x10;
constexpr size_t kUiTemplateString = 0x20;
constexpr size_t kUiTemplateForceVisible = 0xf0;
constexpr size_t kUiTemplateForceSplitscreen = 0x110;
constexpr size_t kUiTemplateDisableSoundLayersAbove = 0x130;
constexpr size_t kUiTemplateInitialSubstates = 0x1b0;
constexpr size_t kUiTemplateEvents = 0x1b8;
constexpr size_t kUiTemplateWidgets = 0x1c0;
constexpr size_t kUiTemplateActivateOnRestore = 0x1c8;
constexpr size_t kUiWidgetTemplateIgnoreHitTest = 0x280;
constexpr size_t kUiWidgetTemplateBlockedLoading = 0x281;
constexpr size_t kUiWidgetTemplateAllowCreationDelay = 0x282;
constexpr size_t kUiWidgetTemplateLibraries = 0x288;
constexpr size_t kUiEventHandlesEvent = 0x30;
constexpr size_t kUiEventActions = 0x38;

// gui::TwitchOverlayManager
constexpr size_t kTwitchManagerSize = kGameUiTutorialManager - kGameUiTwitchManager;    // 0x108
constexpr size_t kTwitchManagerLock = 0xd8;
constexpr size_t kTwitchManagerRequests = 0xf0;         // the request manager's pointer

// ui::Widget: Norbyte's ui::UIWidget. NoesisGUI's user control ends at 0x26c
// and the widget's critical section begins there, in the four bytes that
// pad the control to its alignment (the Windows widget has a 0x28-byte lock
// at 0x278). The members after it are as the Windows ones, 0x20 earlier up
// to the file name, which is a game string of 0x10 bytes (0x20 on Windows),
// and 0x30 earlier after it.
constexpr size_t kUiWidgetLock = 0x26c;                 // ls::CriticalSection
constexpr size_t kUiWidgetInputChildren = 0x280;        // two arrays
constexpr size_t kUiWidgetVector = 0x2a0;               // a NoesisGUI vector with no elements of its own: 0x10 bytes
constexpr size_t kUiWidgetContextMenuManager = 0x2b0;
constexpr size_t kUiWidgetTooltipManager = 0x2c0;
constexpr size_t kUiWidgetWindowManager = 0x2c8;
constexpr size_t kUiWidgetStateMachine = 0x2d0;
constexpr size_t kUiWidgetData = 0x2d8;                 // ui::StateWidget*, the template the widget was made from
constexpr size_t kUiWidgetInputManager = 0x2e0;
constexpr size_t kUiWidgetStateId = 0x2e8;              // the GUID of the widget's state
constexpr size_t kUiWidgetParentCanvas = 0x2f8;
constexpr size_t kUiWidgetLayerIndex = 0x300;           // 32 bits
constexpr size_t kUiWidgetIndexInLayer = 0x304;         // 32 bits
constexpr size_t kUiWidgetFileName = 0x308;             // ls::STDString
constexpr size_t kUiWidgetContextMenu = 0x318;          // five pointers
constexpr size_t kUiWidgetFocusRefresh = 0x320;
constexpr size_t kUiWidgetEvents = 0x340;               // ten 32-bit words
constexpr size_t kUiWidgetMoveFocusInputState = 0x364;  // the last of them
constexpr size_t kUiWidgetFlags = 0x368;                // three bytes
constexpr size_t kUiWidgetHasInput = 0x08;              // in the third: the widget takes input
constexpr size_t kUiWidgetPlayer = 0x36c;               // 16 bits

// ui::State: Norbyte's ui::UIStateInstance. Unlike the Windows one it keeps
// no array of widget metadata after its widgets, has a critical section
// after the player's id, and two more words before the collection of the
// widget templates it still has to load.
constexpr size_t kUiInstanceSize = 0xe8;
constexpr size_t kUiInstanceWidgets = 0x00;             // array of Noesis::Ptr<ui::Widget>
constexpr size_t kUiInstanceStacks = 0x10;              // array of ui::StateStack*
constexpr size_t kUiInstanceTemplate = 0x20;            // ui::StateTemplate*, counted
constexpr size_t kUiInstancePlayer = 0x28;              // 16 bits
constexpr size_t kUiInstanceLock = 0x2c;                // ls::CriticalSection
constexpr size_t kUiInstanceFlags = 0x40;               // 16 bits
constexpr size_t kUiInstanceLayout = 0x42;              // one byte
constexpr size_t kUiInstanceOwner = 0x43;               // one byte
constexpr size_t kUiInstanceByte = 0x44;
constexpr size_t kUiInstanceName = 0x48;                // a fixed string, and 32 bits after it
constexpr size_t kUiInstanceStrings = 0x50;             // three NoesisGUI strings, the third the tutorial layer
constexpr size_t kUiInstanceGuid = 0xb0;                // the state's GUID
constexpr size_t kUiInstanceStackGuid = 0xc0;           // the GUID of the stack the state is in
constexpr size_t kUiInstanceExtra = 0xd0;               // two words
constexpr size_t kUiInstanceWidgetTemplates = 0xe0;     // collection of ui::StateWidget

// ui::StateStack: what a state's array of stacks points to.
constexpr size_t kUiStackSize = 0x30;
constexpr size_t kUiStackStates = 0x00;                 // array of ui::State*
constexpr size_t kUiStackGuid = 0x10;
constexpr size_t kUiStackParent = 0x20;                 // the GUID of the state the stack is under

}
