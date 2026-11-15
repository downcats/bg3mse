#include <stdafx.h>
#include <GameDefinitions/Base/Base.h>

#include "GameStringNorbyte.h"

BEGIN_NS(mac::test)

size_t NorbyteStringSize()
{
    return sizeof(STDString);
}

size_t NorbyteStringLength(void const* string)
{
    return static_cast<STDString const*>(string)->size();
}

char const* NorbyteStringText(void const* string)
{
    return static_cast<STDString const*>(string)->data();
}

END_NS()
