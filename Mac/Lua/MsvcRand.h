#pragma once

// rand() and srand() as Microsoft's C library has them, which is what Lua's
// math.random and math.randomseed call in Norbyte's Windows build
// (lmathlib.c: l_rand, l_srand and L_RANDMAX without LUA_USE_POSIX).
//
// The generator is a linear congruential one on 32 bits: each call sets the
// state to state * 214013 + 2531011 and returns bits 16 to 30 of it, so
// RAND_MAX is 0x7fff. srand sets the state to its argument. The state is the
// calling thread's (the C library keeps it with each thread's own data) and
// a thread that never called srand starts from 1, as if srand(1) had been
// called: its first rand() is 41, then 18467, 6334, 26500.
//
// The constants and the start are the ones Microsoft's library is known to
// use, and the sequence above is the one widely published for it; neither
// was read off a Windows build. Mac/Tests/LuaTests.cpp checks the sequence
// and Lua's use of it.

#if !defined(__cplusplus)
#error "Lua is compiled as C++ here, as Norbyte's project compiles it"
#endif

// In C++14, which Lua is compiled as.
namespace bg3se { namespace mac { namespace msvc {

constexpr int kRandMax = 0x7fff;

inline unsigned& RandState()
{
    thread_local unsigned state = 1;
    return state;
}

inline void SeedRand(unsigned seed)
{
    RandState() = seed;
}

inline int Rand()
{
    unsigned& state = RandState();
    state = state * 214013u + 2531011u;
    return static_cast<int>((state >> 16) & kRandMax);
}

} } }
