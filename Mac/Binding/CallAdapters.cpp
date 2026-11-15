#include <Binding/CallAdapters.h>
#include <Binding/StatsFunctions.h>
#include <Platform/Log.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

namespace bg3se::mac::binding {

namespace {

// The game functions. Null until the binding gives each adapter its target;
// StaticSymbols holds an adapter only once its target is set.
adapter::GstGet* gGstGet{ nullptr };
adapter::GstAdd* gGstAdd{ nullptr };
adapter::CreateAction* gCreateAction{ nullptr };
adapter::FindPathImmediate* gFindPathImmediate{ nullptr };
adapter::LoadVirtualTexture* gLoadVirtualTexture{ nullptr };
adapter::TryOverrideTexture2DParameter* gTryOverrideTexture2DParameter{ nullptr };
adapter::LoadXaml* gLoadXaml{ nullptr };
adapter::PutTexture* gPutTexture{ nullptr };
adapter::OsirisAddStr* gOsirisAddStr{ nullptr };
// ls::ResourceManager::m_ptr: the variable that holds the resource manager.
void** gResourceManager{ nullptr };

// What ls__GlobalAllocator__Get returns. The game has no allocator object
// that its allocation code uses; Norbyte's code only passes the value back.
char gAllocatorToken;

template <class T>
T ReadAt(void const* object, size_t offset)
{
    T value;
    memcpy(&value, static_cast<char const*>(object) + offset, sizeof(value));
    return value;
}

}

namespace adapter {

// ls::gst::Get(unsigned int) takes the index by value and returns the
// characters in x0 and the length in w1.
GameStringView* FixedStringGetString(uint32_t const* self, GameStringView& out)
{
    out = gGstGet(*self);
    return &out;
}

// ls::gst::Add(ls::_StringView<char> const&) takes the view alone and returns
// the index. The game's own callers do not call it for no text and store the
// null index instead.
uint32_t* MainTableCreateFromString(void*, uint32_t* out, GameStringView* text)
{
    if (text == nullptr || text->Text == nullptr || text->Length == 0) {
        *out = 0xffffffffu;
    } else {
        *out = gGstAdd(text);
    }
    return out;
}

// esv::SurfaceActionFactory::CreateAction takes the story action id and the
// class descriptions as its third and fourth arguments and stores them in the
// action (at +0x10 and +0x50) without reading through either. The Windows
// function takes neither: there the caller stores them afterwards, and
// Norbyte's caller stores the class descriptions. The game's callers that
// have no story action pass 0.
void* SurfaceActionFactoryCreateAction(void* factory, uint8_t type, uint64_t handle)
{
    return gCreateAction(factory, type, 0, nullptr, handle);
}

// eoc::AiGrid::FindPathImmediate looks the id up among the grid's paths and
// reads the path it found without testing that it found one: an id that is
// not there, or kAiGridNoPath, faults. So the adapter makes the same lookup
// first and does not call the function for such an id. The function returns
// nothing; it returns once the path's search is complete. The result here is
// whether it ran.
bool AiGridFindPathImmediate(void* grid, int32_t pathId)
{
    if (pathId == kAiGridNoPath) return false;
    auto buckets = ReadAt<uint32_t>(grid, kAiGridPathBuckets);
    if (buckets == 0) return false;
    auto table = ReadAt<void* const*>(grid, kAiGridPathTable);
    // The game's own arithmetic: the id sign-extended to 64 bits, unsigned modulo.
    auto bucket = static_cast<uint64_t>(static_cast<int64_t>(pathId)) % buckets;
    void const* path = nullptr;
    for (void const* node = table[bucket]; node != nullptr; node = ReadAt<void const*>(node, kAiPathNodeNext)) {
        if (ReadAt<int32_t>(node, kAiPathNodeId) == pathId) {
            path = ReadAt<void const*>(node, kAiPathNodePath);
            break;
        }
    }
    if (path == nullptr) return false;
    gFindPathImmediate(grid, pathId);
    return true;
}

// ls::AppliedMaterial::LoadVirtualTexture takes the GUID alone.
void* AppliedMaterialLoadVirtualTexture(void*, uint32_t const* guid)
{
    return gLoadVirtualTexture(guid);
}

// ls::AppliedMaterial::TryOverrideTexture2DParameter takes the parameter's
// name by value: its 32-bit index.
bool AppliedMaterialTryOverrideTexture2DParameter(void* material, uint32_t const* parameter, uint32_t const* guid)
{
    return gTryOverrideTexture2DParameter(material, *parameter, guid);
}

// ls::TextureManager::PutTexture takes the texture manager, which the game
// has at kTextureManagerInResourceManager of the resource manager. Norbyte's
// caller passes the TextureManager member of his ResourceManager; the
// adapter does not use what it was passed and takes the manager from where
// the game takes it, an offset the engine table reads from the game's code.
bool TextureManagerUnloadTexture(void*, uint32_t const* guid)
{
    void* resourceManager = gResourceManager ? *gResourceManager : nullptr;
    if (!resourceManager) return false;
    auto textureManager = ReadAt<void*>(resourceManager, kTextureManagerInResourceManager);
    if (!textureManager) return false;
    return gPutTexture(textureManager, guid);
}

// Noesis::GUI::LoadXaml(char const*) takes the path alone and builds its
// result at the address in x8. Norbyte passes a Ptr that holds nothing, and
// the Windows function builds its result over it in the same way.
void** NoesisLoadXaml(void** result, char const* path)
{
    ::new (static_cast<void*>(result)) NoesisPtrResult(gLoadXaml(path));
    return result;
}

// COsiStringTable::AddStr(char const*, bool) takes the table, the text and
// the flag, and returns the handle (64 bits, in x0). The Windows function
// writes the handle through its second argument and returns that address. A
// null or empty text gives the handle 0, there as here.
uint64_t* OsirisAddString(void* stringTable, uint64_t& out, char const* text, bool isGuidString)
{
    // Once a session, that a string reaches the story this way.
    static std::once_flag logged;
    std::call_once(logged, [] { Log(LogLevel::Debug, "Adding a string to the Osiris string table"); });
    out = gOsirisAddStr(stringTable, text, isGuidString);
    return &out;
}

// The game allocates with malloc and posix_memalign and frees with free,
// inline, wherever Norbyte's Windows patterns found a call of its allocator:
// malloc for an alignment of up to 16 bytes, posix_memalign above (as
// ls::MemoryManager::Allocate, the nearest named function, does it). Memory
// from here can be freed by the game and the reverse.
void* GlobalAllocatorGet()
{
    return &gAllocatorToken;
}

void* GlobalAllocatorAlloc(void*, size_t size, int, int, uint64_t alignment)
{
    if (alignment <= 16) return malloc(size);
    void* memory = nullptr;
    if (posix_memalign(&memory, alignment, size) != 0) return nullptr;
    return memory;
}

void GlobalAllocatorFree(void*, void* memory)
{
    free(memory);
}

}

namespace {

template <class Function>
constexpr void** TargetOf(Function** target)
{
    return reinterpret_cast<void**>(target);
}

#define BG3MSE_ADAPTER(entry, function, target) \
    CallAdapter{ entry, kPrototypeOf<decltype(adapter::function)>, kPrototypeOf<std::remove_pointer_t<decltype(target)>>, \
        reinterpret_cast<void*>(&adapter::function), TargetOf(&target) }
#define BG3MSE_LIBSYSTEM_ADAPTER(entry, function) \
    CallAdapter{ entry, kPrototypeOf<decltype(adapter::function)>, Prototype{}, reinterpret_cast<void*>(&adapter::function), nullptr }

const CallAdapter kCallAdapters[] = {
    BG3MSE_ADAPTER("ls__FixedString__GetString", FixedStringGetString, gGstGet),
    BG3MSE_ADAPTER("ls__GlobalStringTable__MainTable__CreateFromString", MainTableCreateFromString, gGstAdd),
    BG3MSE_ADAPTER("esv__SurfaceActionFactory__DoCreateAction", SurfaceActionFactoryCreateAction, gCreateAction),
    BG3MSE_ADAPTER("eoc__AiGrid__FindPathImmediate", AiGridFindPathImmediate, gFindPathImmediate),
    BG3MSE_ADAPTER("ls__AppliedMaterial__LoadVirtualTexture", AppliedMaterialLoadVirtualTexture, gLoadVirtualTexture),
    BG3MSE_ADAPTER("ls__AppliedMaterial__TryOverrideTexture2DParameter", AppliedMaterialTryOverrideTexture2DParameter, gTryOverrideTexture2DParameter),
    BG3MSE_ADAPTER("Noesis__GUI__LoadXaml", NoesisLoadXaml, gLoadXaml),
    CallAdapter{ "ls__TextureManager__UnloadTexture", kPrototypeOf<decltype(adapter::TextureManagerUnloadTexture)>, kPrototypeOf<adapter::PutTexture>,
        reinterpret_cast<void*>(&adapter::TextureManagerUnloadTexture), TargetOf(&gPutTexture),
        "ls__gGlobalResourceManager", reinterpret_cast<void**>(&gResourceManager), kTextureManagerInResourceManager },
    BG3MSE_ADAPTER("Osiris.Globals.AddString", OsirisAddString, gOsirisAddStr),
    BG3MSE_LIBSYSTEM_ADAPTER("ls__GlobalAllocator__Get", GlobalAllocatorGet),
    BG3MSE_LIBSYSTEM_ADAPTER("ls__GlobalAllocator__Alloc", GlobalAllocatorAlloc),
    BG3MSE_LIBSYSTEM_ADAPTER("ls__GlobalAllocator__Free", GlobalAllocatorFree),
};

// The adapters of this file and those of Binding/StatsFunctions.cpp, as one list.
std::vector<CallAdapter> const& AllAdapters()
{
    static const std::vector<CallAdapter> all = [] {
        std::vector<CallAdapter> list(std::begin(kCallAdapters), std::end(kCallAdapters));
        auto stats = StatsFunctionAdapters();
        list.insert(list.end(), stats.begin(), stats.end());
        return list;
    }();
    return all;
}

}

std::span<CallAdapter const> CallAdapters()
{
    return AllAdapters();
}

CallAdapter const* FindCallAdapter(std::string_view entry)
{
    for (auto const& a : AllAdapters()) {
        if (entry == a.Entry) return &a;
    }
    return nullptr;
}

}
