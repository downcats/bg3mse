// What a script sees of Lua where the configuration Norbyte's Windows build
// gets differs from the one this system would get (Mac/Lua/WindowsConfiguration.h):
// each chunk is run through bg3se_lua, the library the extender links, and
// its result compared with what the Windows build gives.
//
// Where the Windows values come from: Lua's own conversions (lua_strx2number,
// lua_number2strx) are the same code on both systems and use only IEEE 754
// arithmetic, frexp, ldexp and floor, which are exact; the values below were
// worked out from that code. The others say at their place what they rest on.
// None was read off a Windows build.

// As Norbyte's code includes them (Lua/LuaHelpers.h): Lua is compiled as C++.
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

#include <cstdio>
#include <clocale>
#include <cstring>
#include <string>
#include <thread>

// What Norbyte's Lua asks of its host, which the extender provides in
// Lua/LuaBinding.cpp.
//
// It reports an error that a handler will take (ldebug.c, luaG_errormsg),
// for the Lua debugger. This host has none.
void nse_lua_report_handled_error(lua_State*)
{
}

namespace {

int gFailures = 0;
int gChecks = 0;

// It hands every string constant it compiles to the host, and every string it
// frees (lua_setup_strcache): the extender keeps the game's fixed string for
// each. This host keeps nothing, and creates none of the C++ objects the
// other hooks (lua_setup_cppobjects) are for.
void CacheNothing(lua_State*, TString*)
{
}

void ReleaseNothing(lua_State*, TString*)
{
}

// The libraries the extender opens, as it opens them (Lua/LuaBinding.cpp, State::OpenLibs).
const luaL_Reg kLoadedLibs[] = {
    { "_G", luaopen_base },
    { LUA_TABLIBNAME, luaopen_table },
    { LUA_STRLIBNAME, luaopen_string },
    { LUA_MATHLIBNAME, luaopen_math },
    { LUA_COLIBNAME, luaopen_coroutine },
    { LUA_DBLIBNAME, luaopen_debug },
};

// The first result of the chunk, as tostring() gives it; "error: ..." when it fails.
std::string Run(lua_State* L, char const* chunk)
{
    int top = lua_gettop(L);
    std::string result;
    if (luaL_loadstring(L, chunk) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
        result = std::string("error: ") + lua_tostring(L, -1);
    } else {
        size_t length = 0;
        char const* text = luaL_tolstring(L, -1, &length);
        result.assign(text, length);
    }
    lua_settop(L, top);
    return result;
}

struct Case
{
    char const* Chunk;
    char const* OnWindows;
};

void Check(lua_State* L, char const* what, Case const& c)
{
    gChecks++;
    auto result = Run(L, c.Chunk);
    if (result == c.OnWindows) return;
    printf("FAILED %s: %s\n    gives     %s\n    Windows:  %s\n", what, c.Chunk, result.c_str(), c.OnWindows);
    gFailures++;
}

template <size_t N>
void CheckAll(lua_State* L, char const* what, Case const (&cases)[N])
{
    for (auto const& c : cases) Check(L, what, c);
}

// --- hexadecimal floats, read ---------------------------------------------------
//
// Windows: Lua's lua_strx2number. It adds the digits up in a double, one at a
// time, and leaves out those after the thirtieth; strtod(), which this
// system's configuration uses, rounds the whole numeral once.
const Case kHexFloatsRead[] = {
    // 1 + 2^-53 + 2^-60. Digit by digit: 0x100000000000008 is half way between
    // two doubles and goes to the even one, 2^56, and the last digit is then
    // lost in 2^60 + 1. Rounded once, the numeral is above half way: 1 + 2^-52.
    { "return 0x1.000000000000081p0 == 1.0", "true" },
    { "return tonumber('0x1.000000000000081p0') == 1.0", "true" },
    { "return tonumber('  -0x1.000000000000081p0  ') == -1.0", "true" },
    { "return string.format('%.17g', 0x1.000000000000081p0)", "1" },
    // The same, with the last digit beyond the thirty that are read.
    { "return 0x1.0000000000000800000000000000001p0 == 1.0", "true" },
    // What both read alike.
    { "return 0x10p-4", "1.0" },
    { "return 0x.8p1", "1.0" },
    { "return 0xA.8p0", "10.5" },
    { "return 0X1P+4", "16.0" },
    { "return 0x1.8", "1.5" },
    { "return 0x1.0000000000001p0 > 1.0", "true" },
    { "return 0x1p-1074 > 0 and 0x1p-1075 == 0", "true" },
    { "return 0x1p1024 == math.huge", "true" },
    { "return tostring(tonumber('0x1p'))", "nil" },
    { "return tostring(tonumber('0x'))", "nil" },
    { "return tostring(tonumber('0x.p1'))", "nil" },
    { "return tostring(tonumber('0x1p+'))", "nil" },
    { "return tostring(tonumber('1e5x'))", "nil" },
    // A hexadecimal integer is not a float: it wraps around.
    { "return 0xffffffffffffffff", "-1" },
    { "return math.type(0x10)", "integer" },
};

// --- hexadecimal floats, written ------------------------------------------------
//
// Windows: Lua's lua_number2strx, for string.format's "%a" and "%A" and for
// the floats of "%q". printf's "%a", which this system's configuration uses,
// takes a width, a precision and flags; for the numbers below it writes the
// same digits.
const Case kHexFloatsWritten[] = {
    { "return string.format('%a', 1)", "0x1p+0" },
    { "return string.format('%a', 0.5)", "0x1p-1" },
    { "return string.format('%a', 3)", "0x1.8p+1" },
    { "return string.format('%a', 0.1)", "0x1.999999999999ap-4" },
    { "return string.format('%a', -2.5)", "-0x1.4p+1" },
    { "return string.format('%a', 2^53 - 1)", "0x1.fffffffffffffp+52" },
    { "return string.format('%a', 0.0)", "0x0p+0" },
    { "return string.format('%a', -0.0)", "-0x0p+0" },
    { "return string.format('%A', 0.1)", "0X1.999999999999AP-4" },
    { "return string.format('[%a|%a]', 1, 2)", "[0x1p+0|0x1p+1]" },
    // Subnormal: normalised like any other number.
    { "return string.format('%a', 5e-324)", "0x1p-1074" },
    { "return string.format('%a', 2.0^-1023)", "0x1p-1023" },
    { "return string.format('%a', 0x0.fffffffffffffp-1022)", "0x1.ffffffffffffep-1023" },
    // Nothing between the % and the a.
    { "return select(2, pcall(string.format, '%.3a', 1))", "modifiers for format '%a'/'%A' not implemented" },
    { "return select(2, pcall(string.format, '%20a', 1))", "modifiers for format '%a'/'%A' not implemented" },
    { "return select(2, pcall(string.format, '%+A', 1))", "modifiers for format '%a'/'%A' not implemented" },
    // An infinity is written as "%.14g" writes it: the C library's text, which
    // is "inf" in Microsoft's since Visual Studio 2015 as it is here.
    { "return string.format('%a', math.huge)", "inf" },
    { "return string.format('%a', -math.huge)", "-inf" },
    // "%q" writes a float in the same way, so that it reads back as the same number.
    { "return string.format('%q', 0.1)", "0x1.999999999999ap-4" },
    { "return string.format('%q', 5e-324)", "0x1p-1074" },
    { "return load('return ' .. string.format('%q', 5e-324))() == 5e-324", "true" },
    { "return string.format('%q', 7)", "7" },
};

// --- math.log with base 2 -------------------------------------------------------
//
// Windows: log(x) / log(2), as for any base but 10. This system's
// configuration would call log2(x). The quotient is not always the nearest
// double, and scripts see the difference: math.log(2^29, 2) == 29 is false.
//
// The values are the quotients of the correctly rounded logarithms, in IEEE
// 754 division. That Microsoft's log() returns the correctly rounded value for
// these arguments, as this system's does, is assumed, not checked on Windows.
const Case kLogBaseTwo[] = {
    { "return string.format('%.17g', math.log(2^29, 2))", "29.000000000000004" },
    { "return math.log(2^29, 2) == 29", "false" },
    { "return string.format('%.17g', math.log(2^31, 2))", "31.000000000000004" },
    { "return string.format('%.17g', math.log(3, 2))", "1.5849625007211563" },
    { "return string.format('%.17g', math.log(10, 2))", "3.3219280948873626" },
    { "return math.log(8, 2)", "3.0" },
    { "return math.log(1024, 2)", "10.0" },
    { "return math.log(1, 2)", "0.0" },
    // By definition, whatever log() returns.
    { "for i = 1, 4000 do local x = i * 0.37 if math.log(x, 2) ~= math.log(x) / math.log(2) then return x end end return 'the quotient'",
      "the quotient" },
    { "return math.log(2^29, 2.0) == math.log(2^29) / math.log(2)", "true" },
    // Base 10 is log10 on both, and no base the natural logarithm.
    { "return math.log(1000, 10)", "3.0" },
    { "return math.log(1)", "0.0" },
};

// --- addresses ------------------------------------------------------------------
//
// tostring() of a table, function or thread ends in the "%p" of its address.
// Microsoft's library writes sixteen hexadecimal digits in capitals with no
// prefix; this system's writes 0x and the significant digits in small letters.
// (The form is the one its documentation and sources show; it was not checked
// on Windows.)
const Case kAddresses[] = {
    { "local s = tostring({}) return #s == 23 and s:match('^table: [0-9A-F]+$') ~= nil", "true" },
    { "local s = tostring(function() end) return #s == 26 and s:match('^function: [0-9A-F]+$') ~= nil", "true" },
    { "local s = tostring(coroutine.create(function() end)) return #s == 24 and s:match('^thread: [0-9A-F]+$') ~= nil", "true" },
    { "local s = string.format('%s', {}) return #s == 23 and s:match('^table: [0-9A-F]+$') ~= nil", "true" },
    // The digits are the address: a number in base 16, and the same for the same table.
    { "local t = {} return math.type(tonumber(tostring(t):sub(8), 16)) == 'integer' and tostring(t) == tostring(t)", "true" },
    { "return tostring({}) ~= tostring({})", "true" },
};

// --- string.pack: a C long ------------------------------------------------------
//
// "l" and "L" are a long of the C compiler: 4 bytes for MSVC, 8 for this
// system's. Packed data is exchanged and stored, so it is 4 here too.
const Case kPackedLongs[] = {
    { "return string.packsize('l')", "4" },
    { "return string.packsize('L')", "4" },
    { "return #string.pack('l', 1)", "4" },
    { "return string.pack('<l', -2) == '\\xfe\\xff\\xff\\xff'", "true" },
    { "return string.pack('>L', 0x01020304) == '\\1\\2\\3\\4'", "true" },
    { "return string.unpack('<L', '\\1\\2\\3\\4')", "67305985" },
    { "return string.unpack('<l', '\\xff\\xff\\xff\\xff')", "-1" },
    { "return select(2, string.unpack('<l', '\\1\\0\\0\\0\\2\\0\\0\\0'))", "5" },
    // Aligned as a 4-byte value: one byte, three of padding, the long.
    { "return string.packsize('!bl')", "8" },
    // It no longer holds what does not fit 32 bits.
    { "return (pcall(string.pack, 'l', 2^31))", "false" },
    { "return (pcall(string.pack, 'l', 2^31 - 1))", "true" },
    { "return (pcall(string.pack, 'L', 2^32))", "false" },
    // The other sizes are the same for both compilers.
    { "return string.packsize('b') .. string.packsize('h') .. string.packsize('i') .. string.packsize('j') .. string.packsize('T') "
      ".. string.packsize('f') .. string.packsize('d') .. string.packsize('n')", "12488488" },
    { "return string.packsize('!bj')", "16" },
};

// --- math.random and math.randomseed --------------------------------------------
//
// Windows: MSVC's rand() and srand() (Mac/Lua/MsvcRand.h). math.randomseed(x)
// calls srand and discards one rand(); math.random() is rand() / 32768;
// math.random(m, n) is m plus the integer part of that times n - m + 1, so
// math.random(32768) - 1 is rand() itself.
//
// The values were worked out from the generator and lmathlib.c's arithmetic
// (all of it exact in a double) with a model written apart from the code
// here; the model gives the sequence published for MSVC's rand() from its
// start (41, 18467, 6334, 26500, 19169) and 38 after srand(0). None was read
// off a Windows build.
const Case kRandom[] = {
    // srand(1) is the start: after the discarded 41, the published sequence.
    { "math.randomseed(1) local t = {} for i = 1, 5 do t[i] = math.random(32768) - 1 end return table.concat(t, ' ')",
      "18467 6334 26500 19169 15724" },
    { "math.randomseed(0) local t = {} for i = 1, 5 do t[i] = math.random(32768) - 1 end return table.concat(t, ' ')",
      "7719 21238 2437 8855 11797" },
    { "math.randomseed(42) local t = {} for i = 1, 5 do t[i] = math.random(32768) - 1 end return table.concat(t, ' ')",
      "400 17869 30056 16083 12879" },
    { "math.randomseed(12345) local t = {} for i = 1, 5 do t[i] = math.random(32768) - 1 end return table.concat(t, ' ')",
      "19164 25795 22125 5828 23405" },
    // The seed is converted to an unsigned int: -1 and 2^32 - 1 are one seed.
    { "math.randomseed(4294967295) local t = {} for i = 1, 5 do t[i] = math.random(32768) - 1 end return table.concat(t, ' ')",
      "29739 3374 11141 31308 7870" },
    { "math.randomseed(-1) local t = {} for i = 1, 5 do t[i] = math.random(32768) - 1 end return table.concat(t, ' ')",
      "29739 3374 11141 31308 7870" },
    { "math.randomseed(1.0) return math.random(32768) - 1", "18467" },
    // A float: one of 32768 steps of 2^-15.
    { "math.randomseed(1) local t = {} for i = 1, 3 do t[i] = string.format('%.17g', math.random()) end return table.concat(t, ' ')",
      "0.563568115234375 0.19329833984375 0.8087158203125" },
    { "math.randomseed(42) local t = {} for i = 1, 3 do t[i] = string.format('%.17g', math.random()) end return table.concat(t, ' ')",
      "0.01220703125 0.545318603515625 0.917236328125" },
    { "math.randomseed(12345) local t = {} for i = 1, 3 do t[i] = string.format('%.17g', math.random()) end return table.concat(t, ' ')",
      "0.5848388671875 0.787200927734375 0.675201416015625" },
    { "math.randomseed(0) local x = math.random() return math.type(x) .. ' ' .. tostring(x * 32768 % 1 == 0)", "float true" },
    // Integers in a range.
    { "math.randomseed(1) local t = {} for i = 1, 8 do t[i] = math.random(1, 6) end for i = 9, 12 do t[i] = math.random(-10, 10) end "
      "for i = 13, 15 do t[i] = math.random(100) end return table.concat(t, ' ')",
      "4 2 5 4 3 3 6 5 5 -7 8 4 52 31 2" },
    { "math.randomseed(0) local t = {} for i = 1, 8 do t[i] = math.random(1, 6) end for i = 9, 12 do t[i] = math.random(-10, 10) end "
      "for i = 13, 15 do t[i] = math.random(100) end return table.concat(t, ' ')",
      "2 4 1 2 3 2 6 2 9 -7 8 -10 1 63 49" },
    { "math.randomseed(42) local t = {} for i = 1, 8 do t[i] = math.random(1, 6) end for i = 9, 12 do t[i] = math.random(-10, 10) end "
      "for i = 13, 15 do t[i] = math.random(100) end return table.concat(t, ' ')",
      "1 4 6 3 3 2 2 3 -9 10 9 -2 22 8 71" },
    { "math.randomseed(4294967295) local t = {} for i = 1, 8 do t[i] = math.random(1, 6) end for i = 9, 12 do t[i] = math.random(-10, 10) end "
      "for i = 13, 15 do t[i] = math.random(100) end return table.concat(t, ' ')",
      "6 1 3 6 2 1 1 5 -8 -7 7 -3 51 95 96" },
    { "math.randomseed(7) local x = math.random(3, 3) return math.type(x) .. ' ' .. x", "integer 3" },
    // A range wider than the generator's 15 bits gets 32768 values spread over it.
    { "math.randomseed(1) local x = math.random(0, (1 << 40) - 1) return (x % (1 << 25)) .. ' ' .. (x >> 25)", "0 18467" },
};

// The generator's state is each thread's. These run the same Lua state on
// another thread, one after the other, as nothing else uses it meanwhile.
std::string RunOnThread(lua_State* L, char const* chunk)
{
    std::string result;
    std::thread thread([&] { result = Run(L, chunk); });
    thread.join();
    return result;
}

void CheckRandomPerThread(lua_State* L)
{
    auto check = [](char const* what, std::string const& result, char const* expected) {
        gChecks++;
        if (result == expected) return;
        printf("FAILED math.random on threads, %s:\n    gives     %s\n    Windows:  %s\n", what, result.c_str(), expected);
        gFailures++;
    };

    // A thread that never seeded starts as after srand(1), whatever another
    // thread seeded: its first number is the 41 math.randomseed(1) discards.
    Run(L, "math.randomseed(42)");
    check("a new thread's first numbers", RunOnThread(L, "return (math.random(32768) - 1) .. ' ' .. (math.random(32768) - 1)"), "41 18467");

    // Seeding on one thread leaves another's sequence where it was.
    Run(L, "math.randomseed(1)");
    std::string first = Run(L, "return math.random(32768) - 1");
    std::string other = RunOnThread(L, "math.randomseed(12345) return math.random(32768) - 1");
    std::string second = Run(L, "return math.random(32768) - 1");
    check("this thread's sequence around another thread's seed", first + " " + other + " " + second, "18467 19164 6334");
}

// --- as before ------------------------------------------------------------------
//
// What the configuration header must not have changed.
const Case kUnchanged[] = {
    { "return math.type(1), math.type(1.0)", "integer" },
    { "return math.maxinteger", "9223372036854775807" },
    { "return math.mininteger", "-9223372036854775808" },
    { "return 1e15", "1e+15" },
    { "return 2^53", "9.007199254741e+15" },
    { "return 3 / 2", "1.5" },
    { "return 7 // 2", "3" },
    { "return 1 / 0", "inf" },
    { "return -1 / 0", "-inf" },
    { "return string.format('%5.2f|%d|%x|%g', 3.14159, 42, 255, 0.1)", " 3.14|42|ff|0.1" },
    { "return tostring(tonumber('1e1'))", "10.0" },
    { "return tostring(tonumber('inf')) .. tostring(tonumber('nan'))", "nilnil" },
    { "return (select(2, pcall(function() return 1 // 0 end)):gsub('^.-:%d+: ', ''))", "attempt to divide by zero" },
};

// --- the locale -----------------------------------------------------------------
//
// Windows: MSVC's "C" locale, whatever the game's (the extender's C library
// is its own, WindowsConfiguration.h). The macOS game sets the locale of its
// environment for the whole process; these give the same in each.
const Case kLocale[] = {
    { "return tostring(0.5) .. ' ' .. 1.25 .. ' ' .. 1e300 .. ' ' .. -0.0", "0.5 1.25 1e+300 -0.0" },
    { "return string.format('%.3f %g %e %5.1f %a', 1234.5, 0.25, 12345.678, -2.5, 1.0)", "1234.500 0.25 1.234568e+04  -2.5 0x1p+0" },
    { "return tonumber('1.5') + tonumber('  2.25  ') + 0.25", "4.0" },
    { "return tostring(tonumber('1,5'))", "nil" },
    { "return 3.75", "3.75" },
    { "return math.type(tonumber('0x1.8p1')) .. tonumber('0x1.8p1')", "float3.0" },
    { "return string.format('%q', 0.1)", "0x1.999999999999ap-4" },
    { "return ('\xC3\xA9t\xC3\xA9 ABC'):upper() == '\xC3\xA9T\xC3\xA9 ABC'", "true" },
    { "return ('\xC3\x89T\xC3\x89 abc'):lower() == '\xC3\x89t\xC3\x89 abc'", "true" },
    { "return ('I i'):upper() .. ('I i'):lower()", "I Ii i" },
    { "local n = 0 for _ in ('\xC3\xA9" "a\xD0\x96'):gmatch('%a') do n = n + 1 end return n", "1" },
    { "return #(('\xC3\xA9 \xE3\x81\x82 x'):gsub('[%w%p%s%c%l%u%x%g]', ''))", "5" },
    { "return tostring('a' < 'B') .. tostring('B' < 'a') .. tostring('\xC3\xA9' < 'z')", "falsetruefalse" },
    { "local t = { 'b', 'B', 'a', 'A', '\xC3\xA9', 'z' } table.sort(t) return table.concat(t, ',')", "A,B,a,b,z,\xC3\xA9" },
    { "return tonumber('zz', 36) + tonumber('Ff', 16)", "1550" },
};

// Each locale named is set for the whole process, as the game does, and the
// chunks above run again. A locale this system does not have is a failure:
// a test that ran in the C locale would show nothing.
void CheckLocales(lua_State* L)
{
    struct Expected
    {
        char const* Name;
        char Point;
    };
    const Expected locales[] = {
        { "de_DE.UTF-8", ',' },
        { "fr_FR.ISO8859-1", ',' },
        { "tr_TR.UTF-8", ',' },
        { "ru_RU.UTF-8", ',' },
        { "ja_JP.UTF-8", '.' },
        { "en_US.UTF-8", '.' },
    };
    for (auto const& locale : locales) {
        gChecks++;
        if (!setlocale(LC_ALL, locale.Name) || localeconv()->decimal_point[0] != locale.Point) {
            printf("FAILED the locale %s could not be set as expected\n", locale.Name);
            gFailures++;
            continue;
        }
        std::string what = std::string("in the locale ") + locale.Name;
        CheckAll(L, what.c_str(), kLocale);
    }
    setlocale(LC_ALL, "C");
}

}

