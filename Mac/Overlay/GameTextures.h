#pragma once

#include <cstdint>

// Where the Mac game keeps what the overlay's texture loading reads and
// writes, as the game's own code shows it: the offsets the texture loading
// inlined into ls::AppliedMaterial::TryOverrideTexture2DParameter uses, which
// the engine table's overlay area reads back from that function.
//
// The numbers are stated once, here. Mac/Overlay/GameTextures.cpp asserts,
// when it is compiled, that Norbyte's declarations (GameDefinitions/
// Resources.h) put their members at these places in this build, so that the
// loading can use his members; Mac/Overlay/Dependencies.cpp compares them
// with the game's code when the game runs, and the textures are off where
// they differ.
//
// The two members of ResourceManager the loading goes through (the resource
// banks and the texture manager) are not among them: the game's code gives
// their places, and whether Norbyte's declaration has them there is asked
// when the game runs (GameTexturesUsable), not asserted.

namespace bg3se::mac::overlay::layout {

// resource::TextureResource and its bases.
constexpr uint32_t kResourceSourceFile = 0x08;      // Resource::SourceFile
constexpr uint32_t kResourceGuid = 0x18;            // Resource::Guid
constexpr uint32_t kResourceState = 0x20;           // LoadableResource::State
constexpr uint32_t kResourceStreaming = 0x34;       // TextureResource::Streaming
constexpr uint32_t kResourceSRGB = 0x35;            // TextureResource::SRGB
constexpr uint32_t kResourceType = 0x36;            // TextureResource::Type

// A resource's load state as the inlined loading writes it: registered with
// the texture manager, or nothing to register (no source file).
constexpr uint8_t kStateRegistered = 2, kStateNothingToLoad = 3;
// The first texture type that is not registered as a texture (a render
// target; anything above it the game treats as an error).
constexpr uint8_t kFirstTypeNotATexture = 8;

}
