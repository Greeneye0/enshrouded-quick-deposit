// Which of the game's ten player voices the local character uses, read from its save.
//
// characterN.ksd (Steam\userdata\<account>\1203620\remote) holds zstd frames; the second one
// starts with CharacterSetup: gender (u8), then voicePackId (u32, FNV-1a of the
// VoicePackResource GUID's bytes). The game saves the character it is playing, so the newest
// file is the current character. Only the first five bytes are needed and a frame always
// begins with literals, so this decodes just the start of the first block's literals (raw,
// RLE or Huffman with FSE-coded weights) instead of carrying a whole zstd decoder.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace voicepack {

// From Town Portal (src/portal_voicepack.h); keep the two in step.
// The game's voice packs, in this order; -1 when unknown.
inline constexpr const char* kNames[] = {"f1", "f2", "f3", "f4", "f5", "m1", "m2", "m3", "m4", "m5"};
inline constexpr uint32_t kIds[] = { // FNV-1a of each VoicePackResource GUID (bytes_le)
    0xb43d9c48u, // f1 71d0d268-8979-4846-884b-a6639b363677
    0xcb617abcu, // f2 a988c073-de0f-43ca-b46b-d1fa27d52978
    0x69b41b80u, // f3 7df689aa-916a-4bd4-8f0e-44faa55b5991
    0x28c5c26cu, // f4 aacc3a58-1975-4ef8-96cd-5b484112ec74
    0x0e2f2ea6u, // f5 1d37f1e3-9ad3-4782-b3aa-0ef897a96694
    0xce9ea7a3u, // m1 359c3f56-00ac-4f44-bea8-7d548999b08a
    0xbd71b290u, // m2 0d547f37-1384-4322-ab54-56972e01fdae
    0xd64695bdu, // m3 14eb59ea-afd4-408c-8ae7-bb0e736c03d7
    0xd65cb555u, // m4 bbb90dea-746a-42c0-b436-772e5dc7bb67
    0xbedb135fu, // m5 796f0ccb-5cac-4f04-9ca9-5bbea08865fd
};

// Female packs are f1..f5, male m1..m5.
inline bool Female(int index) { return index >= 0 && index < int(std::size(kNames)) && kNames[index][0] == 'f'; }
inline int VoiceIndex(uint32_t id) {
    for (int i = 0; i < int(std::size(kIds)); ++i)
        if (kIds[i] == id && id) return i;
    return -1;
}

