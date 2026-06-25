#include "library/SmartPlaylist.h"

#include "Test.h"
#include "library/Database.h"

using namespace carsh::library;

static TrackMeta mk(const std::string& title, const std::string& artist,
                    const std::string& album, const std::string& genre,
                    uint16_t year, uint32_t durMs) {
    TrackMeta m;
    m.title = title; m.artist = artist; m.album = album; m.genre = genre;
    m.year = year; m.durationMs = durMs; m.sampleRate = 44100; m.channels = 2;
    return m;
}

static Database lib() {
    Database db;
    db.addTrack(mk("Take Five", "Dave Brubeck", "Time Out", "Jazz", 1959, 324000),
                1, "/a.flac", 1);
    db.addTrack(mk("So What", "Miles Davis", "Kind of Blue", "Jazz", 1959, 545000),
                1, "/b.flac", 1);
    db.addTrack(mk("Giant Steps", "John Coltrane", "Giant Steps", "Jazz", 2000,
                   286000), 1, "/c.flac", 1);
    db.addTrack(mk("Smells Like Teen Spirit", "Nirvana", "Nevermind", "Rock",
                   1991, 301000), 1, "/d.flac", 1);
    db.buildIndexes();
    return db;
}

TEST(SmartPlaylist, GenreEquals) {
    Database db = lib();
    SmartRules r;
    r.rules.push_back({Field::Genre, Op::Eq, "jazz", 0});
    auto res = SmartPlaylist::evaluate(db, r);
    CHECK_EQ(res.size(), std::size_t(3));
}

TEST(SmartPlaylist, AndNumericAndString) {
    Database db = lib();
    SmartRules r;
    r.match = MatchMode::All;
    r.rules.push_back({Field::Genre, Op::Eq, "jazz", 0});
    r.rules.push_back({Field::Year, Op::Ge, "", 2000});
    auto res = SmartPlaylist::evaluate(db, r);
    REQUIRE(res.size() == 1);
    CHECK_EQ(db.title(*db.get(res[0])), std::string("Giant Steps"));
}

TEST(SmartPlaylist, AnyMatch) {
    Database db = lib();
    SmartRules r;
    r.match = MatchMode::Any;
    r.rules.push_back({Field::Genre, Op::Eq, "rock", 0});
    r.rules.push_back({Field::Artist, Op::Contains, "miles", 0});
    auto res = SmartPlaylist::evaluate(db, r);
    CHECK_EQ(res.size(), std::size_t(2));
}

TEST(SmartPlaylist, SortAndLimit) {
    Database db = lib();
    SmartRules r;
    r.rules.push_back({Field::Genre, Op::Eq, "jazz", 0});
    r.sortBy = SortBy::DurationMs;
    r.descending = true;
    r.limit = 2;
    auto res = SmartPlaylist::evaluate(db, r);
    REQUIRE(res.size() == 2);
    // Longest first: So What (545s) then Take Five (324s).
    CHECK_EQ(db.title(*db.get(res[0])), std::string("So What"));
    CHECK_EQ(db.title(*db.get(res[1])), std::string("Take Five"));
}

TEST(SmartPlaylist, EmptyRulesMatchesAll) {
    Database db = lib();
    SmartRules r;
    CHECK_EQ(SmartPlaylist::evaluate(db, r).size(), std::size_t(4));
}

TEST(SmartPlaylist, ContainsStartsWith) {
    Database db = lib();
    SmartRules r;
    r.rules.push_back({Field::Title, Op::StartsWith, "giant", 0});
    CHECK_EQ(SmartPlaylist::evaluate(db, r).size(), std::size_t(1));
    SmartRules r2;
    r2.rules.push_back({Field::Album, Op::Contains, "blue", 0});
    CHECK_EQ(SmartPlaylist::evaluate(db, r2).size(), std::size_t(1));
}
