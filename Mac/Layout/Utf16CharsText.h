#pragma once

#include "Utf16Chars.h"

#include <CoreLib/Base/BaseString.h>

// `ToUTF8(record.FileNameBuf)` in Norbyte's code: CoreLib's ToUTF8 of the
// buffer's text.

namespace bg3se::mac {

template <size_t N>
auto ToUTF8(Utf16Chars<N> const& text)
{
    auto wide = text.Wide();
    return ::bg3se::ToUTF8(::bg3se::WStringView(wide.data(), wide.size()));
}

}
