#include "decoder.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace lzx::detail {

namespace {

std::uint16_t readBe16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

int positionSlotsForWindow(unsigned windowBits)
{
    if (windowBits < 15 || windowBits > 21) {
        throw Error("unsupported LZX window size 2^" + std::to_string(windowBits));
    }
    if (windowBits == 20) {
        return 42;
    }
    if (windowBits == 21) {
        return 50;
    }
    return static_cast<int>(windowBits) * 2;
}

struct Chunk {
    std::span<const std::uint8_t> payload;
    std::size_t outputSize;
};

} // namespace

void LzxDecoder::BitReader::reset(std::span<const std::uint8_t> chunk)
{
    m_data = chunk;
    m_pos = 0;
    m_wordOrigin = 0;
    m_originPending = true;
    m_buffer = 0;
    m_bitsLeft = 0;
}

std::uint32_t LzxDecoder::BitReader::peek(int n)
{
    if (m_bitsLeft < n) {
        refill();
    }
    return n == 0 ? 0 : static_cast<std::uint32_t>(m_buffer >> (64 - n));
}

void LzxDecoder::BitReader::consume(int n)
{
    m_buffer <<= n;
    m_bitsLeft -= n;
}

std::uint32_t LzxDecoder::BitReader::read(int n)
{
    const std::uint32_t value = peek(n);
    consume(n);
    return value;
}

void LzxDecoder::BitReader::refill()
{
    // Past the end of the chunk the reader supplies zero words, so a Huffman
    // lookahead near the end is safe; decodeFrame rejects any real overrun.
    if (m_originPending) {
        m_wordOrigin = m_pos;
        m_originPending = false;
    }
    while (m_bitsLeft <= 48) {
        std::uint32_t word = 0;
        if (m_pos + 1 < m_data.size()) {
            word = static_cast<std::uint32_t>(m_data[m_pos] | (m_data[m_pos + 1] << 8));
        } else if (m_pos < m_data.size()) {
            word = m_data[m_pos];
        }
        m_pos += 2;
        m_buffer |= static_cast<std::uint64_t>(word) << (48 - m_bitsLeft);
        m_bitsLeft += 16;
    }
}

void LzxDecoder::BitReader::beginRawBytes()
{
    const std::size_t bitsSinceOrigin = consumedBits() - m_wordOrigin * 8;
    const std::size_t alignedBits = (bitsSinceOrigin / 16 + 1) * 16;
    m_pos = m_wordOrigin + alignedBits / 8;
    m_buffer = 0;
    m_bitsLeft = 0;
    m_originPending = true;
}

std::uint8_t LzxDecoder::BitReader::readRawByte()
{
    if (m_pos >= m_data.size()) {
        throw Error("uncompressed block runs past end of chunk");
    }
    return m_data[m_pos++];
}

std::uint32_t LzxDecoder::BitReader::readRawU32le()
{
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(readRawByte()) << (8 * i);
    }
    return value;
}

void LzxDecoder::HuffmanTable::build()
{
    std::fill(std::begin(counts), std::end(counts), std::uint16_t{0});
    for (std::uint8_t len : lengths) {
        ++counts[len];
    }
    counts[0] = 0;

    int left = 1;
    for (int len = 1; len <= kMaxCodeLength; ++len) {
        left <<= 1;
        left -= counts[len];
        if (left < 0) {
            throw Error("over-subscribed Huffman table");
        }
    }
    // An unused table is all zeros. Anything else must be a complete code,
    // as libmspack and the console's decoder expect.
    if (left != 0 && left != (1 << kMaxCodeLength)) {
        throw Error("incomplete Huffman table");
    }

    std::uint16_t offsets[kMaxCodeLength + 2] = {};
    for (int len = 1; len <= kMaxCodeLength; ++len) {
        offsets[len + 1] = static_cast<std::uint16_t>(offsets[len] + counts[len]);
    }
    const std::size_t used = offsets[kMaxCodeLength + 1];
    empty = used == 0;
    sortedSymbols.assign(used, 0);
    for (std::size_t sym = 0; sym < lengths.size(); ++sym) {
        if (lengths[sym] != 0) {
            sortedSymbols[offsets[lengths[sym]]++] = static_cast<std::uint16_t>(sym);
        }
    }

    // The fast table maps the next kFastBits bits straight to (symbol << 5 | length)
    // for every code no longer than kFastBits; 0xFFFF marks a slow-path prefix.
    fast.assign(std::size_t{1} << kFastBits, 0xFFFF);
    std::uint32_t code = 0;
    std::size_t index = 0;
    for (int len = 1; len <= kMaxCodeLength; ++len) {
        for (int i = 0; i < counts[len]; ++i, ++code, ++index) {
            if (len <= kFastBits) {
                const std::uint32_t start = code << (kFastBits - len);
                const std::uint32_t span = 1u << (kFastBits - len);
                const auto entry = static_cast<std::uint16_t>((sortedSymbols[index] << 5) | len);
                std::fill_n(fast.begin() + start, span, entry);
            }
        }
        code <<= 1;
    }
}

