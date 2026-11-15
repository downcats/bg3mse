#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>

// An HTTP server on 127.0.0.1, on a port the system picks, for the
// updater's tests: each path is answered as its route says.

namespace bg3se::mac::updater::test {

struct Route
{
    enum class Mode
    {
        Normal,       // 200 and the body
        Drip,         // 200, then one byte every DripMs
        Stall,        // 200 and the headers, then nothing until the server stops
        Redirect,     // 302 to Location
        NotFound,     // 404
        Close,        // closes the connection without answering
    };
    Mode How{ Mode::Normal };
    std::string Body;
    std::string Location;
    int DripMs{ 200 };
    // The Content-Length announced; the body's size when not set.
    long long Length{ -1 };
};

class Server
{
public:
    bool Start();
    int Port() const { return port_; }
    std::string URL(std::string const& path) const;
    void Set(std::string const& path, Route route);
    void Clear();
    int Hits(std::string const& path);

private:
    void Serve(int fd);
    int listen_{ -1 };
    int port_{ 0 };
    std::mutex lock_;
    std::map<std::string, Route> routes_;
    std::map<std::string, int> hits_;
};

}
