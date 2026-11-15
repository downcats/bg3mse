#include "stdafx.h"
#if defined(_MSC_VER)
#include <Extender/Shared/DWriteWrapper.h>
#endif
#include <Extender/Shared/ExtenderConfig.h>
#include <Extender/ScriptExtender.h>
#include <CoreLib/JsonLibs.h>
#include <sstream>
#include <fstream>
#include <Extender/Shared/ExtenderConfig.inl>
#if !defined(_MSC_VER)
#include <Mac/Platform/Config.h>
#include <Mac/Platform/Process.h>
#endif

#if defined(_MSC_VER)
using namespace bg3se;
#else
// A using-directive for bg3se at namespace scope would make "std" name two
// namespaces (Mac/Layout/StdVariant.h): the code below is in bg3se instead.
namespace bg3se {
#endif

void SetupScriptExtender(HMODULE hModule)
{
    gExtender = std::make_unique<ScriptExtender>();
    auto & config = gExtender->GetConfig();
#if defined(_MSC_VER)
    LoadConfig(L"ScriptExtenderSettings.json", config);
#else
    // On Windows the file is beside the game's executable, which is the
    // working directory there. The macOS game is started with a working
    // directory that is nobody's place for a file (the root directory, from
    // the Finder or Steam): the file is looked for where bg3mse keeps its
    // own settings, ~/Library/Application Support/bg3mse.
    LoadConfig(FromStdUTF8(mac::DataDirectory() + "/ScriptExtenderSettings.json"), config);
#endif

    // Enable console if shift is down when the game is launched
    if (GetAsyncKeyState(VK_SHIFT) < 0) {
        config.CreateConsole = true;
    }

    DisableThreadLibraryCalls(hModule);
    if (config.CreateConsole) {
        gCoreLibPlatformInterface.GlobalConsole->Create();
    }

    if (config.DebugFlags == 0) {
        // Disabled: DF_FunctionList, DF_NodeList ,DF_LogSuccessfulFacts, DF_LogFailedFacts, DB_LogFactFailures, DF_DumpDatabases, DF_DebugFacts, DF_LogRuleFailures
        config.DebugFlags = DF_DebugTrace | DF_SuppressInitLog;
    }

    gExtender->Initialize();

#if 0
    DEBUG(" ***** ScriptExtender setup completed ***** ");
#endif
}

HANDLE ExtenderMutex{ NULL };

bool ShouldInitializeExtender()
{
#if defined(_MSC_VER)
    if (GetModuleHandleW(L"bg3.exe") == NULL && GetModuleHandleW(L"bg3_dx11.exe") == NULL) {
        return false;
    }
#else
    // The macOS game is one executable, known by its name (Mac/Platform/Process.h).
    if (!mac::IsGameProcess()) {
        return false;
    }
#endif

    if (ExtenderMutex == NULL) {
        char mutexName[MAX_PATH];
        sprintf_s(mutexName, "BG3SE_Ext_%d", GetCurrentProcessId());
        ExtenderMutex = CreateMutexA(NULL, TRUE, mutexName);
    }

    return ExtenderMutex != NULL;
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        bg3se::gCoreLibPlatformInterface.ThisModule = hModule;
#if defined(_MSC_VER)
        // The extender is loaded in place of DWrite.dll on Windows only.
        gDWriteWrapper = std::make_unique<DWriteWrapper>();
#endif
        if (ShouldInitializeExtender()) {
            SetupScriptExtender(hModule);
        }
        break;

    case DLL_PROCESS_DETACH:
        if (gExtender) {
            gExtender->Shutdown();
            gExtender.reset();
        }
#if defined(_MSC_VER)
        gDWriteWrapper.reset();
#endif
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}

#if !defined(_MSC_VER)
}
#endif
