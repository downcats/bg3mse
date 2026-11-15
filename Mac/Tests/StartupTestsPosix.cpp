// The BSD-socket side of the start-up harness's debugger tests: a loopback
// client with receive timeouts. Kept out of StartupTests.cpp, whose headers
// bring in the Windows socket declarations of Mac/Shim.
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

// A connection to 127.0.0.1:port, or -1; each receive gives up after
// timeoutMs.
int DebugClientConnect(uint16_t port, int timeoutMs)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
    return fd;
}

bool DebugClientSend(int fd, void const* data, size_t size)
{
    auto bytes = static_cast<char const*>(data);
    while (size > 0) {
        ssize_t n = send(fd, bytes, size, 0);
        if (n <= 0) return false;
        bytes += n;
        size -= (size_t)n;
    }
    return true;
}

// Exactly size bytes, or false (a timeout, the peer gone).
bool DebugClientReceive(int fd, void* data, size_t size)
{
    auto bytes = static_cast<char*>(data);
    while (size > 0) {
        ssize_t n = recv(fd, bytes, size, 0);
        if (n <= 0) return false;
        bytes += n;
        size -= (size_t)n;
    }
    return true;
}

void DebugClientClose(int fd)
{
    if (fd >= 0) close(fd);
}

// A loopback port nothing listens on at the moment, or 0.
uint16_t DebugClientFreePort()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in addr{};
    addr.sin_len = sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(addr);
    uint16_t port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0
        && getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length) == 0) {
        port = ntohs(addr.sin_port);
    }
    close(fd);
    return port;
}

// --- a child process on pipes: the debug adapter, as an editor runs it ---------

// Starts the program with its standard input and output on pipes (its
// standard error is this process's). Returns its process id, or -1.
int ChildStart(char const* program, int* toChild, int* fromChild)
{
    int in[2], out[2];
    if (pipe(in) != 0) return -1;
    if (pipe(out) != 0) {
        close(in[0]);
        close(in[1]);
        return -1;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    for (int fd : { in[0], in[1], out[0], out[1] }) posix_spawn_file_actions_addclose(&actions, fd);
    char* const argv[] = { const_cast<char*>(program), nullptr };
    pid_t pid = -1;
    int failed = posix_spawn(&pid, program, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in[0]);
    close(out[1]);
    if (failed != 0) {
        close(in[1]);
        close(out[0]);
        return -1;
    }
    // A write to a child that has gone fails; it does not end this process.
    int on = 1;
    fcntl(in[1], F_SETNOSIGPIPE, on);
    *toChild = in[1];
    *fromChild = out[0];
    return (int)pid;
}

bool ChildWrite(int fd, void const* data, size_t size)
{
    auto bytes = static_cast<char const*>(data);
    while (size > 0) {
        ssize_t n = write(fd, bytes, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += n;
        size -= (size_t)n;
    }
    return true;
}

// Up to size bytes of what the child wrote: their number, 0 when it closed
// its output, -1 when nothing came within timeoutMs.
long ChildRead(int fd, void* data, size_t size, int timeoutMs)
{
    pollfd waiting{ fd, POLLIN, 0 };
    int ready;
    do {
        ready = poll(&waiting, 1, timeoutMs);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) return -1;
    ssize_t n;
    do {
        n = read(fd, data, size);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? -1 : (long)n;
}

// Ends the child and closes its pipes.
void ChildStop(int pid, int toChild, int fromChild)
{
    close(toChild);
    close(fromChild);
    if (pid > 0) {
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
    }
}
