#pragma once

#include <string>
#include <vector>

#include <Gfx/GfxPatch.h>
#include <Platform/CodeShape.h>
#include <Platform/Symbols.h>

// Where the graphics features attach to the game, found at start-up.
//
// No game version is compared and no address is written down. Functions and
// globals are found by exact mangled symbol; a site inside a function by
// searching that function for its instruction sequence (only the fields that
// move with code or data left open), with the number of matches stated; a
// vtable slot by searching the named vtable for the method's address. Every
// struct offset a feature relies on is either read back from an instruction
// that uses it, or stands next to the function that uses it; either way every
// function whose behaviour a feature relies on is checked by the shape of its
// whole code, so a patch that changes that code turns the feature off, and a
// patch that only moves it does not.
//
// Each feature is resolved on its own: one that cannot be resolved names the
// reason in `Problem` and the others are unaffected. All addresses are
// link-time addresses (GameImage::LoadedAt gives the running ones).
//
// The research these sites come from (what to patch, sequences, layouts) is
// bg3mfx's, https://github.com/AndrewTriesToCode/bg3mfx (MIT, Copyright (c)
// 2026 AndrewTriesToCode). The code is written for bg3mse.

namespace bg3se::mac::gfx {

struct FunctionCheck
{
    char const* Name;
    CodeShape Expected;
    CodeShape Found;
    bool Present{ false };
};

struct SiteCheck
{
    std::vector<FunctionCheck> Functions;
    std::string Problem;            // empty when the feature can be installed

