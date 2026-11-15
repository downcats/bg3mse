#pragma once

#include <span>
#include <string>
#include <string_view>

// Which of Metal's methods the hooks (GfxMetal.mm) replace, and whether those
// are every way the game has of doing what the hooks follow.
//
// The hooks see the game's rendering only through the Objective-C messages it
// sends: an encoder made by a method that is not hooked has no record, and a
// texture bound or a draw made by one is not seen. The game's code sends each
// message through a stub the linker named after the selector
// (_objc_msgSend$<selector>), so the selectors it can send are in its symbol
// table. Each family below is a kind of call the hooks must see all of: a
// selector of a family that is not in the hooked list means the game has a
// path the hooks were not written for, and the Metal features stay off.

namespace bg3se::mac::gfx {

// The prefix of the linker's stub for a selector.
inline constexpr std::string_view kSelectorStubPrefix = "_objc_msgSend$";

// The selectors the hooks replace, by the class of object they are sent to.
std::span<char const* const> HookedDeviceSelectors();
std::span<char const* const> HookedQueueSelectors();
std::span<char const* const> HookedBufferSelectors();
std::span<char const* const> HookedRenderEncoderSelectors();
std::span<char const* const> HookedComputeEncoderSelectors();

// Whether `selector` is one of a family the hooks must see all of.
bool InHookedFamily(std::string_view selector);
// Whether the hooks replace it.
bool IsHooked(std::string_view selector);

// Empty when the selectors of the game (its stubs' names, without the prefix)
// can all be followed: the ones the hooks cannot do without are there, and
// none of a family is unhooked. Otherwise why not.
std::string CheckMetalCalls(std::span<std::string_view const> selectors);

}
