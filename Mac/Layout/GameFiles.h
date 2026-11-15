#pragma once

#include <cstddef>
#include <cstdint>

#include "GameMembers.h"

// The Mac game's string, its path roots and its file reader, as the game's
// own code shows them (Layout/GameShapes.cpp says which code). Norbyte's
// STDString, StaticSymbols::ls__PathRoots and FileReader
// (GameDefinitions/FileReader.h) are asserted or tested to agree
// (Layout/DefinitionChecks.cpp, Tests/GameStringNorbyte.cpp).

namespace bg3se::mac {

// ls::STDString: 16 bytes. The last byte is negative when the characters are
// behind the pointer the string starts with, and then the 32 bits at +8 are
// the length; otherwise the characters are in the string itself and the last
// byte is the length.
constexpr size_t kGameStringBytes = 0x10;
constexpr size_t kGameStringFlag = kGamePathTextFlag;
constexpr size_t kGameStringLongLength = 0x08;      // 32 bits
constexpr size_t kGameStringLongCapacity = 0x0c;    // 31 bits, and the flag's bit on top

// ls::Path::s_Roots: one pointer to a string for each path root.
constexpr size_t kPathRoots = 19;

// ls::FileReader.
constexpr size_t kFileReaderLoaded = 0x00;          // one byte
constexpr size_t kFileReaderData = 0x08;
constexpr size_t kFileReaderRead = 0x10;            // the next byte to read
constexpr size_t kFileReaderSize = 0x18;
constexpr size_t kFileReaderHandle = 0x20;          // two 32-bit handles
constexpr size_t kFileReaderType = 0x80;            // 32 bits
constexpr size_t kFileReaderFile = 0x88;            // ls::File*

}
