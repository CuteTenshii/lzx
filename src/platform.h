#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace lzx::detail {

/// The path in UTF-8, for messages. path::string() converts to the ANSI code
/// page on Windows and throws for characters outside it.
std::string displayPath(const std::filesystem::path& path);

/// A file read at explicit offsets, safe to share between threads.
///
/// On Windows the file stays deletable and replaceable while open, as it is
/// on POSIX systems, so ZipWriter can replace an archive that is being read.
class InputFile {
public:
    /// Throws Error if the file cannot be opened.
    explicit InputFile(const std::filesystem::path& path);
    InputFile(const InputFile&) = delete;
    InputFile& operator=(const InputFile&) = delete;
    ~InputFile();

    std::uint64_t size() const { return m_size; }

    /// Fills `out` from `offset`; returns false if the file cannot be read
    /// or ends first.
    bool read(std::uint64_t offset, std::span<std::uint8_t> out) const;

private:
#ifdef _WIN32
    void* m_handle;
#else
    int m_fd;
#endif
    std::uint64_t m_size = 0;
};

/// Moves `from` over `to`, replacing `to` even while an InputFile has it
/// open.
void replaceFile(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& error);

} // namespace lzx::detail
