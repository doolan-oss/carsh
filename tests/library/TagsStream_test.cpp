// Streaming tag-parser tests (review L1). The ID3v2 and FLAC readers must walk
// tags straight from the stream: cover art (APIC / PICTURE / an oversized Vorbis
// comment) is seeked past, never read or buffered, and a corrupt size field can't
// make them allocate what it claims. Each test builds the tag by hand, wraps it
// in a CountingStream (bytes actually read) and runs the parser inside an
// AllocScope (largest operator-new allocation). Also covers the ID3 features the
// frame walker handles: v2.2 ids, extended headers, unsynchronisation, v2.4
// data-length indicators and iTunes-style non-syncsafe frame sizes.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "HeapProbe.h"
#include "Test.h"
#include "fs/MemStream.h"
#include "library/tags/FlacTags.h"
#include "library/tags/Id3v2.h"

using namespace carsh;
using namespace carsh::library;
using carsh::testprobe::AllocScope;
using carsh::testprobe::CountingStream;
using carsh::testprobe::IoStats;

namespace {

using Bytes = std::vector<uint8_t>;

constexpr std::size_t kSmallAlloc = 64 * 1024;  // parser buffers must stay <= this

void append(Bytes& to, const Bytes& b) { to.insert(to.end(), b.begin(), b.end()); }

Bytes syncsafe4(uint32_t v) {
    return {uint8_t((v >> 21) & 0x7F), uint8_t((v >> 14) & 0x7F),
            uint8_t((v >> 7) & 0x7F), uint8_t(v & 0x7F)};
}
Bytes be4(uint32_t v) {
    return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
}

// Text-frame payload: encoding byte + bytes.
Bytes latin1(const std::string& s) {
    Bytes b = {0};
    b.insert(b.end(), s.begin(), s.end());
    return b;
}
// UTF-16 with a little-endian BOM (ASCII input).
Bytes utf16bom(const std::string& s) {
    Bytes b = {1, 0xFF, 0xFE};
    for (char c : s) { b.push_back(uint8_t(c)); b.push_back(0); }
    return b;
}
// APIC-like payload of n bytes, with 0xFF bytes sprinkled in (JPEG-ish).
Bytes artPayload(std::size_t n) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = uint8_t(i % 7 == 0 ? 0xFF : i * 31);
    return b;
}

// v2.3/v2.4 frame (4-char id, 10-byte header).
Bytes frame(int major, const char* id, const Bytes& payload, uint8_t fmtFlags = 0,
            bool plainBe32Size = false) {
    Bytes f(id, id + 4);
    const uint32_t n = uint32_t(payload.size());
    append(f, (major >= 4 && !plainBe32Size) ? syncsafe4(n) : be4(n));
    f.push_back(0);
    f.push_back(fmtFlags);
    append(f, payload);
    return f;
}
// v2.2 frame (3-char id, 3-byte size, no flags).
Bytes frame22(const char* id, const Bytes& payload) {
    Bytes f(id, id + 3);
    const uint32_t n = uint32_t(payload.size());
    f.push_back(uint8_t(n >> 16)); f.push_back(uint8_t(n >> 8)); f.push_back(uint8_t(n));
    append(f, payload);
    return f;
}
// Tag header + body; `claimedSize` overrides the header size field.
Bytes tag(int major, uint8_t flags, const Bytes& body, int64_t claimedSize = -1) {
    Bytes t = {'I', 'D', '3', uint8_t(major), 0, flags};
    append(t, syncsafe4(claimedSize < 0 ? uint32_t(body.size()) : uint32_t(claimedSize)));
    append(t, body);
    return t;
}
// ID3 unsynchronisation: 0x00 after every 0xFF that precedes 0x00 or >= 0xE0
// (and after a trailing 0xFF).
Bytes unsync(const Bytes& in) {
    Bytes out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        out.push_back(in[i]);
        if (in[i] == 0xFF && (i + 1 == in.size() || in[i + 1] == 0x00 || in[i + 1] >= 0xE0))
            out.push_back(0x00);
    }
    return out;
}

struct Parsed {
    bool ok = false;
    TrackMeta meta;
    IoStats io;
    testprobe::AllocStats alloc;
};

template <typename Fn>
Parsed run(Bytes file, Fn parse) {
    Parsed r;
    auto inner = std::make_unique<host::MemStream>(std::move(file));  // outside the scope
    CountingStream s(std::move(inner), r.io);
    {
        AllocScope scope;
        r.ok = parse(&s, r.meta);
        r.alloc = scope.stats();
    }
    return r;
}
Parsed runId3(Bytes file) { return run(std::move(file), tags::parseId3v2); }
Parsed runFlac(Bytes file) { return run(std::move(file), tags::parseFlacTags); }

