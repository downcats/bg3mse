#include <stdafx.h>
#include <Shaders/TranslationCache.h>

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <lz4.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace bg3se::mac {

namespace {

constexpr std::string_view kMagic = "bg3mse translated material 1\n";
constexpr std::string_view kEntryExt = ".translated";
constexpr std::string_view kPartialExt = ".partial";
constexpr std::string_view kMaterialExt = ".lsf";
// Nothing translation makes is near this; a size beyond it is not an entry's.
constexpr size_t kLargestEntry = 256u << 20;

// What a Metal function can be called: letters, digits and underscores.
bool IsFunctionName(std::string_view name)
{
    return !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

struct Hasher
{
    CC_SHA256_CTX Context;
    Hasher() { CC_SHA256_Init(&Context); }
    void Add(void const* data, size_t size)
    {
        // In pieces the 32-bit length of the system's function takes.
        auto const* at = static_cast<uint8_t const*>(data);
        while (size) {
            CC_LONG piece = (CC_LONG)std::min<size_t>(size, 1u << 30);
            CC_SHA256_Update(&Context, at, piece);
            at += piece;
            size -= piece;
        }
    }
    // With its length, so that two texts never run into each other.
    void AddText(std::string_view text)
    {
        uint64_t size = text.size();
        Add(&size, sizeof(size));
        Add(text.data(), text.size());
    }
    ContentHash Done()
    {
        ContentHash hash;
        CC_SHA256_Final(hash.data(), &Context);
        return hash;
    }
};

struct Writer
{
    std::vector<uint8_t> Bytes;

    void Raw(void const* data, size_t size) { Bytes.insert(Bytes.end(), static_cast<uint8_t const*>(data), static_cast<uint8_t const*>(data) + size); }
    void U32(uint32_t value) { Raw(&value, sizeof(value)); }
    void Blob(std::span<uint8_t const> blob) { U32((uint32_t)blob.size()); Raw(blob.data(), blob.size()); }
    void Text(std::string_view text) { U32((uint32_t)text.size()); Raw(text.data(), text.size()); }
    void Hash(ContentHash const& hash) { Raw(hash.data(), hash.size()); }
};

// Every read is checked against what is left: a file is anyone's to have written.
struct Reader
{
    std::span<uint8_t const> Left;
    bool Ok{ true };

    std::span<uint8_t const> Raw(size_t size)
    {
        if (!Ok || size > Left.size()) { Ok = false; return {}; }
        auto out = Left.first(size);
        Left = Left.subspan(size);
        return out;
    }
    uint32_t U32()
    {
        auto raw = Raw(4);
        uint32_t value = 0;
        if (Ok) memcpy(&value, raw.data(), 4);
        return value;
    }
    std::vector<uint8_t> Blob()
    {
        auto raw = Raw(U32());
        return { raw.begin(), raw.end() };
    }
    std::string Text()
    {
        auto raw = Raw(U32());
        return { (char const*)raw.data(), raw.size() };
    }
    ContentHash Hash()
    {
        auto raw = Raw(32);
        ContentHash hash{};
        if (Ok) memcpy(hash.data(), raw.data(), 32);
        return hash;
    }
    // A count of things of at least `each` bytes: one that cannot fit is not a count.
    uint32_t Count(size_t each)
    {
        uint32_t count = U32();
        if (Ok && (size_t)count > Left.size() / each) Ok = false;
        return Ok ? count : 0;
    }
};

// Which files the package has under the material's name: a pass that is added or taken away makes another
// translation though no file that was read has changed.
ContentHash NamesOf(MaterialSource const& mod, std::string const& material)
{
    std::string_view stem(material);
    if (stem.ends_with(kMaterialExt)) stem.remove_suffix(kMaterialExt.size());
    std::vector<std::string const*> names;
    for (auto const& name : mod.Names) {
        if (std::string_view(name).starts_with(stem)) names.push_back(&name);
    }
    std::sort(names.begin(), names.end(), [](auto a, auto b) { return *a < *b; });
    Hasher hasher;
    for (auto name : names) hasher.AddText(*name);
    return hasher.Done();
}

std::optional<std::vector<uint8_t>> ReadWhole(std::string const& path)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    struct stat st{};
    std::optional<std::vector<uint8_t>> out;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 && (size_t)st.st_size <= kLargestEntry) {
        std::vector<uint8_t> bytes((size_t)st.st_size);
        size_t got = 0;
        while (got < bytes.size()) {
            ssize_t n = read(fd, bytes.data() + got, bytes.size() - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        if (got == bytes.size()) out = std::move(bytes);
    }
    close(fd);
    return out;
}

bool WriteWhole(std::string const& path, std::span<uint8_t const> bytes)
{
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    size_t put = 0;
    while (put < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + put, bytes.size() - put);
        if (n <= 0) break;
        put += (size_t)n;
    }
    bool ok = close(fd) == 0 && put == bytes.size();
    if (!ok) unlink(path.c_str());
    return ok;
}

void MakeDirectories(std::string const& directory)
{
    for (size_t at = 1; at <= directory.size(); at++) {
        if (at == directory.size() || directory[at] == '/') mkdir(directory.substr(0, at).c_str(), 0755);
    }
}

}

ContentHash HashOf(std::span<uint8_t const> bytes)
{
    Hasher hasher;
    hasher.Add(bytes.data(), bytes.size());
    return hasher.Done();
}

std::string HexOf(std::span<uint8_t const> bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

RecordedSource::RecordedSource(MaterialSource const& mod) : notes_(std::make_shared<Notes>())
{
    source_.Label = mod.Label;
    source_.Names = mod.Names;
    source_.Read = [read = mod.Read, notes = notes_](std::string const& name) -> std::optional<std::vector<uint8_t>> {
        auto bytes = read(name);
        TranslationInput input;
        input.Name = name;
        input.Readable = bytes.has_value();
        if (bytes) input.Hash = HashOf(*bytes);
        std::lock_guard lock(notes->Mutex);
        auto before = std::find_if(notes->Inputs.begin(), notes->Inputs.end(), [&](auto const& i) { return i.Name == name; });
        if (before == notes->Inputs.end()) notes->Inputs.push_back(std::move(input));
        // A file that reads otherwise the second time has no one content to keep a translation by.
        else if (before->Readable != input.Readable || before->Hash != input.Hash) notes->Steady = false;
        return bytes;
    };
}

std::optional<std::vector<TranslationInput>> RecordedSource::Inputs() const
{
    std::lock_guard lock(notes_->Mutex);
    if (!notes_->Steady) return {};
    return notes_->Inputs;
}

TranslationCache::TranslationCache(std::string directory, std::string madeWith, uint64_t limit)
    : directory_(std::move(directory)), madeWith_(std::move(madeWith)), limit_(limit)
{
    MakeDirectories(directory_);
}

std::string TranslationCache::PathOf(std::string const& package, std::string const& material) const
{
    // Of the package and the material alone: what an entry was made with is inside it, so one made with
    // something else is found, and replaced.
    Hasher hasher;
    hasher.AddText(package);
    hasher.AddText(material);
    auto hash = hasher.Done();
    return directory_ + "/" + HexOf(std::span(hash).first(16)) + std::string(kEntryExt);
}

std::optional<KeptTranslation> TranslationCache::Find(MaterialSource const& mod, std::string const& material)
{
    std::string path = PathOf(mod.Label, material);
    auto file = ReadWhole(path);
    if (!file) return {};

    auto drop = [&](std::atomic<uint32_t>& counter) -> std::optional<KeptTranslation> {
        counter.fetch_add(1, std::memory_order_relaxed);
        unlink(path.c_str());
        return {};
    };

    // Whole, and as it was written: it ends in the hash of everything before.
    if (file->size() < kMagic.size() + 32 || memcmp(file->data(), kMagic.data(), kMagic.size()) != 0) return drop(damaged_);
    std::span<uint8_t const> body(file->data(), file->size() - 32);
    if (memcmp(HashOf(body).data(), file->data() + body.size(), 32) != 0) return drop(damaged_);

    Reader in{ body.subspan(kMagic.size()) };
    std::string madeWith = in.Text(), package = in.Text(), name = in.Text();
    ContentHash names = in.Hash();
    std::vector<TranslationInput> inputs(in.Count(4 + 1 + 32));
    for (auto& input : inputs) {
        input.Name = in.Text();
        auto readable = in.Raw(1);
        input.Readable = in.Ok && readable[0] != 0;
        input.Hash = in.Hash();
    }
    uint32_t rawSize = in.U32(), packedSize = in.U32();
    auto packed = in.Raw(packedSize);
    if (!in.Ok || !in.Left.empty() || rawSize > kLargestEntry) return drop(damaged_);

    // Made from what is here now.
    if (madeWith != madeWith_ || package != mod.Label || name != material || names != NamesOf(mod, material)) return drop(stale_);
    for (auto const& input : inputs) {
        auto bytes = mod.Read(input.Name);
        if (bytes.has_value() != input.Readable || (bytes && HashOf(*bytes) != input.Hash)) return drop(stale_);
    }

    std::vector<uint8_t> raw(rawSize);
    if (LZ4_decompress_safe((char const*)packed.data(), (char*)raw.data(), (int)packed.size(), (int)raw.size()) != (int)rawSize) return drop(damaged_);
    Reader payload{ raw };
    KeptTranslation kept;
    kept.Lsf = payload.Blob();
    kept.Stubs.resize(payload.Count(8));
    for (auto& [pass, stub] : kept.Stubs) {
        pass = payload.Text();
        stub = payload.Blob();
    }
    kept.Functions.resize(payload.Count(8));
    bool named = true;
    for (auto& function : kept.Functions) {
        function.Name = payload.Text();
        function.Source = payload.Text();
        named = named && IsFunctionName(function.Name);
    }
    kept.Note = payload.Text();
    // A name that is no function's name is not one this code wrote: the entry is not what it says it is.
    if (!named) return drop(damaged_);
    if (!payload.Ok || !payload.Left.empty() || kept.Lsf.empty() || kept.Stubs.empty() || kept.Functions.empty()) return drop(damaged_);

    // Used now: the last to go when the directory is trimmed.
    utimes(path.c_str(), nullptr);
    found_.fetch_add(1, std::memory_order_relaxed);
    return kept;
}

bool TranslationCache::Keep(MaterialSource const& mod, std::string const& material, std::span<TranslationInput const> inputs,
    KeptTranslation const& translation)
{
    Writer payload;
    payload.Blob(translation.Lsf);
    payload.U32((uint32_t)translation.Stubs.size());
    for (auto const& [pass, stub] : translation.Stubs) {
        payload.Text(pass);
        payload.Blob(stub);
    }
    payload.U32((uint32_t)translation.Functions.size());
    for (auto const& function : translation.Functions) {
        payload.Text(function.Name);
        payload.Text(function.Source);
    }
    payload.Text(translation.Note);
    if (payload.Bytes.size() > kLargestEntry) return false;

    std::vector<char> packed((size_t)LZ4_compressBound((int)payload.Bytes.size()));
    int packedSize = LZ4_compress_default((char const*)payload.Bytes.data(), packed.data(), (int)payload.Bytes.size(), (int)packed.size());
    if (packedSize <= 0) return false;

    Writer out;
    out.Raw(kMagic.data(), kMagic.size());
    out.Text(madeWith_);
    out.Text(mod.Label);
    out.Text(material);
    out.Hash(NamesOf(mod, material));
    out.U32((uint32_t)inputs.size());
    for (auto const& input : inputs) {
        out.Text(input.Name);
        uint8_t readable = input.Readable ? 1 : 0;
        out.Raw(&readable, 1);
        out.Hash(input.Hash);
    }
    out.U32((uint32_t)payload.Bytes.size());
    out.U32((uint32_t)packedSize);
    out.Raw(packed.data(), (size_t)packedSize);
    out.Hash(HashOf(out.Bytes));

    // Written beside and moved into place, under a name no other writer has: a run that ends in the middle
    // leaves no half entry, and two runs at once leave one whole one.
    std::string path = PathOf(mod.Label, material);
    std::string partial = path + "." + std::to_string(getpid()) + "." + std::to_string(written_.fetch_add(1)) + std::string(kPartialExt);
    if (!WriteWhole(partial, out.Bytes)) return false;
    if (rename(partial.c_str(), path.c_str()) != 0) {
        unlink(partial.c_str());
        return false;
    }
    kept_.fetch_add(1, std::memory_order_relaxed);
    Trim();
    return true;
}

void TranslationCache::Forget(std::string const& package, std::string const& material)
{
    unlink(PathOf(package, material).c_str());
}

TranslationCache::Counts TranslationCache::Count() const
{
    return { found_.load(), stale_.load(), damaged_.load(), kept_.load() };
}

void TranslationCache::Trim()
{
    struct Entry
    {
        std::string Path;
        uint64_t Size;
        timespec Used;
    };
    std::vector<Entry> entries;
    uint64_t total = 0;
    time_t now = time(nullptr);
    DIR* dir = opendir(directory_.c_str());
    if (!dir) return;
    while (dirent* e = readdir(dir)) {
        std::string_view name = e->d_name;
        bool entry = name.ends_with(kEntryExt), partial = name.ends_with(kPartialExt);
        if (!entry && !partial) continue;       // not ours: left alone
        std::string path = directory_ + "/" + std::string(name);
        struct stat st{};
        if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (partial) {
            // What a run that ended in the middle left; one being written now is younger than this.
            if (now - st.st_mtimespec.tv_sec > 3600) unlink(path.c_str());
            continue;
        }
        entries.push_back({ std::move(path), (uint64_t)st.st_size, st.st_mtimespec });
        total += (uint64_t)st.st_size;
    }
    closedir(dir);
    if (total <= limit_) return;

    std::sort(entries.begin(), entries.end(), [](Entry const& a, Entry const& b) {
        return a.Used.tv_sec != b.Used.tv_sec ? a.Used.tv_sec < b.Used.tv_sec : a.Used.tv_nsec < b.Used.tv_nsec;
    });
    for (auto const& entry : entries) {
        if (total <= limit_) break;
        if (unlink(entry.Path.c_str()) == 0) total -= entry.Size;
    }
}

}
