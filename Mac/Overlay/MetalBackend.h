// The Mac rendering backend of Norbyte's ImGui overlay, where Windows has
// Extender/Client/IMGUI/DX11.inl and Vulkan.inl: the Mac game renders with
// Metal. IMGUI.cpp includes this file in their place and creates the backend.
//
// The backend draws as Norbyte's Vulkan backend does: in a render pass of its
// own, onto the image about to be presented, after everything the game drew,
// on the thread that presents. In the Mac game that thread is the main
// thread; the frame is built on the client thread as on Windows, and handed
// over the same way (a ring of three viewports whose draw lists are copied
// when a frame is finished, under one lock). The textures ImGui makes for
// itself (its font atlas, which grows while frames are built) are handed
// over as on Windows too: the main thread draws with Norbyte's mirrored
// copies of them (Extender/Client/IMGUI/TextureMirror.inl), brought up to
// date under the same lock, and never reads the ones the client thread is
// writing.
//
// What it uses in the game is checked first (Mac/Overlay/Dependencies.h);
// where a check does not pass the backend never reports itself initialised,
// which leaves the overlay in the state Norbyte's code has before a swap
// chain exists, and the game untouched.
//
// This header is all IMGUI.cpp sees of Mac/Overlay: the backend's state and
// everything it is made of are in MetalBackend.cpp.
#pragma once

// The headers Norbyte's two backends include, less the graphics APIs:
// IMGUI.cpp relies on them.
#include <GameDefinitions/Base/Base.h>
#include <Extender/Client/IMGUI/Backends.h>
#include <Extender/ScriptExtender.h>
#include <CoreLib/Wrappers.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <memory>

BEGIN_NS(extui)

class IMGUIManager;

class MetalBackend : public RenderingBackend
{
public:
    MetalBackend(IMGUIManager& ui);
    ~MetalBackend() override;

    void EnableHooks() override;
    void DisableHooks() override;
    void InitializeUI() override;
    void DestroyUI() override;
    void NewFrame() override;
    void FinishFrame() override;
    void ClearFrame() override;
    // The descriptor is the game's texture object, which on Mac is an
    // rf::Texture (what LoadGameTexture returns), not one of the two shapes
    // Norbyte's TextureDescriptor names.
    std::optional<TextureLoadResult> RegisterTexture(TextureDescriptor* descriptor) override;
    void UnregisterTexture(TextureOpaqueHandle opaqueHandle) override;
    std::optional<ImTextureID> BindTexture(TextureOpaqueHandle opaqueHandle) override;
    bool IsInitialized() override;
    glm::ivec2 GetViewportSize() override;

    // The client thread's turn, at the start of every IMGUIManager::Update().
    // The game makes its device and presents its frames on the main thread;
    // what the backend learns there it acts on here: the first frame
    // initialises the overlay (IMGUIManager::OnRenderBackendInitialized), and
    // a drawable of another size has the scale computed again, as the
    // creation of a swap chain does on Windows.
    void ClientUpdate();

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Whether the texture loader may touch the game: everything it reads and
// calls there has passed its checks, Norbyte's ResourceManager has the
// resource banks and the texture manager where the Mac game's code has them,
// and the function that releases a texture is bound. Says why not, once, in
// the log.
bool GameTexturesUsable();

// What ls__AppliedMaterial__LoadTexture(nullptr, guid) does on Windows, for a
// texture resource already looked up: the Mac game has that function's code
// only inlined into its callers. Registers the resource's file with the
// texture manager, marks the resource as the game does and instantiates the
// texture, which takes a reference that ls__TextureManager__UnloadTexture
// gives back. Null when the game gave no texture (no reference is then held)
// or when what this depends on in the game did not pass its checks.
TextureDescriptor* LoadGameTexture(resource::TextureResource* resource);

END_NS()
