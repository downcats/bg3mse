#include <stdafx.h>
#include <Platform/EntryHook.h>
#include <Platform/FileReaderHook.h>
#include <Platform/Log.h>
#include <Shaders/GameInterface.h>
#include <Shaders/PakFile.h>
#include <map>
#include <set>
#include <Shaders/ShaderProvision.h>
#include <Shaders/SpirvIdentity.h>

extern "C" {
#include <Shaders/identity.h>
}

#if BG3MSE_GFX
#include <Gfx/GfxShaderCheck.h>
#include <Gfx/GfxSites.h>
#include <Platform/Islands.h>
#endif

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// bg3mse-shaders: runs the game's shader provisioning without the game.
//
//   bg3mse-shaders plan <Game.app>                      what the game would decide for this install
//   bg3mse-shaders plan <Materials.pak> <mod.pak>...    the same, over packages in the order given
//   bg3mse-shaders check <Game.app>                     whether the hooks would install on this game binary
//   bg3mse-shaders hooks <Game.app>                     on how many of its functions an entry hook could go, and why not
//   bg3mse-shaders identity <Materials.pak> <build>     writes Mac/Shaders/identity_data.c for a Windows Materials.pak
//   bg3mse-shaders interface <Materials.pak> <Game.app> <build> [proven]
//                                                       writes Mac/Shaders/InterfaceData.cpp from a Windows Materials.pak and the Mac game
//   bg3mse-shaders translate <package.pak> <material.lsf> <Game.app> [out-dir]
//                                                       translates one material of a package for the Mac game and compiles it
//   bg3mse-shaders interface-check <Game.app>            whether the game still has what the interface table names
//   bg3mse-shaders inputs <package.pak> <container.bshd> [Game.app]
//                                                       what each stage of a Vulkan container takes (* marks a member it reads),
//                                                       and what the Mac game's functions of that name take
//   bg3mse-shaders texts <package.pak> <out-dir> [path-part]   what a package's resource files say, as text, for following
//   bg3mse-shaders reads <package.pak>                   which members of the engine's blocks each stage reads, by name
//                                                    what refers to a material (an effect, a visual, a spell)
//   bg3mse-shaders names <Materials.pak> <pass> <stage>   the names the pass's DirectX 12 stage (0 vertex, 4 fragment) carries
//   bg3mse-shaders verify <Materials.pak> <Game.app> [every-nth [path-part]] [--inferred] [--draw] [--reference PAK] [--list FILE] [--prove FILE] [--stages FILE]
//                                                       translates the Windows passes and compares them with the Mac game's
//
// plan takes --out DIR to write every file that would be served, --translate or
// --translate=all to plan as the game does with shader_translation on (as it is unless switched off) or set to all,
// and --cache DIR to keep translated materials in DIR between runs, as the game does.

using namespace bg3se::mac;

