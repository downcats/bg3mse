// A program that is named like the game and has none of its code, for the
// tests of libbg3mse.dylib's start-up (Mac/Tests/DylibLoadTests.cpp): the
// dylib takes a process for the game's by its executable's name. It loads the
// dylib it is given, as the game's load command does, and goes on. Arguments
// after the dylib's path are there for the dylib to find.

#include <cstdio>
#include <dlfcn.h>

int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    if (!dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)) {
        fprintf(stderr, "stand-in: %s\n", dlerror());
        return 3;
    }
    puts("stand-in: loaded");
    return 0;
}
