#pragma once

#include <cstddef>

// Where the Mac game keeps the members of ls::ResourceManager, as its own
// code shows them (Layout/GameShapes.cpp says which code). Norbyte's
// declaration (GameDefinitions/Resources.h) is asserted to give the same
// offsets (Layout/DefinitionChecks.cpp).
//
// The class has no virtual functions in this game. Its first member is a
// reader-writer lock (ls::SRWKernelLock, 16 bytes here; the Windows SRWLOCK
// is 8, which is the pointer-sized word Norbyte's declaration starts with).
// Everything after it is 8 bytes further on than on Windows.

namespace bg3se::mac {

constexpr size_t kResourceManagerSize = 0x140;
constexpr size_t kResourceManagerLock = 0;                  // ls::SRWKernelLock: its two semaphores at +0 and +4
constexpr size_t kResourceManagerPreviewResources = 0x10;
constexpr size_t kResourceManagerBanks = 0x28;              // two pointers
constexpr size_t kResourceManagerEffectDependencies = 0x48;
constexpr size_t kResourceManagerTextureManager = 0x80;
constexpr size_t kResourceManagerSoundManager = 0x90;
constexpr size_t kResourceManagerCriticalSection = 0xb8;
constexpr size_t kResourceManagerDependencies = 0xd0;
constexpr size_t kResourceManagerSources = 0xe0;
constexpr size_t kResourceManagerVisualLoaders = 0xf8;
constexpr size_t kResourceManagerGenomeManagers = 0x108;

// ls::PhysicsBox and ls::PhysicsCapsule, the objects of a physics resource's
// templates, 0x20 bytes as the game makes them: after the vtable whether the
// object is kinematic in the byte at +8, its mass at +0xc and its name at
// +0x10; a box's extents after the name (three floats), a capsule's height
// at +0x14 and radius at +0x18.
constexpr size_t kPhysicsKinematic = 0x08;
constexpr size_t kPhysicsMass = 0x0c;
constexpr size_t kPhysicsName = 0x10;
constexpr size_t kPhysicsBoxExtents = 0x14;
constexpr size_t kPhysicsCapsuleHeight = 0x14;
constexpr size_t kPhysicsCapsuleRadius = 0x18;
constexpr size_t kPhysicsObjectSize = 0x20;

}
