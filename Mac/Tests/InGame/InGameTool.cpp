// bg3mse-ingame: the tool of the in-game test pass (Mac/Tests/InGame).
//
//   bg3mse-ingame lua-check FILE...
//       Compiles each Lua file with the extender's Lua (bg3se_lua), as text
//       only, and says which do not compile.
//
//   bg3mse-ingame pack ROOT OUT.pak
//       Writes every file under ROOT into an LSPK v18 package, named by its
//       path below ROOT, as the game reads mod packages.
//
//   bg3mse-ingame pak-compare ROOT PAK
//       Reads PAK back with the extender's own reader (Mac/Shaders/PakFile)
//       and compares it, file by file, with ROOT.
//
//   bg3mse-ingame check LOG...
//       Reads the result lines of the test mod (Runner.lua) from bg3mse logs,
//       a launch that crashed first and the one that resumed after it, prints
//       which bg3mse and extender each log was made by, the passes, then the
//       failures and skips by audit ID, and exits with 1 when a planned case
//       has no result line, the client or the server has no run or did not
//       end, a run whose plan has a reset phase did not end that phase, or a
//       resumed run comes without the log of the launch that crashed. The
//       logs of both machines of a two-machine run may be given together
//       (a Windows machine's Norbyte log included): each Lua state's machine
//       line says which side it is on (host or remote) and which machine it
//       is, the runs are told apart by side, and by machine too when each
//       machine hosted one game and joined the other, the side that joined
//       is expected to run a client only, and the multiplayer cases are
//       lined up by name. It also prints the overlay's
//       lines about the frames it draws on and the opt-in cases the closing
//       notice listed as not exercised.
//
//   bg3mse-ingame settings LOG...
//       Which settings of ScriptExtenderSettings.json and bg3mse.conf the
//       logs of the settings launches (Mac/Tests/InGame/Settings) show used:
//       each setting with the values the launches' settings lines name and
//       the line or case that shows its effect, or why none can. Exits with
//       1 when a setting that a log can show was not seen.
//
//   bg3mse-ingame crash-check REPORTS LOG... [--symbolicate TOOL DSYM]
//       The report of the crash the Ext.Debug.Crash case made on purpose:
//       the newest report in REPORTS (Crash/CrashReport.h), its sections,
//       its Lua backtrace naming the case, its backtrace text with the
//       other threads, its stack file, and the notice of the next launch
//       in the LOGs; with --symbolicate, TOOL (bg3mse-symbolicate) run on
//       it with DSYM must name the function that faulted. Exits with 1 when
//       anything is missing.

// As Norbyte's code includes them (Lua/LuaHelpers.h): Lua is compiled as C++.
#include <lua.h>
#include <lauxlib.h>

#include <Shaders/PakFile.h>

#include <lz4.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

// What Norbyte's Lua asks of its host (Lua/LuaBinding.cpp gives it in the
// extender): it reports an error a handler will take, for the Lua debugger.
// This tool has none.
void nse_lua_report_handled_error(lua_State*)
{
}

namespace {

// Norbyte's Lua hands every string constant the parser makes to the host's
// string cache (LUA_STRING_CACHING, lparser.c codestring); a compile-only
// check keeps none.
void CacheNothing(lua_State*, TString*)
{
}

void ReleaseNothing(lua_State*, TString*)
{
}

std::optional<std::string> ReadFile(std::string const& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string text;
    char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, n);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) return {};
    return text;
}

// --- lua-check -----------------------------------------------------------------

int LuaCheck(int argc, char** argv)
{
    if (argc < 1) {
        fprintf(stderr, "lua-check: no files\n");
        return 2;
    }
    int failed = 0;
    lua_State* L = luaL_newstate();
    if (!L) {
        fprintf(stderr, "lua-check: no Lua state\n");
        return 1;
    }
    lua_setup_strcache(L, &CacheNothing, &ReleaseNothing);
    for (int i = 0; i < argc; i++) {
        auto text = ReadFile(argv[i]);
        if (!text) {
            printf("%s: cannot read\n", argv[i]);
            failed++;
            continue;
        }
        std::string chunkName = std::string("@") + argv[i];
        int status = luaL_loadbufferx(L, text->data(), text->size(), chunkName.c_str(), "t");
        if (status != LUA_OK) {
            printf("%s\n", lua_tostring(L, -1));
            failed++;
        }
        lua_settop(L, 0);
    }
    lua_close(L);
    printf("%d file(s), %d that do not compile\n", argc, failed);
    return failed ? 1 : 0;
}

// --- pack ------------------------------------------------------------------------

constexpr uint32_t kSignature = 0x4b50534c; // "LSPK"
constexpr uint32_t kVersion = 18;
constexpr size_t kHeaderSize = 40;
constexpr size_t kEntrySize = 272;
constexpr size_t kNameSize = 256;
// The compression of the entries: LZ4 (2) at the level LSLib calls "max"
// (0x40), as the mod packages the game is given are written.
constexpr uint8_t kEntryFlags = 0x42;

