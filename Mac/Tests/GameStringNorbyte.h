#pragma once

#include <cstddef>

// Norbyte's STDString (CoreLib/Base/BaseString.h), compiled as his sources
// are, behind plain functions: what his code reads from 16 bytes of the
// game's that hold a string.

namespace bg3se::mac::test {

size_t NorbyteStringSize();
size_t NorbyteStringLength(void const* string);
char const* NorbyteStringText(void const* string);

}
