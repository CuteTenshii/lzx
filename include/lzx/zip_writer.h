#pragma once

#include <lzx/zip.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lzx {

/// A zip timestamp: MS-DOS date and time fields, in local time.
struct DosDateTime {
    std::uint16_t date = 0;
    std::uint16_t time = 0;

    static DosDateTime fromFileTime(std::filesystem::file_time_type fileTime);
    static DosDateTime now();
};

/// Writes zip archives in the layout of Xbox 360 game archives such as Forza
/// Horizon's: entries are XMemCompress-compressed (or stored when that does
/// not make them smaller), every entry has a local header and its data offset
/// in extra field 0x1123, and there are no directory entries or file
/// attributes.
///
/// The archive is written to a temporary file next to `path` and moved over
/// `path` by finish(), so an archive can be rewritten from a ZipArchive open
/// on the same file. Without finish(), nothing at `path` changes.
class ZipWriter {
public:
    explicit ZipWriter(std::filesystem::path path);
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;
    ~ZipWriter();

    /// Compresses and adds a file. Throws Error for names already added.
    void add(std::string_view name, std::span<const std::uint8_t> data, DosDateTime time = DosDateTime::now());

    /// Adds an entry of another archive without recompressing it, under
    /// `name`, or its own name when `name` is empty.
    void copy(const ZipArchive& source, const ZipEntry& entry, std::string_view name = {});

    void setComment(std::string comment) { m_comment = std::move(comment); }

    /// Writes the central directory and replaces `path` with the new archive.
    void finish();

private:
    struct Written {
        std::string name;
        std::uint16_t method;
        DosDateTime time;
        std::uint32_t crc32;
        std::uint32_t compressedSize;
        std::uint32_t uncompressedSize;
        std::uint32_t localHeaderOffset;
        std::uint32_t dataOffset;
    };

    void write(std::string_view name, std::uint16_t method, DosDateTime time, std::uint32_t crc,
        std::uint32_t uncompressedSize, std::span<const std::uint8_t> stored);
    void writeBytes(std::span<const std::uint8_t> bytes);

    std::filesystem::path m_path;
    std::filesystem::path m_tempPath;
    std::ofstream m_out;
    std::uint64_t m_offset = 0;
    std::vector<Written> m_entries;
    std::unordered_map<std::string, std::size_t> m_names;
    std::string m_comment;
    bool m_finished = false;
};

} // namespace lzx
