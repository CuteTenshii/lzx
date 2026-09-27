#pragma once

#include <lzx/lzx.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lzx::detail {

/// Decoder for the LZX variant produced by the Xbox 360 XMemCompress API.
///
/// The container stream is a sequence of chunks. A chunk starts with either
/// `0xFF, u16be uncompressedSize, u16be compressedSize` or just
/// `u16be compressedSize` (implying 32 KiB of output), and its payload decodes
/// to exactly one LZX output frame. Decoder state (window, repeat offsets,
/// Huffman code lengths, the block in progress) carries from chunk to chunk,
/// while the bit reader restarts at each chunk. Unlike cabinet LZX, an
/// odd-length uncompressed block is not followed by a pad byte: the next block
/// header starts on the very next byte, even if that is an odd offset.
class LzxDecoder {
public:
    /// See lzx::decompress().
    static std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> input, std::size_t expectedSize,
        unsigned windowBits = 17, std::size_t maxOutput = SIZE_MAX);

private:
    static constexpr int kNumChars = 256;
    static constexpr int kPretreeSymbols = 20;
    static constexpr int kSecondaryLengths = 249;
    static constexpr int kAlignedSymbols = 8;
    static constexpr int kMaxCodeLength = 16;
    static constexpr int kFastBits = 10;
    static constexpr std::size_t kFrameSize = 32768;

    enum class BlockType : std::uint8_t { None = 0, Verbatim = 1, Aligned = 2, Uncompressed = 3 };

    /// Reads one chunk's payload as little-endian 16-bit words consumed MSB
    /// first, or as raw bytes inside uncompressed blocks.
    class BitReader {
    public:
        void reset(std::span<const std::uint8_t> chunk);
        std::uint32_t peek(int n);
        void consume(int n);
        std::uint32_t read(int n);
        /// Enters raw byte mode for an uncompressed block: skips 1 to 16 bits
        /// of padding to the next word boundary. Bit reading resumes at
        /// whatever byte follows the raw data, which becomes the new origin
        /// for word alignment.
        void beginRawBytes();
        std::uint8_t readRawByte();
        std::uint32_t readRawU32le();
        /// Bits consumed from the chunk, counting 16-bit alignment padding.
        std::size_t consumedBits() const { return m_pos * 8 - static_cast<std::size_t>(m_bitsLeft); }
        std::size_t size() const { return m_data.size(); }

    private:
        void refill();
        std::span<const std::uint8_t> m_data;
        std::size_t m_pos = 0;
        /// Byte where the current run of 16-bit words began: the chunk start,
        /// or the byte after an uncompressed block's data.
        std::size_t m_wordOrigin = 0;
        /// Set whenever the next refill starts a new run of words: at a chunk
        /// start and after raw bytes, where a raw block may end mid-chunk.
        bool m_originPending = true;
        std::uint64_t m_buffer = 0;
        int m_bitsLeft = 0;
    };

    struct HuffmanTable {
        std::vector<std::uint8_t> lengths;
        std::vector<std::uint16_t> sortedSymbols;
        std::uint16_t counts[kMaxCodeLength + 1] = {};
        std::vector<std::uint16_t> fast;
        bool empty = true;

        explicit HuffmanTable(std::size_t symbolCount)
            : lengths(symbolCount, 0)
        {
        }
        void build();
        int decode(BitReader& in) const;
    };

    explicit LzxDecoder(unsigned windowBits);

    void decodeFrame(std::span<const std::uint8_t> chunk, std::size_t frameSize, std::vector<std::uint8_t>& out);
    void readBlockHeader();
    void readLengths(HuffmanTable& table, std::size_t first, std::size_t last);
    std::size_t decodeCompressed(std::size_t runLength);
    void decodeUncompressed(std::size_t runLength);
    void copyMatch(std::size_t length, std::uint32_t distance);
    void applyE8Translation(std::uint8_t* data, std::size_t size);

    BitReader m_in;

    std::size_t m_windowSize;
    std::vector<std::uint8_t> m_window;
    std::size_t m_windowPos = 0;
    std::size_t m_frameStart = 0;
    int m_positionSlots;

    std::uint32_t m_r0 = 1, m_r1 = 1, m_r2 = 1;
    bool m_headerRead = false;
    bool m_intelStarted = false;
    std::uint32_t m_intelFileSize = 0;
    std::uint32_t m_intelCurPos = 0;
    std::size_t m_framesDone = 0;
    std::size_t m_totalOut = 0;

    BlockType m_blockType = BlockType::None;
    std::size_t m_blockRemaining = 0;

    HuffmanTable m_pretree;
    HuffmanTable m_mainTree;
    HuffmanTable m_lengthTree;
    HuffmanTable m_alignedTree;

    std::uint32_t m_positionBase[52] = {};
    std::uint8_t m_extraBits[52] = {};
};

} // namespace lzx::detail
