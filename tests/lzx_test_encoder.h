#pragma once

// A minimal XMemCompress-style LZX encoder for tests. It produces valid
// streams using uncompressed blocks and verbatim blocks with fixed Huffman
// codes (every literal and every repeat-offset match gets a 9-bit code), and
// splits the output into chunks exactly like the Xbox 360 encoder does:
// one chunk per 32 KiB of output, with the bitstream restarting per chunk.

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace lzxtest {

class BitWriter {
public:
    void put(std::uint32_t value, int bits)
    {
        for (int i = bits - 1; i >= 0; --i) {
            m_word = static_cast<std::uint16_t>((m_word << 1) | ((value >> i) & 1u));
            if (++m_bits == 16) {
                flushWord();
            }
        }
    }

    /// Pads with zero bits to the next 16-bit boundary; no-op when aligned.
    void alignWord()
    {
        if (m_bits != 0) {
            put(0, 16 - m_bits);
        }
    }

    /// The padding an uncompressed block header needs: 1 to 16 bits.
    void padForRawBytes() { put(0, m_bits == 0 ? 16 : 16 - m_bits); }

    void rawByte(std::uint8_t byte)
    {
        if (m_bits != 0) {
            throw std::logic_error("raw byte written mid-word");
        }
        m_bytes.push_back(byte);
    }

    void rawU32le(std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i) {
            rawByte(static_cast<std::uint8_t>(value >> (8 * i)));
        }
    }

    bool midWord() const { return m_bits != 0; }
    std::vector<std::uint8_t> take()
    {
        std::vector<std::uint8_t> bytes;
        bytes.swap(m_bytes);
        m_word = 0;
        m_bits = 0;
        return bytes;
    }

private:
    void flushWord()
    {
        m_bytes.push_back(static_cast<std::uint8_t>(m_word & 0xFF));
        m_bytes.push_back(static_cast<std::uint8_t>(m_word >> 8));
        m_word = 0;
        m_bits = 0;
    }

    std::vector<std::uint8_t> m_bytes;
    std::uint16_t m_word = 0;
    int m_bits = 0;
};

enum class BlockKind { Uncompressed, Verbatim };

struct Block {
    BlockKind kind;
    std::size_t length;
};

/// Encodes `data` as the given sequence of blocks (their lengths must sum to
/// data.size()) and returns the full chunked stream. `intelFileSize` != 0
/// writes the E8 translation header.
inline std::vector<std::uint8_t> encode(
    const std::vector<std::uint8_t>& data, const std::vector<Block>& blocks, std::uint32_t intelFileSize = 0)
{
    constexpr std::size_t kFrame = 32768;
    constexpr int kMainSymbols = 256 + 34 * 8;
    std::vector<std::vector<std::uint8_t>> chunks;
    std::vector<std::size_t> chunkOutput;
    BitWriter writer;
    std::size_t produced = 0;
    std::size_t chunkStart = 0;
    bool treesSent = false;

    auto endChunkIfFull = [&] {
        if (produced - chunkStart == kFrame) {
            writer.alignWord();
            chunks.push_back(writer.take());
            chunkOutput.push_back(kFrame);
            chunkStart = produced;
        }
    };
    auto writeLengths = [&](int count, int symbol) {
        for (int i = 0; i < 20; ++i) {
            writer.put(i < 16 ? 4 : 0, 4);
        }
        for (int i = 0; i < count; ++i) {
            writer.put(static_cast<std::uint32_t>(symbol), 4);
        }
    };

    if (intelFileSize != 0) {
        writer.put(1, 1);
        writer.put(intelFileSize >> 16, 16);
        writer.put(intelFileSize & 0xFFFF, 16);
    } else {
        writer.put(0, 1);
    }

    for (const Block& block : blocks) {
        writer.put(block.kind == BlockKind::Uncompressed ? 3 : 1, 3);
        writer.put(static_cast<std::uint32_t>(block.length >> 8), 16);
        writer.put(static_cast<std::uint32_t>(block.length & 0xFF), 8);
        const std::size_t blockEnd = produced + block.length;

        if (block.kind == BlockKind::Uncompressed) {
            writer.padForRawBytes();
            writer.rawU32le(1);
            writer.rawU32le(1);
            writer.rawU32le(1);
            while (produced < blockEnd) {
                writer.rawByte(data[produced++]);
                endChunkIfFull();
            }
            continue;
        }

        // Pretree symbol k sets a length to (previous - k) mod 17. The first
        // verbatim block raises 512 main-tree lengths from 0 to 9 (symbol 8);
        // later blocks keep them (symbol 0). The length tree stays empty.
        const int raise = treesSent ? 0 : 8;
        writeLengths(0, 0);
        for (int i = 0; i < 256; ++i) {
            writer.put(static_cast<std::uint32_t>(raise), 4);
        }
        writeLengths(0, 0);
        for (int i = 256; i < kMainSymbols; ++i) {
            writer.put(static_cast<std::uint32_t>(i < 512 ? raise : 0), 4);
        }
        writeLengths(249, 0);
        treesSent = true;

        while (produced < blockEnd) {
            const std::size_t frameLeft = kFrame - (produced - chunkStart);
            const std::size_t limit = std::min<std::size_t>({8, blockEnd - produced, frameLeft});
            std::size_t run = 0;
            if (produced > 0) {
                while (run < limit && data[produced + run] == data[produced - 1]) {
                    ++run;
                }
            }
            if (run >= 2) {
                // Slot 0 repeats offset R0, which stays 1 in these streams.
                writer.put(static_cast<std::uint32_t>(256 + (run - 2)), 9);
                produced += run;
            } else {
                writer.put(data[produced], 9);
                ++produced;
            }
            endChunkIfFull();
        }
    }

    if (produced != data.size()) {
        throw std::logic_error("block lengths do not cover the data");
    }
    if (produced > chunkStart) {
        writer.alignWord();
        chunks.push_back(writer.take());
        chunkOutput.push_back(produced - chunkStart);
    }

    std::vector<std::uint8_t> stream;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const std::size_t size = chunks[i].size();
        if (chunkOutput[i] != kFrame) {
            stream.push_back(0xFF);
            stream.push_back(static_cast<std::uint8_t>(chunkOutput[i] >> 8));
            stream.push_back(static_cast<std::uint8_t>(chunkOutput[i]));
        }
        stream.push_back(static_cast<std::uint8_t>(size >> 8));
        stream.push_back(static_cast<std::uint8_t>(size));
        stream.insert(stream.end(), chunks[i].begin(), chunks[i].end());
    }
    return stream;
}

} // namespace lzxtest
