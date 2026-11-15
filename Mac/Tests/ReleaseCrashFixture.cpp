// A dylib that carries the crash reporter as libbg3mse.dylib does, and
// crashes in a function of its own: Mac/Tests/ReleaseSymbolication.cmake
// strips it as a release does (Mac/Release/Symbols.cmake), crashes it, and
// names the function from the report with the dSYM; and the same for a
// thread of its own that is waiting in another of its functions.

#include <Crash/CrashReporter.h>

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

namespace {

using namespace bg3se::mac;

char gDirectory[PATH_MAX];

size_t ReportDirectory(char* out, size_t size)
{
    size_t length = strlen(gDirectory);
    if (length == 0 || length >= size) return 0;
    memcpy(out, gDirectory, length + 1);
    return length;
}

int* volatile gNowhere = reinterpret_cast<int*>(0x10);

// What the report must name, by the dSYM: a function the stripped image has
// no symbol for.
__attribute__((noinline)) int FixtureFaultingFunction(int x)
{
    return *gNowhere + x;   // The fault.
}

std::atomic<bool> gWorkerWaits{ false };

// What the report must name on the other thread, by the dSYM.
__attribute__((noinline)) void FixtureWaitingFunction()
{
    gWorkerWaits = true;
    for (;;) pause();   // The worker waits.
}

void* FixtureWorker(void*)
{
    pthread_setname_np("bg3mse fixture worker");
    FixtureWaitingFunction();
    return nullptr;
}

}

constinit crash::Host const bg3se::mac::crash::gHost{
    .ReportDirectory = ReportDirectory,
};

void bg3se::mac::crash::LaunchCrashReporter(char const* reportPath)
{
    fprintf(stderr, "report: %s\n", reportPath);
}

// The fixture's one exported function: starts the reporter with `directory`
// for its reports, and faults.
extern "C" __attribute__((visibility("default"))) int bg3mse_release_fixture_crash(char const* directory)
{
    snprintf(gDirectory, sizeof(gDirectory), "%s", directory);
    if (!crash::Start()) return 1;
    pthread_t worker;
    if (pthread_create(&worker, nullptr, FixtureWorker, nullptr) != 0) return 2;
    while (!gWorkerWaits) usleep(1000);
    int result = FixtureFaultingFunction(1);
    // Not a tail call: this function is a frame of the backtrace.
    __asm__ volatile("" ::: "memory");
    return result;
}
