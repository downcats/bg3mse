// Tests of Mac/Layout that need no game: the game's variant, the name
// std::variant inside namespace bg3se, and the reading of component
// registrations from code.

#include <Layout/CodeProof.h>
#include <Layout/ComponentRegistry.h>
#include <Layout/GameEntityWorld.h>
#include <Layout/GameFiles.h>
#include <Layout/StdVariant.h>
#include <Layout/VirtualSlots.h>
#include <Layout/Utf16Chars.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "FakeBinary.h"
#include "GameStringNorbyte.h"

namespace {

int gFailures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            gFailures++; \
        } \
    } while (0)

using bg3se::mac::GameVariant;

struct Vector3 { float X, Y, Z; };

// Counts the objects alive, to catch a missing or doubled destructor.
struct Counted
{
    static inline int Alive = 0;
    int Value;

    explicit Counted(int value = 0) : Value(value) { Alive++; }
    Counted(Counted const& other) : Value(other.Value) { Alive++; }
    Counted(Counted&& other) noexcept : Value(other.Value) { other.Value = -1; Alive++; }
    Counted& operator = (Counted const&) = default;
    Counted& operator = (Counted&& other) noexcept { Value = other.Value; other.Value = -1; return *this; }
    ~Counted() { Alive--; }
};

struct ThrowsOnCopy
{
    ThrowsOnCopy() = default;
    ThrowsOnCopy(ThrowsOnCopy const&) { throw 1; }
    ThrowsOnCopy& operator = (ThrowsOnCopy const&) { throw 1; }
};

// --- Layout -------------------------------------------------------------------

// One byte of index directly after storage as large as the largest
// alternative; size rounded up to the largest alignment.
static_assert(sizeof(GameVariant<char>) == 2 && alignof(GameVariant<char>) == 1);
static_assert(sizeof(GameVariant<uint8_t, bool>) == 2);
static_assert(sizeof(GameVariant<int, float>) == 8 && alignof(GameVariant<int, float>) == 4 && GameVariant<int, float>::IndexOffset == 4);
static_assert(sizeof(GameVariant<int, double>) == 16 && GameVariant<int, double>::IndexOffset == 8);
static_assert(sizeof(GameVariant<uint64_t, Vector3>) == 16 && GameVariant<uint64_t, Vector3>::IndexOffset == 12);
static_assert(sizeof(GameVariant<std::array<char, 5>, int>) == 8 && GameVariant<std::array<char, 5>, int>::IndexOffset == 5);
static_assert(sizeof(GameVariant<std::array<uint64_t, 4>, int>) == 0x28 && GameVariant<std::array<uint64_t, 4>, int>::IndexOffset == 0x20);
// libc++'s differs, which is why it cannot be used for the game's.
static_assert(sizeof(std::variant<uint8_t, bool>) != sizeof(GameVariant<uint8_t, bool>));

// As trivial as its alternatives, like std::variant.
static_assert(std::is_trivially_copyable_v<GameVariant<int, float, Vector3>>);
static_assert(std::is_trivially_destructible_v<GameVariant<int, float, Vector3>>);
static_assert(!std::is_trivially_copyable_v<GameVariant<int, std::string>>);
static_assert(!std::is_trivially_destructible_v<GameVariant<int, std::string>>);
static_assert(std::is_copy_constructible_v<GameVariant<int, std::string>> && std::is_copy_assignable_v<GameVariant<int, std::string>>);
static_assert(std::is_nothrow_move_constructible_v<GameVariant<int, std::string>>);
static_assert(!std::is_copy_constructible_v<GameVariant<int, std::unique_ptr<int>>>);
static_assert(std::is_move_constructible_v<GameVariant<int, std::unique_ptr<int>>>);
static_assert(std::is_default_constructible_v<GameVariant<int, std::string>>);

// Which alternative a value selects: as std::variant selects it.
static_assert(std::is_constructible_v<GameVariant<bool, int64_t, double>, int>);
static_assert(std::is_constructible_v<GameVariant<std::string, bool>, char const*>);
static_assert(!std::is_constructible_v<GameVariant<int, float>, char const*>);
static_assert(!std::is_constructible_v<GameVariant<long, long long>, int>);  // two equally good conversions

template <class V>
uint8_t IndexByte(V const& v)
{
    uint8_t byte;
    memcpy(&byte, reinterpret_cast<char const*>(&v) + V::IndexOffset, 1);
    return byte;
}

