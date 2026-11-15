#include <stdafx.h>
#include <Platform/CodeSearch.h>
#include <Platform/CodeShape.h>
#include <Platform/Config.h>
#include <Platform/DebugLog.h>
#include <Platform/FileReaderHook.h>
#include <Platform/Log.h>
#include <Shaders/PakFile.h>
#include <Shaders/GameInterface.h>
#include <Shaders/ShaderMapping.h>
#include <Shaders/ShaderProvision.h>
#include <Shaders/SpirvIdentity.h>
#include <Shaders/TranslatedFunctions.h>
#include <Shaders/Translation.h>

#include <GameDefinitions/FileReader.h>
#include <GameHooks/EngineHooksFwdDecl.h>
#include <detours.h>
#include <lz4.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cmath>

#include "FakeBinary.h"

extern "C" {
#include <Shaders/identity.h>
#include <Shaders/lsf.h>
}

using namespace bg3se::mac;

namespace {

int gFailures = 0;
char const* gTest = "";

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; name(); } static void name()

using Bytes = std::vector<uint8_t>;

void Put(Bytes& o, void const* p, size_t n) { o.insert(o.end(), (uint8_t const*)p, (uint8_t const*)p + n); }
template <class T> void Put(Bytes& o, T v) { Put(o, &v, sizeof(v)); }
Bytes Text(std::string_view s) { return Bytes(s.begin(), s.end()); }

// --- packages ----------------------------------------------------------------

struct PakEntrySpec
{
    std::string Name;
    Bytes Data;
    uint8_t Compression{ 0 };
};

// A version 18 package. `claimedCount` overrides the file count in the list header.
Bytes BuildPak(std::vector<PakEntrySpec> const& entries, std::optional<uint32_t> claimedCount = {}, uint8_t priority = 0)
{
    Bytes pak(40, 0);
    Bytes list;
    for (auto const& e : entries) {
        Bytes disk = e.Data;
        if (e.Compression == 2) {
            disk.resize(LZ4_compressBound((int)e.Data.size()));
            disk.resize(LZ4_compress_default((char const*)e.Data.data(), (char*)disk.data(), (int)e.Data.size(), (int)disk.size()));
        }
        uint64_t offset = pak.size();
        Put(pak, disk.data(), disk.size());

        uint8_t raw[272] = {};
        memcpy(raw, e.Name.data(), std::min<size_t>(e.Name.size(), 255));
        uint32_t lo = (uint32_t)offset;
        uint16_t hi = (uint16_t)(offset >> 32);
        memcpy(raw + 256, &lo, 4);
        memcpy(raw + 260, &hi, 2);
        raw[263] = e.Compression;
        uint32_t diskSize = (uint32_t)disk.size(), size = e.Compression ? (uint32_t)e.Data.size() : 0;
        memcpy(raw + 264, &diskSize, 4);
        memcpy(raw + 268, &size, 4);
        Put(list, raw, sizeof(raw));
    }

    Bytes packed(LZ4_compressBound((int)list.size()) + 1);
    int packedSize = list.empty() ? 0 : LZ4_compress_default((char const*)list.data(), (char*)packed.data(), (int)list.size(), (int)packed.size());
    uint64_t listOffset = pak.size();
    Put(pak, claimedCount.value_or((uint32_t)entries.size()));
    Put(pak, (uint32_t)packedSize);
    Put(pak, packed.data(), (size_t)packedSize);

    uint32_t signature = 0x4b50534c, version = 18;
    memcpy(pak.data(), &signature, 4);
    memcpy(pak.data() + 4, &version, 4);
    memcpy(pak.data() + 8, &listOffset, 8);
    pak[21] = priority;
    return pak;
}

std::string gTempDir;

std::string WriteTemp(std::string const& name, Bytes const& data)
{
    std::string path = gTempDir + "/" + name;
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return path;
}

TEST(pak_reads_stored_and_compressed_entries)
{
    Bytes big(5000, 'x');
    auto path = WriteTemp("ok.pak", BuildPak({ { "a/stored.txt", Text("hello") }, { "a/packed.bin", big, 2 }, { "empty", {} } }, {}, 30));
    std::string error;
    auto pak = PakFile::Open(path, &error);
    CHECK(pak != nullptr);
    if (!pak) return;
    CHECK(pak->Entries().size() == 3);
    CHECK(pak->Priority() == 30);
    CHECK(pak->FileName() == "ok.pak");
    auto stored = pak->Find("a/stored.txt");
    CHECK(stored && pak->Read(*stored) == Text("hello"));
    auto packed = pak->Find("a/packed.bin");
    CHECK(packed && pak->Read(*packed) == big);
    CHECK(pak->Find("missing") == nullptr);
}

TEST(pak_refuses_a_file_count_that_overflows)
{
    // 0x00F0F0F1 * 272 wraps to 16 in 32 bits: a 16-byte list must not be read as 15 million entries.
    std::string error;
    auto path = WriteTemp("overflow.pak", BuildPak({}, 0x00F0F0F1u));
    CHECK(PakFile::Open(path, &error) == nullptr);
    CHECK(!error.empty());

    path = WriteTemp("lying.pak", BuildPak({ { "a", Text("1") } }, 1000));
    CHECK(PakFile::Open(path, &error) == nullptr);
}

TEST(pak_refuses_truncated_and_foreign_files)
{
    std::string error;
    CHECK(PakFile::Open(WriteTemp("short.pak", Text("LSPK")), &error) == nullptr);
    CHECK(PakFile::Open(WriteTemp("zip.pak", Bytes(100, 'P')), &error) == nullptr);

    auto pak = BuildPak({ { "a", Text("1") } });
    uint64_t beyond = pak.size() + 100;
    memcpy(pak.data() + 8, &beyond, 8);
    CHECK(PakFile::Open(WriteTemp("listoff.pak", pak), &error) == nullptr);

    auto old = BuildPak({ { "a", Text("1") } });
    uint32_t v15 = 15;
    memcpy(old.data() + 4, &v15, 4);
    CHECK(PakFile::Open(WriteTemp("v15.pak", old), &error) == nullptr);
}

TEST(pak_refuses_an_entry_outside_the_file)
{
    auto data = BuildPak({ { "a", Text("payload") } });
    auto path = WriteTemp("entry.pak", data);
    auto pak = PakFile::Open(path);
    CHECK(pak != nullptr);
    if (!pak) return;
    auto entry = *pak->Find("a");
    entry.Offset = data.size() - 2;
    CHECK(!pak->Read(entry));
    entry = *pak->Find("a");
    entry.Part = 1;
    CHECK(!pak->Read(entry));
}

// --- load order ----------------------------------------------------------------

TEST(load_order_keeps_every_module_in_order)
{
    std::string xml = "<save><node id=\"Mods\"><children>";
    for (int i = 0; i < 200; i++) {
        xml += "<node id=\"ModuleShortDesc\"><attribute id=\"Folder\" type=\"LSString\" value=\"Mod" + std::to_string(i) + "\"/>"
               "<attribute id=\"Name\" type=\"LSString\" value=\"Name " + std::to_string(i) + "\"/></node>";
    }
    xml += "<node id=\"ModuleShortDesc\"><attribute id=\"Folder\" type=\"LSString\" value=\"Fade&apos;s &amp; Co\"/></node></children></node></save>";
    auto order = ParseLoadOrder(xml);
    CHECK(order.size() == 201);
    CHECK(order[0] == "Mod0" && order[199] == "Mod199");
    CHECK(order[200] == "Fade's & Co");
    CHECK(ParseLoadOrder("").empty());
    CHECK(ParseLoadOrder("<attribute id=\"Folder\" value=\"unterminated").empty());
}

// --- code shape ----------------------------------------------------------------

TEST(code_shape_ignores_position_and_sees_changes)
{
    // stp; adrp x8; ldr x1,[x8,#off]; bl; cbz; str w21,[x0,#0x80]; ret
    uint32_t a[] = { 0xa9bf7bfd, 0x90000008 | (0x123 << 5), 0xf9400101 | (0x190 << 10), 0x94000000 | 0x1234, 0x34000040 | (0x11 << 5), 0xb9008015, 0xd65f03c0 };
    uint32_t moved[] = { 0xa9bf7bfd, 0xb0000008 | (0x7ff << 5), 0xf9400101 | (0x021 << 10), 0x94000000 | 0x0042, 0x34000040 | (0x33 << 5), 0xb9008015, 0xd65f03c0 };
    uint32_t changed[] = { 0xa9bf7bfd, 0x90000008 | (0x123 << 5), 0xf9400101 | (0x190 << 10), 0x94000000 | 0x1234, 0x34000040 | (0x11 << 5), 0xb9008815, 0xd65f03c0 };
    auto sa = ShapeToFirstReturn(a), sm = ShapeToFirstReturn(moved), sc = ShapeToFirstReturn(changed);
    CHECK(sa && sm && sc);
    CHECK(sa->Instructions == 7);
    CHECK(*sa == *sm);          // same code at another address, referring to data elsewhere
    CHECK(!(*sa == *sc));       // a store to a different field offset
    uint32_t endless[] = { 0xd503201f, 0xd503201f, 0xd503201f, 0xd503201f };
    CHECK(!ShapeToFirstReturn(endless, 4));
}

TEST(code_helpers_decode_calls_and_page_addresses)
{
    // The real words of GetCameraPitchDegrees+28: adrp x8, <page>; ldr x8, [x8, #0xf40]
    CHECK(PageAddress(0x1034f304c, 0xd002b188, 0xf947a108) == 0x108b25f40);
    CHECK(PageAddress(0x1034f304c, 0xd002b188, 0xf947a128) == 0);       // base is another register
    CHECK(PageAddress(0x1034f304c, 0xf947a108, 0xf947a108) == 0);       // not an adrp
    // CameraSystem::Update+0x2c60: bl UpdateGameCameraBehavior
    CHECK(CallTarget(0x103337570, 0x94000cc2) == 0x10333a878);
    CHECK(CallTarget(0x103337570, 0x14000cc2) == 0);                    // b, not bl

    uint32_t code[] = { 0xd10103ff, 0x94000010, 0xd65f03c0 };
    CHECK(ShapeOfRange(code, 3) == *ShapeToFirstReturn(code));
    CHECK(ShapeOfRange(code, 2).Instructions == 2);
}

// --- settings file -------------------------------------------------------------------

TEST(conf_reads_keys_comments_and_repeats)
{
    char const* text =
        "# comment = 1\n"
        "; gfx_a = 1\n"
        "  GFX_Thread_Idling\t=  yes \r\n"
        "no equals sign\n"
        "gfx_empty =\n"
        "gfx_twice = 0\n"
        "gfx_twice = on\n"
        "gfx_inner = a = b\n";
    CHECK(ConfLookup(text, "gfx_thread_idling") == "yes");
    CHECK(ConfLookup(text, "gfx_a") == std::nullopt);              // commented out
    CHECK(ConfLookup(text, "comment") == std::nullopt);
    CHECK(ConfLookup(text, "gfx_empty") == "");
    CHECK(ConfLookup(text, "gfx_twice") == "on");                   // the last one wins
    CHECK(ConfLookup(text, "gfx_inner") == "a = b");
    CHECK(ConfLookup(text, "gfx_missing") == std::nullopt);
    CHECK(ConfLookup(text, "") == std::nullopt);
    CHECK(ConfLookup("last_line = 1", "last_line") == "1");         // no newline at the end

    std::string longLine = "gfx_long = 1 #" + std::string(600, 'x') + "\n";
    CHECK(ConfLookup(longLine, "gfx_long") == std::nullopt);       // 512 characters or more: ignored
    CHECK(ConfLookup(std::string_view("gfx_nul = 1\n\0gfx_after = 1\n", 26), "gfx_after") == std::nullopt);
}

TEST(conf_flags_accept_only_the_on_values)
{
    for (auto on : { "1", "true", "TRUE", "Yes", "on", "ON" }) CHECK(FlagValue(on));
    for (auto off : { "0", "", "false", "no", "off", "2", "enabled", " 1" }) CHECK(!FlagValue(off));
}

// --- instruction fields -------------------------------------------------------------

TEST(instruction_fields_decode)
{
    CHECK(LoadStoreOffset(0x3d882680) == 0x2090u);   // str q0, [x20, #0x2090]
    CHECK(LoadStoreOffset(0xf9504b28) == 0x2090u);   // ldr x8, [x25, #0x2090]
    CHECK(LoadStoreOffset(0x39402aa8) == 0xau);      // ldrb w8, [x21, #0xa]
    CHECK(LoadStoreOffset(0xb9402a88) == 0x28u);     // ldr w8, [x20, #0x28]
    CHECK(LoadStoreOffset(0xfd50c282) == 0x2180u);   // ldr d2, [x20, #0x2180]
    CHECK(!LoadStoreOffset(0x91002334));             // add
    CHECK(PairOffset(0x29412109) == 8);              // ldp w9, w8, [x8, #0x8]
    CHECK(PairOffset(0xa9bf47f0) == -16);            // stp x16, x17, [sp, #-16]!
    CHECK(!PairOffset(0xf9504b28));
    CHECK(AddImmediate(0x91002334) == 8u);           // add x20, x25, #0x8
    CHECK(AddImmediate(0x9107e2fb) == 0x1f8u);       // add x27, x23, #0x1f8
    CHECK(!AddImmediate(0xf9504b28));
    CHECK(MovzValue(0x52800115) == 8u);              // mov w21, #8
    CHECK(MovzValue(0xd2fff813) == 0xffc0000000000000ull);
    CHECK(MovzMovkValue(0x52836c22, 0x72a981a2) == 0x4c0d1b61u);
    CHECK(!MovzMovkValue(0x52836c22, 0x72a981a3));   // movk of another register
    CHECK(JumpTarget(0x105d07df4, 0x14000082) == 0x105d07ffc);
    CHECK(JumpTarget(0x105d07df4, 0x94000082) == 0);
    CHECK(RegD(0xf9401c39) == 25 && RegN(0xf9401c39) == 1);
    CHECK(WithRegD(0xb9402a88, 9) == 0xb9402a89);
    CHECK(WithRegM(WithRegD(0xf8685a81, 9), 9) == 0xf8695a89);

    // GPUDevice::CreateShader and MetalGPUDevice::CreatePipelineState: the program handle's salt.
    CHECK(LogicalImmediate(0x1200568a) == 0x3fffffu);                  // and w10, w20, #0x3fffff
    CHECK(LogicalImmediate(0xf260555f) == 0x3fffff00000000ull);        // tst x10, #0x3fffff00000000
    CHECK(LogicalImmediate(0x121c6d09) == 0xfffffff0u);                // and w9, w8, #0xfffffff0
    CHECK(LogicalImmediate(0x12000508) == 3u);                         // and w8, w8, #3
    CHECK(!LogicalImmediate(0x91002334));                              // add
    CHECK((BitfieldMoveOf(0xb360557b) == BitfieldMove{ 32, 22, true }));    // bfi x27, x11, #32, #22
    CHECK((BitfieldMoveOf(0xd3605569) == BitfieldMove{ 32, 22, true }));    // ubfiz x9, x11, #32, #22
    CHECK((BitfieldMoveOf(0xb34a253b) == BitfieldMove{ 54, 10, true }));    // bfi x27, x9, #54, #10
    CHECK((BitfieldMoveOf(0xd360d54a) == BitfieldMove{ 32, 22, false }));   // ubfx x10, x10, #32, #22
    CHECK((BitfieldMoveOf(0xd37ef6c9) == BitfieldMove{ 2, 62, true }));     // lsl x9, x22, #2
    CHECK(!BitfieldMoveOf(0x1200568a));
}

// --- searching an image -------------------------------------------------------------

TEST(image_search_finds_patterns_slots_and_imports)
{
    static FakeBinary binary;
    GameImage image;
    CHECK(image.OpenFile(binary.File));
    if (!image.FindVA("_first")) return;

    char const* names[] = { "_first", "_second", "_vtable", "_missing" };
    uint64_t vas[4] = {}, ends[4] = {};
    image.FindVAs(names, vas);
    CHECK(vas[0] == FakeBinary::Text && vas[1] == FakeBinary::Text + 0x40 && vas[2] == FakeBinary::Data && vas[3] == 0);
    image.NextSymbolVAs(vas, ends);
    CHECK(ends[0] == FakeBinary::Text + 0x40 && ends[1] == FakeBinary::Data && ends[2] == FakeBinary::Data + 0x30 && ends[3] == 0);
    auto text = image.TextSymbols();
    CHECK(text.size() == 2 && text[0].VA == FakeBinary::Text && text[0].Name == "_first" && text[1].VA == FakeBinary::Text + 0x40 &&
          text[1].Name == "_second");
    CHECK(image.TextBegin() == FakeBinary::Text && image.TextEnd() == FakeBinary::Text + 0x100);

    // ldr x8,[x19,#?]; add x0,x8,#? matches twice; with the add's offset pinned, once.
    MaskedWord loose[] = { { 0xf9400268, Any::Imm12 }, { 0x91000100, Any::Imm12 } };
    MaskedWord pinned[] = { { 0xf9400268, Any::Imm12 }, { 0x9107c100 } };
    CHECK(FindPattern(image, vas[0], ends[0], loose).size() == 2);
    auto one = FindPattern(image, vas[0], ends[0], pinned);
    CHECK(one.size() == 1 && one[0] == FakeBinary::Text);
    CHECK(FindPattern(image, vas[0], FakeBinary::Text + 4, pinned).empty());   // must fit in the range

    CHECK(CallsImport(image, FakeBinary::Text + 8, "_longjmp"));
    CHECK(!CallsImport(image, FakeBinary::Text + 8, "_setjmp"));
    CHECK(CallsImport(image, FakeBinary::Text + 0x40, "_setjmp"));
    CHECK(!CallsImport(image, FakeBinary::Text + 0x44, "_first"));             // not a stub
    CHECK(CallsTo(image, vas[1], ends[1], vas[0]) == std::vector<uint64_t>{ FakeBinary::Text + 0x44 });

    CHECK(VtableSlotsOf(image, vas[2], ends[2], vas[1]) == std::vector<uint64_t>{ FakeBinary::Data + 0x18 });
    CHECK(VtableSlotsOf(image, vas[2], ends[2], vas[0]).size() == 2);
    CHECK(image.DataAt(FakeBinary::Data + 0x7f8, 8) != nullptr);
    CHECK(image.DataAt(FakeBinary::Data + 0x7fc, 8) == nullptr);   // past the segment's file contents
    CHECK(!image.PointersOnlyAtRunTime());

    // With chained fixups the same file has no slot that can be read.
    static FakeBinary chained(true);
    GameImage chainedImage;
    CHECK(chainedImage.OpenFile(chained.File));
    CHECK(chainedImage.PointersOnlyAtRunTime());
    CHECK(!chainedImage.PointerAt(FakeBinary::Data + 0x18));
    CHECK(VtableSlotsOf(chainedImage, vas[2], ends[2], vas[1]).empty());
    CHECK(FindPattern(chainedImage, vas[0], ends[0], pinned).size() == 1);   // code is unaffected
}

// A name looked up alone, through the index, is what the one pass over the table gives it.
TEST(image_lookup_of_one_name_is_that_of_a_pass)
{
    static FakeBinary binary(false, {
        { "_twice", FakeBinary::Text + 0x80, N_SECT | N_EXT, 1 },
        { "_twice", FakeBinary::Text + 0x80, N_SECT, 1 },
        { "_two", FakeBinary::Text + 0x84, N_SECT | N_EXT, 1 },
        { "_two", FakeBinary::Text + 0x88, N_SECT, 1 },
        { "_debug", FakeBinary::Text + 0x8c, N_FUN, 1 },
        { "_low", FakeBinary::Base - 0x10, N_SECT, 1 },
    });
    GameImage image;
    CHECK(image.OpenFile(binary.File));
    char const* names[] = { "_first", "_second", "_vtable", "_after", "_twice", "_two", "_debug", "_low", "_longjmp", "_missing",
        "_firs", "_first_", "" };
    GameImage::Symbol found[std::size(names)];
    image.Lookup(names, found);
    for (size_t n = 0; n < std::size(names); n++) {
        uint64_t pass = found[n].Status == GameImage::SymbolStatus::Found ? found[n].VA : 0;
        CHECK(image.FindVA(names[n]) == pass);
    }
    CHECK(image.FindVA("_first") == FakeBinary::Text && image.FindVA("_vtable") == FakeBinary::Data);
    CHECK(image.FindVA("_twice") == FakeBinary::Text + 0x80);
    CHECK(found[5].Status == GameImage::SymbolStatus::Ambiguous && image.FindVA("_two") == 0);
    CHECK(image.FindVA("_debug") == 0 && image.FindVA("_low") == 0 && image.FindVA("_longjmp") == 0);
    CHECK(image.FindVA(nullptr) == 0);
    // Asked before it is opened, an image is asked of its table once it is.
    GameImage later;
    CHECK(later.FindVA("_first") == 0);
    CHECK(later.OpenFile(binary.File) && later.FindVA("_first") == FakeBinary::Text);
}

// --- material files ----------------------------------------------------------------

struct TNode { char const* Name; int32_t Parent; };
struct TAttr { int Node; char const* Name; uint32_t Type; int32_t Int; char const* String; };

// An LSOF v7 document in the long layout with one string bucket, as lsf.c reads it.
Bytes BuildLsf(std::vector<TNode> const& nodes, std::vector<TAttr> const& attrs)
{
    std::vector<std::string> names;
    auto ref = [&](char const* s) {
        auto it = std::find(names.begin(), names.end(), s);
        if (it == names.end()) { names.emplace_back(s); return (uint32_t)(names.size() - 1); }
        return (uint32_t)(it - names.begin());
    };
    for (auto const& n : nodes) ref(n.Name);
    for (auto const& a : attrs) ref(a.Name);

    Bytes str, nod, att, val;
    Put(str, (uint32_t)1);
    Put(str, (uint16_t)names.size());
    for (auto const& n : names) { Put(str, (uint16_t)n.size()); Put(str, n.data(), n.size()); }

    for (int i = 0; i < (int)nodes.size(); i++) {
        int first = -1;
        for (int a = 0; a < (int)attrs.size(); a++) if (attrs[a].Node == i) { first = a; break; }
        Put(nod, ref(nodes[i].Name));
        Put(nod, (uint32_t)nodes[i].Parent);
        Put(nod, 0xffffffffu);
        Put(nod, (uint32_t)first);
    }
    for (int a = 0; a < (int)attrs.size(); a++) {
        int next = -1;
        for (int b = a + 1; b < (int)attrs.size(); b++) if (attrs[b].Node == attrs[a].Node) { next = b; break; }
        uint32_t len = attrs[a].Type == LSF_T_STRING ? (uint32_t)strlen(attrs[a].String) + 1 : attrs[a].Type == LSF_T_INT8 ? 1 : 4;
        Put(att, ref(attrs[a].Name));
        Put(att, (len << 6) | attrs[a].Type);
        Put(att, (uint32_t)next);
        Put(att, (uint32_t)val.size());
        if (attrs[a].Type == LSF_T_STRING) Put(val, attrs[a].String, len);
        else if (attrs[a].Type == LSF_T_INT8) Put(val, (uint8_t)attrs[a].Int);
        else Put(val, (uint32_t)attrs[a].Int);
    }

    Bytes o;
    Put(o, "LSOF", 4);
    Put(o, (uint32_t)7);
    Put(o, (uint64_t)0);
    uint32_t meta[10] = { (uint32_t)str.size(), 0, 0, 0, (uint32_t)nod.size(), 0, (uint32_t)att.size(), 0, (uint32_t)val.size(), 0 };
    Put(o, meta, sizeof(meta));
    Put(o, (uint16_t)0);    // no compression
    Put(o, (uint16_t)0);
    Put(o, (uint32_t)1);    // long layout
    Put(o, str.data(), str.size());
    Put(o, nod.data(), nod.size());
    Put(o, att.data(), att.size());
    Put(o, val.data(), val.size());
    return o;
}

// A Windows-packed material and its Mac twin: MetalReady, no StaticDeferred, a
// larger engine CB, a MaterialCBSize binding node, other Dx indices.
Bytes ModMaterial()
{
    return BuildLsf(
        { { "Material", -1 }, { "ShaderDescriptions", 0 }, { "ShaderDesc_StaticForward", 1 }, { "EngineCBBinding", 2 },
          { "Texture2DParameters", 0 }, { "StaticForward", 4 }, { "StaticDeferred", 4 } },
        { { 2, "EngineCBSize", LSF_T_INT, 160, nullptr },
          { 3, "DxVsIndex", LSF_T_INT, 0, nullptr }, { 3, "DxPsIndex", LSF_T_INT, 0, nullptr },
          { 3, "VkDescriptorSet", LSF_T_INT, 1, nullptr }, { 3, "VkBindingIndex", LSF_T_INT, 0, nullptr },
          { 4, "UniformName", LSF_T_STRING, 0, "Texture2DParameter_Mask" },
          { 5, "DxVsIndex", LSF_T_INT, -1, nullptr }, { 5, "DxPsIndex", LSF_T_INT, 3, nullptr },
          { 5, "VkDescriptorSet", LSF_T_INT, 1, nullptr }, { 5, "VkBindingIndex", LSF_T_INT, 2, nullptr },
          { 6, "DxPsIndex", LSF_T_INT, 1, nullptr }, { 6, "VkDescriptorSet", LSF_T_INT, 1, nullptr },
          { 6, "VkBindingIndex", LSF_T_INT, 5, nullptr } });
}

