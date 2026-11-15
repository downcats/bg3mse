// Sun shadow fixes. See GfxShadows.h.

#include <Gfx/GfxShadows.h>
#include <Gfx/GfxSites.h>
#include <Platform/CodeWriter.h>

#include <cstdint>

#if __has_feature(objc_arc)
#error "GfxShadows.mm uses manual retain/release"
#endif

namespace bg3se::mac::gfx {

namespace {

GameImage const* gImage = nullptr;

uintptr_t GameAddr(uint64_t va)
{
    return (uintptr_t)gImage->LoadedAt(va);
}

bool VtableSlotIs(uint64_t slotVA, uint64_t methodVA)
{
    return *reinterpret_cast<uintptr_t const*>(GameAddr(slotVA)) == GameAddr(methodVA);
}

std::string SetVtableSlot(uint64_t slotVA, void* value)
{
    return StorePointer(reinterpret_cast<void*>(GameAddr(slotVA)), value);
}

// --- Duplicate shadow pass ----------------------------------------------------------------------
// BaseApp::InitFrameStages adds each view's stages by passing fixed "packs" of
// stage types to a virtual, and it passes the Shadow pack twice. Both resulting
// stages draw the same cascades into the same maps, and nothing reads them in
// between. So drop every Shadow pack after a view's first; stages that need the
// shadow buffer look up the first match.

struct StagePack
{
    uint8_t const *begin, *end;
};
using AddStagePack = void (*)(void* app, void* view, StagePack const* pack);

AddStagePack gAddStagePack;
uint8_t const* gPackFirst;
uint8_t const* gPackShadow;
thread_local void* tViewWithShadow;

void HookAddStagePack(void* app, void* view, StagePack const* pack)
{
    if (pack->begin == gPackFirst) {
        tViewWithShadow = nullptr;   // every view's list opens with this one
    } else if (pack->begin == gPackShadow) {
        if (tViewWithShadow == view) return;
        tViewWithShadow = view;
    }
    gAddStagePack(app, view, pack);
}

}

std::string InstallDuplicateShadowFix(GameImage const& image, StagePackSites const& sites)
{
    if (!sites.Ok()) return sites.Problem;
    gImage = &image;
    if (!VtableSlotIs(sites.AppInitStagesSlot, sites.AppInitStages) || !VtableSlotIs(sites.AppAddPackSlot, sites.BaseAddPack)) {
        return "App's vtable does not hold InitFrameStages where it was found";
    }
    gAddStagePack = reinterpret_cast<AddStagePack>(GameAddr(sites.BaseAddPack));
    gPackFirst = reinterpret_cast<uint8_t const*>(GameAddr(sites.PackFirst));
    gPackShadow = reinterpret_cast<uint8_t const*>(GameAddr(sites.PackShadow));
    if (auto problem = SetVtableSlot(sites.AppAddPackSlot, (void*)HookAddStagePack); !problem.empty()) {
        return "could not write App's vtable: " + problem;
    }
    return {};
}

// --- Improved shadows ---------------------------------------------------------------------------

std::string InstallImprovedShadows(ShadowMaskSites const& mask, ProgramSites const& programs)
{
    if (!mask.Ok()) return mask.Problem;
    if (!programs.Ok()) return programs.Problem;
    return "where the shadow-mask programs go in the game is found, but bg3mse has no programs to put there yet: "
           "they have to be written and looked at in the game";
}

}
