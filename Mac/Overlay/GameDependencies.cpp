#include <Overlay/Dependencies.h>

#include <Platform/Log.h>

#include <atomic>

namespace bg3se::mac::overlay {

namespace {

constinit std::atomic<engine::Resolution const*> gResolution{ nullptr };

Dependencies CheckRunningGame()
{
    Dependencies dependencies;
    if (auto resolution = gResolution.load(std::memory_order_acquire)) {
        dependencies = CheckDependencies(*resolution, ObjcClassesOfRuntime());
    } else {
        for (size_t part = 0; part < (size_t)Part::Count; part++) {
            dependencies.Checks.push_back({ (Part)part, "the engine table", false, "it was not resolved for this game before the overlay was asked for" });
        }
    }
    for (size_t part = 0; part < (size_t)Part::Count; part++) {
        if (dependencies.Usable((Part)part)) continue;
        Log(LogLevel::Error, "overlay: %s are off: %s", Describe((Part)part), dependencies.Problem((Part)part).c_str());
    }
    if (dependencies.Usable(Part::Frame) != dependencies.Usable(Part::Events)) {
        Log(LogLevel::Error, "overlay: nothing will be drawn: it needs both the frame and the events");
    }
    return dependencies;
}

}

void SetGameResolution(engine::Resolution const* resolution)
{
    gResolution.store(resolution, std::memory_order_release);
}

Dependencies const& GameDependencies()
{
    static const Dependencies dependencies = CheckRunningGame();
    return dependencies;
}

}
