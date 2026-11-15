#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <Shaders/Provision.h>
#include <Shaders/TranslatedFunctions.h>

// Translated materials, kept on disk between runs.
//
// Translating a material and compiling its functions takes the better part of
// a second, and what comes of it depends on nothing but the mod's files, the
// game's Metal interface, the interface table and this build: kept once, it
// is the same the next time the game starts. One file per material holds the
// .lsf that is served, the stubs, and the source of every function with its
// name, together with what they were made from:
//
//   - `madeWith`: everything but the mod's files, as text (this build, the
//     system's, the game's interface, the table as it stands for the game);
//   - the package and the material;
//   - which files the package has under the material's name;
//   - every file translation read from the package, and the hash of what it
//     read (RecordedSource notes them as translation goes).
//
// An entry is used only when all of that is as it is now, checked against the
// package's bytes each time. One that is not is removed, and the material is
// translated afresh: a material of a package has one entry, named by the two,
// and what is made now takes the place of what was made with something else. An entry ends in the hash of everything before it: one
// that was cut short or changed on disk is removed like a stale one. The
// directory is kept under a size, the entries used longest ago going first.

namespace bg3se::mac {

// Where the game keeps them, in bg3mse's data directory.
inline constexpr char kTranslationCacheDirectory[] = "shader-cache";

using ContentHash = std::array<uint8_t, 32>;
ContentHash HashOf(std::span<uint8_t const> bytes);
std::string HexOf(std::span<uint8_t const> bytes);

struct KeptTranslation
{
    std::vector<uint8_t> Lsf;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> Stubs;        // by pass, "ST_FOR"
    std::vector<TranslatedSource> Functions;
    std::string Note;           // what the log says of how it was made
};

// A file translation asked the package for: whether it could be read, and what was read.
struct TranslationInput
{
    std::string Name;
    bool Readable{ false };
    ContentHash Hash{};
};

// A package's files as translation is to read them, with every read noted.
class RecordedSource
{
public:
    explicit RecordedSource(MaterialSource const& mod);

    MaterialSource const& Source() const { return source_; }
    // In the order of the first read of each. Nothing when a file read one way
    // and then another: there is then no one content to keep a translation by.
    std::optional<std::vector<TranslationInput>> Inputs() const;

private:
    struct Notes
    {
        std::mutex Mutex;
        std::vector<TranslationInput> Inputs;
        bool Steady{ true };
    };
    std::shared_ptr<Notes> notes_;
    MaterialSource source_;
};

class TranslationCache
{
public:
    static constexpr uint64_t kDefaultLimit = 256ull << 20;

    // `directory` is made when it is not there; a cache whose directory cannot
    // be made finds nothing and keeps nothing.
    TranslationCache(std::string directory, std::string madeWith, uint64_t limit = kDefaultLimit);

    // What was kept for `material` of `mod`, when it was made from what the
    // package has now.
    std::optional<KeptTranslation> Find(MaterialSource const& mod, std::string const& material);

    // Keeps a translation, in place of what was kept for the material before.
    bool Keep(MaterialSource const& mod, std::string const& material, std::span<TranslationInput const> inputs,
        KeptTranslation const& translation);

    // Removes what is kept for a material.
    void Forget(std::string const& package, std::string const& material);

    std::string const& Directory() const { return directory_; }
    std::string const& MadeWith() const { return madeWith_; }
    // Where a material's entry is.
    std::string PathOf(std::string const& package, std::string const& material) const;

    struct Counts
    {
        uint32_t Found{ 0 };        // entries used
        uint32_t Stale{ 0 };        // entries made from something else: removed
        uint32_t Damaged{ 0 };      // entries that could not be read back: removed
        uint32_t Kept{ 0 };         // entries written
    };
    Counts Count() const;

private:
    // Removes the entries used longest ago until the directory is under the limit.
    void Trim();

    std::string directory_;
    std::string madeWith_;
    uint64_t limit_;
    std::atomic<uint32_t> found_{ 0 }, stale_{ 0 }, damaged_{ 0 }, kept_{ 0 };
    std::atomic<uint32_t> written_{ 0 };
};

}
