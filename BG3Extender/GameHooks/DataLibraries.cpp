#include "stdafx.h"
#include <GameHooks/DataLibraries.h>
#include <Extender/Shared/ExtensionState.h>
#include <Extender/ScriptExtender.h>
#include <GameDefinitions/Symbols.h>
#include <string>
#include <functional>
#include <chrono>
#include <psapi.h>
#include <DbgHelp.h>
#include "resource.h"
#if !defined(_MSC_VER)
#include <Mac/Binding/Libraries.h>
#endif

namespace bg3se
{
    _Post_writable_byte_size_(size) void* BG3Alloc(std::size_t size)
    {
        auto const& sym = GetStaticSymbols();
        // Fallback in case allocator is not available
        if (sym.ls__GlobalAllocator__Alloc && sym.ls__GlobalAllocator__Free && sym.ls__GlobalAllocator__Get) {
            return sym.ls__GlobalAllocator__Alloc(sym.ls__GlobalAllocator__Get(), size, 2, 0, 8);
        } else {
            return malloc(size);
        }
    }

    void BG3Free(_Post_ptr_invalid_ _Pre_valid_ void* ptr) noexcept
    {
        auto const& sym = GetStaticSymbols();
        if (sym.ls__GlobalAllocator__Alloc && sym.ls__GlobalAllocator__Free && sym.ls__GlobalAllocator__Get) {
            sym.ls__GlobalAllocator__Free(sym.ls__GlobalAllocator__Get(), ptr);
        } else {
            return free(ptr);
        }
    }

    LibraryManager::LibraryManager()
        : symbolMapper_(mappings_)
    {}

    bool LibraryManager::FindLibraries(uint32_t gameRevision)
    {
        RegisterSymbols();

#if defined(_MSC_VER)
        SymbolMappingLoader loader(mappings_);
        PreRegisterLibraries(loader);
        if (!loader.LoadBuiltinMappings(IDR_BINARY_MAPPINGS)) {
            ERR("Failed to load symbol mapping table");
            CriticalInitFailed = true;
        }

#if defined(_DEBUG)
        if (!CriticalInitFailed) {
            // Debug check for symbol xml -> engine bindings
            for (auto const& sym : mappings_.StaticSymbols) {
                if (!sym.second.Bound) {
                    WARN("No mapping exists for engine symbol: %s", sym.first.c_str());
                }
            }
        }
#endif

        if (!BindApp()) {
            ERR("LibraryManager::FindLibraries(): Unable to bind BG3 module.");
            return false;
        }

        RegisterLibraries(symbolMapper_);
        symbolMapper_.MapAllSymbols(false);

        CriticalInitFailed = CriticalInitFailed || symbolMapper_.HasFailedCriticalMappings();
        InitFailed = InitFailed || symbolMapper_.HasFailedMappings();
#else
        // The macOS game is not searched for byte patterns: the members are
        // bound from the engine table (Mac/Binding/StaticSymbolBinding.h).
        auto bound = mac::binding::BindLibraries(&GetStaticSymbols());
        CriticalInitFailed = CriticalInitFailed || bound.CriticalFailed;
        InitFailed = InitFailed || bound.Failed;
#endif

        if (gExtender->GetConfig().DisableLauncher && !CriticalInitFailed) {
            ApplyCodePatch("KillLauncher");
        }

        auto const& sym = GetStaticSymbols();
        gCoreLibPlatformInterface.Alloc = &BG3Alloc;
        gCoreLibPlatformInterface.Free = &BG3Free;
        gCoreLibPlatformInterface.ls__GlobalStringTable__MainTable__CreateFromString = sym.ls__GlobalStringTable__MainTable__CreateFromString;
        gCoreLibPlatformInterface.ls__FixedString__GetString = sym.ls__FixedString__GetString;
        gCoreLibPlatformInterface.ls__FixedString__IncRef = sym.ls__FixedString__IncRef;
        gCoreLibPlatformInterface.ls__GlobalStringTable__MainTable__DecRef = sym.ls__GlobalStringTable__MainTable__DecRef;
        gCoreLibPlatformInterface.ls__gGlobalStringTable = sym.ls__gGlobalStringTable;

        return !CriticalInitFailed;
    }

