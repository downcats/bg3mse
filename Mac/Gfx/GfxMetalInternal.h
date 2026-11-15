#pragma once

// What the files of the Metal hooks share. Objective-C++, manual retain/release.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#import <objc/runtime.h>

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <string>

namespace bg3se::mac::gfx {

// --- Replacing methods -----------------------------------------------------------------------
// One slot per selector, holding the method each hooked class had.

constexpr unsigned kSlotClasses = 8;

struct HookSlot
{
    std::atomic<unsigned> count;
    Class cls[kSlotClasses];
    IMP imp[kSlotClasses];
};

struct HookSpec
{
    HookSlot& slot;
    char const* selector;
    IMP replacement;
};

// Replaces the methods of `object`'s class named in `specs`, all or none; a
// method the object does not respond to is left out. Empty when done (or
// there was nothing to do), else why nothing was replaced.
std::string HookClass(id object, std::initializer_list<HookSpec> specs);

// The method `object`'s class had before `slot`'s replacement.
IMP OriginalSlow(HookSlot& slot, Class cls);
inline IMP Original(HookSlot& slot, id object)
{
    Class cls = object_getClass(object);
    unsigned count = slot.count.load(std::memory_order_acquire);
    for (unsigned i = 0; i < count; i++) {
        if (slot.cls[i] == cls) return slot.imp[i];
    }
    return OriginalSlow(slot, cls);
}

// --- Encoder records -------------------------------------------------------------------------
// What the hooks know of one encoder between its creation and its end.

constexpr unsigned kEncoderTextures = 64;
constexpr unsigned kComputeTextures = 3;

// A fragment constant buffer Temporal reads: where it is bound, or a copy when it was set by value.
enum { kConstTemporal = 0, kConstViewport, kConstCamera, kConstCount };
struct GfxConstant
{
    id<MTLBuffer> buffer;      // not retained
    NSUInteger offset;
    uint32_t byteCount;        // nonzero: from setFragmentBytes
    uint8_t bytes[224];
};

// Pointers to what the game bound are not retained. One is only ever compared
// with the pointer of a later bind in the same encoder, never used: see
// gfx_bind_dedup in GfxMetal.mm for why an equal pointer is the same object.
struct GfxEncoder
{
    id encoder;
    id<MTLCommandBuffer> buffer;
    bool compute;
    id pipeline;
    id fragment[kEncoderTextures];
    uint32_t bindsSeen, bindsDropped;
    // Compute: which of buffer slots 0..31 have had something bound, and what
    // is known of the bound pipeline (PipelineFact), 0 when nothing.
    uint32_t buffersBound;
    uint32_t pipelineFact;

    // Temporal (GfxTemporal.mm).
    id<MTLTexture> colorTarget;                 // a render encoder's first colour attachment
    id computeTextures[kComputeTextures];
    GfxConstant constants[kConstCount];
    bool copyBound;                             // Temporal's own pipeline is bound for the draw being made
    struct
    {
        uint8_t state;                          // FsrState
        bool destinationSeen;                   // RCAS came with the route's textures
        id frame;                               // retained: the frame FSR was given
        id<MTLTexture> color, scaled;
        unsigned route;
    } fsr;
};
enum FsrState : uint8_t { kFsrNone = 0, kFsrWatched, kFsrTakenOver };

// --- Pipelines the features care about --------------------------------------------------------
// Only those are remembered, in a table that grows before it is half full, so
// a miss (nearly every bind) costs a hash and a read or two however many there
// are. A fact is 32 bits: what a feature needs to know at a draw or a
// dispatch. A pipeline is not kept alive by being remembered: its entry goes
// when it is deallocated, before its address can be another's.

constexpr uint32_t kFactClusterSlot = 0x100;        // the kernel reads ClusterInfoBuffer; the slot is in the low 5 bits
constexpr uint32_t kFactKindShift = 16;             // a PipelineKind (GfxTemporalLogic.h), 0 when Temporal does not know it
constexpr uint32_t kFactKindMask = 0xfu << kFactKindShift;
uint32_t PipelineFact(id state);
void RememberPipeline(id state, uint32_t fact, char const* name);
// The name the pipeline's function had, for the log; "" when not remembered.
char const* PipelineName(id state);

// The record of a live encoder the hooks made one for, or null.
GfxEncoder* EncoderFind(id encoder);

// A render pipeline bound past the hooks, and whether this thread is encoding
// passes of the module's own (their encoders get no record).
void RawSetRenderPipeline(id encoder, id state);
extern thread_local bool tOwnEncoding;

// A command buffer of the game's that the module encoded work of its own into
// (MetalFX's scaling): the stall log says so of a buffer that ran long.
void MarkOwnWork(id buffer);

// --- Temporal (GfxTemporal.mm) ------------------------------------------------------------------

// Empty when MetalFX's temporal scaler can be used on `device`, else why not.
std::string TemporalUnavailable(id<MTLDevice> device);
void TemporalStart(float sharpen);
bool TemporalOn();
// What Temporal makes of a pipeline the game has just made with `fragment`
// or `kernel`: 0, or the kind as a fact. A pipeline of a known name whose
// arguments are not the known ones turns Temporal off.
uint32_t TemporalRenderFact(id<MTLDevice> device, MTLRenderPipelineDescriptor* descriptor, MTLRenderPipelineReflection* reflection, id state);
uint32_t TemporalComputeFact(MTLComputePipelineReflection* reflection, id<MTLFunction> kernel);
// A draw with a pipeline Temporal knows. True: Temporal bound a pipeline of
// its own for it, and TemporalAfterDraw puts the game's back.
bool TemporalBeforeDraw(id encoder, GfxEncoder* e);
void TemporalAfterDraw(id encoder, GfxEncoder* e);
// A dispatch with a pipeline Temporal knows, or after FSR was taken over.
// True: the game's dispatch must not run.
bool TemporalDispatch(GfxEncoder* e);
// The end of an encoder in which FSR was seen: what is needed of the record
// is taken before the record is freed, and acted on once the encoder has ended.
struct TemporalEnding
{
    uint8_t state;
    bool destinationSeen;
    id<MTLCommandBuffer> buffer;        // retained, as all below
    id frame;
    id<MTLTexture> color, scaled;
    unsigned route;
};
void TemporalTakeEnding(GfxEncoder* e, TemporalEnding* out);
void TemporalFinishEnding(TemporalEnding* ending);
// Lets go of what a record holds for Temporal, before the record is reused.
void TemporalClear(GfxEncoder* e);

// What a pipeline's description (its MTLBinding objects) lacks of what
// Temporal relies on for `kind`; empty when nothing.
std::string TemporalArgumentsProblem(uint8_t kind, NSArray* bindings) API_AVAILABLE(macos(13.0));
// The cluster guard's fact for a compute pipeline, from its description; 0
// when the guard has nothing to do with it.
uint32_t ClusterFact(MTLComputePipelineReflection* reflection) API_AVAILABLE(macos(13.0));

// A method of Metal's may be written in terms of another of the same object
// (one draw call in terms of the general one, a method of a class in terms of
// its superclass's): the replacement of the inner one then runs inside the
// outer one's, for the same call of the game's. Inside: pass on, do nothing
// else. One object is used by one thread at a time, so the mark is per thread.
extern thread_local id tInside;
struct Entered
{
    id outer;
    bool nested;
    explicit Entered(id object) : outer(tInside), nested(tInside == object) { tInside = object; }
    ~Entered() { tInside = outer; }
    Entered(Entered const&) = delete;
    Entered& operator = (Entered const&) = delete;
};

}
