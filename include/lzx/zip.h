#pragma once

#include <lzx/lzx.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lzx {

inline constexpr std::uint16_t kMethodStored = 0;
inline constexpr std::uint16_t kMethodDeflate = 8;
/// XMemCompress LZX. Not a registered zip method; Xbox 360 titles such as
/// Forza Horizon use 21 for it.
inline constexpr std::uint16_t kMethodXMemCompress = 21;

/// Extra field holding an entry's absolute data offset, which lets an archive
/// leave out local file headers. Written by the packing tool behind Forza
/// Horizon's archives; not a registered zip extra field.
inline constexpr std::uint16_t kOffsetExtraField = 0x1123;

/// One file or directory in a zip archive, as its central directory
/// describes it.
struct ZipEntry {
    /// The name as stored, in either slash direction.
    std::string name;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    std::uint32_t crc32 = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t uncompressedSize = 0;
    std::uint16_t dosTime = 0;
    std::uint16_t dosDate = 0;
    /// The high byte of "version made by": 3 is Unix.
    std::uint8_t hostSystem = 0;
    std::uint32_t externalAttributes = 0;
    std::uint32_t localHeaderOffset = 0;
    /// Absolute offset of the entry's data, from extra field 0x1123.
    std::optional<std::uint64_t> offsetField;

    /// The name with backslashes turned into forward slashes.
    std::string path() const;
    bool isDirectory() const;
    bool isEncrypted() const { return (flags & 1) != 0; }
};

/// Read-only access to zip archives, including Xbox 360 ones: entries may be
/// stored, deflated or XMemCompress-compressed, may lack local headers, and
/// the archive may hold more than 65,535 entries without using zip64.
///
/// Reading entries is thread-safe.
class ZipArchive {
public:
    /// Opens the archive and reads its central directory. Throws Error if
    /// the file cannot be read or is not a zip archive.
    static ZipArchive open(const std::filesystem::path& path);

    ZipArchive(ZipArchive&&) noexcept;
    ZipArchive& operator=(ZipArchive&&) noexcept;
    ~ZipArchive();

    const std::filesystem::path& path() const { return m_path; }
    const std::vector<ZipEntry>& entries() const { return m_entries; }
    const std::string& comment() const { return m_comment; }

    /// Finds an entry the way the game does: ignoring case and slash
    /// direction, and any leading slashes.
    const ZipEntry* find(std::string_view name) const;

    /// True if other zip readers cannot extract this archive: an entry uses
    /// XMemCompress, or has no local header and can only be found through
    /// the offset field.
    bool needsLzxReader() const;

    /// Returns the entry's contents, checked against its CRC-32. Throws
    /// Error for unsupported methods, encrypted entries and corrupt data.
    std::vector<std::uint8_t> read(const ZipEntry& entry) const;

    /// Returns the entry's data as stored, still compressed.
    std::vector<std::uint8_t> rawData(const ZipEntry& entry) const;

    /// Game-style lookup key for a name: lower case, forward slashes, no
    /// leading slash.
    static std::string normalizeName(std::string_view name);

private:
    ZipArchive() = default;

    std::vector<std::uint8_t> readRaw(std::uint64_t offset, std::size_t size) const;
    std::uint64_t dataOffset(const ZipEntry& entry) const;
    bool hasLocalHeader(const ZipEntry& entry) const;

    std::filesystem::path m_path;
    std::uint64_t m_size = 0;
    std::vector<ZipEntry> m_entries;
    std::unordered_map<std::string, std::size_t> m_index;
    std::string m_comment;

    struct Stream;
    std::unique_ptr<Stream> m_stream;
};

} // namespace lzx
