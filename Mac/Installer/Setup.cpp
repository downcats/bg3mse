#include <Installer/Setup.h>

#include <Installer/Extras.h>
#include <Installer/Launch.h>
#include <Installer/MachO.h>
#include <Installer/Support.h>
#include <Updater/Stamp.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <copyfile.h>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bg3se::mac::installer {

namespace {

constexpr mode_t kExecutableMode = 0755, kTextMode = 0644;

std::string CameraLicence(char const* name)
{
    return std::string(kCameraDirectory) + "/" + name;
}

// An arm64 image of the kind wanted whose signature holds.
bool CheckImage(std::string const& path, uint32_t fileType, std::string& problem)
{
    Mapped file;
    if (!file.Open(path, problem)) return false;
    if (!file.Present()) {
        problem = path + " is not there";
        return false;
    }
    auto macho = ReadMachO(file.Bytes(), problem);
    if (!macho) {
        problem = path + ": " + problem;
        return false;
    }
    Slice const* arm64 = macho->Find((uint32_t)CPU_TYPE_ARM64);
    if (!arm64 || arm64->FileType != fileType) {
        problem = path + " is not an arm64 " + (fileType == MH_DYLIB ? "dynamic library" : "executable");
        return false;
    }
    auto signature = CheckSignature(file.Bytes(), *arm64);
    if (!signature.HashesMatch) {
        problem = path + ": its code signature does not hold (" + signature.Problem + ")";
        return false;
    }
    return true;
}

// The data directory as a path from the root with no link in it, also
// before it is made: the launch option names the launcher by it.
std::string Resolved(std::string const& data)
{
    return ResolvedPath(data);
}

std::string StampText(std::string const& path)
{
    if (!Exists(path)) return "not there";
    updater::Stamp stamp;
    std::string why;
    if (!updater::ReadStamp(path, stamp, why)) return "there, without bg3mse's version stamp";
    return "bg3mse " + stamp.Version + " (commit " + stamp.Commit.substr(0, 12) + ", Script Extender " + stamp.Extender + ", " + stamp.Channel +
        ")";
}

std::vector<std::string> Entries(std::string const& dir)
{
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string name = e->d_name;
            if (name != "." && name != "..") names.push_back(name);
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Every file under bin/ that is not this installer's, as paths in bin/.
std::vector<std::string> ForeignFiles(std::string const& bin)
{
    auto own = OwnBinFiles();
    std::vector<std::string> found;
    for (auto const& name : Entries(bin)) {
        if (name == kCameraDirectory && IsDirectory(bin + "/" + name)) {
            for (auto const& inner : Entries(bin + "/" + name)) {
                std::string path = name + "/" + inner;
                if (std::find(own.begin(), own.end(), path) == own.end()) found.push_back(path);
            }
        } else if (std::find(own.begin(), own.end(), name) == own.end()) {
            found.push_back(name);
        }
    }
    return found;
}

std::string RunningText(std::vector<std::string> const& games)
{
    std::string text = "the game is running (" + games.front() + "); quit it first";
    return text;
}

}

std::vector<std::string> OwnBinFiles()
{
    std::vector<std::string> names = { kLauncher, kInstallTool, kLibrary, kLoaderLibrary, kCameraLibrary, kReleaseManifest };
    for (auto licence : kCameraLicences) names.push_back(CameraLicence(licence));
    names.push_back(CameraLicence(kCameraDefaultSettings));
    return names;
}

std::string BundleOf(std::string const& executable)
{
    std::string marker = ".app/Contents/MacOS/";
    size_t at = executable.rfind(marker);
    if (at == std::string::npos || executable.find('/', at + marker.size()) != std::string::npos) return {};
    return executable.substr(0, at + 4);
}

Setup::Setup(Options const& options, Host const& host) : o_(options), h_(host) {}

void Setup::Say(std::string const& text) const
{
    fprintf(h_.Out, "%s\n", text.c_str());
}

bool Setup::Plan(std::string& refusal)
{
    if (!h_.AppleSilicon) {
        refusal = "bg3mse requires an Apple Silicon Mac";
        return false;
    }
    if (h_.DataDirectory.empty()) {
        refusal = "there is no home directory to install bg3mse in";
        return false;
    }
    data_ = Resolved(h_.DataDirectory);
    bin_ = data_ + "/" + kBinDirectory;
    previous_ = data_ + "/" + kPreviousDirectory;
    std::string launcher = bin_ + "/" + kLauncher;
    if (launcher.find('"') != std::string::npos) {
        refusal = launcher + " has a double quote in it, which a launch option cannot hold";
        return false;
    }
    line_ = LaunchOption(launcher);
    if (Exists(bin_) && !IsDirectory(bin_)) {
        refusal = bin_ + " is there and is not a folder; move it away, then run bg3mse-install again";
        return false;
    }

    // What the release (or a build directory) holds.
    struct Wanted
    {
        std::string Name;
        std::vector<std::string> From;     // where it may be in --from, in order
        uint32_t Kind;                      // MH_EXECUTE, MH_DYLIB, or 0 for a text
        bool Required;
    };
    std::string from = o_.From;
    std::vector<Wanted> wanted = {
        { kLauncher, { from + "/" + kLauncher }, MH_EXECUTE, true },
        { kInstallTool, { from + "/" + kInstallTool }, MH_EXECUTE, true },
        { kLibrary, { from + "/" + kLibrary }, MH_DYLIB, true },
        { kLoaderLibrary, { from + "/" + kLoaderLibrary }, MH_DYLIB, false },
        { kReleaseManifest, { from + "/" + kReleaseManifest }, 0, false },
    };
    // The camera module (GPL-3.0-or-later) goes only with its licence texts
    // beside it; with them, the settings file it writes when there is none,
    // which bg3mse-install makes the player's from (Extras.h).
    std::vector<std::string> cameraAt = { from + "/" + kCameraDirectory + "/" + kCameraLibrary, from + "/" + kCameraLibrary };
    bool camera = std::any_of(cameraAt.begin(), cameraAt.end(), [](std::string const& p) { return Exists(p); });
    std::string missing;
    for (auto licence : kCameraLicences) {
        if (!Exists(from + "/" + CameraLicence(licence))) missing += std::string(missing.empty() ? "" : ", ") + licence;
    }
    if (!camera) {
        cameraMissing_ = std::string("the release in ") + from + " has no camera module (" + kCameraLibrary + ")";
    } else if (!missing.empty()) {
        cameraMissing_ = std::string(kCameraLibrary) + " is not installed without its licence texts";
        notes_.push_back(std::string(kCameraLibrary) + " is left out: its licence texts are not all in " + from + "/" + kCameraDirectory +
                         " (missing: " + missing + "), and it is not installed without them.");
    } else {
        wanted.push_back({ kCameraLibrary, cameraAt, MH_DYLIB, true });
        for (auto licence : kCameraLicences) wanted.push_back({ CameraLicence(licence), { from + "/" + CameraLicence(licence) }, 0, true });
        wanted.push_back({ CameraLicence(kCameraDefaultSettings), { from + "/" + CameraLicence(kCameraDefaultSettings) }, 0, false });
    }

    for (auto const& w : wanted) {
        std::string source;
        for (auto const& candidate : w.From) {
            if (Exists(candidate)) {
                source = candidate;
                break;
            }
        }
        if (source.empty()) {
            if (w.Required) {
                refusal = w.Name + " is not in " + from + " (run the bg3mse-install of the release's folder, or give that folder with --from)";
                return false;
            }
            continue;
        }
        if (w.Kind && !CheckImage(source, w.Kind, refusal)) return false;
        if (!w.Kind) {
            Mapped text;
            if (!text.Open(source, refusal)) return false;
        }
        bool changed = !SameFile(source, bin_ + "/" + w.Name);
        files_.push_back({ w.Name, source, w.Kind ? kExecutableMode : kTextMode, changed });
        swap_ = swap_ || changed;
    }

    // What bin/ has of this installer's that the release has not: it goes
    // with the previous files, so that the launcher does not pick a stale loader.
    for (auto const& name : OwnBinFiles()) {
        bool shipped = std::any_of(files_.begin(), files_.end(), [&](File const& f) { return f.Name == name; });
        if (!shipped && Exists(bin_ + "/" + name)) stale_.push_back(name);
    }
    swap_ = swap_ || !stale_.empty();
    if (swap_) carried_ = ForeignFiles(bin_);
    for (auto const& name : Entries(data_)) {
        if (name.starts_with(kStagingPrefix)) leftovers_.push_back(data_ + "/" + name);
    }

    createData_ = !Exists(data_);
    std::pair<char const*, char const*> settings[] = { { kConfFile, kDefaultConf }, { kExtenderSettingsFile, kDefaultExtenderSettings } };
    for (auto const& [name, text] : settings) {
        (void)text;
        if (!Exists(data_ + "/" + name)) settings_.push_back(name);
    }
    return true;
}

std::vector<std::string> Setup::Changes() const
{
    std::vector<std::string> lines;
    if (createData_) lines.push_back("create " + data_);
    if (swap_) {
        std::string list;
        for (auto const& f : files_) {
            if (f.Changed) list += (list.empty() ? "" : ", ") + f.Name;
        }
        lines.push_back("put bg3mse in " + bin_ + (list.empty() ? "" : " (new or changed: " + list + ")") + ", copied from " + o_.From +
                        "; staged and checked beside it, then swapped in at once");
        if (!stale_.empty()) {
            std::string names;
            for (auto const& s : stale_) names += (names.empty() ? "" : ", ") + s;
            lines.push_back("leave out " + names + ", which " + o_.From + " does not have");
        }
        if (!carried_.empty()) {
            std::string names;
            for (auto const& c : carried_) names += (names.empty() ? "" : ", ") + Quoted(c);
            lines.push_back("keep " + names + " in " + bin_ + ", which are not bg3mse-install's");
        }
        if (Exists(bin_)) lines.push_back("keep the files bin/ holds now in " + previous_ + (Exists(previous_) ? ", in place of the ones there" : ""));
    }
    for (auto const& name : settings_) lines.push_back("create " + data_ + "/" + name + " with the default settings");
    for (auto const& l : leftovers_) lines.push_back("remove " + l + ", left by an interrupted run");
    return lines;
}

bool Setup::Execute(std::string& problem)
{
    if (createData_ && mkdir(data_.c_str(), 0755) != 0 && errno != EEXIST) {
        problem = data_ + ": " + Errno();
        return false;
    }
    DataLock lock(data_);
    if (!lock.Held()) {
        problem = "another bg3mse-install is working in " + data_;
        return false;
    }
    for (auto const& l : leftovers_) RemoveTree(l);

    if (swap_) {
        std::string staging = data_ + "/" + kStagingPrefix + std::to_string(getpid());
        auto fail = [&](std::string why) {
            problem = std::move(why) + "; " + bin_ + " is as it was";
            RemoveTree(staging);
            return false;
        };
        RemoveTree(staging);
        if (mkdir(staging.c_str(), 0755) != 0) return fail(staging + ": " + Errno());
        for (auto const& f : files_) {
            std::string target = staging + "/" + f.Name;
            if (f.Name.find('/') != std::string::npos && mkdir(DirectoryOf(target).c_str(), 0755) != 0 && errno != EEXIST) {
                return fail(DirectoryOf(target) + ": " + Errno());
            }
            Mapped source;
            std::string why;
            if (!source.Open(f.Source, why) || !source.Present()) return fail(why.empty() ? f.Source + " is gone" : why);
            if (!WriteAtomically(target, source.Bytes(), f.Mode, why)) return fail(why);
        }
        for (auto const& c : carried_) {
            std::string target = staging + "/" + c;
            if (c.find('/') != std::string::npos) mkdir(DirectoryOf(target).c_str(), 0755);
            if (copyfile((bin_ + "/" + c).c_str(), target.c_str(), nullptr, COPYFILE_ALL | COPYFILE_RECURSIVE | COPYFILE_CLONE) != 0) {
                return fail("cannot copy " + bin_ + "/" + c + ": " + Errno());
            }
        }
        // Checked as staged: every file the bytes of its source.
        for (auto const& f : files_) {
            if (!SameFile(f.Source, staging + "/" + f.Name)) return fail(staging + "/" + f.Name + " is not the same as " + f.Source);
        }
        SyncDirectory(staging);
        if (IsDirectory(bin_)) {
            // One rename: bin/ is the old set or the new one, never a mix.
            if (renamex_np(staging.c_str(), bin_.c_str(), RENAME_SWAP) != 0) return fail("cannot swap " + staging + " with " + bin_ + ": " + Errno());
            SyncDirectory(data_);
            if (!RemoveTree(previous_) || rename(staging.c_str(), previous_.c_str()) != 0) {
                notes_.push_back("The files bin/ held before are in " + staging + " (they could not be moved to " + previous_ + ").");
            }
        } else if (rename(staging.c_str(), bin_.c_str()) != 0) {
            return fail("cannot move " + staging + " to " + bin_ + ": " + Errno());
        }
        SyncDirectory(data_);
    }

    std::pair<char const*, char const*> settings[] = { { kConfFile, kDefaultConf }, { kExtenderSettingsFile, kDefaultExtenderSettings } };
    for (auto const& [name, text] : settings) {
        if (std::find(settings_.begin(), settings_.end(), name) == settings_.end()) continue;
        std::string path = data_ + "/" + name;
        int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) {
            if (errno == EEXIST) continue;     // made since: it stays as it is
            problem = path + ": " + Errno();
            return false;
        }
        size_t size = strlen(text);
        bool ok = write(fd, text, size) == (ssize_t)size && FullSync(fd);
        close(fd);
        if (!ok) {
            unlink(path.c_str());
            problem = "cannot write " + path;
            return false;
        }
    }
    return true;
}

