#include <windows.h>

#include <map>
#include <mutex>
#include <pthread.h>
#include <string>

// CreateMutexA: named mutex objects, as far as one process sees them.
//
// A name is known within this process only: Windows shares a named object
// with the other processes of the session, and nothing here does. Norbyte's
// one caller (Extender/Shared/dllmain.cpp) names its mutex after the process
// id, to know whether an extender already runs in this process.
//
// The handle is the object. The shim has no function that waits on, releases
// or closes one yet (Norbyte's code calls none): the owner is recorded for
// the day one does.

namespace {

struct Mutex
{
    pthread_t Owner{};
    unsigned Recursion{ 0 };
};

std::mutex gLock;
std::map<std::string, Mutex*> gNamed;

}

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES, BOOL initialOwner, LPCSTR name)
{
    std::lock_guard lock(gLock);
    if (name) {
        // A name may not hold a backslash, apart from a namespace prefix.
        std::string text = name;
        for (char const* prefix : { "Global\\", "Local\\" }) {
            if (text.rfind(prefix, 0) == 0) text.erase(0, strlen(prefix));
        }
        if (text.empty() || text.size() > MAX_PATH || text.find('\\') != std::string::npos) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return NULL;
        }
        if (auto it = gNamed.find(text); it != gNamed.end()) {
            // The existing object; the request for ownership is ignored, as on Windows.
            SetLastError(ERROR_ALREADY_EXISTS);
            return it->second;
        }
        auto mutex = new Mutex();
        gNamed.emplace(std::move(text), mutex);
        if (initialOwner) {
            mutex->Owner = pthread_self();
            mutex->Recursion = 1;
        }
        SetLastError(0);
        return mutex;
    }
    auto mutex = new Mutex();
    if (initialOwner) {
        mutex->Owner = pthread_self();
        mutex->Recursion = 1;
    }
    SetLastError(0);
    return mutex;
}