namespace detail {

inline int HighBit(uint32_t v) {
    int n = -1;
    while (v) { v >>= 1; ++n; }
    return n;
}

// Reads a zstd backward bit stream: from the last byte's top set bit down to bit 0.
struct BackBits {
    const uint8_t* data{};
    int64_t pos{}; // bits left
    bool Init(const uint8_t* p, size_t size) {
        if (!size || !p[size - 1]) return false;
        data = p;
        pos = int64_t(size) * 8 - 8 + HighBit(p[size - 1]);
        return true;
    }
    uint32_t Bit(int64_t i) const { return i < 0 ? 0 : (data[i >> 3] >> (i & 7)) & 1; }
    uint32_t Peek(int n) const {
        uint32_t v = 0;
        for (int k = 0; k < n; ++k) v = (v << 1) | Bit(pos - 1 - k);
        return v;
    }
    uint32_t Read(int n) { const uint32_t v = Peek(n); pos -= n; return v; }
};

// The Huffman weights, FSE-coded (RFC 8878 4.2.1.2 and 4.1.1). Returns the bytes used.
inline size_t FseWeights(const uint8_t* p, size_t size, std::vector<uint8_t>& weights) {
    if (size < 1) return 0;
    // FSE table description, read forward.
    size_t bit = 0;
    auto forward = [&](int n) {
        uint32_t v = 0;
        for (int k = 0; k < n; ++k, ++bit)
            if ((bit >> 3) < size) v |= uint32_t((p[bit >> 3] >> (bit & 7)) & 1) << k;
        return v;
    };
    const int log = int(forward(4)) + 5;
    if (log > 6) return 0;
    int remaining = (1 << log) + 1, threshold = 1 << log, bits = log + 1, symbol = 0;
    int16_t norm[256]{};
    bool previousZero = false;
    while (remaining > 1 && symbol < 256) {
        if (previousZero) {
            for (;;) {
                const uint32_t repeat = forward(2);
                symbol += int(repeat);
                if (repeat != 3) break;
            }
            if (symbol >= 256) return 0;
        }
        const int max = 2 * threshold - 1 - remaining;
        int count;
        const uint32_t low = forward(bits - 1);
        if (int(low) < max) {
            count = int(low);
        } else {
            count = int(low | (forward(1) << (bits - 1)));
            if (count >= threshold) count -= max;
        }
        --count;
        remaining -= count < 0 ? -count : count;
        norm[symbol++] = int16_t(count);
        previousZero = count == 0;
        while (remaining < threshold) { --bits; threshold >>= 1; }
    }
    if (remaining != 1) return 0;
    const size_t header = (bit + 7) / 8;
    if (header >= size) return 0;
    // Decoding table.
    const int tableSize = 1 << log;
    struct Entry { uint8_t symbol, bits; uint16_t base; } table[64]{};
    uint16_t next[256]{};
    int high = tableSize - 1;
    for (int s = 0; s < symbol; ++s) {
        if (norm[s] == -1) { table[high--].symbol = uint8_t(s); next[s] = 1; }
        else next[s] = uint16_t(std::max<int>(norm[s], 0));
    }
    const int step = (tableSize >> 1) + (tableSize >> 3) + 3, mask = tableSize - 1;
    int position = 0;
    for (int s = 0; s < symbol; ++s)
        for (int i = 0; i < norm[s]; ++i) {
            table[position].symbol = uint8_t(s);
            do position = (position + step) & mask; while (position > high);
        }
    if (position) return 0;
    for (int u = 0; u < tableSize; ++u) {
        const uint16_t n = next[table[u].symbol]++;
        table[u].bits = uint8_t(log - HighBit(n));
        table[u].base = uint16_t((n << table[u].bits) - tableSize);
    }
    // Two interleaved states over the backward stream.
    BackBits in;
    if (!in.Init(p + header, size - header)) return 0;
    uint32_t state[2] = {in.Read(log), in.Read(log)};
    weights.clear();
    for (int which = 0; weights.size() < 255; which ^= 1) {
        const Entry& e = table[state[which]];
        weights.push_back(e.symbol);
        state[which] = e.base + in.Read(e.bits);
        if (in.pos < 0) { weights.push_back(table[state[which ^ 1]].symbol); break; }
    }
    return size;
}

} // namespace detail

