// AVRCP now-playing assembly tests. The metadata attributes (title/artist/album/
// song-length) and the playback-status notification arrive from the controller as
// SEPARATE events; NowPlayingAssembler accumulates them into one AvrcpMetadata.
// This is the platform-independent half the device glue feeds (BtStackGlue.cpp);
// here we drive it with synthetic values, no radio needed (H6).
#include "bt/NowPlaying.h"

#include "Test.h"

using namespace carsh;
using carsh::bt::AvrcpAttr;
using carsh::bt::NowPlayingAssembler;

namespace {
// Feed a text attribute the way the glue does: id + length-delimited bytes (the
// literal is intentionally NOT relied on to be NUL-terminated by the assembler).
void attr(NowPlayingAssembler& a, AvrcpAttr id, const char* s) {
    a.setAttribute(static_cast<uint8_t>(id), s, std::char_traits<char>::length(s));
}
}  // namespace

TEST(BtParse, AssemblesSeparateAttributesIntoOneMetadata) {
    NowPlayingAssembler a;
    CHECK(!a.hasText());
    attr(a, AvrcpAttr::Title, "Teardrop");
    attr(a, AvrcpAttr::Artist, "Massive Attack");
    attr(a, AvrcpAttr::Album, "Mezzanine");
    CHECK(a.hasText());
    const bt::AvrcpMetadata& m = a.metadata();
    CHECK_EQ(m.title, std::string("Teardrop"));
    CHECK_EQ(m.artist, std::string("Massive Attack"));
    CHECK_EQ(m.album, std::string("Mezzanine"));
}

TEST(BtParse, PlayingTimeParsedAsMilliseconds) {
    NowPlayingAssembler a;
    attr(a, AvrcpAttr::PlayingTimeMs, "330000");  // AVRCP sends ms as ASCII text
    CHECK_EQ(a.metadata().durationMs, uint32_t(330000));
}

TEST(BtParse, PlaybackStatusMapsOnlyPlayingToTrue) {
    NowPlayingAssembler a;
    a.setPlaybackStatus(uint8_t(bt::AvrcpPlayStatus::Playing));
    CHECK(a.metadata().playing);
    a.setPlaybackStatus(uint8_t(bt::AvrcpPlayStatus::Paused));
    CHECK(!a.metadata().playing);
    a.setPlaybackStatus(uint8_t(bt::AvrcpPlayStatus::Stopped));
    CHECK(!a.metadata().playing);
    a.setPlaybackStatus(uint8_t(bt::AvrcpPlayStatus::Error));
    CHECK(!a.metadata().playing);
}

TEST(BtParse, LengthDelimitedValuesIgnoreTrailingBytes) {
    // AVRCP values are length-delimited, not NUL-terminated: only the first `len`
    // bytes belong to the field.
    NowPlayingAssembler a;
    const char buf[] = "ArtistXXXXX";
    a.setAttribute(uint8_t(AvrcpAttr::Artist), buf, 6);  // "Artist"
    CHECK_EQ(a.metadata().artist, std::string("Artist"));
}

TEST(BtParse, DurationParseIsDefensive) {
    NowPlayingAssembler a;
    attr(a, AvrcpAttr::PlayingTimeMs, "12a45");  // stops at first non-digit
    CHECK_EQ(a.metadata().durationMs, uint32_t(12));
    attr(a, AvrcpAttr::PlayingTimeMs, "");        // empty -> 0
    CHECK_EQ(a.metadata().durationMs, uint32_t(0));
    attr(a, AvrcpAttr::PlayingTimeMs, "99999999999999");  // saturates, no overflow
    CHECK_EQ(a.metadata().durationMs, uint32_t(0xFFFFFFFF));
}

TEST(BtParse, ResetClearsStaleFieldsAcrossTracks) {
    NowPlayingAssembler a;
    attr(a, AvrcpAttr::Title, "Old Title");
    attr(a, AvrcpAttr::Artist, "Old Artist");
    a.setPlaybackStatus(uint8_t(bt::AvrcpPlayStatus::Playing));
    a.reset();
    CHECK(!a.hasText());
    CHECK(!a.metadata().playing);
    CHECK_EQ(a.metadata().title, std::string(""));
    // New track sets only the title; artist must NOT carry over from the old one.
    attr(a, AvrcpAttr::Title, "New Title");
    CHECK_EQ(a.metadata().title, std::string("New Title"));
    CHECK_EQ(a.metadata().artist, std::string(""));
}

TEST(BtParse, UnknownAttributeIdsAreIgnored) {
    NowPlayingAssembler a;
    a.setAttribute(0, "x", 1);     // not a valid AVRCP attribute id
    a.setAttribute(200, "y", 1);
    attr(a, AvrcpAttr::Genre, "Trip-Hop");        // known id, but not surfaced
    attr(a, AvrcpAttr::TrackNumber, "3");
    CHECK(!a.hasText());           // none of these populate title/artist/album
    CHECK_EQ(a.metadata().durationMs, uint32_t(0));
}
