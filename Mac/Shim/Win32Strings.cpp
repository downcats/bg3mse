// UTF-8 <-> UTF-32 conversion with the Win32 calling conventions, and the
// MSVC C runtime's bounds-checked string functions.
#include <windows.h>

namespace {

constexpr uint32_t Replacement = 0xFFFD;
constexpr DWORD ERROR_INVALID_FLAGS = 1004;

// Decodes one code point at s[0..n). Returns the number of bytes consumed
// (at least 1); `cp` is the code point, or Replacement for an ill-formed
// sequence, which consumes its maximal subpart as the Unicode standard
// recommends (U+FFFD substitution of maximal subparts, chapter 3.9).
size_t DecodeUtf8(unsigned char const* s, size_t n, uint32_t& cp, bool& valid)
{
    unsigned char c = s[0];
    valid = true;
    if (c < 0x80) {
        cp = c;
        return 1;
    }

    size_t length;
    uint32_t value;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
        length = 2; value = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        length = 3; value = c & 0x0F;
        if (c == 0xE0) lo = 0xA0;          // no overlong forms
        if (c == 0xED) hi = 0x9F;          // no surrogates
    } else if (c >= 0xF0 && c <= 0xF4) {
        length = 4; value = c & 0x07;
        if (c == 0xF0) lo = 0x90;          // no overlong forms
        if (c == 0xF4) hi = 0x8F;          // nothing above U+10FFFF
    } else {
        cp = Replacement;
        valid = false;
        return 1;
    }

    for (size_t i = 1; i < length; i++) {
        if (i >= n) {
            cp = Replacement;
            valid = false;
            return i;
        }
        unsigned char t = s[i];
        unsigned char tlo = (i == 1) ? lo : 0x80;
        unsigned char thi = (i == 1) ? hi : 0xBF;
        if (t < tlo || t > thi) {
            cp = Replacement;
            valid = false;
            return i;
        }
        value = (value << 6) | (t & 0x3F);
    }

    cp = value;
    return length;
}

size_t EncodeUtf8(uint32_t cp, char* out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

bool IsScalarValue(uint32_t cp)
{
    return cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF);
}

// What the MSVC runtime's default invalid parameter handler does: end the process.
[[noreturn]] void InvalidParameter(char const* function)
{
    fprintf(stderr, "%s: invalid parameter\n", function);
    abort();
}

}

int MultiByteToWideChar(UINT codePage, DWORD flags, LPCSTR src, int srcLength, LPWSTR dest, int destLength)
{
    if (codePage != CP_UTF8 || src == nullptr || srcLength == 0 || srcLength < -1 || destLength < 0
        || (dest == nullptr && destLength != 0)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & ~(DWORD)MB_ERR_INVALID_CHARS) != 0) {
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }

    auto s = reinterpret_cast<unsigned char const*>(src);
    size_t n = (srcLength == -1) ? strlen(src) + 1 : (size_t)srcLength;
    size_t written = 0;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        bool valid;
        i += DecodeUtf8(s + i, n - i, cp, valid);
        if (!valid && (flags & MB_ERR_INVALID_CHARS)) {
            SetLastError(ERROR_NO_UNICODE_TRANSLATION);
            return 0;
        }
        if (destLength != 0) {
            if (written >= (size_t)destLength) {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            dest[written] = (wchar_t)cp;
        }
        written++;
    }
    return (int)written;
}

int WideCharToMultiByte(UINT codePage, DWORD flags, LPCWSTR src, int srcLength, LPSTR dest, int destLength,
    LPCSTR defaultChar, BOOL* usedDefaultChar)
{
    if (codePage != CP_UTF8 || src == nullptr || srcLength == 0 || srcLength < -1 || destLength < 0
        || (dest == nullptr && destLength != 0) || defaultChar != nullptr || usedDefaultChar != nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & ~(DWORD)WC_ERR_INVALID_CHARS) != 0) {
        SetLastError(ERROR_INVALID_FLAGS);
        return 0;
    }

    size_t n = (srcLength == -1) ? wcslen(src) + 1 : (size_t)srcLength;
    size_t written = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = (uint32_t)src[i];
        if (!IsScalarValue(cp)) {
            if (flags & WC_ERR_INVALID_CHARS) {
                SetLastError(ERROR_NO_UNICODE_TRANSLATION);
                return 0;
            }
            cp = Replacement;
        }
        char buf[4];
        size_t len = EncodeUtf8(cp, buf);
        if (destLength != 0) {
            if (written + len > (size_t)destLength) {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            memcpy(dest + written, buf, len);
        }
        written += len;
    }
    return (int)written;
}

int vsprintf_s(char* buffer, size_t size, const char* format, va_list args)
{
    if (buffer == nullptr || size == 0 || format == nullptr) {
        InvalidParameter("vsprintf_s");
    }
    int n = vsnprintf_l(buffer, size, LC_C_LOCALE, format, args);
    if (n < 0) {
        buffer[0] = 0;
        return -1;
    }
    if ((size_t)n >= size) {
        buffer[0] = 0;
        InvalidParameter("vsprintf_s");
    }
    return n;
}

int _vsnprintf_s(char* buffer, size_t size, size_t count, const char* format, va_list args)
{
    if (buffer == nullptr || size == 0 || format == nullptr) {
        InvalidParameter("_vsnprintf_s");
    }

    // At most `count` characters (or as many as fit with _TRUNCATE) plus the terminator.
    bool truncating = (count == _TRUNCATE) || (count < size);
    size_t limit = (count == _TRUNCATE || count >= size) ? size : count + 1;
    int n = vsnprintf_l(buffer, limit, LC_C_LOCALE, format, args);
    if (n < 0) {
        buffer[0] = 0;
        return -1;
    }
    if ((size_t)n >= limit) {
        if (truncating) {
            return -1;
        }
        buffer[0] = 0;
        InvalidParameter("_vsnprintf_s");
    }
    return n;
}

int strcpy_s(char* dest, size_t size, const char* src)
{
    if (dest == nullptr || size == 0 || src == nullptr) {
        InvalidParameter("strcpy_s");
    }
    size_t len = strlen(src);
    if (len >= size) {
        dest[0] = 0;
        InvalidParameter("strcpy_s");
    }
    memcpy(dest, src, len + 1);
    return 0;
}

int strncpy_s(char* dest, size_t size, const char* src, size_t count)
{
    if (dest == nullptr || size == 0 || src == nullptr) {
        InvalidParameter("strncpy_s");
    }
    size_t len = strnlen(src, count == _TRUNCATE ? size : count);
    if (len >= size) {
        if (count != _TRUNCATE) {
            dest[0] = 0;
            InvalidParameter("strncpy_s");
        }
        memcpy(dest, src, size - 1);
        dest[size - 1] = 0;
        return STRUNCATE;
    }
    memcpy(dest, src, len);
    dest[len] = 0;
    return 0;
}

int wcscpy_s(wchar_t* dest, size_t size, const wchar_t* src)
{
    if (dest == nullptr || size == 0 || src == nullptr) {
        InvalidParameter("wcscpy_s");
    }
    size_t len = wcslen(src);
    if (len >= size) {
        dest[0] = 0;
        InvalidParameter("wcscpy_s");
    }
    wmemcpy(dest, src, len + 1);
    return 0;
}