Bytes MacMaterial()
{
    return BuildLsf(
        { { "Material", -1 }, { "ShaderDescriptions", 0 }, { "ShaderDesc_StaticForward", 1 }, { "EngineCBBinding", 2 },
          { "MaterialCBSize", 2 }, { "Texture2DParameters", 0 }, { "StaticForward", 5 } },
        { { 0, "MetalReady", LSF_T_INT, 1, nullptr },
          { 2, "EngineCBSize", LSF_T_INT, 176, nullptr },
          { 3, "DxVsIndex", LSF_T_INT, 2, nullptr }, { 3, "DxPsIndex", LSF_T_INT, 2, nullptr },
          { 3, "VkDescriptorSet", LSF_T_INT, 7, nullptr }, { 3, "VkBindingIndex", LSF_T_INT, 7, nullptr },
          { 4, "DxVsIndex", LSF_T_INT, 1, nullptr }, { 4, "DxPsIndex", LSF_T_INT, 1, nullptr },
          { 5, "UniformName", LSF_T_STRING, 0, "Texture2DParameter_Mask" },
          { 6, "DxVsIndex", LSF_T_INT, -1, nullptr }, { 6, "DxPsIndex", LSF_T_INT, 4, nullptr },
          { 6, "VkDescriptorSet", LSF_T_INT, 7, nullptr }, { 6, "VkBindingIndex", LSF_T_INT, 7, nullptr } });
}

// --- the same shader, compiled again ---------------------------------------------

// A fragment shader, out = (in + ubo.v) * 0.5, assembled here. The variants
// are what a second compilation changes (ids, the order of declarations,
// names, a capability the shader does not use) and what an edit changes.
struct SpirvVariant
{
    bool Renumber{ false };
    bool Reorder{ false };
    bool Extras{ false };
    uint32_t Constant{ 0x3f000000 };    // 0.5f
    uint32_t Binding{ 2 };
    bool SwapOperands{ false };
    bool Unknown{ false };
    bool Statics{ false };          // two private variables of one type
    bool SwapStatics{ false };
    bool Rotation{ false };         // also adds in.xy turned by a 2x2 matrix built from scalars
    bool Precise{ false };          // with Rotation: that product must not be contracted
};

Bytes SpirvModule(SpirvVariant v = {})
{
    std::vector<uint32_t> w = { 0x07230203, 0x00010300, 0x000e0000, 41, 0 };
    auto id = [&](uint32_t n) { return v.Renumber ? 40 - n : n; };
    auto op = [&](uint16_t opcode, std::initializer_list<uint32_t> operands) {
        w.push_back((uint32_t)(operands.size() + 1) << 16 | opcode);
        w.insert(w.end(), operands);
    };
    enum { Void = 1, Fn, Float, V4, POut, PIn, Out, In, Half, Struct, PUbo, Ubo, Int, Zero, PV4, Main, Label, A, P, B, Sum, Scaled,
           PPrivate, First, Second, V2, M2, Quarter, Eighth, Column0, Column1, Matrix, Xy, Turned, Wide, Total };
    constexpr uint32_t kMain = 0x6e69616d;     // "main"

    op(17, { 1 });                                                  // OpCapability Shader
    if (v.Extras) {
        op(17, { 5302 });                                           // a capability nothing uses
        op(10, { 0x5f565053, 0x00545845 });                         // OpExtension "SPV_EXT"
    }
    op(14, { 0, 1 });                                               // OpMemoryModel Logical GLSL450
    if (v.Reorder) op(15, { 4, id(Main), kMain, 0, id(In), id(Out) });
    else op(15, { 4, id(Main), kMain, 0, id(Out), id(In) });        // OpEntryPoint Fragment
    op(16, { id(Main), 7 });                                        // OpExecutionMode OriginUpperLeft
    if (v.Extras) op(5, { id(Ubo), 0x006f6275 });                   // OpName "ubo"

    auto decorations = [&](bool reversed) {
        std::vector<std::function<void()>> d = {
            [&] { op(71, { id(Out), 30, 0 }); },                    // Location
            [&] { op(71, { id(In), 30, 1 }); },
            [&] { op(71, { id(Ubo), 34, 1 }); },                    // DescriptorSet
            [&] { op(71, { id(Ubo), 33, v.Binding }); },            // Binding
            [&] { op(72, { id(Struct), 0, 35, 0 }); },              // Offset
            [&] { op(71, { id(Struct), 2 }); },                     // Block
        };
        if (reversed) std::reverse(d.begin(), d.end());
        for (auto& one : d) one();
    };
    decorations(v.Reorder);
    if (v.Rotation && v.Precise) op(71, { id(Turned), 42 });        // NoContraction

    op(19, { id(Void) });
    op(33, { id(Fn), id(Void) });
    op(22, { id(Float), 32 });
    if (v.Reorder) op(43, { id(Float), id(Half), v.Constant });
    op(23, { id(V4), id(Float), 4 });
    op(21, { id(Int), 32, 1 });
    op(43, { id(Int), id(Zero), 0 });
    op(30, { id(Struct), id(V4) });
    op(32, { id(PUbo), 2, id(Struct) });
    op(32, { id(PV4), 2, id(V4) });
    op(32, { id(POut), 3, id(V4) });
    op(32, { id(PIn), 1, id(V4) });
    if (v.Reorder) {
        op(59, { id(PUbo), id(Ubo), 2 });
        op(59, { id(PIn), id(In), 1 });
        op(59, { id(POut), id(Out), 3 });
    } else {
        op(59, { id(POut), id(Out), 3 });
        op(59, { id(PIn), id(In), 1 });
        op(59, { id(PUbo), id(Ubo), 2 });
        op(43, { id(Float), id(Half), v.Constant });
    }
    if (v.Unknown) op(4999, { id(Float) });
    if (v.Rotation) {
        op(23, { id(V2), id(Float), 2 });
        op(24, { id(M2), id(V2), 2 });
        op(43, { id(Float), id(Quarter), 0x3e800000 });
        op(43, { id(Float), id(Eighth), 0x3e000000 });
    }
    if (v.Statics) {
        op(32, { id(PPrivate), 6, id(V4) });
        op(59, { id(PPrivate), id(First), 6 });
        op(59, { id(PPrivate), id(Second), 6 });
    }

    op(54, { id(Void), id(Main), 0, id(Fn) });
    op(248, { id(Label) });
    op(61, { id(V4), id(A), id(In) });
    op(65, { id(PV4), id(P), id(Ubo), id(Zero) });
    op(61, { id(V4), id(B), id(P) });
    if (v.SwapOperands) op(129, { id(V4), id(Sum), id(B), id(A) });
    else op(129, { id(V4), id(Sum), id(A), id(B) });                // OpFAdd
    op(142, { id(V4), id(Scaled), id(Sum), id(Half) });             // OpVectorTimesScalar
    if (v.Rotation) {
        op(80, { id(V2), id(Column0), id(Half), id(Quarter) });         // OpCompositeConstruct
        op(80, { id(V2), id(Column1), id(Eighth), id(Half) });
        op(80, { id(M2), id(Matrix), id(Column0), id(Column1) });
        op(79, { id(V2), id(Xy), id(A), id(A), 0, 1 });                 // OpVectorShuffle
        op(144, { id(V2), id(Turned), id(Xy), id(Matrix) });            // OpVectorTimesMatrix
        op(80, { id(V4), id(Wide), id(Turned), id(Turned) });
        op(129, { id(V4), id(Total), id(Scaled), id(Wide) });
        op(62, { id(Out), id(Total) });
    } else {
        op(62, { id(Out), id(Scaled) });
    }
    if (v.Statics) {
        op(62, { id(v.SwapStatics ? Second : First), id(A) });
        op(62, { id(v.SwapStatics ? First : Second), id(B) });
    }
    op(253, {});
    op(56, {});

    Bytes bytes(w.size() * 4);
    memcpy(bytes.data(), w.data(), bytes.size());
    return bytes;
}

std::optional<SpirvHash> HashOf(Bytes const& module)
{
    SpirvHash hash;
    std::string why;
    if (!SpirvModuleHash(module, hash, why)) return {};
    return hash;
}

TEST(spirv_hash_is_of_what_the_shader_does)
{
    auto base = HashOf(SpirvModule());
    CHECK(base.has_value());

    // What a second compilation changes.
    CHECK(HashOf(SpirvModule({ .Renumber = true })) == base);
    CHECK(HashOf(SpirvModule({ .Reorder = true })) == base);
    CHECK(HashOf(SpirvModule({ .Extras = true })) == base);
    CHECK(HashOf(SpirvModule({ .Renumber = true, .Reorder = true, .Extras = true })) == base);
    CHECK(SpirvModule({ .Renumber = true, .Reorder = true }) != SpirvModule());

    // What an edit changes.
    auto constant = HashOf(SpirvModule({ .Constant = 0x3f800000 }));
    auto binding = HashOf(SpirvModule({ .Binding = 3 }));
    auto swapped = HashOf(SpirvModule({ .SwapOperands = true }));
    CHECK(constant && binding && swapped);
    CHECK(constant != base && binding != base && swapped != base && constant != binding && binding != swapped);

    // What cannot be read with certainty is refused.
    SpirvHash hash;
    std::string why;
    CHECK(!SpirvModuleHash(SpirvModule({ .Unknown = true }), hash, why) && why.find("4999") != std::string::npos);
    // Two places nothing tells apart: which one a store goes to cannot be compared.
    CHECK(SpirvModule({ .Statics = true }) != SpirvModule({ .Statics = true, .SwapStatics = true }));
    CHECK(!SpirvModuleHash(SpirvModule({ .Statics = true }), hash, why) && why.find("variables") != std::string::npos);
    auto cut = SpirvModule();
    cut.resize(cut.size() - 8);
    CHECK(!SpirvModuleHash(cut, hash, why));
    auto wrong = SpirvModule();
    wrong[0] ^= 1;
    CHECK(!SpirvModuleHash(wrong, hash, why));
    auto wild = SpirvModule();
    wild[wild.size() - 12] = 0x18;          // the stored value: an id nothing defines
    CHECK(!SpirvModuleHash(wild, hash, why));
    CHECK(!SpirvModuleHash({}, hash, why));
}

// A Vulkan container as the Windows toolkit writes it: a reflection table and
// one SHD8 chunk per stage.
Bytes VulkanContainer(std::string const& name, Bytes const& reflection, Bytes const& fragment, bool chunked = true)
{
    Bytes stage;
    if (chunked) {
        Put(stage, "SHD8", 4);
        Put(stage, (uint16_t)0x6e);
        Put(stage, (uint16_t)0x1c);
        Put(stage, (uint16_t)4);
        Put(stage, (uint32_t)fragment.size());
        Put(stage, (uint32_t)0);
        uint8_t digest[32] = {};
        digest[0] = fragment[20];           // stands for the toolkit's hash of the module's bytes
        Put(stage, digest, sizeof(digest));
    }
    Put(stage, fragment.data(), fragment.size());

    Bytes o;
    Put(o, "BSHD", 4);
    Put(o, (uint32_t)2);
    uint32_t padded = (uint32_t)((name.size() + 4) & ~(size_t)3);
    Put(o, padded);
    Put(o, (uint32_t)name.size());
    Bytes nameBytes(padded);
    memcpy(nameBytes.data(), name.data(), name.size());
    Put(o, nameBytes.data(), nameBytes.size());
    uint32_t sizes[7] = { (uint32_t)reflection.size(), 0, 0, 0, 0, (uint32_t)stage.size(), 0 };
    Put(o, sizes, sizeof(sizes));
    Put(o, reflection.data(), reflection.size());
    Put(o, stage.data(), stage.size());
    return o;
}

TEST(container_hash_takes_a_recompiled_shader_for_the_same)
{
    auto shipped = VulkanContainer("M_ST_FOR_Vulkan", Text("refl"), SpirvModule());
    auto recompiled = VulkanContainer("M_ST_FOR_Vulkan", Text("refl"), SpirvModule({ .Renumber = true, .Reorder = true, .Extras = true }));
    CHECK(shipped != recompiled);

    ContainerParts parts;
    CHECK(ParseContainer(shipped, parts));
    CHECK(parts.Version == 2 && parts.Reflection.size() == 4 && parts.Stages[0].empty() && !parts.Stages[4].empty());
    VulkanStageHeader header;
    std::span<uint8_t const> module;
    CHECK(SplitVulkanStage(parts.Stages[4], header, module) && header[0] == 0x6e && module.size() == SpirvModule().size());

    uint32_t structural = 0;
    auto hash = ContainerHash(shipped, &structural);
    CHECK(structural == 1);
    CHECK(ContainerHash(recompiled) == hash);

    // Another shader, another table, another name, another chunk header: other containers.
    CHECK(ContainerHash(VulkanContainer("M_ST_FOR_Vulkan", Text("refl"), SpirvModule({ .Constant = 1 }))) != hash);
    CHECK(ContainerHash(VulkanContainer("M_ST_FOR_Vulkan", Text("rEfl"), SpirvModule())) != hash);
    CHECK(ContainerHash(VulkanContainer("N_ST_FOR_Vulkan", Text("refl"), SpirvModule())) != hash);
    size_t chunk = shipped.size() - SpirvModule().size() - 50;
    for (size_t at : { 4, 9, 14, 17, 34, 49 }) {
        auto other = shipped;
        other[chunk + at] ^= 1;
        CHECK(ContainerHash(other) != hash);
    }
    // The toolkit's hash of the module's bytes is not part of it: the module is compared itself.
    auto digest = shipped;
    digest[chunk + 18] ^= 1;
    CHECK(ContainerHash(digest) == hash);

    // A stage that is not a readable module counts by its bytes: identical bytes still match, nothing else does.
    auto unknown = VulkanContainer("M_ST_FOR_Vulkan", Text("refl"), SpirvModule({ .Unknown = true }));
    CHECK(ContainerHash(unknown, &structural) == ContainerHash(unknown) && structural == 0);
    CHECK(ContainerHash(VulkanContainer("M_ST_FOR_Vulkan", Text("refl"), SpirvModule({ .Renumber = true, .Unknown = true })))
        != ContainerHash(unknown));
    auto bare = VulkanContainer("M_ST_FOR_Vulkan", Text("refl"), SpirvModule(), false);
    CHECK(ContainerHash(bare, &structural) != hash && structural == 0);

    // Not a container at all: by its bytes.
    CHECK(!ParseContainer(Text("AAA"), parts));
    CHECK(ContainerHash(Text("AAA")) == ContainerHash(Text("AAA")) && ContainerHash(Text("AAA")) != ContainerHash(Text("AAB")));
    auto truncated = shipped;
    truncated.pop_back();
    CHECK(!ParseContainer(truncated, parts));

    // The table's key is this hash.
    uint8_t a[16], b[16];
    identity_hash(shipped.data(), shipped.size(), a);
    identity_hash(recompiled.data(), recompiled.size(), b);
    CHECK(memcmp(a, b, sizeof(a)) == 0 && memcmp(a, hash.data(), sizeof(a)) == 0);
}

// --- a Windows shader's inputs, placed where the Mac game has them ----------------

struct ParsedLsfDoc
{
    LsfDoc Doc{};
    bool Ok{ false };
    explicit ParsedLsfDoc(Bytes const& bytes)
    {
        char err[256] = "";
        Ok = lsf_parse(bytes.data(), bytes.size(), &Doc, err, sizeof(err));
    }
    ~ParsedLsfDoc() { if (Ok) lsf_free(&Doc); }
};

// A DirectX 11 vertex stage as the toolkit writes it: a SHD2 chunk around a
// DXBC container with one part, the input signature.
Bytes Dx11VertexStage(std::vector<std::tuple<std::string, uint32_t, uint32_t>> const& elements /* name, index, system value */)
{
    Bytes names, table;
    uint32_t namesAt = 8 + (uint32_t)elements.size() * 24;
    for (auto const& [name, index, systemValue] : elements) {
        uint32_t fields[6] = { namesAt + (uint32_t)names.size(), index, systemValue, 3, 0, 0 };
        Put(table, fields, sizeof(fields));
        Put(names, name.c_str(), name.size() + 1);
    }
    Bytes part;
    Put(part, (uint32_t)elements.size());
    Put(part, (uint32_t)8);
    Put(part, table.data(), table.size());
    Put(part, names.data(), names.size());

    Bytes dxbc;
    Put(dxbc, "DXBC", 4);
    uint8_t digest[16] = {};
    Put(dxbc, digest, sizeof(digest));
    Put(dxbc, (uint32_t)1);
    Put(dxbc, (uint32_t)(36 + 8 + part.size()));
    Put(dxbc, (uint32_t)1);
    Put(dxbc, (uint32_t)36);
    Put(dxbc, "ISGN", 4);
    Put(dxbc, (uint32_t)part.size());
    Put(dxbc, part.data(), part.size());

    Bytes stage;
    Put(stage, "SHD2", 4);
    uint8_t header[14] = {};
    Put(stage, header, sizeof(header));
    Put(stage, dxbc.data(), dxbc.size());
    return stage;
}

TEST(input_signature_names_the_vertex_inputs_in_location_order)
{
    auto stage = Dx11VertexStage({ { "POSITION", 0, 0 }, { "SV_VertexID", 0, 6 }, { "TEXCOORD", 1, 0 }, { "COLOR", 0, 0 } });
    std::vector<InputSemantic> inputs;
    std::string why;
    CHECK(ReadInputSignature(stage, inputs, why));
    CHECK(inputs.size() == 3);      // the system value is no vertex data and has no location
    CHECK(inputs[0].Name == "POSITION" && inputs[1].Name == "TEXCOORD" && inputs[1].Index == 1 && inputs[2].Name == "COLOR");

    auto cut = stage;
    cut.resize(cut.size() - 6);
    CHECK(!ReadInputSignature(cut, inputs, why));
    auto other = stage;
    other[3] = '8';
    CHECK(!ReadInputSignature(other, inputs, why));
    auto wild = stage;
    wild[18 + 36 + 8 + 8] = 0xff;      // the first element's name is past the part
    wild[18 + 36 + 8 + 9] = 0xff;
    CHECK(!ReadInputSignature(wild, inputs, why));
    CHECK(!ReadInputSignature({}, inputs, why));
}

TEST(material_slots_are_what_the_material_declares_for_a_pass)
{
    auto material = BuildLsf(
        { { "Material", -1 }, { "ShaderDescriptions", 0 }, { "ShaderDesc_StaticForward", 1 }, { "EngineCBBinding", 2 }, { "MaterialCBSize", 2 },
          { "Texture2DParameters", 0 }, { "StaticForward", 5 }, { "StaticDepth", 5 },
          { "VirtualTextureParameters", 0 }, { "StaticForward", 8 }, { "TranslationTable", 9 }, { "Cache1", 9 },
          { "SamplerStateParameters", 0 }, { "StaticForward", 12 } },
        { { 3, "DxVsIndex", LSF_T_INT, 0, nullptr }, { 3, "DxPsIndex", LSF_T_INT, 0, nullptr },
          { 3, "VkDescriptorSet", LSF_T_INT, 1, nullptr }, { 3, "VkBindingIndex", LSF_T_INT, 0, nullptr },
          { 4, "DxVsIndex", LSF_T_INT, -1, nullptr }, { 4, "DxPsIndex", LSF_T_INT, 1, nullptr },
          { 4, "VkDescriptorSet", LSF_T_INT, 1, nullptr }, { 4, "VkBindingIndex", LSF_T_INT, 1, nullptr },
          { 5, "UniformName", LSF_T_STRING, 0, "Texture2DParameter_Mask" },
          { 6, "DxVsIndex", LSF_T_INT, -1, nullptr }, { 6, "DxPsIndex", LSF_T_INT, 3, nullptr },
          { 6, "VkDescriptorSet", LSF_T_INT, 1, nullptr }, { 6, "VkBindingIndex", LSF_T_INT, 5, nullptr },
          { 7, "DxPsIndex", LSF_T_INT, 9, nullptr }, { 7, "VkDescriptorSet", LSF_T_INT, 1, nullptr }, { 7, "VkBindingIndex", LSF_T_INT, 9, nullptr },
          { 8, "UniformName", LSF_T_STRING, 0, "VirtualTexture" },
          { 10, "DxPsIndex", LSF_T_INT, 0, nullptr }, { 10, "VkDescriptorSet", LSF_T_INT, 1, nullptr }, { 10, "VkBindingIndex", LSF_T_INT, 3, nullptr },
          { 11, "DxPsIndex", LSF_T_INT, 2, nullptr }, { 11, "VkDescriptorSet", LSF_T_INT, 1, nullptr }, { 11, "VkBindingIndex", LSF_T_INT, 4, nullptr },
          { 12, "UniformName", LSF_T_STRING, 0, "_DefaultWrapSampler" },
          { 13, "DxPsIndex", LSF_T_INT, 15, nullptr }, { 13, "VkDescriptorSet", LSF_T_INT, -1, nullptr },
          { 13, "VkBindingIndex", LSF_T_INT, -1, nullptr } });
    ParsedLsfDoc doc(material);
    CHECK(doc.Ok);

    CHECK(PassNodeName("ST_FOR") == "StaticForward" && PassNodeName("STI_DEPST") == "StaticInstancedDepthShadowTetrahedron");
    CHECK(PassNodeName("ST_XYZ").empty());

    auto slots = ReadMaterialSlots(doc.Doc, "ST_FOR");
    auto find = [&](char const* name) {
        auto it = std::find_if(slots.begin(), slots.end(), [&](auto const& s) { return s.Name == name; });
        return it == slots.end() ? nullptr : &*it;
    };
    CHECK(slots.size() == 5);       // the sampler has no Vulkan binding: the pass does not use it
    auto engine = find("CB_EngineParams"), constants = find("CB_MaterialConstants"), mask = find("Texture2DParameter_Mask");
    CHECK(engine && engine->Set == 1 && engine->Binding == 0 && engine->DxVs == 0);
    CHECK(constants && constants->Binding == 1 && constants->DxVs == -1 && constants->DxPs == 1);
    CHECK(mask && mask->Binding == 5 && mask->DxPs == 3);
    auto table = find("VirtualTexture_Pagetable"), cache = find("VirtualTexture_Cache1");
    CHECK(table && table->Binding == 3 && table->DxPs == 0 && cache && cache->Binding == 4 && cache->DxPs == 2);

    CHECK(ReadMaterialSlots(doc.Doc, "ST_DEP").size() == 1);        // only the texture, at its binding for that pass
    CHECK(ReadMaterialSlots(doc.Doc, "ST_DEP")[0].Binding == 9);
    CHECK(ReadMaterialSlots(doc.Doc, "ST_VEL").empty() && ReadMaterialSlots(doc.Doc, "nonsense").empty());
    CHECK(engine && engine->Kind == 'b' && constants->Kind == 'b' && mask && mask->Kind == 't' && table && table->Kind == 't');

    // What the material names for the pass with a register and no Vulkan place is kept apart.
    auto unplaced = ReadUnplacedSlots(doc.Doc, "ST_FOR");
    CHECK(unplaced.size() == 1 && unplaced[0].Name == "_DefaultWrapSampler" && unplaced[0].Kind == 's' && unplaced[0].DxPs == 15
        && unplaced[0].DxVs == -1);
    CHECK(ReadUnplacedSlots(doc.Doc, "ST_DEP").empty() && ReadUnplacedSlots(doc.Doc, "nonsense").empty());
}

MacResource MacBlock(std::string name, uint32_t index, std::vector<BlockMember> members)
{
    MacResource r;
    r.Name = std::move(name);
    r.Kind = ResourceKind::Buffer;
    r.Index = index;
    r.Type = "block";
    r.Members = std::move(members);
    r.Size = r.Members.empty() ? 0 : r.Members.back().Offset + 16;
    r.Used = true;
    return r;
}

// A DirectX 11 stage as the toolkit writes it, with its program alone: `words` are the instructions after the
// program's version and length.
Bytes Dx11ProgramStage(bool vertex, std::vector<uint32_t> const& words)
{
    Bytes part;
    Put(part, (uint32_t)((vertex ? 1u : 0u) << 16 | 0x50));
    Put(part, (uint32_t)(words.size() + 2));
    Put(part, words.data(), words.size() * 4);

    Bytes dxbc;
    Put(dxbc, "DXBC", 4);
    uint8_t digest[16] = {};
    Put(dxbc, digest, sizeof(digest));
    Put(dxbc, (uint32_t)1);
    Put(dxbc, (uint32_t)(36 + 8 + part.size()));
    Put(dxbc, (uint32_t)1);
    Put(dxbc, (uint32_t)36);
    Put(dxbc, "SHEX", 4);
    Put(dxbc, (uint32_t)part.size());
    Put(dxbc, part.data(), part.size());

    // A vertex stage has a hash of eight bytes before its container that a pixel stage does not.
    Bytes stage;
    Put(stage, "SHD2", 4);
    uint8_t header[14] = {};
    Put(stage, header, vertex ? 14 : 6);
    Put(stage, dxbc.data(), dxbc.size());
    return stage;
}

