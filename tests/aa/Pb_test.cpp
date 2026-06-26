// Protobuf wire-codec tests. The AA service-discovery / sensor / input messages
// are built and parsed with this codec; here we verify it against the proto2 wire
// spec (known byte sequences), round-trip every field type, and exercise nested
// submessages + unknown-field skipping. Pure logic, no hardware (AA_PLAN Phase 1).
#include "aa/Pb.h"

#include "Test.h"

using namespace carsh::aa;

// The canonical protobuf example: field 1, varint 150 -> 08 96 01.
TEST(Pb, KnownVarintEncoding) {
    std::vector<uint8_t> out;
    pb::Writer w(out);
    w.uint32(1, 150);
    const std::vector<uint8_t> expect = {0x08, 0x96, 0x01};
    CHECK(out == expect);
}

TEST(Pb, VarintBoundaries) {
    for (uint64_t v : {uint64_t(0), uint64_t(1), uint64_t(127), uint64_t(128),
                       uint64_t(16383), uint64_t(16384), uint64_t(0xFFFFFFFFull),
                       uint64_t(0xFFFFFFFFFFFFFFFFull)}) {
        std::vector<uint8_t> out;
        pb::Writer(out).varint(v);
        pb::Reader r(out);
        uint64_t got = 0;
        REQUIRE(r.varint(got));
        CHECK_EQ(got, v);
        CHECK(r.eof());
    }
}

TEST(Pb, RoundTripScalarFields) {
    std::vector<uint8_t> out;
    pb::Writer w(out);
    w.uint32(1, 44100);
    w.boolean(2, true);
    w.int32(3, -5);
    w.string(4, "Android Auto");

    pb::Reader r(out);
    uint32_t field;
    uint8_t wire;

    REQUIRE(r.tag(field, wire));
    CHECK_EQ(int(field), 1); CHECK_EQ(int(wire), 0);
    uint32_t u; REQUIRE(r.u32(u)); CHECK_EQ(u, uint32_t(44100));

    REQUIRE(r.tag(field, wire)); CHECK_EQ(int(field), 2);
    REQUIRE(r.u32(u)); CHECK_EQ(int(u), 1);

    REQUIRE(r.tag(field, wire)); CHECK_EQ(int(field), 3);
    uint64_t raw; REQUIRE(r.varint(raw));
    CHECK_EQ(static_cast<int32_t>(raw), -5);  // sign-extended varint

    REQUIRE(r.tag(field, wire)); CHECK_EQ(int(field), 4); CHECK_EQ(int(wire), 2);
    std::string s; REQUIRE(r.str(s)); CHECK_EQ(s, std::string("Android Auto"));
    CHECK(r.eof());
}

// Mirrors how a service-discovery response nests channel descriptors.
TEST(Pb, NestedSubmessage) {
    std::vector<uint8_t> out;
    pb::Writer w(out);
    w.uint32(1, 7);  // top-level field
    w.submessage(2, [](pb::Writer& sub) {
        sub.uint32(1, 3);          // channel_id
        sub.string(2, "audio");
    });

    pb::Reader r(out);
    uint32_t field; uint8_t wire;
    REQUIRE(r.tag(field, wire)); CHECK_EQ(int(field), 1);
    uint32_t top; REQUIRE(r.u32(top)); CHECK_EQ(int(top), 7);

    REQUIRE(r.tag(field, wire)); CHECK_EQ(int(field), 2); CHECK_EQ(int(wire), 2);
    pb::Reader sub(nullptr, 0);
    REQUIRE(r.submessage(sub));
    REQUIRE(sub.tag(field, wire)); CHECK_EQ(int(field), 1);
    uint32_t cid; REQUIRE(sub.u32(cid)); CHECK_EQ(int(cid), 3);
    REQUIRE(sub.tag(field, wire)); CHECK_EQ(int(field), 2);
    std::string name; REQUIRE(sub.str(name)); CHECK_EQ(name, std::string("audio"));
    CHECK(sub.eof());
    CHECK(r.eof());
}

// A decoder must skip fields it doesn't recognise (forward-compat) and still find
// the ones it wants — across all four wire types.
TEST(Pb, SkipUnknownFields) {
    std::vector<uint8_t> out;
    pb::Writer w(out);
    w.uint32(1, 99);                                   // wire 0
    w.string(2, "ignore me");                          // wire 2
    // Hand-emit a 32-bit (wire 5) and 64-bit (wire 1) field to be skipped.
    pb::Writer raw(out);
    raw.tag(5, 3); out.insert(out.end(), {0xDE, 0xAD, 0xBE, 0xEF});
    raw.tag(1, 4); out.insert(out.end(), {1, 2, 3, 4, 5, 6, 7, 8});
    w.uint32(5, 1234);                                 // the one we want

    pb::Reader r(out);
    uint32_t field; uint8_t wire;
    uint32_t found = 0;
    while (!r.eof()) {
        REQUIRE(r.tag(field, wire));
        if (field == 5) { REQUIRE(r.u32(found)); break; }
        REQUIRE(r.skip(wire));
    }
    CHECK_EQ(found, uint32_t(1234));
}

// Truncated input must fail cleanly, never read out of bounds.
TEST(Pb, TruncatedInputFails) {
    const uint8_t buf[] = {0x12, 0x05, 'a', 'b'};  // says 5-byte string, only 2 present
    pb::Reader r(buf, sizeof(buf));
    uint32_t field; uint8_t wire;
    REQUIRE(r.tag(field, wire));
    std::string s;
    CHECK(!r.str(s));  // refuses — length exceeds remaining
}