    bool Ok() const { return Problem.empty(); }
};

// gfx_lua_jump_fix: the two calls Lua's error handling makes through the
// signal-saving setjmp/longjmp.
struct LuaJumpSites : SiteCheck
{
    uint64_t ThrowCall{ 0 };        // luaD_throw: bl _longjmp
    uint64_t ProtectCall{ 0 };      // luaD_rawrunprotected: bl _setjmp
};

// gfx_thread_idling
struct ThreadIdlingSites : SiteCheck
{
    uint64_t WorkerBudget{ 0 };             // WorkerThreadWorker::DoWork: ldr xN, [x19, #budget]
    uint32_t WorkerBudgetWord{ 0 };
    uint32_t WorkerBudgetOffset{ 0 };       // in the worker
    uint64_t SchedulerWindow[2]{};          // WorkerThreadScheduler::DoWork: movz/movk wN, #window, twice
    uint32_t SchedulerWindowWord{ 0 };      // the movz
    uint64_t SchedulerWindowNs{ 0 };
    uint64_t CapIdleYield{ 0 };             // GameClock::CapFrame: bl pthread_yield_np, then b back
    uint64_t CapTargetSelect{ 0 };          // csel x<target>, ...: the wait target in microseconds
    uint32_t CapClockReg{ 0 };              // the register holding the clock
    uint32_t CapTargetReg{ 0 };             // the register holding the target
    uint32_t ClockStartTicks{ 0 };          // GameClock: u64 mach_absolute_time of the frame start
    uint32_t ClockStarted{ 0 };             // GameClock: u8, the start is valid
};

// gfx_reduced_locking
struct ReducedLockingSites : SiteCheck
{
    uint64_t BindAlloc{ 0 }, BindResume{ 0 };       // BindMaterialParameters
    uint32_t BindWords[kBindAllocLiveWords]{};
    uint64_t SetupAlloc{ 0 }, SetupResume{ 0 };     // RenderableObject::SetupEngineParameters
    uint32_t SetupWords[kSetupAllocLiveWords]{};
    uint32_t SetupRendererHolder{ 0 };              // offset of the renderer's holder in the object (0x2000)
    uint32_t SetupRendererInHolder{ 0 };            // offset of the renderer in the holder (0x1f0)
    uint64_t FrameReset{ 0 };                       // MetalRenderer::BeginFrame: str q0, [xR, #bump]
    uint32_t FrameResetWord{ 0 };
    uint32_t RendererLock{ 0 };                     // MetalRenderer: os_unfair_lock
    uint32_t RendererOffset{ 0 };                   // MetalRenderer: u64 bump offset of the uniform buffer
};

// gfx_ao_blur_fix
struct AoBlurSites : SiteCheck
{
    uint64_t Bind{ 0 };                             // CompareSetAndBindConstantBuffer<PPSAO_Blur::blur>, unified-memory branch
    uint32_t BindWords[kAoBlurBindWords]{};
    uint32_t BufferCommand{ 0 };                    // the command kind the other branch records (7)
    uint32_t CounterLoad{ 0 }, CounterMask{ 0 }, SlotLoad{ 0 };   // the function's own ring update
    uint32_t RingCounter{ 0 };                      // DynamicCB: u32 ring counter (0x28)
};

// gfx_temporal: the three loads of BG3's jitter sample and the view layout the
// jitter walks to find an enabled FSR step. The functions checked here also fill
// the constants Temporal reads (UpdateConstantBuffersData), so gfx_temporal as a
// whole depends on them.
struct JitterSites : SiteCheck
{
    struct Load
    {
        uint64_t Site{ 0 };
        uint32_t Word{ 0 };         // ldr dN, [x9, w8, uxtw #3]
        uint32_t SampleReg{ 0 };    // N
        uint32_t ViewReg{ 0 };      // the register the load's frame read uses as the view (projection, camera)
    };
    Load Projection, Camera, Velocity;
    uint32_t VelocityStageReg{ 0 };     // the velocity load's frame read: the stage, holding the view at...
    uint32_t VelocityViewInStage{ 0 };  // ... this offset (read back from the stage's own ldr of the view's camera)
    uint64_t JitterTable{ 0 };          // what the loads index ((anonymous)::s_Halton_2_3)
    uint64_t PostStageVtable{ 0 };      // ls::PostProcessStage's address point
    uint64_t PostStageMethodSlot{ 0 };  // the slot holding GetType, found by searching the vtable
    uint64_t PostStageMethod{ 0 };      // ls::PostProcessStage::GetType
    uint64_t FsrStepVtable{ 0 };        // ls::PostProcessStep_FSR's address point
    uint64_t RenderSettings{ 0 };       // _GraphicSettings, a pointer
    uint32_t ViewStages{ 0 }, ViewStageCount{ 0 };
    uint32_t ViewFrame{ 0 }, ViewSize{ 0 }, ViewCamera{ 0 };
    uint32_t StageSteps{ 0 }, StageStepCount{ 0 };
    uint32_t NodeEnabled{ 0 };
    uint32_t FrameIndex{ 0 }, FrameIndexAdd{ 0 };
    uint32_t CameraUnjittered{ 0 };
    uint32_t SettingsQuality{ 0 };
    uint32_t VelocityFrame{ 0 };        // where the velocity stage keeps its own pointer to the frame object
};

// gfx_cluster_guard: no code is patched. The guard acts on the kernels of
// BG3's clustered-lighting pass; that pass is checked by shape so a game patch
// that changes it turns the guard off.
struct ClusterSites : SiteCheck
{
};

// The Metal hooks (gfx_stall_log, gfx_bind_dedup, gfx_cluster_guard,
// gfx_temporal): no code is patched. The game's Objective-C calls into Metal
// must all be ones the hooks follow (Gfx/GfxMetalCalls.h).
struct MetalCallSites : SiteCheck
{
    uint32_t Calls{ 0 };            // the game's selectors, all of them
    uint32_t Followed{ 0 };         // those of them the hooks replace
};

// gfx_duplicate_shadow_fix
struct StagePackSites : SiteCheck
{
    uint64_t AppInitStagesSlot{ 0 }, AppInitStages{ 0 };   // App::InitFrameStages(view)
    uint64_t AppAddPackSlot{ 0 }, BaseAddPack{ 0 };        // BaseApp::InitFrameStages(view, Span)
    uint64_t PackShadow{ 0 }, PackFirst{ 0 };              // SHADOW_BUFFER_PACK, FIRST_PACK
    uint8_t PackShadowByte{ 0 }, PackFirstByte{ 0 };       // their first stage types, for the log
};

// gfx_improved_shadows: the mask stage, its program table and how a texture is bound in it.
struct ShadowMaskSites : SiteCheck
{
    static constexpr int kMaxRows = 4;
    static constexpr uint8_t kNormalsSlot = 6;  // the shader's texture(6), a slot the stage leaves free
    struct Row
    {
        char Filter[8]{};           // "PCSS", "PCF": the name part Load builds for this row
        bool Pcss{ false };         // BG3's filter type 1
        uint32_t MapSize{ 0 };
    };

