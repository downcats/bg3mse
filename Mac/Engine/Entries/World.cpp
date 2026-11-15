#include <Engine/Entries/Areas.h>
#include <Engine/TypeIndices.h>
#include <Layout/GameMembers.h>

#include <cstddef>

// The world: levels, templates, surfaces, pathfinding, resources, materials,
// the net message Norbyte hooks, and the type indices of the entity system.

namespace bg3se::mac::engine {

namespace {

constexpr char kNotifySkeletonUpdated[] = "__ZN2ls6Visual21NotifySkeletonUpdatedEb";
constexpr char kNotifySkeletonUpdatedName[] = "ls::Visual::NotifySkeletonUpdated";
constexpr char kInstantiateTexture[] = "__ZN2ls15ResourceManager18InstantiateTextureERKNS_11FixedStringEbbb";
constexpr char kOverrideTexture[] = "__ZN2ls15AppliedMaterial29TryOverrideTexture2DParameterERKNS_11FixedStringES3_b";
constexpr char kLoadShaders[] = "__ZN2ls15AppliedMaterial11LoadShadersERKNS_23AppliedMaterialLoadDescERNS_12ShaderResultE";
constexpr char kApplyLoadedShaders[] = "__ZN2ls15AppliedMaterial18ApplyLoadedShadersERKNS_12ShaderResultE";
constexpr char kConnectVisit[] = "__ZN6eocnet20ClientConnectMessage5VisitEPN3net14MessageVisitorE";

// Which template bank is the current one. ls::GlobalTemplateManager::GetTemplateRaw
// begins by working that out: a byte global is the bank's index, unless it is
// 2, and then the index is the calling thread's byte, reached through its
// thread-local descriptor (called with itself in x0, it returns the byte's
// address); the bank is the pointer at +0x20 + 8 * index of the manager.
//     adrp x9, page ; ldrb w9, [x9, #offset] ; cmp w9, #2
//     adrp x0, page ; add x0, x0, #offset ; ldr x9, [x0] ; blr x9 ; ldrb w9, [x0]
//     add x8, x8, w9, sxtb #3 ; ldr x8, [x8, #0x20]
// (game build 4.1.1.7631656)
constexpr char kGetTemplateRaw[] = "__ZNK2ls21GlobalTemplateManager14GetTemplateRawERKNS_11FixedStringE";
constexpr CodeShape kGetTemplateRawShape{ 30, 0x1115d0ebbece26b6ull };
constexpr MaskedWord kBankTypeLoad[] = {
    { 0x90000009, Any::Adrp },
    { 0x39400129, Any::Imm12 },
    { 0x7100093f },
};
constexpr MaskedWord kThreadBankTypeLoad[] = {
    { 0x90000000, Any::Adrp },
    { 0x91000000, Any::Imm12 },
    { 0xf9400009 },
    { 0xd63f0120 },
    { 0x39400009 },
};
constexpr MaskedWord kBankOfIndex[] = {
    { 0x8b298d08 },                 // add x8, x8, w9, sxtb #3
    { 0xf9400108, Any::Imm12 },     // ldr x8, [x8, #offset]
};
constexpr Derivation kBankType{
    .Host = Exact(kGetTemplateRaw),
    .HostShape = kGetTemplateRawShape,
    .Pattern = kBankTypeLoad,
    .What = Derivation::Take::PageAddress,
    .At = 0,
};
constexpr Derivation kThreadBankType{
    .Host = Exact(kGetTemplateRaw),
    .HostShape = kGetTemplateRawShape,
    .Pattern = kThreadBankTypeLoad,
    .What = Derivation::Take::PageAddress,
    .At = 0,
};
constexpr Operand kBankTypeOperands[] = {
    { .What = "the banks' offset in the manager", .In = Exact(kGetTemplateRaw), .Pattern = kBankOfIndex, .At = 1,
      .How = Operand::Read::LoadStoreOffset, .Expected = kTemplateManagerBanks },
};

constexpr Named kNotifyHosts[] = { Exact(kNotifySkeletonUpdated) };
constexpr char const* kBlendShapeReplacements[] = { kNotifySkeletonUpdatedName };

// The three functions the texture loading of a material is inlined into.
constexpr Named kLoadTextureHosts[] = { Exact(kOverrideTexture), Exact(kLoadShaders), Exact(kApplyLoadedShaders) };
constexpr Operand kLoadTextureOperands[] = {
    { .What = "instantiates a texture", .In = Exact(kOverrideTexture), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kInstantiateTexture) },
    { .What = "instantiates a texture", .In = Exact(kLoadShaders), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kInstantiateTexture) },
    { .What = "instantiates a texture", .In = Exact(kApplyLoadedShaders), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kInstantiateTexture) },
};

// In ls::Visual::NotifySkeletonUpdated, the test Norbyte's Windows function
// starts with: ldrsh wN, [xM, #0x102] ; cmn wN, #1 (no blend shapes: skip).
constexpr MaskedWord kBlendShapeTest[] = {
    { 0x79c20400, 0xfffffc00 },     // ldrsh w?, [x?, #0x102]
    { 0x3100041f, 0xfffffc1f },     // cmn w?, #1
};
constexpr Operand kNotifyOperands[] = {
    { .What = "the blend shape test", .Pattern = kBlendShapeTest, .How = Operand::Read::LoadStoreOffset, .Expected = kVisualBlendShapeBase },
};