void TestLayout()
{
    GameVariant<uint64_t, Vector3> v;
    CHECK(v.index() == 0 && IndexByte(v) == 0);
    v = Vector3{ 1, 2, 3 };
    CHECK(v.index() == 1 && IndexByte(v) == 1);
    // The alternative starts at the variant's address.
    CHECK(static_cast<void const*>(&bg3se::mac::get<Vector3>(v)) == static_cast<void const*>(&v));
    CHECK(bg3se::mac::get<Vector3>(v).Z == 3);

    // The game writes the index as one byte and leaves the padding after it alone.
    GameVariant<int, double> w;
    memset(static_cast<void*>(&w), 0xee, sizeof(w));
    new (&w) GameVariant<int, double>(2.5);
    CHECK(IndexByte(w) == 1 && w.index() == 1);
    unsigned char raw[sizeof(w)];
    memcpy(raw, &w, sizeof(w));
    CHECK(raw[9] == 0xee && raw[15] == 0xee);

    // A variant the game wrote: the storage and one index byte.
    alignas(8) unsigned char game[16];
    memset(game, 0xcd, sizeof(game));
    Vector3 position{ 4, 5, 6 };
    memcpy(game, &position, sizeof(position));
    game[12] = 1;
    auto const& read = *reinterpret_cast<GameVariant<uint64_t, Vector3> const*>(game);
    CHECK(read.index() == 1 && bg3se::mac::get<1>(read).Y == 5);
}

void TestValues()
{
    using V = GameVariant<bool, int64_t, double, std::string>;
    V v;
    CHECK(v.index() == 0 && bg3se::mac::get<bool>(v) == false);
    v = 7;                                  // int goes to int64_t: bool takes only bool, double would narrow
    CHECK(v.index() == 1 && bg3se::mac::get<int64_t>(v) == 7);
    v = true;
    CHECK(v.index() == 0);
    v = 1.5;
    CHECK(v.index() == 2);
    v = "text";
    CHECK(v.index() == 3 && bg3se::mac::get<std::string>(v) == "text");
    CHECK(bg3se::mac::holds_alternative<std::string>(v) && !bg3se::mac::holds_alternative<double>(v));
    CHECK(bg3se::mac::get_if<double>(&v) == nullptr && *bg3se::mac::get_if<3>(&v) == "text");

    bool threw = false;
    try {
        (void)bg3se::mac::get<double>(v);
    } catch (std::bad_variant_access const&) {
        threw = true;
    }
    CHECK(threw);

    V copy(v);
    CHECK(copy.index() == 3 && bg3se::mac::get<3>(copy) == "text");
    V moved(std::move(copy));
    CHECK(bg3se::mac::get<3>(moved) == "text");
    copy = 3.25;
    moved = copy;
    CHECK(moved.index() == 2 && bg3se::mac::get<double>(moved) == 3.25);
    moved = std::string(40, 'x');           // longer than the small-string buffer: owns memory
    copy = std::move(moved);
    CHECK(bg3se::mac::get<std::string>(copy).size() == 40);
    copy.emplace<int64_t>(-1);
    CHECK(bg3se::mac::get<1>(copy) == -1);
    copy.emplace<3>(3, 'y');
    CHECK(bg3se::mac::get<3>(copy) == "yyy");

    V const fixed(std::in_place_type<std::string>, "fixed");
    CHECK(bg3se::mac::get<std::string>(fixed) == "fixed");
    static_assert(std::is_same_v<decltype(bg3se::mac::get<3>(fixed)), std::string const&>);
    static_assert(std::is_same_v<decltype(bg3se::mac::get<3>(std::move(copy))), std::string&&>);
}

void TestLifetimes()
{
    using V = GameVariant<int, Counted>;
    {
        V a(Counted(1));
        CHECK(Counted::Alive == 1);
        V b(a);
        CHECK(Counted::Alive == 2 && bg3se::mac::get<Counted>(b).Value == 1);
        V c(std::move(b));
        CHECK(Counted::Alive == 3);
        a = 5;
        CHECK(Counted::Alive == 2 && a.index() == 0);
        a = c;
        CHECK(Counted::Alive == 3);
        c = std::move(a);                   // same alternative: assigned, not rebuilt
        CHECK(Counted::Alive == 3 && bg3se::mac::get<Counted>(c).Value == 1);
        c.emplace<Counted>(9);
        CHECK(Counted::Alive == 3 && bg3se::mac::get<Counted>(c).Value == 9);
        V& self = c;
        c = self;
        CHECK(bg3se::mac::get<Counted>(c).Value == 9);
    }
    CHECK(Counted::Alive == 0);

    // A constructor that throws leaves the variant without a value, with the
    // old one destroyed.
    GameVariant<Counted, ThrowsOnCopy> v(Counted(3));
    ThrowsOnCopy thrower;
    bool threw = false;
    try {
        v.emplace<ThrowsOnCopy>(thrower);
    } catch (int) {
        threw = true;
    }
    CHECK(threw && v.valueless_by_exception() && v.index() == std::variant_npos && Counted::Alive == 0);
    threw = false;
    try {
        bg3se::mac::visit([](auto const&) {}, v);
    } catch (std::bad_variant_access const&) {
        threw = true;
    }
    CHECK(threw);
    v = Counted(4);
    CHECK(v.index() == 0 && Counted::Alive == 1);
    v = Counted(5);
    v.emplace<1>();
    CHECK(Counted::Alive == 0);
}

