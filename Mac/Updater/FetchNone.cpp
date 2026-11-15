// Fetch.h for a loader that does not update itself (BG3MSE_SELF_UPDATE=OFF,
// Mac/Updater/Updater.cmake): in place of the system's URL loading
// (Fetch.mm) and libcurl (FetchCurl.cpp), a fetch that opens nothing, so
// that the loader has no way to the network at all.

#include <Updater/Fetch.h>

namespace bg3se::mac::updater {

namespace {

FetchResult Nothing()
{
    FetchResult r;
    r.Failure = FetchFailure::Invalid;
    r.Message = "this bg3mse does not update itself";
    return r;
}

}

FetchResult Fetch(std::string const&, FetchLimits const&, std::vector<uint8_t>& body, std::atomic<bool> const*, FetchOptions const&)
{
    body.clear();
    return Nothing();
}

FetchResult FetchOverCurl(std::string const&, FetchLimits const&, std::vector<uint8_t>& body, std::atomic<bool> const*, FetchOptions const&)
{
    body.clear();
    return Nothing();
}

std::string ResolveURL(std::string const&, std::string const&)
{
    return {};
}

}
