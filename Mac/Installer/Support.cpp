#include <Installer/Support.h>

#include <Installer/Installer.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace bg3se::mac::installer {

std::string Errno()
{
    return strerror(errno);
}

std::string Quoted(std::string const& s)
{
    return "\"" + s + "\"";
}

bool Exists(std::string const& path)
{
    struct stat st{};
    return lstat(path.c_str(), &st) == 0;
}

bool IsDirectory(std::string const& path)
{
    struct stat st{};
    return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string DirectoryOf(std::string const& path)
{
    return path.substr(0, path.rfind('/'));
}

std::string NameOf(std::string const& path)
{
    return path.substr(path.rfind('/') + 1);
}

bool Mapped::Open(std::string const& path, std::string& problem)
{
    Close();
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) return true;
        problem = path + ": " + Errno();
        return false;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        problem = path + ": not a file";
        close(fd);
        return false;
    }
    present_ = true;
    size_ = (size_t)st.st_size;
    if (size_) {
        void* data = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (data == MAP_FAILED) {
            problem = path + ": " + Errno();
            close(fd);
            present_ = false;
            size_ = 0;
            return false;
        }
        data_ = static_cast<uint8_t const*>(data);
    }
    close(fd);
    return true;
}

void Mapped::Close()
{
    if (data_) munmap(const_cast<uint8_t*>(data_), size_);
    data_ = nullptr;
    size_ = 0;
    present_ = false;
}

bool SameBytes(std::span<uint8_t const> a, std::span<uint8_t const> b)
{
    return a.size() == b.size() && (a.empty() || memcmp(a.data(), b.data(), a.size()) == 0);
}

bool SameFile(std::string const& a, std::string const& b)
{
    Mapped x, y;
    std::string ignored;
    return x.Open(a, ignored) && y.Open(b, ignored) && x.Present() && y.Present() && SameBytes(x.Bytes(), y.Bytes());
}

bool FullSync(int fd)
{
    return fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0;
}

void SyncDirectory(std::string const& path)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        FullSync(fd);
        close(fd);
    }
}

bool WriteAtomically(std::string const& path, std::span<uint8_t const> bytes, mode_t mode, std::string& problem)
{
    std::string temporary = path + kTemporarySuffix;
    unlink(temporary.c_str());
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0) {
        problem = temporary + ": " + Errno();
        return false;
    }
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    bool ok = done == bytes.size() && FullSync(fd) && fchmod(fd, mode) == 0;
    close(fd);
    Mapped written;
    if (!ok || !written.Open(temporary, problem) || !SameBytes(written.Bytes(), bytes)) {
        if (problem.empty()) problem = "cannot write " + temporary + (errno ? ": " + Errno() : "");
        unlink(temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        problem = "cannot move " + temporary + " to " + path + ": " + Errno();
        unlink(temporary.c_str());
        return false;
    }
    SyncDirectory(DirectoryOf(path));
    return true;
}

std::string ResolvedPath(std::string const& path)
{
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved)) return resolved;
    if (realpath(DirectoryOf(path).c_str(), resolved)) return std::string(resolved) + "/" + NameOf(path);
    return path;
}

DataLock::DataLock(std::string const& data)
{
    fd_ = open(data.c_str(), O_RDONLY | O_CLOEXEC);
    held_ = fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) == 0;
}

DataLock::~DataLock()
{
    if (fd_ >= 0) close(fd_);
}

bool RemoveTree(std::string const& path)
{
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(path.c_str()) == 0;
    bool ok = true;
    if (DIR* dir = opendir(path.c_str())) {
        while (dirent* e = readdir(dir)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            ok = RemoveTree(path + "/" + e->d_name) && ok;
        }
        closedir(dir);
    } else {
        return false;
    }
    return rmdir(path.c_str()) == 0 && ok;
}

int RunProgram(std::vector<std::string> const& args, std::string& output, std::string const* input,
    std::vector<std::string> const& extraEnvironment)
{
    int out[2], in[2] = { -1, -1 };
    if (pipe(out) != 0) return -1;
    if (input && pipe(in) != 0) {
        close(out[0]);
        close(out[1]);
        return -1;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    if (input) {
        posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, in[1]);
    }
    std::vector<char*> argv;
    for (auto const& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char** e = environ; *e; e++) {
        std::string entry = *e;
        bool replaced = false;
        for (auto const& extra : extraEnvironment) replaced = replaced || entry.starts_with(extra.substr(0, extra.find('=') + 1));
        if (!replaced) environment.push_back(entry);
    }
    for (auto const& extra : extraEnvironment) environment.push_back(extra);
    std::vector<char*> envp;
    for (auto& e : environment) envp.push_back(e.data());
    envp.push_back(nullptr);
    pid_t pid = 0;
    int spawned = posix_spawn(&pid, args[0].c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    if (input) close(in[0]);
    if (spawned != 0) {
        close(out[0]);
        if (input) close(in[1]);
        output = args[0] + ": " + strerror(spawned);
        return -1;
    }
    if (input) {
        // A program that stops reading must not end this one.
        void (*previous)(int) = signal(SIGPIPE, SIG_IGN);
        size_t done = 0;
        while (done < input->size()) {
            ssize_t n = write(in[1], input->data() + done, input->size() - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            done += (size_t)n;
        }
        close(in[1]);
        signal(SIGPIPE, previous);
    }
    char buffer[4096];
    ssize_t n;
    while ((n = read(out[0], buffer, sizeof(buffer))) > 0 || (n < 0 && errno == EINTR)) {
        if (n > 0) output.append(buffer, (size_t)n);
    }
    close(out[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}