int main()
{
    lua_State* L = luaL_newstate();
    if (!L) {
        printf("FAILED: no Lua state\n");
        return 1;
    }
    lua_setup_strcache(L, &CacheNothing, &ReleaseNothing);
    for (auto const& lib : kLoadedLibs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }

    CheckAll(L, "reading a hexadecimal float", kHexFloatsRead);
    CheckAll(L, "writing a hexadecimal float", kHexFloatsWritten);
    CheckAll(L, "math.log with base 2", kLogBaseTwo);
    CheckAll(L, "the text of an address", kAddresses);
    CheckAll(L, "string.pack with a long", kPackedLongs);
    CheckAll(L, "math.random", kRandom);
    CheckRandomPerThread(L);
    CheckAll(L, "what is the same on both", kUnchanged);
    CheckAll(L, "in the C locale", kLocale);
    CheckLocales(L);

    // The address itself, through the C API as luaL_tolstring uses it.
    gChecks++;
    lua_pushfstring(L, "%p|%p", reinterpret_cast<void*>(0x1234abcdull), reinterpret_cast<void*>(0x7ffdeadbeef0ull));
    if (strcmp(lua_tostring(L, -1), "000000001234ABCD|00007FFDEADBEEF0") != 0) {
        printf("FAILED the text of an address: %%p gives %s\n", lua_tostring(L, -1));
        gFailures++;
    }
    lua_pop(L, 1);

    lua_close(L);
    printf("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
