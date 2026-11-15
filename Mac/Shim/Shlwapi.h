// Shlwapi.h (also reached as <shlwapi.h>: the volume this builds on is case-insensitive).
#pragma once

#include <windows.h>

// Implemented in Win32Files.cpp. Paths as in windows.h.
BOOL PathFileExistsW(LPCWSTR path);
BOOL PathIsDirectoryW(LPCWSTR path);
