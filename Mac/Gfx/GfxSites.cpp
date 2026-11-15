#include <stdafx.h>
#include <Gfx/GfxSites.h>
#include <Gfx/GfxMetalCalls.h>
#include <Platform/CodeSearch.h>
#include <Platform/CodeWriter.h>
#include <Platform/EntryHook.h>

namespace bg3se::mac::gfx {

namespace {

// Every symbol the graphics features use. A function with a shape is one
// whose behaviour, offsets or calling convention a feature relies on; its
// whole code (symbol to next symbol) must have that shape, read from build
// 4.1.1.7631656 (arm64). Symbols without a shape are data, or functions only
// searched for a site or a materialised address.
enum Sym : size_t {
    kLuaThrow, kLuaProtect,
    kWorkerDoWork, kSchedulerDoWork, kCapFrame,
    kBindMaterial, kSetupEngine, kBeginFrame,
    kAoBlurBind, kAddCommand,
    kJitteredProjection, kUpdateConstantBuffers, kVelocityStartRecording, kViewSubmit, kPostStartRecording,
    kFsrExecute, kPostVtable, kPostGetType, kFsrVtable, kGraphicSettings, kHalton, kPostLoad, kBaseAddPack,
    kAppVtable, kAppInitStages, kBaseInitStages, kPackShadow, kPackFirst,
    kMaskVtable, kMaskLoad, kMaskUnload, kStageStartRecording, kMaskExecute, kGetTextureSRV, kSetTextureSafe, kSsaoExecute,
    kPathCtor, kPathDtor, kCreateShader, kDefaultShaderPath, kRendererData, kLoadEngineShader, kDeviceCreateShader,
    kCreatePipelineState, kAddPipelineState,
    kClusterExecute,
    kMapVertices, kMapIndices, kUnmapVertices, kUnmapIndices, kDrawBatch,
    kSymbolCount
};

struct Known
{
    char const* Name;
    char const* Symbol;
    CodeShape Shape;        // {0, 0}: not shape-checked
};

constexpr Known kSymbols[kSymbolCount] = {
    { "luaD_throw", "_luaD_throw", { 64, 0x399cbd4ba60b7ad6ull } },
    { "luaD_rawrunprotected", "_luaD_rawrunprotected", { 31, 0x90af147488c75459ull } },
    { "ls::WorkerThreadWorker::DoWork", "__ZN2ls18WorkerThreadWorker6DoWorkEv", { 214, 0x420c0d230648e3d0ull } },
    { "ls::WorkerThreadScheduler::DoWork", "__ZN2ls21WorkerThreadScheduler6DoWorkEv", { 387, 0xaf902b55b9fabb63ull } },
    { "ls::GameClock::CapFrame", "__ZN2ls9GameClock8CapFrameEy", { 292, 0x8d152ea0f6e8d02cull } },
    { "ls::AppliedMaterial::BindMaterialParameters",
      "__ZN2ls15AppliedMaterial22BindMaterialParametersERN2rf21RendererCommandBufferERKNS_21MaterialRenderingDataENS1_11ERenderPassEPKS0_",
      { 982, 0xa3e57460de768748ull } },
    { "ls::RenderableObject::SetupEngineParameters",
      "__ZNK2ls16RenderableObject21SetupEngineParametersERKNS_8OptionalINS_14EngineCBVTDataEEERKNS_18MaterialShaderDescERKNS_21MaterialRenderingDataERKNS_13RenderContextENS1_IRK10Matrix4x4fEEbRKNS_11FixedStringEfb",
      { 244, 0x4bf9f77795c02123ull } },
    { "rf::metal::MetalRenderer::BeginFrame", "__ZN2rf5metal13MetalRenderer10BeginFrameEv", { 72, 0xaaad9596c636ad81ull } },
    { "rf::RendererCommandBuffer::CompareSetAndBindConstantBuffer<PPSAO_Blur::blur>",
      "__ZN2rf21RendererCommandBuffer31CompareSetAndBindConstantBufferIN2ls2sb10PPSAO_Blur4blurEEEvRNS_9DynamicCBIT_EES7_",
      { 111, 0x3c11d509d2d53620ull } },
    { "ls::CommandDispatcherBuffer::AddCommand", "__ZN2ls23CommandDispatcherBuffer10AddCommandEmm", { 95, 0x75ef9efd79a2c564ull } },

    { "ls::GameRenderView::GetJitteredProjectionMatrix", "__ZNK2ls14GameRenderView27GetJitteredProjectionMatrixEv", { 72, 0xa3f09581550e3812ull } },
    { "ls::GameRenderView::UpdateConstantBuffersData",
      "__ZNK2ls14GameRenderView25UpdateConstantBuffersDataERN2rf2sb7PerViewERNS2_9PerCameraEb", { 717, 0x8bb0e2db674e0187ull } },
    { "ls::VelocityRenderStage::StartRecording", "__ZN2ls19VelocityRenderStage14StartRecordingEv", { 214, 0xebdc1479ced256ccull } },
    { "ls::GameRenderView::Submit", "__ZN2ls14GameRenderView6SubmitEv", { 21, 0x01ad0f2f8a58f2daull } },
    { "ls::PostProcessStage::StartRecording", "__ZN2ls16PostProcessStage14StartRecordingEv", { 53, 0x18a03d300e710559ull } },
    { "ls::PostProcessStep_FSR::ExecuteWTKernel", "__ZN2ls19PostProcessStep_FSR15ExecuteWTKernelEv", { 1265, 0xddb1e02618b3897eull } },
    { "vtable for ls::PostProcessStage", "__ZTVN2ls16PostProcessStageE", { 0, 0 } },
    { "ls::PostProcessStage::GetType", "__ZNK2ls16PostProcessStage7GetTypeEv", { 2, 0x53e3acc05d09d9ffull } },
    { "vtable for ls::PostProcessStep_FSR", "__ZTVN2ls19PostProcessStep_FSRE", { 0, 0 } },
    { "GraphicSettings", "_GraphicSettings", { 0, 0 } },
    { "(anonymous)::s_Halton_2_3", "__ZN12_GLOBAL__N_112s_Halton_2_3E", { 0, 0 } },
    { "ls::PostProcessStage::Load", "__ZN2ls16PostProcessStage4LoadEv", { 2352, 0x9cac2d4045924a38ull } },
    { "BaseApp::InitFrameStages(view, Span)",
      "__ZN7BaseApp15InitFrameStagesEPN2rf10RenderViewERKN2ls4SpanIKNS0_12AppStageTypeEEE", { 7627, 0x0d16e42a0cdffea6ull } },

    { "vtable for App", "__ZTV3App", { 0, 0 } },
    { "App::InitFrameStages", "__ZN3App15InitFrameStagesEPN2rf10RenderViewE", { 953, 0x2d12cb0620614481ull } },
    { "BaseApp::InitFrameStages(view)", "__ZN7BaseApp15InitFrameStagesEPN2rf10RenderViewE", { 210, 0x75765ddc5a44dedcull } },
    { "BaseApp::InitFrameStages::SHADOW_BUFFER_PACK", "__ZZN7BaseApp15InitFrameStagesEPN2rf10RenderViewEE18SHADOW_BUFFER_PACK", { 0, 0 } },
    { "BaseApp::InitFrameStages::FIRST_PACK", "__ZZN7BaseApp15InitFrameStagesEPN2rf10RenderViewEE10FIRST_PACK", { 0, 0 } },

    { "vtable for ls::CascadedShadowMaskStage", "__ZTVN2ls23CascadedShadowMaskStageE", { 0, 0 } },
    { "ls::CascadedShadowMaskStage::Load", "__ZN2ls23CascadedShadowMaskStage4LoadEv", { 2738, 0x81add2bf295820c0ull } },
    { "ls::CascadedShadowMaskStage::Unload", "__ZN2ls23CascadedShadowMaskStage6UnloadEv", { 300, 0x8a718e423f61e161ull } },
    { "ls::RenderStage::StartRecording", "__ZN2ls11RenderStage14StartRecordingEv", { 43, 0xb263278b24929e0full } },
    { "ls::CascadedShadowMaskStage::ExecuteWTKernel", "__ZN2ls23CascadedShadowMaskStage15ExecuteWTKernelEv", { 1293, 0x7e5451c58b4d1063ull } },
    { "rf::SceneGraph::GetTextureSRV",
      "__ZNK2rf10SceneGraph13GetTextureSRVEPKNS_9IAppStageEN2ls8TypeWrapIjPNS_7TextureELb1EEEsiNS_17ETextureNamedViewE", { 92, 0xc00b48a622e0208cull } },
    { "rf::RendererCommandBuffer::SetTextureSafe",
      "__ZN2rf21RendererCommandBuffer14SetTextureSafeENS_2sb7BindingENS_18TextureBindingInfoE", { 74, 0xf729fad1d9f07813ull } },
    { "ls::SSAOStage::ExecuteWTKernel", "__ZN2ls9SSAOStage15ExecuteWTKernelEv", { 1810, 0x4afd73c711505831ull } },

    { "ls::Path::Path<STDString, StringView>",
      "__ZN2ls4PathC2IJNS_9STDStringENS_11_StringViewIcEEEEENS_9EPathRootEDpRKT_", { 200, 0xf94f2aec79724d15ull } },
    { "ls::Path::~Path", "__ZN2ls4PathD1Ev", { 36, 0x0492b774a70ff077ull } },
    { "rf::DataLoader::CreateShader", "__ZN2rf10DataLoader12CreateShaderERKN2ls4PathE", { 92, 0xd622d8bf7fe963afull } },
    { "rf::details::RendererBase::s_DefaultShaderPath", "__ZN2rf7details12RendererBase19s_DefaultShaderPathE", { 0, 0 } },
    { "(anonymous)::s_Data", "__ZN12_GLOBAL__N_16s_DataE", { 0, 0 } },
    { "ls::RenderStage::LoadEngineShader", "__ZN2ls11RenderStage16LoadEngineShaderER8ShaderIDRKNS_11_StringViewIcEE", { 58, 0x01dc2420908cbd05ull } },
    { "rf::GPUDevice::CreateShader", "__ZN2rf9GPUDevice12CreateShaderEONS_4bshd6ShaderE", { 623, 0x04a65fbd0445ddf3ull } },
    { "rf::metal::GPUDevice::CreatePipelineState",
      "__ZN2rf5metal9GPUDevice19CreatePipelineStateENS_24TPipelineStateDescriptorIPNS_10RenderPassELS4_0EEE", { 543, 0x4be2b31e5a6e9fbfull } },
    { "rf::IAppStage::AddPipelineState",
      "__ZN2rf9IAppStage16AddPipelineStateERKNS_24TPipelineStateDescriptorIPNS_17ManagedRenderPassELS3_0EEE", { 522, 0xa44da4a11fcc94a2ull } },

    { "ls::SubStage_DrawClusteredLighting::ExecuteWTKernel", "__ZN2ls30SubStage_DrawClusteredLighting15ExecuteWTKernelEv", { 3394, 0x6a3488f1424b76a0ull } },

    { "ns::Renderer::MapVertices", "__ZN2ns8Renderer11MapVerticesEj", { 34, 0x6974a0309e5b938bull } },
    { "ns::Renderer::MapIndices", "__ZN2ns8Renderer10MapIndicesEj", { 34, 0x236e64607e9b853cull } },
    { "ns::Renderer::UnmapVertices", "__ZN2ns8Renderer13UnmapVerticesEv", { 15, 0xc202ac606e463999ull } },
    { "ns::Renderer::UnmapIndices", "__ZN2ns8Renderer12UnmapIndicesEv", { 15, 0xae52edfec3b770c3ull } },
    { "ns::Renderer::DrawBatch", "__ZN2ns8Renderer9DrawBatchERKN6Noesis5BatchE", { 1134, 0x6ff61aec062eefe1ull } },
};

// The Itanium C++ ABI: a vtable's address point, what an object's vptr holds,
// follows its offset-to-top and RTTI words.
constexpr uint64_t kVtableAddressPoint = 16;

using P = MaskedWord;
constexpr uint32_t kBl = 0x94000000, kB = 0x14000000;

class Resolver
{
public:
    explicit Resolver(GameImage const& image) : image_(image)
    {
        char const* names[kSymbolCount];
        for (size_t i = 0; i < kSymbolCount; i++) names[i] = kSymbols[i].Symbol;
        image.FindVAs(names, va_);
        image.NextSymbolVAs(va_, end_);
    }

