#include <lzx/zip.h>

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>

namespace lzx {

namespace {

constexpr std::uint32_t kEndOfCentralDirSignature = 0x06054b50;
constexpr std::uint32_t kCentralDirSignature = 0x02014b50;
constexpr std::uint32_t kLocalHeaderSignature = 0x04034b50;
constexpr std::size_t kEndOfCentralDirSize = 22;
constexpr std::size_t kCentralDirRecordSize = 46;
constexpr std::size_t kLocalHeaderSize = 30;
constexpr std::size_t kMaxCommentSize = 0xFFFF;

std::uint16_t le16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
        | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::vector<std::uint8_t> inflateRaw(const std::vector<std::uint8_t>& input, std::size_t expectedSize)
{
    std::vector<std::uint8_t> output(expectedSize);
    z_stream stream{};
    // Negative window bits: a raw deflate stream, as zip stores it.
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        throw Error("zlib failed to initialise");
    }
    // zlib's sizes are 32 bits wide; zip entry sizes are too.
    stream.next_in = const_cast<Bytef*>(input.data());
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = output.data();
    stream.avail_out = static_cast<uInt>(output.size());
    const int result = inflate(&stream, Z_FINISH);
    const std::size_t produced = stream.total_out;
    inflateEnd(&stream);
    if (result != Z_STREAM_END || produced != expectedSize) {
        throw Error("deflate data is corrupt");
    }
    return output;
}

} // namespace

struct ZipArchive::Stream {
    std::mutex mutex;
    std::ifstream file;
};

ZipArchive::ZipArchive(ZipArchive&&) noexcept = default;
ZipArchive& ZipArchive::operator=(ZipArchive&&) noexcept = default;
ZipArchive::~ZipArchive() = default;