// ls::TextureAtlasManager::GetTextureEntry begins by reading the two maps, at
// the offsets of Norbyte's TextureAtlasMap:
//     ldr w8, [x0, #AtlasMap.ItemCount] ; cbz w8 ; ldr w8, [x0, #IconMap.ItemCount] ; cbz w8 ; ldr w8, [x1]
//     ldr w9, [x0, #IconMap.HashSize] ; udiv w10, w8, w9 ; msub w9, w10, w9, w8 ; ldr x10, [x0, #IconMap.HashTable]
constexpr char kGetTextureEntry[] = "__ZN2ls19TextureAtlasManager15GetTextureEntryERKNS_11FixedStringERNS_21TextureMapIconUVEntryEPPKNS_12TextureAtlasE";
constexpr MaskedWord kAtlasLookup[] = {
    { 0xb9400008, Any::Imm12 }, { 0x34000008, Any::CondBranch }, { 0xb9400008, Any::Imm12 }, { 0x34000008, Any::CondBranch }, { 0xb9400028 },
    { 0xb9400009, Any::Imm12 }, { 0x1ac9090a }, { 0x1b09a149 }, { 0xf940000a, Any::Imm12 },
};
constexpr Operand kAtlasOperands[] = {
    { .What = "AtlasMap.ItemCount", .In = Exact(kGetTextureEntry), .Pattern = kAtlasLookup, .At = 0, .Expected = kAtlasMapItemCount },
    { .What = "IconMap.ItemCount", .In = Exact(kGetTextureEntry), .Pattern = kAtlasLookup, .At = 2, .Expected = kIconMapItemCount },
    { .What = "IconMap.HashSize", .In = Exact(kGetTextureEntry), .Pattern = kAtlasLookup, .At = 5, .Expected = kIconMapHashSize },
    { .What = "IconMap.HashTable", .In = Exact(kGetTextureEntry), .Pattern = kAtlasLookup, .At = 8, .Expected = kIconMapHashTable },
};

// ls::ResourceManager::RemoveTexture takes the texture manager from the
// resource manager and releases the texture through it:
//     cbz x1 ; ldr x19, [x0, #TextureManager] ; cbz x19 ; ... ; bl ls::TextureManager::PutTexture
constexpr char kRemoveTexture[] = "__ZN2ls15ResourceManager13RemoveTextureEPN2rf7TextureE";
constexpr char kPutTexture[] = "__ZN2ls14TextureManager10PutTextureERKNS_11FixedStringE";
constexpr MaskedWord kTextureManagerLoad[] = { { 0xb4000001, Any::CondBranch }, { 0xf9400013, Any::Imm12 }, { 0xb4000013, Any::CondBranch } };
constexpr Operand kPutTextureOperands[] = {
    { .What = "the manager's place", .In = Exact(kRemoveTexture), .Pattern = kTextureManagerLoad, .At = 1, .Expected = kResourceManagerTextureManager },
    { .What = "released through it", .In = Exact(kRemoveTexture), .How = Operand::Read::CallCount, .Expected = 1, .Target = Exact(kPutTexture) },
};

// ecl::StatusVFX and the seven classes Norbyte tells apart behind a pointer to
// it (GameDefinitions/Status.inl, MakePolymorphicRef). His Windows code has no
// names: it reads the first bytes of three of the object's virtual functions.
// What it reads there is what each class's vtable holds here by name:
//
//   DestroyVisuals starts by testing the byte at +0x20     StatusVFXBoostMaterial
//   Update hands the handler at +0x18 to the effect        StatusVFXEffect, and StatusVFXApplyEffect when
//   handler's update                                       DoStatusApplyEffect is more than a return
//   DoStatusApplyEffect is more than a return              StatusVisual
//   Update hands the handler at +0x28 on                   ManagedStatusVFX
//   Update is more than a return                           StatusVFXBeam (ecl::StatusFXBeam here)
//   none of these                                          StatusVFXAura, StatusVFXMaterial: left as the base class
//
// A vtable here has seven slots, two destructors first, where the Windows one
// has five: the object's vtable pointer is compared and no slot is read
// through Norbyte's StatusVFXVMT. Each entry states the function of every
// slot, the base class's where the class does not override it, so that a
// class that overrides other functions than it did is refused; the offsets
// his structures give the members named above are read from the functions
// that use them. The game has no type information for these classes: that
// no other class derives from ecl::StatusVFX was read from the binary (the
// base class's three functions are in no vtable but these; build
// 4.1.1.7631656) and is not checked here.
#define STATUS_VFX(cls, method) "__ZN3ecl" cls method
#define STATUS_VFX_SLOTS(name, cls, update, apply, gather) \
    constexpr VtableMethod name[] = { \
        { "Destroy", Exact(STATUS_VFX(cls, "D1Ev")), 0x00 }, \
        { "the deleting destructor", Exact(STATUS_VFX(cls, "D0Ev")), 0x08 }, \
        { "UpdateVisuals", Prefix(STATUS_VFX(update, "6UpdateE")), 0x10 }, \
        { "RecreateVisuals", Prefix(STATUS_VFX(cls, "15RecreateVisualsE")), 0x18 }, \
        { "DestroyVisuals", Prefix(STATUS_VFX(cls, "14DestroyVisualsE")), 0x20 }, \
        { "Apply", Prefix(STATUS_VFX(apply, "19DoStatusApplyEffectE")), 0x28 }, \
        { "GatherEffectInfo", Prefix("__ZNK3ecl" gather "16GatherEffectInfoE"), 0x30 }, \
    }