TEST(a_directx_stage_says_at_which_registers_it_has_textures_and_samplers)
{
    constexpr uint32_t kTexture = 0x00107000, kSampler = 0x00106000;        // an operand with one index, said outright
    std::vector<uint32_t> program = {
        0x0100006a,                                                 // global flags: nothing to it
        0x0300005a, kSampler, 14,                                   // a sampler at 14
        0x04003058, kTexture, 30, 0x5555,                           // a cube at 30
        0x00000035, 3, 0xdeadbeef,                                  // custom data, with its length in its second word
        0x04001858, kTexture, 0, 0x5555,                            // a 2d texture at 0
        0x0300005a, kSampler, 1,
        0x04004058, kTexture, 3, 0x5555,                            // an array of 2d textures at 3
        0x0100003e,                                                 // ret
    };
    StageRegisters registers;
    std::string why;
    CHECK(ReadStageRegisters(Dx11ProgramStage(false, program), false, registers, why));
    CHECK(registers.Samplers == std::vector<uint32_t>({ 1, 14 }) && registers.Textures.size() == 3);
    CHECK(registers.Textures.size() == 3 && registers.Textures[0].Register == 0 && registers.Textures[0].Type == "2d"
        && registers.Textures[1].Register == 3 && registers.Textures[1].Type == "2darray"
        && registers.Textures[2].Register == 30 && registers.Textures[2].Type == "cube");

    CHECK(ReadStageRegisters(Dx11ProgramStage(true, program), true, registers, why) && registers.Textures.size() == 3);

    // A stage of the other kind, a program cut short, an index said another way, and what is no stage at all.
    CHECK(!ReadStageRegisters(Dx11ProgramStage(false, program), true, registers, why) && why.find("vertex") != std::string::npos);
    auto cut = program;
    cut.resize(3);
    CHECK(!ReadStageRegisters(Dx11ProgramStage(false, cut), false, registers, why));
    auto other = program;
    other[2] = kSampler | (1u << 22);
    CHECK(!ReadStageRegisters(Dx11ProgramStage(false, other), false, registers, why));
    CHECK(!ReadStageRegisters(Dx11VertexStage({ { "POSITION", 0, 0 } }), true, registers, why) && why.find("no program") != std::string::npos);
    CHECK(!ReadStageRegisters({}, false, registers, why));
}

// A fragment stage that samples three textures of the material's own set, at bindings 4, 8 and 9, through one
// sampler at binding 2, and writes their sum.
Bytes SpirvThreeTexturesModule()
{
    std::vector<uint32_t> w = { 0x07230203, 0x00010300, 0x000e0000, 40, 0 };
    auto op = [&](uint16_t opcode, std::initializer_list<uint32_t> operands) {
        w.push_back((uint32_t)(operands.size() + 1) << 16 | opcode);
        w.insert(w.end(), operands);
    };
    enum { Void = 1, Fn, Float, V4, V2, POut, Out, Image, PImage, Sampler, PSampler, Sampled, T0, T1, T2, S, Zero, Uv, Main, Label,
           Loaded /* four ids a texture */, Sum1 = Loaded + 12, Sum2 };
    constexpr uint32_t kMain = 0x6e69616d;     // "main"
    op(17, { 1 });                                                  // OpCapability Shader
    op(14, { 0, 1 });                                               // OpMemoryModel Logical GLSL450
    op(15, { 4, Main, kMain, 0, Out });                             // OpEntryPoint Fragment
    op(16, { Main, 7 });                                            // OpExecutionMode OriginUpperLeft
    op(71, { Out, 30, 0 });                                         // Location
    uint32_t bindings[3] = { 4, 8, 9 };
    for (uint32_t i = 0; i < 3; i++) {
        op(71, { T0 + i, 34, 1 });                                  // DescriptorSet
        op(71, { T0 + i, 33, bindings[i] });                        // Binding
    }
    op(71, { S, 34, 1 });
    op(71, { S, 33, 2 });
    op(19, { Void });
    op(33, { Fn, Void });
    op(22, { Float, 32 });
    op(23, { V4, Float, 4 });
    op(23, { V2, Float, 2 });
    op(32, { POut, 3, V4 });
    op(59, { POut, Out, 3 });
    op(25, { Image, Float, 1, 0, 0, 0, 1, 0 });                     // OpTypeImage: 2D, sampled
    op(32, { PImage, 0, Image });
    op(26, { Sampler });
    op(32, { PSampler, 0, Sampler });
    op(27, { Sampled, Image });
    for (uint32_t i = 0; i < 3; i++) op(59, { PImage, T0 + i, 0 });
    op(59, { PSampler, S, 0 });
    op(43, { Float, Zero, 0 });
    op(44, { V2, Uv, Zero, Zero });
    op(54, { Void, Main, 0, Fn });
    op(248, { Label });
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t texture = Loaded + i * 4, sampler = texture + 1, both = texture + 2, colour = texture + 3;
        op(61, { Image, texture, T0 + i });
        op(61, { Sampler, sampler, S });
        op(86, { Sampled, both, texture, sampler });                // OpSampledImage
        op(87, { V4, colour, both, Uv });                           // OpImageSampleImplicitLod
    }
    op(129, { V4, Sum1, Loaded + 3, Loaded + 7 });
    op(129, { V4, Sum2, Sum1, Loaded + 11 });
    op(62, { Out, Sum2 });
    op(253, {});
    op(56, {});
    Bytes bytes(w.size() * 4);
    memcpy(bytes.data(), w.data(), bytes.size());
    return bytes;
}

TEST(what_the_lsf_gives_no_vulkan_place_is_where_the_directx_stage_has_it)
{
    std::string why;
    InterfaceTable table;
    MacInterface mac;
    std::vector<PlacedSlot> placed;
    auto place = [&](std::vector<MaterialSlot> const& material, PassRegisters const* registers) {
        auto shader = SpirvShader::Parse(SpirvThreeTexturesModule(), why);
        placed.clear();
        return shader && PlaceShader(*shader, {}, material, table, mac, placed, why, nullptr, registers);
    };
    auto at = [&](char const* name) {
        for (auto const& slot : placed) if (slot.Name == name) return (int)slot.Index;
        return -1;
    };
    auto says = [&](char const* part) { return why.find(part) != std::string::npos; };

    // The material names two of the three textures: the shader samples one of them through a second sampler,
    // and the toolkit made two textures of it and wrote one parameter.
    std::vector<MaterialSlot> material = { { 1, 4, "Texture2DParameter_A", -1, 0, 't' }, { 1, 8, "Texture2DParameter_B", -1, 1, 't' },
        { 1, 2, "_Sampler", -1, 0, 's' } };
    StageRegisters declares{ { { 0, "2d" }, { 1, "2d" }, { 2, "2d" }, { 30, "2d" } }, { 0, 14 } };
    PassRegisters registers{ &declares, {}, {} };
    CHECK(place(material, &registers) && at("Texture2DParameter_A") == 0 && at("Texture2DParameter_B") == 1 && at("bg3mse_texture_at_2") == 2
        && at("_Sampler") == 0);

    // Nothing to go by, or a DirectX stage that cannot be read: refused, and said.
    CHECK(!place(material, nullptr) && says("nothing is known to be at set 1, binding 9"));
    PassRegisters unread{ nullptr, {}, "it is cut short" };
    CHECK(!place(material, &unread) && says("cannot be read (it is cut short)"));
    // A DirectX stage with fewer textures than the stage has of the material's.
    StageRegisters fewer{ { { 0, "2d" }, { 1, "2d" } }, { 0 } };
    PassRegisters short_{ &fewer, {}, {} };
    CHECK(!place(material, &short_) && says("declares 2 in all"));
    // One that does not have the material's three at its first three registers: it did not keep what the Vulkan
    // stage kept, and the third would be put at a register of the engine's.
    StageRegisters dropped{ { { 0, "2d" }, { 1, "2d" }, { 30, "2d" } }, { 0 } };
    PassRegisters gap{ &dropped, {}, {} };
    CHECK(!place(material, &gap) && says("at its first registers (it has none at 2)"));
    // One whose registers do not go with what the .lsf does say, in order.
    std::vector<MaterialSlot> swapped = { { 1, 4, "Texture2DParameter_A", -1, 1, 't' }, { 1, 8, "Texture2DParameter_B", -1, 0, 't' },
        { 1, 2, "_Sampler", -1, 0, 's' } };
    CHECK(!place(swapped, &registers) && says("Texture2DParameter_A at another register"));
    // One that has another kind of texture there.
    StageRegisters cube{ { { 0, "2d" }, { 1, "2d" }, { 2, "cube" } }, { 0 } };
    PassRegisters another{ &cube, {}, {} };
    CHECK(!place(material, &another) && says("is cube at register 2"));

    // A pass the .lsf wrote without any Vulkan place: every thing of the material's by order, under the names the
    // .lsf has at those registers. A sampler of the engine's it names as well is not in the way.
    constexpr uint32_t kNone = UINT32_MAX;
    std::vector<MaterialSlot> unplaced = { { kNone, kNone, "Texture2DParameter_A", -1, 0, 't' }, { kNone, kNone, "Texture2DParameter_B", -1, 1, 't' },
        { kNone, kNone, "Texture2DParameter_C", -1, 2, 't' }, { kNone, kNone, "_Sampler", -1, 0, 's' }, { kNone, kNone, "_DefaultClampSampler", -1, 14, 's' } };
    PassRegisters whole{ &declares, unplaced, {} };
    CHECK(place({}, &whole) && at("Texture2DParameter_A") == 0 && at("Texture2DParameter_B") == 1 && at("Texture2DParameter_C") == 2
        && at("_Sampler") == 0 && at("_DefaultClampSampler") == -1);
}

TEST(mac_interface_is_what_every_function_agrees_on)
{
    MacPass first, second;
    first.Attributes = { { "Position", 0, "f3" }, { "Weights", 4, "f4" } };
    second.Attributes = { { "Position", 0, "u2" }, { "Weights", 5, "f4" } };
    first.Vertex = { MacBlock("PerCamera", 11, { { "g_View", 0, "f16" } }), MacBlock("CB_MaterialConstants", 20, { { "A", 0, "f4" } }) };
    second.Fragment = { MacBlock("PerCamera", 11, { { "g_View", 0, "f16" } }), MacBlock("CB_MaterialConstants", 20, { { "B", 0, "f1" } }) };
    MacResource texture;
    texture.Name = "Texture2DParameter_Mask";
    texture.Kind = ResourceKind::Texture;
    texture.Type = "2d";
    texture.Index = 2;
    first.Fragment.push_back(texture);
    texture.Index = 3;
    second.Fragment.push_back(texture);

    MacInterface mac;
    mac.Add(first);
    CHECK(mac.Resource("Texture2DParameter_Mask", ResourceKind::Texture) != nullptr);       // one function: nothing disagrees yet
    mac.Add(second);
    CHECK(mac.Passes() == 2);

    auto camera = mac.Resource("PerCamera", ResourceKind::Buffer);
    CHECK(camera && camera->Index == 11 && mac.HasFixedLayout(*camera));
    auto constants = mac.Resource("CB_MaterialConstants", ResourceKind::Buffer);
    CHECK(constants && constants->Index == 20 && !mac.HasFixedLayout(*constants));         // a place of its own, a layout per material
    CHECK(!mac.Resource("Texture2DParameter_Mask", ResourceKind::Texture));                 // a material's: where its .lsf says
    CHECK(!mac.Resource("PerCamera", ResourceKind::Texture) && !mac.Resource("Missing", ResourceKind::Buffer));

    CHECK(mac.Attribute("Position", "f3") && mac.Attribute("Position", "u4") && !mac.Attribute("Position", "i1"));
    CHECK(!mac.Attribute("Weights", "f4") && !mac.Attribute("Missing", "f4"));               // seen at two indices
}

// The vertex shader that goes with SpirvModule(): gl_Position = (x, -y, z, 1)
// of the input at location 0, as the toolkit's Vulkan shaders negate y, and
// the input at location 1 passed on at location 1.
Bytes SpirvVertexModule()
{
    std::vector<uint32_t> w = { 0x07230203, 0x00010300, 0x000e0000, 30, 0 };
    auto op = [&](uint16_t opcode, std::initializer_list<uint32_t> operands) {
        w.push_back((uint32_t)(operands.size() + 1) << 16 | opcode);
        w.insert(w.end(), operands);
    };
    enum { Void = 1, Fn, Float, V3, V4, PIn3, PIn4, POut4, Pos, Col, GlPos, OutCol, One, Main, Label, A, X, Y, Z, NegY, P, C };
    op(17, { 1 });
    op(14, { 0, 1 });
    op(15, { 0, Main, 0x6e69616d, 0, Pos, Col, GlPos, OutCol });       // OpEntryPoint Vertex "main"
    op(71, { Pos, 30, 0 });
    op(71, { Col, 30, 1 });
    op(71, { GlPos, 11, 0 });                                          // BuiltIn Position
    op(71, { OutCol, 30, 1 });
    op(19, { Void });
    op(33, { Fn, Void });
    op(22, { Float, 32 });
    op(23, { V3, Float, 3 });
    op(23, { V4, Float, 4 });
    op(32, { PIn3, 1, V3 });
    op(32, { PIn4, 1, V4 });
    op(32, { POut4, 3, V4 });
    op(59, { PIn3, Pos, 1 });
    op(59, { PIn4, Col, 1 });
    op(59, { POut4, GlPos, 3 });
    op(59, { POut4, OutCol, 3 });
    op(43, { Float, One, 0x3f800000 });
    op(54, { Void, Main, 0, Fn });
    op(248, { Label });
    op(61, { V3, A, Pos });
    op(81, { Float, X, A, 0 });
    op(81, { Float, Y, A, 1 });
    op(81, { Float, Z, A, 2 });
    op(127, { Float, NegY, Y });
    op(80, { V4, P, X, NegY, Z, One });
    op(62, { GlPos, P });
    op(61, { V4, C, Col });
    op(62, { OutCol, C });
    op(253, {});
    op(56, {});
    Bytes bytes(w.size() * 4);
    memcpy(bytes.data(), w.data(), bytes.size());
    return bytes;
}

// What the Mac game would ship for that pair: written by hand, with the
// block where the engine has it on the Mac and laid out as it is there.
constexpr char kGameSource[] = R"(
#include <metal_stdlib>
using namespace metal;
struct FakeBlock { float4 first; float4 second; };
struct VIn { float3 Position [[attribute(0)]]; float4 Tint [[attribute(3)]]; };
struct VOut { float4 position [[position]]; float4 tint; };
vertex VOut game_vertex(VIn in [[stage_in]]) { VOut o; o.position = float4(in.Position, 1.0); o.tint = in.Tint; return o; }
fragment float4 game_fragment(VOut in [[stage_in]], constant FakeBlock& Fake [[buffer(5)]]) { return (in.tint + Fake.second) * 0.5; }
)";

InterfaceTable FakeTable(bool inferred = false)
{
    InterfaceTable table;
    table.Slots.push_back({ 1, 2, ResourceKind::Buffer, "block", "Fake", inferred });
    table.Blocks.push_back({ "Fake", { "second" }, { "f4" }, { 0 } });
    table.Attributes = { { "POSITION", 0, "Position", false }, { "COLOR", 2, "Tint", false } };
    return table;
}

TEST(a_windows_shader_is_placed_translated_and_draws_what_the_mac_function_draws)
{
    std::string why;
    std::string gameSources[] = { kGameSource };
    auto game = MetalFunctions::Compile(gameSources, why);
    if (!game) {
        printf("    (no Metal here: %s)\n", why.c_str());
        return;
    }
    MacPass gamePass;
    CHECK(game->Reflect("game_vertex", "game_fragment", gamePass, why));
    CHECK(gamePass.Attributes.size() == 2 && gamePass.Fragment.size() == 1 && gamePass.Fragment[0].Name == "Fake");
    CHECK(gamePass.Fragment[0].Index == 5 && gamePass.Fragment[0].Members.size() == 2 && gamePass.Fragment[0].Members[1].Offset == 16);
    MacInterface mac;
    mac.Add(gamePass);

    // What the Windows shader takes, as it says it.
    auto vertex = SpirvShader::Parse(SpirvVertexModule(), why);
    auto fragment = SpirvShader::Parse(SpirvModule(), why);
    CHECK(vertex && fragment);
    if (!vertex || !fragment) return;
    CHECK(vertex->IsVertex() && !fragment->IsVertex());
    CHECK(vertex->Inputs().size() == 2 && vertex->Resources().empty());
    CHECK(fragment->Resources().size() == 1);
    auto const& block = fragment->Resources()[0];
    CHECK(block.Set == 1 && block.Binding == 2 && block.Kind == ResourceKind::Buffer && block.Type == "block");
    CHECK(block.Members.size() == 1 && block.Members[0].Type == "f4" && block.ReadKnown && block.Read == std::vector<uint32_t>{ 0 });

    // The vertex inputs are POSITION0 and COLOR2 in the DirectX signature.
    std::vector<InputSemantic> inputs = { { "POSITION", 0 }, { "COLOR", 2 } };
    auto table = FakeTable();
    std::vector<PlacedSlot> placed;
    CHECK(PlaceShader(*vertex, inputs, {}, table, mac, placed, why));
    CHECK(PlaceShader(*fragment, inputs, {}, table, mac, placed, why));
    std::string sources[2];
    CHECK(vertex->Translate("ours_vertex", sources[0], why));
    CHECK(fragment->Translate("ours_fragment", sources[1], why));

    auto ours = MetalFunctions::Compile(sources, why);
    CHECK(ours != nullptr);
    if (!ours) { printf("    %s\n", why.c_str()); return; }
    MacPass ourPass;
    CHECK(ours->Reflect("ours_vertex", "ours_fragment", ourPass, why));
    // Everything is where the game's functions have it: the attributes, the block, the member that was moved.
    CHECK(ourPass.Attributes.size() == 2);
    for (auto const& a : ourPass.Attributes) CHECK((a.Name == "Position" && a.Index == 0) || (a.Name == "Tint" && a.Index == 3));
    CHECK(ourPass.Fragment.size() == 1 && ourPass.Fragment[0].Name == "Fake" && ourPass.Fragment[0].Index == 5);
    auto const& members = ourPass.Fragment[0].Members;
    CHECK(std::any_of(members.begin(), members.end(), [](auto const& m) { return m.Name == "second" && m.Offset == 16 && m.Type == "f4"; }));

    // And the same draw gives the same picture: y is not flipped, the member is read where it is.
    DrawSetup setup;
    setup.Attributes = { { 0, 'f' }, { 3, 'f' } };
    Bytes blockBytes(32);
    for (int i = 0; i < 8; i++) {
        float value = 0.125f * (float)(i + 1);
        memcpy(blockBytes.data() + i * 4, &value, 4);
    }
    setup.Buffers.push_back({ 5, blockBytes, false });
    DrawResult theirs, mine;
    CHECK(game->Draw("game_vertex", "game_fragment", setup, theirs, why));
    CHECK(ours->Draw("ours_vertex", "ours_fragment", setup, mine, why));
    uint32_t drawn = 0, different = 0;
    for (size_t i = 0; i < theirs.Color[0].size() && i < mine.Color[0].size(); i++) {
        drawn += theirs.Color[0][i] != MetalFunctions::kClearColor;
        different += std::fabs(theirs.Color[0][i] - mine.Color[0][i]) > 1e-3f;
    }
    CHECK(drawn > 1000 && different == 0);
    CHECK(theirs.Depth == mine.Depth);
}

// A fragment shader that reads what the Windows engine keeps in a block of lighting constants and the Mac engine
// does not: out = (fade, bias, widths.y + widths.z, count), the block { float fade @0; float bias @4;
// vec4 far @16; vec4 widths @32; float count @48 } at set 1, binding 2. widths.y is read through a pointer to
// that one number, widths.z out of the whole vector.
Bytes SpirvLightingModule()
{
    std::vector<uint32_t> w = { 0x07230203, 0x00010300, 0x000e0000, 40, 0 };
    auto op = [&](uint16_t opcode, std::initializer_list<uint32_t> operands) {
        w.push_back((uint32_t)(operands.size() + 1) << 16 | opcode);
        w.insert(w.end(), operands);
    };
    enum { Void = 1, Fn, Float, V4, Int, Struct, PUbo, Ubo, PFloat, PV4, POut, Out, I0, I1, I3, I4, Main, Label, PFade, Fade, PBias, Bias,
           PW1, W1, PWAll, WAll, W2, WSum, PCount, Count, Result };
    op(17, { 1 });
    op(14, { 0, 1 });
    op(15, { 4, Main, 0x6e69616d, 0, Out });                           // OpEntryPoint Fragment "main"
    op(16, { Main, 7 });
    op(71, { Out, 30, 0 });
    op(71, { Ubo, 34, 1 });
    op(71, { Ubo, 33, 2 });
    op(71, { Struct, 2 });
    op(72, { Struct, 0, 35, 0 });
    op(72, { Struct, 1, 35, 4 });
    op(72, { Struct, 2, 35, 16 });
    op(72, { Struct, 3, 35, 32 });
    op(72, { Struct, 4, 35, 48 });
    op(19, { Void });
    op(33, { Fn, Void });
    op(22, { Float, 32 });
    op(23, { V4, Float, 4 });
    op(21, { Int, 32, 1 });
    op(30, { Struct, Float, Float, V4, V4, Float });
    op(32, { PUbo, 2, Struct });
    op(32, { PFloat, 2, Float });
    op(32, { PV4, 2, V4 });
    op(32, { POut, 3, V4 });
    op(59, { PUbo, Ubo, 2 });
    op(59, { POut, Out, 3 });
    op(43, { Int, I0, 0 });
    op(43, { Int, I1, 1 });
    op(43, { Int, I3, 3 });
    op(43, { Int, I4, 4 });
    op(54, { Void, Main, 0, Fn });
    op(248, { Label });
    op(65, { PFloat, PFade, Ubo, I0 });
    op(61, { Float, Fade, PFade });
    op(65, { PFloat, PBias, Ubo, I1 });
    op(61, { Float, Bias, PBias });
    op(65, { PFloat, PW1, Ubo, I3, I1 });
    op(61, { Float, W1, PW1 });
    op(65, { PV4, PWAll, Ubo, I3 });
    op(61, { V4, WAll, PWAll });
    op(81, { Float, W2, WAll, 2 });
    op(129, { Float, WSum, W1, W2 });
    op(65, { PFloat, PCount, Ubo, I4 });
    op(61, { Float, Count, PCount });
    op(80, { V4, Result, Fade, Bias, WSum, Count });
    op(62, { Out, Result });
    op(253, {});
    op(56, {});
    Bytes bytes(w.size() * 4);
    memcpy(bytes.data(), w.data(), bytes.size());
    return bytes;
}

// What the Mac game has for the same lighting: one over the fade and over the widths, a bias for each cascade that
// is the Windows bias over the cascade's far plane, the count as a whole number; and a function that works the
// Windows numbers out of them the long way, for the translated shader to be drawn beside.
constexpr char kLightingGameSource[] = R"(
#include <metal_stdlib>
using namespace metal;
struct LightingBlock { float g_InvFade; float g_Unrelated; float4 g_Far; float4 g_InvWidths; float4 g_CascadeBias; uint g_Count; };
struct VIn { float3 Position [[attribute(0)]]; float4 Tint [[attribute(3)]]; };
struct VOut { float4 position [[position]]; float4 tint; };
vertex VOut game_vertex(VIn in [[stage_in]]) { VOut o; o.position = float4(in.Position, 1.0); o.tint = in.Tint; return o; }
fragment float4 game_fragment(VOut in [[stage_in]], constant LightingBlock& Lighting [[buffer(5)]])
{
    float4 widths = float4(1.0) / Lighting.g_InvWidths;
    return float4(1.0 / Lighting.g_InvFade, Lighting.g_CascadeBias.x * Lighting.g_Far.x, widths.y + widths.z, float(Lighting.g_Count));
}
)";

InterfaceTable LightingTable()
{
    InterfaceTable table;
    table.Slots.push_back({ 1, 2, ResourceKind::Buffer, "block", "Lighting", false });
    EngineBlock block{ "Lighting", { "", "", "g_Far", "", "" }, { "f1", "f1", "f4", "f4", "f1" }, { 0, 4, 16, 32, 48 } };
    block.Absent = { { 0, "fade" }, { 1, "bias" }, { 3, "widths" }, { 4, "count" } };
    block.OtherMeaning = { { 0, "its reciprocal" } };
    block.Derived = {
        { 0, { "g_InvFade", "f1", 0, "(1.0 / $)" } },
        { 1, { "g_CascadeBias", "f4", 0, "($ * {g_Far}.x)" } },
        { 3, { "g_InvWidths", "f4", 0, "(1.0 / $)" } },
        { 4, { "g_Count", "u1", 0, "float(as_type<uint>($))" } },
    };
    table.Blocks.push_back(block);
    table.Attributes = { { "POSITION", 0, "Position", false }, { "COLOR", 2, "Tint", false } };
    return table;
}

