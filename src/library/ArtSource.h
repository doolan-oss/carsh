// ArtSource — find the album cover sitting alongside a track and turn it into the
// ASCII art the Now Playing screen shows. Sidecar-only for now (cover.jpg /
// folder.png / front.* / albumart.* / any image in the track's folder); embedded-
// tag art (APIC / FLAC PICTURE / MP4 covr) is a later enhancement — the tag reader
// only sets a hasEmbeddedArt flag today, it doesn't extract the bytes.
//
// Pure logic over the Vfs + the existing Art pipeline (decodeImage -> generateAscii),
// so it's host-tested with HostVfs + a synthesized cover image.
#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "hal/Vfs.h"
#include "library/Art.h"

namespace carsh::library {

namespace detail {
inline std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
inline bool endsWith(const std::string& s, const char* suf) {
    const std::string x(suf);
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}
inline bool startsWith(const std::string& s, const char* pre) {
    const std::string x(pre);
    return s.size() >= x.size() && s.compare(0, x.size(), x) == 0;
}
// Preference rank for a candidate filename (higher = better); -1 = not an image.
inline int artRank(const std::string& nameLower) {
    if (!(endsWith(nameLower, ".jpg") || endsWith(nameLower, ".jpeg") ||
          endsWith(nameLower, ".png") || endsWith(nameLower, ".bmp")))
        return -1;
    if (startsWith(nameLower, "cover")) return 4;
    if (startsWith(nameLower, "folder")) return 3;
    if (startsWith(nameLower, "front")) return 2;
    if (startsWith(nameLower, "albumart")) return 1;
    return 0;  // some other image in the folder
}
}  // namespace detail

// Find the best cover image in the folder containing `trackPath`, decode it, and
// render AsciiArt into `out`. Returns false if the folder has no usable image.
inline bool loadSidecarArt(carsh::hal::Vfs& vfs, const std::string& trackPath,
                           AsciiArt& out, int cols = 22, int rows = 11) {
    const auto slash = trackPath.find_last_of('/');
    std::string folder = slash == std::string::npos ? std::string()
                                                    : trackPath.substr(0, slash);
    if (folder.empty()) folder = "/";

    std::vector<carsh::hal::DirEntry> entries;
    if (!vfs.readdir(folder, entries)) return false;

    int bestRank = -1;
    std::string bestName;
    for (const auto& e : entries) {
        if (e.isDir) continue;
        const int r = detail::artRank(detail::toLower(e.name));
        if (r > bestRank) { bestRank = r; bestName = e.name; }
    }
    if (bestRank < 0) return false;

    const std::string path =
        (folder == "/" ? "/" : folder + "/") + bestName;
    auto stream = vfs.open(path);
    if (!stream) return false;

    std::vector<uint8_t> bytes(static_cast<size_t>(stream->size()));
    if (!bytes.empty()) {
        size_t got = stream->read(bytes.data(), bytes.size());
        bytes.resize(got);
    }
    if (bytes.empty()) return false;

    Image img;
    if (!decodeImage(bytes.data(), bytes.size(), img) || !img.valid()) return false;
    out = generateAscii(img, cols, rows);
    return true;
}

}  // namespace carsh::library