// ---- FLAC builders ----
Bytes flacBlock(uint8_t type, bool last, const Bytes& body, int64_t claimedLen = -1) {
    const uint32_t n = claimedLen < 0 ? uint32_t(body.size()) : uint32_t(claimedLen);
    Bytes b = {uint8_t((last ? 0x80 : 0) | type), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
    append(b, body);
    return b;
}
// STREAMINFO: 44100 Hz, 2 ch, 16 bit, `total` samples.
Bytes streamInfo(uint64_t total) {
    Bytes b(34, 0);
    const uint32_t sr = 44100;
    b[10] = uint8_t(sr >> 12);
    b[11] = uint8_t(sr >> 4);
    b[12] = uint8_t(((sr & 0x0F) << 4) | ((2 - 1) << 1) | ((16 - 1) >> 4));
    b[13] = uint8_t((((16 - 1) & 0x0F) << 4) | ((total >> 32) & 0x0F));
    b[14] = uint8_t(total >> 24); b[15] = uint8_t(total >> 16);
    b[16] = uint8_t(total >> 8);  b[17] = uint8_t(total);
    return b;
}
void le4(Bytes& b, uint32_t v) {
    b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
}
Bytes vorbisComment(const std::vector<std::string>& comments) {
    Bytes b;
    const std::string vendor = "carsh test";
    le4(b, uint32_t(vendor.size()));
    b.insert(b.end(), vendor.begin(), vendor.end());
    le4(b, uint32_t(comments.size()));
    for (const auto& c : comments) {
        le4(b, uint32_t(c.size()));
        b.insert(b.end(), c.begin(), c.end());
    }
    return b;
}
Bytes flacFile(const std::vector<Bytes>& blocks) {
    Bytes f = {'f', 'L', 'a', 'C'};
    for (const auto& b : blocks) append(f, b);
    Bytes audio(512, 0xAA);  // stand-in frames after the metadata
    append(f, audio);
    return f;
}

}  // namespace

// A 2 MiB cover between the text frames is seeked past: all fields parse, the
// art flag is set, and neither the reads nor the allocations scale with it.
TEST(TagsStream, Id3v23LargeApicIsSkippedNotRead) {
    Bytes body;
    append(body, frame(3, "TIT2", latin1("Night Drive")));
    append(body, frame(3, "APIC", artPayload(2u << 20)));
    append(body, frame(3, "TPE1", latin1("Carsh Test")));
    append(body, frame(3, "TALB", latin1("Demos")));
    append(body, frame(3, "TRCK", latin1("3/12")));
    append(body, frame(3, "TYER", latin1("2023")));
    append(body, Bytes(256, 0));  // padding
    Bytes file = tag(3, 0, body);
    append(file, Bytes(1024, 0x55));  // "audio"

    Parsed r = runId3(std::move(file));
    REQUIRE(r.ok);
    CHECK_EQ(r.meta.title, std::string("Night Drive"));
    CHECK_EQ(r.meta.artist, std::string("Carsh Test"));
    CHECK_EQ(r.meta.album, std::string("Demos"));
    CHECK_EQ(r.meta.trackNo, uint16_t(3));
    CHECK_EQ(r.meta.year, uint16_t(2023));
    CHECK(r.meta.hasEmbeddedArt);
    CHECK(r.io.bytesRead < 4096);          // the 2 MiB APIC body was never read
    CHECK(r.alloc.maxBytes <= kSmallAlloc);
    CHECK(r.alloc.totalBytes < 16 * 1024);
}

TEST(TagsStream, Id3v24LargeApicIsSkippedNotRead) {
    Bytes body;
    append(body, frame(4, "APIC", artPayload(1536u << 10)));
    append(body, frame(4, "TIT2", Bytes{3, 'O', 'k'}));
    append(body, frame(4, "TDRC", latin1("1999-05-01")));
    Parsed r = runId3(tag(4, 0, body));
    REQUIRE(r.ok);
    CHECK_EQ(r.meta.title, std::string("Ok"));
    CHECK_EQ(r.meta.year, uint16_t(1999));
    CHECK(r.meta.hasEmbeddedArt);
    CHECK(r.io.bytesRead < 4096);
    CHECK(r.alloc.maxBytes <= kSmallAlloc);
}

