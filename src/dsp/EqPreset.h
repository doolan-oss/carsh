// EQ preset (.eq) serialization (PLAN.md §4.4).
//
// Persists a dsp::EqConfig: header{magic,version,bandCount,preampDb} then each
// band{type,freqHz,gainDb,q}. Little-endian, fixed layout — portable between the
// build server and the Pi (both LE). Used for user presets and the global
// car-correction curve.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "dsp/Eq.h"

namespace carsh::dsp {

class EqPreset {
public:
    static constexpr uint16_t kVersion = 1;

    static std::vector<uint8_t> serialize(const EqConfig& cfg) {
        std::vector<uint8_t> v;
        putBytes(v, "EQP1", 4);
        putU16(v, kVersion);
        v.push_back(uint8_t(cfg.bands.size()));
        putF32(v, float(cfg.preampDb));
        for (const auto& b : cfg.bands) {
            v.push_back(uint8_t(b.type));
            putF32(v, float(b.freqHz));
            putF32(v, float(b.gainDb));
            putF32(v, float(b.q));
        }
        return v;
    }

    static bool deserialize(const uint8_t* d, std::size_t len, EqConfig& out) {
        if (len < 11 || std::memcmp(d, "EQP1", 4) != 0) return false;
        std::size_t p = 4;
        uint16_t ver = uint16_t(d[p] | (d[p + 1] << 8));
        p += 2;
        if (ver != kVersion) return false;
        uint8_t n = d[p++];
        out.preampDb = getF32(d, p);
        p += 4;
        out.bands.clear();
        for (uint8_t i = 0; i < n; ++i) {
            if (p + 13 > len) return false;
            EqBand b;
            b.type = FilterType(d[p++]);
            b.freqHz = getF32(d, p); p += 4;
            b.gainDb = getF32(d, p); p += 4;
            b.q = getF32(d, p); p += 4;
            out.bands.push_back(b);
        }
        return true;
    }

private:
    static void putBytes(std::vector<uint8_t>& v, const char* s, std::size_t n) {
        v.insert(v.end(), s, s + n);
    }
    static void putU16(std::vector<uint8_t>& v, uint16_t x) {
        v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
    }
    static void putF32(std::vector<uint8_t>& v, float f) {
        uint32_t x;
        std::memcpy(&x, &f, 4);
        v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
        v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 24));
    }
    static float getF32(const uint8_t* d, std::size_t p) {
        uint32_t x = uint32_t(d[p]) | (uint32_t(d[p + 1]) << 8) |
                     (uint32_t(d[p + 2]) << 16) | (uint32_t(d[p + 3]) << 24);
        float f;
        std::memcpy(&f, &x, 4);
        return f;
    }
};

}  // namespace carsh::dsp
