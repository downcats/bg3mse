// bg3mse-sign: the update key and the signed channel manifest (Mac/Updater/Manifest.h).
//
//   bg3mse-sign keygen <private key file>
//       Makes a key pair. Writes the private key (32 bytes, mode 0600) to a
//       new file and prints the public key, the value of
//       -DBG3MSE_UPDATER_PUBLIC_KEY=<hex> for the build. Every verb refuses a
//       private key inside a git work tree: it is never in a repository.
//   bg3mse-sign public <private key file>
//       Prints the public key of a private key.
//   bg3mse-sign manifest --key <private key file> --channel <Release|Devel|Nightly> --out <Manifest.json>
//                        [--previous <Manifest.json>] [--notice <text>] [--no-match-notice <text>]
//                        [--add <bg3mse-x.y.z.zip> [--url <url>] [--min-game <v>] [--max-game <v>] [--release-notice <text>]]
//                        [--revoke <x.y.z>]...
//       Writes the channel's next manifest: the previous one (verified with
//       the same key) with its sequence raised by one, the zip added (its
//       size and SHA-256, and what its manifest.json says), the versions
//       given revoked, then signed. --url defaults to ../v<version>/<zip name>,
//       the zip as an asset of the release tagged v<version>, read relative to
//       the manifest's own URL (an asset of the release tagged channel-<channel>).
//   bg3mse-sign verify <public key hex> <Manifest.json>
//       Checks a signed manifest and prints it.

#include <Updater/Files.h>
#include <Updater/Manifest.h>
#include <Updater/Signature.h>
#include <Updater/Zip.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace bg3se::mac::updater;

namespace {

int Usage()
{
    fprintf(stderr,
        "usage: bg3mse-sign keygen <private key file>\n"
        "       bg3mse-sign public <private key file>\n"
        "       bg3mse-sign manifest --key <file> --channel <name> --out <Manifest.json> [--previous <Manifest.json>]\n"
        "                            [--notice <text>] [--no-match-notice <text>]\n"
        "                            [--add <zip> [--url <url>] [--min-game <v>] [--max-game <v>] [--release-notice <text>]]\n"
        "                            [--revoke <version>]...\n"
        "       bg3mse-sign verify <public key hex> <Manifest.json>\n");
    return 2;
}

std::span<uint8_t const> Bytes(std::string const& s)
{
    return { reinterpret_cast<uint8_t const*>(s.data()), s.size() };
}

// Whether the path is inside a git work tree (a directory above it holds
// .git): a private key is kept out of every repository, ignored or not.
bool InsideRepository(std::string const& path, std::string& why)
{
    size_t slash = path.rfind('/');
    std::string dir = slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
    char resolved[PATH_MAX];
    if (!realpath(dir.c_str(), resolved)) {
        why = "cannot find " + dir + ": " + strerror(errno);
        return true;
    }
    for (std::string d = resolved;;) {
        struct stat st{};
        if (lstat((d + "/.git").c_str(), &st) == 0) {
            why = "it is inside the git work tree " + d + "; keep the private key outside every repository";
            return true;
        }
        size_t up = d.rfind('/');
        if (up == std::string::npos || d == "/") return false;
        d = up == 0 ? "/" : d.substr(0, up);
    }
}

bool ReadKey(std::string const& path, PrivateKey& key)
{
    std::string text, why;
    if (InsideRepository(path, why)) {
        fprintf(stderr, "bg3mse-sign: refusing the key %s: %s\n", path.c_str(), why.c_str());
        return false;
    }
    if (ReadFile(path, text, 64) != ReadResult::Ok || text.size() != key.size()) {
        fprintf(stderr, "bg3mse-sign: %s is not a private key (32 bytes)\n", path.c_str());
        return false;
    }
    memcpy(key.data(), text.data(), key.size());
    return true;
}

int Keygen(std::string const& path)
{
    std::string why;
    if (InsideRepository(path, why)) {
        fprintf(stderr, "bg3mse-sign: refusing %s: %s\n", path.c_str(), why.c_str());
        return 1;
    }
    PublicKey pub;
    PrivateKey priv;
    if (!MakeKeyPair(pub, priv)) {
        fprintf(stderr, "bg3mse-sign: no key pair was made\n");
        return 1;
    }
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "bg3mse-sign: cannot create %s: %s\n", path.c_str(), strerror(errno));
        return 1;
    }
    bool ok = write(fd, priv.data(), priv.size()) == (ssize_t)priv.size() && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok) {
        unlink(path.c_str());
        fprintf(stderr, "bg3mse-sign: cannot write %s\n", path.c_str());
        return 1;
    }
    printf("%s\n", Hex(pub).c_str());
    return 0;
}