void TestVisit()
{
    using V = GameVariant<int, std::string, Vector3>;
    auto name = [](V const& v) {
        return bg3se::mac::visit([](auto const& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, int>) return "int " + std::to_string(value);
            else if constexpr (std::is_same_v<T, std::string>) return "string " + value;
            else return "vector";
        }, v);
    };
    CHECK(name(V(3)) == "int 3");
    CHECK(name(V(std::string("s"))) == "string s");
    CHECK(name(V(Vector3{})) == "vector");

    V v(1);
    bg3se::mac::visit([](auto& value) {
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, int>) value = 42;
    }, v);
    CHECK(bg3se::mac::get<int>(v) == 42);

    // An rvalue variant hands its alternative over as an rvalue.
    V s(std::string(40, 'z'));
    std::string taken;
    bg3se::mac::visit([&](auto&& value) {
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::string>) taken = std::move(value);
    }, std::move(s));
    CHECK(taken.size() == 40);
}

void TestNested()
{
    // A variant as an alternative of another, in a container and in an optional.
    using Inner = GameVariant<std::string, int, double, bool, Vector3>;
    using Outer = GameVariant<std::vector<Inner>, Inner>;
    Inner inner(2);
    Outer outer(inner);
    CHECK(outer.index() == 1);
    outer = std::vector<Inner>{ Inner(1.5), Inner(std::string("a")) };
    CHECK(outer.index() == 0 && bg3se::mac::get<0>(outer).size() == 2);
    outer = std::move(inner);
    CHECK(outer.index() == 1 && bg3se::mac::get<int>(bg3se::mac::get<Inner>(outer)) == 2);

    std::optional<Inner> maybe;
    CHECK(!maybe);
    maybe = Inner(true);
    CHECK(maybe && maybe->index() == 3);
    static_assert(bg3se::mac::variant_size_v<Outer> == 2 && bg3se::mac::variant_size_v<Inner const> == 5);
    static_assert(std::is_same_v<bg3se::mac::variant_alternative_t<1, Outer>, Inner>);
    static_assert(std::is_same_v<bg3se::mac::variant_alternative_t<1, Inner const>, int const>);
}

