#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What the installer reads of a Mach-O file, thin or universal: its slices,
// their library commands and whether their code signatures hold.
//
// The file is read from bytes in memory; nothing here touches a file.

namespace bg3se::mac::installer {

// A load command that makes dyld load a library (LC_LOAD_DYLIB and its kin).
struct DylibCommand
{
    uint32_t Cmd{ 0 };
    uint32_t Index{ 0 };            // its place among the slice's load commands
    uint32_t Size{ 0 };
    uint32_t NameOffset{ 0 };
    uint32_t Timestamp{ 0 };
    uint32_t CurrentVersion{ 0 };
    uint32_t CompatibilityVersion{ 0 };
    std::string Path;
    // Placed after one of the commands a linker writes after the libraries
    // (LC_RPATH, LC_FUNCTION_STARTS, LC_DATA_IN_CODE, LC_CODE_SIGNATURE):
    // the linker did not put it there.
    bool AfterLinkerCommands{ false };
};

struct Slice
{
    uint32_t CpuType{ 0 };
    uint32_t CpuSubtype{ 0 };
    uint64_t Offset{ 0 };           // of the slice in the file
    uint64_t Size{ 0 };
    uint32_t FileType{ 0 };
    uint32_t CommandCount{ 0 };
    uint32_t CommandBytes{ 0 };
    uint32_t LastCommand{ 0 };
    uint32_t LastCommandSize{ 0 };
    // Where the header padding ends: the first byte, from the slice's start,
    // of a section's or segment's contents after the load commands.
    uint64_t PaddingEnd{ 0 };
    std::optional<std::array<uint8_t, 16>> Uuid;
    std::vector<DylibCommand> Dylibs;
    // LC_ID_DYLIB of a library.
    std::string IdName;
    uint32_t IdCurrentVersion{ 0 };
    // LC_CODE_SIGNATURE, from the slice's start; no signature when the size is 0.
    uint32_t SignatureOffset{ 0 };
    uint32_t SignatureSize{ 0 };
};

struct MachOFile
{
    bool Universal{ false };
    std::vector<Slice> Slices;

    Slice const* Find(uint32_t cpuType) const;
};

// The slices of a Mach-O file: a 64-bit image, or a universal file (32-bit
// fat header) of 64-bit images. Nothing, with the reason, for anything else
// or for a file whose structure does not hold together.
std::optional<MachOFile> ReadMachO(std::span<uint8_t const> file, std::string& problem);

// "arm64", "x86_64", or the number.
std::string CpuName(uint32_t cpuType);

std::string UuidText(std::array<uint8_t, 16> const& uuid);

// What a slice's code signature says of it.
struct SignatureFacts
{
    bool Present{ false };
    // Every page hash of every code directory, and the hash of every blob a
    // special slot names, matches the slice's bytes.
    bool HashesMatch{ false };
    // Ad hoc: no certificate (an empty CMS blob or none) and CS_ADHOC set.
    bool AdHoc{ false };
    uint32_t Flags{ 0 };
    std::string Identifier;
    std::string Problem;            // why the hashes do not match, or the signature cannot be read
};

SignatureFacts CheckSignature(std::span<uint8_t const> file, Slice const& slice);

}
