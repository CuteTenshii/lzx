#include <lzx/lzx.h>

#include <algorithm>
#include <array>
#include <limits>
#include <queue>
#include <utility>

namespace lzx {

namespace {

constexpr std::size_t kFrameSize = 32768;
constexpr std::size_t kWindowSize = std::size_t{1} << kDefaultWindowBits;
constexpr int kPositionSlots = static_cast<int>(kDefaultWindowBits) * 2;
constexpr int kNumChars = 256;
constexpr int kMainSymbols = kNumChars + kPositionSlots * 8;
constexpr int kLengthSymbols = 249;
constexpr int kPretreeSymbols = 20;
constexpr int kAlignedSymbols = 8;
constexpr int kMaxCodeLength = 16;
constexpr int kMaxPretreeLength = 15;
/// Aligned-tree lengths are sent in 3 bits.
constexpr int kMaxAlignedLength = 7;

constexpr std::size_t kMinMatch = 2;
constexpr std::size_t kMaxMatch = 257;
/// The largest distance LZX can encode: the window size minus 3.
constexpr std::size_t kMaxDistance = kWindowSize - 3;

constexpr int kHashBits = 16;
constexpr std::uint32_t kNoPosition = 0xFFFFFFFF;
/// Candidates examined per position, and the length at which the search
/// stops early. Together they trade speed for ratio.
constexpr int kMaxChain = 96;
constexpr std::size_t kNiceLength = 96;

/// Assumed cost in bits of a symbol the previous block did not use.
constexpr std::uint32_t kUnusedSymbolPrice = 14;
constexpr std::uint32_t kInfinity = std::numeric_limits<std::uint32_t>::max();

constexpr int kBlockVerbatim = 1;
constexpr int kBlockAligned = 2;
constexpr int kBlockUncompressed = 3;

struct PositionTables {
    std::array<std::uint32_t, kPositionSlots> base{};
    std::array<std::uint8_t, kPositionSlots> extraBits{};

    PositionTables()
    {
        // The same tables the decoder builds.
        std::uint8_t extra = 0;
        for (int i = 0; i < kPositionSlots; i += 2) {
            extraBits[static_cast<std::size_t>(i)] = extra;
            extraBits[static_cast<std::size_t>(i) + 1] = extra;
            if (i != 0 && extra < 17) {
                ++extra;
            }
        }
        std::uint32_t next = 0;
        for (std::size_t i = 0; i < base.size(); ++i) {
            base[i] = next;
            next += 1u << extraBits[i];
        }
    }

    /// The slot for a non-repeat match, whose formatted offset is the
    /// distance plus 2.
    int slotFor(std::uint32_t formattedOffset) const
    {
        const auto it = std::upper_bound(base.begin() + 3, base.end(), formattedOffset);
        return static_cast<int>(it - base.begin()) - 1;
    }
};

const PositionTables& positionTables()
{
    static const PositionTables tables;
    return tables;
}

/// Writes 16-bit little-endian words, filled from the most significant bit,
/// and raw bytes for uncompressed blocks.
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

    /// Pads with zero bits to the next 16-bit boundary; nothing when aligned.
    void alignWord()
    {
        if (m_bits != 0) {
            put(0, 16 - m_bits);
        }
    }

    /// An uncompressed block's header is followed by 1 to 16 padding bits.
    void padForRawBytes() { put(0, m_bits == 0 ? 16 : 16 - m_bits); }

    void rawBytes(const std::uint8_t* data, std::size_t size) { m_bytes.insert(m_bytes.end(), data, data + size); }

    void rawU32le(std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i) {
            m_bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
        }
    }

