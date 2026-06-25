// Driver profile (PLAN.md §2.11, §4.3). Keyed on the phone's Classic BR/EDR
// address (stable for A2DP, unlike BLE RPAs — R10). Carries the per-driver EQ
// curve, theme, last source, and favorites; persisted as <bdaddr>.prof.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dsp/EqPreset.h"

namespace carsh::profile {

struct BdAddr {
    std::array<uint8_t, 6> b{};
    bool operator==(const BdAddr& o) const { return b == o.b; }
    // "AA:BB:CC:DD:EE:FF" for display and <bdaddr>.prof filenames.
    std::string str() const {
        char buf[18];
        std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", b[0],
                      b[1], b[2], b[3], b[4], b[5]);
        return buf;
    }
    std::string key() const {  // compact filename-safe key
        char buf[13];
        std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x", b[0], b[1],
                      b[2], b[3], b[4], b[5]);
        return buf;
    }
};

enum class ThemeVariant : uint8_t { Dark = 0, Light = 1, Amber = 2, Green = 3 };
enum class SourceKind : uint8_t { Bt = 0, Usb = 1, Sd = 2 };

enum ProfileFlags : uint32_t {
    ProfFlagGuest = 1u << 0,
    ProfFlagRgAlbumMode = 1u << 1,
};

struct Profile {
    static constexpr uint16_t kVersion = 1;

    BdAddr addr;
    std::string displayName;
    dsp::EqConfig eq;
    ThemeVariant theme = ThemeVariant::Dark;
    SourceKind lastSource = SourceKind::Usb;
    std::vector<uint64_t> favorites;
    uint32_t flags = 0;

    bool isGuest() const { return flags & ProfFlagGuest; }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> v;
        v.insert(v.end(), {'P', 'R', 'O', 'F', '1'});
        putU16(v, kVersion);
        v.insert(v.end(), addr.b.begin(), addr.b.end());
        putStr(v, displayName);
        v.push_back(uint8_t(theme));
        v.push_back(uint8_t(lastSource));
        putU32(v, flags);
        putU32(v, uint32_t(favorites.size()));
        for (uint64_t f : favorites) putU64(v, f);
        auto eqBlob = dsp::EqPreset::serialize(eq);
        putU32(v, uint32_t(eqBlob.size()));
        v.insert(v.end(), eqBlob.begin(), eqBlob.end());
        return v;
    }

    static bool deserialize(const uint8_t* d, std::size_t len, Profile& out) {
        if (len < 7 || std::memcmp(d, "PROF1", 5) != 0) return false;
        std::size_t p = 5;
        uint16_t ver = rdU16(d, p); p += 2;
        if (ver != kVersion) return false;
        if (p + 6 > len) return false;
        std::memcpy(out.addr.b.data(), d + p, 6); p += 6;
        if (!rdStr(d, len, p, out.displayName)) return false;
        if (p + 2 > len) return false;
        out.theme = ThemeVariant(d[p++]);
        out.lastSource = SourceKind(d[p++]);
        out.flags = rdU32(d, p); p += 4;
        uint32_t favN = rdU32(d, p); p += 4;
        out.favorites.clear();
        for (uint32_t i = 0; i < favN; ++i) {
            if (p + 8 > len) return false;
            out.favorites.push_back(rdU64(d, p)); p += 8;
        }
        uint32_t eqLen = rdU32(d, p); p += 4;
        if (p + eqLen > len) return false;
        if (eqLen && !dsp::EqPreset::deserialize(d + p, eqLen, out.eq))
            return false;
        return true;
    }

private:
    static void putU16(std::vector<uint8_t>& v, uint16_t x) {
        v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
    }
    static void putU32(std::vector<uint8_t>& v, uint32_t x) {
        for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
    }
    static void putU64(std::vector<uint8_t>& v, uint64_t x) {
        for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
    }
    static void putStr(std::vector<uint8_t>& v, const std::string& s) {
        putU16(v, uint16_t(s.size()));
        v.insert(v.end(), s.begin(), s.end());
    }
    static uint16_t rdU16(const uint8_t* d, std::size_t p) {
        return uint16_t(d[p] | (d[p + 1] << 8));
    }
    static uint32_t rdU32(const uint8_t* d, std::size_t p) {
        return uint32_t(d[p]) | (uint32_t(d[p + 1]) << 8) |
               (uint32_t(d[p + 2]) << 16) | (uint32_t(d[p + 3]) << 24);
    }
    static uint64_t rdU64(const uint8_t* d, std::size_t p) {
        uint64_t x = 0;
        for (int i = 0; i < 8; ++i) x |= uint64_t(d[p + i]) << (8 * i);
        return x;
    }
    static bool rdStr(const uint8_t* d, std::size_t len, std::size_t& p,
                      std::string& out) {
        if (p + 2 > len) return false;
        uint16_t n = rdU16(d, p); p += 2;
        if (p + n > len) return false;
        out.assign(reinterpret_cast<const char*>(d + p), n); p += n;
        return true;
    }
};

}  // namespace carsh::profile