// A 12-byte file whose header claims a 256 MiB tag used to allocate (and, on
// Circle, permanently lose) 256 MiB. The size is now clamped to the stream.
TEST(TagsStream, Id3HugeClaimedTagSizeIsClamped) {
    Bytes file = {'I', 'D', '3', 3, 0, 0, 0x7F, 0x7F, 0x7F, 0x7F, 0, 0};
    Parsed r = runId3(file);
    CHECK(r.ok);  // valid header, just nothing in it
    CHECK(r.meta.title.empty());
    CHECK(r.alloc.maxBytes <= kSmallAlloc);

    // Same lie, but a real frame follows: it still parses.
    Bytes body = frame(3, "TIT2", latin1("abcd"));
    Parsed r2 = runId3(tag(3, 0, body, 0x0FFFFFFF));
    REQUIRE(r2.ok);
    CHECK_EQ(r2.meta.title, std::string("abcd"));
    CHECK(r2.alloc.maxBytes <= kSmallAlloc);
}

// A frame whose size runs past the tag stops the walk (fields before it kept);
// an in-bounds but absurdly large text frame is skipped, not buffered.
TEST(TagsStream, Id3OversizedFramesAreNotBuffered) {
    Bytes body = frame(3, "TPE1", latin1("Kept"));
    Bytes lying = frame(3, "TIT2", latin1("x"));
    lying[4] = 0x7F; lying[5] = 0xFF; lying[6] = 0xFF; lying[7] = 0xFF;  // ~2 GiB
    append(body, lying);
    Parsed r = runId3(tag(3, 0, body));
    REQUIRE(r.ok);
    CHECK_EQ(r.meta.artist, std::string("Kept"));
    CHECK(r.meta.title.empty());
    CHECK(r.alloc.maxBytes <= kSmallAlloc);

    Bytes body2 = frame(3, "TIT2", latin1(std::string(200 * 1024, 'z')));
    append(body2, frame(3, "TALB", latin1("After")));
    Parsed r2 = runId3(tag(3, 0, body2));
    REQUIRE(r2.ok);
    CHECK(r2.meta.title.empty());  // > kMaxId3TextFrame: skipped
    CHECK_EQ(r2.meta.album, std::string("After"));
    CHECK(r2.alloc.maxBytes <= kSmallAlloc);
    CHECK(r2.io.bytesRead < 4096);
}

// v2.3 tag-level unsynchronisation: frame sizes count the de-unsynced bytes, so
// the reader must undo it across the whole tag (including inside the skipped
// APIC, which is full of 0xFF 0xE0.. pairs that gained stuffing bytes).
TEST(TagsStream, Id3v23TagLevelUnsync) {
    Bytes art = artPayload(4096);
    for (std::size_t i = 0; i + 1 < art.size(); i += 64) { art[i] = 0xFF; art[i + 1] = 0xE7; }
    Bytes body;
    append(body, frame(3, "TIT2", utf16bom("Hi")));  // BOM FF FE gets a stuffed 00
    append(body, frame(3, "APIC", art));
    append(body, frame(3, "TPE1", latin1("Band")));
    const Bytes stuffed = unsync(body);
    REQUIRE(stuffed.size() > body.size());
    Parsed r = runId3(tag(3, 0x80, stuffed));
    REQUIRE(r.ok);
    CHECK_EQ(r.meta.title, std::string("Hi"));
    CHECK_EQ(r.meta.artist, std::string("Band"));
    CHECK(r.meta.hasEmbeddedArt);
    CHECK(r.alloc.maxBytes <= kSmallAlloc);
}

// v2.4: per-frame unsync flag + data-length indicator (4 extra bytes) + grouping.
TEST(TagsStream, Id3v24FrameFlags) {
    Bytes title = utf16bom("Yo");
    Bytes stored = unsync(title);
    Bytes withDli = syncsafe4(uint32_t(title.size()));  // data length indicator
    append(withDli, stored);
    Bytes body = frame(4, "TIT2", withDli, 0x02 | 0x01);
    Bytes grouped = {0x42};  // group id byte
    append(grouped, latin1("Grp"));
    append(body, frame(4, "TPE1", grouped, 0x40));
    Bytes compressed = latin1("zlib?");
    append(body, frame(4, "TALB", compressed, 0x08 | 0x01));  // compressed: skipped
    append(body, frame(4, "TCON", latin1("Synth")));
    Parsed r = runId3(tag(4, 0, body));
    REQUIRE(r.ok);
    CHECK_EQ(r.meta.title, std::string("Yo"));
    CHECK_EQ(r.meta.artist, std::string("Grp"));
    CHECK(r.meta.album.empty());
    CHECK_EQ(r.meta.genre, std::string("Synth"));
}