void Setup::Tell(bool changed) const
{
    if (!o_.DryRun) {
        Say(changed ? "bg3mse is installed in " + bin_ + "." : "bg3mse is already installed in " + bin_ + ".");
        Say("");
    }
    Say("Steam launch option:");
    Say("");
    Say("    " + line_);
    Say("");
    std::string problem;
    if (o_.DryRun) {
        Say("(Dry run, not copied.)");
    } else if (h_.Clipboard && h_.Clipboard(line_, problem)) {
        Say("Copied.");
    } else {
        Say("Couldn't copy it" + (problem.empty() ? std::string() : " (" + problem + ")") + ". Copy it from above.");
    }
    Say("");
    Say("Paste it into Launch Options under Properties > General in Steam, then press Play.");
    Say("");
    Say("Run \"" + bin_ + "/" + kInstallTool + "\" status afterwards to check bg3mse ran.");
}

std::vector<std::string> LaunchStatus(Options const& options, Host const& host, ExtrasPlace const& place, std::string& lastGame)
{
    std::vector<std::string> lines;
    if (host.DataDirectory.empty()) {
        lines.push_back("bg3mse's launch option install: there is no home directory");
        return lines;
    }
    std::string data = Resolved(host.DataDirectory), bin = data + "/" + kBinDirectory, previous = data + "/" + kPreviousDirectory;
    lines.push_back("bg3mse's launch option install: " + bin);
    bool launcher = Exists(bin + "/" + kLauncher), library = Exists(bin + "/" + kLibrary);
    for (auto name : { kLauncher, kInstallTool, kLibrary, kLoaderLibrary }) lines.push_back(std::string("  ") + name + ": " + StampText(bin + "/" + name));
    lines.push_back(std::string("  ") + kCameraLibrary + ": " + StampText(bin + "/" + kCameraLibrary));
    if (Exists(bin + "/" + kCameraLibrary)) {
        bool all = true;
        for (auto licence : kCameraLicences) all = all && Exists(bin + "/" + CameraLicence(licence));
        if (!all) lines.push_back(std::string("    its licence texts are not all in ") + bin + "/" + kCameraDirectory);
    }
    auto foreign = ForeignFiles(bin);
    if (!foreign.empty()) {
        std::string list;
        for (auto const& f : foreign) list += (list.empty() ? "" : ", ") + Quoted(f);
        lines.push_back("  other files in it, not bg3mse-install's (left as they are): " + list);
    }
    if (IsDirectory(previous)) lines.push_back("  the files it held before: " + previous + " (" + kLibrary + ": " + StampText(previous + "/" + kLibrary) + ")");
    std::string builds = data + "/Cache/Builds";
    auto cached = Entries(builds);
    if (!cached.empty()) {
        std::string list;
        for (auto const& c : cached) list += (list.empty() ? "" : ", ") + c;
        lines.push_back("  builds the updater downloaded (in " + builds + ", the updater's own): " + list);
    }
    for (auto name : { kConfFile, kExtenderSettingsFile }) {
        lines.push_back("  " + data + "/" + name + ": " + (Exists(data + "/" + name) ? "there" : "missing"));
    }
    // The optional extras, as the files the game reads at its start have them.
    Extras extras(options, host, data, place.Library, CameraMissingIn(place.Library), std::nullopt);
    for (auto const& line : extras.StatusLines()) lines.push_back("  " + line);
    if (launcher) {
        lines.push_back("  the launch option for Steam: " + LaunchOption(bin + "/" + kLauncher));
    }
    std::string log = LaunchLogPath(data);
    auto last = LastLaunch(log);
    if (last.empty()) {
        lines.push_back("  last start through bg3mse-launch: none recorded in " + log);
    } else {
        lines.push_back("  last start through bg3mse-launch (" + log + "):");
        for (auto const& l : last) lines.push_back("    " + l);
        lastGame = BundleOf(LaunchedGame(last));
    }
    lines.push_back("bg3mse-install can't see Steam's settings. Check Launch Options under Properties > General in Steam.");
    if (launcher) {
        lines.push_back("It should be the launch option above and nothing else.");
    } else {
        lines.push_back("bg3mse-launch is missing from " + bin + ". Steam won't start the game until you clear the launch option or "
                        "install bg3mse again.");
    }
    if (launcher && library) {
        lines.push_back("bg3mse is installed.");
    } else if (launcher || library || Exists(bin)) {
        lines.push_back(std::string(launcher ? kLibrary : kLauncher) + " is missing from " + bin + ". Install bg3mse again.");
    } else {
        lines.push_back("bg3mse isn't installed.");
    }
    return lines;
}

