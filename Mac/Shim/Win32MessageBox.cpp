// MessageBoxA / MessageBoxW: a modal alert of the system's, with the text and
// buttons the call asks for, that returns the button the user chose.
//
// The alert is shown by the system on the process's behalf
// (CFUserNotificationDisplayAlert): it needs no window of the process and no
// run loop, can be asked for from any thread, appears above a full-screen
// game, and the call returns when the user has answered. Norbyte's callers
// are Fail() (CoreLib/Utils.cpp), from whatever thread failed, and the
// fallback of LibraryManager::ShowStartupError, from a thread of its own.
//
// What is kept of the type: the buttons, which of them is the default, and
// the icon as the alert's level. The owner window and the modality flags
// have no meaning for an alert that is not a window of the process. Text is
// UTF-8 (what this layer's "ANSI" strings are).

#include <Win32MessageBox.h>

#include <CoreFoundation/CoreFoundation.h>

namespace bg3se::shim {

namespace {

struct Button
{
    char const* Title;
    int Result;
};

// The buttons of each type, in the order Windows shows them.
constexpr Button kOk{ "OK", IDOK }, kCancel{ "Cancel", IDCANCEL }, kAbort{ "Abort", IDABORT }, kRetry{ "Retry", IDRETRY },
    kIgnore{ "Ignore", IDIGNORE }, kYes{ "Yes", IDYES }, kNo{ "No", IDNO }, kTryAgain{ "Try Again", IDTRYAGAIN },
    kContinue{ "Continue", IDCONTINUE }, kNone{ nullptr, 0 };
constexpr Button kLayouts[][3] = {
    { kOk, kNone, kNone },              // MB_OK
    { kOk, kCancel, kNone },            // MB_OKCANCEL
    { kAbort, kRetry, kIgnore },        // MB_ABORTRETRYIGNORE
    { kYes, kNo, kCancel },             // MB_YESNOCANCEL
    { kYes, kNo, kNone },               // MB_YESNO
    { kRetry, kCancel, kNone },         // MB_RETRYCANCEL
    { kCancel, kTryAgain, kContinue },  // MB_CANCELTRYCONTINUE
};

CFStringRef MakeString(std::string const& text)
{
    auto bytes = reinterpret_cast<UInt8 const*>(text.data());
    auto length = static_cast<CFIndex>(text.size());
    if (auto string = CFStringCreateWithBytes(kCFAllocatorDefault, bytes, length, kCFStringEncodingUTF8, false)) return string;
    // Not UTF-8: every byte is a character of Latin-1.
    return CFStringCreateWithBytes(kCFAllocatorDefault, bytes, length, kCFStringEncodingISOLatin1, false);
}

}

MessageBoxAlert BuildMessageBoxAlert(char const* text, char const* caption, UINT type)
{
    MessageBoxAlert alert;
    unsigned layout = type & 0xF;
    if (layout >= std::size(kLayouts)) return alert;
    alert.Valid = true;
    alert.Text = text ? text : "";
    // MessageBox titles a box without a caption "Error".
    alert.Caption = caption ? caption : "Error";

    switch (type & 0xF0) {
    case MB_ICONERROR: alert.Level = kCFUserNotificationStopAlertLevel; break;
    case MB_ICONWARNING: alert.Level = kCFUserNotificationCautionAlertLevel; break;
    case MB_ICONQUESTION:
    case MB_ICONINFORMATION: alert.Level = kCFUserNotificationNoteAlertLevel; break;
    default: alert.Level = kCFUserNotificationPlainAlertLevel; break;
    }

    // The default button first (MB_DEFBUTTON2 and 3 name another than the
    // first; one that the type does not have leaves the first), then the
    // others in their order.
    auto const& buttons = kLayouts[layout];
    unsigned chosen = (type & 0xF00) == MB_DEFBUTTON2 ? 1 : (type & 0xF00) == MB_DEFBUTTON3 ? 2 : 0;
    if (!buttons[chosen].Title) chosen = 0;
    unsigned slot = 0;
    alert.Buttons[slot] = buttons[chosen].Title;
    alert.Results[slot++] = buttons[chosen].Result;
    for (unsigned i = 0; i < 3; i++) {
        if (i == chosen || !buttons[i].Title) continue;
        alert.Buttons[slot] = buttons[i].Title;
        alert.Results[slot++] = buttons[i].Result;
    }

    alert.Dismissed = alert.Results[0];
    for (auto const& button : buttons) {
        if (button.Result == IDCANCEL) alert.Dismissed = IDCANCEL;
    }
    return alert;
}

int MessageBoxResult(MessageBoxAlert const& alert, unsigned long response)
{
    switch (response & 0x3) {
    case kCFUserNotificationDefaultResponse: return alert.Results[0];
    case kCFUserNotificationAlternateResponse: return alert.Buttons[1] ? alert.Results[1] : alert.Dismissed;
    case kCFUserNotificationOtherResponse: return alert.Buttons[2] ? alert.Results[2] : alert.Dismissed;
    default: return alert.Dismissed;
    }
}

}

// 0, as MessageBox returns when it fails: ERROR_INVALID_PARAMETER for a type
// it does not know, ERROR_NOT_SUPPORTED when the system could not show the
// alert (a process outside a login session).
int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type)
{
    auto alert = bg3se::shim::BuildMessageBoxAlert(text, caption, type);
    if (!alert.Valid) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }

    CFStringRef strings[5]{};
    strings[0] = bg3se::shim::MakeString(alert.Caption);
    strings[1] = bg3se::shim::MakeString(alert.Text);
    for (int i = 0; i < 3; i++) {
        if (alert.Buttons[i]) strings[2 + i] = CFStringCreateWithCString(kCFAllocatorDefault, alert.Buttons[i], kCFStringEncodingUTF8);
    }
    CFOptionFlags response = 0;
    SInt32 error = CFUserNotificationDisplayAlert(0, alert.Level, nullptr, nullptr, nullptr, strings[0], strings[1], strings[2],
        strings[3], strings[4], &response);
    for (auto string : strings) {
        if (string) CFRelease(string);
    }
    if (error != 0) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    return bg3se::shim::MessageBoxResult(alert, response);
}

int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type)
{
    auto narrow = [](LPCWSTR wide, std::string& out) -> char const* {
        if (!wide) return nullptr;
        int count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        out.resize(static_cast<size_t>(count));
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), count, nullptr, nullptr);
        return out.c_str();
    };
    std::string textUtf8, captionUtf8;
    char const* textPointer = narrow(text, textUtf8);
    char const* captionPointer = narrow(caption, captionUtf8);
    return MessageBoxA(owner, textPointer, captionPointer, type);
}