namespace {

// Every resource file of a package as lines of text under `outDir`: a text
// file as it is, a binary resource as one "node.attribute=value" line per
// attribute that holds text or an identifier. Nothing else is written.
// A binary resource as lines of text: one "node.attribute=value" line per
// attribute that holds text, an identifier or a whole number.
bool ResourceText(std::span<uint8_t const> data, std::string& out)
{
    LsfDoc doc;
    char why[128];
    if (!lsf_parse(data.data(), data.size(), &doc, why, sizeof(why))) return false;
    for (uint32_t n = 0; n < doc.node_count; n++) {
        auto const& node = doc.nodes[n];
        for (uint32_t a = 0; a < node.attr_count; a++) {
            auto const& attr = doc.attrs[node.attr_begin + a];
            std::string value;
            if (attr.type >= LSF_T_STRING && attr.type <= LSF_T_LSSTRING) {
                value.assign((char const*)attr.value, strnlen((char const*)attr.value, attr.size));
            } else if (attr.type == 31 && attr.size == 16) {
                // An identifier, written the way the game's text files write one.
                auto const* b = attr.value;
                char guid[40];
                snprintf(guid, sizeof(guid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[9], b[8], b[11], b[10], b[13], b[12], b[15], b[14]);
                value = guid;
            } else if (int64_t number = 0; lsf_attr_int(&attr, &number)) {
                value = std::to_string(number);
            } else if ((attr.type == LSF_T_FLOAT || (attr.type >= 11 && attr.type <= 13)) && attr.size % 4 == 0 && attr.size <= 16) {
                for (uint32_t at = 0; at < attr.size; at += 4) {
                    float f;
                    memcpy(&f, attr.value + at, 4);
                    char text[32];
                    snprintf(text, sizeof(text), "%s%g", at ? " " : "", f);
                    value += text;
                }
            } else {
                continue;
            }
            if (value.empty()) continue;
            out += std::to_string(n) + " " + (node.name ? node.name : "") + "." + (attr.name ? attr.name : "") + "=" + value
                + (a == 0 ? "   (in " + std::to_string(node.parent) + ")" : "") + "\n";
        }
        // A node with nothing to say for itself still says where it hangs.
        if (!node.attr_count) out += std::to_string(n) + " " + (node.name ? node.name : "") + " in " + std::to_string(node.parent) + "\n";
    }
    lsf_free(&doc);
    return true;
}

int TextsCommand(std::string const& package, std::string const& outDir, std::string const& part)
{
    if (package.ends_with(".lsf") || package.ends_with(".lsfx")) {
        // One resource file on its own: to the output.
        std::vector<uint8_t> data;
        std::string text;
        if (FILE* f = fopen(package.c_str(), "rb")) {
            uint8_t chunk[65536];
            for (size_t got; (got = fread(chunk, 1, sizeof(chunk), f)) > 0;) data.insert(data.end(), chunk, chunk + got);
            fclose(f);
        }
        if (data.empty() || !ResourceText(data, text)) { fprintf(stderr, "cannot read %s\n", package.c_str()); return 1; }
        fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    std::string error;
    auto pak = PakFile::Open(package, &error);
    if (!pak) { fprintf(stderr, "%s\n", error.c_str()); return 1; }
    auto endsWith = [](std::string const& name, std::initializer_list<char const*> extensions) {
        for (auto e : extensions) if (name.ends_with(e)) return true;
        return false;
    };
    size_t written = 0, unread = 0;
    for (auto const& entry : pak->Entries()) {
        if (!part.empty() && entry.Name.find(part) == std::string::npos) continue;
        bool binary = endsWith(entry.Name, { ".lsf", ".lsfx", ".lsbc", ".lsbs" });
        bool text = endsWith(entry.Name, { ".txt", ".lsx", ".lsj", ".xml", ".json", ".lua", ".khn" });
        if (!binary && !text) continue;
        auto data = pak->Read(entry);
        if (!data) { unread++; continue; }
        std::string out;
        if (text) {
            out.assign((char const*)data->data(), data->size());
        } else {
            if (!ResourceText(*data, out)) { unread++; continue; }
        }
        std::string path = outDir + "/" + entry.Name + (binary ? ".txt" : "");
        mkdir(outDir.c_str(), 0755);
        for (size_t slash = outDir.size() + 1; (slash = path.find('/', slash)) != std::string::npos; slash++) {
            mkdir(path.substr(0, slash).c_str(), 0755);
        }
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
        written++;
    }
    printf("%zu files written, %zu could not be read\n", written, unread);
    return 0;
}

int Usage()
{
    fprintf(stderr, "usage: bg3mse-shaders plan <Game.app> [--out DIR]\n"
                    "       bg3mse-shaders plan <Materials.pak> <mod.pak>... [--out DIR]\n"
                    "       bg3mse-shaders check <Game.app>\n"
                    "       bg3mse-shaders hooks <Game.app>\n"
                    "       bg3mse-shaders identity <Windows Materials.pak> <build label>\n"
                    "       bg3mse-shaders interface <Windows Materials.pak> <Game.app> <build label> [file of verify --prove [file of verify --stages]]\n"
                    "       bg3mse-shaders translate <package.pak> <material.lsf> <Game.app> [out-dir]\n"
                    "       bg3mse-shaders interface-check <Game.app>\n"
                    "       bg3mse-shaders interface-save <Game.app> <shader-interface.txt>\n"
                    "       bg3mse-shaders verify <Windows Materials.pak> <Game.app> [every-nth [path-part]] [--inferred] [--draw] [--reference PAK] [--list FILE] [--prove FILE] [--stages FILE]\n");
    return 2;
}

bool WriteFile(std::string const& root, ServedFile const& file)
{
    std::string path = root + "/" + file.Path;
    for (size_t at = root.size(); (at = path.find('/', at + 1)) != std::string::npos;) {
        mkdir(path.substr(0, at).c_str(), 0755);
    }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(file.Data.data(), 1, file.Data.size(), f) == file.Data.size();
    fclose(f);
    return ok;
}

int Plan(std::vector<std::string> args)
{
    std::string out;
    for (size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == "--out") {
            out = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
            break;
        }
    }
    if (args.empty()) return Usage();
    // --cache DIR keeps translated materials in DIR between runs, as the game does in its data directory.
    std::string cache;
    if (auto at = std::find(args.begin(), args.end(), "--cache"); at != args.end() && at + 1 != args.end()) {
        cache = at[1];
        args.erase(at, at + 2);
    }

    // --translate plans as the game does with shader_translation on, --translate=all with it set to all.
    bool translateFirst = std::erase(args, "--translate=all") > 0, translate = std::erase(args, "--translate") > 0 || translateFirst;
    if (args.empty()) return Usage();
    bool install = args[0].ends_with(".app") || args[0].ends_with(".app/");
    ProvisionTranslator translator;
    if (translate) {
        static constexpr std::string_view kInBundle = "/Contents/Data/Materials.pak";
        std::string app = install ? args[0] : args[0].ends_with(kInBundle) ? args[0].substr(0, args[0].size() - kInBundle.size()) : "";
        auto mac = std::make_shared<MacInterface>();
        std::string why;
        if (app.empty() || !SurveyMacInterface(app, *mac, why)) {
            fprintf(stderr, "cannot read the game's Metal functions: %s\n", app.empty() ? "the Materials.pak is not a game's" : why.c_str());
            return 1;
        }
        // The table as the game itself would have it: with the sun's shadow constants worked out only when
        // this game's binary fills them the way that was read.
        BridgeProblems problems;
        auto table = std::make_shared<InterfaceTable const>(InterfaceForBundle(app, *mac, problems));
        if (!problems.SunShadow.empty()) fprintf(stderr, "the sun's shadow constants are not worked out for this game: %s\n", problems.SunShadow.c_str());
        if (!problems.TextureTable.empty()) fprintf(stderr, "the table of textures is not bridged for this game: %s\n", problems.TextureTable.c_str());
        translator = MaterialTranslator(mac, table, cache.empty() ? nullptr : OpenTranslationCache(cache, *mac, *table));
    }

    ProvisionPlan plan;
    if (install) {
        if (args.size() != 1) return Usage();
        plan = PlanFromInstall(InstallPaths::ForGame(args[0]), VanillaContainerIdentity(), translator, translateFirst);
    } else {
        plan = PlanFromPackages(args[0], { args.begin() + 1, args.end() }, translator, translateFirst);
    }
    LogPlan(plan);
    if (!plan.Map) return 1;

    if (!out.empty()) {
        mkdir(out.c_str(), 0755);
        for (auto const& file : plan.Map->Files) {
            if (!WriteFile(out, file)) {
                fprintf(stderr, "cannot write %s\n", file.Path.c_str());
                return 1;
            }
        }
        printf("wrote %zu files under %s\n", plan.Map->Files.size(), out.c_str());
    }
    return 0;
}

#if BG3MSE_GFX
// The graphics sites, one feature per block: each function's shape, then every
// address and constant found, then whether the feature would install.
bool PrintGfx(GameImage const& image, std::string const& app)
{
    using namespace bg3se::mac::gfx;
    auto sites = FindGfxSites(image);
    bool all = true;

    // The game's shaders, as its own library on disk has them.
    printf("\n[gfx] the game's shaders (Contents/Data/Shaders/Metal/Core.metallib)\n");
    std::string unavailable;
    for (auto const& check : CheckGameShaders(app + "/Contents/Data/Shaders/Metal/Core.metallib", unavailable)) {
        printf("  %-64s %s%s\n", check.What.c_str(), check.Problem.empty() ? "as expected" : "NOT as expected: ", check.Problem.c_str());
        all = all && check.Problem.empty();
    }
    if (!unavailable.empty()) printf("  not checked here: %s\n", unavailable.c_str());
    auto feature = [&](char const* name, SiteCheck const& check, auto&& values) {
        printf("\n[gfx] %s\n", name);
        for (auto const& fn : check.Functions) {
            printf("  %-72s %5u instructions, shape %016llx%s\n", fn.Name, fn.Found.Instructions, (unsigned long long)fn.Found.Hash,
                !fn.Present ? "  MISSING" : fn.Found == fn.Expected ? "" : "  (differs from the checked shape)");
        }
        values();
        printf("  => %s%s\n", check.Ok() ? "would install" : "would NOT install: ", check.Problem.c_str());
        all = all && check.Ok();
    };
    auto va = [](char const* name, uint64_t value) { printf("  %-28s %#llx\n", name, (unsigned long long)value); };
    auto word = [](char const* name, uint32_t value) { printf("  %-28s %08x\n", name, value); };

    auto const& lua = sites.LuaJump;
    feature("gfx_lua_jump_fix", lua, [&] {
        va("LUA_LONGJMP_CALL", lua.ThrowCall);
        va("LUA_SETJMP_CALL", lua.ProtectCall);
    });
    auto const& idle = sites.ThreadIdling;
    feature("gfx_thread_idling", idle, [&] {
        va("WORKER_BUDGET", idle.WorkerBudget);
        word("WORKER_BUDGET_WORD", idle.WorkerBudgetWord);
        va("worker budget offset", idle.WorkerBudgetOffset);
        va("SCHEDULER_WINDOW_A", idle.SchedulerWindow[0]);
        va("SCHEDULER_WINDOW_B", idle.SchedulerWindow[1]);
        va("scheduler window (ns)", idle.SchedulerWindowNs);
        va("CAP_IDLE_YIELD", idle.CapIdleYield);
        va("CAP_TARGET_SELECT", idle.CapTargetSelect);
        printf("  %-28s x%u / x%u\n", "cap clock / target regs", idle.CapClockReg, idle.CapTargetReg);
        va("CLOCK_START_TICKS", idle.ClockStartTicks);
        va("CLOCK_STARTED", idle.ClockStarted);
    });
    auto const& lock = sites.ReducedLocking;
    feature("gfx_reduced_locking", lock, [&] {
        va("BIND_ALLOC", lock.BindAlloc);
        va("BIND_RESUME", lock.BindResume);
        va("SETUP_ALLOC", lock.SetupAlloc);
        va("SETUP_RESUME", lock.SetupResume);
        va("FRAME_RESET", lock.FrameReset);
        word("FRAME_RESET_WORD", lock.FrameResetWord);
        va("RENDERER_LOCK", lock.RendererLock);
        va("RENDERER_OFFSET", lock.RendererOffset);
        va("setup renderer holder", lock.SetupRendererHolder);
        va("setup renderer in holder", lock.SetupRendererInHolder);
    });
    auto const& ao = sites.AoBlur;
    feature("gfx_ao_blur_fix", ao, [&] {
        va("AO_BLUR_BIND", ao.Bind);
        va("buffer command", ao.BufferCommand);
        va("ring counter", ao.RingCounter);
    });
    auto const& j = sites.Jitter;
    feature("gfx_temporal (jitter)", j, [&] {
        va("JITTER_PROJECTION_LOAD", j.Projection.Site);
        word("JITTER_PROJECTION_WORD", j.Projection.Word);
        printf("  %-28s d%u, view in x%u\n", "projection sample / view", j.Projection.SampleReg, j.Projection.ViewReg);
        va("JITTER_CAMERA_LOAD", j.Camera.Site);
        word("JITTER_CAMERA_WORD", j.Camera.Word);
        printf("  %-28s d%u, view in x%u\n", "camera sample / view", j.Camera.SampleReg, j.Camera.ViewReg);
        va("JITTER_VELOCITY_LOAD", j.Velocity.Site);
        word("JITTER_VELOCITY_WORD", j.Velocity.Word);
        printf("  %-28s d%u, view at [x%u, #%#x]\n", "velocity sample / view", j.Velocity.SampleReg, j.VelocityStageReg, j.VelocityViewInStage);
        va("jitter table", j.JitterTable);
        va("POST_STAGE_VTABLE", j.PostStageVtable);
        va("POST_STAGE_METHOD", j.PostStageMethod);
        va("post stage method slot", j.PostStageMethodSlot);
        va("FSR_STEP_VTABLE", j.FsrStepVtable);
        va("RENDER_SETTINGS", j.RenderSettings);
        va("VIEW_STAGES", j.ViewStages);
        va("VIEW_STAGE_COUNT", j.ViewStageCount);
        va("VIEW_FRAME", j.ViewFrame);
        va("VIEW_SIZE", j.ViewSize);
        va("VIEW_CAMERA", j.ViewCamera);
        va("STAGE_STEPS", j.StageSteps);
        va("STAGE_STEP_COUNT", j.StageStepCount);
        va("NODE_ENABLED", j.NodeEnabled);
        va("FRAME_INDEX", j.FrameIndex);
        va("FRAME_INDEX_ADD", j.FrameIndexAdd);
        va("CAMERA_UNJITTERED", j.CameraUnjittered);
        va("SETTINGS_QUALITY", j.SettingsQuality);
        va("velocity stage frame", j.VelocityFrame);
    });
    feature("gfx_cluster_guard", sites.Cluster, [] {});
    auto const& calls = sites.MetalCalls;
    feature("Metal hooks (gfx_stall_log, gfx_bind_dedup, gfx_cluster_guard, gfx_temporal)", calls, [&] {
        printf("  %-28s %u, of which the hooks replace %u\n", "the game's selectors", calls.Calls, calls.Followed);
    });
    auto const& sp = sites.StagePacks;
    feature("gfx_duplicate_shadow_fix", sp, [&] {
        va("APP_INIT_STAGES_SLOT", sp.AppInitStagesSlot);
        va("APP_INIT_STAGES", sp.AppInitStages);
        va("APP_ADD_PACK_SLOT", sp.AppAddPackSlot);
        va("BASE_ADD_PACK", sp.BaseAddPack);
        va("STAGE_PACK_SHADOW", sp.PackShadow);
        va("STAGE_PACK_SHADOW_BYTE", sp.PackShadowByte);
        va("STAGE_PACK_FIRST", sp.PackFirst);
        va("STAGE_PACK_FIRST_BYTE", sp.PackFirstByte);
    });
    auto const& m = sites.ShadowMask;
    feature("gfx_improved_shadows (mask stage)", m, [&] {
        va("MASK_LOAD_SLOT", m.LoadSlot);
        va("MASK_LOAD", m.Load);
        va("MASK_UNLOAD_SLOT", m.UnloadSlot);
        va("MASK_UNLOAD", m.Unload);
        va("MASK_START_RECORDING_SLOT", m.StartRecordingSlot);
        va("MASK_START_RECORDING", m.StartRecording);
        va("MASK_STAGE_OWNER", m.StageOwner);
        va("MASK_STAGE_LIST", m.StageList);
        va("MASK_STAGE_PROGRAMS", m.StagePrograms);
        printf("  %-28s %u (%u rows of %u)\n", "program count", m.ProgramCount, m.ProgramRows, m.ProgramsPerRow);
        for (uint32_t row = 0; row < m.ProgramRows; row++) printf("  %-28s %s %ux%u\n", "row", m.Rows[row].Filter, m.Rows[row].MapSize, m.Rows[row].MapSize);
        printf("  %-28s %u bytes\n", "texture binding info", m.TextureBindingBytes);
        printf("  %-28s", "normals binding");
        for (uint8_t b : m.FragmentBinding) printf(" %02x", b);
        printf("  (slot byte %u, command %#x)\n", m.BindingSlotByte, m.TextureCommand);
        va("GET_TEXTURE_SRV", m.GetTextureSRV);
        va("SET_TEXTURE_SAFE", m.SetTextureSafe);
        va("GBUFFER0_HASH", m.GBuffer0Hash);
    });
    auto const& p = sites.Programs;
    feature("gfx_improved_shadows (programs)", p, [&] {
        va("MAKE_PATH", p.MakePath);
        va("DESTROY_PATH", p.DestroyPath);
        va("CREATE_PROGRAM", p.CreateProgram);
        va("DEFAULT_SHADER_PATH", p.DefaultShaderPath);
        va("RENDERER_DATA", p.RendererData);
        va("ENGINE_SHADER_ROOT", p.EngineShaderRoot);
        va("INVALID_HANDLE", p.InvalidHandle);
        va("DATA_DEVICE", p.DataDevice);
        va("DEVICE_PROGRAMS", p.DevicePrograms);
        va("DEVICE_CORE_LIBRARY", p.DeviceCoreLibrary);
        va("CONTAINER_PAGES", p.ContainerPages);
        va("CONTAINER_SALTS", p.ContainerSalts);
        va("CONTAINER_PAGE_SIZE", p.ContainerPageSize);
        va("PROGRAM_ENTRY_BYTES", p.ProgramEntryBytes);
        va("PROGRAM_VERTEX", p.ProgramVertex);
        va("PROGRAM_FRAGMENT", p.ProgramFragment);
        printf("  %-28s bits %u..%u (mask %#x)\n", "handle salt", p.SaltShift, p.SaltShift + 31 - __builtin_clz(p.SaltMask ? p.SaltMask : 1), p.SaltMask);
        printf("  %-28s %u bytes; view text +%u, length +%u\n", "path room", p.PathBytes, p.ViewText, p.ViewLength);
    });
    auto const& u = sites.UiRing;
    feature("gfx_ui_ring_fix", u, [&] {
        for (auto [name, r] : { std::pair{ "vertices", &u.Vertices }, std::pair{ "indices", &u.Indices } }) {
            printf("  %-8s map %#llx  wrapper +%#x  offset +%#x  map start +%#x  size %#x  buffer: mapped +%#x  range +%#x  MTLBuffer +%#x\n", name,
                (unsigned long long)r->Map, r->Wrapper, r->Offset, r->MapStart, r->Size, r->Mapped, r->Range, r->Buffer);
        }
    });

    // The islands live in the header padding, from its bottom up: every site
    // that branches to one must reach both of its ends.
    if (auto padding = HeaderPaddingOf(image.CodeAt(image.BaseVA()))) {
        uint64_t low = image.BaseVA() + (padding->Low - (uintptr_t)image.CodeAt(image.BaseVA()));
        uint64_t high = image.BaseVA() + (padding->High - (uintptr_t)image.CodeAt(image.BaseVA()));
        bool reach = true;
        // (site, the offset of its branch to the island); sites not found are left out.
        std::pair<uint64_t, uint64_t> const branches[] = { { lua.ThrowCall, 0 }, { lua.ProtectCall, 0 }, { idle.CapIdleYield, 0 }, { lock.BindAlloc, 16 },
            { lock.SetupAlloc, 12 }, { lock.FrameReset, 0 }, { ao.Bind, 20 }, { j.Projection.Site, 0 }, { j.Camera.Site, 0 }, { j.Velocity.Site, 0 } };
        for (auto [site, at] : branches) {
            if (site && !(EncodeBl(site + at, low) && EncodeBl(site + at, high - 16))) reach = false;
        }
        printf("\n[gfx] island padding %#llx-%#llx (%llu bytes): %s\n", (unsigned long long)low, (unsigned long long)high,
            (unsigned long long)(high - low), reach ? "every site reaches it" : "SOME SITE CANNOT REACH IT");
        all = all && reach;
    }
    return all;
}
#endif

// The game binary of `app`, mapped read-only for the rest of the process.
bool OpenGame(std::string const& app, GameImage& image)
{
    std::string binary = app + "/Contents/MacOS/Baldur's Gate 3";
    int fd = open(binary.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) {
        fprintf(stderr, "cannot open %s\n", binary.c_str());
        return false;
    }
    void* data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        fprintf(stderr, "cannot map %s\n", binary.c_str());
        return false;
    }
    if (!image.OpenFile({ static_cast<uint8_t const*>(data), (size_t)st.st_size })) {
        fprintf(stderr, "%s has no readable arm64 symbol table\n", binary.c_str());
        return false;
    }
    return true;
}

// Runs the entry hook planner over every function in __text.
int Hooks(std::string const& app)
{
    GameImage image;
    if (!OpenGame(app, image)) return 1;
    auto functions = image.TextSymbols();
    std::vector<uint64_t> vas(functions.size()), ends(functions.size());
    for (size_t i = 0; i < functions.size(); i++) vas[i] = functions[i].VA;
    image.NextSymbolVAs(vas, ends);

    struct Tally
    {
        size_t Count{ 0 };
        std::string_view Example;
        uint32_t ExampleWord{ 0 };
    };
    Tally tally[(size_t)HookRefusal::Count];
    for (size_t i = 0; i < functions.size(); i++) {
        uint64_t end = ends[i] && ends[i] <= image.TextEnd() ? ends[i] : image.TextEnd();
        auto refusal = CheckEntryHook(image, vas[i], end);
        auto& t = tally[(size_t)refusal];
        if (!t.Count++) {
            t.Example = functions[i].Name;
            t.ExampleWord = image.Word(vas[i]);
        }
    }
    size_t installable = tally[(size_t)HookRefusal::None].Count;
    printf("functions examined: %zu\n", functions.size());
    printf("an entry hook could be installed on: %zu (%.2f%%)\n", installable, functions.empty() ? 0.0 : 100.0 * installable / functions.size());
    printf("refusals:\n");
    printf("  %9s  %-72s  %-8s  %s\n", "count", "reason", "1st word", "example");
    for (size_t r = 1; r < (size_t)HookRefusal::Count; r++) {
        auto const& t = tally[r];
        if (!t.Count) continue;
        printf("  %9zu  %-72s  %08x  %.*s\n", t.Count, Describe((HookRefusal)r), t.ExampleWord, (int)t.Example.size(), t.Example.data());
    }
    return 0;
}

int Check(std::string const& app)
{
    GameImage image;
    if (!OpenGame(app, image)) return 1;
    auto check = CheckFileReader(image);
    printf("ls::FileReader constructor: %u instructions, shape %016llx (expected %u, %016llx)\n",
        check.Found.Constructor.Instructions, (unsigned long long)check.Found.Constructor.Hash,
        kFileReaderShape.Constructor.Instructions, (unsigned long long)kFileReaderShape.Constructor.Hash);
    printf("ls::FileReader::Read:       %u instructions, shape %016llx (expected %u, %016llx)\n",
        check.Found.Read.Instructions, (unsigned long long)check.Found.Read.Hash,
        kFileReaderShape.Read.Instructions, (unsigned long long)kFileReaderShape.Read.Hash);
    printf("%s\n", check.Ok() ? "file reader hook: would install" : ("file reader hook: would NOT install: " + check.Problem).c_str());
    auto creation = CheckShaderCreation(image);
    printf("rf::GPUDevice::CreateShader asks a Metal library for a function by name in %u places: translated functions %s%s\n",
        creation.Requests, creation.Ok() ? "would be found" : "would NOT be found: ", creation.Problem.c_str());
    auto textureTable = CheckTextureTableBridge(image);
    printf("ls::ClusteredAssignmentStage: a probe's cubes and a light's cookie %s%s\n",
        textureTable.Ok() ? "are packed as the table of textures was read from" : "are NOT bridged: ", textureTable.Problem.c_str());
    auto sunShadow = CheckSunShadowBridge(image);
    printf("ls::SetupForwardLightingConstants: the sun's shadow members %s%s\n",
        sunShadow.Ok() ? "are filled as the bridge was read from" : "are NOT bridged: ", sunShadow.Problem.c_str());

#if BG3MSE_GFX
    bool gfx = PrintGfx(image, app);
#else
    bool gfx = true;
#endif
    return check.Ok() && creation.Ok() && gfx ? 0 : 1;
}

}

