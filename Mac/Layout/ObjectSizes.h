#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <Platform/Symbols.h>

// The size the game allocates for an object of a class, read from the game's
// own code: wherever it allocates a constant number of bytes (operator new
// or malloc) and then makes an object of them, either by storing a class's
// vtable at the start of the block or by handing the block to a
// constructor (when a vtable is stored after the constructor returns, the
// constructor was a base's and the vtable names the class). The class is
// named by that vtable's or constructor's symbol.
// Nothing here is a table: every size is decoded from the image each time.

namespace bg3se::mac {

struct ObjectSize
{
    std::string Witness;        // the vtable's symbol ("__ZTV...") or the constructor's ("__ZN...C1E...")
    uint32_t Size;
    uint32_t Sites;             // the places that allocate one
};

struct ObjectSizeScan
{
    std::vector<ObjectSize> Sizes;          // sorted by witness; only those every site agrees on
    std::vector<std::string> Conflicting;   // witnesses allocated with more than one size, sorted
    uint32_t Allocations{ 0 };              // calls of an allocation function with a constant size
    std::string Problem;                    // empty when the scan ran

    bool Ok() const { return Problem.empty(); }
    ObjectSize const* Find(std::string_view witness) const;
    bool Conflicts(std::string_view witness) const;
};

ObjectSizeScan DecodeObjectSizes(GameImage const& image);

}