std::string ZipEntry::path() const
{
    std::string result = name;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

bool ZipEntry::isDirectory() const
{
    return !name.empty() && (name.back() == '/' || name.back() == '\\');
}

std::string ZipArchive::normalizeName(std::string_view name)
{
    std::string result;
    result.reserve(name.size());
    for (const char c : name) {
        result += c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const std::size_t first = result.find_first_not_of('/');
    return first == std::string::npos ? std::string() : result.substr(first);
}

ZipArchive ZipArchive::open(const std::filesystem::path& path)
{
    ZipArchive archive;
    archive.m_path = path;
    archive.m_stream = std::make_unique<Stream>();
    archive.m_stream->file.open(path, std::ios::binary);
    if (!archive.m_stream->file) {
        throw Error("cannot open " + path.string());
    }
    archive.m_stream->file.seekg(0, std::ios::end);
    archive.m_size = static_cast<std::uint64_t>(archive.m_stream->file.tellg());
    if (archive.m_size < kEndOfCentralDirSize) {
        throw Error(path.string() + " is too small to be a zip archive");
    }

    // The end record sits at the very end unless an archive comment
    // follows it, so search the last 64 KiB backwards.
    const std::size_t tailSize =
        static_cast<std::size_t>(std::min<std::uint64_t>(archive.m_size, kEndOfCentralDirSize + kMaxCommentSize));
    const std::vector<std::uint8_t> tail = archive.readRaw(archive.m_size - tailSize, tailSize);
    std::size_t endRecord = std::string::npos;
    for (std::size_t pos = tailSize - kEndOfCentralDirSize + 1; pos-- > 0;) {
        if (le32(tail.data() + pos) == kEndOfCentralDirSignature) {
            endRecord = pos;
            break;
        }
    }
    if (endRecord == std::string::npos) {
        throw Error(path.string() + " has no end-of-central-directory record");
    }
    const std::uint8_t* end = tail.data() + endRecord;
    const std::uint32_t directorySize = le32(end + 12);
    const std::uint32_t directoryOffset = le32(end + 16);
    const std::uint16_t commentSize = le16(end + 20);
    if (directorySize == std::numeric_limits<std::uint32_t>::max()
        || directoryOffset == std::numeric_limits<std::uint32_t>::max()) {
        throw Error(path.string() + " is a zip64 archive, which is not supported");
    }
    if (static_cast<std::uint64_t>(directoryOffset) + directorySize > archive.m_size) {
        throw Error(path.string() + " has a central directory outside the file");
    }
    const std::size_t commentAvailable = tailSize - endRecord - kEndOfCentralDirSize;
    archive.m_comment.assign(reinterpret_cast<const char*>(end + kEndOfCentralDirSize),
        std::min<std::size_t>(commentSize, commentAvailable));

    // The 16-bit entry count in the end record wraps for archives such as
    // tracks/colorado/bin.zip (230k entries), so walk the directory by size.
    const std::vector<std::uint8_t> directory = archive.readRaw(directoryOffset, directorySize);
    std::size_t pos = 0;
    while (pos + kCentralDirRecordSize <= directory.size()) {
        const std::uint8_t* record = directory.data() + pos;
        if (le32(record) != kCentralDirSignature) {
            throw Error(path.string() + ": bad central directory record at offset "
                + std::to_string(directoryOffset + pos));
        }
        const std::uint16_t nameSize = le16(record + 28);
        const std::uint16_t extraSize = le16(record + 30);
        const std::uint16_t entryCommentSize = le16(record + 32);
        const std::size_t recordSize = kCentralDirRecordSize + nameSize + extraSize + entryCommentSize;
        if (pos + recordSize > directory.size()) {
            throw Error(path.string() + ": truncated central directory");
        }

        ZipEntry entry;
        entry.hostSystem = record[5];
        entry.flags = le16(record + 8);
        entry.method = le16(record + 10);
        entry.dosTime = le16(record + 12);
        entry.dosDate = le16(record + 14);
        entry.crc32 = le32(record + 16);
        entry.compressedSize = le32(record + 20);
        entry.uncompressedSize = le32(record + 24);
        entry.externalAttributes = le32(record + 38);
        entry.localHeaderOffset = le32(record + 42);
        entry.name.assign(reinterpret_cast<const char*>(record + kCentralDirRecordSize), nameSize);

        const std::uint8_t* extra = record + kCentralDirRecordSize + nameSize;
        const std::uint8_t* extraEnd = extra + extraSize;
        while (extra + 4 <= extraEnd) {
            const std::uint16_t id = le16(extra);
            const std::uint16_t size = le16(extra + 2);
            if (extra + 4 + size > extraEnd) {
                break;
            }
            if (id == kOffsetExtraField && size >= 4) {
                entry.offsetField = le32(extra + 4);
            }
            extra += 4 + size;
        }

        archive.m_index.emplace(normalizeName(entry.name), archive.m_entries.size());
        archive.m_entries.push_back(std::move(entry));
        pos += recordSize;
    }
    return archive;
}

const ZipEntry* ZipArchive::find(std::string_view name) const
{
    const auto it = m_index.find(normalizeName(name));
    return it == m_index.end() ? nullptr : &m_entries[it->second];
}

bool ZipArchive::needsLzxReader() const
{
    const bool usesLzx = std::any_of(m_entries.begin(), m_entries.end(), [](const ZipEntry& entry) {
        return entry.method == kMethodXMemCompress;
    });
    if (usesLzx) {
        return true;
    }
    return std::any_of(m_entries.begin(), m_entries.end(), [this](const ZipEntry& entry) {
        return entry.offsetField.has_value() && !hasLocalHeader(entry);
    });
}

bool ZipArchive::hasLocalHeader(const ZipEntry& entry) const
{
    if (std::uint64_t{entry.localHeaderOffset} + kLocalHeaderSize > m_size) {
        return false;
    }
    return le32(readRaw(entry.localHeaderOffset, 4).data()) == kLocalHeaderSignature;
}

std::vector<std::uint8_t> ZipArchive::readRaw(std::uint64_t offset, std::size_t size) const
{
    if (offset > m_size || size > m_size - offset) {
        throw Error(m_path.string() + ": read past the end of the file");
    }
    std::vector<std::uint8_t> data(size);
    const std::lock_guard lock(m_stream->mutex);
    m_stream->file.clear();
    m_stream->file.seekg(static_cast<std::streamoff>(offset));
    m_stream->file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!m_stream->file) {
        throw Error("cannot read " + m_path.string());
    }
    return data;
}

std::uint64_t ZipArchive::dataOffset(const ZipEntry& entry) const
{
    if (entry.offsetField) {
        return *entry.offsetField;
    }
    const std::vector<std::uint8_t> header = readRaw(entry.localHeaderOffset, kLocalHeaderSize);
    if (le32(header.data()) != kLocalHeaderSignature) {
        throw Error(entry.name + " has neither a data offset field nor a local header");
    }
    return std::uint64_t{entry.localHeaderOffset} + kLocalHeaderSize + le16(header.data() + 26)
        + le16(header.data() + 28);
}

std::vector<std::uint8_t> ZipArchive::rawData(const ZipEntry& entry) const
{
    return readRaw(dataOffset(entry), entry.compressedSize);
}

std::vector<std::uint8_t> ZipArchive::read(const ZipEntry& entry) const
{
    if (entry.isEncrypted()) {
        throw Error(entry.name + " is encrypted, which is not supported");
    }
    const std::vector<std::uint8_t> raw = readRaw(dataOffset(entry), entry.compressedSize);

    std::vector<std::uint8_t> data;
    switch (entry.method) {
    case kMethodStored:
        if (entry.compressedSize != entry.uncompressedSize) {
            throw Error(entry.name + ": stored entry has mismatched sizes");
        }
        data = raw;
        break;
    case kMethodDeflate:
        try {
            data = inflateRaw(raw, entry.uncompressedSize);
        } catch (const Error& e) {
            throw Error(entry.name + ": " + e.what());
        }
        break;
    case kMethodXMemCompress:
        try {
            data = decompress(raw, entry.uncompressedSize);
        } catch (const Error& e) {
            throw Error(entry.name + ": LZX data is corrupt: " + e.what());
        }
        break;
    default:
        throw Error(entry.name + ": unsupported compression method " + std::to_string(entry.method));
    }

    const auto crc = static_cast<std::uint32_t>(crc32(0L, data.data(), static_cast<uInt>(data.size())));
    if (crc != entry.crc32) {
        throw Error(entry.name + ": CRC mismatch");
    }
    return data;
}

} // namespace lzx