void TestUtf16Chars()
{
    using bg3se::mac::Utf16Chars;
    static_assert(sizeof(Utf16Chars<256>) == 512 && alignof(Utf16Chars<256>) == 2);
    static_assert(std::is_trivially_copyable_v<Utf16Chars<256>> && std::is_standard_layout_v<Utf16Chars<256>>);

    Utf16Chars<8> buffer{};
    CHECK(buffer.Wide().empty());
    CHECK(wcscpy_s(buffer, L"a\u00e9\U0001f600z") == 0);      // one unit, one unit, a surrogate pair, one unit
    CHECK(buffer.Units[0] == u'a' && buffer.Units[1] == 0x00e9 && buffer.Units[2] == 0xd83d && buffer.Units[3] == 0xde00
        && buffer.Units[4] == u'z' && buffer.Units[5] == 0);
    CHECK(buffer.Wide() == L"a\u00e9\U0001f600z");
    std::wstring wide = buffer;
    CHECK(wide.size() == 4 && wide[2] == (wchar_t)0x1f600);

    // Seven units and the terminator fit; eight do not, and the buffer is emptied.
    CHECK(wcscpy_s(buffer, L"1234567") == 0 && buffer.Wide() == L"1234567");
    CHECK(wcscpy_s(buffer, L"12345678") == ERANGE && buffer.Units[0] == 0);
    CHECK(wcscpy_s(buffer, L"123456\U0001f600") == ERANGE);
    // Above U+10FFFF.
    wchar_t bad[] = { (wchar_t)0x110000, 0 };
    CHECK(!buffer.Assign(bad) && buffer.Units[0] == 0);

    // A buffer read from a file: no terminator, and an unpaired surrogate,
    // which is kept and written back as it was.
    Utf16Chars<3> full{ { u'x', 0xdc00, u'y' } };
    CHECK(full.Wide().size() == 3 && full.Wide()[1] == (wchar_t)0xdc00);
    Utf16Chars<4> copy{};
    CHECK(wcscpy_s(copy, full.Wide().c_str()) == 0);
    CHECK(copy.Units[0] == u'x' && copy.Units[1] == 0xdc00 && copy.Units[2] == u'y' && copy.Units[3] == 0);
    Utf16Chars<4> high{ { 0xd800, u'a', 0xd83d, 0 } };
    CHECK(high.Wide() == std::wstring({ (wchar_t)0xd800, L'a', (wchar_t)0xd83d }));

    // Units of any alignment, a zero among them, and back.
    using bg3se::mac::AppendUtf16;
    using bg3se::mac::WideFromUtf16;
    unsigned char bytes[] = { 0xff, 'a', 0, 0, 0, 0x3d, 0xd8, 0x00, 0xde, 0x00, 0xd8 };
    auto read = WideFromUtf16(bytes + 1, 5);
    CHECK(read == std::wstring({ L'a', 0, (wchar_t)0x1f600, (wchar_t)0xd800 }));
    std::u16string units = u"-";
    CHECK(AppendUtf16(read, units));
    CHECK(units.size() == 6 && std::memcmp(units.data() + 1, bytes + 1, 10) == 0);
    CHECK(!AppendUtf16(std::wstring_view(bad, 1), units) && units.size() == 6);
    CHECK(WideFromUtf16(bytes, 0).empty());
}

}

// Code in namespace bg3se, written as Norbyte's is: std::variant is the
// game's variant there, and the rest of std is unchanged.
namespace bg3se {

namespace {

template <class T>
constexpr bool IsVariantLike = false;
template <class... Args>
constexpr bool IsVariantLike<std::variant<Args...>> = true;

template <class... Args>
size_t Alternatives(std::variant<Args...> const&)
{
    return std::variant_size_v<std::variant<Args...>>;
}

static_assert(std::is_same_v<std::variant<int, float>, mac::GameVariant<int, float>>);
static_assert(!std::is_same_v<std::variant<int, float>, ::std::variant<int, float>>);
static_assert(IsVariantLike<std::variant<int, std::string>> && !IsVariantLike<std::optional<int>>);
static_assert(std::is_same_v<std::variant_alternative_t<1, std::variant<int, std::string>>, std::string>);

void TestStdNames()
{
    std::variant<bool, int64_t, double, std::string> value;
    value = int64_t(9);
    CHECK(value.index() == 1 && std::get<int64_t>(value) == 9);
    CHECK(std::holds_alternative<int64_t>(value) && std::get_if<double>(&value) == nullptr);
    CHECK(Alternatives(value) == 4);
    auto text = std::visit([](auto const& v) -> std::string {
        if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::string>) return v;
        else return std::to_string(v);
    }, value);
    CHECK(text == "9");

    // std::get is still std's for a tuple, a pair and an array.
    std::tuple<int, std::string> tuple(1, "t");
    std::pair<int, double> pair(2, 0.5);
    std::array<int, 3> array{ 4, 5, 6 };
    CHECK(std::get<0>(tuple) == 1 && std::get<std::string>(tuple) == "t" && std::get<1>(pair) == 0.5 && std::get<2>(array) == 6);
    // ... and everything else of std is std's.
    std::optional<std::vector<int>> list = std::vector<int>{ 1, 2 };
    CHECK(list && std::move(*list).size() == 2);
    std::unique_ptr<int> owned = std::make_unique<int>(3);
    CHECK(*owned == 3);
}

}

namespace nested {

void TestStdNamesNested()
{
    // The same in a namespace inside bg3se.
    std::variant<uint8_t, float> speed{ 1.0f };
    CHECK(sizeof(speed) == 8 && std::get<float>(speed) == 1.0f);
    static_assert(sizeof(std::variant<uint8_t, bool>) == 2);
}

}

// --- component registrations ---------------------------------------------------

constexpr char kEmplaceEntry[] =
    "__ZN2ls11SparseArrayIN3ecs4core18ComponentTypeEntryENS_16DefaultAllocatorEE11EmplaceImplIJS3_EEERS3_iDpOT_";
