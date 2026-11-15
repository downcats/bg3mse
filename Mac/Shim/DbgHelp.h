// DbgHelp.h, which CoreLib/SymbolMapper.cpp and GameHooks/DataLibraries.cpp
// include. What they take from it, ImageNtHeader and the PE image headers,
// only their MSVC branches use: a Mach-O image has no PE headers, and the
// code section is found otherwise (Mac/Shim/Win32Images.h).
#pragma once

#include <windows.h>
