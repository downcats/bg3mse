#pragma once

#include <cstddef>

// Objects of the Mac game that are without a member Norbyte's declarations
// have, or have a shorter one, as the game's own code shows (Layout/GameShapes.cpp says
// which code). The declarations are asserted to give these offsets
// (Layout/DefinitionChecks.cpp). No function of his reads the members that
// move; the declarations are kept true so that none can read them wrong.

namespace bg3se::mac {

// gn::GenomeManager: no lock and no array of pending blueprints before the times.
constexpr size_t kGenomeUnloadBatch = 0x500;
constexpr size_t kGenomeUnloadBatchJob = 0x508;
constexpr size_t kGenomeSyncedTime = 0x510;         // two doubles
constexpr size_t kGenomeIsMasterTime = 0x520;

// ls::level::LevelInstanceLoadSystem: no signal before the level manager.
constexpr size_t kLevelLoadSystemSize = 0x48;
constexpr size_t kLevelLoadSystemManager = 0x10;
constexpr size_t kLevelLoadSystemRequests = 0x18;
constexpr size_t kLevelLoadSystemLock = 0x30;

// esv::progression::ManagementSystem: no lock after its two pointers.
constexpr size_t kProgressionSystemSize = 0x90;
constexpr size_t kProgressionSystemPointers = 0x80;

// eoc::SharedDialogGameInterface: each of its two queues of load requests is
// 26 words, not 27. The dialog manager follows the array of requests; the
// server's dialog system holds the interface after four pointers. What
// follows the manager is not proven: of the two objects, his code reads only
// the manager (Ext.Utils.GetDialogManager).
constexpr size_t kDialogInterfaceSecondQueue = 0xd8;
constexpr size_t kDialogInterfaceRequests = 0x1a8;
constexpr size_t kDialogInterfaceManager = 0x1b8;
constexpr size_t kDialogSystemInterface = 0x30;

// LSDialogs::DialogManager: Norbyte's dlg::DialogManager. After his
// field_F8 the macOS game has another array, then the lock (20 bytes) and
// the mod manager; his members through field_F8 are where his declaration
// puts them.
constexpr size_t kDialogManagerDialogs = 0x20;          // ls::RefMap: count, buckets' count, buckets
constexpr size_t kDialogManagerCachedDialogs = 0x80;
constexpr size_t kDialogManagerScriptFlags = 0xb8, kDialogManagerVariables = 0xc8;
constexpr size_t kDialogManagerSpeakerGroups = 0xd8, kDialogManagerDialogToVariable = 0xe8;
constexpr size_t kDialogManagerFieldF8 = 0xf8;
constexpr size_t kDialogManagerField108 = 0x108;
constexpr size_t kDialogManagerLock = 0x118;
constexpr size_t kDialogManagerModManager = 0x130;

// LSDialogs::Dialog: Norbyte's dlg::Dialog, at the members his loading and
// releasing of a dialogue reads and writes (Ext.StaticData.LoadResource and
// ReleaseResource). The file name is an ls::STDString (16 bytes, its last
// byte the flag of a long string); the visitor PerformDialogLoad is given
// and its data follow the reference count's word.
constexpr size_t kDialogFileName = 0xe8;
constexpr size_t kDialogUuid = 0xf8;
constexpr size_t kDialogTimelineId = 0xfc;
constexpr size_t kDialogUnloadedTime = 0x100;       // a double: ls::GameTime's time when the last reference went
constexpr size_t kDialogRefCount = 0x108;
constexpr size_t kDialogNodeVisitor = 0x110;

// ecl::TimelineSystem: its three listeners follow one another (Norbyte's
// declaration has a word after the first), so everything after them is 8
// bytes lower than his Windows layout, to the object's end: the systems its
// constructor is given, the dialog manager the client's dialog system puts
// into it (Ext.Utils.GetDialogManager on the client), the arrays of requests
// its event handlers append to, and the flags that end the object.
constexpr size_t kClientTimelineStateListener = 0x18;
constexpr size_t kClientTimelineDialogListener = 0x20;
constexpr size_t kClientTimelineSoundSystem = 0x2b8;
constexpr size_t kClientTimelineMusicManager = 0x2f8;
constexpr size_t kClientTimelineDialogManager = 0x318;
constexpr size_t kClientTimelineUnloadRequests = 0x770;
constexpr size_t kClientTimelineCameraRequests = 0x780;
constexpr size_t kClientTimelineSessionLoaded = 0x137e;         // three of the flags that end the object
constexpr size_t kClientTimelineLevelUnloaded = 0x1380;
constexpr size_t kClientTimelineLevelDeactivated = 0x1382;

}
