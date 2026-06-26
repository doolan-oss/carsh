// H.264 NAL extraction/classification tests for the AA video channel. Pure logic
// (AA_PLAN Phase 2): strip the media-message timestamp envelope, split Annex-B
// start codes, classify parameter sets vs picture slices vs keyframes. Synthetic
// bitstreams (the real captured video is encrypted in our fixtures).
#include "aa/H264Nal.h"

#include <vector>

#include "Test.h"

using namespace carsh::aa;

namespace {
// Append an Annex-B NAL: 4-byte start code + header byte (type) + body.
void addNal(std::vector<uint8_t>& bs, uint8_t headerByte, std::vector<uint8_t> body = {},
            bool shortStartCode = false) {
    if (shortStartCode) {
        bs.insert(bs.end(), {0x00, 0x00, 0x01});
    } else {
        bs.insert(bs.end(), {0x00, 0x00, 0x00, 0x01});
    }
    bs.push_back(headerByte);
    bs.insert(bs.end(), body.begin(), body.end());
}
// header byte: forbidden_zero(0) | nal_ref_idc(2 bits) | type(5 bits)
uint8_t hdr(NalType t, uint8_t refIdc = 3) {
    return static_cast<uint8_t>((refIdc << 5) | static_cast<uint8_t>(t));
}
}  // namespace

TEST(H264Nal, ExtractStripsTimestampOnlyForWithTs) {
    std::vector<uint8_t> payload = {1, 2, 3, 4, 5, 6, 7, 8, 0xAA, 0xBB};

    auto withTs = extractH264(kAvMediaWithTs, payload.data(), payload.size());
    REQUIRE(withTs.second == 2);
    CHECK_EQ(int(withTs.first[0]), 0xAA);  // first byte after the 8-byte timestamp

    auto data = extractH264(kAvMediaData, payload.data(), payload.size());
    REQUIRE(data.second == payload.size());  // no timestamp stripped
    CHECK_EQ(int(data.first[0]), 1);

    auto tooShort = extractH264(kAvMediaWithTs, payload.data(), 8);
    CHECK(tooShort.first == nullptr);
}

TEST(H264Nal, SplitsAnnexBAndClassifies) {
    std::vector<uint8_t> bs;
    addNal(bs, hdr(NalType::Sps), {0x64, 0x00});
    addNal(bs, hdr(NalType::Pps), {0xCE});
    addNal(bs, hdr(NalType::IdrSlice), {0x88, 0x84, 0x00}, /*shortStartCode=*/true);

    std::vector<NalType> types;
    std::vector<size_t> lens;
    forEachNal(bs.data(), bs.size(), [&](const NalUnit& u) {
        types.push_back(u.type);
        lens.push_back(u.len);
    });

    REQUIRE(types.size() == 3);
    CHECK(types[0] == NalType::Sps);
    CHECK(types[1] == NalType::Pps);
    CHECK(types[2] == NalType::IdrSlice);
    CHECK_EQ(lens[0], std::size_t(3));   // header + 2 body bytes
    CHECK_EQ(lens[1], std::size_t(2));   // header + 1 body
    CHECK_EQ(lens[2], std::size_t(4));   // header + 3 body

    CHECK(isParameterSet(types[0]));
    CHECK(isParameterSet(types[1]));
    CHECK(isPictureSlice(types[2]));
    CHECK(isKeyframe(types[2]));
    CHECK(!isKeyframe(types[0]));
}

TEST(H264Nal, KeyframeVsInterFrame) {
    std::vector<uint8_t> idr, inter;
    addNal(idr, hdr(NalType::IdrSlice), {0x11});
    addNal(inter, hdr(NalType::NonIdrSlice), {0x22});

    bool sawKey = false, sawInter = false;
    forEachNal(idr.data(), idr.size(), [&](const NalUnit& u) { sawKey = isKeyframe(u.type); });
    forEachNal(inter.data(), inter.size(),
               [&](const NalUnit& u) { sawInter = isPictureSlice(u.type) && !isKeyframe(u.type); });
    CHECK(sawKey);
    CHECK(sawInter);
}

TEST(H264Nal, HandlesLeadingGarbageAndEmpty) {
    // No start code at all -> no NALs, no crash.
    std::vector<uint8_t> junk = {0x12, 0x34, 0x56};
    int count = 0;
    forEachNal(junk.data(), junk.size(), [&](const NalUnit&) { ++count; });
    CHECK_EQ(count, 0);

    forEachNal(nullptr, 0, [&](const NalUnit&) { ++count; });
    CHECK_EQ(count, 0);

    // Leading non-start-code bytes before the first start code are skipped.
    std::vector<uint8_t> bs = {0xFF, 0xFF};
    addNal(bs, hdr(NalType::Sps), {0x01});
    NalType got = NalType::Unknown;
    forEachNal(bs.data(), bs.size(), [&](const NalUnit& u) { got = u.type; });
    CHECK(got == NalType::Sps);
}

// A realistic access unit: SPS+PPS+IDR concatenated in one media message. The
// app gates "video ready" on seeing a parameter set followed by a keyframe.
TEST(H264Nal, AccessUnitReadinessGate) {
    std::vector<uint8_t> bs;
    addNal(bs, hdr(NalType::Aud), {0x10});
    addNal(bs, hdr(NalType::Sps), {0x64, 0x00, 0x1F});
    addNal(bs, hdr(NalType::Pps), {0xCE, 0x3C});
    addNal(bs, hdr(NalType::Sei), {0x05});
    addNal(bs, hdr(NalType::IdrSlice), {0x88, 0x84});

    bool haveParams = false, ready = false;
    forEachNal(bs.data(), bs.size(), [&](const NalUnit& u) {
        if (isParameterSet(u.type)) haveParams = true;
        if (haveParams && isKeyframe(u.type)) ready = true;
    });
    CHECK(ready);
}
