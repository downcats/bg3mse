#include <Updater/Files.h>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace bg3se::mac::updater {

std::string Errno()
{
    return strerror(errno);
}

ReadResult ReadFile(std::string const& path, std::string& out, uint64_t limit)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? ReadResult::Missing : ReadResult::Failed;
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return ReadResult::Failed;
    }
    if ((uint64_t)st.st_size > limit) {
        close(fd);
        return ReadResult::TooLarge;
    }
    out.assign((size_t)st.st_size, '\0');
    size_t done = 0;
    while (done < out.size()) {
        ssize_t n = read(fd, out.data() + done, out.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            return ReadResult::Failed;
        }
        done += (size_t)n;
    }
    close(fd);
    return ReadResult::Ok;
}

bool SyncDirectory(std::string const& path)
{
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

bool WriteFileAtomically(std::string const& path, std::span<uint8_t const> bytes, std::string& why)
{
    std::string temp = path + ".tmp-" + std::to_string(getpid());
    int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        why = "cannot create " + temp + ": " + Errno();
        return false;
    }
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            why = "cannot write " + temp + ": " + Errno();
            close(fd);
            unlink(temp.c_str());
            return false;
        }
        done += (size_t)n;
    }
    // F_FULLFSYNC: on macOS fsync alone leaves the data in the drive's cache.
    if (fcntl(fd, F_FULLFSYNC) != 0 && fsync(fd) != 0) {
        why = "cannot sync " + temp + ": " + Errno();
        close(fd);
        unlink(temp.c_str());
        return false;
    }
    if (close(fd) != 0) {
        why = "cannot write " + temp + ": " + Errno();
        unlink(temp.c_str());
        return false;
    }
    if (rename(temp.c_str(), path.c_str()) != 0) {
        why = "cannot rename " + temp + ": " + Errno();
        unlink(temp.c_str());
        return false;
    }
    size_t slash = path.rfind('/');
    if (slash != std::string::npos) SyncDirectory(path.substr(0, slash));
    return true;
}

bool MakeDirectories(std::string const& path, std::string& why)
{
    if (path.empty()) {
        why = "no directory";
        return false;
    }
    for (size_t at = 1; at <= path.size(); at++) {
        if (at < path.size() && path[at] != '/') continue;
        std::string part = path.substr(0, at);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
            why = "cannot create " + part + ": " + Errno();
            return false;
        }
    }
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        why = path + " is not a directory";
        return false;
    }
    return true;
}

bool RemoveTree(std::string const& path)
{
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(path.c_str()) == 0;
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;
    bool ok = true;
    while (dirent* e = readdir(dir)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        ok = RemoveTree(path + "/" + e->d_name) && ok;
    }
    closedir(dir);
    return rmdir(path.c_str()) == 0 && ok;
}

}
