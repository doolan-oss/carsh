// Database browse helpers + LibraryBrowser — the hierarchical artist/album/song
// navigation and the scoped play/shuffle actions behind the local-library tab.
#include "library/LibraryBrowser.h"

#include <algorithm>
#include <string>
#include <vector>

#include "Test.h"
#include "library/Database.h"

using namespace carsh::library;

namespace {
TrackMeta meta(const std::string& title, const std::string& artist,
               const std::string& album, uint16_t trk, uint16_t year) {
    TrackMeta m;
    m.title = title; m.artist = artist; m.album = album;
    m.trackNo = trk; m.year = year; m.durationMs = 200000;
    m.sampleRate = 44100; m.channels = 2; m.bitsPerSample = 16;
    return m;
}

// Two Pink Floyd tracks (one album), one Queen, one Björk. Browse order sorts by
// normalized album-artist, then year/album/disc/track: bjork < pink floyd < queen.
Database makeLib() {
    Database db;
    db.addTrack(meta("Money", "Pink Floyd", "The Dark Side of the Moon", 6, 1973),
                1, "/PF/DSOTM/06.flac", 1, "/PF/DSOTM");
    db.addTrack(meta("Time", "Pink Floyd", "The Dark Side of the Moon", 4, 1973),
                1, "/PF/DSOTM/04.flac", 1, "/PF/DSOTM");
    db.addTrack(meta("Bohemian Rhapsody", "Queen", "A Night at the Opera", 11, 1975),
                1, "/Q/ANATO/11.flac", 1, "/Q/ANATO");
    db.addTrack(meta("Army of Me", "Björk", "Post", 2, 1995),
                2, "/B/Post/02.mp3", 2, "/B/Post");
    db.buildIndexes();
    return db;
}

std::vector<uint64_t> sorted(std::vector<uint64_t> v) {
    std::sort(v.begin(), v.end());
    return v;
}
}  // namespace

// --- Database helpers -------------------------------------------------------

TEST(DatabaseBrowse, ByArtistReturnsOnlyThatArtistInOrder) {
    Database db = makeLib();
    std::vector<uint64_t> pf = db.byArtist("Pink Floyd");
    REQUIRE(pf.size() == 2);
    // Browse order within the album is by track number: Time (4) before Money (6).
    CHECK_EQ(pf[0], makeTrackId(1, "/PF/DSOTM/04.flac"));
    CHECK_EQ(pf[1], makeTrackId(1, "/PF/DSOTM/06.flac"));
    // Normalized match: lowercase query hits the same tracks.
    CHECK_EQ(db.byArtist("pink floyd").size(), std::size_t(2));
    CHECK_EQ(db.byArtist("Nobody").size(), std::size_t(0));
}

TEST(DatabaseBrowse, AlbumsByArtist) {
    Database db = makeLib();
    auto al = db.albumsByArtist("Pink Floyd");
    REQUIRE(al.size() == 1);
    CHECK_EQ(al[0].album, std::string("The Dark Side of the Moon"));
    CHECK_EQ(db.albumsByArtist("Björk").size(), std::size_t(1));
}

// --- LibraryBrowser navigation ----------------------------------------------

TEST(LibraryBrowser, StartsAtArtists) {
    Database db = makeLib();
    LibraryBrowser b(db);
    CHECK(b.level() == LibraryBrowser::Level::Artists);
    CHECK_EQ(b.title(), std::string("Artists"));
    CHECK(!b.canGoBack());
    REQUIRE(b.rows().size() == 3);
    CHECK_EQ(b.rows()[0], std::string("Björk"));
    CHECK_EQ(b.rows()[1], std::string("Pink Floyd"));
    CHECK_EQ(b.rows()[2], std::string("Queen"));
}