constexpr char kHealthIndex[] = "__ZN2ls6TypeIdIN3eoc15HealthComponentEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE";
constexpr char kArmorIndex[] = "__ZN2ls6TypeIdIN3eoc14ArmorComponentEN3ecs22ComponentTypeIdContextEE11m_TypeIndexE";
constexpr char kDiedIndex[] = "__ZN2ls6TypeIdIN3esv5death26DiedEventOneFrameComponentEN3ecs30OneFrameComponentTypeIdContextEE11m_TypeIndexE";

constexpr uint64_t kHealth = FakeBinary::Data + 0x40, kArmor = FakeBinary::Data + 0x44, kDied = FakeBinary::Data + 0x48;
constexpr uint64_t kDiedPointer = FakeBinary::Data + 0x100, kEmplace = FakeBinary::Text + 0xc0;
constexpr uint32_t kEntry = 0x20;       // where the code below builds its entries, from the stack pointer

// The instructions of the game's registrations, assembled.
constexpr uint32_t AdrpData(uint32_t reg) { return 0x90000000u | (3u << 29) | reg; }       // the data's page, from the text's
constexpr uint32_t AddImm(uint32_t rd, uint32_t rn, uint32_t imm) { return 0x91000000u | (imm << 10) | (rn << 5) | rd; }
constexpr uint32_t LoadPointer(uint32_t rt, uint32_t rn, uint32_t offset) { return 0xf9400000u | ((offset / 8) << 10) | (rn << 5) | rt; }
constexpr uint32_t LoadHalf(uint32_t rt, uint32_t rn) { return 0x79400000u | (rn << 5) | rt; }
constexpr uint32_t LoadWord(uint32_t rt, uint32_t rn) { return 0xb9400000u | (rn << 5) | rt; }
constexpr uint32_t AndIndex(uint32_t rd, uint32_t rn) { return 0x12003800u | (rn << 5) | rd; }         // and wd, wn, #0x7fff
constexpr uint32_t OrrOneFrame(uint32_t rd, uint32_t rn) { return 0x32110000u | (rn << 5) | rd; }      // orr wd, wn, #0x8000
constexpr uint32_t StoreHalfSp(uint32_t rt, uint32_t offset) { return 0x79000000u | ((offset / 2) << 10) | (31u << 5) | rt; }
constexpr uint32_t StoreWordSp(uint32_t rt, uint32_t offset) { return 0xb9000000u | ((offset / 4) << 10) | (31u << 5) | rt; }
constexpr uint32_t Movz(uint32_t rd, uint32_t value, uint32_t lane = 0) { return 0x52800000u | (lane << 21) | (value << 5) | rd; }
constexpr uint32_t Movk(uint32_t rd, uint32_t value, uint32_t lane) { return 0x72800000u | (lane << 21) | (value << 5) | rd; }
constexpr uint32_t kRet = 0xd65f03c0, kNop = 0xd503201f;

struct RegistrationCode
{
    FakeBinary Binary{ false, {
        { kEmplaceEntry, kEmplace, N_SECT, 1 },
        { kHealthIndex, kHealth, N_SECT, 3 },
        { kArmorIndex, kArmor, N_SECT, 3 },
        { kDiedIndex, kDied, N_SECT, 3 },
    } };
    uint64_t At{ FakeBinary::Text };

    RegistrationCode()
    {
        for (uint64_t va = FakeBinary::Text; va < FakeBinary::Text + 0x100; va += 4) Binary.Word(va, kNop);
        Binary.Word(kEmplace, kRet);
        Binary.Pointer(kDiedPointer, kDied);
    }
    void Emit(uint32_t word) { Binary.Word(At, word); At += 4; }
    void Call() { Emit(0x94000000u | (uint32_t)(((kEmplace - At) / 4) & 0x03ffffff)); }
    // The start of a registration: the type's index into the entry's first
    // field, read through the variable's address formed directly.
    void TypeIndex(uint64_t variable, bool oneFrame)
    {
        Emit(AdrpData(26));
        Emit(AddImm(26, 26, (uint32_t)(variable - FakeBinary::Data)));
        Emit(LoadHalf(8, 26));
        Emit(oneFrame ? OrrOneFrame(23, 8) : AndIndex(23, 8));
        Emit(StoreHalfSp(23, kEntry));
    }
    void Register()
    {
        Emit(AddImm(2, 31, kEntry));
        Call();
    }
    bg3se::mac::ComponentRegistryScan Scan()
    {
        bg3se::mac::GameImage image;
        CHECK(image.OpenFile(Binary.File));
        return bg3se::mac::DecodeComponentRegistry(image);
    }
};

