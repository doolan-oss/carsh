// Library Database — fixed-schema in-RAM store with built indexes (PLAN.md §4.1).
//
// Holds Track records + a StringTable, an id->index map for upsert, an inverted
// token index for unified search (normalized, casefolded, accent-stripped), and
// a sorted browse order (album-artist / year / album / disc / track / title).
// Persists to a binary blob (records + strings) for the on-disk tracks.db +
// strings.dat; writes are caller-batched to spare SD endurance (PLAN.md R7).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "library/Model.h"
#include "library/StringTable.h"

namespace carsh::library {

class Database {
public:
    // Insert or update a track keyed by (sourceId, path). Returns the trackId.
    uint64_t addTrack(const TrackMeta& meta, uint8_t sourceId,
                      const std::string& path, uint8_t format,
                      const std::string& folderForAlbumKey = "");

    void setReplayGain(uint64_t trackId, float trackGainDb, float albumGainDb,
                       float trackPeak, float albumPeak);

    bool removeTrack(uint64_t trackId);

    const Track* get(uint64_t trackId) const;
    std::size_t trackCount() const { return tracks_.size(); }

    // Field accessors (resolve interned strings).
    std::string title(const Track& t) const { return strings_.get(t.titleRef); }
    std::string artist(const Track& t) const { return strings_.get(t.artistRef); }
    std::string album(const Track& t) const { return strings_.get(t.albumRef); }
    std::string albumArtist(const Track& t) const {
        return strings_.get(t.albumArtistRef);
    }
    std::string genre(const Track& t) const { return strings_.get(t.genreRef); }
    std::string path(const Track& t) const { return strings_.get(t.pathRef); }
    const StringTable& strings() const { return strings_; }

    // Rebuild token + sort indexes. Call after a batch of adds/removes.
    void buildIndexes();

    // Unified search: AND of query tokens; the final token is prefix-matched for
    // typeahead. Returns trackIds in browse order, up to `limit` (0 = all).
    std::vector<uint64_t> search(const std::string& query,
                                 std::size_t limit = 0) const;

    // Browse helpers (all return trackIds in browse order).
    std::vector<uint64_t> allTracks() const;
    std::vector<uint64_t> byAlbumKey(uint64_t albumKey) const;
    std::vector<uint64_t> bySource(uint8_t sourceId) const;
    // Distinct (albumKey, display name) pairs in browse order.
    struct AlbumEntry { uint64_t albumKey; std::string album; std::string artist; };
    std::vector<AlbumEntry> albums() const;
    std::vector<std::string> artists() const;
    // All tracks by one album-artist (normalized match), in browse order — the
    // hierarchical-browse "play/shuffle this artist" scope (LibraryBrowser).
    std::vector<uint64_t> byArtist(const std::string& albumArtist) const;
    // Distinct albums of one album-artist (normalized match), in browse order.
    std::vector<AlbumEntry> albumsByArtist(const std::string& albumArtist) const;

    // Visit every track (for smart-playlist evaluation).
    template <typename Fn>
    void forEach(Fn&& fn) const {
        for (const auto& t : tracks_) fn(t);
    }

    // Persistence: records + strings to/from one blob.
    std::vector<uint8_t> serialize() const;
    bool deserialize(const std::vector<uint8_t>& blob);

private:
    int sortKeyCompare(const Track& a, const Track& b) const;
    std::vector<uint64_t> idsFromSortedIndices(
        const std::vector<uint32_t>& idx) const;

    StringTable strings_;
    std::vector<Track> tracks_;
    std::unordered_map<uint64_t, std::size_t> idIndex_;  // trackId -> tracks_ idx

    // Indexes (rebuilt by buildIndexes()).
    std::vector<uint32_t> sortOrder_;  // tracks_ indices in browse order
    std::unordered_map<std::string, std::vector<uint32_t>> tokenIndex_;
    std::vector<std::string> sortedTokens_;  // for prefix typeahead
    bool indexed_ = false;
};

}  // namespace carsh::library