    GameImage const& Image() const { return image_; }
    uint64_t VA(Sym s) const { return va_[s]; }
    uint64_t End(Sym s) const { return end_[s]; }

    // Records each function's shape in `check`. False, with the reason, when
    // one is missing or not the code the feature was checked against.
    bool Functions(SiteCheck& check, std::initializer_list<Sym> functions)
    {
        for (Sym s : functions) {
            FunctionCheck fn{ kSymbols[s].Name, kSymbols[s].Shape, {}, false };
            if (va_[s] && end_[s] > va_[s]) {
                fn.Present = true;
                fn.Found = ShapeOfRange(image_.CodeAt(va_[s]), (uint32_t)((end_[s] - va_[s]) / 4));
            }
            check.Functions.push_back(fn);
        }
        for (auto const& fn : check.Functions) {
            if (!fn.Present) return Fail(check, std::string(fn.Name) + " is not in the game's symbol table");
            if (fn.Found != fn.Expected) return Fail(check, std::string(fn.Name) + " changed, so what the feature relies on in it has to be checked again");
        }
        return true;
    }

    // A symbol that must be present.
    bool Symbols(SiteCheck& check, std::initializer_list<Sym> symbols)
    {
        for (Sym s : symbols) {
            if (!va_[s]) return Fail(check, std::string(kSymbols[s].Name) + " is not in the game's symbol table");
        }
        return true;
    }

    // The one place in function `s` where `pattern` starts (`count` of them for count > 1).
    bool Find(SiteCheck& check, Sym s, std::span<MaskedWord const> pattern, char const* what, std::vector<uint64_t>& at, size_t count = 1)
    {
        at = FindPattern(image_, va_[s], end_[s], pattern);
        if (at.size() == count) return true;
        char why[256];
        snprintf(why, sizeof(why), "%s: %zu matches in %s, expected %zu", what, at.size(), kSymbols[s].Name, count);
        return Fail(check, why);
    }

    bool Find(SiteCheck& check, Sym s, std::span<MaskedWord const> pattern, char const* what, uint64_t& at)
    {
        std::vector<uint64_t> found;
        if (!Find(check, s, pattern, what, found)) return false;
        at = found[0];
        return true;
    }

    // The one slot of `vtable` holding the address of `method`.
    bool Slot(SiteCheck& check, Sym vtable, Sym method, uint64_t& slot)
    {
        if (image_.PointersOnlyAtRunTime()) {
            return Fail(check, std::string(kSymbols[vtable].Name) + ": this file's pointers are chained fixups, so its slots can only be read in the running game");
        }
        auto slots = VtableSlotsOf(image_, va_[vtable], end_[vtable], va_[method]);
        if (slots.size() != 1) {
            return Fail(check, std::string(kSymbols[method].Name) + " is not in exactly one slot of the " + kSymbols[vtable].Name);
        }
        slot = slots[0];
        return true;
    }

    // Whether an adrp and the add or load after it in function `s` form `address`.
    bool Materialises(Sym s, uint64_t address) const
    {
        for (uint64_t at = va_[s]; at + 8 <= end_[s]; at += 4) {
            if (PageAddress(at, image_.Word(at), image_.Word(at + 4)) == address) return true;
        }
        return false;
    }

    uint32_t Word(uint64_t at) const { return image_.Word(at); }
    uint32_t Offset(uint64_t at) const { return LoadStoreOffset(image_.Word(at)).value_or(0); }

