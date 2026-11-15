#include <Updater/Notice.h>

#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <chrono>

namespace bg3se::mac::updater {

namespace {

CFStringRef MakeString(std::string const& text)
{
    auto bytes = reinterpret_cast<UInt8 const*>(text.data());
    auto length = static_cast<CFIndex>(text.size());
    if (auto string = CFStringCreateWithBytes(kCFAllocatorDefault, bytes, length, kCFStringEncodingUTF8, false)) return string;
    return CFStringCreateWithBytes(kCFAllocatorDefault, bytes, length, kCFStringEncodingISOLatin1, false);
}

// Norbyte's progress window (a task dialog): the title "Script Extender
// Updater", the heading, the status as its text, a marquee progress bar and
// a Cancel button. A notification has no title bar: the heading is its title.
CFDictionaryRef ProgressDictionary(std::string const& status)
{
    CFStringRef heading = MakeString(kProgressHeading);
    CFStringRef message = MakeString(status);
    CFStringRef cancel = MakeString("Cancel");
    void const* keys[] = { kCFUserNotificationAlertHeaderKey, kCFUserNotificationAlertMessageKey,
        kCFUserNotificationProgressIndicatorValueKey, kCFUserNotificationDefaultButtonTitleKey };
    void const* values[] = { heading, message, kCFBooleanTrue, cancel };
    CFDictionaryRef dictionary = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 4, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFRelease(heading);
    CFRelease(message);
    CFRelease(cancel);
    return dictionary;
}

class System : public NoticeDisplay
{
public:
    ~System() override
    {
        if (note_) CFRelease(note_);
    }

    bool OpenProgress(std::string const& status) override
    {
        CFDictionaryRef dictionary = ProgressDictionary(status);
        SInt32 error = 0;
        note_ = CFUserNotificationCreate(kCFAllocatorDefault, 0, kCFUserNotificationPlainAlertLevel, &error, dictionary);
        CFRelease(dictionary);
        return note_ != nullptr && error == 0;
    }

    void UpdateProgress(std::string const& status) override
    {
        if (!note_) return;
        CFDictionaryRef dictionary = ProgressDictionary(status);
        CFUserNotificationUpdate(note_, 0, kCFUserNotificationPlainAlertLevel, dictionary);
        CFRelease(dictionary);
    }

    bool WaitProgress() override
    {
        if (!note_) return false;
        // In short waits, so that a CloseProgress the system did not pass on
        // still ends the wait.
        for (;;) {
            CFOptionFlags response = 0;
            if (CFUserNotificationReceiveResponse(note_, 0.25, &response) == 0) {
                return !closed_ && (response & 0x3) == kCFUserNotificationDefaultResponse;
            }
            if (closed_) return false;
        }
    }

    void CloseProgress() override
    {
        closed_ = true;
        if (note_) CFUserNotificationCancel(note_);
    }

    void Box(std::string const& title, std::string const& text, bool warning) override
    {
        CFStringRef header = MakeString(title);
        CFStringRef message = MakeString(text);
        CFOptionFlags response = 0;
        CFUserNotificationDisplayAlert(0, warning ? kCFUserNotificationCautionAlertLevel : kCFUserNotificationStopAlertLevel, nullptr,
            nullptr, nullptr, header, message, nullptr, nullptr, nullptr, &response);
        CFRelease(header);
        CFRelease(message);
    }

private:
    CFUserNotificationRef note_{ nullptr };
    std::atomic<bool> closed_{ false };
};

}

std::unique_ptr<NoticeDisplay> SystemDisplay()
{
    return std::make_unique<System>();
}

ProgressNotice::ProgressNotice(NoticeDisplay& display, std::function<void()> cancel)
    : display_(display), cancel_(std::move(cancel))
{}

ProgressNotice::~ProgressNotice()
{
    Hide();
    if (thread_.joinable()) thread_.join();
}

void ProgressNotice::Show(std::string const& status)
{
    std::lock_guard guard(lock_);
    if (thread_.joinable() || hidden_) return;
    status_ = status;
    thread_ = std::thread([this] { Run(); });
}

void ProgressNotice::SetStatus(std::string const& status)
{
    std::lock_guard guard(lock_);
    status_ = status;
    if (open_ && !hidden_) display_.UpdateProgress(status_);
}

void ProgressNotice::Hide()
{
    std::lock_guard guard(lock_);
    if (hidden_) return;
    hidden_ = true;
    changed_.notify_all();
    if (open_) display_.CloseProgress();
}

void ProgressNotice::Run()
{
    {
        std::unique_lock guard(lock_);
        // Norbyte's UI thread sleeps a second before it shows the window.
        changed_.wait_for(guard, std::chrono::seconds(1), [this] { return hidden_; });
        if (hidden_) return;
        open_ = display_.OpenProgress(status_);
        if (!open_) return;
    }
    bool cancelled = display_.WaitProgress();
    std::function<void()> cancel;
    {
        std::lock_guard guard(lock_);
        open_ = false;
        if (cancelled && !hidden_) cancel = cancel_;
    }
    if (cancel) cancel();
}

}
