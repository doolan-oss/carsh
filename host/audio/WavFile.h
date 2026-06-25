// Minimal RIFF/WAVE reader + writer for host tests and the WAV sink.
//
// Supports canonical PCM (16/24/32-bit int) and IEEE float32. This is enough to
// (a) write the engine's output for golden-diffing and (b) read small fixtures.
// It is deliberately independent of dr_wav so the sink has no decoder
// dependency; dr_wav is used for the *decoder* path under test.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace carsh::host {

struct WavData {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;       // 16/24/32
    bool isFloat = false;
    std::vector<float> samples;  // interleaved, normalized to [-1,1]
};

class WavWriter {
public:
    // bits: 16/24/32 for int PCM, or 32 with asFloat=true for IEEE float.
    static bool write(const std::string& path, const float* interleaved,
                      size_t frames, uint16_t channels, uint32_t sampleRate,
                      uint16_t bits = 16, bool asFloat = false) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const uint16_t fmtTag = asFloat ? 3 : 1;  // 3 = IEEE float
        const uint16_t bytesPerSample = bits / 8;
        const uint32_t dataBytes =
            uint32_t(frames) * channels * bytesPerSample;
        const uint32_t blockAlign = channels * bytesPerSample;
        const uint32_t byteRate = sampleRate * blockAlign;

        putTag(f, "RIFF");
        putU32(f, 36 + dataBytes);
        putTag(f, "WAVE");
        putTag(f, "fmt ");
        putU32(f, 16);
        putU16(f, fmtTag);
        putU16(f, channels);
        putU32(f, sampleRate);
        putU32(f, byteRate);
        putU16(f, uint16_t(blockAlign));
        putU16(f, bits);
        putTag(f, "data");
        putU32(f, dataBytes);

        const size_t n = frames * channels;
        for (size_t i = 0; i < n; ++i)
            writeSample(f, interleaved[i], bits, asFloat);
        std::fclose(f);
        return true;
    }

    static bool write(const std::string& path, const WavData& w) {
        return write(path, w.samples.data(), w.samples.size() / w.channels,
                     w.channels, w.sampleRate, w.bits, w.isFloat);
    }

private:
    static void putTag(FILE* f, const char* t) { std::fwrite(t, 1, 4, f); }
    static void putU32(FILE* f, uint32_t v) {
        uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16),
                        uint8_t(v >> 24)};
        std::fwrite(b, 1, 4, f);
    }
    static void putU16(FILE* f, uint16_t v) {
        uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        std::fwrite(b, 1, 2, f);
    }
    static void writeSample(FILE* f, float s, uint16_t bits, bool asFloat) {
        if (asFloat) {
            std::fwrite(&s, 1, 4, f);
            return;
        }
        if (s > 1.0f) s = 1.0f;
        if (s < -1.0f) s = -1.0f;
        if (bits == 16) {
            int32_t v = int32_t(lrintf(s * 32767.0f));
            uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
            std::fwrite(b, 1, 2, f);
        } else if (bits == 24) {
            int32_t v = int32_t(lrintf(s * 8388607.0f));
            uint8_t b[3] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16)};
            std::fwrite(b, 1, 3, f);
        } else {  // 32-bit int
            int32_t v = int32_t(lrintf(s * 2147483647.0));
            uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16),
                            uint8_t(v >> 24)};
            std::fwrite(b, 1, 4, f);
        }
    }
    static long lrintf(float x) { return (long)(x < 0 ? x - 0.5f : x + 0.5f); }
    static long lrintf(double x) { return (long)(x < 0 ? x - 0.5 : x + 0.5); }
};

class WavReader {
public:
    static bool read(const std::string& path, WavData& out) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::vector<uint8_t> buf;
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz < 44) { std::fclose(f); return false; }
        buf.resize(size_t(sz));
        if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
            std::fclose(f);
            return false;
        }
        std::fclose(f);
        if (std::memcmp(buf.data(), "RIFF", 4) ||
            std::memcmp(buf.data() + 8, "WAVE", 4))
            return false;

        size_t p = 12;
        uint16_t fmtTag = 1;
        bool haveFmt = false, haveData = false;
        const uint8_t* data = nullptr;
        uint32_t dataLen = 0;
        while (p + 8 <= buf.size()) {
            char id[5] = {0};
            std::memcpy(id, &buf[p], 4);
            uint32_t len = u32(&buf[p + 4]);
            const uint8_t* body = &buf[p + 8];
            if (!std::memcmp(id, "fmt ", 4) && len >= 16) {
                fmtTag = u16(body);
                out.channels = u16(body + 2);
                out.sampleRate = u32(body + 4);
                out.bits = u16(body + 14);
                out.isFloat = (fmtTag == 3);
                haveFmt = true;
            } else if (!std::memcmp(id, "data", 4)) {
                data = body;
                dataLen = (p + 8 + len <= buf.size())
                              ? len
                              : uint32_t(buf.size() - (p + 8));
                haveData = true;
            }
            p += 8 + len + (len & 1);  // chunks are word-aligned
        }
        if (!haveFmt || !haveData) return false;
        decodeSamples(data, dataLen, out);
        return true;
    }

private:
    static uint16_t u16(const uint8_t* p) {
        return uint16_t(p[0] | (p[1] << 8));
    }
    static uint32_t u32(const uint8_t* p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
               (uint32_t(p[3]) << 24);
    }
    static void decodeSamples(const uint8_t* d, uint32_t len, WavData& out) {
        uint16_t bps = out.bits / 8;
        if (bps == 0) return;
        size_t count = len / bps;
        out.samples.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* s = d + i * bps;
            if (out.isFloat) {
                float v;
                std::memcpy(&v, s, 4);
                out.samples.push_back(v);
            } else if (out.bits == 16) {
                int16_t v = int16_t(s[0] | (s[1] << 8));
                out.samples.push_back(float(v) / 32768.0f);
            } else if (out.bits == 24) {
                int32_t v = (s[0] | (s[1] << 8) | (s[2] << 16));
                if (v & 0x800000) v |= ~0xFFFFFF;  // sign-extend
                out.samples.push_back(float(v) / 8388608.0f);
            } else {  // 32-bit int
                int32_t v = int32_t(u32(s));
                out.samples.push_back(float(v) / 2147483648.0f);
            }
        }
    }
};

}  // namespace carsh::host