bool ListFiles(std::string const& root, std::string const& relative, std::vector<std::string>& out)
{
    std::string dir = relative.empty() ? root : root + "/" + relative;
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    std::vector<std::string> names;
    while (auto e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (e->d_name[0] == '.') continue; // .DS_Store and the like
        names.push_back(e->d_name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto const& name : names) {
        std::string rel = relative.empty() ? name : relative + "/" + name;
        struct stat st{};
        if (stat((root + "/" + rel).c_str(), &st) != 0) return false;
        if (S_ISDIR(st.st_mode)) {
            if (!ListFiles(root, rel, out)) return false;
        } else if (S_ISREG(st.st_mode)) {
            out.push_back(rel);
        }
    }
    return true;
}

template <class T>
void PutLE(std::vector<uint8_t>& out, size_t at, T value)
{
    memcpy(out.data() + at, &value, sizeof(value));
}

int Pack(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "pack: ROOT OUT.pak\n");
        return 2;
    }
    std::string root = argv[0];
    std::vector<std::string> files;
    if (!ListFiles(root, "", files) || files.empty()) {
        fprintf(stderr, "pack: no files under %s\n", root.c_str());
        return 1;
    }

    std::vector<uint8_t> pak(kHeaderSize, 0);
    std::vector<uint8_t> list(files.size() * kEntrySize, 0);
    for (size_t i = 0; i < files.size(); i++) {
        auto const& name = files[i];
        if (name.size() >= kNameSize) {
            fprintf(stderr, "pack: the name %s is too long for a package entry\n", name.c_str());
            return 1;
        }
        auto text = ReadFile(root + "/" + name);
        if (!text || text->size() > (64u << 20)) {
            fprintf(stderr, "pack: cannot read %s\n", name.c_str());
            return 1;
        }
        std::vector<char> compressed(LZ4_compressBound((int)text->size()));
        int size = LZ4_compress_default(text->data(), compressed.data(), (int)text->size(), (int)compressed.size());
        if (size <= 0) {
            fprintf(stderr, "pack: cannot compress %s\n", name.c_str());
            return 1;
        }
        uint64_t offset = pak.size();
        pak.insert(pak.end(), compressed.begin(), compressed.begin() + size);

        size_t at = i * kEntrySize;
        memcpy(list.data() + at, name.data(), name.size());
        PutLE<uint32_t>(list, at + 256, (uint32_t)offset);
        PutLE<uint16_t>(list, at + 260, (uint16_t)(offset >> 32));
        list[at + 262] = 0; // archive part
        list[at + 263] = kEntryFlags;
        PutLE<uint32_t>(list, at + 264, (uint32_t)size);
        PutLE<uint32_t>(list, at + 268, (uint32_t)text->size());
    }

    std::vector<char> compressedList(LZ4_compressBound((int)list.size()));
    int listSize = LZ4_compress_default((char const*)list.data(), compressedList.data(), (int)list.size(), (int)compressedList.size());
    if (listSize <= 0) {
        fprintf(stderr, "pack: cannot compress the file list\n");
        return 1;
    }
    uint64_t listOffset = pak.size();
    std::vector<uint8_t> counts(8);
    PutLE<uint32_t>(counts, 0, (uint32_t)files.size());
    PutLE<uint32_t>(counts, 4, (uint32_t)listSize);
    pak.insert(pak.end(), counts.begin(), counts.end());
    pak.insert(pak.end(), compressedList.begin(), compressedList.begin() + listSize);

    // Header: signature, version, file list offset and size, flags, priority,
    // MD5 (zeros, as in the mods the game is given), number of parts.
    PutLE<uint32_t>(pak, 0, kSignature);
    PutLE<uint32_t>(pak, 4, kVersion);
    PutLE<uint64_t>(pak, 8, listOffset);
    PutLE<uint32_t>(pak, 16, (uint32_t)(8 + listSize));
    pak[20] = 0;
    pak[21] = 0;
    PutLE<uint16_t>(pak, 38, 1);

    std::string out = argv[1];
    std::string temp = out + ".partial";
    FILE* f = fopen(temp.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "pack: cannot write %s\n", temp.c_str());
        return 1;
    }
    bool ok = fwrite(pak.data(), 1, pak.size(), f) == pak.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(temp.c_str(), out.c_str()) != 0) {
        remove(temp.c_str());
        fprintf(stderr, "pack: cannot write %s\n", out.c_str());
        return 1;
    }
    printf("%s: %zu files, %zu bytes\n", out.c_str(), files.size(), pak.size());
    return 0;
}

int PakCompare(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "pak-compare: ROOT PAK\n");
        return 2;
    }
    std::string root = argv[0];
    std::vector<std::string> files;
    if (!ListFiles(root, "", files)) {
        fprintf(stderr, "pak-compare: cannot list %s\n", root.c_str());
        return 1;
    }
    std::string error;
    auto pak = bg3se::mac::PakFile::Open(argv[1], &error);
    if (!pak) {
        printf("%s: %s\n", argv[1], error.c_str());
        return 1;
    }
    int failed = 0;
    if (pak->Entries().size() != files.size()) {
        printf("the package has %zu entries, the folder %zu files\n", pak->Entries().size(), files.size());
        failed++;
    }
    for (auto const& name : files) {
        auto entry = pak->Find(name);
        if (!entry) {
            printf("%s: not in the package\n", name.c_str());
            failed++;
            continue;
        }
        auto data = pak->Read(*entry);
        auto text = ReadFile(root + "/" + name);
        if (!data || !text || data->size() != text->size() || memcmp(data->data(), text->data(), text->size()) != 0) {
            printf("%s: reads back different\n", name.c_str());
            failed++;
        }
    }
    printf("%zu files compared, %d difference(s)\n", files.size(), failed);
    return failed ? 1 : 0;
}

// --- check -------------------------------------------------------------------

constexpr char kMark[] = "@BG3MSE-IGT|";

