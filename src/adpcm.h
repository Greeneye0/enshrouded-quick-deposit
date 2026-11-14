// From Town Portal (src/portal_adpcm.h). Voice files: mono WAV, IMA ADPCM (format 0x11, what tools/build-portal-voice.py writes) or
// 16-bit PCM, turned into the plain 44-byte-header 16-bit PCM WAV that PlayScaled plays.
//
// Microsoft IMA ADPCM: blocks of blockAlign bytes, each a 4-byte header (first sample, step index)
// and then two 4-bit codes a byte, low nibble first. The fact chunk holds the real sample count;
// the last block is padded past it.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace adpcm {

namespace detail {
inline constexpr int16_t kSteps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80,
    88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
inline constexpr int8_t kIndexStep[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

inline uint32_t U32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint16_t U16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

inline void PutPcmHeader(std::vector<uint8_t>& out, uint32_t rate, uint32_t samples) {
    const uint32_t bytes = samples * 2, riff = 36 + bytes, byteRate = rate * 2;
    const uint16_t pcm = 1, mono = 1, align = 2, bits = 16;
    const uint32_t fmtSize = 16;
    out.resize(44);
    uint8_t* p = out.data();
    std::memcpy(p, "RIFF", 4); std::memcpy(p + 4, &riff, 4); std::memcpy(p + 8, "WAVEfmt ", 8);
    std::memcpy(p + 16, &fmtSize, 4); std::memcpy(p + 20, &pcm, 2); std::memcpy(p + 22, &mono, 2);
    std::memcpy(p + 24, &rate, 4); std::memcpy(p + 28, &byteRate, 4); std::memcpy(p + 32, &align, 2);
    std::memcpy(p + 34, &bits, 2); std::memcpy(p + 36, "data", 4); std::memcpy(p + 40, &bytes, 4);
}
} // namespace detail

// Empty when the file is not a mono 16-bit PCM or IMA ADPCM WAV.
inline std::vector<uint8_t> ToPcm(const uint8_t* p, size_t size) {
    using namespace detail;
    std::vector<uint8_t> out;
    if (size < 12 || std::memcmp(p, "RIFF", 4) || std::memcmp(p + 8, "WAVE", 4)) return out;
    const uint8_t *fmt = nullptr, *data = nullptr;
    uint32_t fmtSize = 0, dataSize = 0, factSamples = 0;
    bool fact = false;
    for (size_t at = 12; at + 8 <= size;) {
        const uint32_t chunk = U32(p + at + 4);
        if (chunk > size - at - 8) break;
        const uint8_t* body = p + at + 8;
        if (!std::memcmp(p + at, "fmt ", 4)) fmt = body, fmtSize = chunk;
        else if (!std::memcmp(p + at, "data", 4)) data = body, dataSize = chunk;
        else if (!std::memcmp(p + at, "fact", 4) && chunk >= 4) fact = true, factSamples = U32(body);
        at += 8 + chunk + (chunk & 1);
    }
    if (!fmt || !data || fmtSize < 16 || U16(fmt + 2) != 1) return out;
    const uint16_t format = U16(fmt), align = U16(fmt + 12), bits = U16(fmt + 14);
    const uint32_t rate = U32(fmt + 4);
    if (format == 1 && bits == 16) {
        PutPcmHeader(out, rate, dataSize / 2);
        out.insert(out.end(), data, data + dataSize / 2 * 2);
        return out;
    }
    if (format != 0x11 || bits != 4 || align < 5) return out;
    const uint32_t perBlock = (align - 4) * 2 + 1;
    uint32_t total = dataSize / align * perBlock;
    if (fact) total = std::min(total, factSamples);
    PutPcmHeader(out, rate, total);
    out.resize(44 + size_t(total) * 2);
    int16_t* pcm = reinterpret_cast<int16_t*>(out.data() + 44);
    uint32_t written = 0;
    for (const uint8_t* block = data; written < total; block += align) {
        int predictor = int16_t(U16(block));
        int index = std::min<int>(block[2], 88);
        pcm[written++] = int16_t(predictor);
        for (uint32_t i = 0; i < perBlock - 1 && written < total; ++i) {
            const int code = (block[4 + i / 2] >> (i & 1 ? 4 : 0)) & 15;
            const int step = kSteps[index];
            int delta = step >> 3;
            if (code & 4) delta += step;
            if (code & 2) delta += step >> 1;
            if (code & 1) delta += step >> 2;
            predictor = std::clamp(code & 8 ? predictor - delta : predictor + delta, -32768, 32767);
            index = std::clamp(index + kIndexStep[code & 7], 0, 88);
            pcm[written++] = int16_t(predictor);
        }
    }
    return out;
}

} // namespace adpcm