void TestComponentRegistrations()
{
    using bg3se::mac::kComponentTypeEntrySizes;
    {
        RegistrationCode code;
        // _first: a component of 0x28 bytes, both sizes from one register.
        code.TypeIndex(kHealth, false);
        code.Emit(Movz(8, 0x28));
        code.Emit(StoreWordSp(8, kEntry + 0x10));
        code.Emit(StoreHalfSp(8, kEntry + 0x12));
        code.Register();
        code.Emit(kRet);
        // _second: a one-frame component kept by pointer (8 in the storage,
        // 0x60 with what is behind it), the two sizes in one constant, its
        // index variable reached through a pointer to it ...
        code.At = FakeBinary::Text + 0x40;
        code.Emit(AdrpData(26));
        code.Emit(LoadPointer(26, 26, (uint32_t)(kDiedPointer - FakeBinary::Data)));
        code.Emit(LoadHalf(8, 26));
        code.Emit(OrrOneFrame(23, 8));
        code.Emit(StoreHalfSp(23, kEntry));
        code.Emit(Movz(8, 8));
        code.Emit(Movk(8, 0x60, 1));
        code.Emit(StoreWordSp(8, kEntry + 0x10));
        code.Register();
        // ... and one whose size is not a constant: it is not read.
        code.TypeIndex(kArmor, false);
        code.Emit(LoadWord(8, 0));
        code.Emit(StoreWordSp(8, kEntry + 0x10));
        code.Register();
        code.Emit(kRet);

        auto scan = code.Scan();
        CHECK(scan.Ok() && scan.Sites == 3);
        CHECK(scan.Components.size() == 2 && scan.Undecoded.size() == 1);
        auto health = scan.Find("eoc::HealthComponent");
        CHECK(health && health->InlineSize == 0x28 && health->TotalSize == 0x28 && !health->OneFrame);
        auto died = scan.Find("esv::death::DiedEventOneFrameComponent");
        CHECK(died && died->InlineSize == 8 && died->TotalSize == 0x60 && died->OneFrame);
        CHECK(!scan.Find("eoc::ArmorComponent"));
        CHECK(scan.Undecoded.size() == 1 && scan.Undecoded[0].find("not constants") != std::string::npos);
    }
    {
        // Only the storage's size is stored: the component's own is unknown.
        RegistrationCode code;
        code.TypeIndex(kHealth, false);
        code.Emit(Movz(8, 0x28));
        code.Emit(StoreHalfSp(8, kEntry + 0x10));
        code.Register();
        code.Emit(kRet);
        auto scan = code.Scan();
        CHECK(scan.Sites == 1 && scan.Components.empty() && scan.Undecoded.size() == 1);
    }
    {
        // The size is overwritten by something that is no constant.
        RegistrationCode code;
        code.TypeIndex(kHealth, false);
        code.Emit(Movz(8, 0x28));
        code.Emit(StoreWordSp(8, kEntry + 0x10));
        code.Emit(StoreHalfSp(8, kEntry + 0x12));
        code.Emit(StoreHalfSp(1, kEntry + 0x10));
        code.Register();
        code.Emit(kRet);
        auto scan = code.Scan();
        CHECK(scan.Components.empty() && scan.Undecoded.size() == 1);
    }
    {
        // The one-frame bit on a type of the persistent context: not that type's registration.
        RegistrationCode code;
        code.TypeIndex(kHealth, true);
        code.Emit(Movz(8, 0x28));
        code.Emit(StoreWordSp(8, kEntry + 0x10));
        code.Emit(StoreHalfSp(8, kEntry + 0x12));
        code.Register();
        code.Emit(kRet);
        auto scan = code.Scan();
        CHECK(scan.Components.empty() && scan.Undecoded.size() == 1 && scan.Undecoded[0].find("one-frame") != std::string::npos);
    }
    {
        // The index is not from an index variable at all.
        RegistrationCode code;
        code.Emit(AdrpData(26));
        code.Emit(AddImm(26, 26, 0x80));
        code.Emit(LoadHalf(8, 26));
        code.Emit(AndIndex(23, 8));
        code.Emit(StoreHalfSp(23, kEntry));
        code.Emit(Movz(8, 0x28));
        code.Emit(StoreWordSp(8, kEntry + 0x10));
        code.Emit(StoreHalfSp(8, kEntry + 0x12));
        code.Register();
        code.Emit(kRet);
        auto scan = code.Scan();
        CHECK(scan.Components.empty() && scan.Undecoded.size() == 1 && scan.Undecoded[0].find("index variable") != std::string::npos);
    }
    {
        // Two registrations of one type that disagree: neither is taken.
        RegistrationCode code;
        code.TypeIndex(kHealth, false);
        code.Emit(Movz(8, 0x28));
        code.Emit(StoreWordSp(8, kEntry + 0x10));
        code.Emit(StoreHalfSp(8, kEntry + 0x12));
        code.Register();
        code.Emit(kRet);
        code.At = FakeBinary::Text + 0x40;
        code.TypeIndex(kHealth, false);
        code.Emit(Movz(8, 0x30));
        code.Emit(StoreWordSp(8, kEntry + 0x10));
        code.Emit(StoreHalfSp(8, kEntry + 0x12));
        code.Register();
        code.Emit(kRet);
        auto scan = code.Scan();
        CHECK(scan.Sites == 2 && scan.Components.empty() && scan.Undecoded.size() == 1);
    }
    {
        // A binary without the registry's function says so.
        FakeBinary plain;
        bg3se::mac::GameImage image;
        CHECK(image.OpenFile(plain.File));
        auto scan = bg3se::mac::DecodeComponentRegistry(image);
        CHECK(!scan.Ok() && scan.Components.empty());
    }
    static_assert(kComponentTypeEntrySizes == 0x10);
}