TEST(a_member_the_mac_block_lacks_is_worked_out_from_what_it_has)
{
    std::string why;
    std::string gameSources[] = { kLightingGameSource };
    auto game = MetalFunctions::Compile(gameSources, why);
    if (!game) {
        printf("    (no Metal here: %s)\n", why.c_str());
        return;
    }
    MacPass gamePass;
    CHECK(game->Reflect("game_vertex", "game_fragment", gamePass, why));
    CHECK(gamePass.Fragment.size() == 1 && gamePass.Fragment[0].Name == "Lighting" && gamePass.Fragment[0].Members.size() == 6);
    MacInterface mac;
    mac.Add(gamePass);

    std::vector<InputSemantic> inputs = { { "POSITION", 0 }, { "COLOR", 2 } };
    auto placedWith = [&](InterfaceTable const& table, MacInterface const& interface, std::string& source) {
        auto fragment = SpirvShader::Parse(SpirvLightingModule(), why);
        if (!fragment) return false;
        std::vector<PlacedSlot> placed;
        return PlaceShader(*fragment, inputs, {}, table, interface, placed, why) && fragment->Translate("ours_fragment", source, why);
    };

    // Without the members worked out (a game where what they rest on does not hold), the shader is refused as before.
    std::string none;
    CHECK(!placedWith(LightingTable().WithoutDerived(), mac, none) && why.find("reads fade of Lighting, and the Mac game's member of that name holds its reciprocal") != std::string::npos);
    // A Mac block that has not got what one is worked out from, or has it as another type: that one is refused.
    {
        MacPass other = gamePass;
        for (auto& m : other.Fragment[0].Members) if (m.Name == "g_CascadeBias") m.Name = "g_SomethingElse";
        MacInterface without;
        without.Add(other);
        CHECK(!placedWith(LightingTable(), without, none) && why.find("reads bias of Lighting, which has no counterpart") != std::string::npos);
        other = gamePass;
        for (auto& m : other.Fragment[0].Members) if (m.Name == "g_Count") m.Type = "f1";
        MacInterface asFloat;
        asFloat.Add(other);
        CHECK(!placedWith(LightingTable(), asFloat, none) && why.find("reads count of Lighting, which has no counterpart") != std::string::npos);
    }
    // What one is worked out with has to be in the shader's block.
    {
        auto table = LightingTable();
        table.Blocks[0].Derived[1].Read = "($ * {g_Near}.x)";
        CHECK(!placedWith(table, mac, none) && why.find("reads bias of Lighting, which is worked out with g_Near") != std::string::npos);
    }
    // Two members over the same numbers: no room.
    {
        auto table = LightingTable();
        table.Blocks[0].Derived[1].From = "g_Far";
        CHECK(!placedWith(table, mac, none) && why.find("no room for a member that is worked out") != std::string::npos);
    }

    auto vertex = SpirvShader::Parse(SpirvVertexModule(), why);
    CHECK(vertex != nullptr);
    if (!vertex) return;
    std::vector<PlacedSlot> placed;
    std::string sources[2];
    CHECK(PlaceShader(*vertex, inputs, {}, LightingTable(), mac, placed, why) && vertex->Translate("ours_vertex", sources[0], why));
    CHECK(placedWith(LightingTable(), mac, sources[1]));
    // Each read is the expression, and nothing reads a member bare.
    CHECK(sources[1].find("(1.0 / Lighting.bg3mse_from_g_InvFade)") != std::string::npos);
    CHECK(sources[1].find("(Lighting.bg3mse_from_g_CascadeBias * Lighting.g_Far.x)") != std::string::npos);
    CHECK(sources[1].find("float(as_type<uint>(Lighting.bg3mse_from_g_Count))") != std::string::npos);
    auto ours = MetalFunctions::Compile(sources, why);
    CHECK(ours != nullptr);
    if (!ours) { printf("    %s\n%s\n", why.c_str(), sources[1].c_str()); return; }
    MacPass ourPass;
    CHECK(ours->Reflect("ours_vertex", "ours_fragment", ourPass, why));
    CHECK(ourPass.Fragment.size() == 1 && ourPass.Fragment[0].Name == "Lighting" && ourPass.Fragment[0].Index == 5);
    auto at = [&](char const* name) {
        for (auto const& m : ourPass.Fragment[0].Members) if (m.Name == name) return (int)m.Offset;
        return -1;
    };
    // Each where the thing it is worked out from is: the bias after the far planes, though it comes before them in the shader.
    CHECK(at("bg3mse_from_g_InvFade") == 0 && at("g_Far") == 16 && at("bg3mse_from_g_InvWidths") == 32 && at("bg3mse_from_g_CascadeBias") == 48
        && at("bg3mse_from_g_Count") == 64);

    // The same bytes to both, as the Mac engine would fill them; one to four cascades.
    for (uint32_t count = 1; count <= 4; count++) {
        float fade = 2.5f + (float)count, bias = 0.75f, far[4] = { 12.0f, 30.0f, 70.0f, 150.0f }, widths[4] = { 10.0f, 24.0f, 60.0f, 128.0f };
        Bytes blockBytes(80, 0);
        auto put = [&](size_t where, float value) { memcpy(blockBytes.data() + where, &value, 4); };
        put(0, 1.0f / fade);
        put(4, 123.0f);
        for (int i = 0; i < 4; i++) {
            put(16 + 4 * i, far[i]);
            put(32 + 4 * i, 1.0f / widths[i]);
            put(48 + 4 * i, bias / far[i]);
        }
        memcpy(blockBytes.data() + 64, &count, 4);
        DrawSetup setup;
        setup.Attributes = { { 0, 'f' }, { 3, 'f' } };
        setup.Buffers.push_back({ 5, blockBytes, false });
        DrawResult theirs, mine;
        CHECK(game->Draw("game_vertex", "game_fragment", setup, theirs, why));
        CHECK(ours->Draw("ours_vertex", "ours_fragment", setup, mine, why));
        uint32_t drawn = 0, different = 0, wrong = 0;
        const float expected[4] = { fade, bias, widths[1] + widths[2], (float)count };
        for (size_t i = 0; i + 3 < theirs.Color[0].size() && i + 3 < mine.Color[0].size(); i += 4) {
            if (theirs.Color[0][i] == MetalFunctions::kClearColor && mine.Color[0][i] == MetalFunctions::kClearColor) continue;
            drawn++;
            for (int k = 0; k < 4; k++) {
                different += std::fabs(theirs.Color[0][i + k] - mine.Color[0][i + k]) > 1e-4f * std::fabs(expected[k]);
                wrong += std::fabs(mine.Color[0][i + k] - expected[k]) > 1e-4f * std::fabs(expected[k]);
            }
        }
        CHECK(drawn > 1000 && different == 0 && wrong == 0);
    }
}

// A fragment shader that reads an array of structures { vec4 a @0; float b @16; ivec2 fill @24 } of 32 bytes at
// set 1, binding 2: out = lights[1].a * lights[1].b. `whole`: the element is loaded whole and a and b are taken
// out of it. `fill`: what fills the element up is read as well. `kept`: the loaded element is stored somewhere.
struct ElementsVariant
{
    bool Whole{ false };
    bool Fill{ false };
    bool Kept{ false };
};

Bytes SpirvElementsModule(ElementsVariant v = {})
{
    std::vector<uint32_t> w = { 0x07230203, 0x00010300, 0x000e0000, 60, 0 };
    auto op = [&](uint16_t opcode, std::initializer_list<uint32_t> operands) {
        w.push_back((uint32_t)(operands.size() + 1) << 16 | opcode);
        w.insert(w.end(), operands);
    };
    enum { Void = 1, Fn, Float, V4, Int, IV2, Element, Array, Block, PBlock, Lights, PV4, PFloat, PIV2, PElement, POut, Out, I0, I1, I2, Main, Label,
           PA, A, PB, B, PE, E, Scaled, PFill, Fill, FillX, FillF, Sum, PLocal, Local };
    op(17, { 1 });
    op(14, { 0, 1 });
    op(15, { 4, Main, 0x6e69616d, 0, Out });
    op(16, { Main, 7 });
    op(71, { Out, 30, 0 });
    op(71, { Lights, 34, 1 });
    op(71, { Lights, 33, 2 });
    op(71, { Block, 2 });
    op(72, { Block, 0, 35, 0 });
    op(71, { Array, 6, 32 });                                           // ArrayStride
    op(72, { Element, 0, 35, 0 });
    op(72, { Element, 1, 35, 16 });
    op(72, { Element, 2, 35, 24 });
    op(19, { Void });
    op(33, { Fn, Void });
    op(22, { Float, 32 });
    op(23, { V4, Float, 4 });
    op(21, { Int, 32, 1 });
    op(23, { IV2, Int, 2 });
    op(30, { Element, V4, Float, IV2 });
    op(29, { Array, Element });                                         // OpTypeRuntimeArray
    op(30, { Block, Array });
    op(32, { PBlock, 12, Block });                                      // StorageBuffer
    op(32, { PV4, 12, V4 });
    op(32, { PFloat, 12, Float });
    op(32, { PIV2, 12, IV2 });
    op(32, { PElement, 12, Element });
    op(32, { POut, 3, V4 });
    op(32, { PLocal, 7, Element });                                     // Function
    op(59, { PBlock, Lights, 12 });
    op(59, { POut, Out, 3 });
    op(43, { Int, I0, 0 });
    op(43, { Int, I1, 1 });
    op(43, { Int, I2, 2 });
    op(54, { Void, Main, 0, Fn });
    op(248, { Label });
    if (v.Kept) op(59, { PLocal, Local, 7 });
    uint32_t a = A, b = B;
    if (v.Whole) {
        op(65, { PElement, PE, Lights, I0, I1 });
        op(61, { Element, E, PE });
        op(81, { V4, A, E, 0 });
        op(81, { Float, B, E, 1 });
        if (v.Kept) op(62, { Local, E });
        if (v.Fill) op(81, { IV2, Fill, E, 2 });
    } else {
        op(65, { PV4, PA, Lights, I0, I1, I0 });
        op(61, { V4, A, PA });
        op(65, { PFloat, PB, Lights, I0, I1, I1 });
        op(61, { Float, B, PB });
        if (v.Fill) {
            op(65, { PIV2, PFill, Lights, I0, I1, I2 });
            op(61, { IV2, Fill, PFill });
        }
    }
    if (v.Fill) {
        op(81, { Int, FillX, Fill, 0 });
        op(111, { Float, FillF, FillX });                               // OpConvertSToF
        op(129, { Float, Sum, B, FillF });
        b = Sum;
    }
    op(142, { V4, Scaled, a, b });
    op(62, { Out, Scaled });
    op(253, {});
    op(56, {});
    Bytes bytes(w.size() * 4);
    memcpy(bytes.data(), w.data(), bytes.size());
    return bytes;
}

// An array's element on the Mac may leave unlisted what fills it up to its size, where the Windows shader has a
// member for it: the same element, as long as the shader is known not to read that member.
TEST(an_array_element_may_end_in_members_the_shader_does_not_read)
{
    std::string why;
    auto read = [&](ElementsVariant v, std::vector<uint32_t>& members) {
        auto shader = SpirvShader::Parse(SpirvElementsModule(v), why);
        if (!shader || shader->Resources().size() != 1) return false;
        auto const& lights = shader->Resources()[0];
        if (lights.Type != "array" || lights.Size != 32 || lights.Members.size() != 3) return false;
        members = lights.Read;
        return lights.ReadKnown;
    };
    std::vector<uint32_t> members;
    CHECK(read({}, members) && members == std::vector<uint32_t>({ 0, 1 }));
    CHECK(read({ .Whole = true }, members) && members == std::vector<uint32_t>({ 0, 1 }));
    CHECK(read({ .Fill = true }, members) && members == std::vector<uint32_t>({ 0, 1, 2 }));
    CHECK(read({ .Whole = true, .Fill = true }, members) && members == std::vector<uint32_t>({ 0, 1, 2 }));
    CHECK(!read({ .Whole = true, .Kept = true }, members));     // the element went somewhere whole: not known

    InterfaceTable table;
    table.Slots.push_back({ 1, 2, ResourceKind::Buffer, "array", "Lights", false });
    auto placed = [&](ElementsVariant v, uint32_t macSize, std::vector<BlockMember> macMembers) {
        MacPass pass;
        pass.Fragment = { MacBlock("Lights", 3, std::move(macMembers)) };
        pass.Fragment[0].Size = macSize;
        MacInterface mac;
        mac.Add(pass);
        auto shader = SpirvShader::Parse(SpirvElementsModule(v), why);
        std::vector<PlacedSlot> slots;
        return shader && PlaceShader(*shader, {}, {}, table, mac, slots, why);
    };
    std::vector<BlockMember> two = { { "a", 0, "f4" }, { "b", 16, "f1" } }, three = { { "a", 0, "f4" }, { "b", 16, "f1" }, { "fill", 24, "i2" } };
    CHECK(placed({}, 32, three) && placed({ .Fill = true }, 32, three));
    CHECK(placed({}, 32, two) && placed({ .Whole = true }, 32, two));
    // Read, or not known to be left alone, or an element of another size: not the same element.
    CHECK(!placed({ .Fill = true }, 32, two) && why.find("laid out differently") != std::string::npos);
    CHECK(!placed({ .Whole = true, .Fill = true }, 32, two) && !placed({ .Whole = true, .Kept = true }, 32, two));
    CHECK(!placed({}, 48, two) && !placed({}, 48, three));
    CHECK(!placed({}, 32, { { "a", 0, "f4" }, { "b", 20, "f1" } }));
}

// A fragment shader that takes a light probe's cubes and a light's cookie out of a table of textures the way the
// Windows shaders do: probes at (1, 2) and lights at (1, 3), arrays of { vec4; int; int } and { vec4; int }; the
// table at (1, 4), declared as cubes and as flat textures; a sampler at (1, 5).
//   out = cubes[2(k-1)] + cubes[2(k-1)+1], k = (probe.diffuse >> 3) & 16383, at level 0
//       + cubes[2(k-1)], k = (probe.specular >> 16) & 65535, at level 3
//       + flat[(light.cookie >> 1) - 1], with gradients
struct TableVariant
{
    bool OtherShift{ false };       // the diffuse number is taken from another bit
    bool Stray{ false };            // a place in the table is also used for something else
};

Bytes SpirvTableModule(TableVariant v = {})
{
    std::vector<uint32_t> w = { 0x07230203, 0x00010300, 0x000e0000, 200, 0 };
    auto op = [&](uint16_t opcode, std::initializer_list<uint32_t> operands) {
        w.push_back((uint32_t)(operands.size() + 1) << 16 | opcode);
        w.insert(w.end(), operands);
    };
    enum { Void = 1, Fn, Float, V2, V3, V4, Int, ProbeT, LightT, ProbeArr, LightArr, ProbeBlock, LightBlock, PProbeBlock, PLightBlock, Probes, Lights,
           PInt, ImgCube, ImgFlat, RtCube, RtFlat, PRtCube, PRtFlat, TableCube, TableFlat, PImgCube, PImgFlat, SICube, SIFlat, SamplerT, PSampler,
           Smp, POut, Out, I0, I1, I2, I3, I4, I16, C16383, C65535, F0, F3, FA, FB, FC, FD, Dir, Uv, Dx, Dy, Main, Label, SmpL,
           PM5, M5, S3, K, D, X, Y, PC, IC, SC, RC, PA, IA, SA, RA, PM6, M6, S16, K2, D2, X2, PS, IS, SS, RS, PM1, M1, KC, DC, PK, IK, SK, RK,
           Sum1, Sum2, Sum3, Extra };
    op(17, { 1 });
    op(14, { 0, 1 });
    op(15, { 4, Main, 0x6e69616d, 0, Out });
    op(16, { Main, 7 });
    op(71, { Out, 30, 0 });
    for (auto [id, binding] : { std::pair<uint32_t, uint32_t>{ Probes, 2 }, { Lights, 3 }, { TableCube, 4 }, { TableFlat, 4 }, { Smp, 5 } }) {
        op(71, { id, 34, 1 });
        op(71, { id, 33, binding });
    }
    op(71, { ProbeBlock, 2 });
    op(71, { LightBlock, 2 });
    op(72, { ProbeBlock, 0, 35, 0 });
    op(72, { LightBlock, 0, 35, 0 });
    op(71, { ProbeArr, 6, 32 });
    op(71, { LightArr, 6, 32 });
    op(72, { ProbeT, 0, 35, 0 });
    op(72, { ProbeT, 1, 35, 16 });
    op(72, { ProbeT, 2, 35, 20 });
    op(72, { LightT, 0, 35, 0 });
    op(72, { LightT, 1, 35, 16 });
    op(19, { Void });
    op(33, { Fn, Void });
    op(22, { Float, 32 });
    op(23, { V2, Float, 2 });
    op(23, { V3, Float, 3 });
    op(23, { V4, Float, 4 });
    op(21, { Int, 32, 1 });
    op(30, { ProbeT, V4, Int, Int });
    op(30, { LightT, V4, Int });
    op(29, { ProbeArr, ProbeT });
    op(29, { LightArr, LightT });
    op(30, { ProbeBlock, ProbeArr });
    op(30, { LightBlock, LightArr });
    op(32, { PProbeBlock, 12, ProbeBlock });
    op(32, { PLightBlock, 12, LightBlock });
    op(32, { PInt, 12, Int });
    op(25, { ImgCube, Float, 3, 2, 0, 0, 1, 0 });                      // OpTypeImage: a cube
    op(25, { ImgFlat, Float, 1, 2, 0, 0, 1, 0 });                      // a flat texture
    op(29, { RtCube, ImgCube });
    op(29, { RtFlat, ImgFlat });
    op(32, { PRtCube, 0, RtCube });
    op(32, { PRtFlat, 0, RtFlat });
    op(32, { PImgCube, 0, ImgCube });
    op(32, { PImgFlat, 0, ImgFlat });
    op(27, { SICube, ImgCube });
    op(27, { SIFlat, ImgFlat });
    op(26, { SamplerT });
    op(32, { PSampler, 0, SamplerT });
    op(32, { POut, 3, V4 });
    op(59, { PProbeBlock, Probes, 12 });
    op(59, { PLightBlock, Lights, 12 });
    op(59, { PRtCube, TableCube, 0 });
    op(59, { PRtFlat, TableFlat, 0 });
    op(59, { PSampler, Smp, 0 });
    op(59, { POut, Out, 3 });
    for (auto [id, value] : { std::pair<uint32_t, uint32_t>{ I0, 0 }, { I1, 1 }, { I2, 2 }, { I3, 3 }, { I4, 4 }, { I16, 16 }, { C16383, 16383 },
            { C65535, 65535 } }) {
        op(43, { Int, id, value });
    }
    for (auto [id, value] : { std::pair<uint32_t, float>{ F0, 0.0f }, { F3, 3.0f }, { FA, 0.3f }, { FB, 0.5f }, { FC, 0.8f }, { FD, 0.01f } }) {
        uint32_t bits;
        memcpy(&bits, &value, 4);
        op(43, { Float, id, bits });
    }
    op(44, { V3, Dir, FA, FB, FC });                                    // OpConstantComposite
    op(44, { V2, Uv, FA, FB });
    op(44, { V2, Dx, FD, F0 });
    op(44, { V2, Dy, F0, FD });
    op(54, { Void, Main, 0, Fn });
    op(248, { Label });
    op(61, { SamplerT, SmpL, Smp });
    op(65, { PInt, PM5, Probes, I0, I0, I1 });
    op(61, { Int, M5, PM5 });
    op(194, { Int, S3, M5, v.OtherShift ? (uint32_t)I4 : (uint32_t)I3 });
    op(199, { Int, K, S3, C16383 });
    op(130, { Int, D, K, I1 });
    op(132, { Int, X, D, I2 });
    op(128, { Int, Y, X, I1 });
    op(65, { PImgCube, PC, TableCube, X });
    op(61, { ImgCube, IC, PC });
    op(86, { SICube, SC, IC, SmpL });
    op(88, { V4, RC, SC, Dir, 2, F0 });
    op(65, { PImgCube, PA, TableCube, Y });
    op(61, { ImgCube, IA, PA });
    op(86, { SICube, SA, IA, SmpL });
    op(88, { V4, RA, SA, Dir, 2, F0 });
    op(65, { PInt, PM6, Probes, I0, I0, I2 });
    op(61, { Int, M6, PM6 });
    op(194, { Int, S16, M6, I16 });
    op(199, { Int, K2, S16, C65535 });
    op(130, { Int, D2, K2, I1 });
    op(132, { Int, X2, D2, I2 });
    op(65, { PImgCube, PS, TableCube, X2 });
    op(61, { ImgCube, IS, PS });
    op(86, { SICube, SS, IS, SmpL });
    op(88, { V4, RS, SS, Dir, 2, F3 });
    op(65, { PInt, PM1, Lights, I0, I0, I1 });
    op(61, { Int, M1, PM1 });
    op(194, { Int, KC, M1, I1 });
    op(130, { Int, DC, KC, I1 });
    op(65, { PImgFlat, PK, TableFlat, DC });
    op(61, { ImgFlat, IK, PK });
    op(86, { SIFlat, SK, IK, SmpL });
    op(88, { V4, RK, SK, Uv, 4, Dx, Dy });
    if (v.Stray) op(111, { Float, Extra, X });                           // OpConvertSToF of a place
    op(129, { V4, Sum1, RC, RA });
    op(129, { V4, Sum2, Sum1, RS });
    op(129, { V4, Sum3, Sum2, RK });
    op(62, { Out, Sum3 });
    op(253, {});
    op(56, {});
    Bytes bytes(w.size() * 4);
    memcpy(bytes.data(), w.data(), bytes.size());
    return bytes;
}

// The same thing as the Mac game has it: the cubes are layers of cube arrays (one for the diffuse cubes, four for
// the specular ones, sampled at the level less the array's number), the cookies layers of six flat arrays, and the
// same members hold an array's number and a layer.
constexpr char kTableGameSource[] = R"(
#include <metal_stdlib>
using namespace metal;
struct ProbeElement { float4 a; int diffuse; int specular; };
struct LightElement { float4 c; int cookie; };
struct VIn { float3 Position [[attribute(0)]]; float4 Tint [[attribute(3)]]; };
struct VOut { float4 position [[position]]; float4 tint; };
vertex VOut game_vertex(VIn in [[stage_in]]) { VOut o; o.position = float4(in.Position, 1.0); o.tint = in.Tint; return o; }
fragment float4 game_fragment(VOut in [[stage_in]], const device ProbeElement* Probes [[buffer(5)]], const device LightElement* Lights [[buffer(6)]],
    texturecube_array<float> DiffRGB [[texture(1)]], texturecube_array<float> DiffAlpha [[texture(2)]],
    array<texturecube_array<float>, 4> SpecRGB [[texture(3)]], array<texturecube_array<float>, 4> SpecAlpha [[texture(7)]],
    array<texture2d_array<float>, 6> Cookie [[texture(11)]], sampler Smp [[sampler(2)]])
{
    float3 direction = float3(0.3, 0.5, 0.8);
    int diffuse = Probes[0].diffuse, specular = Probes[0].specular, cookie = Lights[0].cookie;
    int layer = ((diffuse >> 3) & 511) - 1;
    float4 sum = DiffRGB.sample(Smp, direction, uint(layer), level(0.0)) + DiffAlpha.sample(Smp, direction, uint(layer), level(0.0));
    int array = (specular >> 11) & 3;
    sum += SpecRGB[array].sample(Smp, direction, uint((specular >> 13) - 1), level(3.0 - float(array)));
    sum += Cookie[(cookie >> 1) & 7].sample(Smp, float2(0.3, 0.5), uint((cookie >> 4) - 1), gradient2d(float2(0.01, 0.0), float2(0.0, 0.01)));
    return sum + 0.0 * SpecAlpha[0].sample(Smp, direction, 0.0);
}
)";

InterfaceTable TableOfTexturesTable()
{
    InterfaceTable table;
    table.Slots = { { 1, 2, ResourceKind::Buffer, "array", "Probes", false }, { 1, 3, ResourceKind::Buffer, "array", "Lights", false },
        { 1, 5, ResourceKind::Sampler, "", "Smp", false },
        { 15, 0, ResourceKind::Texture, "cubearray", "DiffRGB", false }, { 15, 1, ResourceKind::Texture, "cubearray", "DiffAlpha", false },
        { 15, 2, ResourceKind::Texture, "cubearray[]", "SpecRGB", false }, { 15, 3, ResourceKind::Texture, "cubearray[]", "SpecAlpha", false },
        { 15, 4, ResourceKind::Texture, "2darray[]", "Cookie", false } };
    table.Textures = TextureTableRewrite{ 1, 4, 1, 2, 1, 2, 1, 3, 1, 4, 6, 15, { 0, 1, 2, 3, 4 } };
    table.Attributes = { { "POSITION", 0, "Position", false }, { "COLOR", 2, "Tint", false } };
    return table;
}

