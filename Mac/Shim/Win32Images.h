// What the module and resource functions share: a loaded Mach-O image, which is
// what an HMODULE names. Not part of the Win32 surface.
#pragma once

#include <windows.h>

#include <mach-o/loader.h>

namespace bg3se::shim {

struct Image
{
    mach_header_64 const* Header{ nullptr };
    char const* Path{ nullptr };

    explicit operator bool() const { return Header != nullptr; }
    bool IsExecutable() const { return Header->filetype == MH_EXECUTE; }
};

// The image an HMODULE names, or an empty Image; NULL names the main executable.
Image ImageOf(HMODULE module);

// Where the image's code is in memory: the __text section of its __TEXT
// segment. False when the module is no loaded image or has no such section.
bool CodeSection(HMODULE module, uint8_t const*& start, size_t& size);

// Calls fn(segment_command_64 const&) for each segment of the image.
template <class Fn>
void ForEachSegment(Image const& image, Fn&& fn)
{
    auto command = reinterpret_cast<load_command const*>(image.Header + 1);
    for (uint32_t i = 0; i < image.Header->ncmds; i++) {
        if (command->cmd == LC_SEGMENT_64) {
            fn(*reinterpret_cast<segment_command_64 const*>(command));
        }
        command = reinterpret_cast<load_command const*>(reinterpret_cast<uint8_t const*>(command) + command->cmdsize);
    }
}

}