// The identity table of a Windows Materials.pak, as C source on stdout; what
// was compared how, on stderr.
int Identity(std::string const& pakPath, std::string const& build)
{
    std::string error;
    auto pak = PakFile::Open(pakPath, &error);
    if (!pak) {
        fprintf(stderr, "cannot read %s: %s\n", pakPath.c_str(), error.c_str());
        return 1;
    }

    static constexpr std::string_view kVulkanExt = "_Vulkan.bshd";
    std::vector<IdentityEntry> entries;
    std::map<std::string, uint32_t> refusals;
    uint32_t stages = 0, structural = 0;
    std::set<SpirvHash> byHash, byBytes;
    for (auto const& entry : pak->Entries()) {
        std::string_view name = entry.Name;
        if (!name.ends_with(kVulkanExt)) continue;
        auto data = pak->Read(entry);
        if (!data) {
            fprintf(stderr, "cannot read %s\n", entry.Name.c_str());
            return 1;
        }
        ContainerParts parts;
        if (!ParseContainer(*data, parts)) {
            fprintf(stderr, "%s is not a shader container\n", entry.Name.c_str());
            return 1;
        }
        for (auto const& stage : parts.Stages) {
            if (stage.empty()) continue;
            stages++;
            VulkanStageHeader header;
            std::span<uint8_t const> module;
            SpirvHash hash;
            std::string why = "the stage is not a SHD8 chunk";
            if (SplitVulkanStage(stage, header, module) && SpirvModuleHash(module, hash, why)) {
                structural++;
                byHash.insert(hash);
                byBytes.insert(ContainerHash(module));      // not a container: the hash of its bytes
            } else {
                refusals[why]++;
            }
        }

        IdentityEntry e;
        identity_hash(data->data(), data->size(), e.hash);
        auto stem = name.substr(0, name.size() - kVulkanExt.size());
        e.stem = identity_stem_hash(stem.data(), stem.size());
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(), [](IdentityEntry const& a, IdentityEntry const& b) {
        int order = memcmp(a.hash, b.hash, sizeof(a.hash));
        return order ? order < 0 : a.stem < b.stem;
    });

    printf("/**\n * bg3mse - the identity table, shipped data\n *\n"
           " * The hash (Mac/Shaders/SpirvIdentity.h, truncated) of every Vulkan shader container in the Windows Materials.pak of\n"
           " * %s. Written by `bg3mse-shaders identity`. Do not edit.\n */\n\n"
           "#include \"identity.h\"\n\nconst char k_identity_build[] = \"%s\";\n\nconst IdentityEntry k_identity[] = {\n",
        build.c_str(), build.c_str());
    for (auto const& e : entries) {
        printf("{{");
        for (size_t i = 0; i < sizeof(e.hash); i++) printf("%s0x%02x", i ? "," : "", e.hash[i]);
        printf("},0x%016llxull},\n", (unsigned long long)e.stem);
    }
    printf("};\n\nconst uint32_t k_identity_count = %zu;\n", entries.size());

    fprintf(stderr, "%zu containers, %u stages: %u compared as SPIR-V, %u by their bytes\n", entries.size(), stages, structural,
        stages - structural);
    for (auto const& [why, count] : refusals) fprintf(stderr, "  %u: %s\n", count, why.c_str());
    fprintf(stderr, "modules compared as SPIR-V: %zu different by their bytes, %zu different shaders\n", byBytes.size(), byHash.size());
    return 0;
}