// The first `want` bytes a zstd frame decompresses to, from its first block's literals.
inline bool FrameStart(const uint8_t* p, size_t size, uint8_t* out, size_t want) {
    using namespace detail;
    if (size < 6 || std::memcmp(p, "\x28\xb5\x2f\xfd", 4)) return false;
    const uint8_t fhd = p[4];
    const bool single = (fhd >> 5) & 1;
    static constexpr size_t kDictionary[] = {0, 1, 2, 4}, kContentSize[] = {0, 2, 4, 8};
    size_t at = 5 + (single ? 0 : 1) + kDictionary[fhd & 3] + ((fhd >> 6) ? kContentSize[fhd >> 6] : single ? 1 : 0);
    if (at + 3 > size) return false;
    const uint32_t block = p[at] | p[at + 1] << 8 | p[at + 2] << 16;
    const uint32_t type = (block >> 1) & 3, blockSize = block >> 3;
    at += 3;
    if (type == 0) { // raw block
        if (blockSize < want || at + want > size) return false;
        std::memcpy(out, p + at, want);
        return true;
    }
    if (type == 1) { // RLE block
        if (blockSize < want || at >= size) return false;
        std::memset(out, p[at], want);
        return true;
    }
    if (type != 2 || at + 5 > size) return false;
    const uint8_t* lit = p + at;
    const int litType = lit[0] & 3, format = (lit[0] >> 2) & 3;
    if (litType == 0 || litType == 1) {
        size_t header, regenerated;
        if (!(format & 1)) { header = 1; regenerated = lit[0] >> 3; }
        else if (format == 1) { header = 2; regenerated = (lit[0] | lit[1] << 8) >> 4; }
        else { header = 3; regenerated = (lit[0] | lit[1] << 8 | lit[2] << 16) >> 4; }
        if (regenerated < want || at + header + (litType ? 1 : want) > size) return false;
        if (litType == 0) std::memcpy(out, lit + header, want);
        else std::memset(out, lit[header], want);
        return true;
    }
    if (litType != 2) return false; // treeless literals never start a frame
    uint64_t h = 0;
    for (int i = 0; i < 5; ++i) h |= uint64_t(lit[i]) << (8 * i);
    size_t header, regenerated, compressed;
    bool four = format != 0;
    if (format <= 1) { header = 3; regenerated = (h >> 4) & 0x3ff; compressed = (h >> 14) & 0x3ff; }
    else if (format == 2) { header = 4; regenerated = (h >> 4) & 0x3fff; compressed = (h >> 18) & 0x3fff; }
    else { header = 5; regenerated = (h >> 4) & 0x3ffff; compressed = (h >> 22) & 0x3ffff; }
    if (at + header + compressed > size) return false;
    const uint8_t* q = lit + header;
    // Huffman tree description: the weights, then the last one implied.
    std::vector<uint8_t> weights;
    size_t used;
    if (q[0] < 128) {
        used = 1 + detail::FseWeights(q + 1, q[0], weights);
        if (used == 1) return false;
    } else {
        const size_t count = q[0] - 127;
        used = 1 + (count + 1) / 2;
        for (size_t i = 0; i < count; ++i) weights.push_back(i & 1 ? q[1 + i / 2] & 15 : q[1 + i / 2] >> 4);
    }
    if (used > compressed || weights.empty() || weights.size() > 255) return false;
    uint32_t total = 0;
    for (uint8_t w : weights) {
        if (w > 11) return false;
        if (w) total += 1u << (w - 1);
    }
    if (!total) return false;
    const int maxBits = HighBit(total) + 1;
    const uint32_t rest = (1u << maxBits) - total;
    if (rest & (rest - 1)) return false;
    weights.push_back(uint8_t(HighBit(rest) + 1));
    // Decoding table: lowest weights (longest codes) first, then by symbol.
    struct Entry { uint8_t symbol, bits; };
    std::vector<Entry> table(size_t(1) << maxBits);
    uint32_t start[13]{}, rank[13]{};
    for (uint8_t w : weights) if (w) ++rank[w];
    for (uint32_t w = 1, next = 0; w <= uint32_t(maxBits); ++w) { start[w] = next; next += rank[w] << (w - 1); }
    for (size_t s = 0; s < weights.size(); ++s) {
        const uint8_t w = weights[s];
        if (!w) continue;
        const uint32_t length = 1u << (w - 1);
        for (uint32_t i = 0; i < length; ++i) table[start[w] + i] = {uint8_t(s), uint8_t(maxBits + 1 - w)};
        start[w] += length;
    }
    // The first stream holds the first literals.
    const uint8_t* stream = q + used;
    size_t streamSize = compressed - used, perStream = regenerated;
    if (four) {
        if (streamSize < 6) return false;
        const size_t first = stream[0] | stream[1] << 8;
        stream += 6;
        if (first > streamSize - 6) return false;
        streamSize = first;
        perStream = (regenerated + 3) / 4;
    }
    if (perStream < want) return false;
    BackBits in;
    if (!in.Init(stream, streamSize)) return false;
    for (size_t i = 0; i < want; ++i) {
        const Entry e = table[in.Peek(maxBits)];
        out[i] = e.symbol;
        in.pos -= e.bits;
    }
    return in.pos >= 0;
}

// The voice pack id in a characterN.ksd file's contents, 0 when it cannot be read.
inline uint32_t FromSave(const uint8_t* p, size_t size) {
    int seen = 0;
    for (size_t i = 0; i + 4 <= size; ++i) {
        if (std::memcmp(p + i, "\x28\xb5\x2f\xfd", 4)) continue;
        if (++seen < 2) continue;
        uint8_t setup[5];
        if (!FrameStart(p + i, size - i, setup, sizeof(setup))) return 0;
        return uint32_t(setup[1]) | uint32_t(setup[2]) << 8 | uint32_t(setup[3]) << 16 | uint32_t(setup[4]) << 24;
    }
    return 0;
}

} // namespace voicepack
