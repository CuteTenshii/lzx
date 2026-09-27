#pragma once

#include "lzx_test_encoder.h"

#include <zlib.h>

#include <cstdint>
#include <string>
#include <vector>

/// Writes zip archives in memory, in the layouts found on Xbox 360 discs.
namespace zipbuilder {

struct Entry {
    std::string name;
    std::vector<std::uint8_t> content;
    std::uint16_t method = 0;
    /// No local header; the data offset goes in extra field 0x1123.
    bool headerless = true;
    bool corruptCrc = false;
    /// Write the 0x1123 offset field even though a local header exists.
    bool offsetField = false;
};

inline void le16(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

inline void le32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    le16(out, value & 0xFFFF);
    le16(out, value >> 16);
}

inline std::vector<std::uint8_t> bytes(const std::string& text)
{
    return {text.begin(), text.end()};
}

inline std::vector<std::uint8_t> deflateRaw(const std::vector<std::uint8_t>& data)
{
    z_stream stream{};
    deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    std::vector<std::uint8_t> out(deflateBound(&stream, static_cast<uLong>(data.size())));
    stream.next_in = const_cast<Bytef*>(data.data());
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = out.data();
    stream.avail_out = static_cast<uInt>(out.size());
    deflate(&stream, Z_FINISH);
    out.resize(stream.total_out);
    deflateEnd(&stream);
    return out;
}

inline std::vector<std::uint8_t> build(const std::vector<Entry>& entries, const std::string& comment = {})
{
    std::vector<std::uint8_t> file;
    std::vector<std::uint8_t> directory;
    for (const Entry& entry : entries) {
        std::vector<std::uint8_t> stored = entry.content;
        if (entry.method == 21) {
            stored = lzxtest::encode(entry.content, {{lzxtest::BlockKind::Verbatim, entry.content.size()}});
        } else if (entry.method == 8) {
            stored = deflateRaw(entry.content);
        }
        auto crc = static_cast<std::uint32_t>(
            crc32(0L, entry.content.data(), static_cast<uInt>(entry.content.size())));
        if (entry.corruptCrc) {
            crc ^= 1u;
        }
        const auto headerOffset = static_cast<std::uint32_t>(file.size());
        if (!entry.headerless) {
            le32(file, 0x04034b50);
            le16(file, 20);
            le16(file, 0);
            le16(file, entry.method);
            le32(file, 0);
            le32(file, crc);
            le32(file, static_cast<std::uint32_t>(stored.size()));
            le32(file, static_cast<std::uint32_t>(entry.content.size()));
            le16(file, static_cast<std::uint32_t>(entry.name.size()));
            le16(file, 0);
            file.insert(file.end(), entry.name.begin(), entry.name.end());
        }
        const auto dataOffset = static_cast<std::uint32_t>(file.size());
        file.insert(file.end(), stored.begin(), stored.end());

        std::vector<std::uint8_t> extra;
        if (entry.headerless || entry.offsetField) {
            le16(extra, 0x1123);
            le16(extra, 4);
            le32(extra, dataOffset);
        }
        le32(directory, 0x02014b50);
        le16(directory, 20);
        le16(directory, 20);
        le16(directory, 0);
        le16(directory, entry.method);
        // 2011-08-26 12:34:56 in DOS time and date.
        le16(directory, (12u << 11) | (34u << 5) | (56u / 2));
        le16(directory, ((2011u - 1980u) << 9) | (8u << 5) | 26u);
        le32(directory, crc);
        le32(directory, static_cast<std::uint32_t>(stored.size()));
        le32(directory, static_cast<std::uint32_t>(entry.content.size()));
        le16(directory, static_cast<std::uint32_t>(entry.name.size()));
        le16(directory, static_cast<std::uint32_t>(extra.size()));
        le16(directory, 0);
        le16(directory, 0);
        le16(directory, 0);
        le32(directory, 0);
        le32(directory, headerOffset);
        directory.insert(directory.end(), entry.name.begin(), entry.name.end());
        directory.insert(directory.end(), extra.begin(), extra.end());
    }
    const auto directoryOffset = static_cast<std::uint32_t>(file.size());
    file.insert(file.end(), directory.begin(), directory.end());
    le32(file, 0x06054b50);
    le16(file, 0);
    le16(file, 0);
    // The counts are 16 bits and wrap for large archives, as in the game.
    le16(file, static_cast<std::uint32_t>(entries.size() & 0xFFFF));
    le16(file, static_cast<std::uint32_t>(entries.size() & 0xFFFF));
    le32(file, static_cast<std::uint32_t>(directory.size()));
    le32(file, directoryOffset);
    le16(file, static_cast<std::uint32_t>(comment.size()));
    file.insert(file.end(), comment.begin(), comment.end());
    return file;
}

} // namespace zipbuilder
