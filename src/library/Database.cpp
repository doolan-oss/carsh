#include "library/Database.h"

#include <algorithm>
#include <cstring>

namespace carsh::library {

uint64_t Database::addTrack(const TrackMeta& meta, uint8_t sourceId,
                            const std::string& path, uint8_t format,
                            const std::string& folderForAlbumKey) {
    Track t;
    t.trackId = makeTrackId(sourceId, path);
    t.titleRef = strings_.intern(meta.title);
    t.artistRef = strings_.intern(meta.artist);
    t.albumRef = strings_.intern(meta.album);
    t.albumArtistRef = strings_.intern(meta.effectiveAlbumArtist());
    t.genreRef = strings_.intern(meta.genre);
    t.pathRef = strings_.intern(path);
    t.durationMs = meta.durationMs;
    t.sampleRate = meta.sampleRate;
    t.bitsPerSample = meta.bitsPerSample;
    t.channels = meta.channels;
    t.sourceId = sourceId;
    t.format = format;
    t.trackNo = meta.trackNo;
    t.discNo = meta.discNo;
    t.year = meta.year;
    t.albumKey = makeAlbumKey(meta.effectiveAlbumArtist(), meta.album,
                              folderForAlbumKey);
    if (meta.hasEmbeddedArt) t.flags |= FlagHasEmbeddedArt;

    auto it = idIndex_.find(t.trackId);
    if (it != idIndex_.end()) {
        // Preserve previously-scanned ReplayGain across a metadata refresh.
        Track& old = tracks_[it->second];
        t.rgTrackGain = old.rgTrackGain; t.rgAlbumGain = old.rgAlbumGain;
        t.rgTrackPeak = old.rgTrackPeak; t.rgAlbumPeak = old.rgAlbumPeak;
        t.flags |= (old.flags & FlagRgScanned);
        old = t;
    } else {
        idIndex_.emplace(t.trackId, tracks_.size());
        tracks_.push_back(t);
    }
    indexed_ = false;
    return t.trackId;
}

void Database::setReplayGain(uint64_t trackId, float trackGainDb,
                             float albumGainDb, float trackPeak,
                             float albumPeak) {
    auto it = idIndex_.find(trackId);
    if (it == idIndex_.end()) return;
    Track& t = tracks_[it->second];
    t.rgTrackGain = trackGainDb;
    t.rgAlbumGain = albumGainDb;
    t.rgTrackPeak = trackPeak;
    t.rgAlbumPeak = albumPeak;
    t.flags |= FlagRgScanned;
}

bool Database::removeTrack(uint64_t trackId) {
    auto it = idIndex_.find(trackId);
    if (it == idIndex_.end()) return false;
    std::size_t idx = it->second;
    std::size_t last = tracks_.size() - 1;
    if (idx != last) {
        tracks_[idx] = tracks_[last];
        idIndex_[tracks_[idx].trackId] = idx;
    }
    tracks_.pop_back();
    idIndex_.erase(it);
    indexed_ = false;
    return true;
}

const Track* Database::get(uint64_t trackId) const {
    auto it = idIndex_.find(trackId);
    return it == idIndex_.end() ? nullptr : &tracks_[it->second];
}

int Database::sortKeyCompare(const Track& a, const Track& b) const {
    std::string aa = normalize(strings_.get(a.albumArtistRef));
    std::string ba = normalize(strings_.get(b.albumArtistRef));
    if (aa != ba) return aa < ba ? -1 : 1;
    if (a.year != b.year) return a.year < b.year ? -1 : 1;
    std::string al = normalize(strings_.get(a.albumRef));
    std::string bl = normalize(strings_.get(b.albumRef));
    if (al != bl) return al < bl ? -1 : 1;
    if (a.discNo != b.discNo) return a.discNo < b.discNo ? -1 : 1;
    if (a.trackNo != b.trackNo) return a.trackNo < b.trackNo ? -1 : 1;
    std::string at = normalize(strings_.get(a.titleRef));
    std::string bt = normalize(strings_.get(b.titleRef));
    if (at != bt) return at < bt ? -1 : 1;
    return 0;
}

void Database::buildIndexes() {
    sortOrder_.resize(tracks_.size());
    for (uint32_t i = 0; i < tracks_.size(); ++i) sortOrder_[i] = i;
    std::sort(sortOrder_.begin(), sortOrder_.end(),
              [&](uint32_t a, uint32_t b) {
                  return sortKeyCompare(tracks_[a], tracks_[b]) < 0;
              });

    tokenIndex_.clear();
    for (uint32_t i = 0; i < tracks_.size(); ++i) {
        const Track& t = tracks_[i];
        std::string blob = strings_.get(t.titleRef) + " " +
                           strings_.get(t.artistRef) + " " +
                           strings_.get(t.albumRef) + " " +
                           strings_.get(t.albumArtistRef);
        for (const auto& tok : tokenize(blob)) {
            auto& v = tokenIndex_[tok];
            if (v.empty() || v.back() != i) v.push_back(i);
        }
    }
    sortedTokens_.clear();
    sortedTokens_.reserve(tokenIndex_.size());
    for (auto& kv : tokenIndex_) sortedTokens_.push_back(kv.first);
    std::sort(sortedTokens_.begin(), sortedTokens_.end());
    indexed_ = true;
}

std::vector<uint64_t> Database::idsFromSortedIndices(
    const std::vector<uint32_t>& idx) const {
    std::vector<uint64_t> out;
    out.reserve(idx.size());
    for (uint32_t i : idx) out.push_back(tracks_[i].trackId);
    return out;
}

std::vector<uint64_t> Database::search(const std::string& query,
                                       std::size_t limit) const {
    std::vector<std::string> toks = tokenize(query);
    if (toks.empty()) return {};

    // Postings (track indices) per token; final token prefix-matched.
    auto postingsExact = [&](const std::string& tok) -> std::vector<uint32_t> {
        auto it = tokenIndex_.find(tok);
        return it == tokenIndex_.end() ? std::vector<uint32_t>{} : it->second;
    };
    auto postingsPrefix = [&](const std::string& pre) -> std::vector<uint32_t> {
        std::vector<uint32_t> merged;
        auto lo = std::lower_bound(sortedTokens_.begin(), sortedTokens_.end(), pre);
        for (auto it = lo; it != sortedTokens_.end(); ++it) {
            if (it->compare(0, pre.size(), pre) != 0) break;
            const auto& p = tokenIndex_.at(*it);
            merged.insert(merged.end(), p.begin(), p.end());
        }
        std::sort(merged.begin(), merged.end());
        merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
        return merged;
    };

    std::vector<uint32_t> acc;
    for (std::size_t k = 0; k < toks.size(); ++k) {
        bool last = (k + 1 == toks.size());
        std::vector<uint32_t> p = last ? postingsPrefix(toks[k])
                                       : postingsExact(toks[k]);
        std::sort(p.begin(), p.end());
        if (k == 0) {
            acc = std::move(p);
        } else {
            std::vector<uint32_t> inter;
            std::set_intersection(acc.begin(), acc.end(), p.begin(), p.end(),
                                  std::back_inserter(inter));
            acc = std::move(inter);
        }
        if (acc.empty()) return {};
    }
    // Order results by browse rank.
    std::vector<uint32_t> ranked(acc.begin(), acc.end());
    std::sort(ranked.begin(), ranked.end(), [&](uint32_t a, uint32_t b) {
        return sortKeyCompare(tracks_[a], tracks_[b]) < 0;
    });
    if (limit && ranked.size() > limit) ranked.resize(limit);
    return idsFromSortedIndices(ranked);
}

std::vector<uint64_t> Database::allTracks() const {
    return idsFromSortedIndices(sortOrder_);
}

std::vector<uint64_t> Database::byAlbumKey(uint64_t albumKey) const {
    std::vector<uint32_t> idx;
    for (uint32_t i : sortOrder_)
        if (tracks_[i].albumKey == albumKey) idx.push_back(i);
    return idsFromSortedIndices(idx);
}

std::vector<uint64_t> Database::bySource(uint8_t sourceId) const {
    std::vector<uint32_t> idx;
    for (uint32_t i : sortOrder_)
        if (tracks_[i].sourceId == sourceId) idx.push_back(i);
    return idsFromSortedIndices(idx);
}

std::vector<Database::AlbumEntry> Database::albums() const {
    // Dedup by albumKey (not by adjacency: with sparse tags two tracks of one
    // album need not be contiguous in browse order). First-seen order preserved.
    std::vector<AlbumEntry> out;
    std::unordered_map<uint64_t, bool> seen;
    for (uint32_t i : sortOrder_) {
        const Track& t = tracks_[i];
        if (seen.emplace(t.albumKey, true).second)
            out.push_back({t.albumKey, strings_.get(t.albumRef),
                           strings_.get(t.albumArtistRef)});
    }
    return out;
}

std::vector<std::string> Database::artists() const {
    std::vector<std::string> out;
    std::unordered_map<std::string, bool> seen;
    for (uint32_t i : sortOrder_) {
        std::string a = strings_.get(tracks_[i].albumArtistRef);
        if (seen.emplace(normalize(a), true).second) out.push_back(a);
    }
    return out;
}

// --- Persistence ------------------------------------------------------------
// Layout: ["CDB1"][u32 trackCount][Track[] raw][u32 stringsLen][strings blob].
// Track is POD; we copy it byte-for-byte (host is little-endian; the Pi 4 is too,
// so the blob is portable between build server and device).

std::vector<uint8_t> Database::serialize() const {
    std::vector<uint8_t> out;
    auto putU32 = [&](uint32_t x) {
        out.push_back(uint8_t(x)); out.push_back(uint8_t(x >> 8));
        out.push_back(uint8_t(x >> 16)); out.push_back(uint8_t(x >> 24));
    };
    out.insert(out.end(), {'C', 'D', 'B', '1'});
    putU32(uint32_t(tracks_.size()));
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(tracks_.data());
    out.insert(out.end(), raw, raw + tracks_.size() * sizeof(Track));
    auto sblob = strings_.serialize();
    putU32(uint32_t(sblob.size()));
    out.insert(out.end(), sblob.begin(), sblob.end());
    return out;
}

bool Database::deserialize(const std::vector<uint8_t>& blob) {
    if (blob.size() < 8 || std::memcmp(blob.data(), "CDB1", 4) != 0)
        return false;
    std::size_t p = 4;
    auto getU32 = [&](uint32_t& x) -> bool {
        if (p + 4 > blob.size()) return false;
        x = uint32_t(blob[p]) | (uint32_t(blob[p + 1]) << 8) |
            (uint32_t(blob[p + 2]) << 16) | (uint32_t(blob[p + 3]) << 24);
        p += 4;
        return true;
    };
    uint32_t count;
    if (!getU32(count)) return false;
    if (p + std::size_t(count) * sizeof(Track) > blob.size()) return false;
    tracks_.resize(count);
    std::memcpy(tracks_.data(), blob.data() + p, count * sizeof(Track));
    p += count * sizeof(Track);
    uint32_t slen;
    if (!getU32(slen)) return false;
    if (p + slen > blob.size()) return false;
    if (!strings_.deserialize(blob.data() + p, slen)) return false;

    idIndex_.clear();
    for (std::size_t i = 0; i < tracks_.size(); ++i)
        idIndex_[tracks_[i].trackId] = i;
    buildIndexes();
    return true;
}

}  // namespace carsh::library
