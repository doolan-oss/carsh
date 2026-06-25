// Indexer tests over the generated library tree (real WAV files), including the
// interruptible step model and the ReplayGain (BS.1770) scan pass.
#include "library/Indexer.h"

#include <string>

#include "Test.h"
#include "fs/HostVfs.h"
#include "library/Database.h"

using namespace carsh;
using namespace carsh::library;

static std::string libRoot() {
    return std::string(CARSH_FIXTURES_DIR) + "/library";
}

TEST(Indexer, ScansTreeIntoDatabase) {
    // HostSource rooted at the fixtures library tree.
    host::HostSource src(1, hal::SourceType::Usb, libRoot());
    Database db;
    Indexer idx(db);
    idx.beginScan(src);
    auto p0 = idx.progress();
    CHECK_EQ(p0.filesTotal, std::size_t(4));  // 2 albums x 2 tracks

    // Interruptible: one file per step.
    int steps = 0;
    while (idx.step(1)) { ++steps; if (steps > 100) break; }
    CHECK_EQ(db.trackCount(), std::size_t(4));
    CHECK(idx.progress().complete);

    // Untagged WAVs: titles fall back to filename stems; props from decoder.
    auto all = db.allTracks();
    REQUIRE(all.size() == 4);
    const Track* t = db.get(all[0]);
    REQUIRE(t);
    CHECK_EQ(t->sampleRate, 44100u);
    CHECK_EQ(t->channels, uint8_t(2));
    CHECK(t->durationMs > 0);
    CHECK(!db.title(*t).empty());

    // Two distinct albums discovered.
    CHECK_EQ(db.albums().size(), std::size_t(2));
}

TEST(Indexer, ReplayGainScanPopulatesGain) {
    host::HostSource src(2, hal::SourceType::Usb, libRoot());
    Database db;
    Indexer idx(db);
    idx.enableReplayGain(true);
    idx.beginScan(src);
    while (idx.step(2)) {}

    auto all = db.allTracks();
    REQUIRE(!all.empty());
    const Track* t = db.get(all[0]);
    REQUIRE(t);
    CHECK(t->flags & FlagRgScanned);
    // The fixture sine is loud; gain to -23 LUFS is finite and non-trivial.
    CHECK(t->rgTrackGain != 0.0f);
    CHECK(t->rgTrackGain > -40.0f && t->rgTrackGain < 40.0f);
    CHECK(t->rgTrackPeak > 0.0f && t->rgTrackPeak <= 1.0f);
    // Album gain set on every track in an album.
    CHECK(t->rgAlbumGain != 0.0f);
}

TEST(Indexer, RescanIsIdempotent) {
    host::HostSource src(1, hal::SourceType::Usb, libRoot());
    Database db;
    Indexer idx(db);
    idx.beginScan(src);
    while (idx.step(4)) {}
    CHECK_EQ(db.trackCount(), std::size_t(4));

    Indexer idx2(db);
    idx2.beginScan(src);
    while (idx2.step(4)) {}
    CHECK_EQ(db.trackCount(), std::size_t(4));  // upsert, not duplicate
}