    std::size_t size() const { return m_bytes.size(); }
    const std::vector<std::uint8_t>& bytes() const { return m_bytes; }

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

/// Code lengths for `frequencies`, no longer than `maxLength`; unused
/// symbols get 0. The code is always complete: a lone symbol is paired with
/// a second 1-bit code, because stricter LZX decoders (libmspack among them)
/// reject incomplete tables.
std::vector<std::uint8_t> huffmanLengths(const std::vector<std::uint32_t>& frequencies, int maxLength)
{
    std::vector<std::uint8_t> lengths(frequencies.size(), 0);
    std::vector<std::uint32_t> weights = frequencies;
    const auto used = static_cast<std::size_t>(
        std::count_if(weights.begin(), weights.end(), [](std::uint32_t w) { return w != 0; }));
    if (used == 0) {
        return lengths;
    }
    if (used == 1) {
        const auto symbol = static_cast<std::size_t>(
            std::find_if(weights.begin(), weights.end(), [](std::uint32_t w) { return w != 0; }) - weights.begin());
        lengths[symbol] = 1;
        lengths[symbol == 0 ? 1 : 0] = 1;
        return lengths;
    }

    while (true) {
        // Nodes 0..n-1 are symbols; parents are appended after them.
        const std::size_t n = weights.size();
        std::vector<std::uint64_t> nodeWeight(weights.begin(), weights.end());
        std::vector<std::size_t> parent(n, 0);
        using Item = std::pair<std::uint64_t, std::size_t>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
        for (std::size_t i = 0; i < n; ++i) {
            if (weights[i] != 0) {
                queue.emplace(weights[i], i);
            }
        }
        while (queue.size() > 1) {
            const auto [w1, a] = queue.top();
            queue.pop();
            const auto [w2, b] = queue.top();
            queue.pop();
            const std::size_t node = nodeWeight.size();
            nodeWeight.push_back(w1 + w2);
            parent.push_back(0);
            parent[a] = node;
            parent[b] = node;
            queue.emplace(w1 + w2, node);
        }
        const std::size_t root = queue.top().second;

        int longest = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (weights[i] == 0) {
                lengths[i] = 0;
                continue;
            }
            int depth = 0;
            for (std::size_t node = i; node != root; node = parent[node]) {
                ++depth;
            }
            lengths[i] = static_cast<std::uint8_t>(depth);
            longest = std::max(longest, depth);
        }
        if (longest <= maxLength) {
            return lengths;
        }
        // Flatten the distribution and try again; rare, and only costs a
        // little ratio on the affected block.
        for (std::uint32_t& w : weights) {
            if (w != 0) {
                w = (w + 1) / 2;
            }
        }
    }
}

/// Canonical codes for `lengths`, in the order the decoder assigns them.
std::vector<std::uint16_t> canonicalCodes(const std::vector<std::uint8_t>& lengths)
{
    std::array<std::uint16_t, kMaxCodeLength + 2> count{};
    for (const std::uint8_t length : lengths) {
        ++count[length];
    }
    count[0] = 0;
    std::array<std::uint16_t, kMaxCodeLength + 2> next{};
    std::uint32_t code = 0;
    for (int length = 1; length <= kMaxCodeLength; ++length) {
        code = (code + count[static_cast<std::size_t>(length) - 1]) << 1;
        next[static_cast<std::size_t>(length)] = static_cast<std::uint16_t>(code);
    }
    std::vector<std::uint16_t> codes(lengths.size(), 0);
    for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
        if (lengths[symbol] != 0) {
            codes[symbol] = next[lengths[symbol]]++;
        }
    }
    return codes;
}

struct Code {
    std::vector<std::uint8_t> lengths;
    std::vector<std::uint16_t> codes;

    static Code build(const std::vector<std::uint32_t>& frequencies, int maxLength)
    {
        Code code;
        code.lengths = huffmanLengths(frequencies, maxLength);
        code.codes = canonicalCodes(code.lengths);
        return code;
    }

    void write(BitWriter& out, std::size_t symbol) const { out.put(codes[symbol], lengths[symbol]); }
};

struct PretreeSymbol {
    std::uint8_t symbol;
    std::uint8_t extraBits;
    std::uint32_t extra;
};

