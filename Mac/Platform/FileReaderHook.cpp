#include <stdafx.h>
#include <GameDefinitions/FileReader.h>
#include <GameHooks/EngineHooksFwdDecl.h>
#include <Platform/EntryHook.h>
#include <Platform/FileReaderHook.h>

namespace bg3se::mac {

namespace {

constexpr char kConstructorSymbol[] = "__ZN2ls10FileReaderC1ERKNS_4PathENS0_5ETypeENS_9EnumFlagsINS_10EFileFlagsEEE";
constexpr char kReadSymbol[] = "__ZN2ls10FileReader4ReadEPvm";

FileReader__CtorProc* gOriginal = nullptr;
FileReaderSubscriber* gSubscriber = nullptr;
std::atomic<uint64_t> gOpens{ 0 };

// Never inlined, never cloned: its first instruction is where a second hook
// goes (FileReaderHook.h), so every call must arrive there.
__attribute__((noinline, used))
FileReader* OnFileReaderCreate(FileReader* self, Path const& path, unsigned int type, unsigned int openFlags)
{
    FileReader* result = gOriginal(self, path, type, openFlags);
    gOpens.fetch_add(1, std::memory_order_relaxed);

    std::span<uint8_t const> data;
    try {
        data = gSubscriber->OnFileOpened(std::string_view(path.Name.data(), path.Name.size()));
    } catch (...) {
        // Never into the game's caller: the reader stays as the game built it.
    }
    if (!data.empty()) {
        // What LoadFile leaves for a file read into memory. Read() and the
        // loaders that inline it use exactly these fields; the destructor
        // releases what the game itself opened and never frees DataPtr.
        self->IsLoaded = true;
        self->DataPtr = const_cast<uint8_t*>(data.data());
        self->ReadPtr = self->DataPtr;
        self->FileSize = data.size();
        self->Type = FileMappingType::MemBuffer;
    }
    return result;
}

bool Install(void* constructor, uint64_t bytes, FileReaderSubscriber* subscriber, std::string& problem)
{
    // gOriginal is set to the trampoline before the patch goes live.
    gSubscriber = subscriber;
    auto hook = InstallEntryHook(constructor, bytes, (void const*)&OnFileReaderCreate, reinterpret_cast<void**>(&gOriginal), problem);
    if (!hook) {
        gOriginal = nullptr;
        gSubscriber = nullptr;
        return false;
    }
    return true;
}

}

// Read from build 4.1.1.7631656 (arm64), where the constructor zeroes IsLoaded,
// DataPtr, ReadPtr, FileSize and the handles, stores the mapping type at +0x80
// and FileObject at +0x88, and Read consumes those same fields.
FileReaderShape const kFileReaderShape{
    .Constructor = { 48, 0x915fde595be89045ull },
    .Read = { 36, 0xd4a7640e9a01d768ull },
};

FileReaderCheck CheckFileReader(GameImage const& image)
{
    FileReaderCheck check;
    uint64_t constructorVA = image.FindVA(kConstructorSymbol);
    check.Constructor = constructorVA ? image.CodeAt(constructorVA) : nullptr;
    void const* read = image.FindCode(kReadSymbol);
    if (!check.Constructor || !read) {
        check.Problem = "ls::FileReader's constructor or Read is not in the game's symbol table under the expected signature";
        return check;
    }

    auto ctorShape = ShapeToFirstReturn(check.Constructor);
    auto readShape = ShapeToFirstReturn(read);
    if (!ctorShape || !readShape) {
        check.Problem = "ls::FileReader's constructor or Read has no return where one was expected";
        return check;
    }
    check.Found = { *ctorShape, *readShape };
    if (check.Found.Constructor != kFileReaderShape.Constructor || check.Found.Read != kFileReaderShape.Read) {
        check.Problem = "ls::FileReader's code changed, so its layout has to be checked again";
        return check;
    }

    uint64_t end = image.NextSymbolVA(constructorVA);
    if (auto refusal = CheckEntryHook(image, constructorVA, end); refusal != HookRefusal::None) {
        check.Problem = std::string("the constructor cannot be hooked: ") + Describe(refusal);
        return check;
    }
    check.ConstructorBytes = end - constructorVA;
    return check;
}

bool InstallFileReaderHook(FileReaderSubscriber* subscriber)
{
    if (gOriginal) return true;

    auto image = GameImage::Current();
    if (!image) {
        WARN("[FileReader] the game's symbol table cannot be read; not hooked");
        return false;
    }
    auto check = CheckFileReader(*image);
    if (!check.Ok()) {
        WARN("[FileReader] not hooked: %s (constructor %u/%016llx, Read %u/%016llx)", check.Problem.c_str(),
            check.Found.Constructor.Instructions, (unsigned long long)check.Found.Constructor.Hash,
            check.Found.Read.Instructions, (unsigned long long)check.Found.Read.Hash);
        return false;
    }

    std::string problem;
    if (!Install(const_cast<void*>(check.Constructor), check.ConstructorBytes, subscriber, problem)) {
        ERR("[FileReader] hooking the constructor at %p failed: %s", check.Constructor, problem.c_str());
        return false;
    }
    INFO("[FileReader] constructor hooked at %p", check.Constructor);
    return true;
}

bool InstallFileReaderHookOn(void* constructor, uint64_t bytes, FileReaderSubscriber* subscriber)
{
    if (gOriginal) return false;
    std::string problem;
    return Install(constructor, bytes, subscriber, problem);
}

void* FileReaderHookFunction()
{
    return gOriginal ? reinterpret_cast<void*>(&OnFileReaderCreate) : nullptr;
}

uint64_t FileReaderOpenCount()
{
    return gOpens.load(std::memory_order_relaxed);
}

}
