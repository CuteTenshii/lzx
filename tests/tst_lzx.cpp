#include "check.h"
#include "lzx_test_encoder.h"
#include "zip_builder.h"

#include <lzx/lzx.h>
#include <lzx/zip.h>
#include <lzx/zip_writer.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> randomBytes(std::size_t size, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> byte(0, 255);
    std::vector<std::uint8_t> data(size);
    for (auto& b : data) {
        b = static_cast<std::uint8_t>(byte(rng));
    }
    return data;
}

/// Text-like data with long runs, so verbatim blocks use repeat matches.
std::vector<std::uint8_t> runnyBytes(std::size_t size, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> letter('a', 'h');
    std::uniform_int_distribution<int> runLength(1, 12);
    std::vector<std::uint8_t> data;
    data.reserve(size);
    while (data.size() < size) {
        const auto value = static_cast<std::uint8_t>(letter(rng));
        for (int i = runLength(rng); i > 0 && data.size() < size; --i) {
            data.push_back(value);
        }
    }
    return data;
}

/// A zip archive written to a temporary file, removed afterwards.
class TempArchive {
public:
    TempArchive()
        : m_path(std::filesystem::temp_directory_path()
              / ("lzx-test-" + std::to_string(std::random_device{}()) + ".zip"))
    {
    }
    explicit TempArchive(const std::vector<std::uint8_t>& bytes)
        : TempArchive()
    {
        std::ofstream out(m_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    TempArchive(const TempArchive&) = delete;
    TempArchive& operator=(const TempArchive&) = delete;
    ~TempArchive() { std::filesystem::remove(m_path); }
    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

} // namespace

TEST_CASE("uncompressed block in a single chunk")
{
    const auto data = randomBytes(1000, 1);
    const auto stream = lzxtest::encode(data, {{lzxtest::BlockKind::Uncompressed, data.size()}});
    CHECK_EQ(stream.front(), 0xFF);
    CHECK(lzx::decompress(stream, data.size()) == data);
}

TEST_CASE("uncompressed block spanning chunks")
{
    const auto data = randomBytes(100000, 2);
    const auto stream = lzxtest::encode(data, {{lzxtest::BlockKind::Uncompressed, data.size()}});
    CHECK(lzx::decompress(stream, data.size()) == data);
}

// XMemCompress follows an odd-length uncompressed block directly with the
// next block header, restarting word alignment at that odd byte.
TEST_CASE("odd uncompressed blocks have no pad byte")
{
    const auto data = randomBytes(3 * 32768 + 517, 3);
    const std::vector<lzxtest::Block> blocks{
        {lzxtest::BlockKind::Uncompressed, 40001},
        {lzxtest::BlockKind::Uncompressed, 33333},
        {lzxtest::BlockKind::Uncompressed, data.size() - 40001 - 33333},
    };
    CHECK(lzx::decompress(lzxtest::encode(data, blocks), data.size()) == data);
}

TEST_CASE("verbatim blocks with repeat matches")
{
    const auto data = runnyBytes(5000, 4);
    const auto stream = lzxtest::encode(data, {{lzxtest::BlockKind::Verbatim, data.size()}});
    CHECK(stream.size() < data.size() + 1000);
    CHECK(lzx::decompress(stream, data.size()) == data);
}

TEST_CASE("verbatim block spanning chunks")
{
    const auto data = runnyBytes(90000, 5);
    CHECK(lzx::decompress(lzxtest::encode(data, {{lzxtest::BlockKind::Verbatim, data.size()}}), data.size()) == data);
}

TEST_CASE("mixed blocks across chunks")
{
    const auto text = runnyBytes(50001, 6);
    const auto noise = randomBytes(30001, 7);
    std::vector<std::uint8_t> data = text;
    data.insert(data.end(), noise.begin(), noise.end());
    data.insert(data.end(), text.begin(), text.begin() + 20000);
    const std::vector<lzxtest::Block> blocks{
        {lzxtest::BlockKind::Verbatim, 50001},
        {lzxtest::BlockKind::Uncompressed, 30001},
        {lzxtest::BlockKind::Verbatim, 20000},
    };
    CHECK(lzx::decompress(lzxtest::encode(data, blocks), data.size()) == data);
}

TEST_CASE("partial decode stops after the requested prefix")
{
    const auto data = runnyBytes(100000, 10);
    const auto stream = lzxtest::encode(data, {{lzxtest::BlockKind::Verbatim, data.size()}});
    const auto prefix = lzx::decompress(stream, data.size(), lzx::kDefaultWindowBits, 40000);
    CHECK_EQ(prefix.size(), std::size_t{40000});
    CHECK(std::equal(prefix.begin(), prefix.end(), data.begin()));
}

TEST_CASE("intel E8 translation")
{
    // An E8 at offset 5 carrying the absolute target 105 decodes to the
    // relative 105 - 5 = 100.
    std::vector<std::uint8_t> encoded(64, 0x90);
    encoded[5] = 0xE8;
    encoded[6] = 105;
    encoded[7] = 0;
    encoded[8] = 0;
    encoded[9] = 0;
    const auto stream = lzxtest::encode(encoded, {{lzxtest::BlockKind::Verbatim, encoded.size()}}, 1000000);
    std::vector<std::uint8_t> expected = encoded;
    expected[6] = 100;
    CHECK(lzx::decompress(stream, encoded.size()) == expected);
}

TEST_CASE("malformed streams are rejected")
{
    const auto data = randomBytes(40000, 9);
    const auto stream = lzxtest::encode(data, {{lzxtest::BlockKind::Uncompressed, data.size()}});
    CHECK_THROWS(lzx::decompress(stream, data.size() + 1));

    auto truncated = stream;
    truncated.resize(truncated.size() - 100);
    CHECK_THROWS(lzx::decompress(truncated, data.size()));

    // One chunk of 16 output bytes whose payload declares block type 7.
    const std::vector<std::uint8_t> badType{0xFF, 0x00, 0x10, 0x00, 0x04, 0x00, 0x70, 0x00, 0x00};
    CHECK_THROWS(lzx::decompress(badType, 16));
    CHECK_THROWS(lzx::decompress(stream, data.size(), 22));
}

TEST_CASE("compressed streams round-trip at every frame boundary")
{
    for (const std::size_t size : {std::size_t{1}, std::size_t{2}, std::size_t{300}, std::size_t{32767},
             std::size_t{32768}, std::size_t{32769}, std::size_t{65536}, std::size_t{200001}}) {
        const auto text = runnyBytes(size, static_cast<std::uint32_t>(size));
        CHECK(lzx::decompress(lzx::compress(text), text.size()) == text);
        const auto noise = randomBytes(size, static_cast<std::uint32_t>(size) + 1);
        CHECK(lzx::decompress(lzx::compress(noise), noise.size()) == noise);
    }
    CHECK(lzx::compress({}).empty());
    CHECK(lzx::decompress(lzx::compress({}), 0).empty());
}

TEST_CASE("compression shrinks redundant data and bounds incompressible data")
{
    const auto text = runnyBytes(100000, 20);
    CHECK(lzx::compress(text).size() < text.size() / 3);

    const std::vector<std::uint8_t> zeros(1000000, 0);
    const auto packed = lzx::compress(zeros);
    CHECK(packed.size() < 5000);
    CHECK(lzx::decompress(packed, zeros.size()) == zeros);

    // Incompressible frames are stored: 32 KiB of data, 2 header bytes and
    // at most 20 bytes of block header.
    const auto noise = randomBytes(3 * 32768, 21);
    CHECK(lzx::compress(noise).size() <= noise.size() + 3 * 22 + 3);
}

TEST_CASE("the final chunk always has the long header")
{
    // XMemCompress marks the last chunk with 0xFF and its output size even
    // when it holds a full 32 KiB frame.
    const auto data = runnyBytes(65536, 22);
    const auto stream = lzx::compress(data);
    CHECK(stream.front() != 0xFF);
    const std::size_t firstPayload = (static_cast<std::size_t>(stream[0]) << 8) | stream[1];
    const std::uint8_t* last = stream.data() + 2 + firstPayload;
    CHECK_EQ(last[0], 0xFF);
    CHECK_EQ((last[1] << 8) | last[2], 32768);
}

TEST_CASE("repeat offsets and long matches survive frame changes")
{
    // A 16-byte pattern with sparse changes exercises repeat offsets, and
    // the long zero run forces length-tree symbols and frame-capped matches.
    std::vector<std::uint8_t> data;
    std::mt19937 rng(23);
    for (int i = 0; i < 20000; ++i) {
        for (int j = 0; j < 16; ++j) {
            data.push_back(static_cast<std::uint8_t>(j * 7 + ((rng() % 50) == 0 ? rng() % 256 : 0)));
        }
    }
    data.insert(data.end(), 100000, 0);
    CHECK(lzx::decompress(lzx::compress(data), data.size()) == data);
}

TEST_CASE("zip entries decode in every layout and method")
{
    const auto big = runnyBytes(70000, 11);
    const auto text = zipbuilder::bytes("route data, route data, route data!");
    const TempArchive archiveFile(zipbuilder::build(
        {
            {"Tracks\\Colorado\\Info.TXT", text, 0, true},
            {"big.bin", big, 21, true},
            {"classic.txt", text, 0, false},
            {"deflated.bin", big, 8, false},
            {"folder/", {}, 0, false},
        },
        "archive comment"));
    const lzx::ZipArchive archive = lzx::ZipArchive::open(archiveFile.path());
    CHECK_EQ(archive.entries().size(), std::size_t{5});
    CHECK(archive.comment() == "archive comment");
    CHECK(archive.needsLzxReader());

    const lzx::ZipEntry* info = archive.find("tracks/colorado/info.txt");
    CHECK(info != nullptr);
    CHECK(info->path() == "Tracks/Colorado/Info.TXT");
    CHECK(archive.read(*info) == text);
    CHECK(archive.read(*archive.find("/BIG.bin")) == big);
    CHECK(archive.read(*archive.find("classic.txt")) == text);
    CHECK(archive.read(*archive.find("deflated.bin")) == big);
    CHECK(archive.find("folder/")->isDirectory());
    CHECK(archive.find("missing.txt") == nullptr);
    CHECK_EQ(info->dosDate, ((2011u - 1980u) << 9) | (8u << 5) | 26u);
}

TEST_CASE("only archives other readers cannot open need the lzx reader")
{
    const TempArchive plain(zipbuilder::build({
        {"a.txt", zipbuilder::bytes("a"), 0, false},
        {"b.txt", runnyBytes(3000, 12), 8, false},
    }));
    CHECK(!lzx::ZipArchive::open(plain.path()).needsLzxReader());

    // The offset field alongside intact local headers: libzip copes.
    auto withOffsets = zipbuilder::build({{"a.txt", zipbuilder::bytes("a"), 0, false, false, true}});
    const TempArchive offsetsOnly(withOffsets);
    CHECK(!lzx::ZipArchive::open(offsetsOnly.path()).needsLzxReader());

    const TempArchive headerless(zipbuilder::build({{"a.txt", zipbuilder::bytes("a"), 0, true}}));
    CHECK(lzx::ZipArchive::open(headerless.path()).needsLzxReader());

    const TempArchive lzxEntry(zipbuilder::build({{"a.txt", runnyBytes(3000, 13), 21, false}}));
    CHECK(lzx::ZipArchive::open(lzxEntry.path()).needsLzxReader());
}

TEST_CASE("entry counts above 65535 are read from the directory size")
{
    std::vector<zipbuilder::Entry> entries;
    for (int i = 0; i < 70000; ++i) {
        entries.push_back({"f" + std::to_string(i), zipbuilder::bytes(std::to_string(i)), 0, true});
    }
    const TempArchive archiveFile(zipbuilder::build(entries));
    const lzx::ZipArchive archive = lzx::ZipArchive::open(archiveFile.path());
    CHECK_EQ(archive.entries().size(), std::size_t{70000});
    CHECK(archive.read(*archive.find("f69999")) == zipbuilder::bytes("69999"));
}

TEST_CASE("corrupt and unsupported zips are rejected")
{
    const TempArchive badCrc(zipbuilder::build({{"bad.txt", zipbuilder::bytes("content"), 0, true, true}}));
    const lzx::ZipArchive archive = lzx::ZipArchive::open(badCrc.path());
    CHECK_THROWS(archive.read(archive.entries().front()));

    const TempArchive notZip(std::vector<std::uint8_t>(100, 'z'));
    CHECK_THROWS(lzx::ZipArchive::open(notZip.path()));
    CHECK_THROWS(lzx::ZipArchive::open("/nonexistent/archive.zip"));

    const TempArchive bzip2(zipbuilder::build({{"x.bin", zipbuilder::bytes("xx"), 12, false}}));
    const lzx::ZipArchive unsupported = lzx::ZipArchive::open(bzip2.path());
    CHECK_THROWS(unsupported.read(unsupported.entries().front()));
}

std::vector<std::uint8_t> fileBytes(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::uint32_t le32At(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

TEST_CASE("written archives read back with the game's layout")
{
    const TempArchive target;
    const auto text = runnyBytes(80000, 30);
    const auto noise = randomBytes(5000, 31);
    const lzx::DosDateTime time{((2012u - 1980u) << 9) | (10u << 5) | 23u, (9u << 11) | (30u << 5)};
    {
        lzx::ZipWriter writer(target.path());
        writer.add("media/text.bin", text, time);
        writer.add("media/noise.bin", noise, time);
        CHECK_THROWS(writer.add("MEDIA\\TEXT.BIN", text, time));
        writer.finish();
    }
    const lzx::ZipArchive archive = lzx::ZipArchive::open(target.path());
    CHECK_EQ(archive.entries().size(), std::size_t{2});
    const lzx::ZipEntry& packed = archive.entries()[0];
    const lzx::ZipEntry& stored = archive.entries()[1];
    CHECK_EQ(packed.method, lzx::kMethodXMemCompress);
    CHECK_EQ(stored.method, lzx::kMethodStored);
    CHECK(archive.read(packed) == text);
    CHECK(archive.read(stored) == noise);
    CHECK(archive.needsLzxReader());
    CHECK_EQ(packed.dosDate, time.date);
    CHECK_EQ(packed.dosTime, time.time);
    CHECK_EQ(packed.hostSystem, 0);
    CHECK_EQ(packed.externalAttributes, 0u);

    // A local header precedes each entry's data, and the offset field
    // points just past it.
    const auto bytes = fileBytes(target.path());
    CHECK_EQ(le32At(bytes, packed.localHeaderOffset), 0x04034b50u);
    CHECK_EQ(*packed.offsetField, packed.localHeaderOffset + 30u + packed.name.size());
    CHECK_EQ(bytes[4], 10);
}

TEST_CASE("archives can be rewritten in place without recompressing")
{
    const TempArchive target;
    const auto a = runnyBytes(50000, 32);
    const auto b = runnyBytes(20000, 33);
    {
        lzx::ZipWriter writer(target.path());
        writer.add("a.bin", a);
        writer.add("b.bin", b);
        writer.finish();
    }
    std::vector<std::uint8_t> rawA;
    {
        const lzx::ZipArchive source = lzx::ZipArchive::open(target.path());
        rawA = source.rawData(*source.find("a.bin"));
        lzx::ZipWriter writer(target.path());
        writer.copy(source, *source.find("a.bin"), "renamed/a.bin");
        writer.add("c.bin", zipbuilder::bytes("new"));
        writer.finish();
    }
    const lzx::ZipArchive result = lzx::ZipArchive::open(target.path());
    CHECK_EQ(result.entries().size(), std::size_t{2});
    CHECK(result.find("b.bin") == nullptr);
    CHECK(result.read(*result.find("renamed/a.bin")) == a);
    CHECK(result.rawData(*result.find("renamed/a.bin")) == rawA);
    CHECK(result.read(*result.find("c.bin")) == zipbuilder::bytes("new"));

    // Abandoning a writer leaves the archive and its folder as they were.
    const auto before = fileBytes(target.path());
    const auto folder = target.path().parent_path();
    const auto countFiles = [&] {
        return std::distance(std::filesystem::directory_iterator(folder), std::filesystem::directory_iterator());
    };
    const auto filesBefore = countFiles();
    {
        lzx::ZipWriter writer(target.path());
        writer.add("x.bin", zipbuilder::bytes("x"));
    }
    CHECK(fileBytes(target.path()) == before);
    CHECK_EQ(countFiles(), filesBefore);
}

TEST_CASE("written archives may hold more than 65535 entries")
{
    const TempArchive target;
    {
        lzx::ZipWriter writer(target.path());
        for (int i = 0; i < 70000; ++i) {
            writer.add("f" + std::to_string(i), zipbuilder::bytes(std::to_string(i)));
        }
        writer.finish();
    }
    const lzx::ZipArchive archive = lzx::ZipArchive::open(target.path());
    CHECK_EQ(archive.entries().size(), std::size_t{70000});
    CHECK(archive.read(*archive.find("f69999")) == zipbuilder::bytes("69999"));
}

TEST_CASE("file times convert to local DOS time")
{
    std::tm local{};
    local.tm_year = 2012 - 1900;
    local.tm_mon = 9;
    local.tm_mday = 23;
    local.tm_hour = 9;
    local.tm_min = 30;
    local.tm_sec = 44;
    local.tm_isdst = -1;
    const auto systemTime = std::chrono::system_clock::from_time_t(std::mktime(&local));
    const auto written = std::filesystem::temp_directory_path() / "lzx-test-time";
    std::ofstream(written) << "x";
#if __cpp_lib_chrono >= 201907L
    const auto fileTime = std::chrono::clock_cast<std::chrono::file_clock>(systemTime);
#else
    const auto fileTime = std::chrono::file_clock::from_sys(systemTime);
#endif
    std::filesystem::last_write_time(written, fileTime);
    const auto stamp = lzx::DosDateTime::fromFileTime(std::filesystem::last_write_time(written));
    std::filesystem::remove(written);
    CHECK_EQ(stamp.date, ((2012u - 1980u) << 9) | (10u << 5) | 23u);
    CHECK_EQ(stamp.time, (9u << 11) | (30u << 5) | 22u);
}

int main()
{
    return check::runAll();
}
