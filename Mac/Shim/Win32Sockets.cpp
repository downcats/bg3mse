// Winsock over BSD sockets (Win32SocketsPosix.cpp). A SOCKET holds the file descriptor.
#include <WinSock2.h>
#include <WS2tcpip.h>

#include "Win32SocketsPosix.h"

#include <atomic>
#include <climits>

namespace {

thread_local int gWsaError = 0;
// WSAStartup calls not yet matched by WSACleanup; sockets need one.
std::atomic<int> gStartups{ 0 };

int Fail(int error)
{
    gWsaError = error;
    return SOCKET_ERROR;
}

bool ToFd(SOCKET s, int& fd)
{
    if (s == INVALID_SOCKET || s > (SOCKET)INT_MAX) {
        gWsaError = WSAENOTSOCK;
        return false;
    }
    fd = (int)s;
    return true;
}

}

int WSAStartup(WORD versionRequested, LPWSADATA data)
{
    if (data == nullptr) {
        return WSAEFAULT;
    }
    // Windows answers any request for 2.0 or later with 2.2.
    if ((versionRequested & 0xff) < 2) {
        data->wVersion = MAKEWORD(2, 2);
        data->wHighVersion = MAKEWORD(2, 2);
        return WSAVERNOTSUPPORTED;
    }
    memset(data, 0, sizeof(*data));
    data->wVersion = MAKEWORD(2, 2);
    data->wHighVersion = MAKEWORD(2, 2);
    strcpy(data->szDescription, "WinSock 2.0");
    strcpy(data->szSystemStatus, "Running");
    gStartups++;
    return 0;
}

int WSACleanup()
{
    int n = gStartups.load();
    while (n > 0 && !gStartups.compare_exchange_weak(n, n - 1)) {}
    if (n == 0) {
        gWsaError = WSANOTINITIALISED;
        return SOCKET_ERROR;
    }
    return 0;
}

int WSAGetLastError()
{
    return gWsaError;
}

SOCKET socket(int family, int type, int protocol)
{
    if (gStartups.load() == 0) {
        gWsaError = WSANOTINITIALISED;
        return INVALID_SOCKET;
    }
    if (family != AF_INET) {
        gWsaError = WSAEAFNOSUPPORT;
        return INVALID_SOCKET;
    }
    int error = 0;
    int fd = bg3mse::posix::Socket(family, type, protocol, error);
    if (fd < 0) {
        gWsaError = error;
        return INVALID_SOCKET;
    }
    return (SOCKET)fd;
}

int bind(SOCKET s, const struct sockaddr* name, int nameLength)
{
    int fd;
    if (!ToFd(s, fd)) return SOCKET_ERROR;
    if (name == nullptr || nameLength < (int)sizeof(sockaddr_in)) return Fail(WSAEFAULT);
    auto in = reinterpret_cast<sockaddr_in const*>(name);
    if (in->sin_family != AF_INET) return Fail(WSAEAFNOSUPPORT);
    int error = 0;
    if (bg3mse::posix::BindIPv4(fd, in->sin_addr.S_un.S_addr, in->sin_port, error) != 0) return Fail(error);
    return 0;
}

int listen(SOCKET s, int backlog)
{
    int fd;
    if (!ToFd(s, fd)) return SOCKET_ERROR;
    int error = 0;
    if (bg3mse::posix::Listen(fd, backlog, error) != 0) return Fail(error);
    return 0;
}

SOCKET accept(SOCKET s, struct sockaddr* addr, int* addrLength)
{
    int fd;
    if (!ToFd(s, fd)) return INVALID_SOCKET;
    if (addr != nullptr && (addrLength == nullptr || *addrLength < (int)sizeof(sockaddr_in))) {
        gWsaError = WSAEFAULT;
        return INVALID_SOCKET;
    }
    uint32_t address = 0;
    uint16_t port = 0;
    int error = 0;
    int client = bg3mse::posix::AcceptIPv4(fd, address, port, error);
    if (client < 0) {
        gWsaError = error;
        return INVALID_SOCKET;
    }
    if (addr != nullptr) {
        sockaddr_in in{};
        in.sin_family = AF_INET;
        in.sin_port = port;
        in.sin_addr.S_un.S_addr = address;
        memcpy(addr, &in, sizeof(in));
        *addrLength = sizeof(in);
    }
    return (SOCKET)client;
}

int recv(SOCKET s, char* buf, int length, int flags)
{
    int fd;
    if (!ToFd(s, fd)) return SOCKET_ERROR;
    if (length < 0) return Fail(WSAEINVAL);
    int error = 0;
    long n = bg3mse::posix::Recv(fd, buf, (unsigned long)length, flags, error);
    if (n < 0) return Fail(error);
    return (int)n;
}

int send(SOCKET s, const char* buf, int length, int flags)
{
    int fd;
    if (!ToFd(s, fd)) return SOCKET_ERROR;
    if (length < 0) return Fail(WSAEINVAL);
    int error = 0;
    long n = bg3mse::posix::Send(fd, buf, (unsigned long)length, flags, error);
    if (n < 0) return Fail(error);
    return (int)n;
}

int closesocket(SOCKET s)
{
    int fd;
    if (!ToFd(s, fd)) return SOCKET_ERROR;
    int error = 0;
    if (bg3mse::posix::Close(fd, error) != 0) return Fail(error);
    return 0;
}

int inet_pton(int family, const char* text, void* address)
{
    if (family != AF_INET) {
        gWsaError = WSAEAFNOSUPPORT;
        return -1;
    }
    if (text == nullptr || address == nullptr) {
        gWsaError = WSAEFAULT;
        return -1;
    }
    uint32_t value = 0;
    if (!bg3mse::posix::ParseIPv4(text, value)) {
        return 0;
    }
    memcpy(address, &value, sizeof(value));
    return 1;
}
