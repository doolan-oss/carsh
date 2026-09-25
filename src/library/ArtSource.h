// ArtSource — find the album cover sitting alongside a track and turn it into the
// ASCII art the Now Playing screen shows. Sidecar-only for now (cover.jpg /
// folder.png / front.* / albumart.* / any image in the track's folder); embedded-
// tag art (APIC / FLAC PICTURE / MP4 covr) is a later enhancement — the tag reader
// only sets a hasEmbeddedArt flag today, it doesn't extract the bytes.
//
// Pure logic over the Vfs + the existing Art pipeline (decodeImageFitted ->
// generateAscii), so it's host-tested with HostVfs + a synthesized cover image.
//
// The cover is streamed (never read into one buffer), probed before decoding, and
// downscaled straight out of the decoder — see Art.h for why that matters on
// Circle's heap (review L1). A candidate that is too large (> kMaxArtDim) or fails
// to decode falls through to the next-best image, so a folder with a huge scan
// as cover.jpg plus a small folder.jpg still shows art.
#pragma once

#include <algorithm>
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

// Most candidate images tried per album before giving up (bounds the work for a
// folder full of oversized scans).
constexpr int kMaxArtCandidates = 4;

// Find the best cover image in the folder containing `trackPath`, decode it, and
// render AsciiArt into `out`. Returns false if the folder has no usable image
// (none, or every tried candidate failed to decode / exceeds maxDim).
inline bool loadSidecarArt(carsh::hal::Vfs& vfs, const std::string& trackPath,
                           AsciiArt& out, int cols = 22, int rows = 11,
                           int maxDim = kMaxArtDim) {
    const auto slash = trackPath.find_last_of('/');
    std::string folder = slash == std::string::npos ? std::string()
                                                    : trackPath.substr(0, slash);
    if (folder.empty()) folder = "/";

    std::vector<carsh::hal::DirEntry> entries;
    if (!vfs.readdir(folder, entries)) return false;

    struct Candidate { int rank; std::string name; };
    std::vector<Candidate> cands;
    for (const auto& e : entries) {
        if (e.isDir) continue;
        const int r = detail::artRank(detail::toLower(e.name));
        if (r >= 0) cands.push_back({r, e.name});
    }
    // Best rank first; ties keep directory order (first listed wins, as before).
    std::stable_sort(cands.begin(), cands.end(),
                     [](const Candidate& a, const Candidate& b) { return a.rank > b.rank; });

    int tried = 0;
    for (const Candidate& c : cands) {
        if (tried++ >= kMaxArtCandidates) break;
        const std::string path = (folder == "/" ? "/" : folder + "/") + c.name;
        auto stream = vfs.open(path);
        if (!stream || stream->size() == 0) continue;
        Image img;
        if (!decodeImageFitted(*stream, img, kArtWorkDim, maxDim)) continue;
        out = generateAscii(img, cols, rows);
        return true;
    }
    return false;
}

}  // namespace carsh::library