    uint64_t LoadSlot{ 0 }, Load{ 0 };
    uint64_t UnloadSlot{ 0 }, Unload{ 0 };
    uint64_t StartRecordingSlot{ 0 }, StartRecording{ 0 };
    uint32_t StageOwner{ 0 };           // stage+owner -> object whose first field is the scene graph
    uint32_t StageList{ 0 };            // the stage's RendererCommandBuffer
    uint32_t StagePrograms{ 0 };        // ProgramCount u64 program handles
    uint32_t ProgramCount{ 0 };         // Rows x ProgramsPerRow, as Load fills and Unload releases them
    uint32_t ProgramRows{ 0 }, ProgramsPerRow{ 0 };
    Row Rows[kMaxRows];
    uint64_t GetTextureSRV{ 0 }, SetTextureSafe{ 0 };
    uint32_t GBuffer0Hash{ 0 };
    uint32_t TextureBindingBytes{ 0 };  // rf::TextureBindingInfo, as GetTextureSRV writes and SetTextureSafe reads it
    uint8_t FragmentBinding[9]{};       // rf::sb::Binding of a fragment texture, from the stage's own binds
    uint32_t BindingSlotByte{ 0 };      // which of its bytes holds the slot
    uint32_t TextureCommand{ 0 };       // the command kind both the stage's binds and SetTextureSafe record
};

// Making a program from a .bshd past BG3's shader cache (gfx_improved_shadows).
struct ProgramSites : SiteCheck
{
    uint64_t MakePath{ 0 }, DestroyPath{ 0 }, CreateProgram{ 0 };
    uint64_t DefaultShaderPath{ 0 };    // STDString*
    uint64_t RendererData{ 0 };         // (anonymous)::s_Data, a pointer
    uint32_t EngineShaderRoot{ 0 };
    uint32_t PathBytes{ 0 };            // the room LoadEngineShader gives an ls::Path
    uint32_t ViewText{ 0 }, ViewLength{ 0 };    // ls::_StringView<char>: char const*, u32
    uint64_t InvalidHandle{ 0 };
    uint32_t DataDevice{ 0 };
    uint32_t DevicePrograms{ 0 };       // the paged container of programs, in the device
    uint32_t ContainerPages{ 0 }, ContainerSalts{ 0 }, ContainerPageSize{ 0 };
    uint32_t SaltShift{ 0 };            // a handle: u32 index, then the salt at this bit...
    uint32_t SaltMask{ 0 };             // ... of this many bits
    uint32_t ProgramEntryBytes{ 0 }, ProgramVertex{ 0 }, ProgramFragment{ 0 };
    uint32_t DeviceCoreLibrary{ 0 };
};

// gfx_ui_ring_fix: ns::Renderer's Noesis geometry rings.
struct UiRingSites : SiteCheck
{
    struct Ring
    {
        uint64_t Map{ 0 };              // ns::Renderer::MapVertices / MapIndices
        uint64_t MapBytes{ 0 };         // its length, up to the next symbol
        uint32_t Wrapper{ 0 };          // rf buffer pointer in ns::Renderer
        uint32_t Offset{ 0 };           // u32 ring offset
        uint32_t MapStart{ 0 };         // u32 offset of the current map
        uint32_t Size{ 0 };             // ring size in bytes
        uint32_t Mapped{ 0 };           // in the rf buffer: u8 mapped flag
        uint32_t Range{ 0 };            // in the rf buffer: u64 offset, u64 length
        uint32_t Buffer{ 0 };           // in the rf buffer: id<MTLBuffer>
    };
    Ring Vertices, Indices;
};

struct GfxSites
{
    LuaJumpSites LuaJump;
    ThreadIdlingSites ThreadIdling;
    ReducedLockingSites ReducedLocking;
    AoBlurSites AoBlur;
    JitterSites Jitter;
    ClusterSites Cluster;
    MetalCallSites MetalCalls;
    StagePackSites StagePacks;
    ShadowMaskSites ShadowMask;
    ProgramSites Programs;
    UiRingSites UiRing;
};

// Runs on the loaded game and on a game binary mapped from disk alike. On a
// file whose pointers are chained fixups, vtable slots cannot be read and the
// features that need them say so (they resolve in the running game only).
GfxSites FindGfxSites(GameImage const& image);

}
