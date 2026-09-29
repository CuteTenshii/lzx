#include <lzx/zip_writer.h>

#include "platform.h"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <limits>
#include <random>
#include <system_error>

namespace lzx {

namespace {

constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50;
constexpr std::uint32_t kCentralDirSignature = 0x02014b50;
constexpr std::uint32_t kEndOfCentralDirSignature = 0x06054b50;
/// "Version made by" 1.0 on MS-DOS, and "version needed" 1.0: what the game
/// archives' packing tool writes, whatever the method.
constexpr std::uint16_t kVersion = 10;
constexpr std::uint16_t kOffsetFieldSize = 4;
constexpr std::size_t kMaxCommentSize = 0xFFFF;

void le16(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void le32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    le16(out, value & 0xFFFF);
    le16(out, value >> 16);
}

DosDateTime fromTimeT(std::time_t seconds)
{
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    // DOS dates start in 1980.
    const int year = std::clamp(local.tm_year + 1900, 1980, 2107);
    DosDateTime result;
    result.date = static_cast<std::uint16_t>(((year - 1980) << 9) | ((local.tm_mon + 1) << 5) | local.tm_mday);
    result.time = static_cast<std::uint16_t>((local.tm_hour << 11) | (local.tm_min << 5) | (local.tm_sec / 2));
    return result;
}

std::uint32_t checked32(std::uint64_t value, const char* what)
{
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw Error(std::string(what) + " does not fit a zip archive without zip64");
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

DosDateTime DosDateTime::fromFileTime(std::filesystem::file_time_type fileTime)
{
    const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(fileTime);
    return fromTimeT(std::chrono::system_clock::to_time_t(systemTime));
}

DosDateTime DosDateTime::now()
{
    return fromTimeT(std::time(nullptr));
}

ZipWriter::ZipWriter(std::filesystem::path path)
    : m_path(std::move(path))
{
    std::random_device random;
    m_tempPath = m_path;
    m_tempPath += ".lzx-" + std::to_string(random()) + ".tmp";
    m_out.open(m_tempPath, std::ios::binary | std::ios::trunc);
    if (!m_out) {
        throw Error("cannot create " + detail::displayPath(m_tempPath));
    }
}

ZipWriter::~ZipWriter()
{
    if (!m_finished) {
        m_out.close();
        std::error_code ignored;
        std::filesystem::remove(m_tempPath, ignored);
    }
}

void ZipWriter::writeBytes(std::span<const std::uint8_t> bytes)
{
    m_out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!m_out) {
        throw Error("cannot write " + detail::displayPath(m_tempPath));
    }
    m_offset += bytes.size();
}

void ZipWriter::write(std::string_view name, std::uint16_t method, DosDateTime time, std::uint32_t crc,
    std::uint32_t uncompressedSize, std::span<const std::uint8_t> stored)
{
    if (name.empty() || name.size() > 0xFFFF) {
        throw Error("invalid entry name '" + std::string(name) + "'");
    }
    const std::string key = ZipArchive::normalizeName(name);
    if (m_names.contains(key)) {
        throw Error(std::string(name) + " is already in the archive");
    }

    Written entry{std::string(name), method, time, crc, checked32(stored.size(), "entry size"), uncompressedSize,
        checked32(m_offset, "archive size"), 0};
    std::vector<std::uint8_t> header;
    le32(header, kLocalHeaderSignature);
    le16(header, kVersion);
    le16(header, 0);
    le16(header, method);
    le16(header, time.time);
    le16(header, time.date);
    le32(header, crc);
    le32(header, entry.compressedSize);
    le32(header, uncompressedSize);
    le16(header, static_cast<std::uint32_t>(name.size()));
    le16(header, 0);
    header.insert(header.end(), name.begin(), name.end());
    writeBytes(header);
    entry.dataOffset = checked32(m_offset, "archive size");
    writeBytes(stored);

    m_names.emplace(key, m_entries.size());
    m_entries.push_back(std::move(entry));
}

void ZipWriter::add(std::string_view name, std::span<const std::uint8_t> data, DosDateTime time)
{
    const auto size = checked32(data.size(), "entry size");
    const auto crc = static_cast<std::uint32_t>(crc32(0L, data.data(), static_cast<uInt>(data.size())));
    const std::vector<std::uint8_t> compressed = compress(data);
    if (compressed.size() < data.size()) {
        write(name, kMethodXMemCompress, time, crc, size, compressed);
    } else {
        write(name, kMethodStored, time, crc, size, data);
    }
}

void ZipWriter::copy(const ZipArchive& source, const ZipEntry& entry, std::string_view name)
{
    const std::vector<std::uint8_t> raw = source.rawData(entry);
    write(name.empty() ? std::string_view(entry.name) : name, entry.method, {entry.dosDate, entry.dosTime},
        entry.crc32, entry.uncompressedSize, raw);
}

void ZipWriter::finish()
{
    if (m_finished) {
        throw Error("archive is already finished");
    }
    if (m_comment.size() > kMaxCommentSize) {
        throw Error("archive comment is longer than 65535 bytes");
    }
    const std::uint32_t directoryOffset = checked32(m_offset, "archive size");
    std::vector<std::uint8_t> directory;
    for (const Written& entry : m_entries) {
        le32(directory, kCentralDirSignature);
        le16(directory, kVersion);
        le16(directory, kVersion);
        le16(directory, 0);
        le16(directory, entry.method);
        le16(directory, entry.time.time);
        le16(directory, entry.time.date);
        le32(directory, entry.crc32);
        le32(directory, entry.compressedSize);
        le32(directory, entry.uncompressedSize);
        le16(directory, static_cast<std::uint32_t>(entry.name.size()));
        le16(directory, 4 + kOffsetFieldSize);
        le16(directory, 0);
        le16(directory, 0);
        le16(directory, 0);
        le32(directory, 0);
        le32(directory, entry.localHeaderOffset);
        directory.insert(directory.end(), entry.name.begin(), entry.name.end());
        le16(directory, kOffsetExtraField);
        le16(directory, kOffsetFieldSize);
        le32(directory, entry.dataOffset);
    }
    writeBytes(directory);

    std::vector<std::uint8_t> end;
    le32(end, kEndOfCentralDirSignature);
    le16(end, 0);
    le16(end, 0);
    // The 16-bit counts wrap past 65535 entries, as in the game's archives;
    // readers walk the directory by its size instead.
    le16(end, static_cast<std::uint32_t>(m_entries.size() & 0xFFFF));
    le16(end, static_cast<std::uint32_t>(m_entries.size() & 0xFFFF));
    le32(end, checked32(directory.size(), "central directory"));
    le32(end, directoryOffset);
    le16(end, static_cast<std::uint32_t>(m_comment.size()));
    end.insert(end.end(), m_comment.begin(), m_comment.end());
    writeBytes(end);

    m_out.close();
    if (!m_out) {
        throw Error("cannot write " + detail::displayPath(m_tempPath));
    }
    std::error_code error;
    detail::replaceFile(m_tempPath, m_path, error);
    if (error) {
        std::filesystem::remove(m_tempPath, error);
        throw Error("cannot replace " + detail::displayPath(m_path) + ": " + error.message());
    }
    m_finished = true;
}

} // namespace lzx