TEST(the_table_of_textures_is_taken_from_the_mac_games_arrays)
{
    std::string why;
    // The rewrite alone: done for the forms it knows, refused for anything else, and a module without the table is left alone.
    auto rewritten = [&](Bytes const& module) {
        std::vector<uint32_t> words(module.size() / 4);
        memcpy(words.data(), module.data(), module.size());
        auto before = words;
        auto result = RewriteTextureTable(words, *TableOfTexturesTable().Textures, why);
        if (result != TextureTableResult::Done) CHECK(words == before);
        return result;
    };
    CHECK(rewritten(SpirvTableModule()) == TextureTableResult::Done);
    CHECK(rewritten(SpirvModule()) == TextureTableResult::NotThere);
    CHECK(rewritten(SpirvTableModule({ .OtherShift = true })) == TextureTableResult::Refused && why.find("a probe's member is used in a way that is not known") != std::string::npos);
    CHECK(rewritten(SpirvTableModule({ .Stray = true })) == TextureTableResult::Refused && why.find("a probe's place is used for something else") != std::string::npos);

    std::string gameSources[] = { kTableGameSource };
    auto game = MetalFunctions::Compile(gameSources, why);
    if (!game) {
        printf("    (no Metal here: %s)\n", why.c_str());
        return;
    }
    MacPass gamePass;
    CHECK(game->Reflect("game_vertex", "game_fragment", gamePass, why));
    MacInterface mac;
    mac.Add(gamePass);
    auto specular = mac.Resource("SpecRGB", ResourceKind::Texture), cookie = mac.Resource("Cookie", ResourceKind::Texture);
    CHECK(specular && specular->Type == "cubearray[]" && specular->Size == 4 && cookie && cookie->Type == "2darray[]" && cookie->Size == 6);

    std::vector<InputSemantic> inputs = { { "POSITION", 0 }, { "COLOR", 2 } };
    auto table = TableOfTexturesTable();
    auto translated = [&](InterfaceTable const& with, std::string& source) {
        auto fragment = SpirvShader::Parse(SpirvTableModule(), why, {}, with.Textures ? &*with.Textures : nullptr);
        std::vector<PlacedSlot> placed;
        return fragment && PlaceShader(*fragment, inputs, {}, with, mac, placed, why) && fragment->Translate("ours_fragment", source, why);
    };
    // Without the table of textures (a game whose engine is another), the shader is refused for the table itself.
    std::string none;
    CHECK(!translated(table.WithoutTextureTable(), none));
    // As many arrays as the Mac game has, or it is refused.
    {
        auto fewer = table;
        fewer.Textures->SpecularArrays = 3;
        CHECK(!translated(fewer, none) && why.find("SpecRGB is 4 textures in the Mac game, 3 in the shader") != std::string::npos);
    }
    auto vertex = SpirvShader::Parse(SpirvVertexModule(), why);
    CHECK(vertex != nullptr);
    if (!vertex) return;
    std::vector<PlacedSlot> placed;
    std::string sources[2];
    CHECK(PlaceShader(*vertex, inputs, {}, table, mac, placed, why) && vertex->Translate("ours_vertex", sources[0], why));
    CHECK(translated(table, sources[1]));
    auto ours = MetalFunctions::Compile(sources, why);
    CHECK(ours != nullptr);
    if (!ours) { printf("    %s\n%s\n", why.c_str(), sources[1].c_str()); return; }
    MacPass ourPass;
    CHECK(ours->Reflect("ours_vertex", "ours_fragment", ourPass, why));
    auto indexOf = [&](char const* name) {
        for (auto const& r : ourPass.Fragment) if (r.Name == name) return (int)r.Index;
        return -1;
    };
    // Each of the Mac's textures where the game's function takes it.
    CHECK(indexOf("DiffRGB") == 1 && indexOf("DiffAlpha") == 2 && indexOf("SpecRGB") == 3 && indexOf("Cookie") == 11 && indexOf("Probes") == 5 && indexOf("Lights") == 6);

    // The same bytes and textures to both; other layers, arrays and cookies from draw to draw.
    for (uint32_t draw = 0; draw < 6; draw++) {
        uint32_t diffuseLayer = draw % 4, specularArray = (draw + 1) % 4, specularLayer = (draw + 2) % 4, cookieArray = draw % 6, cookieLayer = (draw + 3) % 4;
        Bytes probes(64, 0), lights(64, 0);
        uint32_t diffuse = 1u | 2u | 4u | ((diffuseLayer + 1) << 3), specularWord = 2u | (3u << 2) | (specularArray << 11) | ((specularLayer + 1) << 13),
            cookieWord = 1u | (cookieArray << 1) | ((cookieLayer + 1) << 4);
        memcpy(probes.data() + 16, &diffuse, 4);
        memcpy(probes.data() + 20, &specularWord, 4);
        memcpy(lights.data() + 16, &cookieWord, 4);
        DrawSetup setup;
        setup.Attributes = { { 0, 'f' }, { 3, 'f' } };
        setup.Buffers = { { 5, probes, false }, { 6, lights, false } };
        setup.Textures = { { 1, "cubearray", false, 11, false, false, -1, 0, false, false, true }, { 2, "cubearray", false, 12, false, false, -1, 0, false, false, true } };
        for (uint32_t i = 0; i < 4; i++) {
            setup.Textures.push_back({ 3 + i, "cubearray", false, 20 + i, false, false, -1, 0, false, false, true });
            setup.Textures.push_back({ 7 + i, "cubearray", false, 30 + i, false, false, -1, 0, false, false, true });
        }
        for (uint32_t i = 0; i < 6; i++) setup.Textures.push_back({ 11 + i, "2darray", false, 40 + i, false });
        setup.Samplers = { { 2, false, 0 } };
        DrawResult theirs, mine;
        CHECK(game->Draw("game_vertex", "game_fragment", setup, theirs, why));
        CHECK(ours->Draw("ours_vertex", "ours_fragment", setup, mine, why));
        uint32_t drawn = 0, different = 0;
        for (size_t i = 0; i < theirs.Color[0].size() && i < mine.Color[0].size(); i++) {
            drawn += theirs.Color[0][i] != MetalFunctions::kClearColor;
            different += std::fabs(theirs.Color[0][i] - mine.Color[0][i]) > 1e-3f;
        }
        CHECK(drawn > 1000 && different == 0);
        // And the picture is another with another layer: the draw has something to show.
        static std::vector<float> first;
        if (!draw) first = theirs.Color[0];
        else CHECK(first != theirs.Color[0]);
    }
}

TEST(a_kept_pipeline_is_not_taken_for_another_set_of_functions)
{
    // A vertex function drawn with a fragment function of another set, then with the function of the same name of a
    // set made after the first is gone: the second picture is the second function's, wherever the set came to lie.
    std::string why;
    std::string gameSources[] = { kGameSource };
    auto game = MetalFunctions::Compile(gameSources, why);
    if (!game) {
        printf("    (no Metal here: %s)\n", why.c_str());
        return;
    }
    DrawSetup setup;
    setup.Attributes = { { 0, 'f' }, { 3, 'f' } };
    for (int turn = 0; turn < 6; turn++) {
        float number = 0.125f * (float)(turn + 1);
        std::string writers[] = { "#include <metal_stdlib>\nusing namespace metal;\nstruct VOut { float4 position [[position]]; float4 tint; };\n"
            "fragment float4 writer(VOut in [[stage_in]]) { return float4(" + std::to_string(number) + "); }\n" };
        auto writer = MetalFunctions::Compile(writers, why);
        CHECK(writer != nullptr);
        if (!writer) return;
        DrawResult drawn;
        CHECK(game->Draw("game_vertex", "writer", setup, drawn, why, writer.get()));
        uint32_t pixels = 0, wrong = 0;
        for (size_t i = 0; i < drawn.Color[0].size(); i++) {
            if (drawn.Depth[i / 4] == 1.0f) continue;
            pixels++;
            wrong += drawn.Color[0][i] != number;
        }
        CHECK(pixels > 1000 && wrong == 0);
    }
}

TEST(a_depth_texture_has_content_and_a_sampler_compares_against_it)
{
    // A function that asks a shadow map whether a depth is in the light gets both answers over a picture: the
    // depth texture is not all one depth, and the sampler compares.
    std::string why;
    std::string sources[] = { R"(
#include <metal_stdlib>
using namespace metal;
struct VIn { float3 Position [[attribute(0)]]; float4 Tint [[attribute(3)]]; };
struct VOut { float4 position [[position]]; float4 tint; };
vertex VOut shadow_vertex(VIn in [[stage_in]]) { VOut o; o.position = float4(in.Position, 1.0); o.tint = in.Tint; return o; }
fragment float4 shadow_fragment(VOut in [[stage_in]], depth2d_array<float> Shadow [[texture(2)]], sampler ShadowSampler [[sampler(1)]])
{
    return float4(Shadow.sample_compare(ShadowSampler, in.position.xy / 64.0, 1, 0.5, level(0.0)));
}
)" };
    auto functions = MetalFunctions::Compile(sources, why);
    if (!functions) {
        printf("    (no Metal here: %s)\n", why.c_str());
        return;
    }
    DrawSetup setup;
    setup.Attributes = { { 0, 'f' }, { 3, 'f' } };
    setup.Textures.push_back({ 2, "2darray", true, 7, false });
    setup.Samplers.push_back({ 1, false, 2 });
    DrawResult drawn;
    CHECK(functions->Draw("shadow_vertex", "shadow_fragment", setup, drawn, why));
    if (!why.empty()) printf("    %s\n", why.c_str());
    uint32_t lit = 0, shaded = 0;
    for (size_t pixel = 0; pixel < drawn.Depth.size(); pixel++) {
        if (drawn.Depth[pixel] == 1.0f) continue;
        (drawn.Color[0][pixel * 4] > 0.5f ? lit : shaded)++;
    }
    CHECK(lit > 100 && shaded > 100);
}

TEST(a_shader_with_anything_uncertain_is_not_placed)
{
    std::string why;
    std::string gameSources[] = { kGameSource };
    auto game = MetalFunctions::Compile(gameSources, why);
    if (!game) return;
    MacPass gamePass;
    CHECK(game->Reflect("game_vertex", "game_fragment", gamePass, why));
    MacInterface mac;
    mac.Add(gamePass);
    std::vector<InputSemantic> inputs = { { "POSITION", 0 }, { "COLOR", 2 } };
    std::vector<PlacedSlot> placed;
    std::string source;

    auto refused = [&](Bytes const& module, InterfaceTable const& table, std::span<InputSemantic const> semantics, bool, char const* reason) {
        auto shader = SpirvShader::Parse(module, why);
        if (!shader) return false;
        bool ok = PlaceShader(*shader, semantics, {}, table, mac, placed, why);
        return !ok && why.find(reason) != std::string::npos;
    };

    // Nothing known at the binding; a slot of another type; a block of another shape.
    CHECK(refused(SpirvModule({ .Binding = 7 }), FakeTable(), inputs, false, "nothing is known"));
    auto wrongKind = FakeTable();
    wrongKind.Slots[0].Kind = ResourceKind::Texture;
    CHECK(refused(SpirvModule(), wrongKind, inputs, false, "has a buffer at set 1, binding 2, where the game has a texture (Fake): it was built for another version"));
    auto wrongType = FakeTable();
    wrongType.Slots[0].Type = "array";
    CHECK(refused(SpirvModule(), wrongType, inputs, false, "not of the type"));
    auto wrongShape = FakeTable();
    wrongShape.Blocks[0].Types[0] = "f3";
    CHECK(refused(SpirvModule(), wrongShape, inputs, false, "in the interface table"));
    auto wrongOffset = FakeTable();
    wrongOffset.Blocks[0].Offsets[0] = 8;
    CHECK(refused(SpirvModule(), wrongOffset, inputs, false, "in the interface table"));
    auto shorter = FakeTable();
    shorter.Blocks[0] = { "Fake", {}, {}, {} };
    CHECK(refused(SpirvModule(), shorter, inputs, false, "in the interface table"));        // the shader's block has more than today's
    // A block that has grown at its end since the shader was built: the shader's is its beginning.
    auto grown = FakeTable();
    grown.Blocks[0] = { "Fake", { "second", "first" }, { "f4", "f4" }, { 0, 16 } };
    CHECK(!refused(SpirvModule(), grown, inputs, false, "in the interface table"));

    // A member the shader reads that the Mac block does not have, by name or by type.
    auto noMember = FakeTable();
    noMember.Blocks[0].Members[0] = "";
    CHECK(refused(SpirvModule(), noMember, inputs, false, "reads member 0 of Fake, which has no counterpart"));
    noMember.Blocks[0].Absent[0] = "gone";
    CHECK(refused(SpirvModule(), noMember, inputs, false, "reads gone of Fake, which has no counterpart"));
    // A member the Mac block has under the same name and type, holding another thing, is not that member.
    auto otherMeaning = FakeTable();
    otherMeaning.Blocks[0].Absent[0] = "second";
    otherMeaning.Blocks[0].OtherMeaning[0] = "its reciprocal";
    CHECK(refused(SpirvModule(), otherMeaning, inputs, false, "reads second of Fake, and the Mac game's member of that name holds its reciprocal"));
    // A member no shader of the game reads is paired by name alone, and a shader that reads it is placed by that.
    {
        auto untested = FakeTable();
        untested.Blocks[0].Untested.push_back(0);
        auto shader = SpirvShader::Parse(SpirvModule(), why);
        CHECK(shader && PlaceShader(*shader, inputs, {}, untested, mac, placed, why));
    }
    auto otherMember = FakeTable();
    otherMember.Blocks[0].Members[0] = "absent";
    CHECK(refused(SpirvModule(), otherMember, inputs, false, "reads absent of Fake, which has no counterpart"));

    // A name the Mac game has no fixed place for.
    auto unknownName = FakeTable();
    unknownName.Slots[0].Name = unknownName.Blocks[0].Name = "Elsewhere";
    CHECK(refused(SpirvModule(), unknownName, inputs, false, "no fixed place"));
    // An entry that was paired by order alone is used, and named.
    {
        auto shader = SpirvShader::Parse(SpirvModule(), why);
        std::vector<std::string> byOrderOnly;
        CHECK(shader && PlaceShader(*shader, inputs, {}, FakeTable(true), mac, placed, why, &byOrderOnly));
        CHECK(byOrderOnly == std::vector<std::string>{ "Fake" });
        byOrderOnly.clear();
        CHECK(shader && PlaceShader(*shader, inputs, {}, FakeTable(), mac, placed, why, &byOrderOnly) && byOrderOnly.empty());
    }

    // A vertex input the signature does not have, or one with no Mac attribute.
    std::vector<InputSemantic> shortSignature = { { "POSITION", 0 } };
    CHECK(refused(SpirvVertexModule(), FakeTable(), shortSignature, false, "not in the DirectX input signature"));
    std::vector<InputSemantic> otherSemantic = { { "POSITION", 0 }, { "BINORMAL", 0 } };
    CHECK(refused(SpirvVertexModule(), FakeTable(), otherSemantic, false, "no Mac attribute"));

    // Nothing placed, nothing translated.
    auto unplaced = SpirvShader::Parse(SpirvModule(), why);
    CHECK(unplaced && !unplaced->Translate("f", source, why) && why.find("no place") != std::string::npos);
    CHECK(!SpirvShader::Parse(Text("not a module"), why));
}

// --- the decision ----------------------------------------------------------------

#define VBASE "Public/Shared/Assets/Materials/Base/"
#define MBASE "Public/A/Assets/Materials/Base/"

using Files = std::map<std::string, Bytes>;

MaterialSource Memory(std::string label, Files files)
{
    MaterialSource source;
    source.Label = std::move(label);
    for (auto const& [name, data] : files) source.Names.insert(name);
    source.Read = [files = std::move(files)](std::string const& name) -> std::optional<Bytes> {
        auto it = files.find(name);
        if (it == files.end() || it->second == Text("<unreadable>")) return {};
        return it->second;
    };
    return source;
}

// Containers "AAA", "BBB", "CCC" are the vanilla V_ST_FOR, V_ST_DEF and W_ST_DEF.
ContainerIdentity FakeIdentity()
{
    return [](std::span<uint8_t const> container) -> std::vector<uint64_t> {
        std::string_view key((char const*)container.data(), container.size());
        if (key == "AAA") return { PassStemHash(VBASE "V_ST_FOR") };
        if (key == "BBB") return { PassStemHash(VBASE "V_ST_DEF") };
        if (key == "CCC") return { PassStemHash(VBASE "W_ST_DEF") };
        return {};
    };
}

MaterialSource Vanilla()
{
    return Memory("Materials.pak", {
        { VBASE "V.lsf", MacMaterial() },
        { VBASE "V_ST_FOR_Metal.bshd", Text("STUB-FOR") },
        { VBASE "W.lsf", MacMaterial() },
        { VBASE "W_ST_DEF_Metal.bshd", Text("STUB-W") },
    });
}

ProvisionDecision const* DecisionFor(ProvisionMap const& map, std::string_view material, std::string_view package = {})
{
    for (auto const& d : map.Decisions) {
        if (d.Material == material && (package.empty() || d.Package == package)) return &d;
    }
    return nullptr;
}

TEST(provision_decides_twins_and_serves_their_files)
{
    auto vanilla = Vanilla();
    std::vector<MaterialSource> mods;
    mods.push_back(Memory("A.pak", {
        { MBASE "X.lsf", ModMaterial() }, { MBASE "X_ST_FOR_Vulkan.bshd", Text("AAA") }, { MBASE "X_ST_DEF_Vulkan.bshd", Text("BBB") },
        { MBASE "Y.lsf", ModMaterial() }, { MBASE "Y_ST_FOR_Vulkan.bshd", Text("ZZZ") },
        { MBASE "Z.lsf", ModMaterial() }, { MBASE "Z_ST_FOR_Vulkan.bshd", Text("AAA") }, { MBASE "Z_ST_DEF_Vulkan.bshd", Text("CCC") },
        { MBASE "N.lsf", ModMaterial() },
    }));
    auto map = BuildProvisionMap(vanilla, mods, FakeIdentity());

    CHECK(map->Decisions.size() == 3);      // N ships no shaders and replaces nothing: no decision
    CHECK(map->Count(ProvisionKind::Twin) == 1);
    CHECK(map->Count(ProvisionKind::Refused) == 2);

    auto x = DecisionFor(*map, MBASE "X.lsf");
    CHECK(x && x->Kind == ProvisionKind::Twin && x->Vanilla == VBASE "V.lsf");
    CHECK(x && x->Passes == 2 && x->Matched == 2 && x->Stubs == 1);     // vanilla has no ST_DEF stub
    auto y = DecisionFor(*map, MBASE "Y.lsf");
    CHECK(y && y->Kind == ProvisionKind::Refused && y->Reason.find("no Metal function") != std::string::npos);
    auto z = DecisionFor(*map, MBASE "Z.lsf");
    CHECK(z && z->Kind == ProvisionKind::Refused && z->Reason.find("different materials") != std::string::npos);

    CHECK(map->Files.size() == 2);
    auto lsf = map->Find("/Game/Data/" MBASE "X.lsf");
    CHECK(lsf && lsf->IsMaterial);
    auto stub = map->Find(MBASE "X_ST_FOR_Metal.bshd");
    CHECK(stub && !stub->IsMaterial && stub->Data == Text("STUB-FOR"));
    CHECK(map->Find("Other" MBASE "X.lsf") == nullptr);        // a suffix, but not at a '/' boundary
    CHECK(map->Find(MBASE "Y.lsf") == nullptr);
    CHECK(map->Find("X.lsf") == nullptr);

    // The served .lsf has the Mac structure and reads back.
    if (lsf) {
        LsfDoc doc;
        char err[128];
        CHECK(lsf_parse(lsf->Data.data(), lsf->Data.size(), &doc, err, sizeof(err)));
        lsf_free(&doc);
        CHECK(lsf->Data != ModMaterial());
    }
}

TEST(provision_skips_a_material_another_package_has_metal_stubs_for)
{
    auto vanilla = Vanilla();
    std::vector<MaterialSource> mods;
    mods.push_back(Memory("A.pak", { { MBASE "X.lsf", ModMaterial() }, { MBASE "X_ST_FOR_Vulkan.bshd", Text("AAA") } }));
    mods.push_back(Memory("Companion.pak", { { MBASE "X_ST_FOR_Metal.bshd", Text("MINE") } }));
    auto map = BuildProvisionMap(vanilla, mods, FakeIdentity());
    auto x = DecisionFor(*map, MBASE "X.lsf");
    CHECK(x && x->Kind == ProvisionKind::Skipped && x->Reason.find("Companion.pak") != std::string::npos);
    CHECK(map->Files.empty());
}

TEST(provision_lets_the_later_package_win_a_material_path)
{
    auto vanilla = Vanilla();
    std::vector<MaterialSource> mods;
    mods.push_back(Memory("Early.pak", { { MBASE "X.lsf", ModMaterial() }, { MBASE "X_ST_FOR_Vulkan.bshd", Text("AAA") } }));
    mods.push_back(Memory("Late.pak", { { MBASE "X.lsf", ModMaterial() }, { MBASE "X_ST_FOR_Vulkan.bshd", Text("ZZZ") } }));
    auto map = BuildProvisionMap(vanilla, mods, FakeIdentity());
    auto early = DecisionFor(*map, MBASE "X.lsf", "Early.pak");
    auto late = DecisionFor(*map, MBASE "X.lsf", "Late.pak");
    CHECK(early && early->Kind == ProvisionKind::Superseded);
    CHECK(late && late->Kind == ProvisionKind::Refused);
    CHECK(map->Files.empty());      // the early twin's files must not be served for the late package's material

    std::swap(mods[0], mods[1]);
    map = BuildProvisionMap(vanilla, mods, FakeIdentity());
    CHECK(map->Count(ProvisionKind::Twin) == 1 && map->Files.size() == 2);
}

TEST(provision_names_what_it_does_not_handle)
{
    auto vanilla = Vanilla();
    std::vector<MaterialSource> mods;
    mods.push_back(Memory("A.pak", {
        { VBASE "V.lsf", ModMaterial() },                                                                // a vanilla .lsf replaced, no shaders
        { MBASE "U.lsf", ModMaterial() }, { MBASE "U_ST_DEF_Vulkan.bshd", Text("CCC") },                 // twin of W, whose stub cannot be read
    }));
    auto broken = Memory("Materials.pak", {
        { VBASE "V.lsf", MacMaterial() }, { VBASE "W.lsf", MacMaterial() }, { VBASE "W_ST_DEF_Metal.bshd", Text("<unreadable>") },
    });
    auto map = BuildProvisionMap(broken, mods, FakeIdentity());
    auto v = DecisionFor(*map, VBASE "V.lsf");
    CHECK(v && v->Kind == ProvisionKind::Refused && v->Reason.find("without shipping shaders") != std::string::npos);
    auto u = DecisionFor(*map, MBASE "U.lsf");
    CHECK(u && u->Kind == ProvisionKind::Refused && u->Reason.find("cannot read") != std::string::npos);
    CHECK(map->Files.empty());
}

TEST(provisioner_serves_a_stub_only_after_its_material)
{
    auto vanilla = Vanilla();
    std::vector<MaterialSource> mods;
    mods.push_back(Memory("A.pak", { { MBASE "X.lsf", ModMaterial() }, { MBASE "X_ST_FOR_Vulkan.bshd", Text("AAA") } }));
    ShaderProvisioner provisioner(BuildProvisionMap(vanilla, mods, FakeIdentity()));

    CHECK(provisioner.OnFileOpened("/root/" MBASE "X_ST_FOR_Metal.bshd").empty());     // the game has the mod's own .lsf
    CHECK(provisioner.OnFileOpened("/root/Public/Shared/Assets/Materials/Other.lsf").empty());
    CHECK(!provisioner.OnFileOpened("/root/" MBASE "X.lsf").empty());
    auto stub = provisioner.OnFileOpened("/root/" MBASE "X_ST_FOR_Metal.bshd");
    CHECK(Bytes(stub.begin(), stub.end()) == Text("STUB-FOR"));
}

TEST(provisioner_switches_to_a_new_plan)
{
    auto vanilla = Vanilla();
    std::vector<MaterialSource> with;
    with.push_back(Memory("A.pak", { { MBASE "X.lsf", ModMaterial() }, { MBASE "X_ST_FOR_Vulkan.bshd", Text("AAA") } }));
    std::vector<MaterialSource> without;

    ShaderProvisioner provisioner;
    CHECK(provisioner.OnFileOpened("/root/" MBASE "X.lsf").empty());       // nothing planned yet

    provisioner.Adopt(BuildProvisionMap(vanilla, with, FakeIdentity()));
    auto served = provisioner.OnFileOpened("/root/" MBASE "X.lsf");
    CHECK(!served.empty());
    Bytes copy(served.begin(), served.end());

    // The same material in the next plan: the game still holds it, so its stubs stay servable.
    provisioner.Adopt(BuildProvisionMap(vanilla, with, FakeIdentity()));
    CHECK(!provisioner.OnFileOpened("/root/" MBASE "X_ST_FOR_Metal.bshd").empty());

    // The mod was disabled: nothing of it is served any more, and bytes handed out earlier are still intact.
    provisioner.Adopt(BuildProvisionMap(vanilla, without, FakeIdentity()));
    CHECK(provisioner.OnFileOpened("/root/" MBASE "X.lsf").empty());
    CHECK(provisioner.OnFileOpened("/root/" MBASE "X_ST_FOR_Metal.bshd").empty());
    CHECK(Bytes(served.begin(), served.end()) == copy);

    // Enabled again: a fresh material, so the stub waits for its .lsf once more.
    provisioner.Adopt(BuildProvisionMap(vanilla, with, FakeIdentity()));
    CHECK(provisioner.OnFileOpened("/root/" MBASE "X_ST_FOR_Metal.bshd").empty());
    CHECK(!provisioner.OnFileOpened("/root/" MBASE "X.lsf").empty());
    CHECK(!provisioner.OnFileOpened("/root/" MBASE "X_ST_FOR_Metal.bshd").empty());
}