// Old iTunes wrote v2.4 frame sizes as plain big-endian; a set high bit in the
// size bytes can only mean that, so the frame is skipped by its real length.
TEST(TagsStream, Id3v24NonSyncsafeFrameSize) {
    Bytes body = frame(4, "APIC", artPayload(200), 0, /*plainBe32Size=*/true);
    append(body, frame(4, "TPE1", latin1("iTunes")));
    Parsed r = runId3(tag(4, 0, body));
    REQUIRE(r.ok);
    CHECK(r.meta.hasEmbeddedArt);
    CHECK_EQ(r.meta.artist, std::string("iTunes"));
}

TEST(TagsStream, Id3ExtendedHeaderIsSkipped) {
    // v2.3: size (6, excludes itself) + 2 flag bytes + 4-byte padding size.
    Bytes b3 = be4(6);
    append(b3, Bytes{0, 0, 0, 0, 0, 0});
    append(b3, frame(3, "TIT2", latin1("Ext3")));
    Parsed r3 = runId3(tag(3, 0x40, b3));
    REQUIRE(r3.ok);
    CHECK_EQ(r3.meta.title, std::string("Ext3"));

    // v2.4: syncsafe size 6 (includes itself) + flag-byte count + flags.
    Bytes b4 = syncsafe4(6);
    append(b4, Bytes{1, 0});
    append(b4, frame(4, "TIT2", latin1("Ext4")));
    Parsed r4 = runId3(tag(4, 0x40, b4));
    REQUIRE(r4.ok);
    CHECK_EQ(r4.meta.title, std::string("Ext4"));
}

TEST(TagsStream, Id3v22Frames) {
    Bytes body;
    append(body, frame22("TT2", latin1("Old Song")));
    append(body, frame22("PIC", artPayload(3000)));
    append(body, frame22("TP1", latin1("Old Band")));
    append(body, frame22("TAL", latin1("Old Album")));
    append(body, frame22("TRK", latin1("7")));
    append(body, frame22("TYE", latin1("1988")));
    Parsed r = runId3(tag(2, 0, body));
    REQUIRE(r.ok);
    CHECK_EQ(r.meta.title, std::string("Old Song"));
    CHECK_EQ(r.meta.artist, std::string("Old Band"));
    CHECK_EQ(r.meta.album, std::string("Old Album"));
    CHECK_EQ(r.meta.trackNo, uint16_t(7));
    CHECK_EQ(r.meta.year, uint16_t(1988));
    CHECK(r.meta.hasEmbeddedArt);
}

TEST(TagsStream, Id3RejectsInvalidHeaders) {
    TrackMeta m;
    Bytes v5 = tag(5, 0, frame(4, "TIT2", latin1("x")));
    host::MemStream s5(v5);
    CHECK(!tags::parseId3v2(&s5, m));
    Bytes notSyncsafe = tag(3, 0, frame(3, "TIT2", latin1("x")));
    notSyncsafe[9] = 0x80;
    host::MemStream sn(notSyncsafe);
    CHECK(!tags::parseId3v2(&sn, m));
    Bytes shortFile = {'I', 'D', '3'};
    host::MemStream ss(shortFile);
    CHECK(!tags::parseId3v2(&ss, m));
}

// Garbage after the real frames (bad id chars) ends the walk cleanly.
TEST(TagsStream, Id3StopsAtGarbageFrameId) {
    Bytes body = frame(3, "TIT2", latin1("Good"));
    append(body, Bytes{'t', 'i', '#', '2', 0, 0, 0, 2, 0, 0, 0, 'x'});
    append(body, frame(3, "TPE1", latin1("Unreached")));
    Parsed r = runId3(tag(3, 0, body));
    REQUIRE(r.ok);
    CHECK_EQ(r.meta.title, std::string("Good"));
    CHECK(r.meta.artist.empty());
}

// ---- FLAC ----