// --- virtual slots ----------------------------------------------------------------

void TestVirtualSlots()
{
    using bg3se::mac::DeclaredVirtual;
    // The test binary's vtable holds _first, _second, _first.
    FakeBinary binary;
    bg3se::mac::GameImage image;
    CHECK(image.OpenFile(binary.File));
    static constexpr DeclaredVirtual kRows[] = {
        { "Class", "Second", 1, "_vtable", "_second" },         // where the game has it
        { "Class", "SecondEarly", 0, "_vtable", "_second" },    // one slot early, as with one destructor slot for two
        { "Class", "First", 2, "_vtable", "_first" },           // held in two slots: either will do
        { "Class", "FirstElsewhere", 1, "_vtable", "_first" },
        { "Class", "NoFunction", 0, "_vtable", "_no_such_function" },
        { "Class", "NoVtable", 0, "_no_such_vtable", "_first" },
        { "Class", "NotHeld", 0, "_after", "_first" },          // another table, which does not hold it
    };
    auto results = bg3se::mac::CheckVirtualSlots(image, kRows);
    CHECK(results.size() == 7);
    if (results.size() != 7) return;
    CHECK(results[0].Problem.empty() && results[0].GameSlot == 1);
    CHECK(!results[1].Problem.empty() && results[1].GameSlot == 1 && results[1].Problem.find("slot 0") != std::string::npos);
    CHECK(results[2].Problem.empty() && results[2].GameSlot == 0);
    CHECK(!results[3].Problem.empty());
    CHECK(!results[4].Problem.empty() && results[4].GameSlot == -1);
    CHECK(!results[5].Problem.empty() && results[5].GameSlot == -1);
    CHECK(!results[6].Problem.empty() && results[6].GameSlot == -1);
}


