#include "platform.h"

#include <lzx/lzx.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lzx::detail {

std::string displayPath(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

#ifdef _WIN32

// Missing from older SDK and MinGW headers.
#ifndef FILE_RENAME_FLAG_REPLACE_IF_EXISTS
#define FILE_RENAME_FLAG_REPLACE_IF_EXISTS 0x00000001
#endif
#ifndef FILE_RENAME_FLAG_POSIX_SEMANTICS
#define FILE_RENAME_FLAG_POSIX_SEMANTICS 0x00000002
#endif

InputFile::InputFile(const std::filesystem::path& path)
{
    // FILE_SHARE_DELETE lets ZipWriter rename a new archive over this one.
    m_handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) {
        throw Error("cannot open " + displayPath(path));
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(m_handle, &size)) {
        CloseHandle(m_handle);
        throw Error("cannot open " + displayPath(path));
    }
    m_size = static_cast<std::uint64_t>(size.QuadPart);
}

InputFile::~InputFile()
{
    CloseHandle(m_handle);
}

bool InputFile::read(std::uint64_t offset, std::span<std::uint8_t> out) const
{
    while (!out.empty()) {
        // ReadFile takes a 32-bit length.
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(out.size(), 1u << 30));
        // An explicit offset keeps concurrent reads from racing on a shared
        // file pointer.
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(m_handle, out.data(), chunk, &got, &position) || got == 0) {
            return false;
        }
        offset += got;
        out = out.subspan(got);
    }
    return true;
}

void replaceFile(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error)
{
    error.clear();
    // A plain rename fails while the target is open, even when every handle
    // allows deletion. POSIX semantics (Windows 10 1809 and later, on NTFS)
    // unlink the old file the way rename() does on Linux, and readers keep
    // their view of it.
    const HANDLE source = CreateFileW(from.c_str(), DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source != INVALID_HANDLE_VALUE) {
        std::error_code absoluteError;
        const std::wstring target = std::filesystem::absolute(to, absoluteError).native();
        if (!absoluteError && target.size() <= std::numeric_limits<DWORD>::max() / sizeof(wchar_t)) {
            const std::size_t nameBytes = target.size() * sizeof(wchar_t);
            const std::size_t infoSize = offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t);
            const auto buffer = std::make_unique<std::byte[]>(infoSize);
            auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.get());
            info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
            info->RootDirectory = nullptr;
            info->FileNameLength = static_cast<DWORD>(nameBytes);
            std::memcpy(info->FileName, target.c_str(), nameBytes + sizeof(wchar_t));
            const bool renamed =
                SetFileInformationByHandle(source, FileRenameInfoEx, info, static_cast<DWORD>(infoSize)) != 0;
            CloseHandle(source);
            if (renamed) {
                return;
            }
        } else {
            CloseHandle(source);
        }
    }
    // Older Windows versions and file systems such as FAT refuse POSIX
    // semantics.
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return;
    }
    // The target is open. Classic rules still let a file be renamed while
    // every handle to it allows deletion, as InputFile's do: move the old
    // file aside, move the new one in, and delete the old one, which goes
    // away once its last reader closes it.
    std::filesystem::path aside = from;
    aside += L".old";
    if (!MoveFileExW(to.c_str(), aside.c_str(), 0)) {
        error.assign(static_cast<int>(GetLastError()), std::system_category());
        return;
    }
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) {
        error.assign(static_cast<int>(GetLastError()), std::system_category());
        MoveFileExW(aside.c_str(), to.c_str(), 0);
        return;
    }
    DeleteFileW(aside.c_str());
}

#else

InputFile::InputFile(const std::filesystem::path& path)
{
    m_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (m_fd < 0) {
        throw Error("cannot open " + displayPath(path));
    }
    struct stat status {};
    if (fstat(m_fd, &status) != 0 || S_ISDIR(status.st_mode)) {
        ::close(m_fd);
        throw Error("cannot open " + displayPath(path));
    }
    m_size = static_cast<std::uint64_t>(status.st_size);
}

InputFile::~InputFile()
{
    ::close(m_fd);
}

bool InputFile::read(std::uint64_t offset, std::span<std::uint8_t> out) const
{
    while (!out.empty()) {
        const ssize_t got = ::pread(m_fd, out.data(), out.size(), static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            return false;
        }
        offset += static_cast<std::uint64_t>(got);
        out = out.subspan(static_cast<std::size_t>(got));
    }
    return true;
}

void replaceFile(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error)
{
    std::filesystem::rename(from, to, error);
}

#endif

} // namespace lzx::detail
