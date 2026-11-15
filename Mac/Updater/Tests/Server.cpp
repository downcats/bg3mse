#include "Server.h"

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace bg3se::mac::updater::test {

namespace {

bool SendAll(int fd, char const* p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

}

bool Server::Start()
{
    signal(SIGPIPE, SIG_IGN);
    listen_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_ < 0) return false;
    int one = 1;
    setsockopt(listen_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listen_, 64) != 0) return false;
    socklen_t size = sizeof(addr);
    getsockname(listen_, (sockaddr*)&addr, &size);
    port_ = ntohs(addr.sin_port);
    std::thread([this] {
        for (;;) {
            int fd = accept(listen_, nullptr, nullptr);
            if (fd < 0) continue;
            int on = 1;
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
            std::thread([this, fd] { Serve(fd); }).detach();
        }
    }).detach();
    return true;
}

std::string Server::URL(std::string const& path) const
{
    return "http://127.0.0.1:" + std::to_string(port_) + path;
}

void Server::Set(std::string const& path, Route route)
{
    std::lock_guard guard(lock_);
    routes_[path] = std::move(route);
}

void Server::Clear()
{
    std::lock_guard guard(lock_);
    routes_.clear();
    hits_.clear();
}

int Server::Hits(std::string const& path)
{
    std::lock_guard guard(lock_);
    return hits_[path];
}

void Server::Serve(int fd)
{
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 65536) {
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            close(fd);
            return;
        }
        request.append(buffer, (size_t)n);
    }
    size_t a = request.find(' '), b = request.find(' ', a + 1);
    std::string path = a == std::string::npos || b == std::string::npos ? "" : request.substr(a + 1, b - a - 1);
    Route route;
    {
        std::lock_guard guard(lock_);
        hits_[path]++;
        auto it = routes_.find(path);
        if (it == routes_.end()) route.How = Route::Mode::NotFound;
        else route = it->second;
    }
    long long length = route.Length >= 0 ? route.Length : (long long)route.Body.size();
    std::string head;
    switch (route.How) {
    case Route::Mode::Close:
        close(fd);
        return;
    case Route::Mode::NotFound:
        head = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        SendAll(fd, head.data(), head.size());
        break;
    case Route::Mode::Redirect:
        head = "HTTP/1.1 302 Found\r\nLocation: " + route.Location + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        SendAll(fd, head.data(), head.size());
        break;
    case Route::Mode::Normal:
    case Route::Mode::Drip:
    case Route::Mode::Stall:
        head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(length)
            + "\r\nConnection: close\r\n\r\n";
        if (!SendAll(fd, head.data(), head.size())) break;
        if (route.How == Route::Mode::Normal) {
            SendAll(fd, route.Body.data(), route.Body.size());
        } else if (route.How == Route::Mode::Drip) {
            for (char c : route.Body) {
                if (!SendAll(fd, &c, 1)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(route.DripMs));
            }
        } else {
            // Until the client gives up.
            while (recv(fd, buffer, sizeof(buffer), 0) > 0) {}
        }
        break;
    }
    shutdown(fd, SHUT_WR);
    close(fd);
}

}
