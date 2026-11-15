#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Is a SPIR-V module the same shader as another one?
//
// Compiling the same shader twice need not give the same bytes: the compiler
// numbers its ids and orders its declarations as it goes, and what it names
// in the header (capabilities, extensions, debug names) depends on its
// version and switches. None of that changes what the shader does. The hash
// here leaves it out, and keeps everything that does:
//
//   - every entry point: its execution model, name, execution modes and
//     interface variables;
//   - every function an entry point reaches, instruction by instruction in
//     order, with the ids it defines numbered in the order it defines them;
//   - every type, constant and global variable those refer to, each by what
//     it is made of (its opcode, literals and operands, all the way down) and
//     by its decorations (location, set, binding, offsets, built-ins, ...).
//
// Two modules with the same hash differ only in id numbers, in the order of
// their global declarations, in declarations nothing refers to, and in the
// capabilities, extensions and debug instructions they list. Anything this
// code does not know how to read (an instruction or operand outside the
// table, a malformed module), and a module with two global variables that
// nothing tells apart, is refused: the caller then compares bytes.

namespace bg3se::mac {

using SpirvHash = std::array<uint8_t, 32>;

// False, with `why`, when the module cannot be read with certainty.
bool SpirvModuleHash(std::span<uint8_t const> module, SpirvHash& hash, std::string& why);

// For a buffer variable that is an array of structures: the members of an
// element the module reads, by their place in the structure. False when that
// cannot be said: the variable is used otherwise than as the base of an
// access to one member of one element, or to one element that is then
// loaded and has single members taken out of it (it is handed to a
// function, kept whole, its length asked), or the module cannot be read.
bool SpirvElementMembersRead(std::span<uint8_t const> module, uint32_t variable, std::vector<uint32_t>& members);

// Which operands of an instruction are ids, by the same table the hash reads
// instructions with: `operands` is the instruction without its first word.
// `result` is the place of its result id among them, or -1; `ids` the places
// of every other id (its result type among them). False for an instruction
// the table does not know or that does not fit its pattern.
bool SpirvIdOperands(uint16_t opcode, std::span<uint32_t const> operands, int& result, std::vector<uint32_t>& ids);

struct ContainerParts
{
    uint32_t Version{ 0 };
    std::span<uint8_t const> Name;
    std::span<uint8_t const> Reflection;
    std::span<uint8_t const> Stages[6];     // 0 vertex, 4 fragment
};

// The parts of a shader container (.bshd). False when it is not one.
bool ParseContainer(std::span<uint8_t const> container, ContainerParts& parts);

// What a Vulkan stage blob holds besides its module: the chunk header without
// the module's size and the hash of its bytes. False when the blob is not a
// SHD8 chunk.
using VulkanStageHeader = std::array<uint8_t, 26>;
bool SplitVulkanStage(std::span<uint8_t const> stage, VulkanStageHeader& header, std::span<uint8_t const>& module);

// The modules of a Vulkan stage blob: one, or two. A fragment stage that
// discards is shipped twice, once with the kill instruction and once with
// the "demote to helper invocation" extension's, for the driver to choose
// from; nothing in the container says which. A chunk of the toolkit's
// earlier format (SHD7) holds one. Empty when the blob is none of these.
std::vector<std::span<uint8_t const>> VulkanStageModules(std::span<uint8_t const> stage);

// The identity of a container: the SHA-256 of its name, version, reflection
// table and stages, where a Vulkan stage counts by its module's hash above.
// A stage that cannot be read that way, and a file that is not a container,
// count by their bytes. `structural` says how many stages were compared by
// module hash.
SpirvHash ContainerHash(std::span<uint8_t const> container, uint32_t* structural = nullptr);

}
