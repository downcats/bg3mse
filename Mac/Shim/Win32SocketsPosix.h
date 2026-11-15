// The BSD socket calls Win32Sockets.cpp is built on, with plain types so that
// the Winsock declarations and the BSD ones never meet in one translation unit.
#pragma once

#include <cstdint>

namespace bg3mse::posix {

// Each returns the BSD result; on failure, the error is the Winsock code for errno.
int Socket(int family, int type, int protocol, int& error);
// IPv4 only: `address` and `port` in network byte order. As on Windows, the
// port of a listener that was closed can be bound at once, and a port that
// has a listener cannot.
int BindIPv4(int fd, uint32_t address, uint16_t port, int& error);
int Listen(int fd, int backlog, int& error);
int AcceptIPv4(int fd, uint32_t& address, uint16_t& port, int& error);
long Recv(int fd, void* buf, unsigned long length, int flags, int& error);
// Never raises SIGPIPE: a send to a peer that has gone fails with WSAECONNRESET.
long Send(int fd, void const* buf, unsigned long length, int flags, int& error);
int Close(int fd, int& error);
// 1 parsed, 0 not an IPv4 address.
int ParseIPv4(char const* text, uint32_t& address);

}
