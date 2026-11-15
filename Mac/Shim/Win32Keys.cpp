#include <windows.h>

#include <CoreGraphics/CoreGraphics.h>

// GetAsyncKeyState: whether a key is down now, from the state of the login
// session's keyboard (not of this process's event queue, like the Windows
// function). The modifier keys are answered; for any other key the answer is
// 0, which on Windows also means that the state cannot be told. The low bit
// (pressed since the last call) is never set: Windows documents it as
// unreliable.

short GetAsyncKeyState(int key)
{
    CGEventFlags mask = 0;
    switch (key) {
    case VK_SHIFT: mask = kCGEventFlagMaskShift; break;
    case 0x11 /* VK_CONTROL */: mask = kCGEventFlagMaskControl; break;
    case 0x12 /* VK_MENU */: mask = kCGEventFlagMaskAlternate; break;
    default: return 0;
    }
    CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
    return (flags & mask) != 0 ? static_cast<short>(0x8000) : 0;
}