/// Sends `lengths[first, last)` as changes from `previous`, the way the
/// decoder's pretree reads them: runs of zeros with symbols 17 and 18,
/// everything else as a difference modulo 17.
void writeLengths(BitWriter& out, const std::vector<std::uint8_t>& lengths, const std::vector<std::uint8_t>& previous,
    std::size_t first, std::size_t last)
{
    std::vector<PretreeSymbol> symbols;
    std::size_t x = first;
    while (x < last) {
        if (lengths[x] == 0) {
            std::size_t run = 0;
            while (x + run < last && lengths[x + run] == 0 && run < 51) {
                ++run;
            }
            if (run >= 20) {
                symbols.push_back({18, 5, static_cast<std::uint32_t>(run - 20)});
                x += run;
                continue;
            }
            if (run >= 4) {
                symbols.push_back({17, 4, static_cast<std::uint32_t>(run - 4)});
                x += run;
                continue;
            }
        }
        const int delta = (previous[x] - lengths[x] + 17) % 17;
        symbols.push_back({static_cast<std::uint8_t>(delta), 0, 0});
        ++x;
    }

    std::vector<std::uint32_t> frequencies(kPretreeSymbols, 0);
    for (const PretreeSymbol& s : symbols) {
        ++frequencies[s.symbol];
    }
    const Code pretree = Code::build(frequencies, kMaxPretreeLength);
    for (int i = 0; i < kPretreeSymbols; ++i) {
        out.put(pretree.lengths[static_cast<std::size_t>(i)], 4);
    }
    for (const PretreeSymbol& s : symbols) {
        pretree.write(out, s.symbol);
        if (s.extraBits != 0) {
            out.put(s.extra, s.extraBits);
        }
    }
}

struct Token {
    std::uint16_t mainSymbol;
    /// kNoLength when the match length fits in the main symbol.
    std::uint16_t lengthSymbol;
    std::uint8_t extraBits;
    std::uint32_t extra;
};
constexpr std::uint16_t kNoLength = 0xFFFF;

struct RepeatOffsets {
    std::array<std::uint32_t, 3> r{1, 1, 1};
};

class Encoder {
public:
    explicit Encoder(std::span<const std::uint8_t> input)
        : m_input(input)
        , m_head(std::size_t{1} << kHashBits, kNoPosition)
        , m_previous(input.size(), kNoPosition)
        , m_mainLengths(kMainSymbols, 0)
        , m_lengthLengths(kLengthSymbols, 0)
    {
    }

    std::vector<std::uint8_t> run()
    {
        std::vector<std::uint8_t> stream;
        for (std::size_t start = 0; start < m_input.size(); start += kFrameSize) {
            const std::size_t end = std::min(start + kFrameSize, m_input.size());
            const std::vector<std::uint8_t> payload = encodeFrame(start, end, start == 0);
            const std::size_t outputSize = end - start;
            // XMemCompress gives the final chunk the long header even when it
            // holds a full frame, so the stream's end is always explicit.
            if (end == m_input.size()) {
                stream.push_back(0xFF);
                stream.push_back(static_cast<std::uint8_t>(outputSize >> 8));
                stream.push_back(static_cast<std::uint8_t>(outputSize));
            }
            stream.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
            stream.push_back(static_cast<std::uint8_t>(payload.size()));
            stream.insert(stream.end(), payload.begin(), payload.end());
        }
        return stream;
    }

private:
    std::uint32_t hashAt(std::size_t pos) const
    {
        const std::uint32_t value = (static_cast<std::uint32_t>(m_input[pos]) << 16)
            | (static_cast<std::uint32_t>(m_input[pos + 1]) << 8) | m_input[pos + 2];
        return (value * 2654435761u) >> (32 - kHashBits);
    }

    void insert(std::size_t pos)
    {
        if (pos + 3 > m_input.size()) {
            return;
        }
        const std::uint32_t hash = hashAt(pos);
        m_previous[pos] = m_head[hash];
        m_head[hash] = static_cast<std::uint32_t>(pos);
    }

    std::size_t matchLength(std::size_t pos, std::size_t distance, std::size_t limit) const
    {
        const std::uint8_t* a = m_input.data() + pos;
        const std::uint8_t* b = a - distance;
        std::size_t length = 0;
        while (length < limit && a[length] == b[length]) {
            ++length;
        }
        return length;
    }

