// A stand-in for libbg3mse.dylib in the updater's tests: it carries a
// version stamp as the real one does (Mac/Release/Version.h), and the
// graphics module's mark as one built with it does (Mac/Release/GfxMark.h),
// and says on stdout that it was loaded, and from where.

#include <Release/GfxMark.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

__attribute__((used, section("__TEXT,__bg3mse_version")))
static char const kStamp[] = "@(#)bg3mse " FAKE_VERSION " commit=0000000000000000000000000000000000000000 extender=0.0.0.0 channel=Release";

__attribute__((used, section(BG3MSE_GFX_SEGMENT "," BG3MSE_GFX_SECTION))) static char const kGfxMark[] = BG3MSE_GFX_MARK;

// One of the functions a native module looks up in the extender's module.
__attribute__((visibility("default"))) unsigned SEGetAPIVersion(void)
{
    return 0x10000;
}

// A native module's callback for SEUpdaterNotifyWhenLoaded: says what it was
// given, and asks once more, now that the extender is loaded.
typedef void LoadedProc(void* module, void* userData);
static void NativeModuleTold(void* module, void* userData)
{
    void* (*getModule)(void) = (void* (*)(void))dlsym(RTLD_DEFAULT, "SEUpdaterGetModule");
    void (*notify)(LoadedProc*, void*) = (void (*)(LoadedProc*, void*))dlsym(RTLD_DEFAULT, "SEUpdaterNotifyWhenLoaded");
    unsigned (*version)(void) = module ? (unsigned (*)(void))dlsym(module, "SEGetAPIVersion") : NULL;
    printf("native %s: %s, %s\n", (char const*)userData,
        version == &SEGetAPIVersion ? "the module gives dlsym this image's function" : "the module does not lead here",
        getModule && module && getModule() == module ? "as SEUpdaterGetModule" : "not SEUpdaterGetModule's");
    fflush(stdout);
    static int again;
    if (!again++ && notify) notify(&NativeModuleTold, "told at once");
}

__attribute__((constructor))
static void Loaded(void)
{
    Dl_info info = { 0 };
    dladdr((void const*)&Loaded, &info);
    printf("extender %s %s\n", FAKE_VERSION, info.dli_fname ? info.dli_fname : "?");
    fflush(stdout);
    // As the extender does when it can show the updater's errors in the game
    // (UpdaterAPIHelpers::MarkReadyToDisplayErrors).
    if (getenv("FAKE_ERROR_REPORTING")) {
        void (*setCapabilities)(unsigned) = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "SEUpdaterSetCapabilities");
        if (setCapabilities) setCapabilities(1);
    }
    // As a native module that uses the extender does, here at the earliest
    // moment one can: while the loader is still loading the extender.
    if (getenv("FAKE_NATIVE_MODULE")) {
        void* (*getModule)(void) = (void* (*)(void))dlsym(RTLD_DEFAULT, "SEUpdaterGetModule");
        void (*notify)(LoadedProc*, void*) = (void (*)(LoadedProc*, void*))dlsym(RTLD_DEFAULT, "SEUpdaterNotifyWhenLoaded");
        printf("native asks: %s\n", !getModule || !notify ? "no API" : getModule() ? "a module already" : "no module yet");
        fflush(stdout);
        if (notify) notify(&NativeModuleTold, "told when loaded");
    }
}
