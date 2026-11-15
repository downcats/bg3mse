// WS2tcpip.h: address conversion (Win32Sockets.cpp).
#pragma once

#include <WinSock2.h>

// AF_INET only (anything else fails with WSAEAFNOSUPPORT). 1 on success, 0 if
// the text is not a dotted-quad address, -1 on error.
int inet_pton(int family, const char* text, void* address);