    bool LibraryManager::PostStartupFindLibraries()
    {
        if (PostLoaded) {
            return !CriticalInitFailed;
        }

        auto initStart = std::chrono::high_resolution_clock::now();

        symbolMapper_.MapAllSymbols(true);
#if !defined(_MSC_VER)
        // The macOS game gives its types their indices in CoreLibSDM::Init:
        // the type index maps are filled once that has run
        // (Mac/Binding/TypeIndexBinding.h).
        mac::binding::BindTypeIndices(&GetStaticSymbols());
#endif

        if (!CriticalInitFailed) {
            GFS.Initialize();
            InitializeEnumerations();
        }

        auto initEnd = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(initEnd - initStart).count();
        DEBUG("LibraryManager::PostStartupFindLibraries() took %lld ms", ms);

        PostLoaded = true;
        return !CriticalInitFailed;
    }

    void LibraryManager::ApplyCodePatches()
    {
        if (gExtender->GetConfig().EnableAchievements && !WasPatchApplied("ls::ModuleSettings::IsModded")) {
            if (ApplyCodePatch("ls::ModuleSettings::IsModded") && ApplyCodePatch("esv::SavegameManager::ThrowError")) {
                DEBUG("Modded achievements enabled.");
            } else {
                ERR("Couldn't enable modded achievements (symbol not mapped)");
            }
        }

        if (gExtender->GetConfig().DisableStoryPatching && GetStaticSymbols().ls__GlobalSwitches) {
            (*GetStaticSymbols().ls__GlobalSwitches)->DisableStoryPatching = true;
        }
    }

    // Outside MSVC a patch is not bytes of a mapping but instructions of the
    // macOS game, found and written by Mac/Binding (CodePatches.h): the three
    // functions below hand over to it, and the two that write bytes are left out.
    bool LibraryManager::WasPatchApplied(std::string const& mapping)
    {
#if !defined(_MSC_VER)
        return mac::binding::WasCodePatchApplied(mapping);
#else
        auto it = mappings_.Mappings.find(mapping);
        if (it == mappings_.Mappings.end()) {
            ERR("Cannot apply patch - no such mapping: '%s'", mapping.c_str());
            return false;
        }

        bool applied = false;
        for (auto& patch : it->second.Patches) {
            applied = patch.WasApplied || applied;
        }

        return applied;
#endif
    }

    bool LibraryManager::ApplyCodePatch(std::string const& mapping)
    {
#if !defined(_MSC_VER)
        return mac::binding::ApplyCodePatch(mapping);
#else
        auto it = mappings_.Mappings.find(mapping);
        if (it == mappings_.Mappings.end()) {
            ERR("Cannot apply patch - no such mapping: '%s'", mapping.c_str());
            return false;
        }

        if (it->second.Patches.empty()) {
            ERR("Cannot apply patch - mapping '%s' contains no patch entries", mapping.c_str());
            return false;
        }

        bool hasChanges = false;
        for (auto& patch : it->second.Patches) {
            hasChanges = ApplyPatch(patch) || hasChanges;
        }

        return hasChanges;
#endif
    }


    bool LibraryManager::UndoCodePatch(std::string const& mapping)
    {
#if !defined(_MSC_VER)
        return mac::binding::UndoCodePatch(mapping);
#else
        auto it = mappings_.Mappings.find(mapping);
        if (it == mappings_.Mappings.end()) {
            ERR("Cannot undo patch - no such mapping: '%s'", mapping.c_str());
            return false;
        }

        bool hasChanges = false;
        for (auto& patch : it->second.Patches) {
            hasChanges = UndoPatch(patch) || hasChanges;
        }

        return hasChanges;
#endif
    }


#if defined(_MSC_VER)
    bool LibraryManager::ApplyPatch(SymbolMappings::Patch& patch)
    {
        if (patch.WasApplied) {
            return true;
        }
        
        if (patch.ResolvedRef == nullptr) {
            return false;
        }

        WriteAnchor code(patch.ResolvedRef, patch.Bytes.size());
        patch.OriginalBytes.resize(patch.Bytes.size());
        memcpy(patch.OriginalBytes.data(), patch.ResolvedRef, patch.OriginalBytes.size());
        memcpy(const_cast<uint8_t*>(patch.ResolvedRef), patch.Bytes.data(), patch.Bytes.size());
        patch.WasApplied = true;
        return true;
    }


    bool LibraryManager::UndoPatch(SymbolMappings::Patch& patch)
    {
        if (!patch.WasApplied) return false;

        WriteAnchor code(patch.ResolvedRef, patch.OriginalBytes.size());
        memcpy(const_cast<uint8_t*>(patch.ResolvedRef), patch.OriginalBytes.data(), patch.OriginalBytes.size());
        patch.WasApplied = false;
        return true;
    }
#endif

