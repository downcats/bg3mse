// Lua's configuration, where the one Norbyte's Windows build gets differs
// from the one this system would get, for what a script can see. Included in
// front of every source of bg3se_lua (CMakeLists.txt), so that Lua's own
// files stay as they are.
//
// On Windows luaconf.h sees _WIN32 and defines LUA_USE_WINDOWS, and with it
// LUA_USE_C89 ("broadly, Windows is C89"). Defining LUA_USE_C89 for this
// build as a whole would not give the same Lua: without LUA_USE_WINDOWS it
// also makes lua_Integer a long (LUA_C89_NUMBERS), and Norbyte's code is
// compiled against a lua_Integer that is a long long. So luaconf.h is read
// first, as this system's (POSIX), and what LUA_USE_C89 changes for scripts
// is then set as Windows has it:
//
// - a hexadecimal float in source text or given to tonumber() is read by
//   Lua's own lua_strx2number (lobject.c), not by strtod(). The two round
//   differently: Lua's adds the digits up one at a time in a double, so a
//   numeral of more than 53 bits is rounded more than once
//   (0x1.000000000000081p0 is 1.0 there, and the next double up with strtod);
// - string.format("%a") and the floats of "%q" are written by Lua's own
//   lua_number2strx (lstrlib.c), not by printf. A width, precision or flag
//   with "%a" is an error there, where printf takes it ("%.3a" of 1 is
//   0x1.000p+0). For the numbers tried the digits are the same in both,
//   subnormal ones included, but only Lua's are the same code on both systems;
// - math.log(x, 2) is log(x) / log(2), not log2(x) (lmathlib.c): the quotient
//   is not always the nearest double (math.log(2^29, 2) is 29.000000000000004).
//
// LUA_USE_C89 is defined below, after luaconf.h has made its choices. What
// still reads it are the base-2 logarithm above and the list of conversions
// os.date accepts (loslib.c), of a library the extender does not open.
//
// What LUA_USE_C89 changes without a script noticing stays as this system has
// it: snprintf with the buffer's size instead of sprintf.
//
// Two things here are the C library's and not Lua's configuration:
//
// - "%p", which Lua uses for the address in tostring() of a table, function,
//   thread or userdata. MSVC's library writes the 16 hexadecimal digits of
//   the pointer, in capitals and without a prefix (table: 000001E5A0B0C0D0);
//   this system's writes 0x and the digits without leading zeros.
// - the generator of math.random and math.randomseed (lmathlib.c, l_rand):
//   rand() and srand() on Windows, MSVC's generator with 15 bits a call and a
//   state for each thread (Mac/Lua/MsvcRand.h); random() and srandom() with
//   31 bits under this system's configuration. The same seed gives the same
//   numbers as on Windows. In the extender's own Lua states SandboxStartup.lua
//   puts Ext.Math.Random in place of math.random and makes math.randomseed an
//   error, so what this changes there is what runs before that file.
//
// string.pack's "l" and "L" (a C long: 4 bytes for MSVC, 8 here) are beyond
// any macro; CMakeLists.txt compiles lstrlib.c from a copy with that size.
//
// The locale. Norbyte's extender is linked with MSVC's static C library
// (RuntimeLibrary MultiThreaded), whose locale is its own: the game's calls
// of setlocale reach the game's C library, never the extender's, and his
// Lua always runs in MSVC's "C" locale. The macOS game shares one C library
// with the extender, and App::LoadLanguage calls setlocale(LC_ALL, ""): the
// locale of the game's environment (LANG, LC_*), for the whole process. With
// a German or Russian one tostring(0.5) would be "0,5"; with any UTF-8 one,
// English included, string.upper would change the bytes of "é", "%a" would
// match them, and "a" < "B" would be true. So every conversion and test of
// Lua that consults the locale is given the C locale (LC_C_LOCALE, the "C"
// of this system's library: ASCII only, '.' for the decimal point, strcmp's
// order), and what scripts see is the same in any locale:
//
// - numbers written (tostring, .., string.format) and read (tonumber, source
//   text), and the decimal point Lua tries as a second chance;
// - string comparison (<, <=, table.sort), which is strcoll;
// - string.upper and string.lower, the classes of patterns (%a, %l, %w, ...),
//   and tonumber with a base, which use <ctype.h>.
#pragma once

// As every source of Lua begins: lprefix.h before any other header.
#include "lprefix.h"
#include "luaconf.h"

#if defined(LUA_USE_C89) || defined(LUA_USE_WINDOWS) || !defined(LUA_USE_POSIX)
#error "bg3se_lua is configured as this system's Lua (LUA_USE_POSIX); what Windows has differently is set here"
#endif
#if !defined(lua_strx2number) || !defined(lua_number2strx) || !defined(lua_pointer2str)
#error "luaconf.h is not the one this was written for: it defines the conversions that Windows leaves to Lua"
#endif
#if defined(l_rand) || defined(l_srand) || defined(L_RANDMAX)
#error "luaconf.h is not the one this was written for: it chooses math.random's generator"
#endif

#include "MsvcRand.h"

#define l_rand()    bg3se::mac::msvc::Rand()
#define l_srand(x)  bg3se::mac::msvc::SeedRand(x)
#define L_RANDMAX   bg3se::mac::msvc::kRandMax

// Lua's own implementations, as when luaconf.h does not define these.
#undef lua_strx2number
#undef lua_number2strx

#undef lua_pointer2str
#define lua_pointer2str(buff,sz,p)  l_sprintf(buff, sz, "%016llX", (unsigned long long)(size_t)(p))

#define LUA_USE_C89

// The C locale, whatever the process's is (see the top of this file).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xlocale.h>

#if !defined(lua_getlocaledecpoint) || !defined(l_sprintf) || !defined(lua_str2number)
#error "luaconf.h is not the one this was written for: it defines the locale's conversions differently"
#endif

#undef lua_getlocaledecpoint
#define lua_getlocaledecpoint()     '.'
#undef l_sprintf
#define l_sprintf(s,sz,f,i)         snprintf_l(s, sz, LC_C_LOCALE, f, i)
#undef lua_str2number
#define lua_str2number(s,p)         ((lua_Number)strtod_l((s), (p), LC_C_LOCALE))

#define strcoll(a,b)                strcoll_l((a), (b), LC_C_LOCALE)
#define toupper(c)                  toupper_l((c), LC_C_LOCALE)
#define tolower(c)                  tolower_l((c), LC_C_LOCALE)
#define isalpha(c)                  isalpha_l((c), LC_C_LOCALE)
#define isalnum(c)                  isalnum_l((c), LC_C_LOCALE)
#define iscntrl(c)                  iscntrl_l((c), LC_C_LOCALE)
#define isdigit(c)                  isdigit_l((c), LC_C_LOCALE)
#define isgraph(c)                  isgraph_l((c), LC_C_LOCALE)
#define islower(c)                  islower_l((c), LC_C_LOCALE)
#define ispunct(c)                  ispunct_l((c), LC_C_LOCALE)
#define isspace(c)                  isspace_l((c), LC_C_LOCALE)
#define isupper(c)                  isupper_l((c), LC_C_LOCALE)
#define isxdigit(c)                 isxdigit_l((c), LC_C_LOCALE)
