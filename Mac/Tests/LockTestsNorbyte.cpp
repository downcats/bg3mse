#include <stdafx.h>
#include <GameDefinitions/Base/Lock.inl>

#include "LockTestsNorbyte.h"

// The lock classes assert their invariants through this function of CoreLib,
// which the tests do not link: an assertion that fails here fails the test.
void bg3se::AssertionFailed(char const* expr)
{
    fprintf(stderr, "assertion of Lock.inl failed: %s\n", expr);
    abort();
}

BEGIN_NS(mac::test)

namespace {

template <class T>
NorbyteLock Make()
{
    return {
        new T(), sizeof(T),
        [](void* lock) { static_cast<T*>(lock)->ReadLock(); },
        [](void* lock) { static_cast<T*>(lock)->ReadUnlock(); },
        [](void* lock) { static_cast<T*>(lock)->WriteLock(); },
        [](void* lock) { static_cast<T*>(lock)->WriteUnlock(); },
        [](void* lock) { delete static_cast<T*>(lock); },
    };
}

}

NorbyteLock MakeSRWLock() { return Make<SRWLock>(); }
NorbyteLock MakeSRWSpinLock() { return Make<SRWSpinLock>(); }

END_NS()
