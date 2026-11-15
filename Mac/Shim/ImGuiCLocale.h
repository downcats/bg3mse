// Included first in each of ImGui's sources (CMakeLists.txt, bg3se_imgui).
//
// ImGui writes numbers with the C library's vsnprintf, snprintf and sprintf
// and reads them with sscanf and atof, which follow the process's locale.
// The macOS game sets that locale from the system as it loads its language
// (App::LoadLanguage calls setlocale(LC_ALL, "")): on a German Mac a slider
// shows 1,500, and "1.5" typed into a field (ImGui's filter lets only '.'
// through) is read as 1. Norbyte's Windows build links the static C library,
// whose locale is always "C". Here the same functions are given the "C"
// locale.

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <xlocale.h>

#ifdef __cplusplus
#include <cstdio>
#include <cstdlib>
#endif

// The fortified forms of the system's headers are macros of these names.
#undef vsnprintf
#undef snprintf
#undef sprintf
#undef sscanf
#undef atof

#define vsnprintf(buffer, size, format, args) vsnprintf_l(buffer, size, LC_C_LOCALE, format, args)
#define snprintf(buffer, size, ...) snprintf_l(buffer, size, LC_C_LOCALE, __VA_ARGS__)
#define sprintf(buffer, ...) sprintf_l(buffer, LC_C_LOCALE, __VA_ARGS__)
#define sscanf(text, ...) sscanf_l(text, LC_C_LOCALE, __VA_ARGS__)
#define atof(text) strtod_l(text, NULL, LC_C_LOCALE)
