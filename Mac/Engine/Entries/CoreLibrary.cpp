#include <Engine/Entries/Areas.h>
#include <Layout/GameMembers.h>
#include <Layout/GameStringTable.h>

// The core library: the allocator, fixed strings, the thread registry, file
// readers and path roots (the first group of StaticSymbols).

namespace bg3se::mac::engine {

namespace {

constexpr char kRelease[] = "__ZN2ls3gst3Map7ReleaseERKNS0_12NodePoolDataE";
constexpr char kStringTable[] = "__ZN2ls3gst10s_InstanceE";
constexpr char kFixedStringDestructor[] = "__ZN2ls11FixedStringD1Ev";

// ls::FixedString::~FixedString releases its string:
//     adrp x8, s_Instance@PAGE ; ldr x8, [x8, s_Instance@PAGEOFF]
//     mov w9, #<offset of Main> ; add x0, x8, x9
//     add x1, sp, #<index>      ; bl ls::gst::Map::Release
constexpr MaskedWord kReleaseCall[] = {
    { 0x90000008, Any::Adrp },      // adrp x8, page
    { 0xf9400108, Any::Imm12 },     // ldr x8, [x8, #offset]
    { 0x52800009, Any::Imm16 },     // mov w9, #value
    { 0x8b090100 },                 // add x0, x8, x9
    { 0x910003e1, Any::Imm12 },     // add x1, sp, #offset
    { 0x94000000, Any::Branch },    // bl
};

// CoreLibSDM::Init builds the table (the function's whole shape is the
// CoreLibSDM::Init entry's; this entry is refused with it). It constructs
// the pools one after another in one block, the last of them:
//     mov w8, #<pool 10> ; add x20, x19, x8 ; adrp x2, spec@PAGE ; add x2, x2, spec@PAGEOFF
//     mov x0, x20 ; mov w1, #10 ; bl ls::gst::NodePool::NodePool
// then the map after them, whose epoch slots it fills first:
//     mov w8, #<offset of Main> ; add x9, x19, x8 ; mov x10, #-1 ; str x10, [x9]
// and stores the block's address in ls::gst::s_Instance. The one path of the
// function that does not come by that store is the one taken when the core
// library is initialised already.
constexpr char kCoreLibInit[] = "__ZN10CoreLibSDM4InitERKN2ls14ArgumentStringE";
constexpr char kCoreLibInitName[] = "CoreLibSDM::Init";
constexpr char kStringPoolConstructor[] = "__ZN2ls3gst8NodePoolC1EiRKNS0_8PoolSpecE";
constexpr MaskedWord kLastPoolConstructed[] = {
    { 0x52800008, Any::Imm16 },     // mov w8, #value
    { 0x8b080274 },                 // add x20, x19, x8
    { 0x90000002, Any::Adrp },      // adrp x2, page
    { 0x91000042, Any::Imm12 },     // add x2, x2, #offset
    { 0xaa1403e0 },                 // mov x0, x20
    { 0x52800141 },                 // mov w1, #10
    { 0x94000000, Any::Branch },    // bl
};
constexpr MaskedWord kMapConstructed[] = {
    { 0x52800008, Any::Imm16 },     // mov w8, #value
    { 0x8b080269 },                 // add x9, x19, x8
    { 0x9280000a },                 // mov x10, #-1
    { 0xf900012a },                 // str x10, [x9]
};

constexpr Operand kStringTableOperands[] = {
    {
        .What = "read as a pointer",
        .In = Exact(kFixedStringDestructor),
        .Pattern = kReleaseCall,
        .At = 0,
        .How = Operand::Read::PointerLoad,
        .Target = Exact(kStringTable),
    },
    {
        .What = "pools constructed",
        .In = Exact(kCoreLibInit),
        .How = Operand::Read::CallCount,
        .Expected = kStringPools,
        .Target = Exact(kStringPoolConstructor),
    },
    {
        .What = "the last pool's place",
        .In = Exact(kCoreLibInit),
        .Pattern = kLastPoolConstructed,
        .At = 0,
        .How = Operand::Read::MoveValue,
        .Expected = (kStringPools - 1) * kStringPoolSize,
    },
    {
        .What = "the last pool is that one",
        .In = Exact(kCoreLibInit),
        .Pattern = kLastPoolConstructed,
        .At = 6,
        .How = Operand::Read::BranchTarget,
        .Target = Exact(kStringPoolConstructor),
    },
    {
        .What = "Main follows the pools",
        .In = Exact(kCoreLibInit),
        .Pattern = kMapConstructed,
        .At = 0,
        .How = Operand::Read::MoveValue,
        .Expected = kStringTableMain,
    },
    {
        .What = "stored when the core library starts",
        .In = Exact(kCoreLibInit),
        .How = Operand::Read::StoreOrder,
        .Expected = 31,
        .Target = Exact(kStringTable),
    },
};

constexpr Operand kReleaseOperands[] = {
    {
        .What = "offset of Main",
        .In = Exact(kFixedStringDestructor),
        .Pattern = kReleaseCall,
        .At = 2,
        .How = Operand::Read::MoveValue,
        .Expected = kStringTableMain,
    },
    {
        .What = "that call",
        .In = Exact(kFixedStringDestructor),
        .Pattern = kReleaseCall,
        .At = 5,
        .How = Operand::Read::BranchTarget,
        .Target = Exact(kRelease),
    },
};

// ls::FileSystem::CollectFilesWithGlob makes a string of its pattern for the packed files:
//     ldr x1, [x8] ; ldr w2, [x8, #length] ; add x0, sp, #string ; bl ls::STDString::STDString(ls::_StringView<char> const&)
constexpr MaskedWord kGlobToString[] = {
    { 0xf9400101 },
    { 0xb9400102, Any::Imm12 },
    { 0x910003e0, Any::Imm12 },
    { 0x94000000, Any::Branch },
};
constexpr Operand kGlobOperands[] = {
    { .What = "the view's length", .Pattern = kGlobToString, .At = 1, .Expected = kStringViewLength },
    { .What = "read into a string", .Pattern = kGlobToString, .At = 3, .How = Operand::Read::BranchTarget,
      .Target = Exact("__ZN2ls9STDStringC1ERKNS_11_StringViewIcEE") },
    { .What = "files on disk", .How = Operand::Read::CallCount, .Expected = 1,
      .Target = Exact("__Z15do_CollectFilesRKN2ls4PathEPKcbRNS_12DynamicArrayIS0_NS_15TaggedAllocatorIiEEEE") },
};

constexpr Named kAllocatorHosts[] = { Exact("__ZN2ls13MemoryManager8AllocateEmNS_9AllocTypeEim") };

constexpr char kAllocatorFate[] =
    "the game has no allocator functions: its code calls malloc, posix_memalign and free of libSystem directly "
    "(ls::MemoryManager::Allocate is the nearest named function: malloc up to 16-byte alignment, posix_memalign above); "
    "ls::GlobalAllocator::m_ptr exists but only CoreLibSDM::Init and Shutdown refer to it. "
    "Memory from malloc can be freed by the game and the reverse";

constexpr char kFindFileFate[] =
    "Windows API. The game lists directories with opendir and readdir of libSystem and with NSFileManager; its own functions for "
    "it are ls::FileSystem::CollectFiles, CollectFilesWithExtension, CollectFilesWithGlob and CollectDirectories";
constexpr char kFindFileUse[] = "GameHooks/EngineHooks.inl: wrapped, and its handler is commented out: nothing observes it";

constexpr Entry kEntries[] = {
    {
        .Name = "ls__GlobalAllocator__Get",
        .What = Kind::NoCallableCode,
        .Use = "GameHooks/DataLibraries.cpp (BG3Alloc, BG3Free): the allocator object passed to Alloc and Free",
        .Gone = { .Fate = kAllocatorFate, .NoSymbol = "__ZN2ls15GlobalAllocator3Get" },
    },
    {
        .Name = "ls__GlobalAllocator__Alloc",
        .What = Kind::NoCallableCode,
        .Use = "GameHooks/DataLibraries.cpp: BG3Alloc, and the test for \"the game can show messages\" in ShowStartupError and CanShowMessages",
        .Gone = { .Fate = kAllocatorFate, .NoSymbol = "__ZN2ls15GlobalAllocator5Alloc", .Hosts = kAllocatorHosts },
    },
    {
        .Name = "ls__GlobalAllocator__Free",
        .What = Kind::NoCallableCode,
        .Use = "GameHooks/DataLibraries.cpp: BG3Free",
        .Gone = { .Fate = kAllocatorFate, .NoSymbol = "__ZN2ls15GlobalAllocator4Free" },
    },
    {
        .Name = "ls__FixedString__GetString",
        .What = Kind::FunctionToCall,
        .Mac = "ls::gst::Get(unsigned int)",
        .Use = "CoreLib/Base/BaseString.inl: the characters and length of a fixed string",
        .Where = Exact("__ZN2ls3gst3GetEj"),
        .Call = Takes({ Value(Place::X0, 4, 0, "unsigned int: the fixed string's index") },
            Returns::X0W1, "char const* in x0, unsigned int length in w1",
            "Norbyte passes (FixedString const* self, LSStringView& out); here the index goes in by value and the view comes back in two registers"),
        .Shape = { 44, 0xbefcfe73e0706563ull },     // 4.1.1.7631656
    },
    {
        .Name = "ls__FixedString__IncRef",
        .What = Kind::FunctionToCall,
        .Mac = "ls::gst::Acquire(unsigned int)",
        .Use = "CoreLib/Base/BaseString.inl: one more reference to a fixed string",
        .Where = Exact("__ZN2ls3gst7AcquireEj"),
        .Call = Takes({ Value(Place::X0, 4, 0, "unsigned int: the fixed string's index") }),
        .Shape = { 57, 0xb8cc640495cdc4a2ull },     // 4.1.1.7631656
    },
    {
        .Name = "ls__GlobalStringTable__MainTable__CreateFromString",
        .What = Kind::FunctionToCall,
        .Mac = "ls::gst::Add(ls::_StringView<char> const&)",
        .Use = "CoreLib/Base/BaseString.inl: the fixed string for a text, created when new",
        .Where = Exact("__ZN2ls3gst3AddERKNS_11_StringViewIcEE"),
        .Call = Takes({ Pointer(Place::X0, 2, "ls::_StringView<char> const: { char const*, unsigned int length }") },
            Returns::W0, "unsigned int: the string's index",
            "Norbyte passes (MainTable* self, uint32_t* out, LSStringView* text) and gets a pointer back; here the view is the "
            "one argument and the index is the result. The game's own callers do not call it for an empty text"),
        .Shape = { 665, 0x066403c20c401ab7ull },     // 4.1.1.7631656
    },
    {
        .Name = "ls__GlobalStringTable__MainTable__DecRef",
        .What = Kind::FunctionToCall,
        .Mac = "ls::gst::Map::Release(ls::gst::NodePoolData const&)",
        .Use = "CoreLib/Base/BaseString.inl: one reference fewer to a fixed string",
        .Where = Exact(kRelease),
        .Call = Takes({
            Pointer(Place::X0, 0, "ls::gst::Map: the string table's Main member"),
            Pointer(Place::X1, 1, "ls::gst::NodePoolData const: the fixed string's index, 32 bits"),
        }),
        .Shape = { 191, 0x6744be094df769b1ull },     // 4.1.1.7631656
        .CallsItself = true,
        .Operands = kReleaseOperands,
        .Needs = "ls__gGlobalStringTable",
    },
    {
        .Name = "ls__gGlobalStringTable",
        .What = Kind::GlobalPointer,
        .Mac = "ls::gst::s_Instance",
        .Use = "CoreLib/Base/BaseString.inl: the global string table, walked by sub-table and bucket",
        .Where = Exact(kStringTable),
        .Operands = kStringTableOperands,
        .Needs = kCoreLibInitName,
    },
    {
        .Name = "ls__ThreadRegistry__RequestThreadIndex",
        .What = Kind::FunctionToCall,
        .Mac = "ls::ThreadRegistry::RequestThreadIndex()",
        .Use = "GameDefinitions/Base/Base.cpp: the calling thread's index into the engine's per-thread arrays",
        .Where = Exact("__ZN2ls14ThreadRegistry18RequestThreadIndexEv"),
        .Call = Takes({}, Returns::W0, "int: 0 to 62, kept for the life of the thread; -1 when all are taken"),
        .Shape = { 92, 0xdeba64d7dda0ec02ull },     // 4.1.1.7631656
    },
    {
        .Name = "ls__FileReader__ctor",
        .What = Kind::FunctionToCallAndHook,
        .Mac = "ls::FileReader::FileReader(ls::Path const&, ls::FileReader::EType, ls::EnumFlags<ls::EFileFlags>)",
        .Use = "GameHooks/EngineHooks.inl (OnFileReaderCreate) and GameDefinitions/GameHelpers.cpp (MakeFileReader)",
        .Where = Exact("__ZN2ls10FileReaderC1ERKNS_4PathENS0_5ETypeENS_9EnumFlagsINS_10EFileFlagsEEE"),
        .Call = Takes({
            Pointer(Place::X0, 0, "ls::FileReader: the reader to construct"),
            Pointer(Place::X1, 1, "ls::Path const"),
            Value(Place::X2, 4, 2, "ls::FileReader::EType"),
            Value(Place::X3, 4, 3, "ls::EnumFlags<ls::EFileFlags>"),
        }, Returns::X0, "ls::FileReader*: the reader"),
        .Shape = { 53, 0x35f86651259c2072ull },     // 4.1.1.7631656
    },
    {
        .Name = "ls__FileReader__dtor",
        .What = Kind::FunctionToCall,
        .Mac = "ls::FileReader::~FileReader()",
        .Use = "GameDefinitions/GameHelpers.cpp: DestroyFileReader",
        .Where = Exact("__ZN2ls10FileReaderD1Ev"),
        .Call = Takes({ Pointer(Place::X0, 0, "ls::FileReader") }, Returns::X0, "ls::FileReader*: the reader, as every destructor of this ABI returns it"),
        .Shape = { 133, 0x4a4ae41426ab464eull },     // 4.1.1.7631656
    },
    {
        .Name = "ls__FileSystem__CollectFilesWithGlob",
        .What = Kind::FunctionToCall,
        .Mac = "ls::FileSystem::CollectFilesWithGlob(ls::Path const&, ls::_StringView<char> const&, bool, "
               "ls::DynamicArray<ls::Path, ls::TaggedAllocator<int>>&, bool)",
        .Use = "Extender/Shared/ScriptHelpers.cpp: the files under a directory that match a pattern",
        .Where = Exact("__ZN2ls10FileSystem20CollectFilesWithGlobERKNS_4PathERKNS_11_StringViewIcEEbRNS_12DynamicArrayIS1_NS_15TaggedAllocatorIiEEEEb"),
        .Call = Takes({
            Pointer(Place::X0, 0, "ls::Path const: the directory; the function does nothing unless it starts with '/'"),
            Pointer(Place::X1, 1, "ls::_StringView<char> const: { char const*, unsigned int length }"),
            Value(Place::X2, 1, 2, "bool: recursive"),
            Pointer(Place::X3, 3, "ls::DynamicArray<ls::Path, ls::TaggedAllocator<int>>: the result"),
            Value(Place::X4, 1, 4, "bool: also packed files"),
        }),
        .Shape = { 539, 0x0c0c8df0371815e6ull },     // 4.1.1.7631656
        .Operands = kGlobOperands,
        .Caveat = "for the files on disk the function passes the pattern's characters on as a C string (to do_CollectFiles), "
                  "without its length: the text must end with a zero at its length, which a view into a longer text does not",
    },
    { .Name = "Kernel_FindFirstFileW", .What = Kind::NoCallableCode, .Use = kFindFileUse,
      .Gone = { .Fate = kFindFileFate, .NoSymbol = "_FindFirstFileW" } },
    { .Name = "Kernel_FindNextFileW", .What = Kind::NoCallableCode, .Use = kFindFileUse,
      .Gone = { .Fate = kFindFileFate, .NoSymbol = "_FindNextFileW" } },
    { .Name = "Kernel_FindClose", .What = Kind::NoCallableCode, .Use = kFindFileUse,
      .Gone = { .Fate = kFindFileFate, .NoSymbol = "_FindClose" } },
    {
        .Name = "ls__PathRoots",
        .What = Kind::GlobalObject,
        .Mac = "ls::Path::s_Roots",
        .Use = "GameDefinitions/GameHelpers.cpp and Extender/ScriptExtender.cpp: the path roots, indexed (root 4 is the user's game folder)",
        .Where = Exact("__ZN2ls4Path7s_RootsE"),
        .Bytes = 19 * 8,
    },
};

}

std::span<Entry const> CoreLibraryEntries()
{
    return kEntries;
}

}