    /// Emits a match token and updates the repeat offsets as the decoder
    /// will.
    static Token matchToken(std::size_t length, std::size_t distance, int repeat, RepeatOffsets& offsets)
    {
        const PositionTables& tables = positionTables();
        Token token{};
        int slot = 0;
        if (repeat == 0) {
            slot = 0;
        } else if (repeat == 1) {
            slot = 1;
            std::swap(offsets.r[0], offsets.r[1]);
        } else if (repeat == 2) {
            slot = 2;
            std::swap(offsets.r[0], offsets.r[2]);
        } else {
            const auto formatted = static_cast<std::uint32_t>(distance + 2);
            slot = tables.slotFor(formatted);
            token.extraBits = tables.extraBits[static_cast<std::size_t>(slot)];
            token.extra = formatted - tables.base[static_cast<std::size_t>(slot)];
            offsets.r[2] = offsets.r[1];
            offsets.r[1] = offsets.r[0];
            offsets.r[0] = static_cast<std::uint32_t>(distance);
        }
        const std::size_t lengthHeader = std::min<std::size_t>(length - kMinMatch, 7);
        token.mainSymbol = static_cast<std::uint16_t>(kNumChars + slot * 8 + static_cast<int>(lengthHeader));
        token.lengthSymbol =
            lengthHeader == 7 ? static_cast<std::uint16_t>(length - kMinMatch - 7) : kNoLength;
        return token;
    }

    struct Prices {
        std::array<std::uint32_t, kMainSymbols> mainSymbol{};
        std::array<std::uint32_t, kLengthSymbols> lengthSymbol{};

        std::uint32_t match(int slot, std::size_t length) const
        {
            const std::size_t header = std::min<std::size_t>(length - kMinMatch, 7);
            std::uint32_t price = mainSymbol[static_cast<std::size_t>(kNumChars + slot * 8) + header];
            if (header == 7) {
                price += lengthSymbol[length - kMinMatch - 7];
            }
            return price;
        }
    };

    /// Symbol costs in bits: the previous block's code lengths, or a flat
    /// guess before the first block.
    Prices currentPrices() const
    {
        Prices prices;
        const bool haveCodes =
            std::any_of(m_mainLengths.begin(), m_mainLengths.end(), [](std::uint8_t l) { return l != 0; });
        for (std::size_t s = 0; s < prices.mainSymbol.size(); ++s) {
            if (!haveCodes) {
                prices.mainSymbol[s] = s < kNumChars ? 8 : 11;
            } else {
                prices.mainSymbol[s] = m_mainLengths[s] != 0 ? m_mainLengths[s] : kUnusedSymbolPrice;
            }
        }
        for (std::size_t s = 0; s < prices.lengthSymbol.size(); ++s) {
            if (!haveCodes) {
                prices.lengthSymbol[s] = 8;
            } else {
                prices.lengthSymbol[s] = m_lengthLengths[s] != 0 ? m_lengthLengths[s] : kUnusedSymbolPrice;
            }
        }
        return prices;
    }

    /// New-offset matches at `pos` of at least 3 bytes, by increasing length.
    void findMatches(std::size_t pos, std::size_t limit, std::vector<std::pair<std::size_t, std::size_t>>& out) const
    {
        out.clear();
        if (limit < 3 || pos + 3 > m_input.size()) {
            return;
        }
        std::size_t longest = 2;
        std::uint32_t candidate = m_head[hashAt(pos)];
        for (int chain = 0; chain < kMaxChain && candidate != kNoPosition && longest < limit; ++chain) {
            const std::size_t distance = pos - candidate;
            if (distance > kMaxDistance) {
                break;
            }
            if (m_input[candidate + longest] == m_input[pos + longest]) {
                const std::size_t length = matchLength(pos, distance, limit);
                if (length > longest) {
                    longest = length;
                    out.emplace_back(length, distance);
                    if (length >= kNiceLength) {
                        break;
                    }
                }
            }
            candidate = m_previous[candidate];
        }
    }