// The game reads a material before the plan that serves it exists (the player
// disables a companion pack at the menu), and again later.
TEST(provisioner_tells_requests_from_served_files_across_a_replan)
{
    auto vanilla = Vanilla();
    std::vector<MaterialSource> mods;
    mods.push_back(Memory("A.pak", {
        { MBASE "X.lsf", ModMaterial() }, { MBASE "X_ST_FOR_Vulkan.bshd", Text("AAA") },
        { MBASE "R.lsf", ModMaterial() }, { MBASE "R_ST_FOR_Vulkan.bshd", Text("not vanilla") },
    }));

    ShaderProvisioner provisioner;
    CHECK(provisioner.Progress() == ProvisionProgress{});
    CHECK(!provisioner.LogProgressIfChanged());                             // no plan, nothing to say
    CHECK(provisioner.OnFileOpened("/root/" MBASE "X.lsf").empty());        // read as the mod ships it

    provisioner.Adopt(BuildProvisionMap(vanilla, mods, FakeIdentity()));
    auto p = provisioner.Progress();
    CHECK(p.Files == 2 && p.Materials == 1 && p.Refused == 1);
    CHECK(p.ReadBefore == 1 && p.FilesAsked == 0 && p.Requests == 0 && p.MaterialsServed == 0);
    CHECK(provisioner.LogProgressIfChanged());
    CHECK(!provisioner.LogProgressIfChanged());                             // unchanged since

    // The game still holds the material it read: it asks for the stub only.
    CHECK(provisioner.OnFileOpened("/root/" MBASE "X_ST_FOR_Metal.bshd").empty());
    p = provisioner.Progress();
    CHECK(p.FilesAsked == 1 && p.Requests == 1 && p.Withheld == 1 && p.FilesServed == 0 && p.ReadBefore == 1);
    CHECK(provisioner.LogProgressIfChanged());

    // A file of no plan and the refused material: seen, never served.
    CHECK(provisioner.OnFileOpened("/root/Public/Shared/Assets/Materials/Other.lsf").empty());
    CHECK(provisioner.Progress() == p);
    CHECK(provisioner.OnFileOpened("/root/" MBASE "R.lsf").empty());
    CHECK(provisioner.OnFileOpened("/root/" MBASE "R.lsf").empty());
    CHECK(provisioner.Progress().RefusedAsked == 1);

    // The game lets go of the material and loads it again.
    CHECK(!provisioner.OnFileOpened("/root/" MBASE "X.lsf").empty());
    CHECK(!provisioner.OnFileOpened("/root/" MBASE "X_ST_FOR_Metal.bshd").empty());
    p = provisioner.Progress();
    CHECK(p.FilesAsked == 2 && p.FilesServed == 2 && p.Requests == 3 && p.Withheld == 1);
    CHECK(p.MaterialsServed == 1 && p.ReadBefore == 0);

    // The same plan again: what the game holds is what the plan serves.
    provisioner.Adopt(BuildProvisionMap(vanilla, mods, FakeIdentity()));
    p = provisioner.Progress();
    CHECK(p.ReadBefore == 0 && p.MaterialsServed == 1 && p.Requests == 0);
    CHECK(!provisioner.OnFileOpened("/root/" MBASE "X_ST_FOR_Metal.bshd").empty());

    // The mod leaves and comes back with the material changed: the game got other bytes for it.
    provisioner.Adopt(BuildProvisionMap(vanilla, {}, FakeIdentity()));
    CHECK(provisioner.Progress().Files == 0);
    CHECK(provisioner.OnFileOpened("/root/" MBASE "X.lsf").empty());
    provisioner.Adopt(BuildProvisionMap(vanilla, mods, FakeIdentity()));
    CHECK(provisioner.Progress().ReadBefore == 1);
}

TEST(a_stage_holds_one_module_or_two_in_either_chunk_format)
{
    auto module = SpirvModule();
    auto chunk = [&](char const* magic, std::vector<Bytes> const& modules) {
        Bytes stage;
        Put(stage, magic, 4);
        uint8_t words[6] = {};
        Put(stage, words, sizeof(words));
        if (!strcmp(magic, "SHD8")) {
            Put(stage, (uint32_t)modules[0].size());
            Put(stage, (uint32_t)(modules.size() > 1 ? modules[1].size() : 0));
            uint8_t digests[32] = {};
            Put(stage, digests, sizeof(digests));
        }
        for (auto const& m : modules) Put(stage, m.data(), m.size());
        return stage;
    };
    // The modules are views of the stage: it is kept while they are looked at.
    auto single = chunk("SHD8", { module });
    auto one = VulkanStageModules(single);
    CHECK(one.size() == 1 && Bytes(one[0].begin(), one[0].end()) == module);
    auto other = SpirvModule({ .Constant = 1 });
    auto pair = chunk("SHD8", { module, other });
    auto two = VulkanStageModules(pair);
    CHECK(two.size() == 2 && Bytes(two[0].begin(), two[0].end()) == module && Bytes(two[1].begin(), two[1].end()) == other);
    // The toolkit's earlier chunk: one module after a shorter header.
    auto earlier = chunk("SHD7", { module });
    auto old = VulkanStageModules(earlier);
    CHECK(old.size() == 1 && Bytes(old[0].begin(), old[0].end()) == module);

    auto cut = chunk("SHD8", { module });
    cut.pop_back();
    CHECK(VulkanStageModules(cut).empty());
    CHECK(VulkanStageModules(chunk("SHD7", { Text("not a module, whatever its length is") })).empty());
    CHECK(VulkanStageModules(chunk("SHD6", { module })).empty() && VulkanStageModules({}).empty());
}

TEST(a_matrix_the_shader_builds_is_transposed_where_the_mac_game_has_it_so)
{
    // What the module builds, and how it uses it.
    std::string why;
    std::vector<BuiltMatrix> found;
    auto plain = SpirvShader::Parse(SpirvModule({ .Rotation = true }), why, [&](BuiltMatrix const& m, uint32_t) { found.push_back(m); return false; });
    CHECK(plain != nullptr && found.size() == 1);
    if (found.size() != 1) return;
    CHECK(found[0].Size == 2 && found[0].ColumnsFromScalars && found[0].ColumnsBuilt && found[0].TimesVectorLeft);
    CHECK(!found[0].TimesVectorRight && !found[0].TimesMatrixLeft && !found[0].TimesMatrixRight && !found[0].OtherUse);
    CHECK(MacMatrices()(found[0], 0));
    BuiltMatrix larger = found[0];
    larger.Size = 3;
    BuiltMatrix fromVectors = found[0];
    fromVectors.ColumnsFromScalars = false;
    CHECK(!MacMatrices()(larger, 0) && !MacMatrices()(fromVectors, 0));

    std::string gameSources[] = { kGameSource };
    auto game = MetalFunctions::Compile(gameSources, why);
    if (!game) return;
    MacPass gamePass;
    CHECK(game->Reflect("game_vertex", "game_fragment", gamePass, why));
    MacInterface mac;
    mac.Add(gamePass);
    auto table = FakeTable();
    std::vector<PlacedSlot> placed;

    auto translate = [&](MatrixRule const& rule) {
        std::string source;
        auto shader = SpirvShader::Parse(SpirvModule({ .Rotation = true }), why, rule);
        if (!shader || !PlaceShader(*shader, {}, {}, table, mac, placed, why) || !shader->Translate("f", source, why)) return std::string();
        return source;
    };
    auto asBuilt = translate({}), turned = translate(MacMatrices());
    CHECK(!asBuilt.empty() && !turned.empty() && asBuilt != turned);
    CHECK(asBuilt.find("transpose(") == std::string::npos && turned.find("transpose(") != std::string::npos);
    // A product that must not be contracted is a call of a helper, which adds
    // the terms first to last as the game's own functions do.
    {
        std::string source;
        auto shader = SpirvShader::Parse(SpirvModule({ .Rotation = true, .Precise = true }), why);
        CHECK(shader && PlaceShader(*shader, {}, {}, table, mac, placed, why) && shader->Translate("f", source, why));
        CHECK(source.find("spvFMulVectorMatrix(") != std::string::npos);
        CHECK(source.find("for (uint i = 1; i <= Rows; ++i)") != std::string::npos && source.find("i > 0; --i") == std::string::npos);
    }
    // A module without one is not touched by the rule.
    auto none = SpirvShader::Parse(SpirvModule(), why, [&](BuiltMatrix const&, uint32_t) { found.push_back({}); return true; });
    CHECK(none != nullptr && found.size() == 1);
}

// --- a material translated ----------------------------------------------------------

// A Vulkan container with both stages, and the DirectX 11 container that goes with it.
Bytes VulkanPass(std::string const& name, Bytes const& vertex, Bytes const& fragment, uint32_t tableVersion = 3)
{
    auto chunk = [](Bytes const& module) {
        Bytes stage;
        Put(stage, "SHD8", 4);
        uint8_t counts[6] = {};
        Put(stage, counts, sizeof(counts));
        Put(stage, (uint32_t)module.size());
        Put(stage, (uint32_t)0);
        uint8_t digests[32] = {};
        Put(stage, digests, sizeof(digests));
        Put(stage, module.data(), module.size());
        return stage;
    };
    Bytes v = chunk(vertex), f = fragment.empty() ? Bytes{} : chunk(fragment);
    Bytes o;
    Put(o, "BSHD", 4);
    Put(o, (uint32_t)2);
    uint32_t padded = (uint32_t)((name.size() + 4) & ~(size_t)3);
    Put(o, padded);
    Put(o, (uint32_t)name.size());
    Bytes nameBytes(padded);
    memcpy(nameBytes.data(), name.data(), name.size());
    Put(o, nameBytes.data(), nameBytes.size());
    uint32_t sizes[7] = { 4, (uint32_t)v.size(), 0, 0, 0, (uint32_t)f.size(), 0 };
    Put(o, sizes, sizeof(sizes));
    Put(o, tableVersion);       // the container's own table: its version and nothing else
    Put(o, v.data(), v.size());
    Put(o, f.data(), f.size());
    return o;
}

Bytes Dx11Pass(Bytes const& vertexStage)
{
    Bytes o;
    Put(o, "BSHD", 4);
    Put(o, (uint32_t)2);
    Put(o, (uint32_t)4);
    Put(o, (uint32_t)1);
    Put(o, "X\0\0", 4);
    uint32_t sizes[7] = { 0, (uint32_t)vertexStage.size(), 0, 0, 0, 0, 0 };
    Put(o, sizes, sizeof(sizes));
    Put(o, vertexStage.data(), vertexStage.size());
    return o;
}

TEST(a_material_is_translated_into_stubs_sources_and_a_mac_ready_lsf)
{
    auto stub = BuildMetalStub("X_ST_FOR_Metal", "vertexFunction", "fragmentFunction");
    ContainerParts parts;
    CHECK(ParseContainer(stub, parts) && parts.Version == 2 && parts.Reflection.empty());
    CHECK(std::string(parts.Name.begin(), parts.Name.end()) == "X_ST_FOR_Metal");
    CHECK(std::string(parts.Stages[0].begin(), parts.Stages[0].end()) == "vertexFunction");
    CHECK(std::string(parts.Stages[4].begin(), parts.Stages[4].end()) == "fragmentFunction");
    CHECK(ParseContainer(BuildMetalStub("X", "v", ""), parts) && parts.Stages[4].empty());

    auto same = TranslatedFunctionName(Text("AAA"), true);
    CHECK(same == TranslatedFunctionName(Text("AAA"), true) && same != TranslatedFunctionName(Text("AAB"), true));
    CHECK(same != TranslatedFunctionName(Text("AAA"), false) && same.starts_with(kTranslatedPrefix) && same.ends_with("_vertexMain"));

    std::string why;
    std::string gameSources[] = { kGameSource };
    auto game = MetalFunctions::Compile(gameSources, why);
    if (!game) return;
    MacPass gamePass;
    CHECK(game->Reflect("game_vertex", "game_fragment", gamePass, why));
    // The two blocks every material has, where the Mac game binds them.
    gamePass.Vertex.push_back(MacBlock("CB_EngineParams", 21, { { "WorldOffsetMatrix", 0, "f16" } }));
    gamePass.Fragment.push_back(MacBlock("CB_MaterialConstants", 20, { { "Any", 0, "f4" } }));
    MacInterface mac;
    mac.Add(gamePass);

    // A material whose forward pass reads an engine block, and declares its two own.
    auto material = BuildLsf(
        { { "Material", -1 }, { "ShaderDescriptions", 0 }, { "ShaderDesc_StaticForward", 1 }, { "EngineCBBinding", 2 }, { "MaterialCBSize", 2 },
          { "ShaderDesc_StaticBake", 1 }, { "EngineCBBinding", 5 } },
        { { 2, "EngineCBSize", LSF_T_INT, 96, nullptr },
          // In one byte, as a material of the game's has a block's indices; the other block's in four.
          { 3, "DxVsIndex", LSF_T_INT8, 0, nullptr }, { 3, "DxPsIndex", LSF_T_INT8, 0, nullptr },
          { 3, "VkDescriptorSet", LSF_T_INT, 1, nullptr }, { 3, "VkBindingIndex", LSF_T_INT, 0, nullptr },
          { 4, "DxVsIndex", LSF_T_INT, -1, nullptr }, { 4, "DxPsIndex", LSF_T_INT, 1, nullptr },
          { 4, "VkDescriptorSet", LSF_T_INT, 1, nullptr }, { 4, "VkBindingIndex", LSF_T_INT, 1, nullptr },
          { 6, "DxVsIndex", LSF_T_INT, 0, nullptr }, { 6, "DxPsIndex", LSF_T_INT, 0, nullptr } });
    auto dx11 = Dx11Pass(Dx11VertexStage({ { "POSITION", 0, 0 }, { "COLOR", 2, 0 } }));
    Files files = {
        { MBASE "X.lsf", material },
        { MBASE "X_ST_FOR_Vulkan.bshd", VulkanPass("X_ST_FOR_Vulkan", SpirvVertexModule(), SpirvModule()) },
        { MBASE "X_ST_FOR_DX11.bshd", dx11 },
        { MBASE "X_ST_BAKE_Vulkan.bshd", VulkanPass("X_ST_BAKE_Vulkan", SpirvVertexModule(), SpirvModule()) },
    };
    auto mod = Memory("A.pak", files);

    // A vertex stage that is not one of the game's own is translated, and the material says so.
    auto translated = TranslateMaterial(mod, MBASE "X.lsf", FakeTable(), mac, why);
    CHECK(translated.has_value());
    if (!translated) { printf("    %s\n", why.c_str()); return; }
    CHECK(translated->OwnVertexStages == 1);
    // One of the game's own is not said to be the mod's.
    SpirvHash vertexStage{}, fragmentStage{};
    auto vertexModule = SpirvVertexModule(), fragmentModule = SpirvModule();
    CHECK(SpirvModuleHash(vertexModule, vertexStage, why) && SpirvModuleHash(fragmentModule, fragmentStage, why));
    auto gameStage = FakeTable();
    gameStage.GameVertexStages.push_back(vertexStage);
    auto asTheGames = TranslateMaterial(mod, MBASE "X.lsf", gameStage, mac, why);
    CHECK(asTheGames.has_value() && asTheGames->OwnVertexStages == 0);

    // A pass is refused for an open fault it touches, by the fault's name:
    // a vertex stage of the game's that is known to draw elsewhere,
    auto faulty = gameStage;
    faulty.Faults = { { "F8", "the first thing" }, { "F9", "the second thing" } };
    faulty.FaultyVertexStages.push_back({ vertexStage, 0 });
    CHECK(!TranslateMaterial(mod, MBASE "X.lsf", faulty, mac, why));
    CHECK(why.starts_with("ST_FOR: its vertex stage is one of the game's own that draws elsewhere translated: open fault F8: the first thing"));
    // the two stages of a pass of the game's known to draw another picture,
    faulty.FaultyVertexStages.clear();
    faulty.FaultyPairs.push_back({ vertexStage, fragmentStage, 1 });
    CHECK(!TranslateMaterial(mod, MBASE "X.lsf", faulty, mac, why));
    CHECK(why.find("its two stages are those of a pass of the game's that draws another picture translated: open fault F9: the second thing") != std::string::npos);
    // though not the same vertex stage with another fragment stage,
    faulty.FaultyPairs[0].Fragment[0] ^= 1;
    CHECK(TranslateMaterial(mod, MBASE "X.lsf", faulty, mac, why).has_value());
    // and a thing of the engine's that a fault is about.
    faulty.FaultyPairs.clear();
    faulty.FaultyResources.push_back({ "Fake", 1 });
    CHECK(!TranslateMaterial(mod, MBASE "X.lsf", faulty, mac, why));
    CHECK(why.find("it takes Fake: open fault F9: the second thing") != std::string::npos);
    // Asked for every reason, each fault a pass touches is said.
    faulty.FaultyVertexStages.push_back({ vertexStage, 0 });
    std::vector<std::string> reasons;
    CHECK(!TranslateMaterial(mod, MBASE "X.lsf", faulty, mac, why, &reasons));
    CHECK(reasons.size() == 2 && reasons[0].find("open fault F8") != std::string::npos && reasons[1].find("open fault F9") != std::string::npos);
    CHECK(translated->Passes.size() == 1);              // the Mac game has no bake pass
    auto const& pass = translated->Passes[0];
    CHECK(pass.Pass == "ST_FOR" && pass.VertexName != pass.FragmentName && pass.VertexName.starts_with(kTranslatedPrefix));
    CHECK(pass.VertexSource.find(pass.VertexName) != std::string::npos && pass.FragmentSource.find(pass.FragmentName) != std::string::npos);
    CHECK(ParseContainer(pass.Stub, parts) && std::string(parts.Name.begin(), parts.Name.end()) == "X_ST_FOR_Metal");
    CHECK(std::string(parts.Stages[0].begin(), parts.Stages[0].end()) == pass.VertexName);
    CHECK(std::string(parts.Stages[4].begin(), parts.Stages[4].end()) == pass.FragmentName);
    std::string sources[] = { pass.VertexSource, pass.FragmentSource };
    CHECK(MetalFunctions::Compile(sources, why) != nullptr);

    // The .lsf: ready for Metal, the two blocks where the Mac game binds them
    // for the translated pass, everything else as the mod has it.
    ParsedLsfDoc doc(translated->Lsf);
    CHECK(doc.Ok);
    if (!doc.Ok) return;
    auto value = [&](uint32_t node, char const* name) {
        int64_t v = -99;
        auto attr = lsf_attr(&doc.Doc, node, name);
        if (attr) lsf_attr_int(attr, &v);
        return v;
    };
    auto ready = lsf_attr(&doc.Doc, 0, "MetalReady");
    CHECK(ready && ready->type == LSF_T_BOOL && ready->size == 1 && ready->value[0] == 1);
    CHECK(doc.Doc.node_count == 7 && value(2, "EngineCBSize") == 96);
    CHECK(value(3, "DxVsIndex") == 21 && value(3, "DxPsIndex") == 21 && value(3, "VkBindingIndex") == 0);
    CHECK(lsf_attr(&doc.Doc, 3, "DxPsIndex")->size == 1 && lsf_attr(&doc.Doc, 4, "DxPsIndex")->size == 4);
    CHECK(value(4, "DxVsIndex") == -1 && value(4, "DxPsIndex") == 20);          // a stage that does not take it still does not
    CHECK(value(6, "DxVsIndex") == 0 && value(6, "DxPsIndex") == 0);            // the bake pass was not translated

    // Any pass that cannot be placed refuses the material, and says which.
    files[MBASE "X_ST_DEP_Vulkan.bshd"] = VulkanPass("X_ST_DEP_Vulkan", SpirvVertexModule(), SpirvModule({ .Binding = 9 }));
    files[MBASE "X_ST_DEP_DX11.bshd"] = dx11;
    CHECK(!TranslateMaterial(Memory("A.pak", files), MBASE "X.lsf", FakeTable(), mac, why));
    CHECK(why.starts_with("ST_DEP: ") && why.find("gives no place for the buffer at set 1, binding 9") != std::string::npos);
    // So does one built for an earlier game: its table is of an earlier version.
    files[MBASE "X_ST_DEP_Vulkan.bshd"] = VulkanPass("X_ST_DEP_Vulkan", SpirvVertexModule(), SpirvModule(), 2);
    CHECK(!TranslateMaterial(Memory("A.pak", files), MBASE "X.lsf", FakeTable(), mac, why));
    CHECK(why.starts_with("ST_DEP: its container is of version 2") && why.find("another version of the game") != std::string::npos);
    // And one that has shaders of its own for skinned meshes, whatever its containers are: this game draws a
    // skinned mesh through the static ones, and such a material's do not take what it gives them.
    {
        auto skinned = files;
        skinned.erase(MBASE "X_ST_DEP_Vulkan.bshd");
        skinned[MBASE "X.lsf"] = BuildLsf(
            { { "Material", -1 }, { "ShaderDescriptions", 0 }, { "ShaderDesc_StaticForward", 1 }, { "ShaderDesc_SkinnedForward", 1 } },
            { { 2, "EngineCBSize", LSF_T_INT, 64, nullptr }, { 3, "EngineCBSize", LSF_T_INT, 6208, nullptr } });
        std::vector<std::string> reasons;
        CHECK(!TranslateMaterial(Memory("A.pak", skinned), MBASE "X.lsf", FakeTable(), mac, why, &reasons));
        CHECK(why.find("shaders of its own for skinned meshes (ShaderDesc_SkinnedForward)") != std::string::npos
            && why.find("would not follow the body") != std::string::npos && reasons.size() == 1);
    }
    files.erase(MBASE "X_ST_DEP_Vulkan.bshd");
    files.erase(MBASE "X_ST_FOR_DX11.bshd");
    CHECK(!TranslateMaterial(Memory("A.pak", files), MBASE "X.lsf", FakeTable(), mac, why) && why.find("DirectX 11") != std::string::npos);
    CHECK(!TranslateMaterial(mod, MBASE "Missing.lsf", FakeTable(), mac, why));
}

TEST(our_functions_answer_when_a_library_is_asked_for_them)
{
    std::string why;
    std::string gameSources[] = { kGameSource };
    auto game = MetalFunctions::Compile(gameSources, why);
    if (!game) return;
    CHECK(game->Has("game_vertex") && !game->Has("bg3mse_test_vertexMain") && !game->Has("missing"));

    // Before anything is installed our functions can be compiled, and are found by no one else.
    std::string ours[] = { R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 bg3mse_test_vertexMain(uint i [[vertex_id]]) { return float4(float(i)); }
fragment float4 bg3mse_test_fragmentMain() { return float4(1.0); }
fragment float4 helper_not_ours() { return float4(0.0); }
)" };
    auto before = CountTranslatedFunctions();
    CHECK(AddTranslatedFunctions(ours, why, "Public/Test/Assets/Materials/M.lsf"));
    CHECK(CountTranslatedFunctions().Kept == before.Kept + 2);       // the two with the prefix
    CHECK(!game->Has("bg3mse_test_vertexMain"));

    CHECK(InstallTranslatedFunctions(why));
    CHECK(InstallTranslatedFunctions(why));                           // once is enough, twice does no harm
    // The game's library is asked, as rf::GPUDevice::CreateShader asks it. The log names the material the
    // first time one of its functions is taken, and only then.
    std::string logPath = gTempDir + "/functions.log";
    FILE* log = fopen(logPath.c_str(), "w+");
    SetLogStream(log);
    CHECK(game->Has("bg3mse_test_vertexMain") && game->Has("bg3mse_test_fragmentMain"));
    CHECK(game->Has("bg3mse_test_vertexMain"));
    SetLogStream(nullptr);
    std::string logged;
    if (log) {
        fflush(log);
        rewind(log);
        char line[512];
        while (fgets(line, sizeof(line), log)) logged += line;
        fclose(log);
    }
    auto named = logged.find("the game took a translated function of Public/Test/Assets/Materials/M.lsf for the first time (bg3mse_test_vertexMain)");
    CHECK(named != std::string::npos && logged.find("the game took a translated function", named + 1) == std::string::npos);
    CHECK(game->Has("game_vertex") && game->Has("game_fragment"));    // its own are its own
    CHECK(!game->Has("helper_not_ours") && !game->Has("missing") && !game->Has("bg3mse_missing"));
    auto after = CountTranslatedFunctions();
    CHECK(after.Asked == before.Asked + 4 && after.Answered == before.Answered + 3);

    // What is found is a function a pipeline can be made of.
    MacPass pass;
    CHECK(game->Reflect("bg3mse_test_vertexMain", "bg3mse_test_fragmentMain", pass, why));

    std::string broken[] = { "this is not Metal" };
    CHECK(!AddTranslatedFunctions(broken, why) && why.find("refused") != std::string::npos);
    CHECK(CountTranslatedFunctions().Kept == after.Kept);
}

