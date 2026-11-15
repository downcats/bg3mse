#include <Overlay/ObjcClasses.h>

#include <cstring>

// The class data as the compiler lays it out for the 64-bit Objective-C
// runtime (objc4's objc-runtime-new.h): what the linker wrote, before the
// runtime has touched it.
//
//   class            isa, superclass, cache (16 bytes), data: a pointer to the
//                    read-only data, whose low 3 bits are flags
//   read-only data   flags, instance start, instance size, reserved (32 bits
//                    each), ivar layout, name, methods, protocols, variables
//   method list      entry size and flags (32 bits), count (32 bits), entries.
//                    With the flag 0x80000000 an entry is three 32-bit offsets,
//                    each from its own address: to a selector reference (a
//                    pointer to the selector's name), to the type encoding, to
//                    the implementation. Without it, three pointers: name,
//                    type encoding, implementation.
//   variable list    entry size (32 bits), count (32 bits), entries of: a
//                    pointer to the 32-bit offset variable, name, type
//                    encoding, alignment and size (32 bits each)

namespace bg3se::mac::overlay {

namespace {

constexpr uint64_t kClassData = 0x20;
constexpr uint64_t kDataName = 0x18, kDataMethods = 0x20, kDataVariables = 0x30;
constexpr uint32_t kRelativeMethods = 0x80000000, kMethodEntrySize = 0x0000fffc;
constexpr uint32_t kMaxEntries = 4096, kMaxString = 512;

template <class T>
std::optional<T> Read(GameImage const& image, uint64_t va)
{
    auto data = image.DataAt(va, sizeof(T));
    if (!data) return {};
    T value;
    memcpy(&value, data, sizeof(T));
    return value;
}

// The zero-terminated string at `va`; nothing when it runs out of the image
// or past any length a name or type encoding has.
std::optional<std::string> ReadString(GameImage const& image, uint64_t va)
{
    std::string text;
    for (uint32_t i = 0; i < kMaxString; i++) {
        auto c = static_cast<char const*>(image.DataAt(va + i, 1));
        if (!c) return {};
        if (!*c) return text;
        text.push_back(*c);
    }
    return {};
}

bool StringIs(GameImage const& image, uint64_t va, char const* expected)
{
    auto text = va ? ReadString(image, va) : std::nullopt;
    return text && *text == expected;
}

// The address a 32-bit offset stored at `va` points to.
uint64_t RelativeTarget(GameImage const& image, uint64_t va)
{
    auto offset = Read<int32_t>(image, va);
    return offset ? va + (int64_t)*offset : 0;
}

}

uint64_t ObjcClassesOfFile::ReadOnlyData(char const* name) const
{
    if (!image_.FromFile()) return 0;       // a loaded class's data is the runtime's by now
    uint64_t cls = image_.FindVA((std::string("_OBJC_CLASS_$_") + name).c_str());
    if (!cls) return 0;
    auto data = image_.PointerAt(cls + kClassData);
    if (!data) return 0;
    uint64_t readOnly = *data & ~7ull;
    auto nameVA = image_.PointerAt(readOnly + kDataName);
    // The data must be this class's: its name is the name asked for.
    if (!nameVA || !StringIs(image_, *nameVA, name)) return 0;
    return readOnly;
}

bool ObjcClassesOfFile::HasClass(char const* name) const
{
    return ReadOnlyData(name) != 0;
}

ObjcMethod ObjcClassesOfFile::InstanceMethod(char const* className, char const* selector) const
{
    uint64_t readOnly = ReadOnlyData(className);
    auto list = readOnly ? image_.PointerAt(readOnly + kDataMethods) : std::nullopt;
    if (!list || !*list) return {};
    auto flags = Read<uint32_t>(image_, *list);
    auto count = Read<uint32_t>(image_, *list + 4);
    if (!flags || !count || *count > kMaxEntries) return {};
    bool relative = *flags & kRelativeMethods;
    uint32_t size = *flags & kMethodEntrySize;
    if (size != (relative ? 12u : 24u)) return {};

    for (uint32_t i = 0; i < *count; i++) {
        uint64_t entry = *list + 8 + (uint64_t)i * size;
        uint64_t name = 0, types = 0, implementation = 0;
        if (relative) {
            uint64_t reference = RelativeTarget(image_, entry);
            auto named = reference ? image_.PointerAt(reference) : std::nullopt;
            name = named ? *named : 0;
            types = RelativeTarget(image_, entry + 4);
            implementation = RelativeTarget(image_, entry + 8);
        } else {
            name = image_.PointerAt(entry).value_or(0);
            types = image_.PointerAt(entry + 8).value_or(0);
            implementation = image_.PointerAt(entry + 16).value_or(0);
        }
        if (!StringIs(image_, name, selector)) continue;
        ObjcMethod method;
        method.Found = true;
        method.Own = true;
        method.Implementation = implementation;
        if (auto text = types ? ReadString(image_, types) : std::nullopt) method.Types = *text;
        return method;
    }
    return {};
}

ObjcVariable ObjcClassesOfFile::InstanceVariable(char const* className, char const* name) const
{
    uint64_t readOnly = ReadOnlyData(className);
    auto list = readOnly ? image_.PointerAt(readOnly + kDataVariables) : std::nullopt;
    if (!list || !*list) return {};
    auto size = Read<uint32_t>(image_, *list);
    auto count = Read<uint32_t>(image_, *list + 4);
    if (!size || !count || *size != 32 || *count > kMaxEntries) return {};

    for (uint32_t i = 0; i < *count; i++) {
        uint64_t entry = *list + 8 + (uint64_t)i * 32;
        if (!StringIs(image_, image_.PointerAt(entry + 8).value_or(0), name)) continue;
        ObjcVariable variable;
        variable.Found = true;
        if (auto type = image_.PointerAt(entry + 16); type && *type) {
            if (auto text = ReadString(image_, *type)) variable.Type = *text;
        }
        // The offset variable holds the offset the compiler computed; the
        // runtime may move it when a superclass has grown.
        if (auto holder = image_.PointerAt(entry); holder && *holder) {
            if (auto offset = Read<int32_t>(image_, *holder)) variable.Offset = *offset;
        }
        return variable;
    }
    return {};
}

std::optional<bool> ObjcClassesOfFile::DerivesFrom(char const* className, char const* ancestor) const
{
    if (!HasClass(className)) return false;
    if (!strcmp(className, ancestor)) return true;
    return {};      // the superclass is bound when the game is loaded
}

}
