#include <cstdarg>
#include <cstdio>

// The extender's own standard output.
//
// On Windows the extender is built with its own copy of the C runtime
// (BG3Extender.vcxproj: RuntimeLibrary MultiThreaded), so its stdout is its
// own: Console::Create's freopen_s("CONOUT$", stdout) sends to the console
// what the extender's code writes there (printf, puts, fwrite to stdout, and
// the Lua library's own output), and nothing the game or another library
// writes to theirs.
//
// Here every image shares one C library, and with it one stdout. So that
// freopen_s can do the same (Win32Console.cpp), the image this is linked into
// has a stdout of its own: the C library names its standard output stream
// __stdoutp, and a definition of it in this image is what this image's code
// refers to, while every other image goes on referring to the C library's.
// It starts as the C library's stream, so that until a console is made, and
// whenever the console is the terminal, the image writes where the process
// does.
//
// printf, vprintf, puts and putchar write to the C library's stdout from
// inside the C library: this image has its own of those too, writing to its
// own stdout. Neither is exported (the image exports nothing).

extern "C" {

// The C library's standard streams: stdin, stdout and stderr.
extern FILE __sF[];

__attribute__((visibility("hidden"))) FILE* __stdoutp = &__sF[1];

__attribute__((visibility("hidden"))) int vprintf(char const* __restrict format, va_list args)
{
    return vfprintf(__stdoutp, format, args);
}

__attribute__((visibility("hidden"))) int printf(char const* __restrict format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vfprintf(__stdoutp, format, args);
    va_end(args);
    return written;
}

__attribute__((visibility("hidden"))) int puts(char const* text)
{
    FILE* out = __stdoutp;
    flockfile(out);
    int result = fputs(text, out) == EOF || putc_unlocked('\n', out) == EOF ? EOF : 1;
    funlockfile(out);
    return result;
}

__attribute__((visibility("hidden"))) int putchar(int c)
{
    return putc(c, __stdoutp);
}

}
