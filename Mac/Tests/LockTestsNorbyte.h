#pragma once

#include <cstddef>

// Norbyte's SRWLock and SRWSpinLock (GameDefinitions/Base/Lock.h, Lock.inl),
// compiled as his sources are, behind plain functions for the lock tests.

namespace bg3se::mac::test {

struct NorbyteLock
{
    void* Object;           // the lock: an SRWLock or an SRWSpinLock
    size_t Size;
    void (*ReadLock)(void*);
    void (*ReadUnlock)(void*);
    void (*WriteLock)(void*);
    void (*WriteUnlock)(void*);
    void (*Destroy)(void*);
};

NorbyteLock MakeSRWLock();
NorbyteLock MakeSRWSpinLock();

}
