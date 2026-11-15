#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <Platform/CodeShape.h>
#include <Platform/Symbols.h>

// The one hook on ls::FileReader::FileReader(Path const&, EType, EFileFlags),
// the constructor every file the game opens goes through. Whoever needs to
// serve a file from memory subscribes here.
//
// A function takes one entry hook, and Norbyte's extender wants one on this
// constructor too (GameHooks/EngineHooks.inl, FileReader__ctor: the path
// overrides of Ext.IO). The two share it like this. The game's constructor
// carries this layer's hook, whose function has the constructor's own
// prototype: it runs the game's constructor and then asks the subscriber.
// That function (FileReaderHookFunction) is what the extender is given as
// "the constructor", to call and to hook:
//
//     the game, or the extender, opens a file
//       -> this layer's hook function            (its first instruction is where the extender's hook goes)
//            -> the extender's wrapper           (only once it is attached: it may change the path)
//                 -> the hook function's body
//                      -> the game's constructor, with the path as it now is
//                      -> the subscriber, with that same path
//
// So the extender sees every open first, as its hook on the constructor does
// on Windows, and the subscriber is asked about the file that is really
// opened. Until the extender attaches, and when it never does, the hook
// function runs straight through. Neither side can reach itself again: the
// hook function calls the game's constructor through its trampoline, and the
// extender's wrapper calls the hook function through the trampoline of its
// own hook. Each of the two hooks changes one instruction word, of a
// different function, and goes in and out on its own.

namespace bg3se::mac {

class FileReaderSubscriber
{
public:
    virtual ~FileReaderSubscriber() = default;

    // Called on the opening thread after the game's constructor has run.
    // Return the bytes the game should read for `path`, or an empty span to
    // leave the reader as the game built it. The bytes must stay valid for the
    // life of the process. Called for every file the game opens: keep it cheap.
    virtual std::span<uint8_t const> OnFileOpened(std::string_view path) = 0;
};

// The shapes of the constructor and of FileReader::Read for which the layout
// in GameDefinitions/FileReader.h was checked.
struct FileReaderShape
{
    CodeShape Constructor;
    CodeShape Read;
};
extern FileReaderShape const kFileReaderShape;

struct FileReaderCheck
{
    void const* Constructor{ nullptr };
    uint64_t ConstructorBytes{ 0 };     // up to the next symbol
    FileReaderShape Found{};
    std::string Problem;        // empty when the hook can be installed

    bool Ok() const { return Problem.empty(); }
};

// Resolves the two functions in `image`, compares their shapes, and asks the
// entry hook planner whether the constructor can be hooked. Runs on the
// loaded game and on a game binary mapped from disk alike.
FileReaderCheck CheckFileReader(GameImage const& image);

// Hooks the constructor in the running game (Platform/EntryHook.h). Returns
// false, with the reason logged, when the check or the hook is refused.
bool InstallFileReaderHook(FileReaderSubscriber* subscriber);

// The function this layer's hook runs in place of the game's constructor,
// once the hook is installed; nullptr before, and when it was refused (the
// game's constructor then carries no hook, and is itself the constructor to
// call and to hook).
void* FileReaderHookFunction();

uint64_t FileReaderOpenCount();

// For tests: installs the hook on `constructor`, a function of the
// constructor's prototype that is `bytes` long, without the checks of the
// game's code. False when the entry hook is refused.
bool InstallFileReaderHookOn(void* constructor, uint64_t bytes, FileReaderSubscriber* subscriber);

}
