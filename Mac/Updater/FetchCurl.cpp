// A transfer over IPv4 only (FetchOptions::IPv4Only): the system's libcurl,
// as Norbyte's HttpFetcher uses libcurl (BG3Updater/HttpFetcher.cpp), with
// CURL_IPRESOLVE_V4. Everything else is as Fetch.mm does it with the
// system's URL loading: one GET into memory, the whole transfer's deadline
// and the silence's, the cap on bytes, a status other than 200 refused,
// redirects followed only to http and https, cancelling, the same results.
// The system's libcurl checks certificates against the system's trust store.

// The options of the libcurl macOS 11 has (7.64): the newer names of some
// of them are not there.
#define CURL_DISABLE_DEPRECATION 1

#include <Updater/Fetch.h>

#include <Platform/Log.h>

#include <curl/curl.h>

#include <mutex>

namespace bg3se::mac::updater {

namespace {

struct Transfer
{
    CURL* Curl{ nullptr };
    std::vector<uint8_t>* Body{ nullptr };
    uint64_t MaxBytes{ 0 };
    std::atomic<bool> const* Cancel{ nullptr };
    bool Cancelled{ false };
    bool TooLarge{ false };
    bool BadStatus{ false };
    long Status{ 0 };
};

size_t Write(char* data, size_t size, size_t count, void* user)
{
    auto& t = *static_cast<Transfer*>(user);
    size_t bytes = size * count;
    long status = 0;
    curl_easy_getinfo(t.Curl, CURLINFO_RESPONSE_CODE, &status);
    t.Status = status;
    if (status != 200) {
        t.BadStatus = true;
        return 0;
    }
    curl_off_t announced = -1;
    curl_easy_getinfo(t.Curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &announced);
    if ((announced > 0 && (uint64_t)announced > t.MaxBytes) || t.Body->size() + bytes > t.MaxBytes) {
        t.TooLarge = true;
        return 0;
    }
    t.Body->insert(t.Body->end(), reinterpret_cast<uint8_t*>(data), reinterpret_cast<uint8_t*>(data) + bytes);
    return bytes;
}

int Progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto& t = *static_cast<Transfer*>(user);
    if (t.Cancel && t.Cancel->load()) {
        t.Cancelled = true;
        return 1;
    }
    return 0;
}

// CURLOPT_VERBOSE's lines, as DEBUG lines (Norbyte's HttpFetcher::DebugFunc
// prints the text and the headers; the bodies are left out).
int Trace(CURL*, curl_infotype type, char* data, size_t size, void*)
{
    char const* prefix = nullptr;
    switch (type) {
    case CURLINFO_TEXT: prefix = "*"; break;
    case CURLINFO_HEADER_IN: prefix = "<"; break;
    case CURLINFO_HEADER_OUT: prefix = ">"; break;
    default: return 0;
    }
    std::string_view text(data, size);
    while (!text.empty()) {
        size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) Log(LogLevel::Debug, "fetch: %s %.*s", prefix, (int)line.size(), line.data());
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    return 0;
}

}

FetchResult FetchOverCurl(std::string const& url, FetchLimits const& limits, std::vector<uint8_t>& body, std::atomic<bool> const* cancel,
    FetchOptions const& options)
{
    static std::once_flag initialized;
    std::call_once(initialized, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    body.clear();
    FetchResult result;
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        result.Failure = FetchFailure::Invalid;
        result.Message = "'" + url + "' is not an http or https URL";
        return result;
    }

    Transfer t;
    t.Curl = curl_easy_init();
    if (!t.Curl) {
        result.Failure = FetchFailure::Connection;
        result.Message = "libcurl could not start a transfer";
        return result;
    }
    t.Body = &body;
    t.MaxBytes = limits.MaxBytes;
    t.Cancel = cancel;
    char error[CURL_ERROR_SIZE] = {};

    CURL* c = t.Curl;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 20L);
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_USERAGENT, "bg3mse-updater");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)(limits.Total * 1000));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, (long)(limits.Total * 1000));
    // Less than a byte a second for `Idle` seconds: silence.
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, (long)(limits.Idle < 1 ? 1 : limits.Idle));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &Write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, &Progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, error);
    if (limits.MaxBytes) curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)limits.MaxBytes);
    if (options.Trace) {
        curl_easy_setopt(c, CURLOPT_VERBOSE, 1L);
        curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, &Trace);
    }

    CURLcode code = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    result.Status = status ? status : t.Status;
    if (code == CURLE_OK && result.Status != 200) t.BadStatus = true;

    if (t.Cancelled) {
        result.Failure = FetchFailure::Cancelled;
        result.Message = "cancelled";
    } else if (t.BadStatus) {
        result.Failure = FetchFailure::Http;
        result.Message = "the server answered HTTP " + std::to_string(result.Status);
    } else if (t.TooLarge || code == CURLE_FILESIZE_EXCEEDED) {
        result.Failure = FetchFailure::TooLarge;
        result.Message = "the server sent more than the " + std::to_string(limits.MaxBytes) + " bytes expected";
    } else if (code == CURLE_OPERATION_TIMEDOUT) {
        result.Failure = FetchFailure::TimedOut;
        result.Message = "no answer within the time allowed";
    } else if (code == CURLE_UNSUPPORTED_PROTOCOL) {
        result.Failure = FetchFailure::Invalid;
        result.Message = "the server redirected to something that is not http or https";
    } else if (code != CURLE_OK) {
        result.Failure = FetchFailure::Connection;
        result.Message = error[0] ? error : curl_easy_strerror(code);
    }
    curl_easy_cleanup(c);
    if (result.Failure != FetchFailure::None) body.clear();
    return result;
}

}