TEST(mac_interface_is_kept_between_runs_for_the_game_it_was_read_from)
{
    MacPass pass;
    pass.Attributes = { { "Position", 0, "f3" }, { "Weights", 4, "u4" } };
    pass.Vertex = { MacBlock("PerCamera", 11, { { "g_View", 0, "f16" }, { "g_Near", 64, "f1" } }) };
    MacResource texture;
    texture.Name = "Shadow";
    texture.Kind = ResourceKind::Texture;
    texture.Type = "2darray";
    texture.Index = 36;
    texture.Depth = true;
    pass.Fragment = { texture, MacBlock("CB_MaterialConstants", 20, { { "A", 0, "f4" } }) };
    MacPass other = pass;
    other.Fragment[1] = MacBlock("CB_MaterialConstants", 20, { { "B", 0, "f1" } });
    other.Attributes[1].Index = 5;
    MacInterface mac;
    mac.Add(pass);
    mac.Add(other);

    auto text = mac.Serialize();
    auto back = MacInterface::Parse(text);
    CHECK(back.has_value());
    if (!back) return;
    CHECK(back->Serialize() == text && back->Passes() == 2);
    auto camera = back->Resource("PerCamera", ResourceKind::Buffer);
    CHECK(camera && camera->Index == 11 && camera->Members.size() == 2 && camera->Members[1].Offset == 64 && back->HasFixedLayout(*camera));
    auto constants = back->Resource("CB_MaterialConstants", ResourceKind::Buffer);
    CHECK(constants && !back->HasFixedLayout(*constants));
    auto shadow = back->Resource("Shadow", ResourceKind::Texture);
    CHECK(shadow && shadow->Index == 36 && shadow->Type == "2darray" && shadow->Depth);
    CHECK(back->Attribute("Position", "f3") && !back->Attribute("Weights", "u4"));     // seen at two indices

    // Anything but what was written is not an interface.
    CHECK(!MacInterface::Parse(text.substr(0, text.size() - 3)) && !MacInterface::Parse(text + "X\n") && !MacInterface::Parse(""));
    CHECK(!MacInterface::Parse("P\t2\nR\t9\tx\t1\tblock\t0\t1\t1\t0\nE\n") && !MacInterface::Parse("M\tx\t0\tf1\nE\n"));

    // Kept in a file with what it was read from; another game's is not taken.
    std::string file = gTempDir + "/interface.txt";
    CHECK(!LoadMacInterface(file, "stamp-1"));
    CHECK(SaveMacInterface(file, "stamp-1", mac));
    auto kept = LoadMacInterface(file, "stamp-1");
    CHECK(kept && kept->Serialize() == text);
    CHECK(!LoadMacInterface(file, "stamp-2") && !LoadMacInterface(file, "") && !SaveMacInterface(file, "", mac));
    if (FILE* f = fopen(file.c_str(), "wb")) {      // cut short
        fputs("bg3mse Mac interface 2\nstamp-1\nP\t2\n", f);
        fclose(f);
    }
    CHECK(!LoadMacInterface(file, "stamp-1"));
    CHECK(MacInterfaceStamp(gTempDir + "/no-such-game").empty());
}

// A made-up game with ls::SetupForwardLightingConstants in it: the stretches the bridge was read from with something
// else between them, `twice` once more at its end (`outside`: past its end, in the function after it), `without`
// left out, one word of `changed` another. `named` false: the function is there under another name.
std::unique_ptr<FakeBinary> SunShadowGame(int twice = -1, int without = -1, int changed = -1, bool named = true, bool outside = false)
{
    auto stretches = SunShadowStretches();
    constexpr uint64_t kBegin = FakeBinary::Text + 0x100;
    constexpr uint32_t kNop = 0xd503201f;
    std::vector<uint32_t> words;
    for (int i = 0; i < (int)stretches.size(); i++) {
        if (i == without) continue;
        words.push_back(kNop);
        size_t at = words.size();
        words.insert(words.end(), stretches[i].begin(), stretches[i].end());
        if (i == changed) words[at + stretches[i].size() / 2] ^= 1u << 5;       // another register
    }
    words.push_back(kNop);
    uint64_t end = kBegin + 4 * words.size();
    if (twice >= 0) words.insert(words.end(), stretches[twice].begin(), stretches[twice].end());
    if (twice >= 0 && !outside) end = kBegin + 4 * words.size();
    auto binary = std::make_unique<FakeBinary>(false, std::vector<FakeBinary::Sym>{
        { named ? SunShadowFunctionName() : "__ZN2ls29SetupForwardLightingConstantsEv", kBegin, N_SECT | N_EXT, 1 },
        { "_afterSetup", end, N_SECT | N_EXT, 1 },
    });
    // Room for it in __text, which the file has up to the stubs.
    auto text = binary->At<section_64>(sizeof(mach_header_64) + sizeof(segment_command_64));
    text->size = 0x800;
    for (size_t i = 0; i < words.size(); i++) binary->Word(kBegin + 4 * i, words[i]);
    return binary;
}

// The Mac game's lighting block with the sun's shadow members where that function writes them.
MacInterface SunShadowInterface(uint32_t fadeAt = 0x18, char const* countType = "u1")
{
    MacPass pass;
    pass.Fragment = { MacBlock("ForwardLightingConstants", 14, { { "g_ShadowEnabled", 0xc, "f1" }, { "g_ShadowFadeAmount", fadeAt, "f1" },
        { "g_ShadowFarPlanes", 0x20, "f4" }, { "g_ShadowInvFrustumWidths", 0x30, "f4" }, { "g_ShadowInvFadeWorldSize", 0x140, "f4" },
        { "g_ShadowCascadeBias", 0x150, "f4" }, { "g_NumActiveCascades", 0x170, countType } }) };
    MacInterface mac;
    mac.Add(pass);
    return mac;
}

// The sun's shadow is bridged only in a game whose ls::SetupForwardLightingConstants is the one the bridge was
// read from: found by its name, with each stretch of its arithmetic in it exactly once.
TEST(the_sun_shadow_bridge_is_for_the_function_it_was_read_from)
{
    auto stretches = SunShadowStretches();
    CHECK(stretches.size() == 7);
    auto problem = [&](int twice, int without, int changed, bool named, bool outside) {
        auto binary = SunShadowGame(twice, without, changed, named, outside);
        GameImage image;
        if (!image.OpenFile(binary->File)) return std::string("the made-up game cannot be read");
        return CheckSunShadowBridge(image).Problem;
    };
    CHECK(problem(-1, -1, -1, true, false).empty());
    CHECK(problem(-1, -1, -1, false, false).find("has no ls::SetupForwardLightingConstants") != std::string::npos);
    for (int i = 0; i < (int)stretches.size(); i++) {
        CHECK(problem(i, -1, -1, true, false).find("found 2 times") != std::string::npos);
        CHECK(problem(i, -1, -1, true, true).empty());          // the second is another function's
        CHECK(problem(-1, i, -1, true, false).find("not found") != std::string::npos);
        CHECK(problem(-1, -1, i, true, false).find("not found") != std::string::npos);
    }
    CHECK(problem(-1, 3, -1, true, false).find("each cascade's bias and fade") != std::string::npos);

    // And the game's own functions read the members where that function writes them.
    std::string why;
    CHECK(SunShadowMembersWhereTheEngineWritesThem(SunShadowInterface(), why) && why.empty());
    CHECK(!SunShadowMembersWhereTheEngineWritesThem(SunShadowInterface(0x1c), why) && why.find("g_ShadowFadeAmount") != std::string::npos);
    CHECK(!SunShadowMembersWhereTheEngineWritesThem(SunShadowInterface(0x18, "f1"), why) && why.find("g_NumActiveCascades") != std::string::npos);
    CHECK(!SunShadowMembersWhereTheEngineWritesThem(MacInterface(), why) && why.find("no one layout") != std::string::npos);
}

// The table a game is translated with has the members that are worked out only where both hold.
TEST(the_interface_table_works_members_out_only_in_a_game_that_bears_them_out)
{
    auto derived = [](InterfaceTable const& table) {
        size_t count = 0;
        for (auto const& block : table.Blocks) count += block.Derived.size();
        return count;
    };
    auto lighting = BuiltInInterface().Block("ForwardLightingConstants");
    CHECK(lighting && lighting->Derived.size() == 4 && derived(BuiltInInterface()) == 4);
    if (!lighting) return;
    // Each is a member the Windows block has and the Mac block lacks, and is refused without the rest.
    for (auto const& [member, how] : lighting->Derived) CHECK(lighting->Absent.count(member) && lighting->Members[member].empty() && !how.Read.empty());
    CHECK(derived(BuiltInInterface().WithoutDerived()) == 0);

    auto tableFor = [&](std::unique_ptr<FakeBinary> binary, MacInterface const& mac, std::string& problem) {
        GameImage image;
        if (!image.OpenFile(binary->File)) { problem = "the made-up game cannot be read"; return InterfaceTable{}; }
        BridgeProblems problems;
        auto table = InterfaceFor(image, mac, problems);
        problem = problems.SunShadow;
        // This made-up game has none of the functions the table of textures rests on.
        CHECK(!table.Textures && !problems.TextureTable.empty());
        return table;
    };
    std::string problem;
    CHECK(derived(tableFor(SunShadowGame(), SunShadowInterface(), problem)) == 4 && problem.empty());
    CHECK(derived(tableFor(SunShadowGame(-1, 2), SunShadowInterface(), problem)) == 0 && problem.find("the fade, the far planes and the widths") != std::string::npos);
    CHECK(derived(tableFor(SunShadowGame(-1, -1, -1, false), SunShadowInterface(), problem)) == 0 && problem.find("has no ls::SetupForwardLightingConstants") != std::string::npos);
    CHECK(derived(tableFor(SunShadowGame(), SunShadowInterface(0x1c), problem)) == 0 && problem.find("g_ShadowFadeAmount") != std::string::npos);
    // The rest of the table is the same either way.
    auto without = tableFor(SunShadowGame(-1, 2), SunShadowInterface(), problem);
    CHECK(without.Blocks.size() == BuiltInInterface().Blocks.size() && without.Faults.size() == BuiltInInterface().Faults.size());
}

// A made-up game with the two functions of ls::ClusteredAssignmentStage that pack a probe's cubes and a light's
// cookie: each function's stretches with something else between them; in the function numbered `function`, the
// stretch `twice` once more at its end, `without` left out, one word of `changed` another.
std::unique_ptr<FakeBinary> TextureTableGame(int function = -1, int twice = -1, int without = -1, int changed = -1)
{
    auto functions = TextureTableFunctions();
    constexpr uint32_t kNop = 0xd503201f;
    std::vector<FakeBinary::Sym> symbols;
    std::vector<uint32_t> all;
    static const char* const kAfter[] = { "_afterFirst", "_afterSecond" };
    for (int f = 0; f < (int)functions.size() && f < 2; f++) {
        symbols.push_back({ functions[f].Function, FakeBinary::Text + 0x100 + 4 * all.size(), N_SECT | N_EXT, 1 });
        auto const& stretches = functions[f].Stretches;
        for (int i = 0; i < (int)stretches.size(); i++) {
            if (f == function && i == without) continue;
            all.push_back(kNop);
            size_t at = all.size();
            all.insert(all.end(), stretches[i].begin(), stretches[i].end());
            if (f == function && i == changed) all[at + stretches[i].size() / 2] ^= 1u << 5;
        }
        all.push_back(kNop);
        if (f == function && twice >= 0) all.insert(all.end(), stretches[twice].begin(), stretches[twice].end());
        symbols.push_back({ kAfter[f], FakeBinary::Text + 0x100 + 4 * all.size(), N_SECT | N_EXT, 1 });
        all.push_back(kNop);
    }
    auto binary = std::make_unique<FakeBinary>(false, symbols);
    auto text = binary->At<section_64>(sizeof(mach_header_64) + sizeof(segment_command_64));
    text->size = 0x800;
    for (size_t i = 0; i < all.size(); i++) binary->Word(FakeBinary::Text + 0x100 + 4 * i, all[i]);
    return binary;
}

// The Mac game's functions as the table of textures was read from them.
MacInterface TextureTableInterface(uint32_t specularArrays = 4, char const* cookieType = "2darray[]", uint32_t specAt = 140)
{
    MacPass pass;
    auto texture = [](char const* name, uint32_t index, char const* type, uint32_t count) {
        MacResource r;
        r.Name = name;
        r.Kind = ResourceKind::Texture;
        r.Index = index;
        r.Type = type;
        r.Size = count;
        r.Used = true;
        return r;
    };
    auto probes = MacBlock("LightProbeDataBuffer", 5, { { "SizeTransition", 0, "f4" }, { "InvWorldTransformWorldOffset", 16, "f12" }, { "SH", 64, "f4[4]" },
        { "Fade", 128, "f1" }, { "ProbeIntensity", 132, "f1" }, { "ProbeShapeType_Diff", 136, "u1" }, { "Spec", specAt, "u1" } });
    probes.Size = 144;
    auto lights = MacBlock("DirectionLightDataBuffer", 4, { { "Color", 0, "f3" }, { "LightCookiePreExpose", 12, "u1" }, { "AttenuationParamsZ", 16, "f4" } });
    lights.Size = 128;
    pass.Fragment = { probes, lights, texture("LightProbeDiffRGB", 45, "cubearray", 0), texture("LightProbeDiffAlpha", 46, "cubearray", 0),
        texture("LightProbeSpecRGB", 47, "cubearray[]", specularArrays), texture("LightProbeSpecAlpha", 51, "cubearray[]", specularArrays),
        texture("LightCookie", 63, cookieType, 6) };
    MacInterface mac;
    mac.Add(pass);
    return mac;
}

// The table of textures is bridged only in a game whose ls::ClusteredAssignmentStage packs a probe's cubes and a
// light's cookie the way that was read, and whose own functions take them as read.
TEST(the_table_of_textures_is_for_the_engine_it_was_read_from)
{
    auto functions = TextureTableFunctions();
    CHECK(functions.size() == 2 && functions[0].Stretches.size() == 8 && functions[1].Stretches.size() == 1);
    if (functions.size() != 2) return;
    auto problem = [&](int function, int twice, int without, int changed) {
        auto binary = TextureTableGame(function, twice, without, changed);
        GameImage image;
        if (!image.OpenFile(binary->File)) return std::string("the made-up game cannot be read");
        return CheckTextureTableBridge(image).Problem;
    };
    CHECK(problem(-1, -1, -1, -1).empty());
    for (int f = 0; f < 2; f++) {
        for (int i = 0; i < (int)functions[f].Stretches.size(); i++) {
            CHECK(problem(f, i, -1, -1).find("found 2 times") != std::string::npos);
            CHECK(problem(f, -1, i, -1).find("not found") != std::string::npos);
            CHECK(problem(f, -1, -1, i).find("not found") != std::string::npos);
        }
    }
    CHECK(problem(1, -1, 0, -1).find("AssignDiffuseLightProbe does not pack a diffuse cube's layer") != std::string::npos);
    CHECK(problem(0, -1, 2, -1).find("ExecuteWTKernel does not pack a specular cube's array and layer") != std::string::npos);
    {
        // No such functions at all.
        FakeBinary binary;
        GameImage image;
        CHECK(image.OpenFile(binary.File) && CheckTextureTableBridge(image).Problem.find("has no ls::ClusteredAssignmentStage::ExecuteWTKernel") != std::string::npos);
    }

    auto const& table = BuiltInInterface();
    CHECK(table.Textures.has_value());
    if (!table.Textures) return;
    std::string why;
    CHECK(TextureTableAsRead(TextureTableInterface(), table, why) && why.empty());
    CHECK(!TextureTableAsRead(TextureTableInterface(3), table, why) && why.find("LightProbeSpecRGB as 4 arrays") != std::string::npos);
    CHECK(!TextureTableAsRead(TextureTableInterface(4, "2darray"), table, why) && why.find("LightCookie as 6 arrays") != std::string::npos);
    CHECK(!TextureTableAsRead(TextureTableInterface(4, "2darray[]", 136), table, why) && why.find("Spec") != std::string::npos);
    CHECK(!TextureTableAsRead(MacInterface(), table, why) && !TextureTableAsRead(TextureTableInterface(), table.WithoutTextureTable(), why));

    // The table a game is translated with has it only where both hold; the Mac's five textures go with it.
    auto tableFor = [&](std::unique_ptr<FakeBinary> binary, MacInterface const& mac, BridgeProblems& problems) {
        GameImage image;
        if (!image.OpenFile(binary->File)) return InterfaceTable{};
        return InterfaceFor(image, mac, problems);
    };
    BridgeProblems problems;
    auto with = tableFor(TextureTableGame(), TextureTableInterface(), problems);
    CHECK(with.Textures && problems.TextureTable.empty() && !problems.SunShadow.empty() && with.Slot(with.Textures->NewSet, with.Textures->NewBindings[4]));
    auto otherEngine = tableFor(TextureTableGame(0, -1, 4, -1), TextureTableInterface(), problems);
    CHECK(!otherEngine.Textures && problems.TextureTable.find("a cookie's layer") != std::string::npos && !otherEngine.Slot(15, 4)
        && otherEngine.Slots.size() + 5 == table.Slots.size());
    auto otherFunctions = tableFor(TextureTableGame(), TextureTableInterface(3), problems);
    CHECK(!otherFunctions.Textures && problems.TextureTable.find("4 arrays") != std::string::npos);
}

TEST(provision_hands_a_material_of_its_own_shaders_to_the_translator)
{
    auto vanilla = Vanilla();
    std::vector<MaterialSource> mods;
    mods.push_back(Memory("A.pak", {
        { MBASE "X.lsf", ModMaterial() }, { MBASE "X_ST_FOR_Vulkan.bshd", Text("AAA") },                    // a twin: not the translator's
        { MBASE "Own.lsf", ModMaterial() }, { MBASE "Own_ST_FOR_Vulkan.bshd", Text("its own") }, { MBASE "Own_ST_DEF_Vulkan.bshd", Text("too") },
        { MBASE "Bad.lsf", ModMaterial() }, { MBASE "Bad_ST_FOR_Vulkan.bshd", Text("cannot") },
    }));
    std::vector<std::string> asked;
    ProvisionTranslator translator = [&](MaterialSource const& mod, std::string const& material, std::string& why) -> std::optional<TranslatedFiles> {
        asked.push_back(mod.Label + ":" + material);
        if (material.find("Bad") != std::string::npos) { why = "ST_FOR: nothing is known to be at set 9, binding 9"; return {}; }
        TranslatedFiles files;
        files.Lsf = Text("MAC-LSF");
        files.Stubs = { { "ST_FOR", Text("STUB-OWN-FOR") }, { "ST_DEF", Text("STUB-OWN-DEF") } };
        files.Note = "2 passes from Vulkan to Metal";
        return files;
    };

    auto map = BuildProvisionMap(vanilla, mods, FakeIdentity(), translator);
    CHECK(asked == (std::vector<std::string>{ "A.pak:" MBASE "Bad.lsf", "A.pak:" MBASE "Own.lsf" }));
    auto own = DecisionFor(*map, MBASE "Own.lsf"), bad = DecisionFor(*map, MBASE "Bad.lsf"), twin = DecisionFor(*map, MBASE "X.lsf");
    CHECK(twin && twin->Kind == ProvisionKind::Twin);
    CHECK(own && own->Kind == ProvisionKind::Translated && own->Stubs == 2 && own->Reason == "2 passes from Vulkan to Metal");
    CHECK(bad && bad->Kind == ProvisionKind::Refused && bad->Reason.find("not translated: ST_FOR: nothing is known") != std::string::npos);
    CHECK(map->Count(ProvisionKind::Translated) == 1 && map->Count(ProvisionKind::Refused) == 1);

    // Its files are served like a twin's: the .lsf first, then the stubs.
    ShaderProvisioner provisioner(std::move(map));
    CHECK(provisioner.OnFileOpened("/root/" MBASE "Own_ST_FOR_Metal.bshd").empty());
    auto lsf = provisioner.OnFileOpened("/root/" MBASE "Own.lsf");
    CHECK(Bytes(lsf.begin(), lsf.end()) == Text("MAC-LSF"));
    auto forward = provisioner.OnFileOpened("/root/" MBASE "Own_ST_FOR_Metal.bshd"), deferred = provisioner.OnFileOpened("/root/" MBASE "Own_ST_DEF_Metal.bshd");
    CHECK(Bytes(forward.begin(), forward.end()) == Text("STUB-OWN-FOR") && Bytes(deferred.begin(), deferred.end()) == Text("STUB-OWN-DEF"));
    CHECK(provisioner.OnFileOpened("/root/" MBASE "Bad.lsf").empty());
    auto p = provisioner.Progress();
    CHECK(p.Materials == 2 && p.MaterialsServed == 1 && p.Refused == 1 && p.RefusedAsked == 1 && p.Withheld == 1);

    // Without a translator it is refused, as before.
    auto plain = BuildProvisionMap(vanilla, mods, FakeIdentity());
    auto refused = DecisionFor(*plain, MBASE "Own.lsf");
    CHECK(refused && refused->Kind == ProvisionKind::Refused && refused->Reason.find("no Metal function") != std::string::npos);
}

// --- the install ----------------------------------------------------------------

