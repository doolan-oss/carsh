// Tag-parser tests against real encoder output (lame/flac/opusenc), plus a
// hand-crafted ID3v2.4 buffer to exercise the frame walker directly.
#include <string>
#include <vector>

#include "Test.h"
#include "fs/HostVfs.h"
#include "fs/MemStream.h"
#include "library/tags/Id3v2.h"
#include "library/tags/TagReader.h"

using namespace carsh;
using namespace carsh::library;

static std::string fx(const char* n) {
    return std::string(CARSH_FIXTURES_DIR) + "/" + n;
}

static bool readFixtureTags(const char* name, decode::Codec codec,
                            TrackMeta& m) {
    host::HostVfs vfs("/");
    auto s = vfs.open(fx(name));
    if (!s) return false;
    return tags::readTags(s.get(), codec, m);
}

TEST(Tags, FlacVorbisComment) {
    TrackMeta m;
    if (!readFixtureTags("tagged.flac", decode::Codec::Flac, m)) {
        std::printf("  (skip) no tagged.flac\n");
        return;
    }
    CHECK_EQ(m.title, std::string("Night Drive"));
    CHECK_EQ(m.artist, std::string("Carsh Test"));
    CHECK_EQ(m.album, std::string("Demos"));
    CHECK_EQ(m.year, uint16_t(2023));
    CHECK_EQ(m.trackNo, uint16_t(3));
    // STREAMINFO also fills audio props + exact duration (22050 frames @ 44100).
    CHECK_EQ(m.sampleRate, 44100u);
    CHECK_EQ(m.channels, uint8_t(2));
    CHECK_EQ(m.durationMs, 500u);
}

TEST(Tags, Mp3Id3v2) {
    TrackMeta m;
    if (!readFixtureTags("tagged.mp3", decode::Codec::Mp3, m)) {
        std::printf("  (skip) no tagged.mp3\n");
        return;
    }
    CHECK_EQ(m.title, std::string("Night Drive"));
    CHECK_EQ(m.artist, std::string("Carsh Test"));
    CHECK_EQ(m.album, std::string("Demos"));
    CHECK_EQ(m.year, uint16_t(2023));
    CHECK_EQ(m.trackNo, uint16_t(3));
}

TEST(Tags, OpusTags) {
    TrackMeta m;
    if (!readFixtureTags("tagged.opus", decode::Codec::Opus, m)) {
        std::printf("  (skip) no tagged.opus\n");
        return;
    }
    CHECK_EQ(m.title, std::string("Night Drive"));
    CHECK_EQ(m.artist, std::string("Carsh Test"));
    CHECK_EQ(m.album, std::string("Demos"));
    CHECK_EQ(m.year, uint16_t(2023));
    CHECK_EQ(m.trackNo, uint16_t(3));
}

// Hand-built ID3v2.4 tag: header + one UTF-8 TIT2 frame. Exercises syncsafe
// sizes and the text-encoding path without relying on an encoder.
TEST(Tags, Id3v2HandBuiltUtf8) {
    auto frame = [](const char* id, const std::string& text) {
        std::vector<uint8_t> f;
        f.insert(f.end(), id, id + 4);
        uint32_t sz = uint32_t(text.size() + 1);  // +1 encoding byte
        // syncsafe size
        f.push_back(uint8_t((sz >> 21) & 0x7F));
        f.push_back(uint8_t((sz >> 14) & 0x7F));
        f.push_back(uint8_t((sz >> 7) & 0x7F));
        f.push_back(uint8_t(sz & 0x7F));
        f.push_back(0); f.push_back(0);  // frame flags
        f.push_back(3);  // UTF-8
        f.insert(f.end(), text.begin(), text.end());
        return f;
    };
    std::vector<uint8_t> body;
    auto t = frame("TIT2", "Über Song");
    auto a = frame("TPE1", "Mötley");
    body.insert(body.end(), t.begin(), t.end());
    body.insert(body.end(), a.begin(), a.end());

    std::vector<uint8_t> tag = {'I','D','3', 4, 0, 0};
    uint32_t sz = uint32_t(body.size());
    tag.push_back(uint8_t((sz >> 21) & 0x7F));
    tag.push_back(uint8_t((sz >> 14) & 0x7F));
    tag.push_back(uint8_t((sz >> 7) & 0x7F));
    tag.push_back(uint8_t(sz & 0x7F));
    tag.insert(tag.end(), body.begin(), body.end());

    host::MemStream s(tag);
    TrackMeta m;
    REQUIRE(tags::parseId3v2(&s, m));
    CHECK_EQ(m.title, std::string("Über Song"));   // UTF-8 preserved
    CHECK_EQ(m.artist, std::string("Mötley"));
}