int Manifest(int argc, char** argv)
{
    std::string keyPath, channel, out, previous, notice, noMatch;
    bool noticeSet = false, noMatchSet = false;
    struct Add
    {
        std::string Zip, URL, MinGame, MaxGame, Notice;
    };
    std::vector<Add> adds;
    std::vector<std::string> revokes;
    for (int i = 0; i < argc; i++) {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "bg3mse-sign: %s needs a value\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        auto lastAdd = [&]() -> Add& {
            if (adds.empty()) {
                fprintf(stderr, "bg3mse-sign: %s comes after --add\n", a.c_str());
                exit(2);
            }
            return adds.back();
        };
        if (a == "--key") keyPath = value();
        else if (a == "--channel") channel = value();
        else if (a == "--out") out = value();
        else if (a == "--previous") previous = value();
        else if (a == "--notice") { notice = value(); noticeSet = true; }
        else if (a == "--no-match-notice") { noMatch = value(); noMatchSet = true; }
        else if (a == "--add") adds.push_back(Add{ .Zip = value(), .URL = {}, .MinGame = {}, .MaxGame = {}, .Notice = {} });
        else if (a == "--url") lastAdd().URL = value();
        else if (a == "--min-game") lastAdd().MinGame = value();
        else if (a == "--max-game") lastAdd().MaxGame = value();
        else if (a == "--release-notice") lastAdd().Notice = value();
        else if (a == "--revoke") revokes.push_back(value());
        else return Usage();
    }
    if (keyPath.empty() || channel.empty() || out.empty()) return Usage();
    PrivateKey key;
    PublicKey pub;
    if (!ReadKey(keyPath, key) || !PublicKeyOf(key, pub)) return 1;

    ChannelManifest m;
    m.Channel = channel;
    std::string why;
    if (!previous.empty()) {
        std::string bytes;
        std::span<uint8_t const> payload;
        if (ReadFile(previous, bytes, 4u << 20) != ReadResult::Ok || !Verify(Bytes(bytes), pub, payload, why)) {
            fprintf(stderr, "bg3mse-sign: the previous manifest %s does not verify with this key: %s\n", previous.c_str(), why.c_str());
            return 1;
        }
        if (ParseChannelManifest(std::string_view(reinterpret_cast<char const*>(payload.data()), payload.size()), m, why) != ParseResult::Ok) {
            fprintf(stderr, "bg3mse-sign: the previous manifest cannot be read: %s\n", why.c_str());
            return 1;
        }
        if (m.Channel != channel) {
            fprintf(stderr, "bg3mse-sign: the previous manifest is of the %s channel, not %s\n", m.Channel.c_str(), channel.c_str());
            return 1;
        }
    }
    m.Sequence++;
    if (noticeSet) m.Notice = notice;
    if (noMatchSet) m.NoMatchingVersionNotice = noMatch;

    for (auto const& add : adds) {
        std::string zip;
        if (ReadFile(add.Zip, zip, 1ull << 32) != ReadResult::Ok) {
            fprintf(stderr, "bg3mse-sign: cannot read %s\n", add.Zip.c_str());
            return 1;
        }
        ZipReader reader;
        if (!reader.Open(Bytes(zip), why)) {
            fprintf(stderr, "bg3mse-sign: %s: %s\n", add.Zip.c_str(), why.c_str());
            return 1;
        }
        ZipEntry const* entry = nullptr;
        for (auto const& e : reader.Entries()) {
            size_t slash = e.Name.find('/');
            if (slash != std::string::npos && e.Name.substr(slash + 1) == "manifest.json") entry = &e;
        }
        std::string text;
        if (!entry) {
            fprintf(stderr, "bg3mse-sign: %s has no manifest.json\n", add.Zip.c_str());
            return 1;
        }
        bool read = reader.Read(*entry, text, why);
        ReleaseManifest rm;
        if (!read || !ParseReleaseManifest(text, rm, why)) {
            fprintf(stderr, "bg3mse-sign: %s: %s\n", add.Zip.c_str(), why.c_str());
            return 1;
        }
        if (rm.Channel != "Release" && channel == "Release") {
            fprintf(stderr, "bg3mse-sign: %s is a %s build; the Release channel takes Release builds only\n", add.Zip.c_str(), rm.Channel.c_str());
            return 1;
        }
        Release r;
        r.ReleaseVersion = rm.ReleaseVersion;
        r.Commit = rm.Commit;
        r.ScriptExtenderVersion = rm.ScriptExtenderVersion;
        r.GameBuildChecked = rm.GameBuildChecked;
        r.Size = zip.size();
        r.Digest = Hex(Sha256(Bytes(zip)));
        r.ManifestDigest = Hex(Sha256(Bytes(text)));
        r.Architecture = rm.Architecture;
        auto macos = Version::Parse(rm.MacOSMinimum);
        if (!macos) {
            fprintf(stderr, "bg3mse-sign: %s's manifest.json has no macos_minimum\n", add.Zip.c_str());
            return 1;
        }
        r.MacOSMinimum = *macos;
        std::string zipName = add.Zip.substr(add.Zip.rfind('/') == std::string::npos ? 0 : add.Zip.rfind('/') + 1);
        r.URL = add.URL.empty() ? "../v" + r.ReleaseVersion.ToString() + "/" + zipName : add.URL;
        if (!add.MinGame.empty()) r.MinGameVersion = Version::Parse(add.MinGame);
        if (!add.MaxGame.empty()) r.MaxGameVersion = Version::Parse(add.MaxGame);
        if ((!add.MinGame.empty() && !r.MinGameVersion) || (!add.MaxGame.empty() && !r.MaxGameVersion)) {
            fprintf(stderr, "bg3mse-sign: --min-game and --max-game take versions\n");
            return 1;
        }
        r.BuildDate = (uint64_t)time(nullptr);
        r.Notice = add.Notice;
        for (auto const& other : m.Releases) {
            if (other.ReleaseVersion == r.ReleaseVersion) {
                fprintf(stderr, "bg3mse-sign: version %s is listed already; a release is never replaced, revoke it and release another\n",
                    r.ReleaseVersion.ToString().c_str());
                return 1;
            }
        }
        m.Releases.push_back(std::move(r));
    }
    for (auto const& v : revokes) {
        auto version = Version::Parse(v);
        bool found = false;
        for (auto& r : m.Releases) {
            if (version && r.ReleaseVersion == *version) {
                r.Revoked = true;
                found = true;
            }
        }
        if (!found) {
            fprintf(stderr, "bg3mse-sign: no release %s to revoke\n", v.c_str());
            return 1;
        }
    }

    std::string json = StringifyChannelManifest(m);
    ChannelManifest check;
    if (ParseChannelManifest(json, check, why) != ParseResult::Ok) {
        fprintf(stderr, "bg3mse-sign: the manifest written would not be read: %s\n", why.c_str());
        return 1;
    }
    std::vector<uint8_t> signedBytes;
    if (!Sign(Bytes(json), key, signedBytes) || !WriteFileAtomically(out, signedBytes, why)) {
        fprintf(stderr, "bg3mse-sign: cannot sign or write %s: %s\n", out.c_str(), why.c_str());
        return 1;
    }
    printf("%s: the %s channel, sequence %llu, %zu releases\n", out.c_str(), channel.c_str(), (unsigned long long)m.Sequence, m.Releases.size());
    return 0;
}

