#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// What the player sees of the updater outside the game: Norbyte's UpdaterUI
// (BG3Updater/UI.inl), the window that shows the update's progress, and
// GameHelpers::ShowError (BG3Updater/Utils.cpp), the box for an error the
// extender cannot show in the game, and Fail()'s box (CoreLib/Utils.cpp).
//
// The system shows them on the process's behalf (CFUserNotification, as the
// shim's MessageBoxA does): no window of the process and no run loop, so
// they can be shown before the game has a window, from any thread, above a
// full-screen game. The update runs before the game's main() (Loader.cpp),
// so the game waits while the window is up: the window goes when the update
// ends, or at once when the player cancels it.

namespace bg3se::mac::updater {

// How notices are shown: by the system in the shipped loader; the tests'
// loader writes what would be shown to a file instead.
class NoticeDisplay
{
public:
    virtual ~NoticeDisplay() = default;

    // The progress window, with a Cancel button. False when it cannot be shown.
    virtual bool OpenProgress(std::string const& status) = 0;
    virtual void UpdateProgress(std::string const& status) = 0;
    // Returns when the window has gone: true when the player pressed Cancel,
    // false when CloseProgress took it away.
    virtual bool WaitProgress() = 0;
    // Takes the window away; WaitProgress returns. From any thread.
    virtual void CloseProgress() = 0;

    // A box with an OK button; returns when the player has pressed it.
    virtual void Box(std::string const& title, std::string const& text, bool warning) = 0;
};

// The system's (CFUserNotification).
std::unique_ptr<NoticeDisplay> SystemDisplay();

inline constexpr char kProgressHeading[] = "Checking for bg3mse updates";
inline constexpr char kErrorBoxTitle[] = "bg3mse";
inline constexpr char kFailBoxTitle[] = "bg3mse";

// UpdaterUI: Show() asks for the window, which appears a second later if
// Hide() has not been called by then (an update that ends quickly shows
// nothing); SetStatus changes its text; Cancel in the window calls
// `cancel`, once, unless the window was being taken away.
class ProgressNotice
{
public:
    ProgressNotice(NoticeDisplay& display, std::function<void()> cancel);
    ~ProgressNotice();

    void Show(std::string const& status);
    void SetStatus(std::string const& status);
    void Hide();

private:
    void Run();

    NoticeDisplay& display_;
    std::function<void()> cancel_;
    std::mutex lock_;
    std::condition_variable changed_;
    std::thread thread_;
    std::string status_;
    bool hidden_{ false };
    bool open_{ false };
};

}
