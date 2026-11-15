// Files and directories, with Win32 return values and error codes.
#include <windows.h>
#include <Shlwapi.h>

#include <copyfile.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

DWORD ErrorFromErrno(int error)
{
    switch (error) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES:
    case EPERM: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EROFS: return ERROR_WRITE_PROTECT;
    case ENOSPC: return ERROR_HANDLE_DISK_FULL;
    case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case EMFILE:
    case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EBUSY: return ERROR_SHARING_VIOLATION;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    default: return ERROR_INVALID_FUNCTION;
    }
}

BOOL Fail(int error)
{
    SetLastError(ErrorFromErrno(error));
    return FALSE;
}

// The UTF-8 file system path; '\' becomes '/'. False (with ERROR_INVALID_NAME)
// for a string that is not valid UTF-32.
bool ToPath(LPCWSTR path, std::string& out)
{
    if (path == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    int length = (int)wcslen(path);
    out.clear();
    if (length == 0) {
        return true;
    }
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, length, nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        SetLastError(ERROR_INVALID_NAME);
        return false;
    }
    out.resize(size);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, length, out.data(), size, nullptr, nullptr);
    for (auto& c : out) {
        if (c == '\\') c = '/';
    }
    return true;
}

bool StatPath(LPCWSTR path, struct stat& st)
{
    std::string p;
    if (!ToPath(path, p)) return false;
    if (p.empty()) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return false;
    }
    if (stat(p.c_str(), &st) != 0) {
        Fail(errno);
        return false;
    }
    return true;
}

}

BOOL CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES)
{
    std::string p;
    if (!ToPath(path, p)) return FALSE;
    if (p.empty()) return Fail(ENOTDIR);
    if (mkdir(p.c_str(), 0777) != 0) {
        // A missing parent is ERROR_PATH_NOT_FOUND here, as on Windows.
        return Fail(errno == ENOENT ? ENOTDIR : errno);
    }
    return TRUE;
}

BOOL RemoveDirectoryW(LPCWSTR path)
{
    std::string p;
    if (!ToPath(path, p)) return FALSE;
    if (rmdir(p.c_str()) != 0) {
        return Fail(errno);
    }
    return TRUE;
}

BOOL DeleteFileW(LPCWSTR path)
{
    std::string p;
    if (!ToPath(path, p)) return FALSE;
    struct stat st;
    if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return Fail(EACCES);
    }
    if (unlink(p.c_str()) != 0) {
        return Fail(errno);
    }
    return TRUE;
}

BOOL CopyFileW(LPCWSTR from, LPCWSTR to, BOOL failIfExists)
{
    std::string src, dst;
    if (!ToPath(from, src) || !ToPath(to, dst)) return FALSE;
    struct stat st;
    if (stat(src.c_str(), &st) != 0) return Fail(errno);
    if (S_ISDIR(st.st_mode)) return Fail(EACCES);
    if (failIfExists && access(dst.c_str(), F_OK) == 0) {
        SetLastError(ERROR_FILE_EXISTS);
        return FALSE;
    }
    if (copyfile(src.c_str(), dst.c_str(), nullptr, COPYFILE_ALL) != 0) {
        return Fail(errno);
    }
    return TRUE;
}

DWORD GetFileAttributesW(LPCWSTR path)
{
    struct stat st;
    if (!StatPath(path, st)) return INVALID_FILE_ATTRIBUTES;
    DWORD attributes = 0;
    if (S_ISDIR(st.st_mode)) attributes |= FILE_ATTRIBUTE_DIRECTORY;
    if (!(st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH))) attributes |= FILE_ATTRIBUTE_READONLY;
    return attributes ? attributes : FILE_ATTRIBUTE_NORMAL;
}

DWORD GetTempPathW(DWORD size, LPWSTR buffer)
{
    // GetTempPath2W's order: TMP, TEMP, USERPROFILE do not exist here; TMPDIR and /tmp/ do.
    char const* dir = getenv("TMPDIR");
    std::string path = (dir && *dir) ? dir : "/tmp/";
    if (path.back() != '/') path += '/';

    int length = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
    if (length == 0) return 0;
    if ((DWORD)length + 1 > size) {
        return (DWORD)length + 1;
    }
    MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), buffer, length);
    buffer[length] = 0;
    return (DWORD)length;
}

BOOL PathFileExistsW(LPCWSTR path)
{
    struct stat st;
    return StatPath(path, st) ? TRUE : FALSE;
}

BOOL PathIsDirectoryW(LPCWSTR path)
{
    struct stat st;
    if (!StatPath(path, st)) return FALSE;
    // Windows returns FILE_ATTRIBUTE_DIRECTORY (non-zero) for a directory.
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FALSE;
}
