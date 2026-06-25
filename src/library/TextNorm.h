// Text normalization for search + dedup keys (PLAN.md §4.1, §2.9).
//
// Casefold, strip common Latin accents, collapse whitespace, and tokenize on
// non-alphanumeric boundaries. Scope: ASCII + the Latin-1 / common UTF-8
// accented letters that show up in Western music metadata (é, ü, ñ, å, ...).
// Full Unicode folding is out of scope on bare metal; unmapped multibyte
// sequences pass through as separate tokens (documented limitation).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace carsh::library {

// Map one Unicode code point to an ASCII fold (lowercase, accent-stripped).
// Returns 0 to drop the character (treated as a separator). Only common Latin
// ranges are handled; anything else returns the lowercased ASCII or itself.
inline char foldAscii(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return char(cp - 'A' + 'a');
    if (cp >= 'a' && cp <= 'z') return char(cp);
    if (cp >= '0' && cp <= '9') return char(cp);
    // Latin-1 / Latin Extended common accented letters -> base letter.
    switch (cp) {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
        case 0x100: case 0x101: case 0x102: case 0x103: return 'a';
        case 0xC7: case 0xE7: case 0x106: case 0x107: case 0x10C: case 0x10D:
            return 'c';
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB:
        case 0x11A: case 0x11B: return 'e';
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xEC: case 0xED: case 0xEE: case 0xEF: return 'i';
        case 0xD1: case 0xF1: return 'n';
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8:
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8:
            return 'o';
        case 0xD9: case 0xDA: case 0xDB: case 0xDC:
        case 0xF9: case 0xFA: case 0xFB: case 0xFC: return 'u';
        case 0xDD: case 0xFD: case 0xFF: return 'y';
        case 0x160: case 0x161: return 's';
        case 0x17D: case 0x17E: return 'z';
        default: return 0;  // separator / unmapped
    }
}

// Decode a UTF-8 sequence starting at i; advances i past it. Returns the code
// point, or the raw byte for invalid sequences.
inline uint32_t nextCodePoint(const std::string& s, std::size_t& i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { ++i; return c; }
    int extra = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { ++i; return c; }
    if (i + std::size_t(extra) >= s.size() + 1) { ++i; return c; }
    ++i;
    for (int k = 0; k < extra && i < s.size(); ++k, ++i) {
        unsigned char cc = (unsigned char)s[i];
        if ((cc & 0xC0) != 0x80) return cp;  // malformed; stop
        cp = (cp << 6) | (cc & 0x3F);
    }
    return cp;
}

// Normalize to a single casefolded, accent-stripped, single-spaced string.
inline std::string normalize(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool lastSep = true;  // suppress leading spaces
    for (std::size_t i = 0; i < in.size();) {
        uint32_t cp = nextCodePoint(in, i);
        char f = foldAscii(cp);
        if (f == 0) {
            if (!lastSep) { out.push_back(' '); lastSep = true; }
        } else {
            out.push_back(f);
            lastSep = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Tokenize normalized text into words (split on spaces).
inline std::vector<std::string> tokenize(const std::string& in) {
    std::vector<std::string> toks;
    std::string norm = normalize(in);
    std::size_t start = 0;
    for (std::size_t i = 0; i <= norm.size(); ++i) {
        if (i == norm.size() || norm[i] == ' ') {
            if (i > start) toks.emplace_back(norm.substr(start, i - start));
            start = i + 1;
        }
    }
    return toks;
}

}  // namespace carsh::library
