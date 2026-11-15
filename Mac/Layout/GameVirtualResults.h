#pragma once

#include <cstddef>
#include <cstdint>

// How the virtual functions of game classes that Norbyte's code calls, or
// that the game calls on objects of his, hand back a result that is not one
// integer or pointer, as the Mac game's own code shows it (Layout/GameShapes.cpp
// says which code). The slot check (Layout/VirtualSlots.h) cannot see this:
// a function in the right slot is still called wrongly when its result
// travels otherwise than the declaration says. Norbyte's declarations are
// asserted to make clang pass the result the same way
// (Layout/DefinitionChecks.cpp).

namespace bg3se::mac {

// eoc::DynamicStatsExpressionManager::CreateStatsExpression returns an
// ls::SharedPtr<eoc::StatsExpressionInternal> through the address in x8: one
// pointer to an object of 0x28 bytes it allocates with malloc, zeroes, and
// whose count of references, at +0x20, it starts at 1.
constexpr size_t kStatsExpressionSize = 0x28;
constexpr size_t kStatsExpressionRefCount = 0x20;
constexpr uint32_t kStatsExpressionFirstCount = 1;

// eoc::spell_cast::BaseTarget's positions: a Vector3f comes back in s0, s1
// and s2; an ls::Optional<Vector3f> in x0 and x1, the three floats in the
// first 12 bytes and whether there is one in the byte after them; and an
// ls::Optional<ESpellTargetingType> is passed in one register, the type in
// the first byte and whether there is one in the second.
constexpr size_t kOptionalPositionSize = 16;
constexpr size_t kOptionalPositionEngaged = 12;
constexpr uint64_t kOptionalPositionEngagedMask = 0xffull << (8 * (kOptionalPositionEngaged - 8));
constexpr size_t kOptionalTargetingSize = 2;
constexpr uint64_t kOptionalTargetingEngagedMask = 0xff00;

// gn::TGenomeSet<T>::GetValueTypeHash() returns 32 bits, in w0.
constexpr size_t kGenomeTypeHashSize = 4;

}