#define STATUS_VFX_BASE "9StatusVFX"
STATUS_VFX_SLOTS(kStatusVfxApplyEffectSlots, "20StatusVFXApplyEffect", "20StatusVFXApplyEffect", "20StatusVFXApplyEffect", "20StatusVFXApplyEffect");
STATUS_VFX_SLOTS(kStatusVfxAuraSlots, "13StatusVFXAura", STATUS_VFX_BASE, STATUS_VFX_BASE, STATUS_VFX_BASE);
STATUS_VFX_SLOTS(kStatusVfxBeamSlots, "12StatusFXBeam", "12StatusFXBeam", STATUS_VFX_BASE, "12StatusFXBeam");
STATUS_VFX_SLOTS(kStatusVfxBoostMaterialSlots, "22StatusVFXBoostMaterial", STATUS_VFX_BASE, STATUS_VFX_BASE, STATUS_VFX_BASE);
STATUS_VFX_SLOTS(kStatusVfxEffectSlots, "15StatusVFXEffect", "15StatusVFXEffect", STATUS_VFX_BASE, "15StatusVFXEffect");
STATUS_VFX_SLOTS(kManagedStatusVfxSlots, "16ManagedStatusVFX", "16ManagedStatusVFX", STATUS_VFX_BASE, "16ManagedStatusVFX");
STATUS_VFX_SLOTS(kStatusVfxMaterialSlots, "17StatusVFXMaterial", STATUS_VFX_BASE, STATUS_VFX_BASE, STATUS_VFX_BASE);
STATUS_VFX_SLOTS(kStatusVisualSlots, "12StatusVisual", "12StatusVisual", "12StatusVisual", STATUS_VFX_BASE);

// The whole of an Update that hands the effect handler on:
//     ldrb w8, [x0, #flag] ; cbz w8 ; add x0, x0, #EffectHandler ; b ecl::EffectHandler::UpdateEffects()
constexpr char kEffectHandlerUpdate[] = "__ZN3ecl13EffectHandler13UpdateEffectsEv";
constexpr MaskedWord kHandlerUpdated[] = {
    { 0x39400008, Any::Imm12 }, { 0x34000008, Any::CondBranch }, { 0x91000000, Any::Imm12 }, { 0x14000000, Any::Branch },
};
#define STATUS_VFX_UPDATE_OPERANDS(name, cls, handler) \
    constexpr Operand name[] = { \
        { .What = "the effect handler Update hands on", .In = Prefix(STATUS_VFX(cls, "6UpdateE")), .Pattern = kHandlerUpdated, .At = 2, \
          .How = Operand::Read::AddImmediate, .Expected = handler }, \
        { .What = "Update hands it to the effect handler's update", .In = Prefix(STATUS_VFX(cls, "6UpdateE")), .Pattern = kHandlerUpdated, \
          .At = 3, .How = Operand::Read::BranchTarget, .Target = Exact(kEffectHandlerUpdate) }, \
    }
STATUS_VFX_UPDATE_OPERANDS(kStatusVfxEffectOperands, "15StatusVFXEffect", kStatusVfxEffectHandler);
STATUS_VFX_UPDATE_OPERANDS(kStatusVfxApplyEffectOperands, "20StatusVFXApplyEffect", kStatusVfxEffectHandler);
STATUS_VFX_UPDATE_OPERANDS(kManagedStatusVfxOperands, "16ManagedStatusVFX", kManagedStatusVfxEffectHandler);
// ecl::StatusVFXBoostMaterial::DestroyVisuals, after its prologue:
//     add x29, sp, #0x20 ; ldrb w8, [x0, #Created] ; cbz w8
constexpr MaskedWord kBoostMaterialCreated[] = { { 0x910083fd }, { 0x39400008, Any::Imm12 }, { 0x34000008, Any::CondBranch } };
constexpr Operand kStatusVfxBoostMaterialOperands[] = {
    { .What = "Created, which DestroyVisuals tests first", .In = Prefix(STATUS_VFX("22StatusVFXBoostMaterial", "14DestroyVisualsE")),
      .Pattern = kBoostMaterialCreated, .At = 1, .Expected = kStatusVfxBoostMaterialCreated },
};
// ecl::StatusFXBeam::DestroyVisuals destroys the handler and clears the two flags after it:
//     add x0, x19, #EffectHandler ; mov w1, #0 ; bl ecl::EffectHandler::Destroy(bool, bool) ; strh wzr, [x19, #Initialized]
constexpr MaskedWord kBeamDestroyed[] = { { 0x91000260, Any::Imm12 }, { 0x52800001 }, { 0x94000000, Any::Branch }, { 0x7900027f, Any::Imm12 } };
constexpr Operand kStatusVfxBeamOperands[] = {
    { .What = "the effect handler DestroyVisuals destroys", .In = Prefix(STATUS_VFX("12StatusFXBeam", "14DestroyVisualsE")),
      .Pattern = kBeamDestroyed, .At = 0, .How = Operand::Read::AddImmediate, .Expected = kStatusVfxEffectHandler },
    { .What = "DestroyVisuals destroys it", .In = Prefix(STATUS_VFX("12StatusFXBeam", "14DestroyVisualsE")), .Pattern = kBeamDestroyed,
      .At = 2, .How = Operand::Read::BranchTarget, .Target = Exact("__ZN3ecl13EffectHandler7DestroyEbb") },
    { .What = "Initialized and Created, cleared together", .In = Prefix(STATUS_VFX("12StatusFXBeam", "14DestroyVisualsE")),
      .Pattern = kBeamDestroyed, .At = 3, .Expected = kStatusVfxBeamFlags },
};
constexpr char kStatusVfxUse[] = "GameDefinitions/Status.inl (MakePolymorphicRef): the class of an object behind an ecl::StatusVFX pointer";
#define STATUS_VFX_ENTRY(name, mac, symbol, slots, ...) \
    { .Name = name, .What = Kind::Vtable, .Mac = "vtable for " mac, .Use = kStatusVfxUse, .Where = Exact(symbol), .Methods = slots, __VA_ARGS__ }

