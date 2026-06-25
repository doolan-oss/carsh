// OTA manifest (PLAN.md §8.5). manifest.json is a flat object:
//   {"version":"1.2.0","buildId":"...","slotImageSHA256":"<64 hex>",
//    "minPrevVersion":"1.0.0"}
// The Ed25519 signature covers the exact manifest bytes; the manifest pins the
// image SHA-256, so verifying the signature + hashing the image is sufficient.
//
// A tiny restricted JSON reader (flat string/number fields only) keeps the
// dependency surface bare-metal-friendly. It is NOT a general JSON parser.
#pragma once

#include <cstdint>
#include <string>

#include "ota/Crypto.h"

namespace carsh::ota {

struct Manifest {
    std::string version;
    std::string buildId;
    std::string minPrevVersion;
    Sha256Digest imageSha{};
    bool valid = false;
};

// "1.2.3" -> 0x010203 for ordered comparison (components clamped to 0..255).
inline uint32_t parseVersion(const std::string& v) {
    uint32_t parts[3] = {0, 0, 0};
    int idx = 0;
    uint32_t cur = 0;
    bool any = false;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + uint32_t(c - '0');
            any = true;
        } else if (c == '.') {
            if (idx < 3) parts[idx] = cur > 255 ? 255 : cur;
            ++idx;
            cur = 0;
        }
    }
    if (idx < 3 && any) parts[idx] = cur > 255 ? 255 : cur;
    return (parts[0] << 16) | (parts[1] << 8) | parts[2];
}

namespace detail {
// Find the value token for "key" in a flat JSON object. Returns false if absent.
inline bool findValue(const std::string& js, const std::string& key,
                      std::string& out, bool& isString) {
    std::string pat = "\"" + key + "\"";
    auto k = js.find(pat);
    if (k == std::string::npos) return false;
    auto colon = js.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    std::size_t i = colon + 1;
    while (i < js.size() && (js[i] == ' ' || js[i] == '\t' || js[i] == '\n' ||
                             js[i] == '\r'))
        ++i;
    if (i >= js.size()) return false;
    if (js[i] == '"') {
        isString = true;
        std::size_t start = ++i;
        std::string s;
        while (i < js.size() && js[i] != '"') {
            if (js[i] == '\\' && i + 1 < js.size()) ++i;  // unescape next char
            s.push_back(js[i++]);
        }
        out = s;
        (void)start;
        return i < js.size();
    }
    isString = false;
    std::size_t start = i;
    while (i < js.size() && js[i] != ',' && js[i] != '}' && js[i] != ' ' &&
           js[i] != '\n' && js[i] != '\r' && js[i] != '\t')
        ++i;
    out = js.substr(start, i - start);
    return true;
}

inline int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace detail

inline bool hexToSha256(const std::string& hex, Sha256Digest& out) {
    if (hex.size() != 64) return false;
    for (int i = 0; i < 32; ++i) {
        int hi = detail::hexNibble(hex[2 * i]);
        int lo = detail::hexNibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = uint8_t((hi << 4) | lo);
    }
    return true;
}

inline Manifest parseManifest(const uint8_t* data, std::size_t len) {
    Manifest m;
    std::string js(reinterpret_cast<const char*>(data), len);
    std::string val;
    bool isStr;
    if (!detail::findValue(js, "version", m.version, isStr)) return m;
    detail::findValue(js, "buildId", m.buildId, isStr);
    detail::findValue(js, "minPrevVersion", m.minPrevVersion, isStr);
    std::string sha;
    if (!detail::findValue(js, "slotImageSHA256", sha, isStr)) return m;
    if (!hexToSha256(sha, m.imageSha)) return m;
    m.valid = true;
    return m;
}

}  // namespace carsh::ota
