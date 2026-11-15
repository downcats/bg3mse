#include <Engine/EngineTable.h>
#include <Engine/Entries/Areas.h>
#include <Engine/Entries/OverlayNames.h>
#include <Engine/Entries/PatchNames.h>

namespace bg3se::mac::engine {

std::span<Area const> EngineTable()
{
    static Area const kAreas[] = {
        { "core library", CoreLibraryEntries() },
        { "start-up and loops", StartupEntries() },
        { "stats", StatsEntries() },
        { "world and resources", WorldEntries() },
        { "user interface", UiEntries() },
        { "Osiris", OsirisEntries() },
        { overlay::kArea, OverlayEntries() },
        { patches::kArea, PatchEntries() },
    };
    return kAreas;
}

}
