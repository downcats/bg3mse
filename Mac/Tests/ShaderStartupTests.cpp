#include <stdafx.h>
#include <Platform/Log.h>
#include <Shaders/GameInterface.h>
#include <Shaders/ShaderProvision.h>
#include <Shaders/TranslatedFunctions.h>
#include <Shaders/TranslationCache.h>

#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

// What keeps mod shader translation from making a player wait at every start:
// translated materials kept between runs (TranslationCache.h), their
// functions compiled when they are asked for (TranslatedFunctions.h), and
// the game's Metal interface kept in a file that is taken only when it is
// this game's (GameInterface.h).
//
//   bg3mse_shader_startup_tests                               the tests
//   bg3mse_shader_startup_tests game <Game.app> <bg3mse-install>
//                                                             bg3mse-install shader-interface on the real game:
//                                                             what it leaves is the game's interface

using namespace bg3se::mac;

extern char** environ;

namespace {

int gFailures = 0;
char const* gTest = "";
std::string gTempDir;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s: %s (line %d)\n", gTest, #cond, __LINE__); gFailures++; } } while (0)
#define TEST(name) static void name(); static void name##_run() { gTest = #name; name(); } static void name()

using Bytes = std::vector<uint8_t>;
using Files = std::map<std::string, Bytes>;

Bytes Text(std::string_view s) { return Bytes(s.begin(), s.end()); }

MaterialSource Memory(std::string label, Files files)
{
    MaterialSource source;
    source.Label = std::move(label);
    for (auto const& [name, data] : files) source.Names.insert(name);
    source.Read = [files = std::move(files)](std::string const& name) -> std::optional<Bytes> {
        auto it = files.find(name);
        if (it == files.end()) return {};
        return it->second;
    };
    return source;
}

std::optional<Bytes> ReadFile(std::string const& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    Bytes bytes;
    uint8_t chunk[65536];
    for (size_t got; (got = fread(chunk, 1, sizeof(chunk), f)) > 0;) bytes.insert(bytes.end(), chunk, chunk + got);
    fclose(f);
    return bytes;
}

bool WriteFile(std::string const& path, std::span<uint8_t const> bytes)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return fclose(f) == 0 && ok;
}

