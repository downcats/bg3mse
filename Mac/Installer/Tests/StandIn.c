// A stand-in for libbg3mse.dylib (and libbg3mse-camera.dylib, and the
// updater's loader) in the installer's tests: says so when it is loaded. The
// loader's (STAND_IN_LOADS) then loads the libbg3mse.dylib beside it, as the
// real one does. libbg3mse.dylib's carries the graphics module's mark
// (STAND_IN_GFX), but for the one in standin-plain/.

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#ifndef STAND_IN_NAME
#define STAND_IN_NAME "libbg3mse.dylib"
#endif

#ifdef STAND_IN_GFX
#include <Release/GfxMark.h>
// A libbg3mse.dylib with the graphics module says so (Release/GfxMark.h).
__attribute__((used, section(BG3MSE_GFX_SEGMENT "," BG3MSE_GFX_SECTION))) static char const kGfxMark[] = BG3MSE_GFX_MARK;
#endif

__attribute__((constructor)) static void Loaded(void)
{
    printf("stand-in %s loaded\n", STAND_IN_NAME);
    fflush(stdout);
#ifdef STAND_IN_LOADS
    Dl_info self = { 0 };
    char path[4096];
    if (dladdr((void const*)&Loaded, &self) && self.dli_fname && strlen(self.dli_fname) < sizeof(path) - 32) {
        strcpy(path, self.dli_fname);
        char* slash = strrchr(path, '/');
        if (slash) {
            strcpy(slash + 1, STAND_IN_LOADS);
            dlopen(path, RTLD_NOW);
        }
    }
#endif
}