    /// Chooses the cheapest literals and matches for one frame by a
    /// shortest-path search, tracking the repeat offsets along each path
    /// the way the decoder updates them.
    std::vector<Token> parse(std::size_t start, std::size_t end, RepeatOffsets& offsets)
    {
        struct Node {
            std::uint32_t cost = kInfinity;
            /// Length of the step that reached this node; 1 and distance 0
            /// for a literal.
            std::uint32_t length = 0;
            std::uint32_t distance = 0;
            std::int8_t repeat = -1;
            RepeatOffsets offsets;
        };
        const std::size_t n = end - start;
        const Prices prices = currentPrices();
        const PositionTables& tables = positionTables();
        std::vector<Node> nodes(n + 1);
        nodes[0].cost = 0;
        nodes[0].offsets = offsets;
        const auto relax = [&](std::size_t to, std::uint32_t cost, std::size_t length, std::size_t distance,
                               int repeat, const RepeatOffsets& after) {
            if (cost < nodes[to].cost) {
                nodes[to] = {cost, static_cast<std::uint32_t>(length), static_cast<std::uint32_t>(distance),
                    static_cast<std::int8_t>(repeat), after};
            }
        };

        std::vector<std::pair<std::size_t, std::size_t>> matches;
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t pos = start + i;
            const Node node = nodes[i];
            const std::size_t limit = std::min(kMaxMatch, end - pos);
            findMatches(pos, limit, matches);
            insert(pos);

            relax(i + 1, node.cost + prices.mainSymbol[m_input[pos]], 1, 0, -1, node.offsets);

            std::size_t longest = 0;
            for (int r = 0; r < 3; ++r) {
                const std::size_t distance = node.offsets.r[static_cast<std::size_t>(r)];
                if (distance > pos || limit < kMinMatch) {
                    continue;
                }
                const std::size_t available = matchLength(pos, distance, limit);
                RepeatOffsets after = node.offsets;
                std::swap(after.r[0], after.r[static_cast<std::size_t>(r)]);
                for (std::size_t length = kMinMatch; length <= available; ++length) {
                    relax(i + length, node.cost + prices.match(r, length), length, distance, r, after);
                }
                longest = std::max(longest, available);
            }

            std::size_t covered = kMinMatch;
            for (const auto& [available, distance] : matches) {
                const bool isRepeat = std::find(node.offsets.r.begin(), node.offsets.r.end(), distance)
                    != node.offsets.r.end();
                if (!isRepeat) {
                    const int slot = tables.slotFor(static_cast<std::uint32_t>(distance + 2));
                    const std::uint32_t extra = tables.extraBits[static_cast<std::size_t>(slot)];
                    const RepeatOffsets after{{static_cast<std::uint32_t>(distance), node.offsets.r[0], node.offsets.r[1]}};
                    for (std::size_t length = covered + 1; length <= available; ++length) {
                        relax(i + length, node.cost + prices.match(slot, length) + extra, length, distance, -1, after);
                    }
                }
                covered = std::max(covered, available);
                longest = std::max(longest, available);
            }

            // A long match is taken outright: the positions it covers are
            // indexed but not explored, which keeps runs of repeated data fast.
            if (longest >= kNiceLength) {
                for (std::size_t k = 1; k < longest; ++k) {
                    insert(pos + k);
                }
                i += longest - 1;
            }
        }

