#include <stdafx.h>
#include <Mac/Overlay/MetalBackend.h>

#include <GameDefinitions/Resources.h>

#include <Overlay/Dependencies.h>
#include <Overlay/GameTextures.h>

#include <cstddef>
#include <cstdio>
#include <string>

// offsetof on Norbyte's structures, which have virtual functions: clang
// supports it and says that the standard only conditionally does.
#pragma clang diagnostic ignored "-Winvalid-offsetof"

BEGIN_NS(extui)

using namespace mac::overlay;

// Norbyte's declarations put these members where the Mac game has them
// (Mac/Overlay/GameTextures.h; the game's side is checked when it runs).
static_assert(offsetof(resource::Resource, SourceFile) == layout::kResourceSourceFile);
static_assert(offsetof(resource::Resource, Guid) == layout::kResourceGuid);
static_assert(offsetof(resource::LoadableResource, State) == layout::kResourceState);
static_assert(offsetof(resource::TextureResource, Streaming) == layout::kResourceStreaming);
static_assert(offsetof(resource::TextureResource, SRGB) == layout::kResourceSRGB);
static_assert(offsetof(resource::TextureResource, Type) == layout::kResourceType);
static_assert(sizeof(resource::TextureResource::Streaming) == 1 && sizeof(resource::TextureResource::SRGB) == 1
    && sizeof(resource::TextureResource::Type) == 1 && sizeof(resource::LoadableResource::State) == 1);

namespace {

// As the engine table states them (Engine/Entries/Overlay.cpp).
using AddTextureProc = void (TextureManager* self, Path const& sourceFile, FixedString const& guid);
using InstantiateTextureProc = TextureDescriptor* (TextureManager* self, FixedString const& guid, bool streaming, bool srgb, bool one);

}

// Why the texture loader may not touch the game, or nothing.
std::string TexturesProblem()
{
    auto const& dependencies = GameDependencies();
    if (!dependencies.Usable(Part::Textures)) return "what they use in the game did not pass its checks";

    // Norbyte's loader goes through ResourceManager's members itself: the
    // current resource bank to find the resource, the texture manager to
    // release the texture.
    char text[160];
    if (offsetof(ResourceManager, ResourceBanks) != dependencies.ResourceBanks) {
        snprintf(text, sizeof(text), "ResourceManager::ResourceBanks is at +0x%zx in this build and at +0x%x in the game",
            offsetof(ResourceManager, ResourceBanks), dependencies.ResourceBanks);
        return text;
    }
    if (offsetof(ResourceManager, TextureManager) != dependencies.TextureManager) {
        snprintf(text, sizeof(text), "ResourceManager::TextureManager is at +0x%zx in this build and at +0x%x in the game",
            offsetof(ResourceManager, TextureManager), dependencies.TextureManager);
        return text;
    }

    // What is loaded here is released by Norbyte's code through these two.
    auto const& symbols = GetStaticSymbols();
    if (!symbols.ls__gGlobalResourceManager || !symbols.ls__TextureManager__UnloadTexture) {
        return "the resource manager or the function that releases a texture is not bound";
    }
    return {};
}

bool GameTexturesUsable()
{
    static const bool usable = [] {
        auto problem = TexturesProblem();
        if (!problem.empty()) ERR("IMGUI: textures are off: %s", problem.c_str());
        return problem.empty();
    }();
    return usable;
}

TextureDescriptor* LoadGameTexture(resource::TextureResource* resource)
{
    if (!resource || !GameTexturesUsable()) return nullptr;
    auto const& dependencies = GameDependencies();
    auto resourceManager = *GetStaticSymbols().ls__gGlobalResourceManager;
    auto textureManager = resourceManager ? resourceManager->TextureManager : nullptr;
    if (!textureManager) return nullptr;

    // The game's own sequence. A render target is registered another way,
    // and Norbyte's loader asks for one- and two-dimensional textures only.
    if ((uint8_t)resource->Type >= layout::kFirstTypeNotATexture) return nullptr;
    if (!resource->SourceFile.Name.empty()) {
        auto addTexture = reinterpret_cast<AddTextureProc*>((uintptr_t)dependencies.AddTexture);
        addTexture(textureManager, resource->SourceFile, resource->Guid);
        resource->State = layout::kStateRegistered;
    } else {
        resource->State = layout::kStateNothingToLoad;
    }
    auto instantiateTexture = reinterpret_cast<InstantiateTextureProc*>((uintptr_t)dependencies.InstantiateTexture);
    return instantiateTexture(textureManager, resource->Guid, resource->Streaming, resource->SRGB, true);
}

END_NS()
