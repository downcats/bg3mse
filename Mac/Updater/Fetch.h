#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// HTTP(S) with the system's own stack (NSURLSession: the system's TLS and
// trust store, proxies and IPv4/IPv6 racing), in the shape of Norbyte's
// HttpFetcher: one GET into memory, with a deadline for the whole transfer,
// one for a silence, and a cap on what is accepted. Redirects are followed
// (GitHub sends a release asset from another host) only to http or https.

namespace bg3se::mac::updater {

struct FetchLimits
{
    double Total{ 5.0 };        // seconds for the whole transfer, connecting included
    double Idle{ 5.0 };         // seconds without a byte
    uint64_t MaxBytes{ 1 << 20 };
};

enum class FetchFailure
{
    None,
    Connection,     // no network, refused, name not found, TLS: an "internet issue"
    TimedOut,
    Http,           // a status other than 200
    TooLarge,
    Cancelled,
    Invalid,        // not an http(s) URL
};

struct FetchResult
{
    FetchFailure Failure{ FetchFailure::None };
    long Status{ 0 };
    std::string Message;
    bool IsInternetIssue() const { return Failure == FetchFailure::Connection || Failure == FetchFailure::TimedOut; }
};

// Norbyte's HttpFetcher options (BG3Updater/HttpFetcher.cpp).
struct FetchOptions
{
    // IPv4Only: CURLOPT_IPRESOLVE as CURL_IPRESOLVE_V4. The system's URL
    // loading has no such choice (it races IPv4 and IPv6), so such a fetch
    // goes through the system's libcurl instead (FetchCurl.cpp), with the
    // same limits, redirects, cancelling and results.
    bool IPv4Only{ false };
    // Debug: CURLOPT_VERBOSE. Every step of the transfer is a DEBUG line of
    // the log: the request, redirects, the server's address and answer.
    bool Trace{ false };
};

FetchResult Fetch(std::string const& url, FetchLimits const& limits, std::vector<uint8_t>& body, std::atomic<bool> const* cancel = nullptr,
    FetchOptions const& options = {});

// The IPv4-only transfer (FetchCurl.cpp), which Fetch uses for IPv4Only.
FetchResult FetchOverCurl(std::string const& url, FetchLimits const& limits, std::vector<uint8_t>& body, std::atomic<bool> const* cancel,
    FetchOptions const& options);

// `reference` read relative to `base`, as a browser would; empty when the
// result is not an http(s) URL.
std::string ResolveURL(std::string const& base, std::string const& reference);

}
