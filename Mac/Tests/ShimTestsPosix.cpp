// The BSD-socket side of ShimTests.cpp's socket test: a loopback client.
#include <arpa/inet.h>
#include <cstdint>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int Connect(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

}

int TestConnectAndSend(uint16_t port, char const* data, int length)
{
    int fd = Connect(port);
    if (fd < 0) return -1;
    auto n = send(fd, data, (size_t)length, 0);
    close(fd);
    return (int)n;
}

// Connects, then resets the connection (SO_LINGER 0) so the server's sends fail.
int TestConnectAndClose(uint16_t port)
{
    int fd = Connect(port);
    if (fd < 0) return -1;
    linger l{ 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
    close(fd);
    return 0;
}

// Connects and stays until the server has closed the connection, so that the
// server's side is the one that closed first.
int TestConnectAndWaitForClose(uint16_t port)
{
    int fd = Connect(port);
    if (fd < 0) return -1;
    char byte;
    auto n = recv(fd, &byte, 1, 0);
    close(fd);
    return (int)n;
}
