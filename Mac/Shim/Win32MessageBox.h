// What Mac/Shim/Win32MessageBox.cpp makes of a MessageBox call, in two parts
// that can be examined without showing anything: the alert to ask the system
// for, and the value MessageBox returns for the system's answer.
#pragma once

#include <windows.h>

#include <string>

namespace bg3se::shim {

struct MessageBoxAlert
{
    // False for a type MessageBox itself refuses (an unknown set of buttons).
    bool Valid{ false };
    // CFUserNotification's alert level: stop, note, caution or plain.
    unsigned long Level{ 0 };
    std::string Caption;
    std::string Text;
    // The alert's default, alternate and other button, nullptr where there
    // is none, and what MessageBox returns for each (IDOK, IDCANCEL, ...).
    char const* Buttons[3]{};
    int Results[3]{};
    // What MessageBox returns when the alert goes away without a button:
    // IDCANCEL where there is a Cancel button, the default button otherwise.
    int Dismissed{ 0 };
};

MessageBoxAlert BuildMessageBoxAlert(char const* text, char const* caption, UINT type);

// `response` is what CFUserNotificationDisplayAlert stored in its response flags.
int MessageBoxResult(MessageBoxAlert const& alert, unsigned long response);

}
