#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

/// Encoder and decoder for the LZX variant of the Xbox 360 XMemCompress API,
/// stored as ZIP compression method 21 by Xbox 360 titles such as Forza Horizon.
namespace lzx {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// XMemCompress's default window: 128 KiB.
inline constexpr unsigned kDefaultWindowBits = 17;

/// Decompresses a complete XMemCompress chunk stream.
///
/// The stream is a sequence of chunks, each starting with either
/// `0xFF, u16be uncompressedSize, u16be compressedSize` or just
/// `u16be compressedSize` (implying 32 KiB of output). `expectedSize` is the
/// size recorded by the container; decoding fails if the chunks describe a
/// different amount. With `maxOutput` set, decoding stops after the first
/// chunks that cover at least that many bytes and only those are returned.
/// Throws Error on malformed input.
std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> input, std::size_t expectedSize,
    unsigned windowBits = kDefaultWindowBits, std::size_t maxOutput = SIZE_MAX);

/// Compresses `input` into an XMemCompress chunk stream with the default
/// 128 KiB window, which decompress() and the console's XMemDecompress read
/// back. Each 32 KiB frame is one verbatim or aligned-offset block, or is
/// stored when it does not compress. Returns an empty stream for empty input.
std::vector<std::uint8_t> compress(std::span<const std::uint8_t> input);

} // namespace lzx
