#pragma once

#include <cstdint>
#include <type_traits>

// ecs::EntityRef as the macOS game passes it to a subscriber of a component's
// signals: by value, the handle's bits in one register and the entity world
// in the next (Binding/ComponentSignals.h). Plain data, so that a function
// declared with it takes it the same way.

namespace bg3se::mac::binding {

struct GameEntityRef
{
    uint64_t Handle;
    void* World;
};
static_assert(sizeof(GameEntityRef) == 16 && std::is_trivially_copyable_v<GameEntityRef>
    && std::is_standard_layout_v<GameEntityRef>);

}
