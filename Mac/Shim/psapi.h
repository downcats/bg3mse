// psapi.h: module information.
#pragma once

#include <windows.h>

typedef struct _MODULEINFO {
    LPVOID lpBaseOfDll;
    DWORD SizeOfImage;
    LPVOID EntryPoint;
} MODULEINFO, *LPMODULEINFO;

// Win32Modules.cpp. For the current process only (any other handle fails with
// ERROR_INVALID_HANDLE). The base is the image's header and the size reaches
// the end of its last segment; the entry point is the main executable's, and
// NULL for a dylib. A library in the dyld shared cache is not one range of
// memory: for it the call fails with ERROR_NOT_SUPPORTED.
BOOL GetModuleInformation(HANDLE process, HMODULE module, LPMODULEINFO info, DWORD size);