int LaunchUninstall(Options const& options, Host const& host)
{
    auto say = [&](std::string const& text) { fprintf(host.Out, "%s\n", text.c_str()); };
    std::string const clear = "Remember to clear the game's launch options in Steam, or it won't start.";
    if (host.DataDirectory.empty()) {
        say("bg3mse-install: there is no home directory");
        say("Nothing was changed.");
        return 1;
    }
    if (!options.DryRun && host.RunningGames) {
        auto games = host.RunningGames();
        if (!games.empty()) {
            say("bg3mse-install: " + RunningText(games));
            say("Nothing was changed.");
            return 1;
        }
    }
    std::string data = Resolved(host.DataDirectory), bin = data + "/" + kBinDirectory, previous = data + "/" + kPreviousDirectory;
    std::vector<std::string> remove, trees;
    for (auto const& name : OwnBinFiles()) {
        if (Exists(bin + "/" + name)) remove.push_back(bin + "/" + name);
    }
    if (IsDirectory(previous)) trees.push_back(previous);
    // What was read from the game's shaders and made of the mods': of no use without bg3mse, and made again with
    // it. With the interface goes what a read that was stopped while writing left beside it.
    std::string partialStart = std::string(kShaderInterfaceFile) + ".";
    for (auto const& name : Entries(data)) {
        if (name.starts_with(kStagingPrefix)) trees.push_back(data + "/" + name);
        if (name.starts_with(partialStart) && name.ends_with(".partial")) remove.push_back(data + "/" + name);
    }
    if (Exists(data + "/" + kShaderInterfaceFile)) remove.push_back(data + "/" + kShaderInterfaceFile);
    if (IsDirectory(data + "/" + kShaderCacheDirectory)) trees.push_back(data + "/" + kShaderCacheDirectory);
    // What the updater fetched: builds of bg3mse, of no use without it, and fetched again by it. Its manifests
    // stay, unless purged: the last one accepted is what keeps an older signed manifest from being taken again.
    std::string cache = data + "/" + kUpdaterCacheDirectory;
    if (options.Purge) {
        if (IsDirectory(cache)) trees.push_back(cache);
    } else if (IsDirectory(cache)) {
        for (auto name : { "Builds", "Staging" }) {
            if (IsDirectory(cache + "/" + name)) trees.push_back(cache + "/" + name);
        }
        for (auto const& name : Entries(cache)) {
            if (name.starts_with("Failed-")) remove.push_back(cache + "/" + name);
        }
    }
    if (options.Purge) {
        for (auto name : { kConfFile, kExtenderSettingsFile, kCameraSettingsFile }) {
            if (Exists(data + "/" + name)) remove.push_back(data + "/" + name);
        }
    }
    auto foreign = ForeignFiles(bin);
    if (remove.empty() && trees.empty() && !IsDirectory(bin)) {
        say("bg3mse isn't installed.");
        say(clear);
        return 0;
    }
    if (options.DryRun) {
        say("Dry run; nothing is changed. bg3mse-install uninstall would:");
        for (auto const& r : remove) say("  remove " + r);
        for (auto const& t : trees) say("  remove " + t + " and everything in it");
        if (foreign.empty()) say("  remove " + bin);
        say(clear);
        return 0;
    }
    DataLock lock(data);
    if (!lock.Held()) {
        say("bg3mse-install: another bg3mse-install is working in " + data);
        say("Nothing was changed.");
        return 1;
    }
    bool ok = true;
    for (auto const& r : remove) {
        say("  remove " + r);
        if (unlink(r.c_str()) != 0 && errno != ENOENT) {
            say("bg3mse-install: cannot remove " + r + ": " + Errno());
            ok = false;
        }
    }
    for (auto const& t : trees) {
        say("  remove " + t + " and everything in it");
        if (!RemoveTree(t)) {
            say("bg3mse-install: cannot remove all of " + t);
            ok = false;
        }
    }
    rmdir((bin + "/" + kCameraDirectory).c_str());
    if (rmdir(bin.c_str()) == 0) {
        say("  remove " + bin);
    } else if (!foreign.empty()) {
        std::string list;
        for (auto const& f : foreign) list += (list.empty() ? "" : ", ") + Quoted(f);
        say(bin + " stays: it holds files that are not bg3mse-install's: " + list);
    }
    SyncDirectory(data);
    if (!ok) {
        say("Run the same command again once the problem is fixed.");
        return 1;
    }
    say(options.Purge ? "bg3mse is uninstalled, settings removed." : "bg3mse is uninstalled, settings kept.");
    say(clear);
    return 0;
}

}
