// Norbyte's merge of virtual texture tile sets (Extender/Shared/
// VirtualTextures.inl, compiled inside Extender/ScriptExtender.cpp), run as
// this build compiles it on tile sets written here and, given the game's
// bundle, on one of the game's own. What is checked is what the merge reads
// and writes as text: the names of textures and layers in a tile set's
// metadata and the names of its page files, UTF-16 in the file format. The
// strings of the merged file are compared byte for byte with what the merge
// writes on Windows, where wchar_t is UTF-16: the expected bytes are built
// here from UTF-16 literals, not by the conversions under test.
//
// The game's functions the merge uses are stood in for as the game's do
// their part: the path roots (the data directory is a directory made for the
// test, which the merged files are written to) and the file reader (it reads
// a file of that directory whole, or a file stored in one of the game's
// packages, which is only read).
//
// Then what comes before the merge when several mods ship virtual textures:
// VirtualTextureHelpers reading each loaded mod's ScriptExtender/
// VirtualTextures.json in load order, and merging the tile sets they name
// when there is more than one. For that the client's mod manager and the
// table of fixed strings are stood in for too.
//
// Also here: the validator of wide string views (Lua/Shared/
// LuaTypeValidators.h), on a view that runs into memory that cannot be read.

#define __STDC_WANT_LIB_EXT1__ 1
#include <stdafx.h>

#include <Extender/ScriptExtender.h>
#include <Extender/Client/ExtensionStateClient.h>
#include <Extender/Server/ExtensionStateServer.h>
#include <GameDefinitions/GameState.h>
#include <GameDefinitions/FileReader.h>
#include <GameDefinitions/Module.h>
#include <GameDefinitions/Symbols.h>
#include <GameDefinitions/VirtualTextureFormat.h>
#include <Lua/Shared/LuaTypeValidators.h>

#include <Layout/GameStringTable.h>
#include <Platform/Log.h>

#include <External/lz4/lib/lz4.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <malloc/malloc.h>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace bg3se {

// As Extender/Shared/VirtualTextures.inl defines and declares them.
struct GTSMapping
{
    std::optional<STDString> MappedPath;
};

bool MergeGTS(HashMap<STDString, GTSMapping>& relativePaths);

namespace {

namespace fs = std::filesystem;

int gFailures = 0;
char const* gCase = "";

void Expect(bool ok, std::string const& what)
{
    if (!ok) {
        fprintf(stderr, "FAIL [%s] %s\n", gCase, what.c_str());
        gFailures++;
    }
}

void Failed(std::string const& what)
{
    Expect(false, what);
}

// --- the log ------------------------------------------------------------------

std::string gLogPartial;
std::vector<std::string> gLog;

int LogWrite(void*, char const* text, int size)
{
    gLogPartial.append(text, (size_t)size);
    size_t end;
    while ((end = gLogPartial.find('\n')) != std::string::npos) {
        gLog.push_back(gLogPartial.substr(0, end));
        gLogPartial.erase(0, end + 1);
    }
    return size;
}

// Every warning and error of the merge fails the case; `wanted` must each
// appear in a line.
void CheckLog(std::vector<std::string> const& wanted)
{
    for (auto const& line : gLog) {
        Expect(line.find("[WARN") == std::string::npos && line.find("[ERROR]") == std::string::npos, "the log has: " + line);
    }
    for (auto const& text : wanted) {
        bool seen = false;
        for (auto const& line : gLog) {
            seen = seen || line.find(text) != std::string::npos;
        }
        Expect(seen, "no line of the log has \"" + text + "\"");
    }
    gLog.clear();
}

// --- the game's allocator --------------------------------------------------------

// malloc and free, as the start-up harness has them: what is freed is
// overwritten first, so that the use of a freed block shows.
void* Allocate(std::size_t size)
{
    return malloc(size);
}

void Free(void* block) noexcept
{
    if (block) {
        size_t size = malloc_size(block);
        memset_s(block, size, 0xdd, size);
    }
    free(block);
}

// --- the table of fixed strings ----------------------------------------------

#include "StringTableStandIn.h"

// --- what VirtualTextureHelpers keeps to itself -------------------------------

// Its steps before the resource banks, reached as an explicit instantiation
// may name a private member.
template <class Tag, typename Tag::Type Member>
struct Reach
{
    friend typename Tag::Type Reached(Tag) { return Member; }
};

#define REACH(tag, type, member) \
    struct tag { using Type = type; friend Type Reached(tag); }; \
    template struct Reach<tag, &VirtualTextureHelpers::member>;

using Remaps = HashMap<FixedString, FixedString>;

REACH(CollectRemapsOf, Remaps (VirtualTextureHelpers::*)(), CollectRemaps)
REACH(RebuildOf, void (VirtualTextureHelpers::*)(), RebuildIfNecessary)
REACH(PathsOf, Remaps VirtualTextureHelpers::*, gtsPaths_)
REACH(SourcesOf, std::unordered_set<FixedString> VirtualTextureHelpers::*, sourceTileSets_)
REACH(BuiltOf, bool VirtualTextureHelpers::*, built_)

#undef REACH

// --- the game's file reader and path roots ---------------------------------

// A file the reader serves from a range of another file (one of the game's
// packages) instead of the data directory.
struct Stored
{
    fs::path File;
    uint64_t Offset;
    uint64_t Size;
};

std::map<std::string, Stored> gStored;
STDString* gPathRoots[19]{};

std::optional<std::vector<uint8_t>> ReadRange(fs::path const& file, uint64_t offset, std::optional<uint64_t> size)
{
    std::ifstream f(file, std::ios::binary);
    if (!f) return {};
    if (!size) {
        f.seekg(0, std::ios::end);
        size = (uint64_t)f.tellg();
    }
    std::vector<uint8_t> data(*size);
    f.seekg((std::streamoff)offset);
    f.read(reinterpret_cast<char*>(data.data()), (std::streamsize)data.size());
    if (!f) return {};
    return data;
}

// ls::FileReader::FileReader(Path const&, EType, flags) as far as the merge
// reads it: whether it loaded, the whole file at DataPtr, and its size.
FileReader* ReaderMake(FileReader* self, Path const& path, unsigned int, unsigned int)
{
    new (self) FileReader();
    std::string name(path.Name.data(), path.Name.size());
    std::optional<std::vector<uint8_t>> data;
    if (auto stored = gStored.find(name); stored != gStored.end()) {
        data = ReadRange(stored->second.File, stored->second.Offset, stored->second.Size);
    } else {
        data = ReadRange(name, 0, {});
    }
    if (data) {
        auto buffer = static_cast<uint8_t*>(malloc(std::max<size_t>(data->size(), 1)));
        memcpy(buffer, data->data(), data->size());
        self->IsLoaded = true;
        self->DataPtr = buffer;
        self->ReadPtr = buffer;
        self->FileSize = data->size();
        self->Type = FileMappingType::MemBuffer;
    }
    return self;
}

void ReaderFree(FileReader* self)
{
    free(self->DataPtr);
    self->DataPtr = self->ReadPtr = nullptr;
    self->IsLoaded = false;
}

// --- bytes --------------------------------------------------------------------

struct Bytes
{
    std::vector<uint8_t> Data;