namespace bg3se::mac {
int InterfaceCommand(std::string const& windowsPak, std::string const& app, std::string const& build, std::string const& provenPath,
    std::string const& stagesPath);
int VerifyCommand(std::string const& windowsPak, std::string const& app, uint32_t every, std::string const& only, bool inferred, bool draw,
    std::string const& reference, std::string const& listPath, std::string const& provePath, std::string const& stagesPath);
int NamesCommand(std::string const& windowsPak, std::string const& stem, int stage);
int InterfaceCheckCommand(std::string const& app);
int InputsCommand(std::string const& package, std::string const& container, std::string const& app);
int StageSourcesCommand(std::string const& package, std::string const& part, std::string const& outDir);
int ReadsCommand(std::string const& package);
int TranslateCommand(std::string const& package, std::string const& material, std::string const& app, std::string const& out);
}

int main(int argc, char** argv)
{
    SetLogStream(stdout);
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return Usage();
    std::string command = args[0];
    args.erase(args.begin());
    if (command == "plan") return Plan(std::move(args));
    if (command == "check" && args.size() == 1) return Check(args[0]);
    if (command == "hooks" && args.size() == 1) return Hooks(args[0]);
    if (command == "identity" && args.size() == 2) return Identity(args[0], args[1]);
    if (command == "interface" && args.size() >= 3 && args.size() <= 5) {
        return InterfaceCommand(args[0], args[1], args[2], args.size() > 3 ? args[3] : "", args.size() > 4 ? args[4] : "");
    }
    if (command == "translate" && args.size() >= 3) {
        return TranslateCommand(args[0], args[1], args[2], args.size() > 3 ? args[3] : "");
    }
    if (command == "interface-check" && args.size() == 1) return InterfaceCheckCommand(args[0]);
    if (command == "interface-save" && args.size() == 2) {
        // What the game reads once per game build before it can translate, made beforehand:
        // <file> is shader-interface.txt in bg3mse's folder of the user's Application Support.
        MacInterface mac;
        std::string why, stamp = MacInterfaceStamp(args[0]);
        if (stamp.empty() || !SurveyMacInterface(args[0], mac, why) || !SaveMacInterface(args[1], stamp, mac)) {
            fprintf(stderr, "cannot make %s: %s\n", args[1].c_str(), stamp.empty() ? "the game's shader files are not there" : why.c_str());
            return 1;
        }
        printf("%zu function pairs read; kept in %s\n", mac.Passes(), args[1].c_str());
        return 0;
    }
    if (command == "stage-sources" && args.size() == 3) return StageSourcesCommand(args[0], args[1], args[2]);
    if (command == "reads" && args.size() == 1) return ReadsCommand(args[0]);
    if (command == "inputs" && (args.size() == 2 || args.size() == 3)) return InputsCommand(args[0], args[1], args.size() > 2 ? args[2] : "");
    if (command == "texts" && (args.size() == 2 || args.size() == 3)) return TextsCommand(args[0], args[1], args.size() > 2 ? args[2] : "");
    if (command == "names" && args.size() == 3) return NamesCommand(args[0], args[1], atoi(args[2].c_str()));
    if (command == "verify" && args.size() >= 2) {
        // --inferred draws each pass stage by stage, not as TranslateMaterial serves its material (a pass of a
        // material that is refused is then drawn all the same).
        // --draw also draws with both and compares the pictures. --reference PAK takes the Metal stubs
        // and Mac materials to compare with from a package (a mod's hand-made Metal shaders) instead of
        // the game; --list FILE writes one line per pass.
        bool inferred = std::erase(args, "--inferred") > 0, draw = std::erase(args, "--draw") > 0;
        // --prove FILE writes, per pairing the table only inferred, how many passes bear it out (for `interface`).
        // --stages FILE writes, per vertex stage, in how many passes it drew the same and in how many it drew
        // elsewhere (for `interface`); every vertex stage is then translated, proven or not.
        std::string reference, list, prove, stages;
        for (auto option : { std::pair<char const*, std::string*>{ "--reference", &reference }, { "--list", &list }, { "--prove", &prove },
                { "--stages", &stages } }) {
            auto at = std::find(args.begin(), args.end(), option.first);
            if (at == args.end() || at + 1 == args.end()) continue;
            *option.second = at[1];
            args.erase(at, at + 2);
        }
        return VerifyCommand(args[0], args[1], args.size() > 2 ? (uint32_t)atoi(args[2].c_str()) : 1, args.size() > 3 ? args[3] : "", inferred,
            draw, reference, list, prove, stages);
    }
    return Usage();
}
