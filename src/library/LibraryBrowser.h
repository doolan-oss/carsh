// LibraryBrowser — the hierarchical local-library navigation model: drill from
// Artists -> Albums -> Songs, with "play" / "shuffle" actions scoped to whatever
// level you're looking at (the whole library at the artist list, one artist at its
// album list, one album at its song list). It is platform-independent and host-
// tested; the UI (ui/LibraryScreen.h) just renders rows()/title() and forwards taps,
// and the app turns a returned PlayRequest into a player queue.
//
// Scope rule (the heart of the feature): playAll(shuffle) acts on the CURRENT level —
//   Artists level -> all tracks in the library
//   Albums  level -> all tracks by the selected artist
//   Songs   level -> all tracks on the selected album
// and tapping a song plays the album from that song onward.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "library/Database.h"
#include "library/TextNorm.h"

namespace carsh::library {

class LibraryBrowser {
public:
    enum class Level { Artists, Albums, Songs };

    // A queue to hand the player: trackIds (already ordered or shuffled) + where to
    // start. Empty trackIds => nothing to play (e.g. empty library), so callers skip.
    struct PlayRequest {
        std::vector<uint64_t> trackIds;
        int startIndex = 0;
        bool valid() const { return !trackIds.empty(); }
    };

    explicit LibraryBrowser(const Database& db) : db_(db) { reset(); }

    // Seed the shuffle PRNG (device seeds from the timer; tests use a fixed value).
    void setSeed(uint64_t s) { rng_ = s ? s : kDefaultSeed; }

    // Rebuild from the database and return to the Artists root. Call after a (re)scan.
    void reset() {
        curArtist_.clear();
        curAlbumKey_ = 0;
        curAlbumName_.clear();
        loadArtists();
    }

    Level level() const { return level_; }
    const std::string& title() const { return title_; }
    const std::vector<std::string>& rows() const { return rows_; }
    bool canGoBack() const { return level_ != Level::Artists; }

    // A tap on display row `i`. At Songs level it returns a PlayRequest (play that
    // song within its album); at Artists/Albums it drills down and returns nullopt.
    // Out-of-range rows are ignored (returns nullopt, no navigation).
    std::optional<PlayRequest> select(int i) {
        switch (level_) {
            case Level::Artists:
                if (i < 0 || i >= int(artists_.size())) return std::nullopt;
                curArtist_ = artists_[std::size_t(i)];
                loadAlbums();
                return std::nullopt;
            case Level::Albums:
                if (i < 0 || i >= int(albums_.size())) return std::nullopt;
                curAlbumKey_ = albums_[std::size_t(i)].albumKey;
                curAlbumName_ = albums_[std::size_t(i)].album;
                loadSongs();
                return std::nullopt;
            case Level::Songs:
                if (i < 0 || i >= int(songs_.size())) return std::nullopt;
                return PlayRequest{songs_, i};
        }
        return std::nullopt;
    }

    // Up one level (Songs -> Albums -> Artists). No-op at the Artists root.
    void back() {
        if (level_ == Level::Songs) loadAlbums();
        else if (level_ == Level::Albums) loadArtists();
    }

    // Play (true=shuffle) the current scope. See the scope rule in the file header.
    PlayRequest playAll(bool shuffle) {
        std::vector<uint64_t> ids;
        switch (level_) {
            case Level::Artists: ids = db_.allTracks(); break;
            case Level::Albums:  ids = db_.byArtist(curArtist_); break;
            case Level::Songs:   ids = songs_; break;
        }
        if (shuffle) shuffleInPlace(ids);
        return PlayRequest{std::move(ids), 0};
    }

    // Index of the first row that sorts under `letter` (A–Z), or under "#" (any row
    // whose normalized first character isn't a–z, e.g. digits/symbols). -1 if none.
    // Used by the alphabetical-jump bar; most useful at the Artists level (which is
    // alphabetical), but works on whatever rows are showing.
    int firstRowForLetter(char letter) const {
        const char target = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
        const bool wantOther = !(target >= 'a' && target <= 'z');
        for (std::size_t i = 0; i < rows_.size(); ++i) {
            const std::string n = normalize(rows_[i]);
            const char c0 = n.empty() ? '\0' : n[0];
            const bool isAlpha = (c0 >= 'a' && c0 <= 'z');
            if (wantOther ? !isAlpha : (isAlpha && c0 == target)) return int(i);
        }
        return -1;
    }

    // Short scope tag for button captions: "ALL" / "ARTIST" / "ALBUM".
    const char* scope() const {
        switch (level_) {
            case Level::Artists: return "ALL";
            case Level::Albums:  return "ARTIST";
            case Level::Songs:   return "ALBUM";
        }
        return "";
    }

private:
    static constexpr uint64_t kDefaultSeed = 0x9e3779b97f4a7c15ull;

    void loadArtists() {
        level_ = Level::Artists;
        artists_ = db_.artists();
        rows_.clear();
        rows_.reserve(artists_.size());
        for (const auto& a : artists_) rows_.push_back(a.empty() ? "(unknown artist)" : a);
        title_ = "Artists";
    }
    void loadAlbums() {
        level_ = Level::Albums;
        albums_ = db_.albumsByArtist(curArtist_);
        rows_.clear();
        rows_.reserve(albums_.size());
        for (const auto& al : albums_)
            rows_.push_back(al.album.empty() ? "(unknown album)" : al.album);
        title_ = curArtist_.empty() ? "(unknown artist)" : curArtist_;
    }
    void loadSongs() {
        level_ = Level::Songs;
        songs_ = db_.byAlbumKey(curAlbumKey_);
        rows_.clear();
        rows_.reserve(songs_.size());
        for (uint64_t id : songs_) {
            const Track* t = db_.get(id);
            if (t == nullptr) { rows_.push_back("?"); continue; }
            std::string name = db_.title(*t);
            if (name.empty()) name = "(untitled)";
            if (t->trackNo > 0) {
                char num[12];
                std::snprintf(num, sizeof num, "%u. ", unsigned(t->trackNo));
                rows_.push_back(std::string(num) + name);
            } else {
                rows_.push_back(name);
            }
        }
        title_ = curAlbumName_.empty() ? "(unknown album)" : curAlbumName_;
    }

    // Fisher-Yates with an xorshift64 PRNG: deterministic given the seed, no <random>
    // dependency (keeps it bare-metal-friendly and host-test-reproducible).
    void shuffleInPlace(std::vector<uint64_t>& v) {
        for (std::size_t i = v.size(); i > 1; --i) {
            const std::size_t j = std::size_t(nextRand() % uint64_t(i));
            std::swap(v[i - 1], v[j]);
        }
    }
    uint64_t nextRand() {
        uint64_t x = rng_;
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        rng_ = x;
        return x;
    }

    const Database& db_;
    Level level_ = Level::Artists;
    std::string title_;
    std::vector<std::string> rows_;

    // Backing data for the current level (1:1 with rows_).
    std::vector<std::string> artists_;
    std::vector<Database::AlbumEntry> albums_;
    std::vector<uint64_t> songs_;

    // Navigation context.
    std::string curArtist_;
    uint64_t curAlbumKey_ = 0;
    std::string curAlbumName_;

    uint64_t rng_ = kDefaultSeed;
};

}  // namespace carsh::library
