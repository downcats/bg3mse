#include <stdafx.h>
#include <GameDefinitions/Character.h>
#include <GameDefinitions/Item.h>
#include <GameDefinitions/Components/All.h>
#include <Lua/Shared/Proxies/PropertyMapDependencies.h>

#include "BoundComponents.h"

BEGIN_SE()

namespace {

constexpr mac::BoundComponent kBoundComponents[] = {
#define T(cls) { cls::EngineClass, #cls, (uint32_t)sizeof(cls), IsProxyComponentType<cls>, cls::OneFrame },
#include <GameDefinitions/Components/AllComponentTypes.inl>
#undef T
};

}

std::span<mac::BoundComponent const> mac::BoundComponents()
{
    return kBoundComponents;
}

END_SE()
