#include "library/Database.h"

#include <string>

#include "Test.h"
#include "library/TextNorm.h"

using namespace carsh::library;

TEST(TextNorm, FoldsCaseAndAccents) {
    CHECK_EQ(normalize("Björk"), std::string("bjork"));
    CHECK_EQ(normalize("MÖTLEY  CRÜE"), std::string("motley crue"));
    CHECK_EQ(normalize("  Café del Mar! "), std::string("cafe del mar"));
    // Accents fold to the base letter. Ligatures (æ, œ) are not expanded — they
    // act as separators (documented limitation of the single-codepoint fold).
    CHECK_EQ(normalize("Sigur Rós – Café"), std::string("sigur ros cafe"));
}

TEST(TextNorm, Tokenizes) {
    auto t = tokenize("The Dark Side, of the Moon");
    REQUIRE(t.size() == 6);
    CHECK_EQ(t[0], std::string("the"));
    CHECK_EQ(t[3], std::string("of"));
}

static TrackMeta meta(const std::string& title, const std::string& artist,
                      const std::string& album, uint16_t trk, uint16_t year) {
    TrackMeta m;
    m.title = title; m.artist = artist; m.album = album;
    m.trackNo = trk; m.year = year; m.durationMs = 200000;
    m.sampleRate = 44100; m.channels = 2; m.bitsPerSample = 16;
    return m;
}

static Database makeLib() {
    Database db;
    db.addTrack(meta("Money", "Pink Floyd", "The Dark Side of the Moon", 6, 1973),
                1, "/PF/DSOTM/06.flac", 1, "/PF/DSOTM");
    db.addTrack(meta("Time", "Pink Floyd", "The Dark Side of the Moon", 4, 1973),
                1, "/PF/DSOTM/04.flac", 1, "/PF/DSOTM");
    db.addTrack(meta("Bohemian Rhapsody", "Queen", "A Night at the Opera", 11, 1975),
                1, "/Q/ANATO/11.flac", 1, "/Q/ANATO");
    db.addTrack(meta("Björk Song", "Björk", "Post", 2, 1995),
                2, "/B/Post/02.mp3", 2, "/B/Post");
    db.buildIndexes();
    return db;
}

TEST(Database, AddAndGet) {
    Database db = makeLib();
    CHECK_EQ(db.trackCount(), std::size_t(4));
    uint64_t id = makeTrackId(1, "/PF/DSOTM/06.flac");
    const Track* t = db.get(id);
    REQUIRE(t != nullptr);
    CHECK_EQ(db.title(*t), std::string("Money"));
    CHECK_EQ(db.artist(*t), std::string("Pink Floyd"));
    CHECK_EQ(t->year, uint16_t(1973));
}

TEST(Database, UpsertReplacesNotDuplicates) {
    Database db;
    db.addTrack(meta("Time", "Pink Floyd", "DSOTM", 4, 1973), 1, "/x.flac", 1);
    db.addTrack(meta("Time (Remaster)", "Pink Floyd", "DSOTM", 4, 1973), 1,
                "/x.flac", 1);
    CHECK_EQ(db.trackCount(), std::size_t(1));
    const Track* t = db.get(makeTrackId(1, "/x.flac"));
    REQUIRE(t);
    CHECK_EQ(db.title(*t), std::string("Time (Remaster)"));
}

TEST(Database, BrowseOrderByAlbumArtistYearTrack) {
    Database db = makeLib();
    auto all = db.allTracks();
    REQUIRE(all.size() == 4);
    // Browse order: Björk < Pink Floyd < Queen (album-artist). Within DSOTM,
    // track 4 (Time) before track 6 (Money).
    CHECK_EQ(db.title(*db.get(all[0])), std::string("Björk Song"));
    CHECK_EQ(db.title(*db.get(all[1])), std::string("Time"));
    CHECK_EQ(db.title(*db.get(all[2])), std::string("Money"));
    CHECK_EQ(db.title(*db.get(all[3])), std::string("Bohemian Rhapsody"));
}

TEST(Database, SearchTokenAndPrefix) {
    Database db = makeLib();
    // AND across tokens.
    auto r = db.search("pink moon");
    REQUIRE(r.size() == 2);  // both DSOTM tracks (artist+album match)
    // Prefix typeahead on the last token.
    auto r2 = db.search("queen boh");
    REQUIRE(r2.size() == 1);
    CHECK_EQ(db.title(*db.get(r2[0])), std::string("Bohemian Rhapsody"));
    // Accent-insensitive.
    auto r3 = db.search("bjork");
    REQUIRE(r3.size() == 1);
    // No match.
    CHECK(db.search("zzzz").empty());
}

TEST(Database, BrowseAlbumsAndArtists) {
    Database db = makeLib();
    auto albums = db.albums();
    CHECK_EQ(albums.size(), std::size_t(3));
    auto artists = db.artists();
    CHECK_EQ(artists.size(), std::size_t(3));
    // By album key: DSOTM has 2 tracks.
    uint64_t dsotmKey =
        makeAlbumKey("Pink Floyd", "The Dark Side of the Moon", "/PF/DSOTM");
    CHECK_EQ(db.byAlbumKey(dsotmKey).size(), std::size_t(2));
    // By source.
    CHECK_EQ(db.bySource(2).size(), std::size_t(1));  // Björk on source 2
}

TEST(Database, ReplayGainStoredAndPreservedOnRefresh) {
    Database db;
    uint64_t id =
        db.addTrack(meta("T", "A", "Alb", 1, 2000), 1, "/t.flac", 1);
    db.setReplayGain(id, -7.5f, -6.0f, 0.9f, 0.95f);
    db.addTrack(meta("T (new tags)", "A", "Alb", 1, 2000), 1, "/t.flac", 1);
    const Track* t = db.get(id);
    REQUIRE(t);
    CHECK_NEAR(t->rgTrackGain, -7.5f, 1e-4);  // preserved across metadata update
    CHECK(t->flags & FlagRgScanned);
}

TEST(Database, SerializeRoundTrip) {
    Database db = makeLib();
    db.setReplayGain(makeTrackId(1, "/Q/ANATO/11.flac"), -8.0f, -7.0f, 0.8f, 0.85f);
    auto blob = db.serialize();

    Database db2;
    REQUIRE(db2.deserialize(blob));
    CHECK_EQ(db2.trackCount(), std::size_t(4));
    // Search index rebuilt on load.
    CHECK_EQ(db2.search("queen boh").size(), std::size_t(1));
    const Track* t = db2.get(makeTrackId(1, "/Q/ANATO/11.flac"));
    REQUIRE(t);
    CHECK_EQ(db2.title(*t), std::string("Bohemian Rhapsody"));
    CHECK_NEAR(t->rgTrackGain, -8.0f, 1e-4);
}