bool Exists(std::string const& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

std::vector<std::string> Listing(std::string const& directory)
{
    std::vector<std::string> names;
    if (DIR* dir = opendir(directory.c_str())) {
        while (dirent* e = readdir(dir)) {
            if (e->d_name[0] != '.') names.push_back(e->d_name);
        }
        closedir(dir);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string FreshDirectory(std::string const& name)
{
    std::string dir = gTempDir + "/" + name;
    std::string clear = "rm -rf '" + dir + "'";
    system(clear.c_str());
    mkdir(dir.c_str(), 0755);
    return dir;
}

// What the log said while `run` ran.
std::string Logged(std::function<void()> const& run)
{
    std::string path = gTempDir + "/log.txt";
    FILE* log = fopen(path.c_str(), "w+");
    SetLogStream(log);
    run();
    SetLogStream(nullptr);
    std::string text;
    if (log) {
        fflush(log);
        rewind(log);
        char line[1024];
        while (fgets(line, sizeof(line), log)) text += line;
        fclose(log);
    }
    return text;
}

// --- a translation that is watched ------------------------------------------------

#define MBASE "Public/A/Assets/Materials/Base/"
constexpr char kMaterial[] = MBASE "X.lsf";
constexpr char kVulkan[] = MBASE "X_ST_FOR_Vulkan.bshd";
constexpr char kDirectX[] = MBASE "X_ST_FOR_DX11.bshd";
// A file translation reads that is not under the material's name.
constexpr char kShared[] = "Public/A/Assets/Materials/Shared.bin";

Files ModFiles()
{
    return { { kMaterial, Text("the material") }, { kVulkan, Text("the vulkan container") }, { kDirectX, Text("the directx container") },
        { kShared, Text("shared") } };
}

// The names of the two functions made of a container, as translation names them: by what the container holds.
std::pair<std::string, std::string> FunctionNames(std::span<uint8_t const> container)
{
    auto hash = HashOf(container);
    std::string stem = "bg3mse_" + HexOf(std::span(hash).first(8));
    return { stem + "_vertexMain", stem + "_fragmentMain" };
}

std::string VertexSource(std::string const& name)
{
    return "#include <metal_stdlib>\nusing namespace metal;\nvertex float4 " + name + "(uint i [[vertex_id]]) { return float4(float(i)); }\n";
}

std::string FragmentSource(std::string const& name, unsigned value)
{
    return "#include <metal_stdlib>\nusing namespace metal;\nfragment float4 " + name + "() { return float4(" + std::to_string(value) + ".0); }\n";
}

// Stands for TranslateMaterial: reads the files a translation reads, and makes of them one pass whose
// fragment function returns the material's size. Counts how often it is asked.
struct WatchedTranslation
{
    std::shared_ptr<int> Calls = std::make_shared<int>(0);
    bool BrokenFragment{ false };

    MaterialTranslation Function() const
    {
        return [calls = Calls, broken = BrokenFragment](MaterialSource const& mod, std::string const& material, std::string& why)
                   -> std::optional<TranslatedMaterial> {
            ++*calls;
            std::string stem = material.substr(0, material.size() - 4);
            auto lsf = mod.Read(material);
            if (!lsf) { why = "cannot read " + material; return {}; }
            TranslatedMaterial out;
            for (auto pass : { "ST_FOR", "ST_DEF" }) {
                std::string passStem = stem + "_" + pass;
                if (!mod.Names.contains(passStem + "_Vulkan.bshd")) continue;
                auto vulkan = mod.Read(passStem + "_Vulkan.bshd");
                auto directx = mod.Read(passStem + "_DX11.bshd");
                auto shared = mod.Read(kShared);
                if (!vulkan || !directx) { why = "a container cannot be read"; return {}; }
                auto [vertex, fragment] = FunctionNames(*vulkan);
                TranslatedPass translated;
                translated.Pass = pass;
                translated.VertexName = vertex;
                translated.FragmentName = fragment;
                translated.VertexSource = VertexSource(vertex);
                translated.FragmentSource = broken ? "this is not Metal" : FragmentSource(fragment, (unsigned)(lsf->size() + directx->size() + (shared ? shared->size() : 0)));
                translated.Stub = BuildMetalStub(std::string("X_") + pass + "_Metal", vertex, fragment);
                out.Passes.push_back(std::move(translated));
            }
            if (out.Passes.empty()) { why = "it has no pass"; return {}; }
            out.Lsf = *lsf;
            out.Lsf.push_back('!');
            out.OwnVertexStages = 1;
            out.ByOrderOnly = { "SomeTexture" };
            return out;
        };
    }
};

std::shared_ptr<TranslationCache> Cache(std::string const& directory, std::string const& madeWith = "made with A",
    uint64_t limit = TranslationCache::kDefaultLimit)
{
    return std::make_shared<TranslationCache>(directory, madeWith, limit);
}

// The one entry of a cache's directory.
std::string OnlyEntry(std::string const& directory)
{
    auto names = Listing(directory);
    return names.size() == 1 ? directory + "/" + names[0] : std::string();
}

TEST(a_translation_is_kept_and_served_again_without_translating)
{
    std::string dir = FreshDirectory("kept");
    auto mod = Memory("A.pak", ModFiles());
    WatchedTranslation watched;
    std::string why;

    // Without a cache every plan translates.
    auto plain = TranslatorOf(watched.Function());
    auto first = plain(mod, kMaterial, why);
    CHECK(first && *watched.Calls == 1 && plain(mod, kMaterial, why) && *watched.Calls == 2);
    CHECK(first && first->Note == "1 passes from Vulkan to Metal, 2 functions compiled; its vertex stage is the mod's own (1 of 1 passes); "
        "taken by a pairing made by order only: SomeTexture");
    CHECK(Listing(dir).empty());

    // With one, the first translates and keeps, and the second neither translates nor compiles.
    auto cache = Cache(dir);
    auto cached = TranslatorOf(watched.Function(), cache);
    auto made = cached(mod, kMaterial, why);
    CHECK(made && *watched.Calls == 3 && cache->Count().Kept == 1 && cache->Count().Found == 0);
    CHECK(Listing(dir).size() == 1 && OnlyEntry(dir) == cache->PathOf("A.pak", kMaterial));
    auto again = cached(mod, kMaterial, why);
    CHECK(again && *watched.Calls == 3 && cache->Count().Found == 1);
    CHECK(made && again && again->Lsf == made->Lsf && again->Stubs == made->Stubs && again->Stubs.size() == 1 && again->Lsf == Text("the material!"));
    CHECK(again && again->Note == "1 passes from Vulkan to Metal, 2 functions as kept from an earlier run; its vertex stage is the mod's own (1 of 1 passes); "
        "taken by a pairing made by order only: SomeTexture");

    // Another run of the game: a cache of its own over the same directory.
    auto later = Cache(dir);
    auto afterRestart = TranslatorOf(watched.Function(), later)(mod, kMaterial, why);
    CHECK(afterRestart && *watched.Calls == 3 && later->Count().Found == 1 && later->Count().Kept == 0);
    CHECK(afterRestart && made && afterRestart->Lsf == made->Lsf && afterRestart->Stubs == made->Stubs);

    // A material that is refused is not kept, and is asked about again.
    WatchedTranslation none;
    auto refusing = TranslatorOf(none.Function(), cache);
    CHECK(!refusing(mod, MBASE "Missing.lsf", why) && !refusing(mod, MBASE "Missing.lsf", why) && *none.Calls == 2);
    CHECK(Listing(dir).size() == 1);

    // And one whose functions do not compile is refused as without a cache, and not kept.
    WatchedTranslation broken;
    broken.BrokenFragment = true;
    auto other = Memory("B.pak", ModFiles());
    CHECK(!TranslatorOf(broken.Function(), cache)(other, kMaterial, why) && why.find("refused") != std::string::npos);
    CHECK(Listing(dir).size() == 1);
}

TEST(a_kept_translation_is_only_for_what_it_was_made_from)
{
    std::string dir = FreshDirectory("made-from");
    WatchedTranslation watched;
    std::string why;
    auto cache = Cache(dir);
    auto translator = TranslatorOf(watched.Function(), cache);
    CHECK(translator(Memory("A.pak", ModFiles()), kMaterial, why) && *watched.Calls == 1);

    // Each thing it was made from, changed: translated afresh, the entry replaced, and that one used after.
    int calls = 1;
    uint32_t stale = 0;
    auto changed = [&](char const* what, MaterialSource const& mod, bool sameEntry = true) {
        auto first = translator(mod, kMaterial, why);
        bool afresh = first && *watched.Calls == calls + 1;
        if (sameEntry) stale++;
        bool counted = cache->Count().Stale == stale;
        auto second = translator(mod, kMaterial, why);
        bool kept = second && *watched.Calls == calls + 1 && first && second->Lsf == first->Lsf && second->Stubs == first->Stubs;
        calls = *watched.Calls;
        if (!afresh || !counted || !kept) printf("  %s: translated afresh %d, the old entry counted stale %d, kept after %d\n", what, afresh, counted, kept);
        return afresh && counted && kept;
    };
    auto files = ModFiles();
    files[kMaterial] = Text("the material, edited");
    CHECK(changed("the material's .lsf", Memory("A.pak", files)));
    files[kVulkan] = Text("another vulkan container");
    CHECK(changed("a Vulkan container", Memory("A.pak", files)));
    files[kDirectX] = Text("another directx container!");
    CHECK(changed("a DirectX container", Memory("A.pak", files)));
    // A file that is not under the material's name, which translation read all the same.
    files[kShared] = Text("shared, edited");
    CHECK(changed("a file read from elsewhere in the package", Memory("A.pak", files)));
    files.erase(kShared);
    CHECK(changed("a file that was read and is gone", Memory("A.pak", files)));
    // A pass more: no file that was read has changed, the package has more under the material's name.
    files[MBASE "X_ST_DEF_Vulkan.bshd"] = Text("a second pass");
    files[MBASE "X_ST_DEF_DX11.bshd"] = Text("its directx container");
    CHECK(changed("a pass that was added", Memory("A.pak", files)));
    auto twoPasses = translator(Memory("A.pak", files), kMaterial, why);
    CHECK(twoPasses && twoPasses->Stubs.size() == 2 && *watched.Calls == calls);

    // The same files in a package of another name are another package's: an entry of their own.
    CHECK(changed("the package", Memory("B.pak", files), false));
    CHECK(Listing(dir).size() == 2);
    CHECK(translator(Memory("A.pak", files), kMaterial, why) && *watched.Calls == calls);

    // Made with something else (another build, another system, another game or table): what is there is not
    // taken, and what is made now takes its place.
    auto otherBuild = Cache(dir, "made with B");
    auto otherTranslator = TranslatorOf(watched.Function(), otherBuild);
    CHECK(otherBuild->PathOf("A.pak", kMaterial) == cache->PathOf("A.pak", kMaterial));
    CHECK(otherTranslator(Memory("A.pak", files), kMaterial, why) && *watched.Calls == calls + 1);
    CHECK(otherBuild->Count().Found == 0 && otherBuild->Count().Stale == 1 && Listing(dir).size() == 2);
    CHECK(otherTranslator(Memory("A.pak", files), kMaterial, why) && *watched.Calls == calls + 1 && otherBuild->Count().Found == 1);
    // And the other way round: one entry for the material, of whoever made it last.
    uint32_t before = cache->Count().Stale;
    CHECK(translator(Memory("A.pak", files), kMaterial, why) && *watched.Calls == calls + 2 && cache->Count().Stale == before + 1);
    CHECK(translator(Memory("A.pak", files), kMaterial, why) && *watched.Calls == calls + 2 && Listing(dir).size() == 2);
    CHECK(!otherBuild->Find(Memory("A.pak", files), kMaterial) && !Exists(cache->PathOf("A.pak", kMaterial)));
}

TEST(an_entry_that_was_cut_or_changed_on_disk_is_removed_and_made_again)
{
    std::string dir = FreshDirectory("damaged");
    WatchedTranslation watched;
    std::string why;
    auto mod = Memory("A.pak", ModFiles());
    auto cache = Cache(dir);
    auto translator = TranslatorOf(watched.Function(), cache);
    auto made = translator(mod, kMaterial, why);
    std::string path = cache->PathOf("A.pak", kMaterial);
    auto whole = ReadFile(path);
    CHECK(made && whole && whole->size() > 200);
    if (!made || !whole) return;

    int calls = 1;
    uint32_t damaged = 0;
    auto ignored = [&](char const* what, Bytes const& bytes) {
        bool written = WriteFile(path, bytes);
        auto got = translator(mod, kMaterial, why);
        bool afresh = got && *watched.Calls == ++calls && got->Lsf == made->Lsf && got->Stubs == made->Stubs;
        bool counted = cache->Count().Damaged == ++damaged;
        // What is there now is a whole entry again.
        bool replaced = ReadFile(path) == whole && translator(mod, kMaterial, why).has_value() && *watched.Calls == calls;
        if (!written || !afresh || !counted || !replaced) printf("  %s: translated afresh %d, counted damaged %d, replaced %d\n", what, afresh, counted, replaced);
        return written && afresh && counted && replaced;
    };
    CHECK(ignored("cut short", Bytes(whole->begin(), whole->begin() + whole->size() / 2)));
    CHECK(ignored("without its last byte", Bytes(whole->begin(), whole->end() - 1)));
    CHECK(ignored("empty", {}));
    CHECK(ignored("not an entry", Text("something else entirely, in the place of an entry, and long enough to hold a hash at its end .......")));
    for (size_t at : { (size_t)3, whole->size() / 3, whole->size() / 2, whole->size() - 40, whole->size() - 1 }) {
        Bytes flipped = *whole;
        flipped[at] ^= 0x20;
        CHECK(ignored("one bit changed", flipped));
    }
    Bytes longer = *whole;
    longer.push_back(0);
    CHECK(ignored("with a byte after its end", longer));

    // A partial file a run left in the middle is no entry, and does not get in the way.
    CHECK(WriteFile(path + ".1.0.partial", Bytes(whole->begin(), whole->begin() + 50)));
    CHECK(translator(mod, kMaterial, why) && *watched.Calls == calls);
}

void SetUsed(std::string const& path, time_t when)
{
    timeval times[2] = { { when, 0 }, { when, 0 } };
    utimes(path.c_str(), times);
}

TEST(the_directory_is_kept_under_its_size_by_what_was_used_longest_ago)
{
    std::string dir = FreshDirectory("bounded");
    WatchedTranslation watched;
    std::string why;
    auto files = ModFiles();
    auto mod = [&](int n) { return Memory("Pack" + std::to_string(n) + ".pak", files); };

    // How large one entry is.
    size_t one = 0;
    {
        auto probe = Cache(dir);
        CHECK(TranslatorOf(watched.Function(), probe)(mod(0), kMaterial, why));
        auto entry = ReadFile(OnlyEntry(dir));
        CHECK(entry.has_value());
        one = entry ? entry->size() : 1;
    }
    // Room for three and a half; something that is not the cache's lies in the directory.
    auto cache = Cache(dir, "made with A", one * 7 / 2);
    auto translator = TranslatorOf(watched.Function(), cache);
    CHECK(WriteFile(dir + "/notes.txt", Text(std::string(one * 5, 'x'))));
    time_t now = time(nullptr);
    SetUsed(cache->PathOf("Pack0.pak", kMaterial), now - 1000);
    for (int n = 1; n <= 2; n++) {
        CHECK(translator(mod(n), kMaterial, why));
        SetUsed(cache->PathOf("Pack" + std::to_string(n) + ".pak", kMaterial), now - 1000 + n * 100);
    }
    CHECK(Listing(dir).size() == 4);       // three entries and the notes

    // The first is used again: the second is now the one used longest ago.
    int calls = *watched.Calls;
    CHECK(translator(mod(0), kMaterial, why) && *watched.Calls == calls);
    CHECK(translator(mod(3), kMaterial, why) && *watched.Calls == calls + 1);
    auto has = [&](int n) { return Exists(cache->PathOf("Pack" + std::to_string(n) + ".pak", kMaterial)); };
    CHECK(has(0) && !has(1) && has(2) && has(3) && Exists(dir + "/notes.txt"));

    // An old partial file is cleared away with the next entry kept; a young one may be someone's work.
    std::string old = dir + "/abc.1.0.partial", young = dir + "/def.2.0.partial";
    CHECK(WriteFile(old, Text("x")) && WriteFile(young, Text("y")));
    SetUsed(old, now - 7200);
    SetUsed(cache->PathOf("Pack3.pak", kMaterial), now - 500);
    SetUsed(cache->PathOf("Pack2.pak", kMaterial), now - 400);
    CHECK(translator(mod(4), kMaterial, why));
    CHECK(!Exists(old) && Exists(young) && has(0) && !has(3) && has(2) && has(4));

    // A bound nothing fits under keeps nothing, and serves all the same.
    std::string none = FreshDirectory("bounded-none");
    auto tiny = Cache(none, "made with A", 10);
    CHECK(TranslatorOf(watched.Function(), tiny)(mod(0), kMaterial, why) && Listing(none).empty());

    // A directory that cannot be made: nothing is found, nothing kept, the material is served.
    CHECK(WriteFile(gTempDir + "/a-file", Text("x")));
    auto nowhere = Cache(gTempDir + "/a-file/cache");
    calls = *watched.Calls;
    auto nowhereTranslator = TranslatorOf(watched.Function(), nowhere);
    CHECK(nowhereTranslator(mod(0), kMaterial, why) && nowhereTranslator(mod(0), kMaterial, why) && *watched.Calls == calls + 2);
}

// A library of a game's own, to be asked for functions as rf::GPUDevice::CreateShader asks.
std::unique_ptr<MetalFunctions> GameLibrary()
{
    std::string why;
    std::string sources[] = { "#include <metal_stdlib>\nusing namespace metal;\nvertex float4 game_vertex(uint i [[vertex_id]]) { return float4(float(i)); }\n" };
    return MetalFunctions::Compile(sources, why);
}

TEST(kept_sources_are_compiled_when_asked_for_and_in_the_background)
{
    std::string why;
    auto game = GameLibrary();
    if (!game) return;      // no Metal device here
    CHECK(InstallTranslatedFunctions(why));

    // Known by their names without being compiled; the game's question compiles the one it asks for.
    std::vector<TranslatedSource> sources;
    for (int i = 0; i < 6; i++) {
        std::string name = "bg3mse_startup_lazy" + std::to_string(i) + "_fragmentMain";
        sources.push_back({ name, FragmentSource(name, (unsigned)i) });
    }
    auto before = CountTranslatedFunctions();
    KeepTranslatedSources(sources, "Public/Test/Assets/Materials/Lazy.lsf");
    CHECK(game->Has(sources[5].Name) && game->Has(sources[0].Name));
    WaitForTranslatedSources();
    auto after = CountTranslatedFunctions();
    CHECK(after.Kept == before.Kept + 6 && after.Waiting == 0 && after.Answered == before.Answered + 2);
    for (auto const& source : sources) CHECK(game->Has(source.Name));
    CHECK(game->Has("game_vertex") && !game->Has("bg3mse_startup_never_kept"));

    // Kept again from the same source: there already, nothing waits. From another source: the function of
    // the one before is not this one's, and the new one is compiled when asked for.
    KeepTranslatedSources(sources);
    CHECK(CountTranslatedFunctions().Waiting == 0);
    // A name that is not text is no function's: nothing waits under it.
    std::vector<TranslatedSource> unnamed = { { "bg3mse_startup_\xff\xfe", FragmentSource("bg3mse_startup_unnamed", 1) } };
    KeepTranslatedSources(unnamed, "Public/Test/Assets/Materials/Unnamed.lsf");
    CHECK(CountTranslatedFunctions().Waiting == 0);
    std::vector<TranslatedSource> edited = { { sources[0].Name, FragmentSource(sources[0].Name, 99) } };
    KeepTranslatedSources(edited);
    WaitForTranslatedSources();
    CHECK(CountTranslatedFunctions().Kept == after.Kept && CountTranslatedFunctions().Waiting == 0 && game->Has(sources[0].Name));

    // Compiled at once, by name: a source without its function is refused, and so is one that does not compile.
    std::vector<TranslatedSource> named = { { "bg3mse_startup_named_fragmentMain", FragmentSource("bg3mse_startup_named_fragmentMain", 1) } };
    CHECK(AddTranslatedFunctions(named, why, "Public/Test/Assets/Materials/Named.lsf") && game->Has(named[0].Name));
    auto kept = CountTranslatedFunctions().Kept;
    CHECK(AddTranslatedFunctions(named, why) && CountTranslatedFunctions().Kept == kept);     // from the same source: as it is
    std::vector<TranslatedSource> misnamed = { { "bg3mse_startup_misnamed", FragmentSource("bg3mse_startup_other_name", 1) } };
    CHECK(!AddTranslatedFunctions(misnamed, why) && why.find("without the function it is named for") != std::string::npos);
    std::vector<TranslatedSource> broken = { { "bg3mse_startup_ok", FragmentSource("bg3mse_startup_ok", 1) }, { "bg3mse_startup_broken", "this is not Metal" } };
    CHECK(!AddTranslatedFunctions(broken, why) && why.find("refused") != std::string::npos && !game->Has("bg3mse_startup_ok"));
}

TEST(a_kept_source_that_does_not_compile_has_its_material_translated_afresh)
{
    auto game = GameLibrary();
    if (!game) return;
    std::string why, dir = FreshDirectory("uncompilable");
    CHECK(InstallTranslatedFunctions(why));
    auto files = ModFiles();
    files[kVulkan] = Text("a container of this test's own");
    auto mod = Memory("A.pak", files);
    auto [vertex, fragment] = FunctionNames(files[kVulkan]);

    // An entry whose fragment source does not compile, as nothing this code keeps is: put there by hand.
    auto cache = Cache(dir);
    WatchedTranslation watched;
    RecordedSource recorded(mod);
    auto translated = watched.Function()(recorded.Source(), kMaterial, why);
    CHECK(translated.has_value() && recorded.Inputs().has_value());
    if (!translated || !recorded.Inputs()) return;
    KeptTranslation kept;
    kept.Lsf = translated->Lsf;
    kept.Stubs = { { "ST_FOR", translated->Passes[0].Stub } };
    kept.Functions = { { vertex, VertexSource(vertex) }, { fragment, "this is not Metal" } };

    // A function's name that is no name is not what this code wrote: such an entry is removed when it is read.
    KeptTranslation misnamed = kept;
    misnamed.Functions[1].Name = "bg3mse_\xff\xfe";
    CHECK(cache->Keep(mod, kMaterial, *recorded.Inputs(), misnamed));
    uint32_t damaged = cache->Count().Damaged;
    CHECK(!cache->Find(mod, kMaterial) && cache->Count().Damaged == damaged + 1 && !Exists(cache->PathOf("A.pak", kMaterial)));

    CHECK(cache->Keep(mod, kMaterial, *recorded.Inputs(), kept) && Exists(cache->PathOf("A.pak", kMaterial)));

    // Served as kept. The function that does not compile stays for the game's request when the background
    // fails at it; the game's request tries again, gets nothing, and is not tried a third time. The log says
    // whose it is, and the entry is gone: the next plan translates the material afresh.
    auto translator = TranslatorOf(watched.Function(), cache);
    WaitForTranslatedSources();
    uint32_t waiting = CountTranslatedFunctions().Waiting;
    std::string logged = Logged([&] {
        CHECK(translator(mod, kMaterial, why) && *watched.Calls == 1);
        WaitForTranslatedSources();
        CHECK(CountTranslatedFunctions().Waiting == waiting + 1 && !Exists(cache->PathOf("A.pak", kMaterial)));
        CHECK(game->Has(vertex) && !game->Has(fragment) && !game->Has(fragment));
        CHECK(CountTranslatedFunctions().Waiting == waiting);
    });
    CHECK(logged.find(fragment + " of " + kMaterial + " was kept from an earlier run and did not compile in the background") != std::string::npos);
    CHECK(logged.find(fragment + " of " + kMaterial + " was kept from an earlier run and does not compile now") != std::string::npos);
    CHECK(logged.find("does not compile now", logged.find("does not compile now") + 1) == std::string::npos);     // said once
    CHECK(!Exists(cache->PathOf("A.pak", kMaterial)));
    CHECK(translator(mod, kMaterial, why) && *watched.Calls == 2 && game->Has(fragment));
}

MacResource Block(std::string name, uint32_t index, std::vector<BlockMember> members)
{
    MacResource r;
    r.Name = std::move(name);
    r.Kind = ResourceKind::Buffer;
    r.Index = index;
    r.Type = "block";
    r.Members = std::move(members);
    r.Size = 64;
    r.Used = true;
    return r;
}

MacInterface SomeInterface(uint32_t index = 11)
{
    MacPass pass;
    pass.Attributes = { { "Position", 0, "f3" } };
    pass.Vertex = { Block("PerCamera", index, { { "g_View", 0, "f16" } }) };
    MacInterface mac;
    mac.Add(pass);
    return mac;
}

TEST(what_a_translation_is_made_with_tells_builds_games_and_tables_apart)
{
    auto const& table = BuiltInInterface();
    std::string made = TranslationMadeWith(SomeInterface(), table);
    CHECK(!made.empty() && made == TranslationMadeWith(SomeInterface(), table));
    // This image's identifier, as the linker gave it: 32 hexadecimal digits.
    auto image = made.find("bg3mse image ");
    CHECK(image == 0 && made.find(';') == strlen("bg3mse image ") + 32);
    // Another game's functions; the table without what does not stand for a game.
    CHECK(TranslationMadeWith(SomeInterface(12), table) != made);
    bool derives = std::any_of(table.Blocks.begin(), table.Blocks.end(), [](auto const& b) { return !b.Derived.empty(); });
    CHECK(!derives || TranslationMadeWith(SomeInterface(), table.WithoutDerived()) != made);
    CHECK(!table.Textures || TranslationMadeWith(SomeInterface(), table.WithoutTextureTable()) != made);

    std::string dir = FreshDirectory("opened");
    auto cache = OpenTranslationCache(dir + "/deeper/shader-cache", SomeInterface(), table);
    CHECK(cache && cache->MadeWith() == made && Exists(dir + "/deeper/shader-cache"));
}

// --- the game's interface, kept and read by a helper -------------------------------

// A folder with the three files a kept interface is stamped by.
std::string FakeGame(std::string const& name)
{
    std::string app = FreshDirectory(name) + "/Game.app";
    for (auto dir : { "", "/Contents", "/Contents/Data", "/Contents/Data/Shaders", "/Contents/Data/Shaders/Metal" }) mkdir((app + dir).c_str(), 0755);
    for (auto file : { "/Contents/Data/Shaders/Metal/Core.metallib", "/Contents/Data/Shaders/Metal/Materials.metallib", "/Contents/Data/Materials.pak" }) {
        WriteFile(app + file, Text(file));
    }
    return app;
}

TEST(a_kept_interface_is_taken_only_as_this_build_writes_it_for_this_game)
{
    std::string app = FakeGame("kept-interface"), file = gTempDir + "/kept-interface/shader-interface.txt";
    std::string stamp = MacInterfaceStamp(app);
    auto mac = SomeInterface();
    CHECK(!stamp.empty() && SaveMacInterface(file, stamp, mac));
    auto kept = LoadMacInterface(file, stamp);
    CHECK(kept && kept->Serialize() == mac.Serialize());
    // Written under a name of its own and moved: nothing is left beside it.
    CHECK(Listing(gTempDir + "/kept-interface") == (std::vector<std::string>{ "Game.app", "shader-interface.txt" }));
    // What a writer that was stopped in the middle left there goes once it is old; a young one may be someone's work.
    std::string old = file + ".123.partial", young = file + ".456.partial";
    CHECK(WriteFile(old, Text("half")) && WriteFile(young, Text("half")));
    SetUsed(old, time(nullptr) - 7200);
    CHECK(SaveMacInterface(file, stamp, mac) && !Exists(old) && Exists(young));
    SetUsed(young, time(nullptr) - 7200);
    std::string unused;
    CHECK(EnsureMacInterface(app, file, unused).What == EnsuredMacInterface::How::Kept && !Exists(young));
    auto text = ReadFile(file);
    CHECK(text.has_value());
    if (!text) return;
    std::string whole(text->begin(), text->end());
    auto header = whole.substr(0, whole.find('\n'));

    // The game updated (a file of another size), an earlier build's file, a cut one, something else: not taken.
    CHECK(WriteFile(app + "/Contents/Data/Materials.pak", Text("the game after an update")));
    std::string updated = MacInterfaceStamp(app);
    CHECK(updated != stamp && !LoadMacInterface(file, updated) && LoadMacInterface(file, stamp));
    auto with = [&](std::string const& contents) {
        WriteFile(file, Text(contents));
        return LoadMacInterface(file, stamp).has_value();
    };
    CHECK(with(whole));
    CHECK(!with("bg3mse Mac interface 1" + whole.substr(header.size())));
    CHECK(!with(header + " and more" + whole.substr(header.size())));
    CHECK(!with(whole.substr(0, whole.size() - 2)) && !with(whole.substr(0, header.size() + 1)) && !with("") && !with("\n\n\n"));
    CHECK(!with(whole + "more\n"));

    // Made again over whatever is there.
    CHECK(SaveMacInterface(file, updated, mac) && LoadMacInterface(file, updated) && !LoadMacInterface(file, stamp));
    // No game there: nothing to stamp by, nothing kept, nothing read.
    std::string why;
    CHECK(MacInterfaceStamp(gTempDir + "/no-game").empty() && !SaveMacInterface(file, "", mac));
    auto none = EnsureMacInterface(gTempDir + "/no-game", file, why);
    CHECK(!none.Ok() && why.find("not in") != std::string::npos && !MacInterfaceToBeRead(gTempDir + "/no-game", file));
    // What is kept for the game as it is, is what the game's interface is: nothing is read.
    auto ensured = EnsureMacInterface(app, file, why);
    CHECK(ensured.What == EnsuredMacInterface::How::Kept && ensured.Interface && ensured.Interface->Serialize() == mac.Serialize());
    CHECK(!MacInterfaceToBeRead(app, file));

    // A file a later build of bg3mse wrote is that build's: left as it is, whatever game it is of, so that an
    // older tool and a newer library do not each write it over the other's at every start. An earlier build's,
    // a cut one or anything else is to be read again (which needs the game, and is not done here).
    unsigned long own = strtoul(header.c_str() + strlen("bg3mse Mac interface "), nullptr, 10);
    CHECK(own >= 2 && header == "bg3mse Mac interface " + std::to_string(own));
    for (std::string const& stampOf : { updated, std::string("another game's") }) {
        std::string newer = "bg3mse Mac interface " + std::to_string(own + 1) + "\n" + stampOf + "\nwritten some other way\n";
        CHECK(WriteFile(file, Text(newer)) && !LoadMacInterface(file, updated));
        auto left = EnsureMacInterface(app, file, why);
        CHECK(left.What == EnsuredMacInterface::How::OfNewerBuild && left.Ok() && !left.Interface && !MacInterfaceToBeRead(app, file));
        auto still = ReadFile(file);
        CHECK(still && std::string(still->begin(), still->end()) == newer);
    }
    for (std::string const& contents : { "bg3mse Mac interface " + std::to_string(own - 1) + "\n" + updated + "\nP\t1\nE\n",
             whole.substr(0, whole.size() - 2), std::string("bg3mse Mac interface 99999999\n"), std::string("bg3mse Mac interface 3x\n"),
             std::string("something else\n"), std::string() }) {
        CHECK(WriteFile(file, Text(contents)) && MacInterfaceToBeRead(app, file));
    }
    unlink(file.c_str());
    CHECK(MacInterfaceToBeRead(app, file));
}

// bg3mse-install shader-interface on the real game: a kept file of another game version is made again, what
// is left is the game's interface as this build reads it, and a second run reads nothing and leaves the file
// as it is.
int RunTool(std::string const& tool, std::string const& app, std::string const& file)
{
    std::string verb = "shader-interface", to = "--to";
    char* argv[] = { const_cast<char*>(tool.c_str()), verb.data(), const_cast<char*>(app.c_str()), to.data(), const_cast<char*>(file.c_str()), nullptr };
    pid_t pid = 0;
    if (posix_spawn(&pid, tool.c_str(), nullptr, nullptr, argv, environ) != 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int GameInterface(std::string const& app, std::string const& tool)
{
    gTest = "the_install_tool_reads_the_real_game";
    std::string dir = FreshDirectory("game-interface"), file = dir + "/shader-interface.txt";
    std::string stamp = MacInterfaceStamp(app);
    if (stamp.empty()) {
        printf("%s has no shader files: skipped\n", app.c_str());
        return 77;
    }
    CHECK(SaveMacInterface(file, "another version's stamp", SomeInterface()));
    CHECK(!LoadMacInterface(file, stamp));
    CHECK(RunTool(tool, app, file) == 0);
    auto mac = LoadMacInterface(file, stamp);
    CHECK(mac.has_value());
    if (!mac) return 1;
    CHECK(mac->Passes() > 1000 && mac->Resource("CB_EngineParams", ResourceKind::Buffer) && mac->Resource("CB_MaterialConstants", ResourceKind::Buffer));
    auto first = ReadFile(file);
    struct stat before{}, after{};
    CHECK(stat(file.c_str(), &before) == 0);
    CHECK(RunTool(tool, app, file) == 0 && stat(file.c_str(), &after) == 0 && before.st_ino == after.st_ino && ReadFile(file) == first);
    // Not a game: said, and nothing is left.
    CHECK(RunTool(tool, dir, dir + "/none.txt") == 1 && !Exists(dir + "/none.txt"));
    printf("%zu function pairs read by %s; %d failure(s)\n", mac->Passes(), tool.c_str(), gFailures);
    return gFailures ? 1 : 0;
}

}

int main(int argc, char** argv)
{
    SetLogStream(nullptr);
    char const* tmp = getenv("TMPDIR");
    std::string dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/bg3mse_shader_startup_tests.XXXXXX";
    if (!mkdtemp(dir.data())) return 2;
    gTempDir = dir;
    std::string cleanup = "rm -rf '" + gTempDir + "'";

    if (argc == 4 && std::string(argv[1]) == "game") {
        int result = GameInterface(argv[2], argv[3]);
        system(cleanup.c_str());
        return result;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: bg3mse_shader_startup_tests [game <Game.app> <bg3mse-install>]\n");
        return 2;
    }

    void (*tests[])() = {
        a_translation_is_kept_and_served_again_without_translating_run,
        a_kept_translation_is_only_for_what_it_was_made_from_run,
        an_entry_that_was_cut_or_changed_on_disk_is_removed_and_made_again_run,
        the_directory_is_kept_under_its_size_by_what_was_used_longest_ago_run,
        kept_sources_are_compiled_when_asked_for_and_in_the_background_run,
        a_kept_source_that_does_not_compile_has_its_material_translated_afresh_run,
        what_a_translation_is_made_with_tells_builds_games_and_tables_apart_run,
        a_kept_interface_is_taken_only_as_this_build_writes_it_for_this_game_run,
    };
    for (auto test : tests) test();

    system(cleanup.c_str());
    printf("%zu tests, %d failures\n", std::size(tests), gFailures);
    return gFailures ? 1 : 0;
}
