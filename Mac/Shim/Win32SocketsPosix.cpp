#include "Win32SocketsPosix.h"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bg3mse::posix {

namespace {

// Winsock's codes are the BSD errno values plus 10000; EPIPE is a connection
// reset as Windows reports it; anything else is passed through the same way.
int WinsockError(int e)
{
    if (e == EPIPE) return 10054;   // WSAECONNRESET
    if (e == ENOMEM) return 8;      // WSA_NOT_ENOUGH_MEMORY
    return 10000 + e;
}

}

int Socket(int family, int type, int protocol, int& error)
{
    int fd = ::socket(family, type, protocol);
    if (fd < 0) {
        error = WinsockError(errno);
        return -1;
    }
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    return fd;
}

int BindIPv4(int fd, uint32_t address, uint16_t port, int& error)
{
    // A port whose last listener was closed is free on Windows at once. Here
    // a connection that listener accepted and closed first lingers for half a
    // minute (TIME_WAIT) and holds the port against bind unless the socket
    // says otherwise. Two sockets still cannot listen on one port at a time.
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr{};
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = port;
    addr.sin_addr.s_addr = address;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = WinsockError(errno);
        return -1;
    }
    return 0;
}

int Listen(int fd, int backlog, int& error)
{
    if (::listen(fd, backlog) != 0) {
        error = WinsockError(errno);
        return -1;
    }
    return 0;
}

int AcceptIPv4(int fd, uint32_t& address, uint16_t& port, int& error)
{
    sockaddr_in addr{};
    socklen_t length = sizeof(addr);
    int client;
    do {
        client = ::accept(fd, reinterpret_cast<sockaddr*>(&addr), &length);
    } while (client < 0 && errno == EINTR);
    if (client < 0) {
        error = WinsockError(errno);
        return -1;
    }
    int on = 1;
    ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    address = addr.sin_addr.s_addr;
    port = addr.sin_port;
    return client;
}

long Recv(int fd, void* buf, unsigned long length, int flags, int& error)
{
    ssize_t n;
    do {
        n = ::recv(fd, buf, length, flags);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        error = WinsockError(errno);
        return -1;
    }
    return (long)n;
}

long Send(int fd, void const* buf, unsigned long length, int flags, int& error)
{
    ssize_t n;
    do {
        n = ::send(fd, buf, length, flags);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        error = WinsockError(errno);
        return -1;
    }
    return (long)n;
}

int Close(int fd, int& error)
{
    if (::close(fd) != 0) {
        error = WinsockError(errno);
        return -1;
    }
    return 0;
}

int ParseIPv4(char const* text, uint32_t& address)
{
    in_addr addr{};
    if (::inet_pton(AF_INET, text, &addr) != 1) {
        return 0;
    }
    address = addr.s_addr;
    return 1;
}

}
