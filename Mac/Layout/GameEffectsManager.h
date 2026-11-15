#pragma once

#include <cstddef>

// Where the Mac game keeps the members of ls::EffectsManager, as its own
// code shows them (Layout/GameShapes.cpp says which code). Norbyte's
// declaration (GameDefinitions/Components/Effect.h) is asserted to give the
// same offsets (Layout/DefinitionChecks.cpp).
//
// The object differs from the Windows one in a single member: the id of the
// thread that created it is kept in 64 bits here (the thread's Mach port,
// widened), where Windows has a 32-bit thread id. Everything after it is 8
// bytes further on.

namespace bg3se::mac {

constexpr size_t kEffectsManagerSize = 0x1e0;
constexpr size_t kEffectsSignalConnections = 0x20;      // the array of OverlayMaterialRequestSignal, after its 64-bit counter at +0x18
constexpr size_t kEffectsCanUpdate = 0x30;
constexpr size_t kEffectsCreateThread = 0x38;           // 64 bits
constexpr size_t kEffectsDestroyData = 0x40;
constexpr size_t kEffectsInvokes = 0x50;
constexpr size_t kEffectsInvokesLock = 0x68;
constexpr size_t kEffectsSoundObjects = 0x80;
constexpr size_t kEffectsCommandBufferLock = 0xd0;
constexpr size_t kEffectsCommandBuffer = 0xe8;
constexpr size_t kEffectsCommandBuffers = 0x108;
constexpr size_t kEffectsOverlayRequestsLock = 0x118;
constexpr size_t kEffectsOverlayRequests = 0x130;
constexpr size_t kEffectsNonConstructedLock = 0x140;
constexpr size_t kEffectsNonConstructed = 0x158;
constexpr size_t kEffectsVertexBuffers = 0x188;         // three pointers: +0x188, +0x190, +0x198

}
