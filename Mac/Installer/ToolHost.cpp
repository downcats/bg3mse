#include <Installer/ToolHost.h>

#include <Engine/EngineResolver.h>
#include <Engine/EngineTable.h>
#include <Platform/Config.h>
#include <Platform/FunctionExtents.h>
#include <Platform/Symbols.h>
#include <Shaders/GameInterface.h>
#include <Shaders/ShaderProvision.h>

namespace bg3se::mac::installer {

bool SupportsGame(std::span<uint8_t const> executable, std::span<uint8_t const> osiris, std::string& why)
{
    struct Binary
    {
        GameImage Symbols;
        FunctionTable Functions;
    } game, library;
    std::string problem;
    if (!game.Symbols.OpenFile(executable) || !game.Functions.OpenFile(executable, problem)) {
        why = "the game's executable has no readable arm64 symbol table" + (problem.empty() ? std::string() : ": " + problem);
        return false;
    }
    if (!library.Symbols.OpenFile(osiris) || !library.Functions.OpenFile(osiris, problem)) {
        why = "libOsiris.dylib has no readable arm64 symbol table" + (problem.empty() ? std::string() : ": " + problem);
        return false;
    }
    auto resolution = engine::Resolve(engine::EngineTable(),
        engine::Images{ { &game.Symbols, &game.Functions }, { &library.Symbols, &library.Functions } });
    size_t unexpected = 0;
    std::string first;
    for (auto const& r : resolution.Entries) {
        if (r.AsExpected()) continue;
        if (unexpected++ < 5) {
            first += "\n  " + std::string(r.Of->Name) + ": " + engine::Describe(r.Is) + ", expected " + engine::Describe(r.Expected()) +
                (r.Reason.empty() ? "" : ": " + r.Reason);
        }
    }
    if (!unexpected) return true;
    why = std::to_string(unexpected) + " of the " + std::to_string(resolution.Entries.size()) +
        " entries of the engine table are not as the table expects on it (bg3mse-engine check lists them all):" + first;
    return false;
}

Host ToolHost()
{
    Host host;
    host.AppleSilicon = IsAppleSilicon();
    host.Supports = SupportsGame;
    host.DataDirectory = DataDirectory();
    host.RunningGames = [data = host.DataDirectory] { return RunningGames(data); };
    // Nothing to read: what is kept is this game's, or the install has nothing to translate.
    host.ShaderInterfaceKept = [](std::string const& app, std::string const& file) {
        std::string why;
        return !MacInterfaceToBeRead(app, file) ||
            !TranslationHasWork(InstallPaths::ForGame(app), TranslationSettingOf(ConfValue(kShaderTranslationSetting)), why);
    };
    host.ReadShaderInterface = [](std::string const& app, std::string const& file, std::string& problem) {
        return EnsureMacInterface(app, file, problem).Ok();
    };
    host.MacOSVersion = MacOSVersion();
    if (!host.DataDirectory.empty()) {
        // Where Steam puts the game in its own folder; Steam's settings are not read.
        std::string support = host.DataDirectory.substr(0, host.DataDirectory.rfind('/'));
        host.DefaultGame = support + "/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app";
    }
    return host;
}

}
