// LibraryCache — persist the scanned library DB so boot can skip the (slow) tag
// scan when storage hasn't changed (PLAN.md R7: faster boot, fewer SD writes).
//
// The expensive part of indexing is opening + tag-parsing every file. This caches
// the serialized Database and a cheap "signature" of the media tree; on boot we
// recompute the signature (a readdir-only walk — no file opens) and, if it matches,
// deserialize the DB and skip scanning entirely. Platform-independent + host-tested;
// the device write path is CircleVfs::writeFile (FatFs), the host's is HostVfs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "decode/DecoderFactory.h"  // codecFromExtension
#include "decode/IDecoder.h"        // Codec
#include "hal/Vfs.h"
#include "library/Database.h"
#include "library/Model.h"          // fnv1a

namespace carsh::library {

// Order-independent signature of the audio files under `root`: folds each audio
// file's (path|size|mtime) so add/remove/replace changes it, but a readdir-order
// change does not. Skips non-audio files (so the cache blobs + cover art don't
// invalidate it) and hidden entries (mirrors the Indexer walk). Opens no files.
inline uint64_t librarySignature(carsh::hal::Vfs& vfs, const std::string& root) {
    uint64_t acc = 1469598103934665603ull;  // fnv offset basis; combined via XOR
    uint64_t count = 0;
    std::vector<std::string> dirs = {root};
    while (!dirs.empty()) {
        std::string dir = dirs.back();
        dirs.pop_back();
        std::vector<carsh::hal::DirEntry> entries;
        if (!vfs.readdir(dir, entries)) continue;
        for (const auto& e : entries) {
            if (!e.name.empty() && e.name.front() == '.') continue;  // hidden
            std::string child = dir;
            if (child.empty() || child.back() != '/') child += '/';
            child += e.name;
            if (e.isDir) { dirs.push_back(child); continue; }
            if (carsh::decode::codecFromExtension(e.name) ==
                carsh::decode::Codec::Unknown)
                continue;
            const std::string key = child + "|" + std::to_string(e.size) + "|" +
                                    std::to_string(e.mtime);
            acc ^= fnv1a(key);
            ++count;
        }
    }
    return acc ^ (count * 1099511628211ull);
}

namespace detail {
inline std::vector<uint8_t> readAll(carsh::hal::Vfs& vfs, const std::string& path) {
    auto s = vfs.open(path);
    if (!s) return {};
    std::vector<uint8_t> b(static_cast<size_t>(s->size()));
    if (!b.empty()) { size_t g = s->read(b.data(), b.size()); b.resize(g); }
    return b;
}
inline std::vector<uint8_t> u64le(uint64_t v) {
    std::vector<uint8_t> o(8);
    for (int i = 0; i < 8; ++i) o[i] = uint8_t(v >> (8 * i));
    return o;
}
inline bool readU64le(const std::vector<uint8_t>& b, uint64_t& v) {
    if (b.size() < 8) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(b[i]) << (8 * i);
    return true;
}
}  // namespace detail

// Load `db` from cachePath iff sigPath holds exactly `sig`. Returns true on a hit
// (db is populated + indexed via deserialize); false means "rescan needed".
inline bool tryLoadLibrary(carsh::hal::Vfs& vfs, const std::string& cachePath,
                           const std::string& sigPath, uint64_t sig, Database& db) {
    uint64_t cached = 0;
    if (!detail::readU64le(detail::readAll(vfs, sigPath), cached) || cached != sig)
        return false;
    const std::vector<uint8_t> blob = detail::readAll(vfs, cachePath);
    if (blob.empty()) return false;
    return db.deserialize(blob);
}

// Save `db` + `sig` to the cache. Returns true only if both files were written.
inline bool saveLibrary(carsh::hal::Vfs& vfs, const std::string& cachePath,
                        const std::string& sigPath, uint64_t sig, const Database& db) {
    const std::vector<uint8_t> blob = db.serialize();
    if (!vfs.writeFile(cachePath, blob.data(), blob.size())) return false;
    const std::vector<uint8_t> s = detail::u64le(sig);
    return vfs.writeFile(sigPath, s.data(), s.size());
}

}  // namespace carsh::library
