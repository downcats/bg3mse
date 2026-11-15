// Loads a copy of the crash fixture (Mac/Tests/ReleaseCrashFixture.cpp) and
// has it crash: release-crash-driver <fixture.dylib> <reports directory>.

#include <dlfcn.h>
#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <fixture.dylib> <reports directory>\n", argv[0]);
        return 100;
    }
    void* fixture = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!fixture) {
        fprintf(stderr, "%s\n", dlerror());
        return 101;
    }
    auto crash = reinterpret_cast<int (*)(char const*)>(dlsym(fixture, "bg3mse_release_fixture_crash"));
    if (!crash) {
        fprintf(stderr, "%s\n", dlerror());
        return 102;
    }
    // Returns only when the reporter did not start.
    return 103 + crash(argv[2]);
}
