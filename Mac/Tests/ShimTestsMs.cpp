// Compiled with -fms-extensions, as Norbyte's sources are: there clang's own
// _rotl family is used instead of the shim's definitions.
#include <windows.h>

static_assert(__has_builtin(_rotl), "built with -fms-extensions");

bool MsRotateBuiltinsMatch()
{
    return _rotl(0x80000001u, 1) == 0x00000003u && _rotl(1u, 33) == 2u && _rotr(0x00000003u, 1) == 0x80000001u
        && _rotl64(0x8000000000000001ull, 4) == 0x18ull && _rotr64(0x18ull, 4) == 0x8000000000000001ull;
}
