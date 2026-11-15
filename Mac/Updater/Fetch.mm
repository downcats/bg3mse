#include <Updater/Fetch.h>

#include <Platform/Log.h>

#import <Foundation/Foundation.h>

#include <chrono>
#include <mutex>
#include <thread>

using namespace bg3se::mac::updater;

// The one Objective-C class of the loader; no other image defines it.
@interface BG3MSEUpdaterFetch : NSObject <NSURLSessionDataDelegate>
{
@public
    std::mutex lock;
    std::vector<uint8_t>* body;
    uint64_t maxBytes;
    FetchResult result;
    bool tooLarge;
    bool badStatus;
    bool badRedirect;
    bool trace;
    std::chrono::steady_clock::time_point lastByte;
    dispatch_semaphore_t done;
}
@end

@implementation BG3MSEUpdaterFetch

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveResponse:(NSURLResponse*)response
    completionHandler:(void (^)(NSURLSessionResponseDisposition))handler
{
    long status = [response isKindOfClass:[NSHTTPURLResponse class]] ? (long)((NSHTTPURLResponse*)response).statusCode : 0;
    if (trace) {
        bg3se::mac::Log(bg3se::mac::LogLevel::Debug, "fetch: HTTP %ld from %s, %lld bytes announced", status,
            response.URL.absoluteString.UTF8String ?: "?", (long long)response.expectedContentLength);
        if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
            [((NSHTTPURLResponse*)response).allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL*) {
                bg3se::mac::Log(bg3se::mac::LogLevel::Debug, "fetch: < %s: %s", [[key description] UTF8String] ?: "",
                    [[value description] UTF8String] ?: "");
            }];
        }
    }
    std::lock_guard guard(lock);
    result.Status = status;
    lastByte = std::chrono::steady_clock::now();
    if (status != 200) {
        badStatus = true;
        handler(NSURLSessionResponseCancel);
        return;
    }
    if (response.expectedContentLength > 0 && (uint64_t)response.expectedContentLength > maxBytes) {
        tooLarge = true;
        handler(NSURLSessionResponseCancel);
        return;
    }
    handler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data
{
    std::lock_guard guard(lock);
    lastByte = std::chrono::steady_clock::now();
    if (tooLarge) return;
    if (body->size() + data.length > maxBytes) {
        tooLarge = true;
        [task cancel];
        return;
    }
    [data enumerateByteRangesUsingBlock:^(void const* bytes, NSRange range, BOOL*) {
        auto const* p = static_cast<uint8_t const*>(bytes);
        body->insert(body->end(), p, p + range.length);
    }];
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task willPerformHTTPRedirection:(NSHTTPURLResponse*)response
    newRequest:(NSURLRequest*)request completionHandler:(void (^)(NSURLRequest*))handler
{
    NSString* scheme = request.URL.scheme.lowercaseString;
    if (trace) {
        bg3se::mac::Log(bg3se::mac::LogLevel::Debug, "fetch: HTTP %ld redirects to %s", (long)response.statusCode,
            request.URL.absoluteString.UTF8String ?: "?");
    }
    if (![scheme isEqualToString:@"https"] && ![scheme isEqualToString:@"http"]) {
        std::lock_guard guard(lock);
        badRedirect = true;
        handler(nil);
        return;
    }
    handler(request);
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didFinishCollectingMetrics:(NSURLSessionTaskMetrics*)metrics
{
    if (!trace) return;
    for (NSURLSessionTaskTransactionMetrics* m in metrics.transactionMetrics) {
        NSString* address = nil;
        if (@available(macOS 10.15, *)) address = m.remoteAddress;
        bg3se::mac::Log(bg3se::mac::LogLevel::Debug, "fetch: %s %s over %s, connected to %s%s",
            m.request.HTTPMethod.UTF8String ?: "GET", m.request.URL.absoluteString.UTF8String ?: "?",
            m.networkProtocolName.UTF8String ?: "?", address.UTF8String ?: "?", m.reusedConnection ? " (connection reused)" : "");
    }
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error
{
    {
        std::lock_guard guard(lock);
        if (error && result.Failure == FetchFailure::None && !badStatus && !tooLarge) {
            if ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorTimedOut) {
                result.Failure = FetchFailure::TimedOut;
            } else if ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled) {
                result.Failure = FetchFailure::Cancelled;
            } else {
                result.Failure = FetchFailure::Connection;
            }
            result.Message = error.localizedDescription.UTF8String ?: "unknown error";
        }
    }
    dispatch_semaphore_signal(done);
}

@end

namespace bg3se::mac::updater {

std::string ResolveURL(std::string const& base, std::string const& reference)
{
    @autoreleasepool {
        NSURL* baseURL = [NSURL URLWithString:@(base.c_str())];
        NSURL* url = baseURL ? [NSURL URLWithString:@(reference.c_str()) relativeToURL:baseURL]
                             : [NSURL URLWithString:@(reference.c_str())];
        if (!url) return {};
        url = url.absoluteURL;
        NSString* scheme = url.scheme.lowercaseString;
        if (!url.host.length || (![scheme isEqualToString:@"https"] && ![scheme isEqualToString:@"http"])) return {};
        return url.absoluteString.UTF8String ?: "";
    }
}

namespace {

FetchResult FetchOverSession(std::string const& url, FetchLimits const& limits, std::vector<uint8_t>& body, std::atomic<bool> const* cancel,
    FetchOptions const& options)
{
    body.clear();
    FetchResult result;
    @autoreleasepool {
        NSURL* nsurl = [NSURL URLWithString:@(url.c_str())];
        NSString* scheme = nsurl.scheme.lowercaseString;
        if (!nsurl || !nsurl.host.length || (![scheme isEqualToString:@"https"] && ![scheme isEqualToString:@"http"])) {
            result.Failure = FetchFailure::Invalid;
            result.Message = "'" + url + "' is not an http or https URL";
            return result;
        }
        NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.timeoutIntervalForRequest = limits.Idle;
        config.timeoutIntervalForResource = limits.Total;
        config.requestCachePolicy = NSURLRequestReloadIgnoringLocalAndRemoteCacheData;
        config.URLCache = nil;
        config.HTTPCookieStorage = nil;
        config.HTTPShouldSetCookies = NO;
        config.waitsForConnectivity = NO;

        BG3MSEUpdaterFetch* state = [BG3MSEUpdaterFetch new];
        state->body = &body;
        state->maxBytes = limits.MaxBytes;
        state->trace = options.Trace;
        state->done = dispatch_semaphore_create(0);
        state->lastByte = std::chrono::steady_clock::now();
        NSOperationQueue* queue = [NSOperationQueue new];
        queue.maxConcurrentOperationCount = 1;
        NSURLSession* session = [NSURLSession sessionWithConfiguration:config delegate:state delegateQueue:queue];
        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:nsurl];
        [request setValue:@"bg3mse-updater" forHTTPHeaderField:@"User-Agent"];
        NSURLSessionDataTask* task = [session dataTaskWithRequest:request];
        [task resume];

        // The deadlines are kept here as well as by the session, whose
        // request timeout restarts at every byte: a server that sends one
        // byte a second would otherwise never time out.
        auto start = std::chrono::steady_clock::now();
        auto total = std::chrono::duration<double>(limits.Total);
        auto idle = std::chrono::duration<double>(limits.Idle);
        bool timedOut = false, cancelled = false;
        while (dispatch_semaphore_wait(state->done, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) != 0) {
            auto now = std::chrono::steady_clock::now();
            std::chrono::steady_clock::time_point last;
            {
                std::lock_guard guard(state->lock);
                last = state->lastByte;
            }
            if (now - start > total || now - last > idle) timedOut = true;
            if (cancel && cancel->load()) cancelled = true;
            if (timedOut || cancelled) {
                [task cancel];
                dispatch_semaphore_wait(state->done, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
                break;
            }
        }
        [session invalidateAndCancel];

        std::lock_guard guard(state->lock);
        result = state->result;
        if (timedOut) {
            result.Failure = FetchFailure::TimedOut;
            result.Message = "no answer within the time allowed";
        } else if (cancelled) {
            result.Failure = FetchFailure::Cancelled;
            result.Message = "cancelled";
        } else if (state->badStatus) {
            result.Failure = FetchFailure::Http;
            result.Message = "the server answered HTTP " + std::to_string(result.Status);
        } else if (state->tooLarge) {
            result.Failure = FetchFailure::TooLarge;
            result.Message = "the server sent more than the " + std::to_string(limits.MaxBytes) + " bytes expected";
        } else if (state->badRedirect) {
            result.Failure = FetchFailure::Invalid;
            result.Message = "the server redirected to something that is not http or https";
        }
        state->body = nullptr;
        if (result.Failure != FetchFailure::None) body.clear();
    }
    return result;
}


}

FetchResult Fetch(std::string const& url, FetchLimits const& limits, std::vector<uint8_t>& body, std::atomic<bool> const* cancel,
    FetchOptions const& options)
{
    if (options.Trace) {
        Log(LogLevel::Debug, "fetch: GET %s (%.0f s in all, %.0f s without data, IPv4 only %s)", url.c_str(), limits.Total, limits.Idle,
            options.IPv4Only ? "on" : "off");
    }
    FetchResult result = options.IPv4Only ? FetchOverCurl(url, limits, body, cancel, options) : FetchOverSession(url, limits, body, cancel, options);
    if (options.Trace) {
        if (result.Failure == FetchFailure::None) {
            Log(LogLevel::Debug, "fetch: %zu bytes from %s", body.size(), url.c_str());
        } else {
            Log(LogLevel::Debug, "fetch: %s failed: %s", url.c_str(), result.Message.c_str());
        }
    }
    return result;
}

}