    static bool Fail(SiteCheck& check, std::string why)
    {
        if (check.Problem.empty()) check.Problem = std::move(why);
        return false;
    }

private:
    GameImage const& image_;
    uint64_t va_[kSymbolCount]{};
    uint64_t end_[kSymbolCount]{};
};

bool Require(SiteCheck& check, bool ok, char const* why)
{
    return ok || Resolver::Fail(check, why);
}

// --- Lua -------------------------------------------------------------------------------

// BG3's Lua was built without LUA_USE_POSIX, so each protected call uses setjmp
// and each error longjmp, through the image's stubs for them.
LuaJumpSites FindLuaJump(Resolver& r)
{
    LuaJumpSites s;
    if (!r.Functions(s, { kLuaThrow, kLuaProtect })) return s;
    // luaD_throw: add x0, x8, #8 (the jmp_buf in the error handler); mov w1, #1; bl longjmp
    constexpr P kThrow[] = { { 0x91002100 }, { 0x52800021 }, { kBl, Any::Branch } };
    // luaD_rawrunprotected: add x0, x8, #8; bl setjmp
    constexpr P kProtect[] = { { 0x91002100 }, { kBl, Any::Branch } };
    uint64_t t, p;
    if (!r.Find(s, kLuaThrow, kThrow, "the longjmp call", t) || !r.Find(s, kLuaProtect, kProtect, "the setjmp call", p)) return s;
    s.ThrowCall = t + 8;
    s.ProtectCall = p + 4;
    // The shape leaves call targets open, so both calls are checked to go to the
    // imports: a call that goes anywhere else is refused.
    if (!Require(s, CallsImport(r.Image(), s.ThrowCall, "_longjmp"), "luaD_throw's call is not to _longjmp") ||
        !Require(s, CallsImport(r.Image(), s.ProtectCall, "_setjmp"), "luaD_rawrunprotected's call is not to _setjmp")) {
        return s;
    }
    return s;
}

// --- Thread idling ------------------------------------------------------------------------

ThreadIdlingSites FindThreadIdling(Resolver& r)
{
    ThreadIdlingSites s;
    if (!r.Functions(s, { kWorkerDoWork, kSchedulerDoWork, kCapFrame })) return s;

    // WorkerThreadWorker::DoWork: ldr x9, [x19, #budget]; cmp x9, x8, lsr #4; b.gt (keep spinning)
    constexpr P kBudget[] = { { 0xf9401669, Any::Imm12 }, { 0xeb48113f }, { 0x5400000c, Any::CondBranch } };
    if (!r.Find(s, kWorkerDoWork, kBudget, "the worker's spin budget", s.WorkerBudget)) return s;
    s.WorkerBudgetWord = r.Word(s.WorkerBudget);
    s.WorkerBudgetOffset = r.Offset(s.WorkerBudget);

    // WorkerThreadScheduler::DoWork, twice: mov w9, #lo; movk w9, #hi, lsl #16; cmp x23, x9 (its yield window, ns)
    constexpr P kWindow[] = { { 0x529150e9, Any::Imm16 }, { 0x72a00029, Any::Imm16 }, { 0xeb0902ff } };
    std::vector<uint64_t> windows;
    if (!r.Find(s, kSchedulerDoWork, kWindow, "the scheduler's yield window", windows, 2)) return s;
    auto a = MovzMovkValue(r.Word(windows[0]), r.Word(windows[0] + 4));
    auto b = MovzMovkValue(r.Word(windows[1]), r.Word(windows[1] + 4));
    if (!Require(s, a && b && *a == *b, "the scheduler's two yield windows differ") ||
        !Require(s, OnOnePage(windows[0], 2, CodePageSize()) && OnOnePage(windows[1], 2, CodePageSize()),
            "a scheduler yield window crosses a page boundary")) {
        return s;
    }
    s.SchedulerWindow[0] = windows[0];
    s.SchedulerWindow[1] = windows[1];
    s.SchedulerWindowWord = r.Word(windows[0]);
    s.SchedulerWindowNs = *a;

    // GameClock::CapFrame: the wait target, then the clock's start:
    //   ldr x8, [x19, #..]; ldr x9, [x19, #..]; ldr x10, [sp, #..]; add x9, x9, x10; sub x8, x8, x9
    //   cmp x8, #0; csel x27, x8, xzr, gt;  bl mach_absolute_time
    //   ldrb w8, [x19, #started]; cbz w8; ldr x8, [x19, #start]; sub x22, x0, x8
    constexpr P kTarget[] = {
        { 0xf9400e68, Any::Imm12 }, { 0xf9401669, Any::Imm12 }, { 0xf94007ea, Any::Imm12 }, { 0x8b0a0129 }, { 0xcb090108 },
        { 0xf100011f }, { 0x9a9fc11b }, { kBl, Any::Branch },
        { 0x39404268, Any::Imm12 }, { 0x34000008, Any::CondBranch }, { 0xf9400668, Any::Imm12 }, { 0xcb080016 },
    };
    uint64_t t;
    if (!r.Find(s, kCapFrame, kTarget, "CapFrame's wait target", t)) return s;
    if (!Require(s, CallsImport(r.Image(), t + 28, "_mach_absolute_time"), "CapFrame's clock read is not mach_absolute_time")) return s;
    s.CapTargetSelect = t + 24;
    s.CapTargetReg = RegD(r.Word(s.CapTargetSelect));
    s.CapClockReg = RegN(r.Word(t + 32));
    s.ClockStarted = r.Offset(t + 32);
    s.ClockStartTicks = r.Offset(t + 40);

    // The idle step: mov x0, x20; bl mach_timebase_info; bl pthread_yield_np; b (steal again).
    // CapFrame also yields elsewhere (not followed by a b back); only this one is idle.
    constexpr P kYield[] = { { 0xaa1403e0 }, { kBl, Any::Branch }, { kBl, Any::Branch }, { kB, Any::Branch } };
    std::vector<uint64_t> candidates, yields;
    candidates = FindPattern(r.Image(), r.VA(kCapFrame), r.End(kCapFrame), kYield);
    for (uint64_t c : candidates) {
        if (CallsImport(r.Image(), c + 4, "_mach_timebase_info") && CallsImport(r.Image(), c + 8, "_pthread_yield_np")) yields.push_back(c + 8);
    }
    if (!Require(s, yields.size() == 1, "CapFrame's idle yield is not exactly one call")) return s;
    s.CapIdleYield = yields[0];
    return s;
}

// --- Reduced locking -----------------------------------------------------------------------

ReducedLockingSites FindReducedLocking(Resolver& r)
{
    ReducedLockingSites s;
    if (!r.Functions(s, { kBindMaterial, kSetupEngine, kBeginFrame })) return s;
    auto& image = r.Image();

    // BindMaterialParameters: the inlined lock-and-bump of the renderer's uniform offset.
    constexpr P kBind[kBindAllocLiveWords] = {
        { 0xf9401c39 },                 // ldr x25, [x1, #0x38]       the renderer
        { 0x91002334, Any::Imm12 },     // add x20, x25, #lock
        { 0xaa1403e0 },                 // mov x0, x20
        { kBl, Any::Branch },           // bl os_unfair_lock_lock
        { 0xf9504b28, Any::Imm12 },     // ldr x8, [x25, #bump]
        { 0x4b0803e9 }, { 0x92400d29 }, { 0x8b080138 },   // align up to 16 into x24
        { 0x8b150308 },                 // add x8, x24, x21           + the size
        { 0xf9104b28, Any::Imm12 },     // str x8, [x25, #bump]
        { 0xaa1403e0 },                 // mov x0, x20
        { kBl, Any::Branch },           // bl os_unfair_lock_unlock
        { 0xf9406fe1 },                 // ldr x1, [sp, #0xd8]
        { kB, Any::Branch },            // b resume
    };
    if (!r.Find(s, kBindMaterial, kBind, "BindMaterialParameters' uniform allocation", s.BindAlloc)) return s;
    for (int i = 0; i < kBindAllocLiveWords; i++) s.BindWords[i] = r.Word(s.BindAlloc + 4 * i);
    s.BindResume = JumpTarget(s.BindAlloc + 13 * 4, s.BindWords[13]);
    s.RendererLock = AddImmediate(s.BindWords[1]).value_or(0);
    s.RendererOffset = r.Offset(s.BindAlloc + 4 * 4);
    if (!Require(s, CallsImport(image, s.BindAlloc + 3 * 4, "_os_unfair_lock_lock") && CallsImport(image, s.BindAlloc + 11 * 4, "_os_unfair_lock_unlock"),
            "BindMaterialParameters' allocation does not take os_unfair_lock") ||
        !Require(s, r.Offset(s.BindAlloc + 9 * 4) == s.RendererOffset, "BindMaterialParameters bumps a different field than it reads") ||
        !Require(s, s.BindResume > s.BindAlloc && s.BindResume < r.End(kBindMaterial), "BindMaterialParameters' allocation does not resume inside it") ||
        !Require(s, OnOnePage(s.BindAlloc, kBindAllocWords, CodePageSize()), "BindMaterialParameters' allocation crosses a page boundary")) {
        return s;
    }

    // SetupEngineParameters: the same, on the renderer inside the object at [x19, #holder].
    constexpr P kSetup[kSetupAllocLiveWords] = {
        { 0xf9500277, Any::Imm12 },     // ldr x23, [x19, #holder]
        { 0x9107e2fb, Any::Imm12 },     // add x27, x23, #(renderer + lock)
        { 0xaa1b03e0 },                 // mov x0, x27
        { kBl, Any::Branch },           // bl os_unfair_lock_lock
        { 0xf95142e8, Any::Imm12 },     // ldr x8, [x23, #(renderer + bump)]
        { 0x4b0803e9 }, { 0x92400d29 }, { 0x8b08013c },   // align up to 16 into x28
        { 0x8b180388 },                 // add x8, x28, x24           + the size
        { 0xf91142e8, Any::Imm12 },     // str x8, [x23, #(renderer + bump)]
        { 0xaa1303f7 },                 // mov x23, x19
        { 0xaa1b03e0 },                 // mov x0, x27
        { kBl, Any::Branch },           // bl os_unfair_lock_unlock
    };
    if (!r.Find(s, kSetupEngine, kSetup, "SetupEngineParameters' uniform allocation", s.SetupAlloc)) return s;
    for (int i = 0; i < kSetupAllocLiveWords; i++) s.SetupWords[i] = r.Word(s.SetupAlloc + 4 * i);
    s.SetupResume = s.SetupAlloc + kSetupAllocLiveWords * 4;
    s.SetupRendererHolder = r.Offset(s.SetupAlloc);
    uint32_t lockInHolder = AddImmediate(s.SetupWords[1]).value_or(0);
    uint32_t bumpInHolder = r.Offset(s.SetupAlloc + 4 * 4);
    s.SetupRendererInHolder = lockInHolder - s.RendererLock;
    if (!Require(s, CallsImport(image, s.SetupAlloc + 3 * 4, "_os_unfair_lock_lock") && CallsImport(image, s.SetupAlloc + 12 * 4, "_os_unfair_lock_unlock"),
            "SetupEngineParameters' allocation does not take os_unfair_lock") ||
        !Require(s, r.Offset(s.SetupAlloc + 9 * 4) == bumpInHolder, "SetupEngineParameters bumps a different field than it reads") ||
        !Require(s, lockInHolder >= s.RendererLock && bumpInHolder >= s.RendererOffset && bumpInHolder - s.RendererOffset == s.SetupRendererInHolder,
            "SetupEngineParameters' lock and offset are not the renderer's, as BindMaterialParameters lays it out") ||
        !Require(s, OnOnePage(s.SetupAlloc, kSetupAllocWords, CodePageSize()), "SetupEngineParameters' allocation crosses a page boundary")) {
        return s;
    }

    // MetalRenderer::BeginFrame: movi v0.2d, #0; str q0, [x20, #bump] (the offset and the word after it); ldr x0, [x20]
    constexpr P kReset[] = { { 0x6f00e400 }, { 0x3d882680, Any::Imm12 }, { 0xf9400280 } };
    uint64_t reset;
    if (!r.Find(s, kBeginFrame, kReset, "BeginFrame's reset of the uniform offset", reset)) return s;
    s.FrameReset = reset + 4;
    s.FrameResetWord = r.Word(s.FrameReset);
    Require(s, r.Offset(s.FrameReset) == s.RendererOffset, "BeginFrame resets a different field than the allocations bump");
    return s;
}

// --- AO blur ----------------------------------------------------------------------------------

AoBlurSites FindAoBlur(Resolver& r)
{
    AoBlurSites s;
    // AddCommand is kept as the bind's own call; the island writes into the
    // command it returns, as the replaced store did.
    if (!r.Functions(s, { kAoBlurBind, kAddCommand })) return s;

    // The unified-memory branch records the ring (x20) itself:
    //   add x0, x19, #8; mov w21, #8; mov w1, #0x28; mov w2, #8; bl AddCommand; str x20, [x0, #8]
    //   adrp x8; ldr q0, [x8, #..]; str q0, [x0, #0x10]; mov w8, #0xff; strb w8, [x0, #0x20]; strb w21, [x0]; b
    constexpr P kBind[kAoBlurBindWords] = {
        { 0x91002260 }, { 0x52800115, Any::Imm16 }, { 0x52800501 }, { 0x52800102 }, { kBl, Any::Branch }, { 0xf9000414 },
        { 0x90000008, Any::Adrp }, { 0x3dc2e500, Any::Imm12 }, { 0x3d800400 }, { 0x52801fe8 }, { 0x39008008 }, { 0x39000015 },
        { kB, Any::Branch },
    };
    if (!r.Find(s, kAoBlurBind, kBind, "the unified-memory bind", s.Bind)) return s;
    for (int i = 0; i < kAoBlurBindWords; i++) s.BindWords[i] = r.Word(s.Bind + 4 * i);
    if (!Require(s, CallTarget(s.Bind + 16, s.BindWords[4]) == r.VA(kAddCommand), "the bind's call is not CommandDispatcherBuffer::AddCommand") ||
        !Require(s, OnOnePage(s.Bind, kAoBlurBindWords, CodePageSize()), "the unified-memory bind crosses a page boundary")) {
        return s;
    }

    // The other branch records the slot (ldr x20, [x20, #0x20]) as command kind #k:
    //   ... mov w8, #k; strb w8, [x0]
    constexpr P kOther[] = {
        { 0xf9401294 }, { 0x91002260 }, { 0x52800501 }, { 0x52800102 }, { kBl, Any::Branch }, { 0xf9000414 },
        { 0x90000008, Any::Adrp }, { 0x3dc2e500, Any::Imm12 }, { 0x3d800400 }, { 0x52801fe8 }, { 0x39008008 },
        { 0x528000e8, Any::Imm16 }, { 0x39000008 },
    };
    uint64_t other;
    if (!r.Find(s, kAoBlurBind, kOther, "the bind without unified memory", other)) return s;
    s.BufferCommand = (uint32_t)MovzValue(r.Word(other + 11 * 4)).value_or(0);

    // The ring update: ldr w8, [x20, #counter]; add w8, w8, #1; str w8, [x20, #counter]; and w8, w8, #3; ldr x1, [x20, w8, uxtw #3]
    constexpr P kCounter[] = { { 0xb9402a88, Any::Imm12 }, { 0x11000508 }, { 0xb9002a88, Any::Imm12 }, { 0x12000508 }, { 0xf8685a81 } };
    uint64_t counter;
    if (!r.Find(s, kAoBlurBind, kCounter, "the ring update", counter)) return s;
    s.CounterLoad = r.Word(counter);
    s.CounterMask = r.Word(counter + 12);
    s.SlotLoad = r.Word(counter + 16);
    s.RingCounter = r.Offset(counter);
    Require(s, r.Offset(counter + 8) == s.RingCounter, "the ring update stores a different counter than it loads");
    return s;
}

// --- Temporal jitter ---------------------------------------------------------------------------

JitterSites FindJitter(Resolver& r)
{
    JitterSites s;
    if (!r.Functions(s, { kJitteredProjection, kUpdateConstantBuffers, kVelocityStartRecording, kViewSubmit, kPostStartRecording, kFsrExecute, kPostGetType,
             kPostLoad, kBaseAddPack }) ||
        !r.Symbols(s, { kPostVtable, kFsrVtable, kGraphicSettings, kHalton })) {
        return s;
    }
    // Each load is preceded by the index and table it uses:
    //   ldr x8, [xV, #frame]; ldp w9, w8, [x8, #index]; add w8, w8, w9; and w8, w8, #7
    //   adrp x9, table; add x9, x9, #table
    constexpr uint32_t kIndex[] = { 0x29402109, 0x0b090108, 0x12000908, 0x90000009, 0x91000129 };
    constexpr uint32_t kIndexMask[] = { Any::Imm7, 0xffffffff, 0xffffffff, Any::Adrp, Any::Imm12 };
    auto withIndex = [&](uint32_t frameLoad, std::initializer_list<P> tail) {
        std::vector<P> p{ { frameLoad, Any::Imm12 } };
        for (int i = 0; i < 5; i++) p.push_back({ kIndex[i], kIndexMask[i] });
        p.insert(p.end(), tail);
        return p;
    };
    // GetJitteredProjectionMatrix (view in x20): ... ldr d2, [x20, #size]; scvtf; movi; fmul; ldr d3, [x9, w8, uxtw #3]
    auto projection = withIndex(0xf9400288, { { 0xfd400282, Any::Imm12 }, { 0x0e21d842 }, { 0x0f0167e3 }, { 0x2e23dc42 }, { 0xfc685923 } });
    // UpdateConstantBuffersData (view in x20): ... ldr d0, [x20, #size]; ...; ldr d1, [x9, w8, uxtw #3]
    auto camera = withIndex(0xf9400288, { { 0xfd400280, Any::Imm12 }, { 0x0e21d800 }, { 0x0f0167e1 }, { 0x2e21dc00 }, { 0x4ea31c61 },
                                           { 0x6e0c0481 }, { 0x2e20fc20 }, { 0xfc685921 } });
    // VelocityRenderStage::StartRecording (stage in x19, view at [x19, #8]): ldr x8, [x19, #frame]; ...; ldr d0, [x9, w8, uxtw #3]
    auto velocity = withIndex(0xf9400268, { { 0xfc685920 } });
    uint64_t p, c, v;
    if (!r.Find(s, kJitteredProjection, projection, "the projection's jitter load", p) ||
        !r.Find(s, kUpdateConstantBuffers, camera, "the camera's jitter load", c) ||
        !r.Find(s, kVelocityStartRecording, velocity, "the velocity's jitter load", v)) {
        return s;
    }
    auto load = [&](uint64_t start, size_t words) {
        uint64_t site = start + 4 * (words - 1);
        return JitterSites::Load{ site, r.Word(site), RegD(r.Word(site)), RegN(r.Word(start)) };
    };
    s.Projection = load(p, projection.size());
    s.Camera = load(c, camera.size());
    s.Velocity = load(v, velocity.size());
    s.VelocityStageReg = s.Velocity.ViewReg;
    s.Velocity.ViewReg = 0;
    s.ViewFrame = r.Offset(p);
    s.VelocityFrame = r.Offset(v);
    s.FrameIndex = (uint32_t)PairOffset(r.Word(p + 4)).value_or(0);
    s.FrameIndexAdd = s.FrameIndex + 4;         // the second w register of the ldp
    s.ViewSize = r.Offset(p + 24);
    s.JitterTable = PageAddress(p + 16, r.Word(p + 16), r.Word(p + 20));
    if (!Require(s, r.Offset(c) == s.ViewFrame && r.Offset(c + 24) == s.ViewSize, "the camera's jitter reads another view field than the projection's") ||
        !Require(s, PairOffset(r.Word(c + 4)) == (int32_t)s.FrameIndex && PairOffset(r.Word(v + 4)) == (int32_t)s.FrameIndex,
            "the three jitter loads index the table differently") ||
        !Require(s, s.JitterTable == r.VA(kHalton) && PageAddress(c + 16, r.Word(c + 16), r.Word(c + 20)) == s.JitterTable &&
                        PageAddress(v + 16, r.Word(v + 16), r.Word(v + 20)) == s.JitterTable,
            "the jitter loads do not all read (anonymous)::s_Halton_2_3")) {
        return s;
    }

    // GetJitteredProjectionMatrix: ldr x21, [x1, #camera]; ldrb w8, [x21, #unjittered]; cbz w8
    constexpr P kCamera[] = { { 0xf9400035, Any::Imm12 }, { 0x394002a8, Any::Imm12 }, { 0x34000008, Any::CondBranch } };
    uint64_t cam;
    if (!r.Find(s, kJitteredProjection, kCamera, "the view's camera", cam)) return s;
    s.ViewCamera = r.Offset(cam);
    s.CameraUnjittered = r.Offset(cam + 4);

    // The velocity stage reaches its view through itself: ldr x1, [x19, #view]; ldr x20, [x1, #camera].
    // That the second load is the view's camera is what makes the first the view.
    constexpr P kStageView[] = { { 0xf9400261, Any::Imm12 }, { 0xf9400034, Any::Imm12 } };
    uint64_t sv;
    if (!r.Find(s, kVelocityStartRecording, kStageView, "the velocity stage's view", sv)) return s;
    if (!Require(s, r.Offset(sv + 4) == s.ViewCamera && RegN(r.Word(sv)) == s.VelocityStageReg,
            "the velocity stage does not hold its view where its jitter load's stage is")) {
        return s;
    }
    s.VelocityViewInStage = r.Offset(sv);

    // GameRenderView::Submit: ldr x19, [x0, #stages]; ldr w8, [x0, #count]; cmp x19, #0
    constexpr P kStages[] = { { 0xf9400013, Any::Imm12 }, { 0xb9400008, Any::Imm12 }, { 0xf100027f } };
    uint64_t st;
    if (!r.Find(s, kViewSubmit, kStages, "the view's stage list", st)) return s;
    s.ViewStages = r.Offset(st);
    s.ViewStageCount = r.Offset(st + 4);

    // PostProcessStage::StartRecording: ldrb w8, [x0, #enabled]; cbz w8; mov x19, x0; ldr x20, [x0, #steps]; ldr w8, [x0, #count]
    constexpr P kSteps[] = { { 0x39400008, Any::Imm12 }, { 0x34000008, Any::CondBranch }, { 0xaa0003f3 }, { 0xf9400014, Any::Imm12 }, { 0xb9400008, Any::Imm12 } };
    // ... and per step: ldr x0, [x20]; ldrb w8, [x0, #enabled]; cbz w8
    constexpr P kStep[] = { { 0xf9400280 }, { 0x39400008, Any::Imm12 }, { 0x34000008, Any::CondBranch } };
    uint64_t steps, step;
    if (!r.Find(s, kPostStartRecording, kSteps, "the post-process stage's steps", steps) ||
        !r.Find(s, kPostStartRecording, kStep, "a step's enabled flag", step)) {
        return s;
    }
    s.NodeEnabled = r.Offset(steps);
    s.StageSteps = r.Offset(steps + 12);
    s.StageStepCount = r.Offset(steps + 16);
    if (!Require(s, r.Offset(step + 4) == s.NodeEnabled, "stages and steps keep their enabled flag in different places")) return s;

    // PostProcessStep_FSR::ExecuteWTKernel: x21 = GraphicSettings; ldrb w8, [x21, #quality]; scvtf..; cmp x8, #3; b.hi
    constexpr P kQuality[] = { { 0x394002a8, Any::Imm12 }, { 0x1e241000 }, { 0xf1000d1f }, { 0x54000008, Any::CondBranch } };   // b.hi
    uint64_t q;
    if (!r.Find(s, kFsrExecute, kQuality, "FSR's quality setting", q)) return s;
    s.SettingsQuality = r.Offset(q);
    s.RenderSettings = r.VA(kGraphicSettings);
    if (!Require(s, r.Materialises(kFsrExecute, s.RenderSettings), "FSR's kernel does not read GraphicSettings")) return s;

    // The vtables an object of each class points to, as the code that builds
    // one stores them, and the post-process stage's GetType in its slot.
    s.PostStageVtable = r.VA(kPostVtable) + kVtableAddressPoint;
    s.FsrStepVtable = r.VA(kFsrVtable) + kVtableAddressPoint;
    s.PostStageMethod = r.VA(kPostGetType);
    if (!Require(s, r.Materialises(kBaseAddPack, s.PostStageVtable), "no post-process stage is built with its vtable's address point") ||
        !Require(s, r.Materialises(kPostLoad, s.FsrStepVtable), "no FSR step is built with its vtable's address point") ||
        !r.Slot(s, kPostVtable, kPostGetType, s.PostStageMethodSlot)) {
        return s;
    }
    return s;
}

// --- Cluster guard ---------------------------------------------------------------------------------

ClusterSites FindCluster(Resolver& r)
{
    ClusterSites s;
    r.Functions(s, { kClusterExecute });
    return s;
}

// --- The game's Metal calls ------------------------------------------------------------------------

MetalCallSites FindMetalCalls(Resolver& r)
{
    MetalCallSites s;
    std::vector<std::string_view> selectors;
    for (auto const& stub : r.Image().SymbolsNamed(kSelectorStubPrefix, "")) {
        std::string_view selector = stub.Name.substr(kSelectorStubPrefix.size());
        if (std::find(selectors.begin(), selectors.end(), selector) == selectors.end()) selectors.push_back(selector);
    }
    s.Calls = (uint32_t)selectors.size();
    for (std::string_view selector : selectors) s.Followed += IsHooked(selector) ? 1 : 0;
    s.Problem = CheckMetalCalls(selectors);
    return s;
}

// --- Duplicate shadow pass -------------------------------------------------------------------------

StagePackSites FindStagePacks(Resolver& r)
{
    StagePackSites s;
    // BaseApp::InitFrameStages(view, Span) is what the fix wraps and skips a call to.
    if (!r.Functions(s, { kAppInitStages, kBaseInitStages, kBaseAddPack }) || !r.Symbols(s, { kAppVtable, kPackShadow, kPackFirst })) return s;
    s.AppInitStages = r.VA(kAppInitStages);
    s.BaseAddPack = r.VA(kBaseAddPack);
    if (!r.Slot(s, kAppVtable, kAppInitStages, s.AppInitStagesSlot) || !r.Slot(s, kAppVtable, kBaseAddPack, s.AppAddPackSlot)) return s;
    if (!Require(s, CallsTo(r.Image(), s.AppInitStages, r.End(kAppInitStages), r.VA(kBaseInitStages)).size() == 1,
            "App::InitFrameStages does not call BaseApp::InitFrameStages(view) once")) {
        return s;
    }
    // BaseApp::InitFrameStages(view): each pack goes to the virtual as a Span {begin, end}:
    //   adrp x8, FIRST_PACK; add x8, x8, #..; add x9, x8, #7; stp x8, x9, [sp]; ldr x8, [x20]; ldr x8, [x8, #slot]
    constexpr P kFirst[] = { { 0x90000008, Any::Adrp }, { 0x91000108, Any::Imm12 }, { 0x91001d09 }, { 0xa90027e8 }, { 0xf9400288 }, { 0xf9400108, Any::Imm12 } };
    //   adrp x21, SHADOW_BUFFER_PACK; add x21, x21, #..; add x22, x21, #1; stp x21, x22, [sp]
    constexpr P kShadow[] = { { 0x90000015, Any::Adrp }, { 0x910002b5, Any::Imm12 }, { 0x910006b6 }, { 0xa9005bf5 } };
    //   ... and after the next pack's call, the same Span again: blr x8; stp x21, x22, [sp]
    constexpr P kShadowAgain[] = { { 0xd63f0100 }, { 0xa9005bf5 } };
    uint64_t first, shadow, again;
    if (!r.Find(s, kBaseInitStages, kFirst, "the first pack", first) || !r.Find(s, kBaseInitStages, kShadow, "the shadow pack", shadow) ||
        !r.Find(s, kBaseInitStages, kShadowAgain, "the shadow pack's second pass", again)) {
        return s;
    }
    s.PackFirst = PageAddress(first, r.Word(first), r.Word(first + 4));
    s.PackShadow = PageAddress(shadow, r.Word(shadow), r.Word(shadow + 4));
    uint64_t vcall = r.Offset(first + 20);
    if (!Require(s, s.PackFirst == r.VA(kPackFirst) && s.PackShadow == r.VA(kPackShadow), "the packs passed are not FIRST_PACK and SHADOW_BUFFER_PACK") ||
        !Require(s, r.VA(kAppVtable) + kVtableAddressPoint + vcall == s.AppAddPackSlot, "the packs go to another virtual than BaseApp::InitFrameStages(view, Span)")) {
        return s;
    }
    auto byte = [&](uint64_t va) { auto p = static_cast<uint8_t const*>(r.Image().DataAt(va, 1)); return p ? *p : 0; };
    s.PackShadowByte = byte(s.PackShadow);
    s.PackFirstByte = byte(s.PackFirst);
    return s;
}

// --- Improved shadows ------------------------------------------------------------------------------

// A 64-bit constant built as movn xR, #imm; movk xR, #imm, lsl #32 [; movk xR, #imm, lsl #48]
// starting at `at`, or nothing.
std::optional<uint64_t> MovnMovk(Resolver& r, uint64_t at, uint64_t end)
{
    uint32_t movn = r.Word(at);
    if ((movn & 0xffe00000) != 0x92800000) return {};       // movn x, #imm (lsl #0)
    uint64_t value = ~(uint64_t)((movn >> 5) & 0xffff);
    uint32_t reg = RegD(movn);
    int lanes = 0;
    for (uint64_t next = at + 4; next < end && lanes < 3; next += 4, lanes++) {
        uint32_t movk = r.Word(next);
        if ((movk & 0xff80001f) != (0xf2800000 | reg)) break;     // movk xR, #imm, lsl #(16 * hw)
        uint32_t hw = (movk >> 21) & 3;
        value = (value & ~(0xffffull << (16 * hw))) | ((uint64_t)((movk >> 5) & 0xffff) << (16 * hw));
    }
    if (!lanes) return {};
    return value;
}

ShadowMaskSites FindShadowMask(Resolver& r)
{
    ShadowMaskSites s;
    if (!r.Functions(s, { kMaskLoad, kMaskUnload, kStageStartRecording, kMaskExecute, kGetTextureSRV, kSetTextureSafe, kSsaoExecute }) ||
        !r.Symbols(s, { kMaskVtable })) {
        return s;
    }
    s.Load = r.VA(kMaskLoad);
    s.Unload = r.VA(kMaskUnload);
    s.StartRecording = r.VA(kStageStartRecording);
    s.GetTextureSRV = r.VA(kGetTextureSRV);
    s.SetTextureSafe = r.VA(kSetTextureSafe);
    if (!r.Slot(s, kMaskVtable, kMaskLoad, s.LoadSlot) || !r.Slot(s, kMaskVtable, kMaskUnload, s.UnloadSlot) ||
        !r.Slot(s, kMaskVtable, kStageStartRecording, s.StartRecordingSlot)) {
        return s;
    }
    // CascadedShadowMaskStage::ExecuteWTKernel looks a texture up as the shader will:
    //   mov x2, x24; bl ..; ldr x8, [x19, #owner]; ldr x1, [x8]; add x0, sp, #..; mov w2, #hash; movk; mov w3, #-1; mov w4, #0; bl GetTextureSRV
    constexpr P kOwner[] = {
        { 0xaa1803e2 }, { kBl, Any::Branch }, { 0xf9400a68, Any::Imm12 }, { 0xf9400101 }, { 0x910003e0, Any::Imm12 },
        { 0x52800002, Any::Imm16 }, { 0x72a00002, Any::Imm16 }, { 0x12800003 }, { 0x52800004 }, { kBl, Any::Branch },
    };
    //   add x24, x0, #list; ldr w20, [x0, #0x88]; add x21, x0, #0x38   (x24: the command buffer it records into)
    constexpr P kList[] = { { 0x91000018, Any::Imm12 }, { 0xb9408814 }, { 0x9100e015 } };
    uint64_t owner, list;
    if (!r.Find(s, kMaskExecute, kOwner, "the mask stage's texture lookup", owner) || !r.Find(s, kMaskExecute, kList, "the mask stage's command buffer", list)) {
        return s;
    }
    if (!Require(s, CallTarget(owner + 36, r.Word(owner + 36)) == s.GetTextureSRV, "the mask stage's lookup is not SceneGraph::GetTextureSRV")) return s;
    s.StageOwner = r.Offset(owner + 8);
    s.StageList = AddImmediate(r.Word(list)).value_or(0);

    // Load fills the program table, a row of handles per shadow quality:
    //   mov w28, #programs ... cmp x22, #rows-1; add x8, x22, #1; cset w21, lo; add x28, x28, #row; mov x22, x8; cmp x8, #rows; b.eq
    constexpr P kPrograms[] = { { 0x5280001c, Any::Imm16 } };
    constexpr P kRowLoop[] = { { 0xf10002df, Any::Imm12 }, { 0x910006c8 }, { 0x1a9f27f5 }, { 0x9100039c, Any::Imm12 }, { 0xaa0803f6 },
                               { 0xf100011f, Any::Imm12 }, { 0x54000000, Any::CondBranch } };
    //   ... and names each row from two tables, by the row's filter type (1: the default "_PCSS" string, 0: the other):
    //   lsl x9, x22, #2; adrp x8; add x8, x8, #filters; ldr w8, [x8, x9]; adrp x10; add x10, x10, #sizes; ldr w21, [x10, x9]
    //   adrp x24; add x24, x24, #..; cmp w8, #1; b.eq; cbnz w8, (fail); adrp x24; add x24, x24, #..
    constexpr P kRowTables[] = {
        { 0xd37ef6c9 }, { 0x90000008, Any::Adrp }, { 0x91000108, Any::Imm12 }, { 0xb8696908 }, { 0x9000000a, Any::Adrp }, { 0x9100014a, Any::Imm12 },
        { 0xb8696955 }, { 0x90000018, Any::Adrp }, { 0x91000318, Any::Imm12 }, { 0x7100051f }, { 0x54000000, Any::CondBranch },
        { 0x35000008, Any::CondBranch }, { 0x90000018, Any::Adrp }, { 0x91000318, Any::Imm12 },
    };
    uint64_t programs, loop, tables;
    if (!r.Find(s, kMaskLoad, kPrograms, "the mask stage's program table", programs) || !r.Find(s, kMaskLoad, kRowLoop, "the program table's rows", loop) ||
        !r.Find(s, kMaskLoad, kRowTables, "the rows' filter and map size", tables)) {
        return s;
    }
    s.StagePrograms = (uint32_t)MovzValue(r.Word(programs)).value_or(0);
    uint32_t rowBytes = AddImmediate(r.Word(loop + 12)).value_or(0);
    s.ProgramRows = AddImmediate(r.Word(loop + 20)).value_or(0);
    s.ProgramsPerRow = rowBytes / 8;
    s.ProgramCount = s.ProgramRows * s.ProgramsPerRow;
    if (!Require(s, s.ProgramRows >= 1 && s.ProgramRows <= ShadowMaskSites::kMaxRows && rowBytes % 8 == 0 && s.ProgramsPerRow >= 1 &&
                    AddImmediate(r.Word(loop)) == s.ProgramRows - 1,
            "the program table's rows are not laid out as expected")) {
        return s;
    }
    auto text = [&](uint64_t at, size_t most = 8) -> std::string {
        auto p = static_cast<char const*>(r.Image().DataAt(PageAddress(at, r.Word(at), r.Word(at + 4)), most));
        return p ? std::string(p, strnlen(p, most)) : std::string();
    };
    std::string typeOne = text(tables + 28), typeZero = text(tables + 48);
    uint64_t filters = PageAddress(tables + 4, r.Word(tables + 4), r.Word(tables + 8));
    uint64_t sizes = PageAddress(tables + 16, r.Word(tables + 16), r.Word(tables + 20));
    auto filterData = static_cast<uint32_t const*>(r.Image().DataAt(filters, 4 * s.ProgramRows));
    auto sizeData = static_cast<uint32_t const*>(r.Image().DataAt(sizes, 4 * s.ProgramRows));
    // ... and its map size likewise (1: the default "x2048.bshd", 0: the other), for each cascade count after the first:
    //   adrp x7; add x7, x7, #..; cmp w21, #1; b.eq; cbnz w21, (fail); adrp x7; add x7, x7, #..
    constexpr P kRowSize[] = { { 0x90000007, Any::Adrp }, { 0x910000e7, Any::Imm12 }, { 0x710006bf }, { 0x54000000, Any::CondBranch },
                               { 0x35000015, Any::CondBranch }, { 0x90000007, Any::Adrp }, { 0x910000e7, Any::Imm12 } };
    auto sizeNames = FindPattern(r.Image(), r.VA(kMaskLoad), r.End(kMaskLoad), kRowSize);
    auto mapSize = [&](uint64_t at) -> uint32_t {
        std::string name = text(at, 16);
        uint32_t size = 0;
        return sscanf(name.c_str(), "x%u.bshd", &size) == 1 && name == "x" + std::to_string(size) + ".bshd" ? size : 0;
    };
    bool sameSizes = !sizeNames.empty();
    for (uint64_t at : sizeNames) sameSizes = sameSizes && mapSize(at) == mapSize(sizeNames[0]) && mapSize(at + 20) == mapSize(sizeNames[0] + 20);
    uint32_t sizeOne = sameSizes ? mapSize(sizeNames[0]) : 0, sizeZero = sameSizes ? mapSize(sizeNames[0] + 20) : 0;
    if (!Require(s, filterData && sizeData && typeOne == "_PCSS" && typeZero == "_PCF", "the rows' names are not built from PCSS / PCF tables") ||
        !Require(s, sizeOne && sizeZero, "the rows' map sizes are not named as expected")) {
        return s;
    }
    for (uint32_t row = 0; row < s.ProgramRows; row++) {
        auto& out = s.Rows[row];
        if (!Require(s, filterData[row] <= 1 && sizeData[row] <= 1, "a row's filter or map size is not one Load names")) return s;
        out.Pcss = filterData[row] == 1;
        snprintf(out.Filter, sizeof(out.Filter), "%s", (out.Pcss ? typeOne : typeZero).c_str() + 1);
        out.MapSize = sizeData[row] == 1 ? sizeOne : sizeZero;
    }

    // Unload releases each handle and marks it invalid:
    //   ldr x8, [x20, #0x88]; ldr x0, [x8]; bl RemoveShader; [mov x8, #invalid;] str xN, [x19, #handle]
    constexpr P kRelease[] = { { 0xf9404688 }, { 0xf9400100 }, { kBl, Any::Branch } };
    constexpr MaskedWord kMarkInvalid{ 0xf9000260, 0xffc003e0 };
    auto releases = FindPattern(r.Image(), r.VA(kMaskUnload), r.End(kMaskUnload), kRelease);
    bool contiguous = releases.size() == s.ProgramCount;
    for (size_t i = 0; contiguous && i < releases.size(); i++) {
        uint64_t at = releases[i] + 12;
        if (!kMarkInvalid.Matches(r.Word(at))) at += 4;
        contiguous = kMarkInvalid.Matches(r.Word(at)) && r.Offset(at) == s.StagePrograms + 8 * i;
    }
    if (!Require(s, contiguous, "Unload does not release the programs Load stores")) return s;

    // ls::SSAOStage::ExecuteWTKernel reads the G-buffer target the shader samples, by this hash,
    // with the same slice (-1) and view (0) the mask stage passes for its own lookups.
    constexpr P kNormals[] = {
        { 0xf9400a68, Any::Imm12 }, { 0xf9400101 }, { 0x910003e0, Any::Imm12 }, { 0x52836c22 }, { 0x72a981a2 },
        { 0x12800003 }, { 0x52800004 }, { kBl, Any::Branch },
    };
    uint64_t normals;
    if (!r.Find(s, kSsaoExecute, kNormals, "SSAO's G-buffer lookup", normals)) return s;
    s.GBuffer0Hash = (uint32_t)MovzMovkValue(r.Word(normals + 12), r.Word(normals + 16)).value_or(0);
    if (!Require(s, CallTarget(normals + 28, r.Word(normals + 28)) == s.GetTextureSRV, "SSAO's lookup is not SceneGraph::GetTextureSRV") ||
        !Require(s, r.Offset(normals) == s.StageOwner, "SSAO finds the scene graph elsewhere than the mask stage")) {
        return s;
    }

    // rf::TextureBindingInfo: GetTextureSRV copies 16 + 8 bytes out (ldr q0, [x0]; str q0, [x19]; ldr x8, [x0, #16]; str x8, [x19, #16])...
    constexpr P kInfoOut[] = { { 0x3dc00000 }, { 0x3d800260 }, { 0xf9400808 }, { 0xf9000a68 } };
    // ... and SetTextureSafe(this, Binding, Info) takes the 9-byte binding in x1 and the low byte of x2, the info by
    // reference in x3, reading 8 + 16 bytes: mov x20, x2; mov x21, x1; mov x19, x0; ldr x23, [x3]; ldur q0, [x3, #8]
    constexpr P kInfoIn[] = { { 0xaa0203f4 }, { 0xaa0103f5 }, { 0xaa0003f3 }, { 0xf9400077 }, { 0x3cc08060 } };
    // It records the bind as: stur x21, [x0, #1]; strb w20, [x0, #9]; str x22, [x0, #16]; ldr q0, [sp]; stur q0, [x0, #24]; mov w8, #kind; strb w8, [x0]
    constexpr P kRecord[] = { { 0xf8001015 }, { 0x39002414 }, { 0xf9000816 }, { 0x3dc003e0 }, { 0x3c818000 }, { 0x52800008, Any::Imm16 }, { 0x39000008 } };
    // The mask stage records its own fragment binds the same way, inlined, with 0xff as the ninth byte:
    //   stur xB, [x0, #1]; mov w8, #0xff; strb w8, [x0, #9]; str xT, [x0, #16]; ldr q0, [sp, #..]; stur q0, [x0, #24]; mov w8, #kind; strb w8, [x0]
    constexpr P kStageRecord[] = { { 0xf8001000, 0xffffffe0 }, { 0x52801fe8 }, { 0x39002408 }, { 0xf9000800, 0xffffffe0 }, { 0x3dc003e0, Any::Imm12 },
                                   { 0x3c818000 }, { 0x52800008, Any::Imm16 }, { 0x39000008 } };
    uint64_t infoOut, infoIn, record;
    if (!r.Find(s, kGetTextureSRV, kInfoOut, "GetTextureSRV's result", infoOut) || !r.Find(s, kSetTextureSafe, kInfoIn, "SetTextureSafe's arguments", infoIn) ||
        !r.Find(s, kSetTextureSafe, kRecord, "SetTextureSafe's command", record)) {
        return s;
    }
    s.TextureBindingBytes = r.Offset(infoOut + 8) + 8;
    s.TextureCommand = (uint32_t)MovzValue(r.Word(record + 20)).value_or(0);
    auto stageRecords = FindPattern(r.Image(), r.VA(kMaskExecute), r.End(kMaskExecute), kStageRecord);
    bool sameKind = !stageRecords.empty();
    for (uint64_t at : stageRecords) sameKind = sameKind && MovzValue(r.Word(at + 24)) == s.TextureCommand;
    if (!Require(s, s.TextureBindingBytes == 24, "rf::TextureBindingInfo is not a pointer and 16 bytes") ||
        !Require(s, sameKind, "the mask stage does not record its texture binds as SetTextureSafe does")) {
        return s;
    }

    // The bindings the mask stage builds (movn x, #0xfe; movk ..., lsl #32 [; movk ..., lsl #48]). A fragment
    // texture's is 01 ff ff ff ff <slot> ff ff: take one, and know every slot the stage binds.
    bool found = false, slotFree = true;
    for (uint64_t at = r.VA(kMaskExecute); at + 8 <= r.End(kMaskExecute); at += 4) {
        auto value = MovnMovk(r, at, r.End(kMaskExecute));
        if (!value || (*value & 0xff) == 0xff) continue;
        uint8_t bytes[8];
        memcpy(bytes, &*value, 8);
        if (bytes[5] == ShadowMaskSites::kNormalsSlot || bytes[6] == ShadowMaskSites::kNormalsSlot) slotFree = false;
        bool fragmentOnly = bytes[0] == 0x01 && bytes[1] == 0xff && bytes[2] == 0xff && bytes[3] == 0xff && bytes[4] == 0xff && bytes[6] == 0xff && bytes[7] == 0xff;
        if (fragmentOnly && !found) {
            memcpy(s.FragmentBinding, bytes, 8);
            s.FragmentBinding[8] = 0xff;
            found = true;
        }
    }
    s.BindingSlotByte = 5;
    if (!Require(s, found, "the mask stage binds no fragment texture the expected way") ||
        !Require(s, slotFree, "the mask stage already binds something at the normals' slot")) {
        return s;
    }
    s.FragmentBinding[s.BindingSlotByte] = ShadowMaskSites::kNormalsSlot;
    return s;
}

// --- Programs from .bshd files -------------------------------------------------------------------

ProgramSites FindPrograms(Resolver& r)
{
    ProgramSites s;
    if (!r.Functions(s, { kPathCtor, kPathDtor, kCreateShader, kLoadEngineShader, kDeviceCreateShader, kCreatePipelineState, kAddPipelineState }) ||
        !r.Symbols(s, { kDefaultShaderPath })) {
        return s;
    }
    s.MakePath = r.VA(kPathCtor);
    s.DestroyPath = r.VA(kPathDtor);
    s.CreateProgram = r.VA(kCreateShader);

    // DataLoader::CreateShader: adrp x8, s_Data; ldr x8, [x8, #..]; ldr x0, [x8, #device]; add x1, sp, #..; bl GPUDevice::CreateShader
    constexpr P kDevice[] = { { 0x90000008, Any::Adrp }, { 0xf9400108, Any::Imm12 }, { 0xf9400100, Any::Imm12 }, { 0x910003e1, Any::Imm12 }, { kBl, Any::Branch } };
    //   cbz w19 ..; mov x19, #invalid
    constexpr P kInvalid[] = { { 0x34000013, Any::CondBranch }, { 0xd2fff813 } };
    uint64_t device, invalid;
    if (!r.Find(s, kCreateShader, kDevice, "CreateShader's device", device) || !r.Find(s, kCreateShader, kInvalid, "CreateShader's failure value", invalid)) return s;
    s.RendererData = PageAddress(device, r.Word(device), r.Word(device + 4));
    s.DataDevice = r.Offset(device + 8);
    s.InvalidHandle = MovzValue(r.Word(invalid + 4)).value_or(0);
    // The anonymous-namespace name may be ambiguous in another build; the decoded load is what counts.
    if (!Require(s, !r.VA(kRendererData) || r.VA(kRendererData) == s.RendererData, "CreateShader reads another s_Data than the symbol's") ||
        !Require(s, CallTarget(device + 16, r.Word(device + 16)) == r.VA(kDeviceCreateShader), "DataLoader::CreateShader does not call GPUDevice::CreateShader")) {
        return s;
    }

    // IAppStage::AddPipelineState makes pipelines on the same device: adrp x8, s_Data; ldr x8, [x8, #..]; ldr x0, [x8, #device]
    constexpr P kPipelineDevice[] = { { 0x90000008, Any::Adrp }, { 0xf9400108, Any::Imm12 }, { 0xf9400100, Any::Imm12 } };
    uint64_t pipelineDevice;
    if (!r.Find(s, kAddPipelineState, kPipelineDevice, "AddPipelineState's device", pipelineDevice)) return s;
    if (!Require(s, PageAddress(pipelineDevice, r.Word(pipelineDevice), r.Word(pipelineDevice + 4)) == s.RendererData &&
                    r.Offset(pipelineDevice + 8) == s.DataDevice,
            "AddPipelineState makes pipelines on another device than CreateShader's") ||
        !Require(s, CallsTo(r.Image(), r.VA(kAddPipelineState), r.End(kAddPipelineState), r.VA(kCreatePipelineState)).size() == 1,
            "AddPipelineState does not call GPUDevice::CreatePipelineState")) {
        return s;
    }

    // RenderStage::LoadEngineShader builds the same path, in the 16 bytes below its saved registers:
    //   sub sp, sp, #frame; stp x20, x19, [sp, #path bytes]; ...; adrp x8; ldr x2, [x8, #..] (*s_DefaultShaderPath); mov x0, sp; mov w1, #root; bl Path::Path
    constexpr P kPathRoom[] = { { 0xd10003ff, Any::Imm12 }, { 0xa9004ff4, Any::Imm7 } };
    constexpr P kPath[] = { { 0x90000008, Any::Adrp }, { 0xf9400102, Any::Imm12 }, { 0x910003e0 }, { 0x52800001, Any::Imm16 }, { kBl, Any::Branch } };
    uint64_t room, path;
    if (!r.Find(s, kLoadEngineShader, kPath, "the engine shader path", path) || !r.Find(s, kLoadEngineShader, kPathRoom, "the engine shader path's room", room)) return s;
    s.DefaultShaderPath = PageAddress(path, r.Word(path), r.Word(path + 4));
    s.EngineShaderRoot = (uint32_t)MovzValue(r.Word(path + 12)).value_or(0);
    s.PathBytes = (uint32_t)PairOffset(r.Word(room + 4)).value_or(0);
    if (!Require(s, room == r.VA(kLoadEngineShader), "LoadEngineShader's frame is not where the path's room is read from") ||
        !Require(s, s.DefaultShaderPath == r.VA(kDefaultShaderPath), "LoadEngineShader's prefix is not s_DefaultShaderPath") ||
        !Require(s, CallTarget(path + 16, r.Word(path + 16)) == s.MakePath, "LoadEngineShader does not build its path with Path::Path<STDString, StringView>")) {
        return s;
    }
    // Path::Path<STDString, StringView>(this, root, prefix, view): mov x20, x3, then the view's
    // length (ldr w10, [x20, #len]) and its text (ldr x7, [x20]); the path itself starts as stp xzr, xzr, [x19]
    constexpr P kViewArg[] = { { 0xaa0303f4 }, { 0xaa0203f5 }, { 0xaa0103f6 }, { 0xaa0003f3 } };
    constexpr P kViewLength[] = { { 0xb9400a8a, Any::Imm12 }, { 0x0b0a0108 } };
    constexpr P kViewText[] = { { 0xf9400287 } };
    constexpr P kPathInit[] = { { 0xa9007e7f } };
    uint64_t viewArg, viewLength, viewText, pathInit;
    if (!r.Find(s, kPathCtor, kViewArg, "the path's arguments", viewArg) || !r.Find(s, kPathCtor, kViewLength, "the view's length", viewLength) ||
        !r.Find(s, kPathCtor, kViewText, "the view's text", viewText) || !r.Find(s, kPathCtor, kPathInit, "the path's initialisation", pathInit)) {
        return s;
    }
    s.ViewLength = r.Offset(viewLength);
    s.ViewText = r.Offset(viewText);
    if (!Require(s, s.ViewText == 0 && s.ViewLength == 8 && s.PathBytes == 16, "ls::Path or ls::_StringView<char> changed layout")) return s;

    // GPUDevice::CreateShader (this = the device): add x22, x0, #programs ...
    constexpr P kContainer[] = { { 0x91000016, Any::Imm12 } };
    //   ldr w8, [x19, #pagesize]; udiv; msub; and w10, w20, #salt mask; ldr x11, [x19, #salts]; ldr x11, [x11, x9, lsl #3]
    //   ldr w11, [x11, w8, uxtw #2]; cmp; b.ne; ldr x10, [x22] (pages); ldr x9, [x10, x9, lsl #3]; mov w10, #entry; madd x25, x8, x10, x9
    constexpr P kLookup[] = {
        { 0xb9401a68, Any::Imm12 }, { 0x1ac80b29 }, { 0x1b08e528 }, { 0x1200028a, 0xff8003ff }, { 0xf9400a6b, Any::Imm12 }, { 0xf869596b }, { 0xb868596b },
        { 0x6b0b015f }, { 0x54000101, Any::CondBranch }, { 0xf94002ca, Any::Imm12 }, { 0xf8697949 }, { 0x52800a0a, Any::Imm16 }, { 0x9b0a2519 },
    };
    //   the new handle: mov w27, w25 (the index); bfi x27, x9, #54, #10; ubfiz x9, x11, #32, #22; bfi x27, x11, #salt (the salt)
    constexpr P kHandle[] = { { 0x2a1903fb }, { 0xb340013b, 0xffc003ff }, { 0xd3400169, 0xffc003ff }, { 0xb340017b, 0xffc003ff } };
    //   ldr x0, [x19, #library]; mov x2, x22; bl newFunctionWithName:; ...; str x23, [x25, #fragment]
    constexpr P kFragment[] = {
        { 0xf9400260, Any::Imm12 }, { 0xaa1603e2 }, { kBl, Any::Branch }, { 0xaa0003f7 }, { 0xb50000a0, Any::CondBranch },
        { 0xf9400660, Any::Imm12 }, { 0xaa1603e2 }, { kBl, Any::Branch }, { 0xaa0003f7 }, { 0xaa1603e0 }, { kBl, Any::Branch },
        { 0xf9000337, Any::Imm12 }, { 0xf94036a8 },
    };
    uint64_t container, lookup, handle, fragment;
    if (!r.Find(s, kDeviceCreateShader, kContainer, "the device's program container", container) ||
        !r.Find(s, kDeviceCreateShader, kLookup, "the program lookup", lookup) || !r.Find(s, kDeviceCreateShader, kHandle, "the program handle", handle) ||
        !r.Find(s, kDeviceCreateShader, kFragment, "the program's fragment function", fragment)) {
        return s;
    }
    s.DevicePrograms = AddImmediate(r.Word(container)).value_or(0);
    uint32_t pageSize = r.Offset(lookup), salts = r.Offset(lookup + 16);
    if (!Require(s, pageSize >= s.DevicePrograms && salts >= s.DevicePrograms, "the program lookup reads outside the container")) return s;
    s.ContainerPageSize = pageSize - s.DevicePrograms;
    s.ContainerSalts = salts - s.DevicePrograms;
    s.ContainerPages = r.Offset(lookup + 36);
    s.ProgramEntryBytes = (uint32_t)MovzValue(r.Word(lookup + 44)).value_or(0);
    s.DeviceCoreLibrary = r.Offset(fragment);
    s.ProgramFragment = r.Offset(fragment + 44);
    auto salt = BitfieldMoveOf(r.Word(handle + 12));
    auto saltMask = LogicalImmediate(r.Word(lookup + 12));
    if (!Require(s, salt && salt->Insert && salt->Lsb == 32 && salt->Width < 32 && saltMask == (1ull << salt->Width) - 1,
            "a program handle is not a u32 index and the salt the lookup compares")) {
        return s;
    }
    s.SaltShift = salt->Lsb;
    s.SaltMask = (uint32_t)*saltMask;

    // rf::metal::GPUDevice::CreatePipelineState (this = the device) finds the same entry the same way, then
    // reads its functions: ldr x10, [x22]; tst x10, #(mask << shift); b.eq; ldr w8, [x20, #pagesize]; udiv; msub;
    // ubfx x10, x10, #shift, #width; ldr x11, [x20, #salts]; ...; ldr x10, [x20, #pages]; ldr x9, [..]; mov w10, #entry; madd x28
    constexpr P kPipelineLookup[] = {
        { 0xf94002ca }, { 0xf200015f, 0xff8003ff }, { 0x54000000, Any::CondBranch }, { 0xb9400288, Any::Imm12 }, { 0x1ac80949 }, { 0x1b08a928 },
        { 0xd340014a, 0xffc003ff }, { 0xf940028b, Any::Imm12 }, { 0xf869596b }, { 0xb868596b }, { 0x6b0b015f }, { 0x54000001, Any::CondBranch },
        { 0xf940028a, Any::Imm12 }, { 0xf8697949 }, { 0x52800a0a, Any::Imm16 }, { 0x9b0a251c },
    };
    //   ldr x2, [x28, #vertex]; bl setVertexFunction:; ldr x2, [x28, #fragment]; mov x0, x24; bl setFragmentFunction:
    constexpr P kPipelineFunctions[] = { { 0xf9400382, Any::Imm12 }, { kBl, Any::Branch }, { 0xf9400382, Any::Imm12 }, { 0xaa1803e0 }, { kBl, Any::Branch } };
    uint64_t pipelineLookup, pipelineFunctions;
    if (!r.Find(s, kCreatePipelineState, kPipelineLookup, "the pipeline's program lookup", pipelineLookup) ||
        !r.Find(s, kCreatePipelineState, kPipelineFunctions, "the pipeline's functions", pipelineFunctions)) {
        return s;
    }
    auto pipelineSalt = BitfieldMoveOf(r.Word(pipelineLookup + 24));
    s.ProgramVertex = r.Offset(pipelineFunctions);
    if (!Require(s, LogicalImmediate(r.Word(pipelineLookup + 4)) == (uint64_t)s.SaltMask << s.SaltShift && pipelineSalt &&
                    !pipelineSalt->Insert && pipelineSalt->Lsb == s.SaltShift && pipelineSalt->Width == salt->Width,
            "pipelines read a program handle's salt differently") ||
        !Require(s, r.Offset(pipelineLookup + 12) == s.DevicePrograms + s.ContainerPageSize && r.Offset(pipelineLookup + 28) == s.DevicePrograms + s.ContainerSalts &&
                        r.Offset(pipelineLookup + 48) == s.DevicePrograms + s.ContainerPages &&
                        MovzValue(r.Word(pipelineLookup + 56)) == s.ProgramEntryBytes,
            "pipelines find programs elsewhere than CreateShader puts them") ||
        !Require(s, r.Offset(pipelineFunctions + 8) == s.ProgramFragment, "pipelines take the fragment function from another field")) {
        return s;
    }
    return s;
}

// --- Noesis rings ------------------------------------------------------------------------------------

UiRingSites FindUiRing(Resolver& r)
{
    UiRingSites s;
    // Unmap* flush the wrapper's range of its buffer, and DrawBatch binds the
    // wrapper's buffer when it draws: both must keep doing so for a swapped buffer.
    if (!r.Functions(s, { kMapVertices, kMapIndices, kUnmapVertices, kUnmapIndices, kDrawBatch })) return s;
    auto ring = [&](Sym map, UiRingSites::Ring& out) {
        out.Map = r.VA(map);
        out.MapBytes = r.End(map) - out.Map;
        if (auto refusal = CheckEntryHook(r.Image(), out.Map, r.End(map)); refusal != HookRefusal::None) {
            return Resolver::Fail(s, std::string(kSymbols[map].Name) + " cannot be hooked at its entry: " + Describe(refusal));
        }
        //   ldr w21, [x0, #offset]; add w8, w21, w1; cmp w8, #size; b.le; mov w21, #0; str wzr, [x19, #offset]
        //   ldr x22, [x19, #wrapper]; cbz x22; ldr x0, [x22, #buffer]
        constexpr P kWrap[] = {
            { 0xb9400015, Any::Imm12 }, { 0x0b0102a8 }, { 0x7148011f, Any::Imm12 }, { 0x5400006d, Any::CondBranch }, { 0x52800015 },
            { 0xb900027f, Any::Imm12 }, { 0xf9400276, Any::Imm12 }, { 0xb4000016, Any::CondBranch }, { 0xf94002c0, Any::Imm12 },
        };
        //   mov w9, #1; strb w9, [x22, #mapped]; mov w1, w20; stp x8, x1, [x22, #range]
        constexpr P kMapped[] = { { 0x52800029 }, { 0x390002c9, Any::Imm12 }, { 0x2a1403e1 }, { 0xa90006c8, Any::Imm7 } };
        //   stp w9, w8, [x19, #offset]: the new offset and the start of this map
        constexpr P kStart[] = { { 0x29002269, Any::Imm7 } };
        uint64_t wrap, mapped, start;
        if (!r.Find(s, map, kWrap, "the ring's wrap", wrap) || !r.Find(s, map, kMapped, "the buffer's mapping", mapped) ||
            !r.Find(s, map, kStart, "the ring's new offset", start)) {
            return false;
        }
        out.Offset = r.Offset(wrap);
        out.Size = AddImmediate(r.Word(wrap + 8)).value_or(0);
        out.Wrapper = r.Offset(wrap + 24);
        out.Buffer = r.Offset(wrap + 32);
        out.Mapped = r.Offset(mapped + 4);
        out.Range = (uint32_t)PairOffset(r.Word(mapped + 12)).value_or(0);
        out.MapStart = out.Offset + 4;
        return Require(s, r.Offset(wrap + 20) == out.Offset && PairOffset(r.Word(start)) == (int32_t)out.Offset,
            "a ring resets or advances another offset than it reads");
    };
    if (ring(kMapVertices, s.Vertices)) ring(kMapIndices, s.Indices);
    return s;
}

}

GfxSites FindGfxSites(GameImage const& image)
{
    Resolver r(image);
    GfxSites sites;
    sites.LuaJump = FindLuaJump(r);
    sites.ThreadIdling = FindThreadIdling(r);
    sites.ReducedLocking = FindReducedLocking(r);
    sites.AoBlur = FindAoBlur(r);
    sites.Jitter = FindJitter(r);
    sites.Cluster = FindCluster(r);
    sites.MetalCalls = FindMetalCalls(r);
    sites.StagePacks = FindStagePacks(r);
    sites.ShadowMask = FindShadowMask(r);
    sites.Programs = FindPrograms(r);
    sites.UiRing = FindUiRing(r);
    return sites;
}

}
