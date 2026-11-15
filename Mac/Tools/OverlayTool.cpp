// bg3mse-overlay: what the ImGui overlay depends on in a game (Overlay/Dependencies.h).
//
//   bg3mse-overlay check <Game.app>    every dependency of the overlay's eight parts: how it was checked and the
//                                      outcome, then the values taken from the game's code
//
// It reads the game executable from disk; the game is not started. It exits
// non-zero when a part of the overlay would be off in this game.

#include <Engine/EngineResolver.h>
#include <Overlay/Dependencies.h>
#include <Platform/FunctionExtents.h>
#include <Platform/Log.h>
#include <Platform/Symbols.h>

#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace bg3se::mac;
using namespace bg3se::mac::overlay;

namespace {

// A binary mapped read-only for the rest of the process.
std::span<uint8_t const> MapFile(std::string const& path)
{
    int fd = open(path.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0) close(fd);
        fprintf(stderr, "cannot open %s\n", path.c_str());
        return {};
    }
    void* data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        fprintf(stderr, "cannot map %s\n", path.c_str());
        return {};
    }
    return { static_cast<uint8_t const*>(data), (size_t)st.st_size };
}

int Check(std::string const& app)
{
    std::string path = app + "/Contents/MacOS/Baldur's Gate 3";
    auto file = MapFile(path);
    if (file.empty()) return 1;
    GameImage image;
    if (!image.OpenFile(file)) {
        fprintf(stderr, "%s: no readable arm64 symbol table\n", path.c_str());
        return 1;
    }
    FunctionTable functions;
    std::string problem;
    if (!functions.OpenFile(file, problem)) {
        fprintf(stderr, "%s: %s\n", path.c_str(), problem.c_str());
        return 1;
    }

    engine::Images images;
    images.Game = { &image, &functions };
    auto resolution = engine::Resolve(DependencyTable(), images);
    ObjcClassesOfFile classes(image);
    auto dependencies = CheckDependencies(resolution, classes);

    bool allUsable = true;
    for (size_t n = 0; n < (size_t)Part::Count; n++) {
        Part part = (Part)n;
        bool usable = dependencies.Usable(part);
        allUsable = allUsable && usable;
        printf("%s: %s\n", Describe(part), usable ? "every check passed" : "OFF");
        for (auto const& check : dependencies.Checks) {
            if (check.Of == part) printf("    %s %-48s %s\n", check.Passed ? "ok  " : "FAIL", check.What.c_str(), check.Detail.c_str());
        }
        printf("\n");
    }

    printf("taken from the game's code:\n");
    printf("    rf::metal::MetalRenderFrame   drawable +0x%x, renderer +0x%x\n", dependencies.FrameDrawable, dependencies.FrameRenderer);
    printf("    rf::metal::MetalRenderer      command buffer +0x%x\n", dependencies.RendererCommandBuffer);
    printf("    key codes                     %u\n", dependencies.KeyCodes);
    printf("    ls::ResourceManager           texture manager +0x%x\n", dependencies.TextureManager);
    printf("    texture resource              source file +0x%x, GUID +0x%x, load state +0x%x, Streaming +0x%x, SRGB +0x%x, type +0x%x\n",
        dependencies.ResourceSourceFile, dependencies.ResourceGuid, dependencies.ResourceState, dependencies.ResourceStreaming,
        dependencies.ResourceSRGB, dependencies.ResourceType);
    printf("    rf::Texture                   MTLTexture +0x%x\n", dependencies.MetalTexture);
    printf("    rf::GraphicSettings           HDRPaperWhite +0x%x\n", dependencies.PaperWhite);
    printf("    the game's UI white in HDR    %g nits\n", dependencies.UiWhite);

    printf("\n%s\n", dependencies.Drawable() ? "the overlay would draw" : "THE OVERLAY WOULD NOT DRAW");
    if (!allUsable) printf("A PART OF THE OVERLAY WOULD BE OFF\n");
    return allUsable ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    SetLogStream(stdout);
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 2 && args[0] == "check") return Check(args[1]);
    fprintf(stderr, "usage: bg3mse-overlay check <Game.app>\n");
    return 2;
}
