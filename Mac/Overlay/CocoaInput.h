#pragma once

#include <cstdint>

// What the rest of Mac/Overlay asks of the overlay's input
// (Mac/Overlay/CocoaInput.cpp, which holds the Mac definitions of Norbyte's
// SDLManager).

namespace bg3se::mac::overlay {

// The drawable has another size: Lua's ViewportResized event, which on
// Windows comes from SDL's window event. Client thread.
void ReportViewportResized(uint32_t width, uint32_t height);

}
