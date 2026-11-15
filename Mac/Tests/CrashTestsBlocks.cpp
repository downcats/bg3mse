// Guarded blocks as Norbyte's sources write them, for Mac/Tests/CrashTests.cpp:
// through the shim's GuardedBlock, with the definitions BEGIN_GUARDED and
// END_GUARDED have outside MSVC (GameDefinitions/Base/Base.h). A file of its
// own, to keep the Windows names out of the tests.

#include <windows.h>

#define BEGIN_GUARDED() ::bg3se::mac::GuardedBlock([&]() {
#define END_GUARDED() });

void CrashTestGuarded(void (*body)(void*), void* arg)
{
    BEGIN_GUARDED()
    body(arg);
    END_GUARDED()
}

void CrashTestGuardedTwice(void (*body)(void*), void* arg, int& before, int& after)
{
    BEGIN_GUARDED()
    before++;
    BEGIN_GUARDED()
    body(arg);
    END_GUARDED()
    after++;
    END_GUARDED()
}