constexpr VtableMethod kConnectMessageSlots[] = {
    { "Serialize", Exact(kConnectVisit), 0x10 },
};

// On Windows the five engine callbacks of BinaryMappings.xml (BindECSContext,
// BindECSIndex, BindECSStaticStringConstructor, BindECSStaticRegistrant,
// BindComponentReplicationIDRef) each match one form of the code that
// registers a type with its context, and collect from it the index variable,
// the type's name and the context. Here all of that is in the names of the
// index variables, whatever form the registering code has; the entries below
// are the contexts Norbyte's EntitySystem uses, and Engine/TypeIndices.h
// looks up a type in any context.
constexpr char kTypeIndexMac[] =
    "the variables ls::TypeId<T, Context>::m_TypeIndex of this context: 32-bit integers, 0x7fffffff from the image's initialisers "
    "until CoreLibSDM::Init numbers the context's types (Engine/TypeIndices.h finds one by type name), and the context's state "
    "ls::TypeContext<Context>::m_State, whose first word is null until then";

// ls::InitializeTypeRegistry, inline in CoreLibSDM::Init, numbers a context's
// types in this loop (x28 walks the context's records, sorted by name; x24 is
// the context's list of names; w20 the index, -1 before the first):
//     ldr w25, [x24, #Count] ; add w8, w25, #1 ; str w8, [x24, #Count]     a name not seen yet: the list grows by it ...
//     ldr x8, [x24] ; ldr q0, [x27] ; str q0, [x8, w25, sxtw #4]           ... a 16-byte view, at the old count
//     ldr x9, [x26, #8] ; ldr w8, [x26, #0x10]
//     add w20, w20, #1                                                     ... and it takes the next index
//     ldr x10, [x26] ; str w20, [x10]                                      every record: the index, through its pointer
// and afterwards hands the list to every state registered under the context's name:
//     ldr x10, [x8], #8 ; ldr x10, [x10] ; str x24, [x10]
// (game build 4.1.1.7631656). The function's whole shape is the CoreLibSDM::Init
// entry's; these entries are refused with it.
constexpr char kCoreLibInit[] = "__ZN10CoreLibSDM4InitERKN2ls14ArgumentStringE";
constexpr char kCoreLibInitName[] = "CoreLibSDM::Init";
constexpr MaskedWord kTypeNameAppended[] = {
    { 0xb9400f19, Any::Imm12 }, { 0x11000728 }, { 0xb9000f08, Any::Imm12 }, { 0xf9400308, Any::Imm12 }, { 0x3dc00360 }, { 0x3cb9d900 },
};
constexpr MaskedWord kTypeIndexStored[] = { { 0x11000694 }, { 0xf940034a, Any::Imm12 }, { 0xb9000154, Any::Imm12 } };
constexpr MaskedWord kTypeNamesHandedOver[] = { { 0xf840850a }, { 0xf940014a, Any::Imm12 }, { 0xf9000158, Any::Imm12 } };
constexpr Operand kTypeRegistryOperands[] = {
    { .What = "the count of a context's names", .In = Exact(kCoreLibInit), .Pattern = kTypeNameAppended, .At = 0,
      .Expected = offsetof(GameTypeNames, Count) },
    { .What = "the count is what grows", .In = Exact(kCoreLibInit), .Pattern = kTypeNameAppended, .At = 2,
      .Expected = offsetof(GameTypeNames, Count) },
    { .What = "the names of a context", .In = Exact(kCoreLibInit), .Pattern = kTypeNameAppended, .At = 3,
      .Expected = offsetof(GameTypeNames, Names) },
    { .What = "a record's pointer to its index variable", .In = Exact(kCoreLibInit), .Pattern = kTypeIndexStored, .At = 1, .Expected = 0 },
    { .What = "the index is stored into the variable", .In = Exact(kCoreLibInit), .Pattern = kTypeIndexStored, .At = 2, .Expected = 0 },
    { .What = "the names go into the state", .In = Exact(kCoreLibInit), .Pattern = kTypeNamesHandedOver, .At = 2,
      .Expected = offsetof(GameTypeContextState, Numbered) },
};