    bool LibraryManager::GetGameVersion(GameVersionInfo & version)
    {
#if !defined(_MSC_VER)
        // The macOS game has no version resource: its version is its
        // bundle's version string (Mac/Binding/GameVersion.h).
        auto found = mac::binding::ReadGameVersion();
        if (!found) return false;
        version.Major = found->Major;
        version.Minor = found->Minor;
        version.Revision = found->Revision;
        version.Build = found->Build;
        return true;
#else
        HMODULE hGameModule = GetModuleHandleW(L"bg3.exe");
        if (hGameModule == NULL) {
            hGameModule = GetModuleHandleW(L"bg3_dx11.exe");
        }

        if (hGameModule == NULL) {
            return false;
        }

        auto hResource = FindResource(hGameModule, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
        if (hResource == NULL) return false;
        auto dwSize = SizeofResource(hGameModule, hResource);
        auto hData = LoadResource(hGameModule, hResource);
        if (hData == NULL) return false;
        auto pRes = LockResource(hData);
        if (pRes == NULL) return false;

        auto pResCopy = LocalAlloc(LMEM_FIXED, dwSize);
        CopyMemory(pResCopy, pRes, dwSize);

        UINT verLength;
        VS_FIXEDFILEINFO * fixedFileInfo;
        if (VerQueryValue(pResCopy, L"\\", (LPVOID*)&fixedFileInfo, &verLength) != TRUE) return false;

        version.Major = HIWORD(fixedFileInfo->dwFileVersionMS);
        version.Minor = LOWORD(fixedFileInfo->dwFileVersionMS);
        version.Revision = HIWORD(fixedFileInfo->dwFileVersionLS);
        version.Build = LOWORD(fixedFileInfo->dwFileVersionLS);

        LocalFree(pResCopy);
        FreeResource(hData);
        return true;
#endif
    }

    void LibraryManager::ShowStartupError(STDString const & msg, bool wait, bool exitGame)
    {
        ERR("STARTUP ERROR: %s", msg.c_str());

        if (GetStaticSymbols().ecl__EoCClient == nullptr
            || GetStaticSymbols().ecl__EoCClient__HandleError == nullptr
            || GetStaticSymbols().ls__GlobalAllocator__Alloc == nullptr) {
            return;
        }

        if (wait) {
            std::thread messageThread([this, msg, exitGame]() {
                unsigned retries{ 0 };
                while (!CanShowError() && retries < 1200) {
                    Sleep(100);
                    retries++;
                }

                if (retries < 1200) {
                    ShowStartupError(msg, exitGame);
                } else {
                    MessageBoxA(NULL, msg.c_str(), "Script Extender Error", 
                        MB_OK | MB_ICONERROR | MB_TASKMODAL | MB_SETFOREGROUND);
                }
            });
            messageThread.detach();
        } else {
            if (CanShowError()) {
                ShowStartupError(msg, exitGame);
            }
        }
    }

    void LibraryManager::ShowStartupError(STDString const & msg, bool exitGame)
    {
        if (!CanShowMessages()) return;

        // Abuse a deprecated string key for displaying custom text
        TranslatedString ts;
        ts.Handle.Handle = FixedString("h11018635g3003g46c6g8013g4630abe55cad");
        ts.Handle.Version = 1;

        // Create a new entry in the string repository text pool
        auto& texts = (*GetStaticSymbols().ls__gTranslatedStringRepository)->TranslatedStrings[0];
        auto tskRef = texts->Texts.try_get(ts.Handle);
        if (!tskRef) return;

        auto str = GameAlloc<STDString>(msg);
        texts->Strings.push_back(str);

        // Update reference to new string
        *tskRef = LSStringView(str->data(), str->size());

        GetStaticSymbols().ecl__EoCClient__HandleError(*GetStaticSymbols().ecl__EoCClient, ts, exitGame, ts, 0);
    }

    bool LibraryManager::CanShowMessages()
    {
        return GetStaticSymbols().GetClientState()
            && GetStaticSymbols().ecl__EoCClient__HandleError != nullptr
            && GetStaticSymbols().ls__GlobalAllocator__Alloc != nullptr;
    }

    bool LibraryManager::CanShowError()
    {
        if (!CanShowMessages()) return false;

        auto state = GetStaticSymbols().GetClientState();
        return state == ecl::GameState::Running
            || state == ecl::GameState::Paused
            || state == ecl::GameState::Menu
            || state == ecl::GameState::Lobby;
    }
}
