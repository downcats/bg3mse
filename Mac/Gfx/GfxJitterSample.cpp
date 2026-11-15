#include <Gfx/GfxJitter.h>

// The stubs' callee, alone in its file: the stub probe test links its own in
// place of this one.
extern "C" __attribute__((used)) uint64_t gfx_jitter_sample(void* view, uint64_t original)
{
    return bg3se::mac::gfx::JitterSample(view, original);
}