int LzxDecoder::HuffmanTable::decode(BitReader& in) const
{
    if (empty) {
        throw Error("symbol read from an empty Huffman table");
    }
    const std::uint32_t lookahead = in.peek(kMaxCodeLength);
    const std::uint16_t entry = fast[lookahead >> (kMaxCodeLength - kFastBits)];
    if (entry != 0xFFFF) {
        in.consume(entry & 0x1F);
        return entry >> 5;
    }

    std::int32_t code = 0;
    std::int32_t first = 0;
    std::int32_t index = 0;
    for (int len = 1; len <= kMaxCodeLength; ++len) {
        code |= static_cast<std::int32_t>((lookahead >> (kMaxCodeLength - len)) & 1);
        const std::int32_t count = counts[len];
        if (code - first < count) {
            in.consume(len);
            return sortedSymbols[static_cast<std::size_t>(index + code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    throw Error("invalid Huffman code");
}

LzxDecoder::LzxDecoder(unsigned windowBits)
    : m_windowSize(std::size_t{1} << windowBits)
    , m_window(m_windowSize, 0)
    , m_positionSlots(positionSlotsForWindow(windowBits))
    , m_pretree(kPretreeSymbols)
    , m_mainTree(static_cast<std::size_t>(kNumChars + m_positionSlots * 8))
    , m_lengthTree(kSecondaryLengths)
    , m_alignedTree(kAlignedSymbols)
{
    std::uint8_t extra = 0;
    for (int i = 0; i < 52; i += 2) {
        m_extraBits[i] = extra;
        m_extraBits[i + 1] = extra;
        if (i != 0 && extra < 17) {
            ++extra;
        }
    }
    std::uint32_t base = 0;
    for (int i = 0; i < 52; ++i) {
        m_positionBase[i] = base;
        base += 1u << m_extraBits[i];
    }
}

std::vector<std::uint8_t> LzxDecoder::decompress(
    std::span<const std::uint8_t> input, std::size_t expectedSize, unsigned windowBits, std::size_t maxOutput)
{
    std::vector<Chunk> chunks;
    std::size_t declaredTotal = 0;
    std::size_t pos = 0;
    while (pos < input.size() && declaredTotal < expectedSize) {
        std::size_t outputSize = kFrameSize;
        std::size_t payloadSize = 0;
        if (input[pos] == 0xFF) {
            if (pos + 5 > input.size()) {
                throw Error("truncated chunk header");
            }
            outputSize = readBe16(&input[pos + 1]);
            payloadSize = readBe16(&input[pos + 3]);
            pos += 5;
        } else {
            if (pos + 2 > input.size()) {
                throw Error("truncated chunk header");
            }
            payloadSize = readBe16(&input[pos]);
            pos += 2;
        }
        if (payloadSize == 0 || outputSize == 0) {
            break;
        }
        if (pos + payloadSize > input.size()) {
            throw Error("chunk payload runs past end of input");
        }
        chunks.push_back({input.subspan(pos, payloadSize), outputSize});
        pos += payloadSize;
        declaredTotal += outputSize;
    }

    if (declaredTotal != expectedSize) {
        throw Error("chunk headers describe " + std::to_string(declaredTotal) + " bytes, container expects "
            + std::to_string(expectedSize));
    }

    LzxDecoder decoder(windowBits);
    std::vector<std::uint8_t> out;
    out.reserve(expectedSize);
    for (const Chunk& chunk : chunks) {
        if (out.size() >= maxOutput) {
            out.resize(maxOutput);
            break;
        }
        decoder.decodeFrame(chunk.payload, chunk.outputSize, out);
    }
    if (out.size() > maxOutput) {
        out.resize(maxOutput);
    }
    return out;
}

void LzxDecoder::decodeFrame(std::span<const std::uint8_t> chunk, std::size_t frameSize, std::vector<std::uint8_t>& out)
{
    if (frameSize > kFrameSize) {
        throw Error("chunk decodes to more than 32 KiB");
    }
    m_in.reset(chunk);
    if (!m_headerRead) {
        if (m_in.read(1) != 0) {
            m_intelFileSize = (m_in.read(16) << 16) | m_in.read(16);
        }
        m_headerRead = true;
    }

    m_frameStart = m_windowPos;
    std::size_t todo = frameSize;
    while (todo > 0) {
        if (m_blockRemaining == 0) {
            readBlockHeader();
        }
        const std::size_t run = std::min(m_blockRemaining, todo);
        if (m_blockType == BlockType::Uncompressed) {
            decodeUncompressed(run);
            m_blockRemaining -= run;
            todo -= run;
        } else {
            const std::size_t produced = decodeCompressed(run);
            if (produced > m_blockRemaining || produced > todo) {
                throw Error("match runs past the end of its block or frame");
            }
            m_blockRemaining -= produced;
            todo -= produced;
        }
    }

    if (m_windowPos - m_frameStart != frameSize) {
        throw Error("frame decoded to the wrong size");
    }
    const std::size_t usedBytes = (m_in.consumedBits() + 7) / 8;
    if (usedBytes > chunk.size()) {
        throw Error("bitstream overran its chunk");
    }

    const std::size_t outStart = out.size();
    out.insert(out.end(), m_window.begin() + static_cast<std::ptrdiff_t>(m_frameStart),
        m_window.begin() + static_cast<std::ptrdiff_t>(m_frameStart + frameSize));
    applyE8Translation(out.data() + outStart, frameSize);

    m_totalOut += frameSize;
    ++m_framesDone;
    if (m_windowPos == m_windowSize) {
        m_windowPos = 0;
    }
}

void LzxDecoder::readBlockHeader()
{
    const auto type = m_in.read(3);
    const std::uint32_t high = m_in.read(16);
    const std::uint32_t low = m_in.read(8);
    m_blockRemaining = (high << 8) | low;
    if (m_blockRemaining == 0) {
        throw Error("zero-length LZX block");
    }

    switch (type) {
    case 2:
        for (int i = 0; i < kAlignedSymbols; ++i) {
            m_alignedTree.lengths[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(m_in.read(3));
        }
        m_alignedTree.build();
        [[fallthrough]];
    case 1:
        readLengths(m_mainTree, 0, kNumChars);
        readLengths(m_mainTree, kNumChars, m_mainTree.lengths.size());
        m_mainTree.build();
        if (m_mainTree.lengths[0xE8] != 0) {
            m_intelStarted = true;
        }
        readLengths(m_lengthTree, 0, kSecondaryLengths);
        m_lengthTree.build();
        m_blockType = type == 1 ? BlockType::Verbatim : BlockType::Aligned;
        break;
    case 3:
        m_intelStarted = true;
        m_in.beginRawBytes();
        m_r0 = m_in.readRawU32le();
        m_r1 = m_in.readRawU32le();
        m_r2 = m_in.readRawU32le();
        m_blockType = BlockType::Uncompressed;
        break;
    default:
        throw Error("bad LZX block type " + std::to_string(type));
    }
}

void LzxDecoder::readLengths(HuffmanTable& table, std::size_t first, std::size_t last)
{
    for (int i = 0; i < kPretreeSymbols; ++i) {
        m_pretree.lengths[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(m_in.read(4));
    }
    m_pretree.build();

    auto& lens = table.lengths;
    auto fill = [&](std::size_t& x, std::size_t count, std::uint8_t value) {
        if (x + count > lens.size()) {
            throw Error("code length run past end of table");
        }
        std::fill_n(lens.begin() + static_cast<std::ptrdiff_t>(x), count, value);
        x += count;
    };

    std::size_t x = first;
    while (x < last) {
        const int sym = m_pretree.decode(m_in);
        if (sym == 17) {
            fill(x, m_in.read(4) + 4, 0);
        } else if (sym == 18) {
            fill(x, m_in.read(5) + 20, 0);
        } else if (sym == 19) {
            const std::size_t count = m_in.read(1) + 4;
            const int delta = m_pretree.decode(m_in);
            if (delta > 16) {
                throw Error("bad pretree delta");
            }
            fill(x, count, static_cast<std::uint8_t>((lens[x] + 17 - delta) % 17));
        } else {
            lens[x] = static_cast<std::uint8_t>((lens[x] + 17 - sym) % 17);
            ++x;
        }
    }
}

std::size_t LzxDecoder::decodeCompressed(std::size_t runLength)
{
    const bool aligned = m_blockType == BlockType::Aligned;
    std::size_t produced = 0;
    while (produced < runLength) {
        int mainElement = m_mainTree.decode(m_in);
        if (mainElement < kNumChars) {
            m_window[m_windowPos++] = static_cast<std::uint8_t>(mainElement);
            ++produced;
            continue;
        }

        mainElement -= kNumChars;
        std::size_t matchLength = static_cast<std::size_t>(mainElement & 7);
        if (matchLength == 7) {
            matchLength += static_cast<std::size_t>(m_lengthTree.decode(m_in));
        }
        matchLength += 2;

        const auto slot = static_cast<std::uint32_t>(mainElement >> 3);
        std::uint32_t matchOffset = 0;
        if (slot > 2) {
            const std::uint32_t extra = slot >= 36 ? 17 : m_extraBits[slot];
            if (!aligned) {
                if (slot != 3) {
                    matchOffset = m_positionBase[slot] - 2 + m_in.read(static_cast<int>(extra));
                } else {
                    matchOffset = 1;
                }
            } else {
                matchOffset = m_positionBase[slot] - 2;
                if (extra > 3) {
                    matchOffset += m_in.read(static_cast<int>(extra - 3)) << 3;
                    matchOffset += static_cast<std::uint32_t>(m_alignedTree.decode(m_in));
                } else if (extra == 3) {
                    matchOffset += static_cast<std::uint32_t>(m_alignedTree.decode(m_in));
                } else if (extra > 0) {
                    matchOffset += m_in.read(static_cast<int>(extra));
                } else {
                    matchOffset = 1;
                }
            }
            m_r2 = m_r1;
            m_r1 = m_r0;
            m_r0 = matchOffset;
        } else if (slot == 0) {
            matchOffset = m_r0;
        } else if (slot == 1) {
            matchOffset = m_r1;
            m_r1 = m_r0;
            m_r0 = matchOffset;
        } else {
            matchOffset = m_r2;
            m_r2 = m_r0;
            m_r0 = matchOffset;
        }

        if (m_windowPos + matchLength > m_windowSize) {
            throw Error("match runs past end of window");
        }
        copyMatch(matchLength, matchOffset);
        produced += matchLength;
    }
    return produced;
}

void LzxDecoder::decodeUncompressed(std::size_t runLength)
{
    for (std::size_t i = 0; i < runLength; ++i) {
        m_window[m_windowPos++] = m_in.readRawByte();
    }
}

void LzxDecoder::copyMatch(std::size_t length, std::uint32_t distance)
{
    if (distance == 0 || distance > m_windowSize) {
        throw Error("match distance out of range");
    }
    const std::size_t decodedSoFar = m_totalOut + (m_windowPos - m_frameStart);
    if (distance > decodedSoFar) {
        throw Error("match refers to data before the start of the stream");
    }
    std::size_t src = (m_windowPos + m_windowSize - distance) % m_windowSize;
    for (std::size_t i = 0; i < length; ++i) {
        m_window[m_windowPos++] = m_window[src];
        src = (src + 1) % m_windowSize;
    }
}

void LzxDecoder::applyE8Translation(std::uint8_t* data, std::size_t size)
{
    const std::uint32_t frameCurPos = m_intelCurPos;
    m_intelCurPos += static_cast<std::uint32_t>(size);
    if (!m_intelStarted || m_intelFileSize == 0 || m_framesDone >= 32768 || size <= 10) {
        return;
    }
    const auto fileSize = static_cast<std::int32_t>(m_intelFileSize);
    auto curPos = static_cast<std::int32_t>(frameCurPos);
    std::uint8_t* p = data;
    std::uint8_t* const end = data + size - 10;
    while (p < end) {
        if (*p++ != 0xE8) {
            ++curPos;
            continue;
        }
        const auto absOff
            = static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
                | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24));
        if (absOff >= -curPos && absOff < fileSize) {
            const auto relOff = static_cast<std::uint32_t>(absOff >= 0 ? absOff - curPos : absOff + fileSize);
            p[0] = static_cast<std::uint8_t>(relOff);
            p[1] = static_cast<std::uint8_t>(relOff >> 8);
            p[2] = static_cast<std::uint8_t>(relOff >> 16);
            p[3] = static_cast<std::uint8_t>(relOff >> 24);
        }
        p += 4;
        curPos += 5;
    }
}

} // namespace lzx::detail

namespace lzx {

std::vector<std::uint8_t> decompress(
    std::span<const std::uint8_t> input, std::size_t expectedSize, unsigned windowBits, std::size_t maxOutput)
{
    return detail::LzxDecoder::decompress(input, expectedSize, windowBits, maxOutput);
}

} // namespace lzx