// A 2 MiB PICTURE block before the Vorbis comment is seeked past.
TEST(TagsStream, FlacLargePictureIsSkippedNotRead) {
    Bytes file = flacFile({
        flacBlock(0, false, streamInfo(22050)),
        flacBlock(6, false, artPayload(2u << 20)),
        flacBlock(1, false, Bytes(8192, 0)),  // PADDING
        flacBlock(4, true, vorbisComment({"TITLE=Night Drive", "artist=Carsh Test",
                                          "ALBUM=Demos", "TRACKNUMBER=3/9", "DATE=2023"})),
    });
    Parsed r = runFlac(std::move(file));
    REQUIRE(r.ok);
    CHECK_EQ(r.meta.title, std::string("Night Drive"));
    CHECK_EQ(r.meta.artist, std::string("Carsh Test"));
    CHECK_EQ(r.meta.album, std::string("Demos"));
    CHECK_EQ(r.meta.trackNo, uint16_t(3));
    CHECK_EQ(r.meta.year, uint16_t(2023));
    CHECK_EQ(r.meta.sampleRate, 44100u);
    CHECK_EQ(r.meta.channels, uint8_t(2));
    CHECK_EQ(r.meta.bitsPerSample, uint8_t(16));
    CHECK_EQ(r.meta.durationMs, 500u);
    CHECK(r.meta.hasEmbeddedArt);
    CHECK(r.io.bytesRead < 4096);
    CHECK(r.alloc.maxBytes <= kSmallAlloc);
}

// Some taggers put cover art in the Vorbis comment itself (base64
// METADATA_BLOCK_PICTURE). That comment is skipped; the ones after it parse.
TEST(TagsStream, FlacOversizedVorbisCommentIsSkipped) {
    const std::string bigPic = "METADATA_BLOCK_PICTURE=" + std::string(300 * 1024, 'A');
    Bytes file = flacFile({
        flacBlock(0, false, streamInfo(44100)),
        flacBlock(4, true, vorbisComment({"ARTIST=Before", bigPic, "TITLE=After"})),
    });
    Parsed r = runFlac(std::move(file));
    REQUIRE(r.ok);
    CHECK_EQ(r.meta.artist, std::string("Before"));
    CHECK_EQ(r.meta.title, std::string("After"));
    CHECK(r.alloc.maxBytes <= kSmallAlloc);
    CHECK(r.io.bytesRead < 4096);
}

// A block length running past EOF (truncated/corrupt file) is never allocated or
// read; STREAMINFO before it is kept.
TEST(TagsStream, FlacBlockPastEofIsNotAllocated) {
    Bytes file = {'f', 'L', 'a', 'C'};
    append(file, flacBlock(0, false, streamInfo(88200)));
    append(file, flacBlock(4, true, vorbisComment({"TITLE=Lost"}), 0xFFFFFF));
    Parsed r = runFlac(std::move(file));
    REQUIRE(r.ok);
    CHECK_EQ(r.meta.sampleRate, 44100u);
    CHECK_EQ(r.meta.durationMs, 2000u);
    CHECK(r.meta.title.empty());
    CHECK(r.alloc.maxBytes <= kSmallAlloc);

    // A comment count / length lying inside a well-formed block is bounded by
    // the block, not trusted.
    Bytes vc = vorbisComment({"TITLE=Fine"});
    vc[vc.size() - 10 - 4] = 0xFF;  // first comment length -> huge
    vc[vc.size() - 10 - 1] = 0x7F;
    Parsed r2 = runFlac(flacFile({flacBlock(0, false, streamInfo(1)),
                                  flacBlock(4, true, vc)}));
    REQUIRE(r2.ok);
    CHECK(r2.meta.title.empty());
    CHECK(r2.alloc.maxBytes <= kSmallAlloc);
}

// The in-RAM Vorbis parser (used by Opus) and the streaming one agree.
TEST(TagsStream, VorbisStreamMatchesBufferParser) {
    Bytes vc = vorbisComment({"TITLE=T", "ALBUM ARTIST=AA", "DISCNUMBER=2/2",
                              "GENRE=Ambient", "junk-without-equals", "YEAR=1971"});
    TrackMeta a, b;
    REQUIRE(tags::parseVorbisComment(vc.data(), vc.size(), a));
    host::MemStream s(vc);
    REQUIRE(tags::parseVorbisCommentStream(&s, vc.size(), b));
    CHECK_EQ(a.title, b.title);
    CHECK_EQ(b.albumArtist, std::string("AA"));
    CHECK_EQ(a.albumArtist, b.albumArtist);
    CHECK_EQ(b.discNo, uint16_t(2));
    CHECK_EQ(a.genre, b.genre);
    CHECK_EQ(b.year, uint16_t(1971));
    CHECK_EQ(a.year, b.year);
}
