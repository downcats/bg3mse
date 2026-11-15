#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <Binding/Prototype.h>
#include <Layout/GameMembers.h>

// Adapters for the game functions Norbyte's code calls whose argument passing
// in the macOS game is not what his prototype passes. Each is a function with
// Norbyte's prototype that calls the game function with what it takes here;
// StaticSymbols holds the adapter, and the adapter's target holds the game
// function.
//
// The types here are plain ones with the passing of Norbyte's: the file
// includes none of his headers, so the adapters can be called by a test
// without his code. Mac/Binding/StaticSymbolRows.cpp, which sees both, proves
// at compile time that each adapter's prototype passes what Norbyte's does.
// The other side, what the adapter passes to the game, is compared with the
// engine table's entry by a test and again before the adapter is bound.
//
// Every statement about a game function below was read from its code in game
// build 4.1.1.7631656; the resolver's shape check on the entry is what keeps
// it true: an adapter is bound only while its entry resolves.

namespace bg3se::mac::binding {

// ls::_StringView<char>, Norbyte's LSStringView.
struct GameStringView
{
    char const* Text;
    uint32_t Length;
};
template <> inline constexpr engine::Returns kComesBackIn<GameStringView> = engine::Returns::X0W1;

// What Noesis::GUI::LoadXaml returns: a Noesis::Ptr, one pointer, which is
// not trivially copyable and so comes back through the address in x8.
struct NoesisPtrResult
{
    void* Object;
    NoesisPtrResult(NoesisPtrResult const&);
    ~NoesisPtrResult();
};
template <> inline constexpr engine::Returns kComesBackIn<NoesisPtrResult> = engine::Returns::IndirectX8;

namespace adapter {

// --- Norbyte's prototypes ----------------------------------------------------

// ls__FixedString__GetString: FixedString::GetStringProc.
GameStringView* FixedStringGetString(uint32_t const* self, GameStringView& out);
// ls__GlobalStringTable__MainTable__CreateFromString: MainTable::CreateFromStringProc.
uint32_t* MainTableCreateFromString(void* mainTable, uint32_t* out, GameStringView* text);
// esv__SurfaceActionFactory__DoCreateAction: esv::SurfaceActionFactoryCreateProc.
void* SurfaceActionFactoryCreateAction(void* factory, uint8_t type, uint64_t handle);
// eoc__AiGrid__FindPathImmediate: AiGrid__FindPathImmediateProc.
bool AiGridFindPathImmediate(void* grid, int32_t pathId);
// ls__AppliedMaterial__LoadVirtualTexture: AppliedMaterial__LoadVirtualTextureProc.
void* AppliedMaterialLoadVirtualTexture(void* material, uint32_t const* guid);
// ls__AppliedMaterial__TryOverrideTexture2DParameter: AppliedMaterial__TryOverrideTexture2DParameterProc.
bool AppliedMaterialTryOverrideTexture2DParameter(void* material, uint32_t const* parameter, uint32_t const* guid);
// ls__TextureManager__UnloadTexture: TextureManager__UnloadTextureProc.
bool TextureManagerUnloadTexture(void* textureManager, uint32_t const* guid);
// Noesis__GUI__LoadXaml: LoadXamlProc (GameDefinitions/UI.h).
void** NoesisLoadXaml(void** result, char const* path);
// Osiris.Globals.AddString: COsiStringTableAddStringProc (GameDefinitions/Osiris.h).
uint64_t* OsirisAddString(void* stringTable, uint64_t& out, char const* text, bool isGuidString);
// ls__GlobalAllocator__Get, __Alloc, __Free (CoreLib/Base/BaseMemory.h).
void* GlobalAllocatorGet();
void* GlobalAllocatorAlloc(void* allocator, size_t size, int pool, int unused, uint64_t alignment);
void GlobalAllocatorFree(void* allocator, void* memory);

// --- the game functions, as the adapters call them ---------------------------

using GstGet = GameStringView(uint32_t index);
using GstAdd = uint32_t(GameStringView const* text);
using CreateAction = void*(void* factory, uint32_t type, uint32_t storyActionId, void const* classDescriptions, uint64_t handle);
using FindPathImmediate = void(void* grid, int32_t pathId);
using LoadVirtualTexture = void*(uint32_t const* guid);
using TryOverrideTexture2DParameter = bool(void* material, uint32_t parameter, uint32_t const* guid);
using PutTexture = bool(void* textureManager, uint32_t const* guid);
using LoadXaml = NoesisPtrResult(char const* path);
using OsirisAddStr = uint64_t(void* stringTable, char const* text, bool isGuidString);

}

// A further thing of the game that an adapter uses: a function it calls,
// with the prototype it calls it through, or (no prototype) a variable,
// object or vtable whose address it takes.
struct AdapterNeed
{
    char const* Entry;
    Prototype Game;
    void** Target;          // where the address goes
};

struct CallAdapter
{
    char const* Entry;      // the table entry, which is the StaticSymbols member's name
    Prototype Norbyte;      // the adapter's own prototype
    Prototype Game;         // what it calls the game function through; not present when it calls none
    void* Function;         // the adapter
    void** Target;          // where the game function's address goes; nullptr when it calls none
    // A global the adapter reads besides: its entry, where its address goes,
    // and the offset the adapter reads there, which the function's entry
    // must state among its operands.
    char const* Global{ nullptr };
    void** GlobalTarget{ nullptr };
    uint64_t GlobalOffset{ 0 };
    // Everything else of the game it uses. An adapter without a target that
    // has needs does, in this image, the work of a function the game has only
    // inline (Binding/StatsFunctions.h): it is bound while the table confirms
    // that the function is absent and that the code it was written from is
    // unchanged, and every need resolved.
    std::span<AdapterNeed const> Needs{};
};

std::span<CallAdapter const> CallAdapters();
CallAdapter const* FindCallAdapter(std::string_view entry);

// Where the game has the texture manager in the resource manager
// (ls::ResourceManager::RemoveTexture loads it from there before it calls
// ls::TextureManager::PutTexture): the member of Layout/GameResourceManager.h.
constexpr size_t kTextureManagerInResourceManager = kResourceManagerTextureManager;

// The fields of eoc::AiGrid and of a path that AiGridFindPathImmediate reads
// before it calls the game function, as the game function itself reads them
// (the function is 33 instructions; the offsets are in its code, which the
// entry's shape covers), are kAiGridPathBuckets, kAiGridPathTable and the
// kAiPathNode members of Layout/GameMembers.h. The id that names no path:
constexpr int32_t kAiGridNoPath = -1337;

}
