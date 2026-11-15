#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <Platform/Symbols.h>

// What the overlay needs to know of an Objective-C class of the game before
// it replaces one of its methods: that the class has the method, with the
// type encoding the replacement was written for, and that the method's code
// is the code the engine table checked.
//
// Two sources say it: the Objective-C runtime in the running game, and the
// class data of the game binary on disk, so that the check tool and the tests
// say what the game will say. The file cannot name a class's superclass (it
// is bound when the game is loaded): that is known in the running game only.

namespace bg3se::mac::overlay {

struct ObjcMethod
{
    bool Found{ false };
    // The class's own list holds the method (it is not inherited).
    bool Own{ false };
    // The implementation: an address of this process from the runtime, a
    // link-time address from a file.
    uint64_t Implementation{ 0 };
    std::string Types;
};

struct ObjcVariable
{
    bool Found{ false };
    std::string Type;
    // The variable's offset in an instance; -1 when the source cannot say it.
    int64_t Offset{ -1 };
};

class ObjcClasses
{
public:
    virtual ~ObjcClasses() = default;

    virtual bool HasClass(char const* name) const = 0;
    virtual ObjcMethod InstanceMethod(char const* className, char const* selector) const = 0;
    virtual ObjcVariable InstanceVariable(char const* className, char const* name) const = 0;
    // Whether the class is, or derives from, `ancestor`; nothing when the
    // source cannot tell.
    virtual std::optional<bool> DerivesFrom(char const* className, char const* ancestor) const = 0;
};

// The classes of a game binary mapped from disk, read from its class data:
// the class by its symbol (_OBJC_CLASS_$_<name>), then its read-only data,
// its list of instance methods (with relative or with absolute entries) and
// its instance variables.
class ObjcClassesOfFile : public ObjcClasses
{
public:
    // `image` must have been opened from a file and outlive this.
    explicit ObjcClassesOfFile(GameImage const& image) : image_(image) {}

    bool HasClass(char const* name) const override;
    ObjcMethod InstanceMethod(char const* className, char const* selector) const override;
    ObjcVariable InstanceVariable(char const* className, char const* name) const override;
    std::optional<bool> DerivesFrom(char const* className, char const* ancestor) const override;

private:
    // The class's read-only data, or 0.
    uint64_t ReadOnlyData(char const* name) const;

    GameImage const& image_;
};

// The classes of this process, from the Objective-C runtime.
class ObjcClassesOfRuntime : public ObjcClasses
{
public:
    bool HasClass(char const* name) const override;
    ObjcMethod InstanceMethod(char const* className, char const* selector) const override;
    ObjcVariable InstanceVariable(char const* className, char const* name) const override;
    std::optional<bool> DerivesFrom(char const* className, char const* ancestor) const override;
};

}
