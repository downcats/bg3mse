#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <Platform/Symbols.h>

// Where the game keeps members of its component types, read from the game's
// own code and data, decoded from the image each time:
//
// ls::FieldMeta<M T::*, &T::member>::FieldPtr is a pointer to a data member
// the game keeps as a variable, once per member that is replicated on its
// own. On this platform such a pointer is the member's offset in T, and the
// variable's symbol names T, the member and its type.
//
// The functions the game has of each component type T, each followed from
// where it comes to the component:
//   ecs::sync::ComponentSerializer<T>::SerializeComponent writes every
//     replicated member in turn, a member of a plain type as its bytes
//     (ls::ScratchBuffer::Write of a pointer into the component and a size),
//     any other by handing a function its address;
//   ecs::sync::ComponentDeserializer<T>::DeserializeComponent reads them back;
//   ls::savegame::Framework::Ops<T, ...>::Save and ::Load hand the saved
//     members to the writer and the reader (Load has the component from
//     ecs::ComponentFrameStorageAllocRaw);
//   the lambda of ecs::_private::GetComponentDestructor<T> is handed the
//     component in x0 and destroys the members that own memory.

namespace bg3se::mac {

struct NamedField
{
    std::string Class;          // e.g. "eoc::HealthComponent"
    std::string Member;         // e.g. "m_HitPoints"
    std::string Type;           // e.g. "int"
    uint32_t Offset;
    uint32_t Size;              // of a fundamental type; 0 for any other
};

// How a function of the game touches a member.
enum class FieldAccess : uint8_t
{
    Written,        // ls::ScratchBuffer::Write of its bytes
    Passed,         // its address handed to a function that was followed and uses it (Size 0)
    Read,           // loaded at a constant offset, Size bytes
    Stored,         // stored at a constant offset, Size bytes
    PassedOn,       // an address handed to a function that was not followed (a library call such as
                    // memcpy or bzero, through a register, past the depth) or does not use it (Size 0)
};

struct SerializedField
{
    uint32_t Offset;
    uint32_t Size;              // the bytes written, loaded or stored; 0 when the member's address is handed to a function
    FieldAccess How{ FieldAccess::Written };
};

struct SerializedComponent
{
    std::string Class;
    char const* Source{ "serializer" };     // which of the component's functions
    std::vector<SerializedField> Fields;    // in address order
    // Writes whose pointer or size is not a constant place in the component
    // (the offset comes from a FieldPtr variable, or the bytes are an array's).
    uint32_t WritesNotRead{ 0 };
};

struct GameFieldScan
{
    std::vector<NamedField> Named;                  // sorted by class, then offset
    std::vector<SerializedComponent> Serialized;    // sorted by class
    uint32_t NamedNotRead{ 0 };                     // FieldPtr variables whose symbol or value could not be read
    uint32_t SerializersNotRead{ 0 };               // serializers and deserializers in which the component is never fetched
    uint32_t SavesNotFollowed{ 0 };                 // save-game functions in which it is not fetched by a call
    uint32_t DestructorsNotRead{ 0 };               // component destructors that could not be followed (a jump to a virtual destructor)
    // The kinds of the functions above the symbol table has none of under the
    // expected signature (or what they need), when the scan ran on the
    // others; empty when every kind was found.
    std::string NotFound;
    std::string Problem;                            // empty when the scan ran

    bool Ok() const { return Problem.empty(); }
};

// With `wanted`, only the functions of the classes it accepts are followed
// (every named member pointer is read): what the start-up sequence needs of
// the bound component types, at less of the cost.
GameFieldScan DecodeGameFields(GameImage const& image, std::function<bool(std::string_view)> const& wanted = {});

}