TEST(LibraryBrowser, DrillsArtistToAlbumToSong) {
    Database db = makeLib();
    LibraryBrowser b(db);

    // Tap "Pink Floyd" (row 1) -> Albums level, no playback.
    CHECK(!b.select(1).has_value());
    CHECK(b.level() == LibraryBrowser::Level::Albums);
    CHECK_EQ(b.title(), std::string("Pink Floyd"));
    CHECK(b.canGoBack());
    REQUIRE(b.rows().size() == 1);
    CHECK_EQ(b.rows()[0], std::string("The Dark Side of the Moon"));

    // Tap the album -> Songs level, track-numbered rows in order.
    CHECK(!b.select(0).has_value());
    CHECK(b.level() == LibraryBrowser::Level::Songs);
    REQUIRE(b.rows().size() == 2);
    CHECK_EQ(b.rows()[0], std::string("4. Time"));
    CHECK_EQ(b.rows()[1], std::string("6. Money"));

    // Tap the second song -> play the album starting at index 1.
    auto req = b.select(1);
    REQUIRE(req.has_value());
    CHECK(req->valid());
    CHECK_EQ(req->startIndex, 1);
    REQUIRE(req->trackIds.size() == 2);
    CHECK_EQ(req->trackIds[0], makeTrackId(1, "/PF/DSOTM/04.flac"));
    CHECK_EQ(req->trackIds[1], makeTrackId(1, "/PF/DSOTM/06.flac"));
}

TEST(LibraryBrowser, BackUnwindsLevels) {
    Database db = makeLib();
    LibraryBrowser b(db);
    b.select(1);  // Albums (Pink Floyd)
    b.select(0);  // Songs (DSOTM)
    b.back();
    CHECK(b.level() == LibraryBrowser::Level::Albums);
    CHECK_EQ(b.title(), std::string("Pink Floyd"));
    b.back();
    CHECK(b.level() == LibraryBrowser::Level::Artists);
    CHECK(!b.canGoBack());
    b.back();  // no-op at root
    CHECK(b.level() == LibraryBrowser::Level::Artists);
}

TEST(LibraryBrowser, FirstRowForLetter) {
    Database db = makeLib();
    LibraryBrowser b(db);  // Artists: Björk(0), Pink Floyd(1), Queen(2)
    CHECK_EQ(b.firstRowForLetter('B'), 0);
    CHECK_EQ(b.firstRowForLetter('b'), 0);   // case-insensitive
    CHECK_EQ(b.firstRowForLetter('P'), 1);   // Björk normalizes to "bjork"
    CHECK_EQ(b.firstRowForLetter('Q'), 2);
    CHECK_EQ(b.firstRowForLetter('Z'), -1);  // no match
    CHECK_EQ(b.firstRowForLetter('#'), -1);  // no non-alpha-leading row
}

TEST(LibraryBrowser, OutOfRangeSelectIgnored) {
    Database db = makeLib();
    LibraryBrowser b(db);
    CHECK(!b.select(99).has_value());
    CHECK(b.level() == LibraryBrowser::Level::Artists);  // didn't navigate
    CHECK(!b.select(-1).has_value());
}

// --- Scoped play / shuffle --------------------------------------------------

TEST(LibraryBrowser, PlayScopeFollowsLevel) {
    Database db = makeLib();
    LibraryBrowser b(db);

    // Artists level -> whole library.
    CHECK_EQ(std::string(b.scope()), std::string("ALL"));
    CHECK_EQ(b.playAll(false).trackIds.size(), std::size_t(4));

    b.select(1);  // Pink Floyd albums
    CHECK_EQ(std::string(b.scope()), std::string("ARTIST"));
    CHECK_EQ(b.playAll(false).trackIds.size(), std::size_t(2));  // both PF tracks

    b.select(0);  // DSOTM songs
    CHECK_EQ(std::string(b.scope()), std::string("ALBUM"));
    auto album = b.playAll(false);
    REQUIRE(album.trackIds.size() == 2);
    CHECK_EQ(album.startIndex, 0);
    CHECK_EQ(album.trackIds[0], makeTrackId(1, "/PF/DSOTM/04.flac"));  // ordered
}

TEST(LibraryBrowser, ShuffleIsAPermutationAndDeterministicPerSeed) {
    Database db = makeLib();
    LibraryBrowser b(db);
    const std::vector<uint64_t> all = b.playAll(false).trackIds;  // ordered, 4 tracks

    b.setSeed(12345);
    auto s1 = b.playAll(true).trackIds;
    REQUIRE(s1.size() == all.size());
    CHECK(sorted(s1) == sorted(all));  // same multiset -> a true permutation

    // Same seed -> same shuffle (reproducible); a different seed generally differs.
    b.setSeed(12345);
    auto s2 = b.playAll(true).trackIds;
    CHECK(s2 == s1);
    b.setSeed(67890);
    auto s3 = b.playAll(true).trackIds;
    CHECK(sorted(s3) == sorted(all));
}