std::vector<std::string> Split(std::string const& text, char sep)
{
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        size_t at = text.find(sep, start);
        if (at == std::string::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

struct Line
{
    std::string Context, Id, Name, Result, Detail;
};

// What one plan of a run gave. A run begins a plan when it starts from its
// first case: at first, and again when the trigger file (or the mod) was
// changed under the same run id, so that the cases are no longer the ones
// the progress file counted. A launch that resumes goes on with the plan it
// finds.
struct Plan
{
    std::string BegunIn; // the log with its first line
    unsigned Begins{ 0 };
    unsigned Resumes{ 0 }; // begin lines of a launch that resumed after a crash
    unsigned Ends{ 0 };
    // The reset phase: planned cases of it, its begin lines (in the state a
    // reset made, or in a launch that resumed the phase) and end lines.
    bool PlansReset{ false };
    unsigned ResetBegins{ 0 };
    unsigned ResetEnds{ 0 };
    bool AlreadyFinished{ false };
    // A case is its id and name; the count is how many lines name it.
    using Case = std::pair<std::string, std::string>;
    std::map<Case, unsigned> Planned;
    std::map<Case, unsigned> Reported;
    std::vector<Case> Order; // the planned cases as the plan lists them
    std::vector<Line> Lines;

    bool Empty() const
    {
        return Begins + Resumes + Ends + ResetBegins + ResetEnds == 0 && !AlreadyFinished && Planned.empty() && Lines.empty();
    }
    bool Finished() const
    {
        return AlreadyFinished || (Ends > 0 && (!PlansReset || ResetEnds > 0));
    }
    std::map<std::string, unsigned> Counts() const
    {
        std::map<std::string, unsigned> counts;
        for (auto const& l : Lines) counts[l.Result]++;
        return counts;
    }
    unsigned PlannedCount() const
    {
        unsigned planned = 0;
        for (auto const& p : Planned) planned += p.second;
        return planned;
    }
};

struct Run
{
    std::string Version; // Ext.Utils.Version and GameVersion
    std::vector<Plan> Plans; // in the order they were begun; the last is the one reported

    Plan& Current(std::string const& log)
    {
        if (Plans.empty()) Begin(log);
        return Plans.back();
    }
    void Begin(std::string const& log)
    {
        if (!Plans.empty() && Plans.back().Empty()) return;
        Plans.emplace_back();
        Plans.back().BegunIn = log;
    }
    Plan const& Last() const { return Plans.back(); }
};

std::string TotalsText(Plan const& plan)
{
    auto counts = plan.Counts();
    char text[160];
    snprintf(text, sizeof(text), "%u planned, %u pass, %u error, %u skip, %u crashed, %u uncovered", plan.PlannedCount(),
        counts["pass"], counts["error"], counts["skip"], counts["crashed"], counts["uncovered"]);
    return text;
}

int Check(int argc, char** argv)
{
    if (argc < 1) {
        fprintf(stderr, "check: LOG...\n");
        return 2;
    }
    // A run is a context on one side of a game: the side is what the Lua
    // state's machine line says (host or remote, in a two-machine run), and
    // until a log's first such line, what that one says; logs from before
    // that line have none. The line may name the machine too (an id the
    // runner keeps in the profile): when a side was taken by more than one
    // machine (each hosted one game and joined the other's), the runs are
    // told apart by machine as well.
    struct Log
    {
        std::string Name, Role;
        std::vector<std::string> Lines;
    };
    std::vector<Log> logs;
    std::set<std::string> roles;
    std::map<std::string, unsigned> machines; // by id: 1, 2, ... in the order the logs name them
    std::map<std::string, std::set<std::string>> machinesOf; // by role: the ids
    auto Field = [](std::vector<std::string> const& f, size_t i) {
        std::string value = i < f.size() ? f[i] : "";
        if (!value.empty() && value.back() == '\r') value.pop_back();
        return value;
    };
    for (int i = 0; i < argc; i++) {
        auto text = ReadFile(argv[i]);
        if (!text) {
            fprintf(stderr, "check: cannot read %s\n", argv[i]);
            return 2;
        }
        Log log;
        log.Name = argv[i];
        if (auto slash = log.Name.rfind('/'); slash != std::string::npos) log.Name = log.Name.substr(slash + 1);
        log.Lines = Split(*text, '\n');
        bool marked = false, said = false;
        for (auto const& raw : log.Lines) {
            auto at = raw.find(kMark);
            if (at == std::string::npos) continue;
            marked = true;
            auto f = Split(raw.substr(at + strlen(kMark)), '|');
            if (f.size() >= 3 && f[1] == "machine") {
                auto role = Field(f, 2);
                auto id = Field(f, 3);
                if (!said) log.Role = role;
                said = true;
                roles.insert(role);
                if (!id.empty() && id != "-") {
                    machines.emplace(id, (unsigned)machines.size() + 1);
                    machinesOf[role].insert(id);
                }
            }
        }
        // A log without a line of the tests (the crash reporter's) names no machine.
        if (marked && !said) roles.insert(log.Role);
        logs.push_back(std::move(log));
    }
    // The sides are named only when the logs come from more than one, the
    // machines only when one side was taken by more than one.
    bool twoMachines = roles.size() > 1;
    bool nameMachines = false;
    for (auto const& [role, ids] : machinesOf) nameMachines = nameMachines || ids.size() > 1;
    auto RunName = [&](unsigned machine, std::string const& role, std::string const& context) {
        std::string name = nameMachines && machine != 0 ? "machine " + std::to_string(machine) + " " : "";
        return name + (twoMachines && !role.empty() ? role + " " + context : context);
    };

    std::map<std::string, Run> runs; // by RunName
    std::vector<std::string> builds; // "log: the line bg3mse writes when it is loaded"
    std::vector<std::string> overlay; // "log: the overlay's line about the frames it draws on"
    std::vector<std::string> notExercised;
    std::set<std::pair<unsigned, std::string>> sides; // the machine (0: not named) and the role
    std::map<unsigned, std::vector<std::string>> logsOf; // by machine: its logs
    for (auto& log : logs) {
        auto const& logName = log.Name;
        // What the machine lines read so far say: each context's side, and
        // the machine.
        std::map<std::string, std::string> roleOf;
        unsigned machine = 0;
        for (auto raw : log.Lines) {
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            auto at = raw.find(kMark);
            if (at == std::string::npos) {
                // Mac/Overlay/MetalBackend.cpp: the target the overlay draws on
                // (the hardware HDR case reads its colour space here).
                auto imgui = raw.find("IMGUI: the game");
                if (imgui != std::string::npos) {
                    overlay.push_back(logName + ": " + raw.substr(imgui));
                    continue;
                }
                // Mac/Main.cpp: "bg3mse <version> loaded (commit ..., extender ..., <channel>)",
                // or "bg3mse loaded" from a build before the version stamp.
                auto loaded = raw.find("] bg3mse ");
                if (loaded != std::string::npos && raw.find(" loaded", loaded) != std::string::npos) {
                    builds.push_back(logName + ": " + raw.substr(loaded + 2));
                }
                continue;
            }
            std::string line = raw.substr(at + strlen(kMark));
            auto f = Split(line, '|');
            if (f.size() < 2) continue;
            if (f[1] == "machine") {
                roleOf[f[0]] = Field(f, 2);
                auto id = machines.find(Field(f, 3));
                if (id != machines.end() && machine != id->second) {
                    machine = id->second;
                    logsOf[machine].push_back(logName);
                }
                continue;
            }
            auto said = roleOf.find(f[0]);
            auto const& role = said == roleOf.end() ? log.Role : said->second;
            sides.insert({ nameMachines ? machine : 0, role });
            auto name = RunName(machine, role, f[0]);
            auto& run = runs[name];
            if (f[1] == "notice" && f.size() >= 4 && f[2] == "not exercised") {
                notExercised.push_back(name + ": " + f[3]);
                continue;
            }
            bool resumed = f.size() >= 5 && f[4].rfind("resume after ", 0) == 0;
            if (f[1] == "begin") {
                if (resumed) {
                    run.Current(logName).Resumes++;
                } else {
                    // From the first case: a plan of its own.
                    run.Begin(logName);
                    run.Current(logName).Begins++;
                }
                continue;
            }
            auto& plan = run.Current(logName);
            if (f[1] == "reset-begin") {
                if (resumed) {
                    plan.Resumes++;
                } else {
                    plan.ResetBegins++;
                }
                continue;
            }
            if (f[1] == "end") {
                plan.Ends++;
                continue;
            }
            if (f[1] == "reset-end") {
                plan.ResetEnds++;
                continue;
            }
            if (f[1] == "version") {
                if (f.size() >= 4) run.Version = "Ext.Utils.Version " + f[2] + ", game " + f[3];
                continue;
            }
            if (f[1] == "plan" || f[1] == "reset" || f[1] == "notice" || f[1] == "settings" || f.size() < 4) continue;
            Line l{ name, f[1], f[2], f[3], "" };
            for (size_t k = 4; k < f.size(); k++) l.Detail += (k > 4 ? "|" : "") + f[k];
            auto key = std::make_pair(l.Id, l.Name);
            if (l.Result == "planned") {
                if (plan.Planned[key]++ == 0) plan.Order.push_back(key);
                if (l.Detail == "reset") plan.PlansReset = true;
            } else if (l.Name == "already-finished") {
                plan.AlreadyFinished = true;
            } else {
                plan.Reported[key]++;
                plan.Lines.push_back(std::move(l));
            }
        }
    }

    if (runs.empty()) {
        printf("no result line of the in-game tests in the log(s)\n");
        return 1;
    }

    printf("== build\n");
    for (auto const& b : builds) printf("%s\n", b.c_str());
    if (nameMachines) {
        for (auto const& [machine, names] : logsOf) {
            std::string list;
            for (auto const& n : names) list += (list.empty() ? "" : ", ") + n;
            printf("machine %u: %s\n", machine, list.c_str());
        }
    }
    for (auto const& [context, run] : runs) {
        if (!run.Version.empty()) printf("%s: %s\n", context.c_str(), run.Version.c_str());
    }

    // A run that began more than one plan: the results are the last one's.
    // What the earlier ones gave is said here, so that a crash in a case the
    // last plan leaves out stays in the report.
    bool replanned = false;
    for (auto const& [context, run] : runs) replanned = replanned || run.Plans.size() > 1;
    if (replanned) {
        printf("== plans\n");
        for (auto const& [context, run] : runs) {
            if (run.Plans.size() < 2) continue;
            auto const& last = run.Last();
            printf("%s: %zu plans were begun; the results are of the last, begun in %s\n", context.c_str(), run.Plans.size(),
                last.BegunIn.c_str());
            for (size_t i = 0; i + 1 < run.Plans.size(); i++) {
                auto const& plan = run.Plans[i];
                std::string state = "it finished";
                if (!plan.Finished()) {
                    state = "it did not finish";
                    for (auto const& key : plan.Order) {
                        auto it = plan.Reported.find(key);
                        if (it == plan.Reported.end() || it->second < plan.Planned.at(key)) {
                            state += ": its first case without a result is " + key.first + " " + key.second;
                            break;
                        }
                    }
                }
                printf("  the plan begun in %s: %s; %s\n", plan.BegunIn.c_str(), TotalsText(plan).c_str(), state.c_str());
                for (auto const& l : plan.Lines) {
                    if (l.Result == "crashed") {
                        printf("    crashed: %s %s%s%s\n", l.Id.c_str(), l.Name.c_str(), l.Detail.empty() ? "" : ": ", l.Detail.c_str());
                    }
                }
                for (auto const& key : plan.Order) {
                    if (last.Planned.find(key) != last.Planned.end()) continue;
                    std::string result;
                    for (auto const& l : plan.Lines) {
                        if (l.Id == key.first && l.Name == key.second) result += (result.empty() ? "" : ", ") + l.Result;
                    }
                    printf("    not in the last plan: %s %s (%s)\n", key.first.c_str(), key.second.c_str(),
                        result.empty() ? "no result" : result.c_str());
                }
            }
        }
    }

    bool incomplete = false;
    std::map<std::string, std::vector<Line const*>> byResult;
    for (auto const& [context, run] : runs) {
        for (auto const& l : run.Last().Lines) {
            byResult[l.Result].push_back(&l);
        }
    }

    printf("== pass\n");
    for (auto l : byResult["pass"]) {
        printf("%s %s %s%s%s\n", l->Context.c_str(), l->Id.c_str(), l->Name.c_str(),
            l->Detail.empty() ? "" : ": ", l->Detail.c_str());
    }

    // The failures and skips, by audit ID.
    for (char const* result : { "error", "crashed", "uncovered", "skip" }) {
        auto& lines = byResult[result];
        if (lines.empty()) continue;
        std::stable_sort(lines.begin(), lines.end(), [](Line const* a, Line const* b) { return a->Id < b->Id; });
        printf("== %s\n", result);
        std::string id;
        for (auto l : lines) {
            if (l->Id != id) {
                id = l->Id;
                printf("%s\n", id.c_str());
            }
            printf("  %s %s%s%s\n", l->Context.c_str(), l->Name.c_str(), l->Detail.empty() ? "" : ": ", l->Detail.c_str());
        }
    }

    // The cases of a two-machine run, lined up by name: what each machine's
    // contexts gave.
    std::map<std::string, std::vector<Line const*>> multiplayer;
    for (auto const& [name, run] : runs) {
        for (auto const& l : run.Last().Lines) {
            if (l.Name.rfind("MP.", 0) == 0) multiplayer[l.Name].push_back(&l);
        }
    }
    if (!multiplayer.empty()) {
        printf("== multiplayer\n");
        for (auto const& [name, lines] : multiplayer) {
            printf("%s\n", name.c_str());
            for (auto l : lines) {
                printf("  %s %s%s%s\n", l->Context.c_str(), l->Result.c_str(), l->Detail.empty() ? "" : ": ", l->Detail.c_str());
            }
        }
    }
    if (!overlay.empty()) {
        printf("== overlay\n");
        for (auto const& o : overlay) printf("%s\n", o.c_str());
    }
    if (!notExercised.empty()) {
        printf("== opt-in cases not exercised\n");
        for (auto const& n : notExercised) printf("%s\n", n.c_str());
    }

    std::vector<std::string> missing;
    // The game runs both on each machine that hosts, the server first, then
    // the client; a machine that joined another's game runs a client only.
    for (auto const& [machine, role] : sides) {
        for (char const* context : { "client", "server" }) {
            if (role == "remote" && std::string(context) == "server") continue;
            auto name = RunName(machine, role, context);
            if (runs.find(name) == runs.end()) missing.push_back(name + ": no line at all");
        }
    }
    for (auto const& [context, whole] : runs) {
        auto const& run = whole.Last();
        if (run.Begins == 0 && run.Resumes > 0) {
            missing.push_back(context + ": the run resumed after a crash, but the log of the launch that crashed was not given");
            continue;
        }
        if (run.Begins == 0 && !run.AlreadyFinished) {
            missing.push_back(context + ": no begin line");
            continue;
        }
        if (run.Ends == 0 && !run.AlreadyFinished) {
            missing.push_back(context + ": no end line (the run did not finish)");
        }
        if (run.PlansReset && run.ResetEnds == 0 && !run.AlreadyFinished) {
            missing.push_back(context + ": no reset-end line (the reset phase did not finish)");
        }
        for (auto const& [key, planned] : run.Planned) {
            auto it = run.Reported.find(key);
            unsigned reported = it == run.Reported.end() ? 0 : it->second;
            for (unsigned n = reported; n < planned; n++) {
                missing.push_back(context + " " + key.first + " " + key.second + ": no result line");
            }
        }
    }
    if (!missing.empty()) {
        incomplete = true;
        printf("== missing\n");
        for (auto const& m : missing) printf("%s\n", m.c_str());
    }

    printf("== total\n");
    for (auto const& [context, run] : runs) {
        printf("%s: %s%s\n", context.c_str(), TotalsText(run.Last()).c_str(), run.Plans.size() > 1 ? " (the last plan)" : "");
    }
    printf("%s%s\n", incomplete ? "incomplete: lines are missing" : "complete",
        replanned ? " (of each run's last plan; what the earlier plans gave is under == plans)" : "");
    return incomplete ? 1 : 0;
}

// --- settings -------------------------------------------------------------------

// How a setting shows that it was used.
enum class Seen
{
    Case,        // a case of that name passed (the runner reads the effect)
    Line,        // a log line holds the text
    Named,       // only the settings line naming it: no line or case shows its effect
    Owner,       // the owner sees the effect; the settings line says it was set
    Coordinator, // the coordinator sees it on disk
    Cannot,      // no launch can show it safely
};

struct SettingRow
{
    char const* File;
    char const* Key;
    Seen How;
    char const* Evidence; // the case's name or the line's text
    char const* Note;
};

constexpr char kSE[] = "ScriptExtenderSettings.json";
constexpr char kConf[] = "bg3mse.conf";

// Every key Norbyte's ApplyConfig reads (Extender/Shared/ExtenderConfig.inl)
// and every key of bg3mse.conf (Platform/Config.h, Binding/Startup.h,
// Overlay/ConsoleWindow.h, Gfx/Gfx.cpp, Crash/ExtenderCrashReporting.cpp).
// The camera module's keys are in its own file, not bg3mse.conf.
SettingRow const kSettings[] = {
    { kSE, "CreateConsole", Seen::Line, "Console: the game has no terminal, so the console is a window", "the console window opened at start (Mac/Overlay/ConsoleWindow.cpp) [rows: CFG-02]" },
    { kSE, "DefaultToClientConsole", Seen::Owner, "", "the console window's prompt is the client's at the start (Norbyte's console) [rows: CFG-03]" },
    { kSE, "EnableDebugger", Seen::Line, "Osiris debugger listening on 127.0.0.1:", "DEBUG line, needs log_debug (OsirisExtender.cpp) [rows: CFG-04]" },
    { kSE, "DebuggerPort", Seen::Line, "Osiris debugger listening on 127.0.0.1:9989", "the port of the settings files (9989) [rows: CFG-05]" },
    { kSE, "EnableLuaDebugger", Seen::Line, "Lua debugger listening on 127.0.0.1:", "DEBUG line, needs log_debug (ScriptExtender.cpp) [rows: CFG-06]" },
    { kSE, "LuaDebuggerPort", Seen::Line, "Lua debugger listening on 127.0.0.1:9988", "the port of the settings files (9988) [rows: CFG-07]" },
    { kSE, "ExtendStory", Seen::Case, "Story#compiled", "the story mod's goal compiled with NRD_LuaCall (a new game) [rows: CFG-08]" },
    { kSE, "DisableStoryCompilation", Seen::Case, "Story#compiled", "false: the new game's story compiled with the mod's goal [rows: CFG-19]" },
    { kSE, "LogFailedCompile", Seen::Cannot, "", "logs only a story compile that fails: no safe way to make one fail [rows: CFG-09]" },
    { kSE, "EnableLogging", Seen::Line, "Starting Osiris Runtime debug logging", "DEBUG line, needs log_debug (OsirisExtender.cpp) [rows: CFG-11, FEAT-18]" },
    { kSE, "LogCompile", Seen::Line, "Starting Compile debug logging", "DEBUG line of a story compile (the new game) [rows: CFG-10, FEAT-18]" },
    { kSE, "LogRuntime", Seen::Line, "Extender runtime log written to", "DEBUG line, needs log_debug (ScriptExtender.cpp) [rows: CFG-12, FEAT-18]" },
    { kSE, "LogDirectory", Seen::Line, "logs with space/", "a log path in the directory the settings files name [rows: CFG-13, G-015]" },
    { kSE, "SendCrashReports", Seen::Line, "crash report written to", "true: the crash run's report (Mac/Crash/CrashDelivery.cpp) [rows: CFG-14]" },
    { kSE, "ForceCrashReporting", Seen::Cannot, "", "changes only a fault in the game's own code: none can be made on purpose safely [rows: CFG-15]" },
    { kSE, "EnableAchievements", Seen::Line, "patch 'ls::ModuleSettings::IsModded' applied", "true: the IsModded patch went in [rows: CFG-16, PATCH-02]" },
    { kSE, "DisableLauncher", Seen::Line, "patch 'KillLauncher' applied", "true: the KillLauncher patch went in [rows: CFG-17, MAP-04, PATCH-01]" },
    { kSE, "DisableStoryPatching", Seen::Case, "Settings.DisableStoryPatching", "the game's switch read from Lua [rows: CFG-18]" },
    { kSE, "InsanityCheck", Seen::Line, "Removed ModCrashSanityCheck", "the custom-profile launch: the empty ModCrashSanityCheck folder the coordinator made in that profile is removed (ScriptExtenderClient.cpp) [rows: CFG-20, SHIM-31]" },
    { kSE, "LocalMessagePassing", Seen::Case, "Ext.Net.PostMessageToServer#module", "false: a mod's message took the network path, not the local one (ClientNet.inl), and arrived [rows: CFG-21, NET-01]" },
    { kSE, "Optick", Seen::Case, "Settings.Optick", "Ext.Config.ProfilerEnabled; the profiler is compiled out of Release, as in Norbyte's [rows: CFG-22, FEAT-33]" },
    { kSE, "MergeVirtualTextures", Seen::Cannot, "", "writes merged tile sets into the game's Data folder, the installed game (VirtualTextureMerge.inl, OpenOutputFile); Norbyte marks it work in progress [rows: CFG-23]" },
    { kSE, "DeveloperMode", Seen::Case, "Settings.DeveloperMode", "Ext.Debug.IsDeveloperMode [rows: CFG-24]" },
    { kSE, "ClearOnReset", Seen::Owner, "", "the console window is emptied at a Lua reset only while it is on [rows: CFG-25]" },
    { kSE, "DebugFlags", Seen::Named, "", "Osiris's debug flags, set to the default 0: what the story log holds, no line names them [rows: CFG-26]" },
    { kSE, "LuaBuiltinResourceDirectory", Seen::Case, "Settings.LuaBuiltinResourceDirectory", "a builtin script read from the directory [rows: CFG-27, G-142]" },
    { kSE, "CustomProfile", Seen::Coordinator, "", "~/Documents/Larian Studios/<CustomProfile> is made at the start [rows: CFG-28, HOOK-02, SYM-12, ENG-02, EVT-02]" },
    { kSE, "EnablePerfMessages", Seen::Case, "Settings.EnablePerfMessages", "Ext.Config.PerfMessagesEnabled [rows: CFG-29]" },
    { kSE, "ProfilerLoadThresholdError", Seen::Line, "Loading BootstrapServer.lua for mod", "a bootstrap's load time over the threshold [rows: CFG-30]" },
    { kSE, "ProfilerLoadCallbackThresholdError", Seen::Case, "Settings.ProfilerLoadCallbackThresholdError", "Ext.Config.ProfilerLoadCallbackErrorThreshold [rows: CFG-31]" },
    { kSE, "ProfilerCallbackThresholdError", Seen::Case, "Settings.ProfilerCallbackThresholdError", "the server's Ext.Config.ProfilerCallbackErrorThreshold [rows: CFG-32]" },
    { kSE, "ProfilerClientCallbackThresholdError", Seen::Case, "Settings.ProfilerClientCallbackThresholdError", "the client's Ext.Config.ProfilerCallbackErrorThreshold [rows: CFG-33]" },
    { kSE, "(not JSON)", Seen::Line, "Failed to load configuration file", "a broken file: Fail(), a message box, the process ends [rows: F2-09, W2-01, FEAT-05, SHIM-103, SHIM-64]" },
    { kConf, "script_extender", Seen::Line, "Script Extender: turned on in bg3mse.conf", "Mac/Main.cpp [rows: F2-10]" },
    { kConf, "log_debug", Seen::Line, "the log takes DEBUG lines", "Mac/Main.cpp [rows: F2-10, FEAT-17]" },
    { kConf, "crash_dialog", Seen::Owner, "", "on: after a crash the next start shows the crash box; off: no box [rows: F2-10]" },
    { kConf, "console_key", Seen::Line, "Console: console_key = ", "a key the settings files name wrongly on purpose: the warning [rows: F2-10]" },
    { kConf, "debug_crash", Seen::Case, "Ext.Debug.Crash#report", "the crash run's Ext.Debug.Crash closed the game [rows: LUA-039]" },
    { kConf, "gfx_lua_jump_fix", Seen::Line, "[Gfx] gfx_lua_jump_fix:", "Mac/Gfx/Gfx.cpp" },
    { kConf, "gfx_thread_idling", Seen::Line, "[Gfx] gfx_thread_idling:", "Mac/Gfx/Gfx.cpp" },
    { kConf, "gfx_reduced_locking", Seen::Line, "[Gfx] gfx_reduced_locking:", "Mac/Gfx/Gfx.cpp" },
    { kConf, "gfx_ao_blur_fix", Seen::Line, "[Gfx] gfx_ao_blur_fix:", "Mac/Gfx/Gfx.cpp" },
    { kConf, "gfx_ui_ring_fix", Seen::Line, "[Gfx] gfx_ui_ring_fix:", "Mac/Gfx/Gfx.cpp" },
    { kConf, "gfx_ui_ring_observe", Seen::Named, "", "only with gfx_ui_ring_fix: what the fix watches" },
    { kConf, "gfx_duplicate_shadow_fix", Seen::Line, "[Gfx] gfx_duplicate_shadow_fix:", "Mac/Gfx/Gfx.cpp" },
    { kConf, "gfx_improved_shadows", Seen::Line, "[Gfx] gfx_improved_shadows:", "Mac/Gfx/Gfx.cpp" },
    { kConf, "gfx_stall_log", Seen::Line, "[Gfx] gfx_stall_log:", "Mac/Gfx/Gfx.cpp" },
    { kConf, "gfx_bind_dedup", Seen::Line, "[Gfx] gfx_bind_dedup:", "Mac/Gfx/Gfx.cpp" },
    { kConf, "gfx_cluster_guard", Seen::Line, "[Gfx] gfx_cluster_guard:", "Mac/Gfx/Gfx.cpp" },
    { kConf, "gfx_temporal", Seen::Line, "[Gfx] gfx_temporal:", "Mac/Gfx/Gfx.cpp" },
    { kConf, "gfx_temporal_sharpen", Seen::Named, "", "read only with gfx_temporal on; no line names the strength" },
};

std::string Trimmed(std::string text)
{
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
    size_t start = 0;
    while (start < text.size() && text[start] == ' ') start++;
    return text.substr(start);
}

int Settings(int argc, char** argv)
{
    if (argc < 1) {
        fprintf(stderr, "settings: LOG...\n");
        return 2;
    }
    // key -> "launch=value" as the settings lines name them
    std::map<std::string, std::vector<std::string>> named;
    // case name -> where it passed
    std::map<std::string, std::string> passed;
    std::vector<std::pair<std::string, std::string>> lines; // log name, line
    for (int i = 0; i < argc; i++) {
        auto text = ReadFile(argv[i]);
        if (!text) {
            fprintf(stderr, "settings: cannot read %s\n", argv[i]);
            return 2;
        }
        std::string logName = argv[i];
        if (auto slash = logName.rfind('/'); slash != std::string::npos) logName = logName.substr(slash + 1);
        for (auto const& raw : Split(*text, '\n')) {
            auto line = Trimmed(raw);
            auto at = line.find(kMark);
            if (at == std::string::npos) {
                lines.emplace_back(logName, line);
                continue;
            }
            auto f = Split(line.substr(at + strlen(kMark)), '|');
            if (f.size() >= 5 && f[1] == "settings") {
                auto& list = named[f[3]];
                auto entry = f[2] + "=" + f[4];
                if (std::find(list.begin(), list.end(), entry) == list.end()) list.push_back(entry);
            } else if (f.size() >= 4 && f[3] == "pass") {
                // ctx|id|name|result|detail
                passed.emplace(f[2], logName + " " + f[0]);
            }
        }
    }

    printf("== settings\n");
    unsigned seen = 0, owner = 0, cannot = 0, unseen = 0;
    char const* file = nullptr;
    for (auto const& row : kSettings) {
        if (file == nullptr || strcmp(file, row.File) != 0) {
            file = row.File;
            printf("%s\n", file);
        }
        std::string values;
        if (auto it = named.find(row.Key); it != named.end()) {
            for (auto const& v : it->second) values += (values.empty() ? "" : ", ") + v;
        }
        std::string verdict;
        bool ok = false;
        switch (row.How) {
        case Seen::Case:
            if (auto it = passed.find(row.Evidence); it != passed.end()) {
                verdict = std::string("seen: ") + row.Evidence + " passed (" + it->second + ")";
                ok = true;
            } else {
                verdict = std::string("NOT SEEN: no pass of ") + row.Evidence;
            }
            break;
        case Seen::Line: {
            for (auto const& [log, text] : lines) {
                if (text.find(row.Evidence) != std::string::npos) {
                    auto shown = text.size() > 160 ? text.substr(0, 160) + "..." : text;
                    verdict = "seen: " + log + ": " + shown;
                    ok = true;
                    break;
                }
            }
            if (!ok) verdict = std::string("NOT SEEN: no line holds \"") + row.Evidence + "\"";
            break;
        }
        case Seen::Named:
            ok = !values.empty();
            verdict = ok ? "set; nothing shows its effect" : "NOT SEEN: no launch named it";
            break;
        case Seen::Owner:
            ok = !values.empty();
            verdict = ok ? "set; the owner's word is the evidence" : "NOT SEEN: no launch named it";
            if (ok) owner++;
            break;
        case Seen::Coordinator:
            ok = true;
            verdict = "the coordinator checks it on disk";
            owner++;
            break;
        case Seen::Cannot:
            ok = true;
            verdict = "cannot be shown";
            cannot++;
            break;
        }
        if (row.How != Seen::Owner && row.How != Seen::Coordinator && row.How != Seen::Cannot && ok) seen++;
        if (!ok) unseen++;
        printf("  %s: %s%s%s [%s]\n", row.Key, verdict.c_str(), values.empty() ? "" : "; values: ", values.c_str(), row.Note);
    }
    printf("== settings total\n%u seen, %u by the owner or the coordinator, %u that cannot be shown, %u not seen\n",
        seen, owner, cannot, unseen);
    return unseen == 0 ? 0 : 1;
}

// --- crash-check --------------------------------------------------------------------

struct ReportSection
{
    std::string Tag;
    std::string Body;
};

std::optional<std::vector<ReportSection>> ReadReport(std::string const& bytes, std::string& error)
{
    auto U32 = [&](size_t at) {
        return uint32_t((unsigned char)bytes[at]) | uint32_t((unsigned char)bytes[at + 1]) << 8
            | uint32_t((unsigned char)bytes[at + 2]) << 16 | uint32_t((unsigned char)bytes[at + 3]) << 24;
    };
    auto TagText = [&](uint32_t tag) {
        // MSVC's character constant: 'VERS' is 0x56455253.
        return std::string{ char(tag >> 24), char(tag >> 16), char(tag >> 8), char(tag) };
    };
    if (bytes.size() < 4 || TagText(U32(0)) != "CRSH") {
        error = "not a crash report (no CRSH signature)";
        return {};
    }
    std::vector<ReportSection> sections;
    size_t pos = 4;
    while (pos + 8 <= bytes.size()) {
        auto tag = TagText(U32(pos));
        uint32_t length = U32(pos + 4);
        if (pos + 8 + length > bytes.size()) {
            error = "section " + tag + " runs past the file's end";
            return {};
        }
        sections.push_back({ tag, bytes.substr(pos + 8, length) });
        pos += 8 + length;
    }
    return sections;
}

int CrashCheck(int argc, char** argv)
{
    std::vector<std::string> logs;
    std::string reports, tool, dsym;
    for (int i = 0; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--symbolicate" && i + 2 < argc) {
            tool = argv[++i];
            dsym = argv[++i];
        } else if (reports.empty()) {
            reports = a;
        } else {
            logs.push_back(a);
        }
    }
    if (reports.empty() || logs.empty()) {
        fprintf(stderr, "crash-check: REPORTS LOG... [--symbolicate TOOL DSYM]\n");
        return 2;
    }
    unsigned missing = 0;
    auto Report = [&](bool ok, std::string const& what) {
        printf("%s %s\n", ok ? "ok     " : "MISSING", what.c_str());
        if (!ok) missing++;
    };

    // The newest report: the names carry the time ("crash_2026-11-12_20-03-03.crpt").
    std::string newest;
    if (DIR* d = opendir(reports.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name.rfind("crash_", 0) == 0 && name.size() > 5 && name.substr(name.size() - 5) == ".crpt" && name > newest) newest = name;
        }
        closedir(d);
    }
    if (newest.empty()) {
        printf("no crash_*.crpt in %s\n", reports.c_str());
        return 1;
    }
    auto stem = reports + "/" + newest.substr(0, newest.size() - 5);
    printf("== report %s\n", newest.c_str());
    auto bytes = ReadFile(stem + ".crpt");
    std::string error;
    auto sections = bytes ? ReadReport(*bytes, error) : std::nullopt;
    if (!sections) {
        printf("cannot read the report: %s\n", bytes ? error.c_str() : "cannot open it");
        return 1;
    }
    std::map<std::string, std::string> byTag;
    for (auto const& s : *sections) byTag[s.Tag] = s.Body;
    // Norbyte's sections (CERT only in a certified build, ERRL only after an
    // error, CSTA/SSTA only once a state exists) and the Mac ones.
    for (char const* tag : { "VERS", "SVER", "GVER", "SDAT", "CHAN", "BLDT", "TRAC", "LUAT", "MODS", "SLEN",
                             "MFLT", "MREG", "MBTR", "MIMG", "MSYS", "MBLD", "MMAP", "MTHR" }) {
        auto it = byTag.find(tag);
        Report(it != byTag.end() && !it->second.empty(), std::string("section ") + tag
            + (it == byTag.end() ? "" : " (" + std::to_string(it->second.size()) + " bytes)"));
    }
    auto Has = [&](char const* tag, char const* text) {
        auto it = byTag.find(tag);
        return it != byTag.end() && it->second.find(text) != std::string::npos;
    };
    Report(Has("LUAT", "CrashReport.lua"), "the Lua backtrace names the case's file (Cases/CrashReport.lua)");
    Report(Has("MODS", "bg3mse in-game tests"), "the mod list names the test mod");
    auto bt = ReadFile(stem + ".bt");
    Report(bt.has_value() && !bt->empty(), "the backtrace text (.bt)");
    Report(bt.has_value() && bt->find("Threads (") != std::string::npos, "the other threads in the backtrace text");
    struct stat st{};
    Report(stat((stem + ".stack").c_str(), &st) == 0 && st.st_size > 0,
        "the stack file (.stack, " + std::to_string((long long)st.st_size) + " bytes)");

    // The crashed launch's log names the report; the next launch's tells of it.
    bool written = false, told = false;
    for (auto const& path : logs) {
        auto text = ReadFile(path);
        if (!text) {
            printf("cannot read %s\n", path.c_str());
            return 2;
        }
        if (text->find("crash report written to") != std::string::npos && text->find(newest) != std::string::npos) written = true;
        if (text->find("The game crashed last time. Crash report:") != std::string::npos
            && text->find(newest) != std::string::npos) {
            told = true;
        }
    }
    Report(written, "the crashed launch's log names the report");
    Report(told, "a later launch's log tells of the crash (\"crashed the last time it ran\") and names the report");

    if (!tool.empty()) {
        std::string command = "'" + tool + "' '" + stem + ".crpt' '" + dsym + "' 2>&1";
        std::string output;
        if (FILE* p = popen(command.c_str(), "r")) {
            char buffer[4096];
            size_t n;
            while ((n = fread(buffer, 1, sizeof(buffer), p)) > 0) output.append(buffer, n);
            pclose(p);
        }
        // Ext.Debug.Crash is bg3se::lua::debug::Crash(int) (Lua/Libs/Debug.inl).
        Report(output.find("debug::Crash(int)") != std::string::npos,
            "bg3mse-symbolicate names bg3se::lua::debug::Crash(int) with the dSYM");
    }
    printf("%s\n", missing == 0 ? "complete" : "incomplete");
    return missing == 0 ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    if (argc >= 2) {
        std::string command = argv[1];
        if (command == "lua-check") return LuaCheck(argc - 2, argv + 2);
        if (command == "pack") return Pack(argc - 2, argv + 2);
        if (command == "pak-compare") return PakCompare(argc - 2, argv + 2);
        if (command == "check") return Check(argc - 2, argv + 2);
        if (command == "settings") return Settings(argc - 2, argv + 2);
        if (command == "crash-check") return CrashCheck(argc - 2, argv + 2);
    }
    fprintf(stderr, "usage: %s lua-check FILE... | pack ROOT OUT.pak | pak-compare ROOT PAK | check LOG..."
        " | settings LOG... | crash-check REPORTS LOG... [--symbolicate TOOL DSYM]\n", argv[0]);
    return 2;
}