    template <class T>
    size_t Put(T const& value)
    {
        auto at = Data.size();
        Data.resize(at + sizeof(T));
        memcpy(Data.data() + at, &value, sizeof(T));
        return at;
    }

    void Put(void const* data, size_t size)
    {
        auto bytes = static_cast<uint8_t const*>(data);
        Data.insert(Data.end(), bytes, bytes + size);
    }

    template <class T>
    void At(size_t offset, T const& value)
    {
        memcpy(Data.data() + offset, &value, sizeof(T));
    }
};

std::string Std(STDString const& text)
{
    return std::string(text.data(), text.size());
}

template <class T>
T Get(std::vector<uint8_t> const& data, size_t offset)
{
    T value{};
    if (offset + sizeof(T) <= data.size()) memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

std::vector<uint8_t> UnitBytes(std::u16string_view units)
{
    std::vector<uint8_t> bytes(units.size() * 2);
    memcpy(bytes.data(), units.data(), bytes.size());
    return bytes;
}

std::string Hex(std::vector<uint8_t> const& bytes, size_t from, size_t count)
{
    std::string text;
    char digits[4];
    for (size_t i = from; i < std::min(bytes.size(), from + count); i++) {
        snprintf(digits, sizeof(digits), "%02x ", bytes[i]);
        text += digits;
    }
    return text;
}

// The first difference of two byte strings, for the failure message.
void ExpectBytes(std::vector<uint8_t> const& actual, std::vector<uint8_t> const& expected, std::string const& what)
{
    if (actual == expected) return;
    size_t at = 0;
    while (at < actual.size() && at < expected.size() && actual[at] == expected[at]) at++;
    auto from = at < 16 ? 0 : at - 16;
    Expect(false, what + ": " + std::to_string(actual.size()) + " bytes, " + std::to_string(expected.size())
        + " expected; they differ at " + std::to_string(at) + "\n  written:  " + Hex(actual, from, 48)
        + "\n  expected: " + Hex(expected, from, 48));
}

// --- the file format ----------------------------------------------------------

// The header of a .gts file and the record of a page file in it, as the
// game's files have them (and as Norbyte's GTSHeader and GTSPageFileInfo
// declare them).
constexpr size_t kHeaderSize = 0xc0;
constexpr size_t kPageFileRecordSize = 0x218;
constexpr size_t kPageFileNameUnits = 256;
static_assert(sizeof(vt::GTSHeader) == kHeaderSize);
static_assert(sizeof(vt::GTSPageFileInfo) == kPageFileRecordSize);
static_assert(offsetof(vt::GTSPageFileInfo, NumPages) == kPageFileNameUnits * 2);

namespace header {
constexpr size_t Magic = 0x00, Version = 0x04, Guid = 0x0c, NumLayers = 0x1c, LayersOffset = 0x20, NumLevels = 0x28,
    LevelsOffset = 0x2c, TileWidth = 0x34, TileHeight = 0x38, TileBorder = 0x3c, NumFlatTileInfos = 0x44,
    FlatTileInfoOffset = 0x48, NumPackedTileIDs = 0x58, PackedTileIDsOffset = 0x5c, PageSize = 0x80,
    NumPageFiles = 0x84, PageFileMetadataOffset = 0x88, FourCCListSize = 0x90, FourCCListOffset = 0x94,
    ParameterBlockHeadersCount = 0x9c, ParameterBlockHeadersOffset = 0xa0, ThumbnailsOffset = 0xa8;
}
static_assert(offsetof(vt::GTSHeader, FourCCListOffset) == header::FourCCListOffset);
static_assert(offsetof(vt::GTSHeader, ThumbnailsOffset) == header::ThumbnailsOffset);
static_assert(offsetof(vt::GTSHeader, NumLevels) == header::NumLevels && offsetof(vt::GTSHeader, NumFlatTileInfos) == header::NumFlatTileInfos);
static_assert(offsetof(vt::GTSHeader, NumPageFiles) == header::NumPageFiles && offsetof(vt::GTSHeader, PageFileMetadataOffset) == header::PageFileMetadataOffset);

// The metadata of a tile set: a tree of tagged elements. A tag is four
// characters, stored in their order; a node holds elements and has a 32-bit
// extension of its length; every element is padded to 4 bytes.
struct FourCCBytes
{
    Bytes Out;
    std::vector<size_t> Open;

    void Header(char const* tag, uint8_t format, bool extended, uint32_t length)
    {
        Out.Put(tag, 4);
        Out.Put(format);
        Out.Put<uint8_t>(extended ? 1 : 0);
        Out.Put((uint16_t)length);
        if (extended) Out.Put(length >> 16);
    }

    void Pad()
    {
        while (Out.Data.size() % 4) Out.Put<uint8_t>(0);
    }

    void Begin(char const* tag)
    {
        Open.push_back(Out.Data.size());
        Header(tag, 1, true, 0);
    }

    void End()
    {
        auto at = Open.back();
        Open.pop_back();
        auto length = (uint32_t)(Out.Data.size() - at - 12);
        Out.At(at + 6, (uint16_t)length);
        Out.At(at + 8, length >> 16);
    }

    void Element(char const* tag, uint8_t format, std::vector<uint8_t> const& value)
    {
        Header(tag, format, false, (uint32_t)value.size());
        Out.Put(value.data(), value.size());
        Pad();
    }

    // `units` with its terminator.
    void String(char const* tag, std::u16string_view units)
    {
        auto bytes = UnitBytes(units);
        bytes.push_back(0);
        bytes.push_back(0);
        Element(tag, 2, bytes);
    }

    void Int(char const* tag, uint32_t value)
    {
        std::vector<uint8_t> bytes(4);
        memcpy(bytes.data(), &value, 4);
        Element(tag, 3, bytes);
    }
};

// One element of a tree, as the reader below finds it.
struct Element
{
    std::string Tag;
    uint8_t Format;
    size_t Offset; // of the value, in the tree's bytes
    uint32_t Length;
};

// The elements directly inside `[from, from + size)`.
std::vector<Element> Elements(std::vector<uint8_t> const& tree, size_t from, size_t size)
{
    std::vector<Element> elements;
    auto end = std::min(tree.size(), from + size);
    while (from + 8 <= end) {
        Element element;
        element.Tag.assign(reinterpret_cast<char const*>(tree.data() + from), 4);
        element.Format = tree[from + 4];
        element.Length = Get<uint16_t>(tree, from + 6);
        element.Offset = from + 8;
        if (tree[from + 5] == 1) {
            element.Length |= Get<uint32_t>(tree, from + 8) << 16;
            element.Offset += 4;
        }
        if (element.Offset + element.Length > end) break;
        elements.push_back(element);
        from = (element.Offset + element.Length + 3) & ~size_t(3);
    }
    return elements;
}

std::optional<Element> Child(std::vector<uint8_t> const& tree, Element const& node, char const* tag)
{
    for (auto const& element : Elements(tree, node.Offset, node.Length)) {
        if (element.Tag == tag) return element;
    }
    return {};
}

// The bytes of every NAME of the textures (META / ATLS / TXTS / TXTR / NAME),
// terminators included.
std::vector<std::vector<uint8_t>> TextureNames(std::vector<uint8_t> const& tree)
{
    std::vector<std::vector<uint8_t>> names;
    Element root{ "", 1, 0, (uint32_t)tree.size() };
    auto meta = Child(tree, root, "META");
    auto atls = meta ? Child(tree, *meta, "ATLS") : std::nullopt;
    auto txts = atls ? Child(tree, *atls, "TXTS") : std::nullopt;
    if (!txts) return names;
    for (auto const& txtr : Elements(tree, txts->Offset, txts->Length)) {
        if (auto name = Child(tree, txtr, "NAME")) {
            names.emplace_back(tree.begin() + name->Offset, tree.begin() + name->Offset + name->Length);
        }
    }
    return names;
}

// What Norbyte's merge writes as the metadata of a merged tile set
// (GTSStitchedFile::BuildFourCC), for textures of these names and places.
struct Texture
{
    std::u16string Name; // the units of the name element, without its terminator
    uint32_t Width, Height, X, Y;
};

std::vector<uint8_t> MergedFourCC(std::vector<Texture> const& textures)
{
    FourCCBytes cc;
    cc.Begin("META");
    cc.Begin("ATLS");
    cc.Begin("TXTS");
    for (auto const& texture : textures) {
        cc.Begin("TXTR");
        cc.String("NAME", texture.Name);
        cc.Int("WDTH", texture.Width);
        cc.Int("HGHT", texture.Height);
        cc.Int("XXXX", texture.X);
        cc.Int("YYYY", texture.Y);
        cc.String("ADDR", u"None");
        cc.Element("SRGB", 8, { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
        cc.Element("THMB", 0xd, std::vector<uint8_t>(48, 0));
        cc.End();
    }
    cc.End();
    cc.End();
    cc.String("PROJ", u"");
    cc.Begin("LINF");
    char16_t const* layers[] = { u"Albedo", u"Normal", u"Physical" };
    for (uint32_t i = 0; i < 3; i++) {
        cc.Begin("LAYR");
        cc.Int("INDX", i);
        cc.String("TYPE", u"BC3");
        cc.String("NAME", layers[i]);
        cc.End();
    }
    cc.End();
    cc.Begin("INFO");
    cc.Begin("COMP");
    cc.Begin("CMPW");
    cc.Int("MAJR", 5);
    cc.Int("MINR", 0);
    cc.End();
    cc.Begin("BLDV");
    cc.Int("MAJR", 5);
    cc.Int("MINR", 1);
    cc.String("BINF", u"ScriptExtender");
    cc.End();
    cc.End();
    cc.String("DATE", u"02-08-2023 07:49:30.7662814 PM +02:00");
    cc.String("BLKS", u"4096");
    cc.String("TILE", u"Software");
    cc.String("BDPR", u"default");
    cc.Int("LTMP", 0);
    cc.End();
    cc.End();
    return cc.Out.Data;
}

// The record of a page file: the name's units, then the rest of the name
// field as `tail` (a repeated byte), the page count, a checksum and the
// field after it.
std::vector<uint8_t> PageFileRecord(std::u16string_view name, uint8_t tail, uint32_t pages, uint8_t checksum)
{
    std::vector<uint8_t> record(kPageFileRecordSize, 0);
    memset(record.data(), tail, kPageFileNameUnits * 2);
    memcpy(record.data(), name.data(), name.size() * 2);
    record[name.size() * 2] = record[name.size() * 2 + 1] = 0;
    memcpy(record.data() + 0x200, &pages, 4);
    memset(record.data() + 0x204, checksum, 16);
    uint32_t f = 2;
    memcpy(record.data() + 0x214, &f, 4);
    return record;
}

// --- the tile sets written here ----------------------------------------------

// A tile set of 2 x 2 tiles in one level and three layers, whose 12 tiles
// are each one chunk of uniform colour in a page file of its own; the other
// page files have no tiles. Its textures are named as `textures` says, with
// the elements' bytes given whole.
struct TileSetSpec
{
    std::u16string PageFile; // the name of the page file of the tiles
    uint8_t PageFileTail;
    std::vector<std::u16string> OtherPageFiles;
    std::vector<std::vector<uint8_t>> TextureNameElements; // terminators included, as stored
    uint8_t Colour;
};

constexpr uint32_t kUniformBlock = 1;

std::vector<uint8_t> MakeTileSet(TileSetSpec const& spec)
{
    Bytes out;
    out.Data.resize(kHeaderSize, 0);
    out.At(header::Magic, 0x47505247u);
    out.At(header::Version, 5u);
    for (size_t i = 0; i < 16; i++) out.Data[header::Guid + i] = (uint8_t)(spec.Colour + i);
    out.At(header::TileWidth, 0x90);
    out.At(header::TileHeight, 0x90);
    out.At(header::TileBorder, 8);
    out.At(header::PageSize, 0x100000u);

    // Layers as the game's tile sets have them: sRGB albedo, then two
    // linear layers, default colour white.
    out.At(header::NumLayers, 3u);
    out.At(header::LayersOffset, (uint64_t)out.Data.size());
    for (uint32_t type : { 1u, 8u, 8u }) {
        out.Put(type);
        out.Put(0xffffffffu);
    }

    // Level 0: the flat tile of each layer of each tile, row by row.
    auto indices = out.Data.size();
    for (uint32_t i = 0; i < 12; i++) out.Put(i);
    out.At(header::NumLevels, 1u);
    out.At(header::LevelsOffset, (uint64_t)out.Data.size());
    out.Put(2u);
    out.Put(2u);
    out.Put((uint64_t)indices);

    // The parameter block of uniform tiles.
    auto block = out.Data.size();
    out.Put<uint16_t>(0x42);
    out.Put<uint16_t>(0);
    out.Put(4u);
    out.Put(1u);
    out.Put(1u);
    out.At(header::ParameterBlockHeadersCount, 1u);
    out.At(header::ParameterBlockHeadersOffset, (uint64_t)out.Data.size());
    out.Put(kUniformBlock);
    out.Put(0u);
    out.Put(16u);
    out.Put((uint64_t)block);

    std::vector<std::vector<uint8_t>> records;
    records.push_back(PageFileRecord(spec.PageFile, spec.PageFileTail, 1, spec.Colour));
    for (auto const& name : spec.OtherPageFiles) records.push_back(PageFileRecord(name, 0, 1, spec.Colour));
    out.At(header::NumPageFiles, (uint32_t)records.size());
    out.At(header::PageFileMetadataOffset, (uint64_t)out.Data.size());
    for (auto const& record : records) out.Put(record.data(), record.size());

    // Metadata as the game's tile sets have it.
    FourCCBytes cc;
    cc.Begin("META");
    cc.Begin("ATLS");
    cc.Begin("TXTS");
    uint32_t x = 0;
    for (auto const& name : spec.TextureNameElements) {
        cc.Begin("TXTR");
        cc.Element("NAME", 2, name);
        cc.Int("WDTH", 128);
        cc.Int("HGHT", 256);
        cc.Int("XXXX", x);
        cc.Int("YYYY", 0);
        cc.String("ADDR", u"None");
        cc.Element("SRGB", 8, { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
        cc.Element("THMB", 0xd, std::vector<uint8_t>(48, 0x11));
        cc.End();
        x += 128;
    }
    cc.End();
    cc.End();
    cc.String("PROJ", u"");
    cc.End();
    while (out.Data.size() % 4) out.Put<uint8_t>(0);
    out.At(header::FourCCListSize, (uint32_t)cc.Out.Data.size());
    out.At(header::FourCCListOffset, (uint64_t)out.Data.size());
    out.Put(cc.Out.Data.data(), cc.Out.Data.size());

    out.At(header::NumPackedTileIDs, 12u);
    out.At(header::PackedTileIDsOffset, (uint64_t)out.Data.size());
    for (uint32_t y = 0; y < 2; y++) {
        for (uint32_t tx = 0; tx < 2; tx++) {
            for (uint32_t layer = 0; layer < 3; layer++) {
                out.Put(layer | (y << 8) | (tx << 20));
            }
        }
    }
    out.At(header::NumFlatTileInfos, 12u);
    out.At(header::FlatTileInfoOffset, (uint64_t)out.Data.size());
    for (uint32_t i = 0; i < 12; i++) {
        out.Put<uint16_t>(0); // page file
        out.Put<uint16_t>(0); // page
        out.Put<uint16_t>((uint16_t)i); // chunk
        out.Put<uint16_t>(1);
        out.Put(i); // packed tile
    }
    return out.Data;
}

// The page file of such a tile set: one page of 12 uniform chunks.
std::vector<uint8_t> MakePageFile(uint8_t colour)
{
    Bytes out;
    out.Put(0x50415247u);
    out.Put(4u);
    for (size_t i = 0; i < 16; i++) out.Put<uint8_t>(colour);
    out.Put(12u);
    auto table = out.Data.size();
    for (uint32_t i = 0; i < 12; i++) out.Put(0u);
    for (uint32_t i = 0; i < 12; i++) {
        out.At(table + i * 4, (uint32_t)out.Data.size());
        out.Put(0u); // uniform
        out.Put(kUniformBlock);
        out.Put(4u);
        uint8_t rgba[] = { colour, (uint8_t)(colour + i), (uint8_t)(0x80 + i), 0xff };
        out.Put(rgba, 4);
    }
    return out.Data;
}

void WriteFile(fs::path const& path, std::vector<uint8_t> const& data)
{
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<char const*>(data.data()), (std::streamsize)data.size());
    Expect(f.good(), "cannot write " + path.string());
}

// UTF-8 of units that are all scalar values (for file names written here).
std::string Utf8(std::u16string_view units)
{
    std::string text;
    for (size_t i = 0; i < units.size(); i++) {
        char32_t point = units[i];
        if (point >= 0xd800 && point < 0xdc00 && i + 1 < units.size()) {
            point = 0x10000 + ((point - 0xd800) << 10) + (units[++i] - 0xdc00);
        }
        if (point < 0x80) {
            text += (char)point;
        } else if (point < 0x800) {
            text += (char)(0xc0 | (point >> 6));
            text += (char)(0x80 | (point & 0x3f));
        } else if (point < 0x10000) {
            text += (char)(0xe0 | (point >> 12));
            text += (char)(0x80 | ((point >> 6) & 0x3f));
            text += (char)(0x80 | (point & 0x3f));
        } else {
            text += (char)(0xf0 | (point >> 18));
            text += (char)(0x80 | ((point >> 12) & 0x3f));
            text += (char)(0x80 | ((point >> 6) & 0x3f));
            text += (char)(0x80 | (point & 0x3f));
        }
    }
    return text;
}

std::vector<uint8_t> NameElement(std::u16string_view units)
{
    auto bytes = UnitBytes(units);
    bytes.push_back(0);
    bytes.push_back(0);
    return bytes;
}

// The merged tile set the merge wrote and says it wrote.
struct Merged
{
    std::vector<uint8_t> File;
    std::vector<uint8_t> FourCC;
    std::vector<std::vector<uint8_t>> PageFiles;
};

std::optional<Merged> RunMerge(fs::path const& data, std::vector<std::string> const& sources)
{
    HashMap<STDString, GTSMapping> paths;
    for (auto const& source : sources) paths.set(STDString(source), GTSMapping{});
    Expect(MergeGTS(paths), "MergeGTS failed");

    std::optional<STDString> mapped;
    for (auto const& path : paths) {
        Expect(path.Value().MappedPath.has_value(), "no merged tile set for " + Std(path.Key()));
        if (path.Value().MappedPath) {
            Expect(!mapped || *mapped == *path.Value().MappedPath, "the sources were not merged into one tile set");
            mapped = path.Value().MappedPath;
        }
    }
    if (!mapped) return {};
    Expect(*mapped == "SEMergedTileSet_0.gts", "merged into " + Std(*mapped));

    auto file = ReadRange(data / Std(*mapped), 0, {});
    Expect(file.has_value(), "the merged tile set cannot be read");
    if (!file) return {};

    Merged merged;
    merged.File = *file;
    Expect(Get<uint32_t>(merged.File, header::Magic) == 0x47505247u && Get<uint32_t>(merged.File, header::Version) == 5,
        "the merged tile set has no header");
    auto ccOffset = Get<uint64_t>(merged.File, header::FourCCListOffset);
    auto ccSize = Get<uint32_t>(merged.File, header::FourCCListSize);
    Expect(ccOffset + ccSize <= merged.File.size(), "the metadata is outside the file");
    if (ccOffset + ccSize <= merged.File.size()) {
        merged.FourCC.assign(merged.File.begin() + ccOffset, merged.File.begin() + ccOffset + ccSize);
    }
    auto pfOffset = Get<uint64_t>(merged.File, header::PageFileMetadataOffset);
    auto pfCount = Get<uint32_t>(merged.File, header::NumPageFiles);
    Expect(pfOffset + (uint64_t)pfCount * kPageFileRecordSize <= merged.File.size(), "the page files are outside the file");
    for (uint32_t i = 0; i < pfCount && pfOffset + (i + 1) * kPageFileRecordSize <= merged.File.size(); i++) {
        auto at = merged.File.begin() + pfOffset + i * kPageFileRecordSize;
        merged.PageFiles.emplace_back(at, at + kPageFileRecordSize);
    }
    return merged;
}

std::u16string Prefix(std::string_view directory)
{
    return std::u16string(directory.begin(), directory.end());
}

// --- the cases ---------------------------------------------------------------

// Two tile sets written here, whose names hold what UTF-16 can hold: a
// character outside ASCII, a surrogate pair, an unpaired surrogate, a zero
// inside a name, an empty name, names of an even and an odd number of
// units, and a page file whose merged name is the longest the record holds.
// The merged set has a level the sources do not, so the merge also opens
// their page files by name and writes one of its own.
void MergeWrittenTileSets(fs::path const& data)
{
    gCase = "tile sets written here";

    std::u16string longName = u"é\U0001F600";
    longName.append(255 - 7 - longName.size() - 4, u'p');
    longName += u".gtp";

    TileSetSpec a{
        .PageFile = longName,
        .PageFileTail = 0,
        // Never opened (it has no tiles), so it may hold what no file name can.
        .OtherPageFiles = { u"pages/\U0001F600 spare \xdbff.gtp" },
        .TextureNameElements = {
            NameElement(u"tex_é\U0001F600"),
            NameElement(std::u16string(u"lone_\xd800_end\0zero", 15)),
            NameElement(u""),
            // A high surrogate the terminator ends.
            NameElement(u"\xd83d"),
        },
        .Colour = 0x20,
    };
    TileSetSpec b{
        .PageFile = u"b.gtp",
        // Kept past the merged name's terminator, as wcscpy_s keeps it.
        .PageFileTail = 0x5a,
        .OtherPageFiles = {},
        .TextureNameElements = {
            NameElement(u"abc"),
            NameElement(u"abcd"),
            NameElement(std::u16string(u"\xdc00", 1)),
        },
        .Colour = 0x60,
    };

    WriteFile(data / "Mods/A/a.gts", MakeTileSet(a));
    WriteFile(data / "Mods/A" / Utf8(longName), MakePageFile(a.Colour));
    WriteFile(data / "Mods/B/b.gts", MakeTileSet(b));
    WriteFile(data / "Mods/B/b.gtp", MakePageFile(b.Colour));

    auto merged = RunMerge(data, { "Mods/A/a.gts", "Mods/B/b.gts" });
    CheckLog({ "Adding source GTS", "Generated 6 tiles for missing mip levels: SEMergedTileSet_0_Mips.gtp",
        "Built merged GTS: SEMergedTileSet_0.gts" });
    if (!merged) return;

    // The merge stitches the sources in the order its map of paths gives
    // them; the first page file says which came first.
    Expect(merged->PageFiles.size() == 4, std::to_string(merged->PageFiles.size()) + " page files");
    if (merged->PageFiles.size() != 4) return;
    bool aFirst = memcmp(merged->PageFiles[0].data(), u"Mods/A/", 14) == 0;
    auto order = aFirst ? std::vector<TileSetSpec const*>{ &a, &b } : std::vector<TileSetSpec const*>{ &b, &a };

    // Each source's page files, named relative to the merged set; the rest
    // of each record as the source has it. Then the page file of the
    // generated tiles, next to the merged set.
    std::vector<std::vector<uint8_t>> pageFiles;
    for (auto source : order) {
        auto directory = Prefix(source == &a ? "Mods/A/" : "Mods/B/");
        pageFiles.push_back(PageFileRecord(directory + source->PageFile, source->PageFileTail, 1, source->Colour));
        for (auto const& other : source->OtherPageFiles) {
            pageFiles.push_back(PageFileRecord(directory + other, 0, 1, source->Colour));
        }
    }
    auto mips = ReadRange(data / "SEMergedTileSet_0_Mips.gtp", 0, {});
    Expect(mips && mips->size() >= 24, "the page file of the generated tiles cannot be read");
    auto mipRecord = PageFileRecord(u"SEMergedTileSet_0_Mips.gtp", 0, 1, 0);
    if (mips && mips->size() >= 24) memcpy(mipRecord.data() + 0x204, mips->data() + 8, 16);
    pageFiles.push_back(mipRecord);
    for (size_t i = 0; i < pageFiles.size(); i++) {
        ExpectBytes(merged->PageFiles[i], pageFiles[i], "page file " + std::to_string(i));
    }
    Expect(pageFiles[aFirst ? 0 : 1][kPageFileNameUnits * 2 - 2] == 0 && pageFiles[aFirst ? 0 : 1][kPageFileNameUnits * 2 - 4] != 0,
        "the long page file name does not fill its field");

    // The metadata, whole: the sources' textures, each name as its source
    // has it, placed where the source was stitched (the first source at the
    // origin, the second 2 tiles of 128 pixels to the right).
    std::vector<Texture> textures;
    uint32_t originX = 0;
    for (auto source : order) {
        uint32_t x = 0;
        for (auto const& element : source->TextureNameElements) {
            std::u16string units(element.size() / 2 - 1, 0);
            memcpy(units.data(), element.data(), units.size() * 2);
            textures.push_back(Texture{ units, 128, 256, originX + x, 0 });
            x += 128;
        }
        originX += 2 * 128;
    }
    ExpectBytes(merged->FourCC, MergedFourCC(textures), "the metadata");

    // 12 tiles of each source and 2 x 1 generated tiles of three layers.
    Expect(Get<uint32_t>(merged->File, header::NumFlatTileInfos) == 30, "the flat tiles are not those of the sources and the generated level");
    Expect(Get<uint32_t>(merged->File, header::NumLevels) == 2, "the merged set does not have 2 levels");
}

// One of the game's own tile sets, merged with itself as if two mods had
// it: every texture's name and every page file's name of the merged set is
// the source's, unit for unit.
void MergeGameTileSet(fs::path const& data, fs::path const& game)
{
    gCase = "a tile set of the game";

    // The game's package of virtual textures (LSPK, version 18): the list of
    // files, LZ4 compressed, at the offset its header gives.
    auto package = game / "Contents/Data/VirtualTextures.pak";
    auto head = ReadRange(package, 0, 40);
    Expect(head.has_value(), "cannot read " + package.string());
    if (!head) return;
    Expect(Get<uint32_t>(*head, 0) == 0x4b50534cu && Get<uint32_t>(*head, 4) == 18, "not an LSPK package of version 18");
    if (Get<uint32_t>(*head, 4) != 18) return;
    auto listOffset = Get<uint64_t>(*head, 8);
    auto counts = ReadRange(package, listOffset, 8);
    if (!counts) return;
    auto numFiles = Get<uint32_t>(*counts, 0);
    auto compressed = ReadRange(package, listOffset + 8, Get<uint32_t>(*counts, 4));
    if (!compressed) return;
    constexpr size_t kEntry = 272;
    std::vector<uint8_t> list((size_t)numFiles * kEntry);
    auto size = LZ4_decompress_safe(reinterpret_cast<char const*>(compressed->data()), reinterpret_cast<char*>(list.data()),
        (int)compressed->size(), (int)list.size());
    Expect(size == (int)list.size(), "the list of files does not decompress");
    if (size != (int)list.size()) return;

    // The first tile set stored uncompressed in the first part of the
    // package, and the page files stored uncompressed in any part (part N
    // is VirtualTextures_N.pak), by name.
    std::optional<Stored> source;
    std::string name;
    std::map<std::string, Stored> pageFileEntries;
    for (uint32_t i = 0; i < numFiles; i++) {
        auto entry = list.data() + i * kEntry;
        std::string entryName(reinterpret_cast<char const*>(entry), strnlen(reinterpret_cast<char const*>(entry), 256));
        auto offset = Get<uint32_t>(list, i * kEntry + 256) | ((uint64_t)Get<uint16_t>(list, i * kEntry + 260) << 32);
        auto part = entry[262];
        auto flags = entry[263];
        auto stored = Get<uint32_t>(list, i * kEntry + 264);
        auto uncompressed = Get<uint32_t>(list, i * kEntry + 268);
        if ((flags & 0x0f) != 0 || (uncompressed != 0 && uncompressed != stored)) continue;
        auto partFile = part == 0 ? package : game / ("Contents/Data/VirtualTextures_" + std::to_string(part) + ".pak");
        if (!source && entryName.ends_with(".gts") && part == 0) {
            source = Stored{ package, offset, stored };
            name = entryName;
        } else if (entryName.ends_with(".gtp")) {
            pageFileEntries[entryName.substr(entryName.find_last_of('/') + 1)] = Stored{ partFile, offset, stored };
        }
    }
    Expect(source.has_value(), "no tile set stored uncompressed in " + package.string());
    if (!source) return;
    auto original = ReadRange(source->File, source->Offset, source->Size);
    if (!original) return;
    auto pfOffset = Get<uint64_t>(*original, header::PageFileMetadataOffset);
    auto pfCount = Get<uint32_t>(*original, header::NumPageFiles);

    // The tile set and its page files in the directories of two mods.
    auto base = name.substr(name.find_last_of('/') + 1);
    gStored["Mods/A/" + base] = *source;
    gStored["Mods/B/" + base] = *source;
    for (uint32_t i = 0; i < pfCount; i++) {
        std::string pageFile;
        for (size_t n = 0; n < kPageFileNameUnits; n++) {
            auto unit = Get<uint16_t>(*original, pfOffset + i * kPageFileRecordSize + n * 2);
            if (unit == 0) break;
            pageFile += (char)unit;
        }
        if (auto entry = pageFileEntries.find(pageFile); entry != pageFileEntries.end()) {
            gStored["Mods/A/" + pageFile] = entry->second;
            gStored["Mods/B/" + pageFile] = entry->second;
        }
    }
    for (auto& [path, stored] : std::map<std::string, Stored>(gStored)) {
        gStored[Std(*gPathRoots[(unsigned)PathRootType::Data]) + "/" + path] = stored;
    }

    auto merged = RunMerge(data, { "Mods/A/" + base, "Mods/B/" + base });
    if (!merged) {
        CheckLog({});
        return;
    }
    // Merged side by side the sets may need a level more than they have,
    // which the merge generates from their page files into one of its own.
    bool mips = Get<uint32_t>(merged->File, header::NumLevels) > Get<uint32_t>(*original, header::NumLevels);
    if (mips) {
        CheckLog({ "Adding source GTS", "for missing mip levels: SEMergedTileSet_0_Mips.gtp", "Built merged GTS: SEMergedTileSet_0.gts" });
    } else {
        CheckLog({ "Adding source GTS", "Built merged GTS: SEMergedTileSet_0.gts" });
    }

    std::vector<uint8_t> tree(original->begin() + Get<uint64_t>(*original, header::FourCCListOffset),
        original->begin() + Get<uint64_t>(*original, header::FourCCListOffset) + Get<uint32_t>(*original, header::FourCCListSize));
    auto names = TextureNames(tree);
    auto mergedNames = TextureNames(merged->FourCC);
    Expect(!names.empty(), "the game's tile set has no textures: " + name);
    Expect(mergedNames.size() == 2 * names.size(), std::to_string(mergedNames.size()) + " textures merged from 2 x "
        + std::to_string(names.size()));
    for (size_t i = 0; i < mergedNames.size() && i < 2 * names.size(); i++) {
        ExpectBytes(mergedNames[i], names[i % names.size()], "the name of texture " + std::to_string(i));
    }

    auto expectedCount = 2 * (size_t)pfCount + (mips ? 1 : 0);
    Expect(merged->PageFiles.size() == expectedCount, std::to_string(merged->PageFiles.size()) + " page files merged from 2 x "
        + std::to_string(pfCount));
    if (merged->PageFiles.size() != expectedCount) return;
    bool aFirst = memcmp(merged->PageFiles[0].data(), u"Mods/A/", 14) == 0;
    for (size_t i = 0; i < 2 * (size_t)pfCount; i++) {
        std::vector<uint8_t> record(original->begin() + pfOffset + (i % pfCount) * kPageFileRecordSize,
            original->begin() + pfOffset + (i % pfCount + 1) * kPageFileRecordSize);
        size_t length = 0;
        while (length < kPageFileNameUnits && Get<uint16_t>(record, length * 2) != 0) length++;
        auto prefix = Prefix((i < pfCount) == aFirst ? "Mods/A/" : "Mods/B/");
        std::u16string units(length, 0);
        memcpy(units.data(), record.data(), length * 2);
        units = prefix + units;
        memcpy(record.data(), units.data(), units.size() * 2);
        record[units.size() * 2] = record[units.size() * 2 + 1] = 0;
        ExpectBytes(merged->PageFiles[i], record, "page file " + std::to_string(i));
    }
    if (mips) {
        auto file = ReadRange(data / "SEMergedTileSet_0_Mips.gtp", 0, {});
        Expect(file && file->size() >= 24 && file->size() % 0x100000 == 0, "the page file of the generated tiles cannot be read");
        if (!file || file->size() < 24) return;
        auto record = PageFileRecord(u"SEMergedTileSet_0_Mips.gtp", 0, (uint32_t)(file->size() / 0x100000), 0);
        memcpy(record.data() + 0x204, file->data() + 8, 16);
        ExpectBytes(merged->PageFiles.back(), record, "the page file of the generated tiles");
    }
}

// A view of wide characters that ends in memory that cannot be read is
// rejected; one that ends at the edge is not.
// --- several mods' virtual textures -------------------------------------------

ecl::EoCClient* gClient = nullptr;

void LoadOrder(std::vector<std::string> const& directories)
{
    auto& mods = gClient->ModManager->LoadOrderedModules;
    mods.clear();
    for (auto const& directory : directories) {
        Module mod;
        mod.Info.Directory = STDString(directory);
        mod.Info.Name = STDString(directory);
        mods.push_back(mod);
    }
}

void WriteText(fs::path const& path, std::string const& text)
{
    WriteFile(path, std::vector<uint8_t>(text.begin(), text.end()));
}

std::map<std::string, std::string> Plain(HashMap<FixedString, FixedString> const& map)
{
    std::map<std::string, std::string> plain;
    for (auto const& entry : map) plain[entry.Key().GetString()] = entry.Value().GetString();
    return plain;
}

TileSetSpec SmallTileSet(char16_t const* name, uint8_t colour)
{
    return TileSetSpec{
        .PageFile = std::u16string(name) + u".gtp",
        .PageFileTail = 0,
        .OtherPageFiles = {},
        .TextureNameElements = { NameElement(std::u16string(name) + u"_0"), NameElement(std::u16string(name) + u"_1") },
        .Colour = colour,
    };
}

// Four mods in load order: two name a tile set each and the same texture,
// one names nothing it can use, one has no file, and a fifth names the
// first's tile set again. A later mod's mapping of a texture replaces an
// earlier one's; the two tile sets are merged into one, which every texture
// is then mapped to; asked again with the same tile sets, nothing is
// rebuilt. With one tile set, nothing is merged.
void MergeSeveralModsTileSets(fs::path const& data)
{
    gCase = "several mods' VirtualTextures.json";

    auto a = SmallTileSet(u"a", 0x30), b = SmallTileSet(u"b", 0x70);
    WriteFile(data / "Mods/A/a.gts", MakeTileSet(a));
    WriteFile(data / "Mods/A/a.gtp", MakePageFile(a.Colour));
    WriteFile(data / "Mods/B/b.gts", MakeTileSet(b));
    WriteFile(data / "Mods/B/b.gtp", MakePageFile(b.Colour));

    WriteText(data / "Mods/A/ScriptExtender/VirtualTextures.json",
        R"({"Mappings": [{"GTexName": "texA", "GTS": "Mods/A/a.gts"}, {"GTexName": "shared", "GTS": "Mods/A/a.gts"}]})");
    WriteText(data / "Mods/B/ScriptExtender/VirtualTextures.json",
        R"({"Mappings": [{"GTexName": "texB", "GTS": "Mods/B/b.gts"}, {"GTexName": "shared", "GTS": "Mods/B/b.gts"}]})");
    WriteText(data / "Mods/C/ScriptExtender/VirtualTextures.json",
        R"({"Mappings": [1, {"GTexName": "noTileSet"}, {"GTexName": "", "GTS": "Mods/A/a.gts"},)"
        R"( {"GTexName": "notText", "GTS": 2}, {"GTexName": "noName", "GTS": ""}]})");
    WriteText(data / "Mods/E/ScriptExtender/VirtualTextures.json", R"({"Mappings": [{"GTexName": "texE", "GTS": "Mods/A/a.gts"}]})");
    WriteText(data / "Mods/F/ScriptExtender/VirtualTextures.json", R"({"Mappings": {"GTexName": "notAnArray", "GTS": "Mods/A/a.gts"}})");

    using Map = std::map<std::string, std::string>;
    LoadOrder({ "A", "B", "C", "D", "E", "F" });
    {
        VirtualTextureHelpers helpers;
        auto remaps = Plain((helpers.*Reached(CollectRemapsOf{}))());
        Expect(remaps == (Map{ { "texA", "Mods/A/a.gts" }, { "shared", "Mods/B/b.gts" }, { "texB", "Mods/B/b.gts" }, { "texE", "Mods/A/a.gts" } }),
            "the mods' mappings are not the four usable ones, the later mod's for the texture both map");
        gLog.clear();

        (helpers.*Reached(RebuildOf{}))();
        CheckLog({ "Loaded 4 virtual texture mappings", "Built merged GTS: SEMergedTileSet_0.gts" });
        Expect(helpers.*Reached(BuiltOf{}), "the merged tile set is not marked built");
        std::set<std::string> sources;
        for (auto const& source : helpers.*Reached(SourcesOf{})) sources.insert(source.GetString());
        Expect(sources == std::set<std::string>{ "Mods/A/a.gts", "Mods/B/b.gts" }, "the sources are not the two tile sets");
        auto merged = "SEMergedTileSet_0.gts";
        Expect(Plain(helpers.*Reached(PathsOf{})) == (Map{ { "texA", merged }, { "shared", merged }, { "texB", merged }, { "texE", merged } }),
            "the textures are not all mapped to the merged tile set");
        Expect(fs::exists(data / merged), "the merged tile set was not written");

        (helpers.*Reached(RebuildOf{}))();
        CheckLog({ "Merged tileset is already up to date, skipping build" });
        Expect(helpers.*Reached(BuiltOf{}) && Plain(helpers.*Reached(PathsOf{})).at("texB") == merged, "the merged tile set was dropped");
    }

    LoadOrder({ "A", "D", "E" });
    {
        VirtualTextureHelpers helpers;
        (helpers.*Reached(RebuildOf{}))();
        for (auto const& line : gLog) Expect(line.find("merged") == std::string::npos, "one tile set was merged: " + line);
        CheckLog({ "Loaded 3 virtual texture mappings" });
        Expect(!(helpers.*Reached(BuiltOf{})), "one tile set is marked built");
        Expect(Plain(helpers.*Reached(PathsOf{})) == (Map{ { "texA", "Mods/A/a.gts" }, { "shared", "Mods/A/a.gts" }, { "texE", "Mods/A/a.gts" } }),
            "the textures of one tile set are not mapped to it");
    }
    LoadOrder({});
}

void ValidateWideViews()
{
    gCase = "the validator of wide string views";
    auto page = (size_t)getpagesize();
    auto memory = static_cast<uint8_t*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    Expect(memory != MAP_FAILED && mprotect(memory + page, page, PROT_NONE) == 0, "cannot map the pages");
    if (memory == MAP_FAILED) return;
    auto edge = memory + page;
    for (size_t length : { 2, 8, 64 }) {
        auto fits = reinterpret_cast<wchar_t const*>(edge - length * sizeof(wchar_t));
        WStringView inside(fits, length);
        Expect(lua::Validate(&inside, Overload<WStringView>{}), "a view of " + std::to_string(length) + " readable characters is rejected");
        auto past = reinterpret_cast<wchar_t const*>(edge - length * sizeof(wchar_t) / 2);
        WStringView over(past, length);
        Expect(!lua::Validate(&over, Overload<WStringView>{}), "a view of " + std::to_string(length) + " characters, half of them unreadable, is accepted");
    }
    munmap(memory, 2 * page);
}

int Run(int argc, char** argv)
{
    FILE* log = funopen(nullptr, nullptr, &LogWrite, nullptr, nullptr);
    setvbuf(log, nullptr, _IOLBF, 0);
    mac::SetLogStream(log);
    mac::SetLogDebug(true);

    gCoreLibPlatformInterface.Alloc = &Allocate;
    gCoreLibPlatformInterface.Free = &Free;
    InitStaticSymbols();
    auto& sym = GetStaticSymbols();
    sym.ls__FileReader__ctor = &ReaderMake;
    sym.ls__FileReader__dtor = &ReaderFree;
    sym.ls__PathRoots = gPathRoots;

    gStrings.Make();
    sym.ls__GlobalStringTable__MainTable__CreateFromString = &StringCreate;
    sym.ls__FixedString__GetString = &StringGet;
    sym.ls__FixedString__IncRef = &StringIncRef;
    sym.ls__GlobalStringTable__MainTable__DecRef = &StringDecRef;
    sym.ls__gGlobalStringTable = &gStrings.Table;
    gCoreLibPlatformInterface.ls__GlobalStringTable__MainTable__CreateFromString = sym.ls__GlobalStringTable__MainTable__CreateFromString;
    gCoreLibPlatformInterface.ls__FixedString__GetString = sym.ls__FixedString__GetString;
    gCoreLibPlatformInterface.ls__FixedString__IncRef = sym.ls__FixedString__IncRef;
    gCoreLibPlatformInterface.ls__GlobalStringTable__MainTable__DecRef = sym.ls__GlobalStringTable__MainTable__DecRef;
    gCoreLibPlatformInterface.ls__gGlobalStringTable = sym.ls__gGlobalStringTable;

    gClient = static_cast<ecl::EoCClient*>(calloc(1, sizeof(ecl::EoCClient)));
    gClient->ModManager = static_cast<ModManager*>(calloc(1, sizeof(ModManager)));
    sym.ecl__EoCClient = &gClient;

    ValidateWideViews();

    auto scratch = fs::temp_directory_path() / ("bg3mse_vt_tests_" + std::to_string(getpid()));
    auto run = [&](char const* name, auto&& test) {
        auto data = scratch / name;
        fs::remove_all(data);
        fs::create_directories(data);
        gPathRoots[(unsigned)PathRootType::Data] = new STDString(data.string());
        test(data);
        fflush(log);
        gStored.clear();
    };

    run("written", [](fs::path const& data) { MergeWrittenTileSets(data); });
    run("mods", [](fs::path const& data) { MergeSeveralModsTileSets(data); });
    if (argc > 1) {
        fs::path game = argv[1];
        run("game", [&](fs::path const& data) { MergeGameTileSet(data, game); });
    }

    std::error_code ignored;
    fs::remove_all(scratch, ignored);
    fflush(log);
    for (auto const& line : gLog) {
        Expect(line.find("[WARN") == std::string::npos && line.find("[ERROR]") == std::string::npos, "the log has: " + line);
    }

    if (gFailures == 0) {
        printf("virtual texture tests passed\n");
        return 0;
    }
    fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
}

}

}

int main(int argc, char** argv)
{
    return bg3se::Run(argc, argv);
}
