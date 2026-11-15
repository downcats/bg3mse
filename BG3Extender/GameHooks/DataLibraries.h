#pragma once

#include <CoreLib/SymbolMapper.h>
#include <CoreLib/Wrappers.h>
#include <optional>
#if !defined(_MSC_VER)
#include <Mac/Binding/GameVersion.h>
#endif

namespace bg3se {

    struct GameVersionInfo
    {
#if defined(_MSC_VER)
        uint16_t Major{ 0 }, Minor{ 0 }, Revision{ 0 }, Build{ 0 };
#else
        // The macOS game's version is its bundle's version string, whose
        // last part is a build number that does not fit 16 bits.
        uint32_t Major{ 0 }, Minor{ 0 }, Revision{ 0 }, Build{ 0 };
#endif

        inline bool IsSupported() const
        {
#if defined(_MSC_VER)
            // We need v4.69 or later for game
            return (Major == 4 && ((Minor == 69 && Revision >= 95) || (Minor > 69)));
#else
            // The macOS game numbers its versions otherwise, and no number
            // says what the extender needs of a game: it is supported when
            // all of that was found and checked in the game itself.
            return mac::binding::GameSupported();
#endif
        }
    };

    class LibraryManager
    {
    public:
        LibraryManager();
        bool FindLibraries(uint32_t gameRevision);
        bool PostStartupFindLibraries();
        void ApplyCodePatches();
        bool GetGameVersion(GameVersionInfo & version);
        HMODULE GetAppHandle();

        void ShowStartupError(STDString const & msg, bool wait, bool exitGame);
        void ShowStartupError(STDString const & msg, bool exitGame);

        bool WasPatchApplied(std::string const& mapping);
        bool ApplyCodePatch(std::string const& mapping);
        bool UndoCodePatch(std::string const& mapping);

        inline bool CriticalInitializationFailed() const
        {
            return CriticalInitFailed;
        }

        inline bool InitializationFailed() const
        {
            return InitFailed;
        }

        inline bool IsDX11() const
        {
            return DX11;
        }

        inline SymbolMapper& Mapper()
        {
            return symbolMapper_;
        }

    private:
        void PreRegisterLibraries(SymbolMappingLoader& loader);
        void RegisterLibraries(SymbolMapper& mapper);
        void RegisterSymbols();
        bool BindApp();
        SymbolMapper::MappingResult BindECSContext(uint8_t const*);
        SymbolMapper::MappingResult BindECSIndex(uint8_t const*);
        SymbolMapper::MappingResult BindECSStaticStringConstructor(uint8_t const*);
        SymbolMapper::MappingResult BindECSStaticRegistrant(uint8_t const*);
        SymbolMapper::MappingResult BindComponentReplicationIDRef(uint8_t const*);

        bool CanShowError();
        bool CanShowMessages();

        bool ApplyPatch(SymbolMappings::Patch& patch);
        bool UndoPatch(SymbolMappings::Patch& patch);

        SymbolMappings mappings_;
        SymbolMapper symbolMapper_;
        SymbolMapper::ModuleInfo appModule_;

        bool InitFailed{ false };
        bool CriticalInitFailed{ false };
        bool PostLoaded{ false };
        bool EnabledCustomStats{ false };
        bool DX11{ false };
    };
}