        std::vector<std::size_t> steps;
        for (std::size_t i = n; i > 0; i -= nodes[i].length) {
            steps.push_back(i);
        }
        std::vector<Token> tokens;
        tokens.reserve(steps.size());
        std::size_t pos = start;
        for (auto it = steps.rbegin(); it != steps.rend(); ++it) {
            const Node& node = nodes[*it];
            if (node.distance == 0) {
                tokens.push_back({m_input[pos], kNoLength, 0, 0});
            } else {
                tokens.push_back(matchToken(node.length, node.distance, node.repeat, offsets));
            }
            pos += node.length;
        }
        return tokens;
    }

    std::vector<std::uint8_t> encodeFrame(std::size_t start, std::size_t end, bool firstChunk)
    {
        RepeatOffsets offsets = m_offsets;
        const std::vector<Token> tokens = parse(start, end, offsets);
        const std::size_t length = end - start;

        std::vector<std::uint32_t> mainFrequencies(kMainSymbols, 0);
        std::vector<std::uint32_t> lengthFrequencies(kLengthSymbols, 0);
        for (const Token& token : tokens) {
            ++mainFrequencies[token.mainSymbol];
            if (token.lengthSymbol != kNoLength) {
                ++lengthFrequencies[token.lengthSymbol];
            }
        }
        const Code main = Code::build(mainFrequencies, kMaxCodeLength);
        const Code lengths = Code::build(lengthFrequencies, kMaxCodeLength);

        // Aligned blocks code the low three bits of long offsets with their
        // own tree, which pays off when match distances share alignment, as
        // in block-compressed textures.
        std::vector<std::uint32_t> alignedFrequencies(kAlignedSymbols, 0);
        for (const Token& token : tokens) {
            if (token.extraBits >= 3) {
                ++alignedFrequencies[token.extra & 7];
            }
        }
        const Code aligned = Code::build(alignedFrequencies, kMaxAlignedLength);

        const auto writeBlock = [&](bool useAligned) {
            BitWriter out;
            if (firstChunk) {
                // No E8 call translation.
                out.put(0, 1);
            }
            out.put(useAligned ? kBlockAligned : kBlockVerbatim, 3);
            out.put(static_cast<std::uint32_t>(length >> 8), 16);
            out.put(static_cast<std::uint32_t>(length & 0xFF), 8);
            if (useAligned) {
                for (int i = 0; i < kAlignedSymbols; ++i) {
                    out.put(aligned.lengths[static_cast<std::size_t>(i)], 3);
                }
            }
            writeLengths(out, main.lengths, m_mainLengths, 0, kNumChars);
            writeLengths(out, main.lengths, m_mainLengths, kNumChars, kMainSymbols);
            writeLengths(out, lengths.lengths, m_lengthLengths, 0, kLengthSymbols);
            for (const Token& token : tokens) {
                main.write(out, token.mainSymbol);
                if (token.lengthSymbol != kNoLength) {
                    lengths.write(out, token.lengthSymbol);
                }
                if (useAligned && token.extraBits >= 3) {
                    if (token.extraBits > 3) {
                        out.put(token.extra >> 3, token.extraBits - 3);
                    }
                    aligned.write(out, token.extra & 7);
                } else if (token.extraBits != 0) {
                    out.put(token.extra, token.extraBits);
                }
            }
            out.alignWord();
            return out;
        };
        BitWriter verbatim = writeBlock(false);
        if (std::any_of(alignedFrequencies.begin(), alignedFrequencies.end(), [](std::uint32_t f) { return f != 0; })) {
            BitWriter alignedBlock = writeBlock(true);
            if (alignedBlock.size() < verbatim.size()) {
                verbatim = std::move(alignedBlock);
            }
        }

        // Header, at most 16 padding bits, three repeat offsets, raw data.
        const std::size_t uncompressedSize = (firstChunk ? 1u : 0u) + 4 + 2 + 12 + length;
        if (verbatim.size() <= uncompressedSize) {
            m_offsets = offsets;
            m_mainLengths = main.lengths;
            m_lengthLengths = lengths.lengths;
            return verbatim.bytes();
        }

        // Incompressible frame: store it, keeping the repeat offsets and
        // code lengths the next block builds on.
        BitWriter raw;
        if (firstChunk) {
            raw.put(0, 1);
        }
        raw.put(kBlockUncompressed, 3);
        raw.put(static_cast<std::uint32_t>(length >> 8), 16);
        raw.put(static_cast<std::uint32_t>(length & 0xFF), 8);
        raw.padForRawBytes();
        for (const std::uint32_t offset : m_offsets.r) {
            raw.rawU32le(offset);
        }
        raw.rawBytes(m_input.data() + start, length);
        return raw.bytes();
    }

    std::span<const std::uint8_t> m_input;
    std::vector<std::uint32_t> m_head;
    std::vector<std::uint32_t> m_previous;
    RepeatOffsets m_offsets;
    std::vector<std::uint8_t> m_mainLengths;
    std::vector<std::uint8_t> m_lengthLengths;
};

} // namespace

std::vector<std::uint8_t> compress(std::span<const std::uint8_t> input)
{
    if (input.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw Error("input is larger than 4 GiB");
    }
    return Encoder(input).run();
}

} // namespace lzx