int VerifyFile(std::string const& hex, std::string const& path)
{
    PublicKey pub;
    if (!FromHex(hex, pub)) {
        fprintf(stderr, "bg3mse-sign: the public key is 128 hexadecimal digits\n");
        return 2;
    }
    std::string bytes, why;
    std::span<uint8_t const> payload;
    if (ReadFile(path, bytes, 4u << 20) != ReadResult::Ok || !Verify(Bytes(bytes), pub, payload, why)) {
        fprintf(stderr, "bg3mse-sign: %s does not verify: %s\n", path.c_str(), why.c_str());
        return 1;
    }
    fwrite(payload.data(), 1, payload.size(), stdout);
    return 0;
}

}

int main(int argc, char** argv)
{
    if (argc < 2) return Usage();
    std::string verb = argv[1];
    if (verb == "keygen" && argc == 3) return Keygen(argv[2]);
    if (verb == "public" && argc == 3) {
        PrivateKey key;
        PublicKey pub;
        if (!ReadKey(argv[2], key) || !PublicKeyOf(key, pub)) return 1;
        printf("%s\n", Hex(pub).c_str());
        return 0;
    }
    if (verb == "manifest") return Manifest(argc - 2, argv + 2);
    if (verb == "verify" && argc == 4) return VerifyFile(argv[2], argv[3]);
    return Usage();
}