constexpr Entry kEntries[] = {
    {
        .Name = "esv__LevelManager",
        .What = Kind::GlobalPointer,
        .Mac = "esv::LevelManager::m_ptr",
        .Use = "GameDefinitions/Symbols.h: the server's levels",
        .Where = Exact("__ZN3esv12LevelManager5m_ptrE"),
    },
    {
        .Name = "ecl__LevelManager",
        .What = Kind::GlobalPointer,
        .Mac = "ecl::LevelManager::m_ptr",
        .Use = "GameDefinitions/Symbols.h: the client's levels",
        .Where = Exact("__ZN3ecl12LevelManager5m_ptrE"),
    },
    {
        .Name = "ls__GlobalTemplateManager",
        .What = Kind::GlobalPointer,
        .Mac = "ls::GlobalTemplateManager::m_ptr",
        .Use = "GameDefinitions/Symbols.h: the root templates",
        .Where = Exact("__ZN2ls21GlobalTemplateManager5m_ptrE"),
    },
    {
        .Name = "g_GlobalTemplateBankType",
        .What = Kind::GlobalObject,
        .Mac = "(anonymous namespace)::g_GlobalTemplateBankType: one byte, the current template bank's index, or 2 for \"the calling thread's\"",
        .Use = "GameDefinitions/GameHelpers.cpp (GetGlobalTemplateBank): which of the manager's banks is the current one; on Windows "
               "the index is a byte of the executable's thread-local block",
        .Derived = &kBankType,
        .Bytes = 1,
        .Agrees = Exact("__ZN12_GLOBAL__N_124g_GlobalTemplateBankTypeE"),
        .Operands = kBankTypeOperands,
    },
    {
        .Name = "tls_CurrentBankType",
        .What = Kind::GlobalObject,
        .Mac = "the thread-local descriptor of (anonymous namespace)::tls_CurrentBankType: called through its first word with "
               "itself as the argument, it gives the address of the calling thread's byte",
        .Use = "GameDefinitions/GameHelpers.cpp (GetGlobalTemplateBank): the bank's index when g_GlobalTemplateBankType is 2",
        .Derived = &kThreadBankType,
        .Bytes = 24,
        .Agrees = Exact("__ZN12_GLOBAL__N_119tls_CurrentBankTypeE"),
        .Needs = "g_GlobalTemplateBankType",
        .Caveat = "a second thread-local of the game has the same name with a numeric suffix (another file's): the one "
                  "ls::GlobalTemplateManager::GetTemplateRaw reads is this one, which is why it is taken from that code",
    },
    {
        .Name = "esv__CacheTemplateManager",
        .What = Kind::GlobalPointer,
        .Mac = "esv::CacheTemplateManager::m_ptr",
        .Use = "GameDefinitions/RootTemplates.inl and GameHelpers.cpp: the server's cache templates",
        .Where = Exact("__ZN3esv20CacheTemplateManager5m_ptrE"),
    },
    {
        .Name = "esv__SavegameManager",
        .What = Kind::GlobalPointer,
        .Mac = "esv::SavegameManager::m_ptr",
        .Use = "registered and not read",
        .Where = Exact("__ZN3esv15SavegameManager5m_ptrE"),
    },
    {
        .Name = "esv__gSurfaceActionFactory",
        .What = Kind::GlobalPointer,
        .Mac = "esv::SurfaceActionFactory::m_ptr",
        .Use = "GameDefinitions/Surface.inl: the factory that creates surface actions",
        .Where = Exact("__ZN3esv20SurfaceActionFactory5m_ptrE"),
    },
    {
        .Name = "esv__SurfaceActionFactory__DoCreateAction",
        .What = Kind::FunctionToCall,
        .Mac = "esv::SurfaceActionFactory::CreateAction(eoc::ESurfaceActionType, story action id, eoc::ClassDescriptions const&, ls::ObjectHandle)",
        .Use = "GameDefinitions/Surface.inl: create a surface action of a type",
        .Where = Prefix("__ZN3esv20SurfaceActionFactory12CreateAction"),
        .Call = Takes({
            Pointer(Place::X0, 0, "esv::SurfaceActionFactory"),
            Value(Place::X1, 4, 1, "eoc::ESurfaceActionType: 1 to 10; anything else gives null"),
            Value(Place::X2, 4, -1, "ls::TypeWrap<unsigned int, eoc::TStoryActionIDClassname, true>: stored in the action at +0x10"),
            Pointer(Place::X3, -1, "eoc::ClassDescriptions const: stored in the action at +0x50"),
            Value(Place::X4, 8, 2, "ls::ObjectHandle; the game's callers pass the null handle"),
        }, Returns::X0, "esv::SurfaceAction*, or null",
            "five arguments where Norbyte declares three: the story action id and the class descriptions, which the Windows caller "
            "stores in the action after the call, are arguments here, and the handle is the fifth"),
        .Shape = { 2439, 0x718123c60b537251ull },     // 4.1.1.7631656
    },
    {
        .Name = "esv__TransformSurfaceAction__Init",
        .What = Kind::FunctionToCall,
        .Mac = "esv::TransformSurfaceAction::Init(eoc::ESurfaceTransformActionType, eoc::ESurfaceLayer, eoc::ESurfaceType)",
        .Use = "GameDefinitions/Surface.inl: set up a transform surface action",
        .Where = Exact("__ZN3esv22TransformSurfaceAction4InitEN3eoc27ESurfaceTransformActionTypeENS1_13ESurfaceLayerENS1_12ESurfaceTypeE"),
        .Call = Takes({
            Pointer(Place::X0, 0, "esv::TransformSurfaceAction"),
            Value(Place::X1, 1, 1, "eoc::ESurfaceTransformActionType"),
            Value(Place::X2, 1, 2, "eoc::ESurfaceLayer"),
            Value(Place::X3, 1, 3, "eoc::ESurfaceType"),
        }),
        .Shape = { 155, 0x656cef3351d3cd34ull },     // 4.1.1.7631656
    },
    {
        .Name = "eoc__AiGrid__FindPath",
        .What = Kind::FunctionToHook,
        .Mac = "eoc::AiGrid::FindPath(int)",
        .Use = "GameHooks/EngineHooks.inl (OnFindPath): a pre-hook before a path is searched",
        .Where = Exact("__ZN3eoc6AiGrid8FindPathEi"),
        .Call = Takes({ Pointer(Place::X0, 0, "eoc::AiGrid"), Value(Place::X1, 4, 1, "int: the path's id") }),
        .Shape = { 279, 0x5303ab558b42c48bull },     // 4.1.1.7631656
    },
    {
        .Name = "eoc__AiGrid__FindPathImmediate",
        .What = Kind::FunctionToCall,
        .Mac = "eoc::AiGrid::FindPathImmediate(int, bool)",
        .Use = "Lua/Libs/Level.inl: finish the search for a path at once",
        .Where = Exact("__ZN3eoc6AiGrid17FindPathImmediateEib"),
        .Call = Takes({ Pointer(Place::X0, 0, "eoc::AiGrid"), Value(Place::X1, 4, 1, "int: the path's id") }, Returns::Nothing, nullptr,
            "the bool of its name is not passed, and there is no result: Norbyte declares a bool result, which here would be "
            "whatever the function left in w0"),
        .Shape = { 33, 0x75684a93f3d7ad24ull },     // 4.1.1.7631656
        .Caveat = "the function looks the id up in the grid's paths and then reads the path without testing that it was found: "
                  "an id that is not there (or -1337) faults. The outcome is in the path object, not in a result",
    },
    {
        .Name = "dlg__DialogManager__PerformDialogLoad",
        .What = Kind::FunctionToCall,
        .Mac = "LSDialogs::DialogManager::PerformDialogLoad(ls::DialogResource const*, bool (*)(ls::ObjectVisitor&, "
               "LSDialogs::DialogNode&, void*), void*)",
        .Use = "Lua/Libs/StaticData.inl (LoadResource): load a dialogue into the manager's cache, or take another reference on "
               "the cached copy",
        .Where = Exact("__ZN9LSDialogs13DialogManager17PerformDialogLoadEPKN2ls14DialogResourceEPFbRNS1_13ObjectVisitorERNS_10DialogNodeEPvES9_"),
        // The visitor and its data are kept in the new dialogue and not
        // called here; the game's own callers pass null for both, as Norbyte
        // does (esv::OsirisLSDialogsFunctions::GetGossipSpeaker). A null
        // resource gives a null result. The function takes no lock.
        .Call = Takes({ Pointer(Place::X0, 0, "LSDialogs::DialogManager"), Pointer(Place::X1, 1, "ls::DialogResource"),
                        Pointer(Place::X2, 2, "bool (ls::ObjectVisitor&, LSDialogs::DialogNode&, void*): the node visitor, or null"),
                        Pointer(Place::X3, 3, "the visitor's data") },
            Returns::X0, "LSDialogs::Dialog*: the cached dialogue with one reference more, or null"),
        .Shape = { 650, 0xca0fc027314bdfa9ull },     // 4.1.1.7631656
    },
    {
        .Name = "eoc__gGuidResourceManager",
        .What = Kind::GlobalPointer,
        .Mac = "ls::ImmutableDataHeadmaster::m_ptr: the object starts with the hash map from a type's index to its manager",
        .Use = "GameDefinitions/EntitySystem.cpp: the managers of static data by type index",
        .Where = Exact("__ZN2ls23ImmutableDataHeadmaster5m_ptrE"),
    },
    {
        .Name = "ls__gGlobalResourceManager",
        .What = Kind::GlobalPointer,
        .Mac = "ls::ResourceManager::m_ptr",
        .Use = "GameDefinitions/Animation.inl, GameHelpers.cpp, Extender/Shared/VirtualTextures.inl: the resource manager",
        .Where = Exact("__ZN2ls15ResourceManager5m_ptrE"),
    },
    {
        .Name = "ls__gTextureAtlasMap",
        .What = Kind::GlobalPointer,
        .Mac = "ls::TextureAtlasManager::m_ptr: the class is ls::TextureAtlasManager, and its GetTextureEntry reads the atlas map "
               "and the icon map at the offsets of Norbyte's TextureAtlasMap",
        .Use = "Extender/Client/IMGUI/IMGUI.cpp and Lua/Libs/StaticData.inl: the texture atlases and their icons",
        .Where = Exact("__ZN2ls19TextureAtlasManager5m_ptrE"),
        .Operands = kAtlasOperands,
        .Caveat = "the atlas map's key is an ls::Path hashed by the game's ls::PathHasher: that Norbyte's map hashes a path the same "
                  "way is the layout check of the texture atlases by path (Layout/GameShapes.cpp), which keeps this member back",
    },
    {
        .Name = "ls__AppliedMaterial__LoadTexture",
        .What = Kind::NoCallableCode,
        .Use = "Extender/Client/IMGUI/IMGUI.cpp: load a texture by its GUID for the debug UI, called with a null material",
        .Operands = kLoadTextureOperands,
        .Gone = {
            .Fate = "ls::AppliedMaterial has no LoadTexture: the same code (look the texture resource up in the current resource bank, "
                    "register it with ls::ResourceManager::AddTexture, instantiate it with ls::ResourceManager::InstantiateTexture) is "
                    "inlined into three of its functions. AddTexture and InstantiateTexture are callable and take the texture manager "
                    "(the pointer at +0x80 of the resource manager), not the resource manager, in x0; InstantiateTexture returns an "
                    "rf::Texture*, which is not Norbyte's TextureDescriptor; ls::TextureManager::PutTexture releases it",
            .NoSymbol = "__ZN2ls15AppliedMaterial11LoadTextureE",
            .Hosts = kLoadTextureHosts,
        },
    },
    {
        .Name = "ls__TextureManager__UnloadTexture",
        .What = Kind::FunctionToCall,
        .Mac = "ls::TextureManager::PutTexture(ls::FixedString const&): drops one reference and destroys the texture at none",
        .Use = "Extender/Client/IMGUI/IMGUI.cpp: release a texture loaded for the debug UI",
        .Where = Exact(kPutTexture),
        .Call = Takes({
            Pointer(Place::X0, 0, "ls::TextureManager: the pointer at +0x80 of the resource manager"),
            Pointer(Place::X1, 1, "ls::FixedString const: the texture's GUID"),
        }, Returns::W0, "bool: true only when the texture was destroyed"),
        .Shape = { 201, 0x87798e4f63312ddeull },     // 4.1.1.7631656
        .Operands = kPutTextureOperands,
        .Needs = "ls__gGlobalResourceManager",
        .Caveat = "its first argument is the texture manager, which the game takes from +0x80 of the resource manager (checked "
                  "on ls::ResourceManager::RemoveTexture): take it from there",
    },
    {
        .Name = "ls__AppliedMaterial__LoadVirtualTexture",
        .What = Kind::FunctionToCall,
        .Mac = "ls::AppliedMaterial::LoadVirtualTexture(ls::FixedString const&)",
        .Use = "GameDefinitions/GameHelpers.cpp: load a virtual texture by its GUID",
        .Where = Exact("__ZN2ls15AppliedMaterial18LoadVirtualTextureERKNS_11FixedStringE"),
        .Call = Takes({ Pointer(Place::X0, 1, "ls::FixedString const: the virtual texture's GUID") },
            Returns::X0, "a pointer to a new 0x70-byte object, or null when there is no such resource",
            "the material is not an argument: the GUID is the first and only one"),
        .Shape = { 105, 0x5a135fe38ab19bd7ull },     // 4.1.1.7631656
    },
    {
        .Name = "ls__AppliedMaterial__TryOverrideTexture2DParameter",
        .What = Kind::FunctionToCall,
        .Mac = "ls::AppliedMaterial::TryOverrideTexture2DParameter(ls::FixedString const&, ls::FixedString const&, bool)",
        .Use = "GameDefinitions/GameHelpers.cpp: point a texture parameter of a material at another texture",
        .Where = Exact(kOverrideTexture),
        .Call = Takes({
            Pointer(Place::X0, 0, "ls::AppliedMaterial"),
            Value(Place::X1, 4, 1, "ls::FixedString, by value: the parameter name's index"),
            Pointer(Place::X2, 2, "ls::FixedString const: the texture's GUID"),
        }, Returns::W0, "bool",
            "the parameter's name is passed by value (its 32-bit index), not by reference; the bool of its name is not passed"),
        .Shape = { 116, 0x34e124f0177fba4full },     // 4.1.1.7631656
    },
    {
        .Name = "ls__Visual__UpdateBlendshapeWeightsFromSkeleton",
        .What = Kind::NoCallableCode,
        .Use = "GameHooks/EngineHooks.inl and Extender/Client/VisualHelpers.cpp: a post-hook that, after the game has written the "
               "blend shape weights from the skeleton, writes back the ones Lua has overridden",
        .Gone = {
            .Fate = "inlined into ls::Visual::NotifySkeletonUpdated(bool), where the test the Windows function starts with (the "
                    "16-bit field at +0x102 of the visual against -1) appears once",
            .NoSymbol = "__ZN2ls6Visual35UpdateBlendshapeWeightsFromSkeleton",
            .Hosts = kNotifyHosts,
            .Replacements = kBlendShapeReplacements,
        },
    },
    {
        .Name = kNotifySkeletonUpdatedName,
        .What = Kind::FunctionToHook,
        .Mac = "ls::Visual::NotifySkeletonUpdated(bool)",
        .Use = "takes the place of the blend shape post-hook: when it returns, the visual's weights (and those of its children, "
               "through its call of itself) are written",
        .Where = Exact(kNotifySkeletonUpdated),
        .Call = Takes({ Pointer(Place::X0, 0, "ls::Visual"), Value(Place::X1, 1, -1, "bool") }, Returns::Nothing, nullptr,
            "it stands in for a function declared void (Visual*): here there is a second argument, a bool, to pass on"),
        .Shape = { 1007, 0x286f9048ede28426ull },     // 4.1.1.7631656
        .CallsItself = true,
        .Operands = kNotifyOperands,
    },

    {
        .Name = "eocnet__ClientConnectMessage__Serialize",
        .What = Kind::FunctionToHook,
        .Mac = "eocnet::ClientConnectMessage::Visit(net::MessageVisitor*)",
        .Use = "Extender/Shared/Hooks.cpp: a wrapper around the client's connect message being written and read",
        .Where = Exact(kConnectVisit),
        .Call = Takes({ Pointer(Place::X0, 0, "eocnet::ClientConnectMessage"), Pointer(Place::X1, 1, "net::MessageVisitor") }),
        .Shape = { 239, 0x815c0f5a19b2b309ull },     // 4.1.1.7631656
        .Caveat = "Norbyte takes the function from the message's vtable through net::Message::VMT, whose Serialize is the second "
                  "slot; here the second slot is the deleting destructor and this function the third. Hook it by name",
    },
    {
        .Name = "eocnet::ClientConnectMessage",
        .What = Kind::Vtable,
        .Mac = "vtable for eocnet::ClientConnectMessage",
        .Use = "where the function above sits in the message's vtable",
        .Where = Exact("__ZTVN6eocnet20ClientConnectMessageE"),
        .Methods = kConnectMessageSlots,
    },

    STATUS_VFX_ENTRY("ecl::StatusVFXBoostMaterial", "ecl::StatusVFXBoostMaterial", "__ZTVN3ecl22StatusVFXBoostMaterialE", kStatusVfxBoostMaterialSlots,
        .Operands = kStatusVfxBoostMaterialOperands),
    STATUS_VFX_ENTRY("ecl::StatusVFXApplyEffect", "ecl::StatusVFXApplyEffect", "__ZTVN3ecl20StatusVFXApplyEffectE", kStatusVfxApplyEffectSlots,
        .Operands = kStatusVfxApplyEffectOperands),
    STATUS_VFX_ENTRY("ecl::StatusVFXEffect", "ecl::StatusVFXEffect", "__ZTVN3ecl15StatusVFXEffectE", kStatusVfxEffectSlots,
        .Operands = kStatusVfxEffectOperands),
    STATUS_VFX_ENTRY("ecl::StatusVisual", "ecl::StatusVisual", "__ZTVN3ecl12StatusVisualE", kStatusVisualSlots),
    STATUS_VFX_ENTRY("ecl::ManagedStatusVFX", "ecl::ManagedStatusVFX", "__ZTVN3ecl16ManagedStatusVFXE", kManagedStatusVfxSlots,
        .Operands = kManagedStatusVfxOperands),
    STATUS_VFX_ENTRY("ecl::StatusVFXBeam", "ecl::StatusFXBeam", "__ZTVN3ecl12StatusFXBeamE", kStatusVfxBeamSlots,
        .Operands = kStatusVfxBeamOperands),
    STATUS_VFX_ENTRY("ecl::StatusVFXAura", "ecl::StatusVFXAura", "__ZTVN3ecl13StatusVFXAuraE", kStatusVfxAuraSlots),
    STATUS_VFX_ENTRY("ecl::StatusVFXMaterial", "ecl::StatusVFXMaterial", "__ZTVN3ecl17StatusVFXMaterialE", kStatusVfxMaterialSlots),

    {
        .Name = "TypeIdContext::Component",
        .What = Kind::TypeIndex,
        .Mac = kTypeIndexMac,
        .Use = "GameDefinitions/EntitySystem.cpp (TypeIdContext::Component, from what the engine callbacks collect): the index of "
               "each component type, in its low 15 bits",
        .Where = Exact("__ZN2ls11TypeContextIN3ecs22ComponentTypeIdContextEE7m_StateE"),
        .Context = "ecs::ComponentTypeIdContext",
        .Operands = kTypeRegistryOperands,
        .Needs = kCoreLibInitName,
    },
    {
        .Name = "TypeIdContext::OneFrameComponent",
        .What = Kind::TypeIndex,
        .Mac = kTypeIndexMac,
        .Use = "GameDefinitions/EntitySystem.cpp (TypeIdContext::OneFrameComponent): the index of each one-frame component type; "
               "Norbyte sets bit 15 of it, as the game's own readers do",
        .Where = Exact("__ZN2ls11TypeContextIN3ecs30OneFrameComponentTypeIdContextEE7m_StateE"),
        .Context = "ecs::OneFrameComponentTypeIdContext",
        .Operands = kTypeRegistryOperands,
        .Needs = kCoreLibInitName,
    },
    {
        .Name = "TypeIdContext::System",
        .What = Kind::TypeIndex,
        .Mac = kTypeIndexMac,
        .Use = "GameDefinitions/EntitySystem.cpp (TypeIdContext::System): the index of each system type",
        .Where = Exact("__ZN2ls11TypeContextIN3ecs14SystemsContextEE7m_StateE"),
        .Context = "ecs::SystemsContext",
        .Operands = kTypeRegistryOperands,
        .Needs = kCoreLibInitName,
        .Caveat = "Norbyte's code looks for a context named ecs::EntityWorld::SystemsContext; here it is ecs::SystemsContext",
    },
    {
        .Name = "TypeIdContext::Replication",
        .What = Kind::TypeIndex,
        .Mac = kTypeIndexMac,
        .Use = "GameDefinitions/EntitySystem.cpp (TypeIdContext::Replication): the replication index of each replicated component type",
        .Where = Exact("__ZN2ls11TypeContextIN3ecs4sync21ReplicatedTypeContextEE7m_StateE"),
        .Context = "ecs::sync::ReplicatedTypeContext",
        .Operands = kTypeRegistryOperands,
        .Needs = kCoreLibInitName,
    },
    {
        .Name = "TypeIdContext::ImmutableData",
        .What = Kind::TypeIndex,
        .Mac = kTypeIndexMac,
        .Use = "GameDefinitions/EntitySystem.cpp (TypeIdContext::ImmutableData): the index of each static data manager, the key "
               "of the map in eoc__gGuidResourceManager",
        .Where = Exact("__ZN2ls11TypeContextINS_23ImmutableDataHeadmasterEE7m_StateE"),
        .Context = "ls::ImmutableDataHeadmaster",
        .Operands = kTypeRegistryOperands,
        .Needs = kCoreLibInitName,
    },
};

}

std::span<Entry const> WorldEntries()
{
    return kEntries;
}

}