void WriteBytes(std::string const& path, Bytes const& data)
{
    FILE* f = fopen(path.c_str(), "wb");
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

TEST(watcher_replans_when_the_player_changes_mods)
{
    std::string root = gTempDir + "/watched";
    mkdir(root.c_str(), 0755);
    mkdir((root + "/Mods").c_str(), 0755);
    InstallPaths paths;
    paths.MaterialsPak = root + "/Materials.pak";
    paths.ModsDir = root + "/Mods";
    paths.ModSettings = root + "/modsettings.lsx";

    WriteBytes(paths.MaterialsPak, BuildPak({ { VBASE "V.lsf", MacMaterial(), 2 }, { VBASE "V_ST_FOR_Metal.bshd", Text("STUB-FOR") } }));
    WriteBytes(paths.ModsDir + "/A.pak", BuildPak({
        { "Mods/A/meta.lsx", Text("<x/>") }, { MBASE "X.lsf", ModMaterial(), 2 }, { MBASE "X_ST_FOR_Vulkan.bshd", Text("AAA") } }));
    WriteBytes(paths.ModsDir + "/Companion.pak", BuildPak({
        { "Mods/Companion/meta.lsx", Text("<x/>") }, { MBASE "X_ST_FOR_Metal.bshd", Text("MINE") } }));
    auto module = [](char const* folder) { return std::string("<attribute id=\"Folder\" type=\"LSString\" value=\"") + folder + "\"/>"; };
    WriteBytes(paths.ModSettings, Text(module("A") + module("Companion")));

    ShaderProvisioner provisioner;
    InstallWatcher watcher(paths, provisioner, FakeIdentity());
    watcher.PlanNow();
    CHECK(provisioner.OnFileOpened("/root/" MBASE "X.lsf").empty());       // the companion ships Metal stubs: skipped
    CHECK(!watcher.Poll());                                                // nothing changed

    // The player disables the companion in the in-game mod manager. The game
    // still mounts its package, so its stubs are still what the game finds:
    // planned again, and skipped as before.
    WriteBytes(paths.ModSettings, Text(module("A")));
    CHECK(InstallStamp::Read(paths) != InstallStamp{});
    CHECK(!watcher.Poll());                                                // seen, but it may still be changing
    CHECK(watcher.Poll());                                                 // settled: planned again
    CHECK(provisioner.OnFileOpened("/root/" MBASE "X.lsf").empty());
    CHECK(!watcher.Poll());

    // The companion's package is taken out of the folder: the material is served.
    auto before = InstallStamp::Read(paths);
    unlink((paths.ModsDir + "/Companion.pak").c_str());
    CHECK(!(InstallStamp::Read(paths) == before));
    CHECK(!watcher.Poll());
    CHECK(watcher.Poll());
    CHECK(!provisioner.OnFileOpened("/root/" MBASE "X.lsf").empty());
    CHECK(!watcher.Poll());

    // The mod's own package is removed from the folder.
    before = InstallStamp::Read(paths);
    unlink((paths.ModsDir + "/A.pak").c_str());
    CHECK(!(InstallStamp::Read(paths) == before));
    CHECK(!watcher.Poll());
    CHECK(watcher.Poll());
    CHECK(provisioner.OnFileOpened("/root/" MBASE "X.lsf").empty());
}

TEST(translation_is_on_unless_the_settings_switch_it_off)
{
    // No file, or a file without the key: on.
    CHECK(TranslationSettingOf(std::nullopt) == TranslationSetting::On);
    for (auto on : { "1", "true", "TRUE", "yes", "on", "On" }) CHECK(TranslationSettingOf(std::string(on)) == TranslationSetting::On);
    for (auto all : { "all", "ALL", "All" }) CHECK(TranslationSettingOf(std::string(all)) == TranslationSetting::All);
    for (auto off : { "0", "false", "no", "off", "", "always", "al", "alll", "2" }) CHECK(TranslationSettingOf(std::string(off)) == TranslationSetting::Off);
    // As bg3mse.conf gives it.
    CHECK(TranslationSettingOf(ConfLookup("script_extender = 1\n", kShaderTranslationSetting)) == TranslationSetting::On);
    CHECK(TranslationSettingOf(ConfLookup("shader_translation = 0\n", kShaderTranslationSetting)) == TranslationSetting::Off);
    CHECK(TranslationSettingOf(ConfLookup("Shader_Translation = All \n", kShaderTranslationSetting)) == TranslationSetting::All);
}

TEST(translation_is_set_up_when_a_plan_first_has_a_material_to_translate)
{
    std::string root = gTempDir + "/late";
    mkdir(root.c_str(), 0755);
    mkdir((root + "/Mods").c_str(), 0755);
    InstallPaths paths;
    paths.MaterialsPak = root + "/Materials.pak";
    paths.ModsDir = root + "/Mods";
    paths.ModSettings = root + "/modsettings.lsx";
    WriteBytes(paths.MaterialsPak, BuildPak({ { VBASE "V.lsf", MacMaterial(), 2 }, { VBASE "V_ST_FOR_Metal.bshd", Text("STUB-FOR") } }));
    // A mod whose shaders are the game's own: a twin, nothing to translate.
    WriteBytes(paths.ModsDir + "/A.pak", BuildPak({
        { "Mods/A/meta.lsx", Text("<x/>") }, { MBASE "X.lsf", ModMaterial(), 2 }, { MBASE "X_ST_FOR_Vulkan.bshd", Text("AAA") } }));
    auto module = [](char const* folder) { return std::string("<attribute id=\"Folder\" type=\"LSString\" value=\"") + folder + "\"/>"; };
    WriteBytes(paths.ModSettings, Text(module("A")));

    int setUps = 0;
    std::vector<std::string> asked;
    auto translator = TranslatorSetUpWhenNeeded([&](std::string&) -> ProvisionTranslator {
        setUps++;
        return [&](MaterialSource const&, std::string const& material, std::string&) -> std::optional<TranslatedFiles> {
            asked.push_back(material);
            TranslatedFiles files;
            files.Lsf = Text("MAC-LSF");
            files.Stubs = { { "ST_FOR", Text("STUB-OWN-FOR") } };
            files.Note = "1 pass from Vulkan to Metal";
            return files;
        };
    });

    ShaderProvisioner provisioner;
    InstallWatcher watcher(paths, provisioner, FakeIdentity(), translator);
    watcher.PlanNow();
    CHECK(!provisioner.OnFileOpened("/root/" MBASE "X.lsf").empty());      // the twin is served
    CHECK(setUps == 0 && asked.empty());                                   // and nothing of translation was set up
    CHECK(!watcher.Poll() && setUps == 0);

    // The player adds a mod that brings shaders of its own while the game runs: set up at the plan that has it.
    WriteBytes(paths.ModsDir + "/B.pak", BuildPak({
        { "Mods/B/meta.lsx", Text("<x/>") }, { MBASE "Own.lsf", ModMaterial(), 2 }, { MBASE "Own_ST_FOR_Vulkan.bshd", Text("its own") } }));
    WriteBytes(paths.ModSettings, Text(module("A") + module("B")));
    CHECK(!watcher.Poll() && setUps == 0);                                 // seen, but it may still be changing
    CHECK(watcher.Poll());
    CHECK(setUps == 1 && asked == std::vector<std::string>{ MBASE "Own.lsf" });
    auto lsf = provisioner.OnFileOpened("/root/" MBASE "Own.lsf");
    CHECK(Bytes(lsf.begin(), lsf.end()) == Text("MAC-LSF"));

    // Once: a later plan translates with what was set up.
    unlink((paths.ModsDir + "/A.pak").c_str());
    WriteBytes(paths.ModSettings, Text(module("B")));
    CHECK(!watcher.Poll());
    CHECK(watcher.Poll());
    CHECK(setUps == 1 && asked.size() == 2);

    // Where translation does not stand, that is found out once, and is every material's reason.
    std::vector<MaterialSource> mods;
    mods.push_back(Memory("C.pak", {
        { MBASE "Own.lsf", ModMaterial() }, { MBASE "Own_ST_FOR_Vulkan.bshd", Text("its own") },
        { MBASE "Other.lsf", ModMaterial() }, { MBASE "Other_ST_FOR_Vulkan.bshd", Text("another") },
    }));
    int refusals = 0;
    auto none = TranslatorSetUpWhenNeeded([&](std::string& why) -> ProvisionTranslator {
        refusals++;
        why = "the game's symbol table cannot be read";
        return {};
    });
    for (int plan = 0; plan < 2; plan++) {
        auto map = BuildProvisionMap(Vanilla(), mods, FakeIdentity(), none);
        auto own = DecisionFor(*map, MBASE "Own.lsf"), other = DecisionFor(*map, MBASE "Other.lsf");
        CHECK(own && own->Kind == ProvisionKind::Refused && own->Reason.find("not translated: the game's symbol table cannot be read") != std::string::npos);
        CHECK(other && other->Kind == ProvisionKind::Refused && other->Reason.find("the game's symbol table cannot be read") != std::string::npos);
    }
    CHECK(refusals == 1);
}

TEST(the_games_functions_are_wanted_only_by_an_install_with_something_to_translate)
{
    std::string root = gTempDir + "/wanted";
    mkdir(root.c_str(), 0755);
    mkdir((root + "/Mods").c_str(), 0755);
    InstallPaths paths;
    paths.MaterialsPak = root + "/Materials.pak";
    paths.ModsDir = root + "/Mods";
    paths.ModSettings = root + "/modsettings.lsx";
    auto module = [](char const* folder) { return std::string("<attribute id=\"Folder\" type=\"LSString\" value=\"") + folder + "\"/>"; };
    std::string why;

    // No game to plan for: nothing.
    CHECK(!TranslationHasWork(paths, TranslationSetting::On, why, FakeIdentity()) && !why.empty());

    // No mods at all, and a mod without materials.
    WriteBytes(paths.MaterialsPak, BuildPak({ { VBASE "V.lsf", MacMaterial(), 2 }, { VBASE "V_ST_FOR_Metal.bshd", Text("STUB-FOR") } }));
    why.clear();
    CHECK(!TranslationHasWork(paths, TranslationSetting::On, why, FakeIdentity()) && why.find("no mod") != std::string::npos);
    CHECK(!TranslationHasWork(paths, TranslationSetting::All, why, FakeIdentity()));
    WriteBytes(paths.ModsDir + "/Plain.pak", BuildPak({ { "Mods/Plain/meta.lsx", Text("<x/>") } }));
    WriteBytes(paths.ModSettings, Text(module("Plain")));
    CHECK(!TranslationHasWork(paths, TranslationSetting::On, why, FakeIdentity()) && !TranslationHasWork(paths, TranslationSetting::All, why, FakeIdentity()));

    // A mod whose shaders are the game's own: nothing to translate, unless everything is ("all").
    WriteBytes(paths.ModsDir + "/A.pak", BuildPak({
        { "Mods/A/meta.lsx", Text("<x/>") }, { MBASE "X.lsf", ModMaterial(), 2 }, { MBASE "X_ST_FOR_Vulkan.bshd", Text("AAA") } }));
    WriteBytes(paths.ModSettings, Text(module("Plain") + module("A")));
    why.clear();
    CHECK(!TranslationHasWork(paths, TranslationSetting::On, why, FakeIdentity()) && why.find("no mod") != std::string::npos);
    CHECK(TranslationHasWork(paths, TranslationSetting::All, why, FakeIdentity()));

    // A mod with shaders of its own, in the load order or only in the folder: wanted, unless switched off.
    WriteBytes(paths.ModsDir + "/B.pak", BuildPak({
        { "Mods/B/meta.lsx", Text("<x/>") }, { MBASE "Own.lsf", ModMaterial(), 2 }, { MBASE "Own_ST_FOR_Vulkan.bshd", Text("its own") } }));
    CHECK(TranslationHasWork(paths, TranslationSetting::On, why, FakeIdentity()) && TranslationHasWork(paths, TranslationSetting::All, why, FakeIdentity()));
    why.clear();
    CHECK(!TranslationHasWork(paths, TranslationSetting::Off, why, FakeIdentity()) && why.find("is off") != std::string::npos);
}

TEST(install_plan_orders_packages_by_load_order)
{
    std::string root = gTempDir + "/install";
    std::string mods = root + "/Mods";
    mkdir(root.c_str(), 0755);
    mkdir(mods.c_str(), 0755);

    InstallPaths paths;
    paths.MaterialsPak = root + "/Materials.pak";
    paths.ModsDir = mods;
    paths.ModSettings = root + "/modsettings.lsx";

    auto write = [&](std::string const& path, Bytes const& data) {
        FILE* f = fopen(path.c_str(), "wb");
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    };
    write(paths.MaterialsPak, BuildPak({ { VBASE "V.lsf", MacMaterial(), 2 }, { VBASE "V_ST_FOR_Metal.bshd", Text("STUB-FOR") } }));

    // 40 packages, each with one refused material, named so that directory order,
    // name order and load order all differ. More than 32, so that an array of
    // packages that grows while pointers into it are held would fail here.
    std::string xml;
    for (int i = 39; i >= 0; i--) {
        xml += "<attribute id=\"Folder\" type=\"LSString\" value=\"Mod" + std::to_string(i) + "\"/>";
    }
    for (int i = 0; i < 40; i++) {
        std::string folder = "Mod" + std::to_string(i);
        std::string material = "Public/" + folder + "/Assets/Materials/M.lsf";
        write(mods + "/" + (char)('a' + i % 7) + "_" + folder + ".pak", BuildPak({
            { "Mods/" + folder + "/meta.lsx", Text("<x/>") },
            { material, ModMaterial(), 2 },
            { "Public/" + folder + "/Assets/Materials/M_ST_FOR_Vulkan.bshd", Text("not vanilla") },
        }));
    }
    // A package whose module is not in the load order is mounted by the game all the same: it is planned, after
    // those of its priority that are. This one ships Mod5's material with its Metal shader.
    write(mods + "/Disabled.pak", BuildPak({
        { "Mods/NotListed/meta.lsx", Text("<x/>") },
        { "Public/Mod5/Assets/Materials/M.lsf", MacMaterial(), 2 },
        { "Public/Mod5/Assets/Materials/M_ST_FOR_Metal.bshd", Text("STUB-FOR") },
    }));
    write(mods + "/Broken.pak", Text("LSPK but not really"));
    write(mods + "/readme.txt", Text("not a package"));
    write(paths.ModSettings, Text(xml));

    auto plan = PlanFromInstall(paths);
    CHECK(plan.Map != nullptr);
    if (!plan.Map) return;
    CHECK(plan.Packages.size() == 41);
    CHECK(plan.NotInLoadOrder == std::vector<std::string>{ "Disabled.pak" });
    CHECK(plan.Unreadable == std::vector<std::string>{ "Broken.pak" });
    CHECK(plan.Packages.front().ends_with("_Mod39.pak") && plan.Packages[39].ends_with("_Mod0.pak") && plan.Packages.back() == "Disabled.pak");
    // The two packages that ship Mod5's material have one priority: said, and the one outside the load order taken.
    CHECK(plan.Ties.size() == 1 && plan.Ties[0].find("Disabled.pak (outside the load order) and ") == 0
        && plan.Ties[0].find("_Mod5.pak both ship 1 materials") != std::string::npos);
    // The same is said of two that are both in the load order.
    write(mods + "/z_Again.pak", BuildPak({
        { "Mods/Mod7/meta.lsx", Text("<x/>") },
        { "Public/Mod9/Assets/Materials/M.lsf", ModMaterial(), 2 },
    }));
    auto again = PlanFromInstall(paths);
    CHECK(again.Ties.size() == 2);
    bool said = false;
    for (auto const& tie : again.Ties) said = said || (tie.find("_Mod9.pak") != std::string::npos && tie.find("z_Again.pak") != std::string::npos
        && tie.find("outside the load order") == std::string::npos);
    CHECK(said);
    unlink((mods + "/z_Again.pak").c_str());

    uint32_t refused = 0, other = 0;
    for (auto const& d : plan.Map->Decisions) {
        auto folder = d.Material.substr(7, d.Material.find('/', 7) - 7);
        if (folder == "Mod5") {
            // Mod5's own is superseded by the package that ships the material with its Metal shader, which needs nothing.
            other++;
            CHECK(d.Kind != ProvisionKind::Refused && d.Kind != ProvisionKind::Twin && d.Kind != ProvisionKind::Translated);
            continue;
        }
        // Every decision names the package its material actually came from.
        CHECK(d.Package.ends_with("_" + folder + ".pak"));
        CHECK(d.Kind == ProvisionKind::Refused);
        refused++;
    }
    CHECK(refused == 39 && other >= 1);

    paths.MaterialsPak = root + "/missing.pak";
    auto none = PlanFromInstall(paths);
    CHECK(!none.Map && !none.Problem.empty());
}

// --- the file reader's constructor, shared by two hooks -----------------------

// What ran, in order.
std::vector<std::string> gOpened;

std::string NameOf(bg3se::Path const& path) { return std::string(path.Name.data(), path.Name.size()); }

}

// The game's allocator, which a Path's name comes from: in this test the C library's.
namespace bg3se {
void* GameAllocRaw(std::size_t size) { return malloc(size); }
void GameFree(void* ptr) noexcept { free(ptr); }
}

// A stand-in for the game's constructor, named so that its extent can be
// taken from this executable's symbol table.
extern "C" __attribute__((noinline)) bg3se::FileReader* TestFileReaderConstructor(bg3se::FileReader* self, bg3se::Path const& path,
    unsigned int type, unsigned int openFlags)
{
    gOpened.push_back("constructor " + NameOf(path));
    self->IsLoaded = false;
    self->DataPtr = self->ReadPtr = nullptr;
    self->FileSize = 0;
    self->Type = bg3se::FileMappingType::MetadataOnly;
    return type == 2 && openFlags == 0 ? self : nullptr;
}

namespace {

const uint8_t kServed[] = { 's', 'e', 'r', 'v', 'e', 'd' };

struct RecordingSubscriber : FileReaderSubscriber
{
    std::span<uint8_t const> OnFileOpened(std::string_view path) override
    {
        gOpened.push_back("subscriber " + std::string(path));
        return path == "Public/Served.lsf" ? std::span<uint8_t const>(kServed) : std::span<uint8_t const>();
    }
};

// Norbyte's side: what WrappableFunction::Wrap attaches (CallToTrampoline
// running ScriptExtender::OnFileReaderCreate, which may open another path).
bg3se::FileReader__CtorProc* gExtenderNext = nullptr;
bg3se::FileReader* ExtenderWrapper(bg3se::FileReader* self, bg3se::Path const& path, unsigned int type, unsigned int openFlags)
{
    gOpened.push_back("extender " + NameOf(path));
    if (NameOf(path) == "Public/Overridden.txt") {
        bg3se::Path other;
        other.Name = "Public/Served.lsf";
        return gExtenderNext(self, other, type, openFlags);
    }
    return gExtenderNext(self, path, type, openFlags);
}

TEST(file_reader_constructor_is_shared_by_the_platform_hook_and_the_extenders)
{
    using Names = std::vector<std::string>;
    auto image = GameImage::Current();
    uint64_t va = image ? image->FindVA("_TestFileReaderConstructor") : 0;
    uint64_t end = va ? image->NextSymbolVA(va) : 0;
    CHECK(va && end > va);
    if (!va || end <= va) return;

    // The game's calls of its constructor, through a pointer the compiler cannot see through.
    bg3se::FileReader__CtorProc* volatile game = &TestFileReaderConstructor;
    auto open = [&](bg3se::FileReader__CtorProc* through, char const* name, bg3se::FileReader& reader) {
        bg3se::Path path;
        path.Name = name;
        gOpened.clear();
        return through(&reader, path, 2, 0);
    };
    // A reader is storage of the caller's that the constructor fills, as in
    // the game; nothing here builds or destroys one.
    alignas(bg3se::FileReader) static unsigned char storage[sizeof(bg3se::FileReader)];
    auto& reader = *reinterpret_cast<bg3se::FileReader*>(storage);

    // No hook: the constructor is the thing to call and to hook.
    CHECK(FileReaderHookFunction() == nullptr);
    CHECK(open(game, "Public/A.txt", reader) == &reader && gOpened == Names{ "constructor Public/A.txt" });

    // The platform's hook, as libbg3mse.dylib installs it at start-up.
    static RecordingSubscriber subscriber;
    CHECK(InstallFileReaderHookOn(reinterpret_cast<void*>(&TestFileReaderConstructor), end - va, &subscriber));
    auto hookFunction = reinterpret_cast<bg3se::FileReader__CtorProc*>(FileReaderHookFunction());
    CHECK(hookFunction != nullptr && hookFunction != &TestFileReaderConstructor);
    if (!hookFunction) return;
    CHECK(!InstallFileReaderHookOn(reinterpret_cast<void*>(&TestFileReaderConstructor), end - va, &subscriber));
    CHECK(open(game, "Public/A.txt", reader) == &reader);
    CHECK((gOpened == Names{ "constructor Public/A.txt", "subscriber Public/A.txt" }) && !reader.IsLoaded);
    // A file the subscriber serves: the reader reads the served bytes.
    CHECK(open(game, "Public/Served.lsf", reader) == &reader);
    CHECK(reader.IsLoaded && reader.DataPtr == kServed && reader.FileSize == sizeof(kServed) && reader.Type == bg3se::FileMappingType::MemBuffer);

    // The game's constructor takes no second hook: this is why the extender's cannot go there.
    void* onGame = reinterpret_cast<void*>(&TestFileReaderConstructor);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttach(&onGame, reinterpret_cast<void*>(&ExtenderWrapper)) != NO_ERROR);
    CHECK(DetourTransactionAbort() == NO_ERROR);

    // The extender is given the hook's function as the constructor
    // (StaticSymbols::ls__FileReader__ctor), and its Wrap detours that.
    void* member = reinterpret_cast<void*>(hookFunction);
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourAttachEx(&member, reinterpret_cast<void*>(&ExtenderWrapper), reinterpret_cast<PDETOUR_TRAMPOLINE*>(&gExtenderNext), nullptr,
        nullptr) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(gExtenderNext != nullptr && FileReaderHookFunction() == hookFunction);

    // The game opens a file: the extender first, then the game's
    // constructor, then the subscriber.
    CHECK(open(game, "Public/A.txt", reader) == &reader);
    CHECK((gOpened == Names{ "extender Public/A.txt", "constructor Public/A.txt", "subscriber Public/A.txt" }));
    // The extender opens another file in its place: the constructor and the
    // subscriber both get the file that is really opened, and its served
    // bytes are what the reader reads.
    CHECK(open(game, "Public/Overridden.txt", reader) == &reader);
    CHECK((gOpened == Names{ "extender Public/Overridden.txt", "constructor Public/Served.lsf", "subscriber Public/Served.lsf" }));
    CHECK(reader.IsLoaded && reader.DataPtr == kServed);
    // The extender's own opens (MakeFileReader calls the member) take the same route.
    CHECK(open(hookFunction, "Public/B.txt", reader) == &reader);
    CHECK((gOpened == Names{ "extender Public/B.txt", "constructor Public/B.txt", "subscriber Public/B.txt" }));
    // What the constructor returns comes back through both.
    {
        bg3se::Path path;
        path.Name = "Public/C.txt";
        CHECK(game(&reader, path, 1, 0) == nullptr);
    }

    // The extender's hook comes out on its own (EngineHooks::UnhookAll): the platform's stays.
    CHECK(DetourTransactionBegin() == NO_ERROR);
    CHECK(DetourDetach(reinterpret_cast<void**>(&gExtenderNext), reinterpret_cast<void*>(&ExtenderWrapper)) == NO_ERROR);
    CHECK(DetourTransactionCommit() == NO_ERROR);
    CHECK(open(game, "Public/A.txt", reader) == &reader);
    CHECK((gOpened == Names{ "constructor Public/A.txt", "subscriber Public/A.txt" }));
}

// What a console sink was given, for the test below.
struct SinkCall
{
    int Type;
    std::string Text;
    bool Local;
};
std::vector<SinkCall> gSinkCalls;
bool gSinkTakes = true;

TEST(debug_messages_are_log_lines_once_with_or_without_a_console)
{
    // What the logging macros name.
    using bg3se::Debug;
    using bg3se::DebugMessageType;
    std::string path = gTempDir + "/debug.log";
    FILE* log = fopen(path.c_str(), "w");
    CHECK(log != nullptr);
    if (!log) return;
    SetLogStream(log);

    // No console object: the entry points write the line themselves, whole.
    std::string longText(3000, 'x');
    INFO("no console %d", 1);
    ERR("%s", longText.c_str());
    bg3se::DebugLocal(bg3se::DebugMessageType::Warning, "local %s", "one");

    // A console object that takes messages: it is given the formatted text,
    // cut as CoreLib cuts it, and the entry points write nothing themselves;
    // the console writes the line (Console::LocalPrint, LogConsoleMessage).
    SetDebugConsoleSink([](int type, char const* text, bool local) {
        gSinkCalls.push_back({ type, text, local });
        if (gSinkTakes) LogConsoleMessage(type, text);
        return gSinkTakes;
    });
    WARN("to the console %d", 2);
    ERR("%s", longText.c_str());
    bg3se::DebugLocal(bg3se::DebugMessageType::Osiris, "local %s", "two");
    // The sink is set and the console object does not exist yet: the line is not lost.
    gSinkTakes = false;
    INFO("before the console %d", 3);
    gSinkTakes = true;
    // A debug message is dropped by the log unless debug lines are on, whichever way it came.
    DEBUG("dropped");
    SetDebugConsoleSink(nullptr);
    DEBUG("dropped too");
    INFO("no console again");

    SetLogStream(nullptr);
    fclose(log);

    CHECK(gSinkCalls.size() == 5);
    if (gSinkCalls.size() == 5) {
        CHECK(gSinkCalls[0].Type == (int)bg3se::DebugMessageType::Warning && gSinkCalls[0].Text == "to the console 2" && !gSinkCalls[0].Local);
        CHECK(gSinkCalls[1].Type == (int)bg3se::DebugMessageType::Error && gSinkCalls[1].Text == std::string(1023, 'x'));
        CHECK(gSinkCalls[2].Type == (int)bg3se::DebugMessageType::Osiris && gSinkCalls[2].Text == "local two" && gSinkCalls[2].Local);
        CHECK(gSinkCalls[3].Text == "before the console 3");
        CHECK(gSinkCalls[4].Type == (int)bg3se::DebugMessageType::Debug && gSinkCalls[4].Text == "dropped");
    }

    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        for (std::string line; std::getline(f, line);) lines.push_back(line);
    }
    // "[HH:MM:SS.mmm] [LEVEL] text": the text starts at column 23.
    auto is = [&](size_t i, char const* level, std::string const& text) {
        return i < lines.size() && lines[i].size() >= 23 && lines[i].compare(14, 9, std::string(" [") + level + "] ") == 0
            && lines[i].substr(23) == text;
    };
    CHECK(lines.size() == 8);
    CHECK(is(0, "INFO ", "no console 1"));
    CHECK(is(1, "ERROR", longText));
    CHECK(is(2, "WARN ", "local one"));
    CHECK(is(3, "WARN ", "to the console 2"));
    CHECK(is(4, "ERROR", std::string(1023, 'x')));
    CHECK(is(5, "OSI  ", "local two"));
    CHECK(is(6, "INFO ", "before the console 3"));
    CHECK(is(7, "INFO ", "no console again"));
}

}

int main()
{
    SetLogStream(nullptr);
    char const* tmp = getenv("TMPDIR");
    std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/bg3mse_tests.XXXXXX";
    if (!mkdtemp(dir.data())) return 2;
    gTempDir = dir;

    void (*tests[])() = {
        pak_reads_stored_and_compressed_entries_run,
        pak_refuses_a_file_count_that_overflows_run,
        pak_refuses_truncated_and_foreign_files_run,
        pak_refuses_an_entry_outside_the_file_run,
        load_order_keeps_every_module_in_order_run,
        code_shape_ignores_position_and_sees_changes_run,
        code_helpers_decode_calls_and_page_addresses_run,
        conf_reads_keys_comments_and_repeats_run,
        conf_flags_accept_only_the_on_values_run,
        instruction_fields_decode_run,
        image_search_finds_patterns_slots_and_imports_run,
        image_lookup_of_one_name_is_that_of_a_pass_run,
        spirv_hash_is_of_what_the_shader_does_run,
        container_hash_takes_a_recompiled_shader_for_the_same_run,
        input_signature_names_the_vertex_inputs_in_location_order_run,
        material_slots_are_what_the_material_declares_for_a_pass_run,
        a_directx_stage_says_at_which_registers_it_has_textures_and_samplers_run,
        what_the_lsf_gives_no_vulkan_place_is_where_the_directx_stage_has_it_run,
        mac_interface_is_what_every_function_agrees_on_run,
        a_windows_shader_is_placed_translated_and_draws_what_the_mac_function_draws_run,
        a_member_the_mac_block_lacks_is_worked_out_from_what_it_has_run,
        an_array_element_may_end_in_members_the_shader_does_not_read_run,
        the_table_of_textures_is_taken_from_the_mac_games_arrays_run,
        a_kept_pipeline_is_not_taken_for_another_set_of_functions_run,
        a_depth_texture_has_content_and_a_sampler_compares_against_it_run,
        a_shader_with_anything_uncertain_is_not_placed_run,
        a_stage_holds_one_module_or_two_in_either_chunk_format_run,
        a_matrix_the_shader_builds_is_transposed_where_the_mac_game_has_it_so_run,
        a_material_is_translated_into_stubs_sources_and_a_mac_ready_lsf_run,
        our_functions_answer_when_a_library_is_asked_for_them_run,
        mac_interface_is_kept_between_runs_for_the_game_it_was_read_from_run,
        the_sun_shadow_bridge_is_for_the_function_it_was_read_from_run,
        the_interface_table_works_members_out_only_in_a_game_that_bears_them_out_run,
        the_table_of_textures_is_for_the_engine_it_was_read_from_run,
        provision_hands_a_material_of_its_own_shaders_to_the_translator_run,
        provision_decides_twins_and_serves_their_files_run,
        provision_skips_a_material_another_package_has_metal_stubs_for_run,
        provision_lets_the_later_package_win_a_material_path_run,
        provision_names_what_it_does_not_handle_run,
        provisioner_serves_a_stub_only_after_its_material_run,
        provisioner_switches_to_a_new_plan_run,
        provisioner_tells_requests_from_served_files_across_a_replan_run,
        watcher_replans_when_the_player_changes_mods_run,
        translation_is_on_unless_the_settings_switch_it_off_run,
        translation_is_set_up_when_a_plan_first_has_a_material_to_translate_run,
        the_games_functions_are_wanted_only_by_an_install_with_something_to_translate_run,
        install_plan_orders_packages_by_load_order_run,
        file_reader_constructor_is_shared_by_the_platform_hook_and_the_extenders_run,
        debug_messages_are_log_lines_once_with_or_without_a_console_run,
    };
    for (auto test : tests) test();

    std::string cleanup = "rm -rf '" + gTempDir + "'";
    system(cleanup.c_str());
    printf("%zu tests, %d failures\n", std::size(tests), gFailures);
    return gFailures ? 1 : 0;
}