// The operands the proofs of NoesisGUI's code read (Layout/GameShapes.cpp):
// a value formed by movz and movk, the bit a tbz or tbnz tests, a 32-bit
// atomic addition, and a tail call of an import.
void TestNoesisOperands()
{
    using namespace bg3se::mac::proof;
    FakeBinary binary;
    constexpr uint64_t at = FakeBinary::Text + 0x80;
    auto branch = [](uint64_t from, uint64_t to, uint32_t op) { return op | (uint32_t)(((to - from) / 4) & 0x03ffffff); };
    uint32_t const words[] = {
        0x52836c22,     // 0: movz w2, #0x1b61
        0x72a981a2,     // 1: movk w2, #0x4c0d, lsl #16
        0x52836c22,     // 2: movz w2, #0x1b61
        0x72a981a3,     // 3: movk w3, #0x4c0d, lsl #16: another register
        0x36180048,     // 4: tbz w8, #3, +8
        0xb7080048,     // 5: tbnz x8, #33, +8
        0xb829010a,     // 6: ldadd w9, w10, [x8]
        0xb8e9010a,     // 7: ldaddal w9, w10, [x8]
        0xf829010a,     // 8: ldadd x9, x10, [x8]: 64 bits
        branch(at + 36, FakeBinary::Stubs + 12, 0x14000000),     // 9: b _longjmp
        branch(at + 40, FakeBinary::Stubs + 12, 0x94000000),     // 10: bl _longjmp
        branch(at + 44, FakeBinary::Stubs, 0x14000000),          // 11: b _setjmp
    };
    for (size_t i = 0; i < std::size(words); i++) binary.Word(at + 4 * i, words[i]);
    bg3se::mac::GameImage image;
    CHECK(image.OpenFile(binary.File));
    Code f{ image, at, at + 4 * std::size(words) };
    auto holds = [&](Expect const& e) { std::string problem; return CheckOperand(f, e, problem); };

    CHECK(holds({ 0, Operand::MoveWide, 0x4c0d1b61 }));
    CHECK(!holds({ 0, Operand::MoveWide, 0x1b61 }));
    CHECK(!holds({ 2, Operand::MoveWide, 0x4c0d1b61 }));
    CHECK(!holds({ 1, Operand::MoveWide, 0x4c0d1b61 }));
    CHECK(holds({ 4, Operand::TestsBit, 3 }));
    CHECK(!holds({ 4, Operand::TestsBit, 4 }));
    CHECK(holds({ 5, Operand::TestsBit, 33 }));
    CHECK(!holds({ 6, Operand::TestsBit, 0 }));
    CHECK(holds({ 6, Operand::AtomicAdd32 }));
    CHECK(holds({ 7, Operand::AtomicAdd32 }));
    CHECK(!holds({ 8, Operand::AtomicAdd32 }));
    CHECK(!holds({ 4, Operand::AtomicAdd32 }));
    CHECK(holds({ 9, Operand::JumpsToImport, 0, "_longjmp" }));
    CHECK(!holds({ 9, Operand::JumpsToImport, 0, "_setjmp" }));
    CHECK(!holds({ 10, Operand::JumpsToImport, 0, "_longjmp" }));
    CHECK(holds({ 10, Operand::CallsImport, 0, "_longjmp" }));
    CHECK(holds({ 11, Operand::JumpsToImport, 0, "_setjmp" }));
}
}

// Norbyte's string reads the game's 16 bytes as the game's code does
// (Layout/GameFiles.h, proven by ls::Path's constructor): a text of up to 14
// characters in the string itself with its length in the last byte, a longer
// one behind the pointer with its length in the 32 bits at +8.
void TestGameString()
{
    using namespace bg3se::mac;
    CHECK(test::NorbyteStringSize() == kGameStringBytes);

    alignas(8) unsigned char bytes[kGameStringBytes]{};
    CHECK(test::NorbyteStringLength(bytes) == 0 && test::NorbyteStringText(bytes) == reinterpret_cast<char const*>(bytes));
    memcpy(bytes, "Public/Shared", 13);
    bytes[kGameStringFlag] = 13;
    CHECK(test::NorbyteStringLength(bytes) == 13 && test::NorbyteStringText(bytes) == reinterpret_cast<char const*>(bytes));
    memset(bytes, 'x', 14);
    bytes[14] = 0;
    bytes[kGameStringFlag] = 14;
    CHECK(test::NorbyteStringLength(bytes) == 14 && test::NorbyteStringText(bytes) == reinterpret_cast<char const*>(bytes));

    static const char kText[] = "/Volumes/Games/Data/Public/Shared/Stats/Generated/Data/Armor.txt";
    char const* text = kText;
    uint32_t length = sizeof(kText) - 1, capacity = 0x80000000u | 0x50;
    memset(bytes, 0, sizeof(bytes));
    memcpy(bytes, &text, sizeof(text));
    memcpy(bytes + kGameStringLongLength, &length, sizeof(length));
    memcpy(bytes + kGameStringLongCapacity, &capacity, sizeof(capacity));
    CHECK(static_cast<signed char>(bytes[kGameStringFlag]) < 0);
    CHECK(test::NorbyteStringLength(bytes) == length && test::NorbyteStringText(bytes) == kText);
    // The bytes above the length's 32 bits are the capacity's, not the length's.
    length = 15;
    capacity = 0xffffffffu;
    memcpy(bytes + kGameStringLongLength, &length, sizeof(length));
    memcpy(bytes + kGameStringLongCapacity, &capacity, sizeof(capacity));
    CHECK(test::NorbyteStringLength(bytes) == 15);
}

int main()
{
    TestGameString();
    bg3se::TestComponentRegistrations();
    bg3se::TestVirtualSlots();
    bg3se::TestNoesisOperands();
    TestLayout();
    TestValues();
    TestLifetimes();
    TestVisit();
    TestNested();
    TestUtf16Chars();
    bg3se::TestStdNames();
    bg3se::nested::TestStdNamesNested();
    if (gFailures) {
        printf("%d checks failed\n", gFailures);
        return 1;
    }
    printf("layout tests passed\n");
    return 0;
}
