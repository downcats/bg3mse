#pragma once

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

// UTF-16 code units, as files written on Windows hold text: what wchar_t is
// there. On Mac wchar_t is four bytes and holds a code point, so such text is
// converted where Norbyte's code reads or writes it as wchar_t.
//
// The conversion loses no unit: a surrogate pair becomes one character, its
// code point, and every other unit (an unpaired surrogate, or a zero) a
// character of its own value. Converting the characters back gives the units
// that were read, as copying them as wchar_t does on Windows; turning such
// text into UTF-8 gives U+FFFD for an unpaired surrogate on both.

namespace bg3se::mac {

// The `count` units at `units`, which need not be aligned.
template <class Wide = std::wstring>
Wide WideFromUtf16(void const* units, size_t count)
{
    auto bytes = static_cast<unsigned char const*>(units);
    auto unitAt = [bytes](size_t n) {
        char16_t unit;
        std::memcpy(&unit, bytes + n * sizeof(char16_t), sizeof(char16_t));
        return (char32_t)unit;
    };

    Wide wide;
    wide.reserve(count);
    for (size_t n = 0; n < count; n++) {
        auto unit = unitAt(n);
        if (unit >= 0xd800 && unit < 0xdc00 && n + 1 < count) {
            auto low = unitAt(n + 1);
            if (low >= 0xdc00 && low < 0xe000) {
                wide.push_back((wchar_t)(0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00)));
                n++;
                continue;
            }
        }
        wide.push_back((wchar_t)unit);
    }
    return wide;
}

// Appends the units of `text` to `units`. When a character is above U+10FFFF
// nothing is appended and false is returned.
inline bool AppendUtf16(std::wstring_view text, std::u16string& units)
{
    auto start = units.size();
    for (wchar_t wide : text) {
        auto point = (char32_t)wide;
        if (point > 0x10ffff) {
            units.resize(start);
            return false;
        }
        if (point >= 0x10000) {
            point -= 0x10000;
            units.push_back((char16_t)(0xd800 + (point >> 10)));
            units.push_back((char16_t)(0xdc00 + (point & 0x3ff)));
        } else {
            units.push_back((char16_t)point);
        }
    }
    return true;
}

// A fixed buffer of units, zero-terminated: what a wchar_t[N] member of an
// on-disk record is in a file written on Windows. It keeps the record's size
// and converts as above where Norbyte's code reads or writes the member. A
// plain aggregate, as the array is.
template <size_t N>
struct Utf16Chars
{
    char16_t Units[N];

    // The text up to the terminator (or the end of the buffer).
    std::wstring Wide() const
    {
        size_t length = 0;
        while (length < N && Units[length]) {
            length++;
        }
        return WideFromUtf16(Units, length);
    }

    // Stores `text` with its terminator. When it does not fit, or holds a
    // character above U+10FFFF, the buffer is left empty and false is
    // returned.
    bool Assign(std::wstring_view text)
    {
        std::u16string units;
        if (!AppendUtf16(text, units) || units.size() >= N) {
            Units[0] = 0;
            return false;
        }
        std::memcpy(Units, units.data(), units.size() * sizeof(char16_t));
        Units[units.size()] = 0;
        return true;
    }

    // `STDWString path = record.FileNameBuf;`
    template <class Traits, class Allocator>
    operator std::basic_string<wchar_t, Traits, Allocator>() const
    {
        auto wide = Wide();
        return std::basic_string<wchar_t, Traits, Allocator>(wide.data(), wide.size());
    }
};

// `wcscpy_s(record.FileNameBuf, path)`, with wcscpy_s's result: 0, or ERANGE
// and an empty destination when the text does not fit.
template <size_t N>
int wcscpy_s(Utf16Chars<N>& dest, wchar_t const* src)
{
    return dest.Assign(src) ? 0 : ERANGE;
}

}
