// ChannelMix host tests: passthrough, mono upmix, stereo fold-down, and the
// normalised ITU multichannel -> stereo downmix the DecodePump applies.
#include "audio/ChannelMix.h"

#include <cmath>
#include <vector>

#include "Test.h"

using carsh::audio::ChannelMix;

TEST(ChannelMix, SupportedCounts) {
    CHECK(!ChannelMix::supported(0));
    for (uint8_t c = 1; c <= 8; ++c) CHECK(ChannelMix::supported(c));
    CHECK(!ChannelMix::supported(9));
    CHECK(!ChannelMix::supported(255));
    ChannelMix m;
    CHECK(!m.configure(0, 2));
    CHECK(!m.configure(9, 2));
    CHECK(!m.configure(2, 0));
    CHECK(!m.configured());
}

TEST(ChannelMix, SameCountIsBitExactCopy) {
    ChannelMix m;
    REQUIRE(m.configure(2, 2));
    const float src[6] = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f};
    float dst[6] = {};
    m.apply(src, dst, 3);
    for (int i = 0; i < 6; ++i) CHECK_EQ(dst[i], src[i]);
}

TEST(ChannelMix, MonoDuplicatedToBothSides) {
    ChannelMix m;
    REQUIRE(m.configure(1, 2));
    const float src[4] = {0.25f, -0.5f, 1.0f, -1.0f};
    float dst[8] = {};
    m.apply(src, dst, 4);
    for (int f = 0; f < 4; ++f) {
        CHECK_EQ(dst[2 * f], src[f]);      // unity, not -3 dB
        CHECK_EQ(dst[2 * f + 1], src[f]);
    }
}

TEST(ChannelMix, MonoToWideDeviceLeavesExtraOutputsSilent) {
    ChannelMix m;
    REQUIRE(m.configure(1, 4));
    const float src[2] = {0.5f, -0.25f};
    float dst[8];
    for (float& v : dst) v = 9.0f;  // must all be overwritten
    m.apply(src, dst, 2);
    CHECK_EQ(dst[0], 0.5f);
    CHECK_EQ(dst[1], 0.5f);
    CHECK_EQ(dst[2], 0.0f);
    CHECK_EQ(dst[3], 0.0f);
    CHECK_EQ(dst[4], -0.25f);
    CHECK_EQ(dst[7], 0.0f);
}

TEST(ChannelMix, StereoToMonoAverages) {
    ChannelMix m;
    REQUIRE(m.configure(2, 1));
    const float src[4] = {0.5f, 0.25f, -1.0f, 1.0f};
    float dst[2] = {};
    m.apply(src, dst, 2);
    CHECK_NEAR(dst[0], 0.375f, 1e-7);
    CHECK_NEAR(dst[1], 0.0f, 1e-7);
}

TEST(ChannelMix, FivePointOneDownmixUsesItuCoefficients) {
    // FL FR FC LFE BL BR: L = FL + k*FC + k*BL, R = FR + k*FC + k*BR (k = -3 dB),
    // LFE dropped, all scaled by 1/(1 + 2k) so nothing can clip.
    ChannelMix m;
    REQUIRE(m.configure(6, 2));
    const double k = std::sqrt(0.5);
    const double norm = 1.0 / (1.0 + 2.0 * k);
    CHECK_NEAR(m.gainL(0), norm, 1e-6);       // FL
    CHECK_NEAR(m.gainR(0), 0.0, 1e-9);
    CHECK_NEAR(m.gainR(1), norm, 1e-6);       // FR
    CHECK_NEAR(m.gainL(2), k * norm, 1e-6);   // FC both sides
    CHECK_NEAR(m.gainR(2), k * norm, 1e-6);
    CHECK_NEAR(m.gainL(3), 0.0, 1e-9);        // LFE
    CHECK_NEAR(m.gainR(3), 0.0, 1e-9);
    CHECK_NEAR(m.gainL(4), k * norm, 1e-6);   // BL -> L only
    CHECK_NEAR(m.gainR(4), 0.0, 1e-9);
    CHECK_NEAR(m.gainR(5), k * norm, 1e-6);   // BR -> R only

    const float frame[6] = {0.1f, 0.2f, 0.3f, 0.9f, 0.4f, 0.5f};
    float out[2] = {};
    m.apply(frame, out, 1);
    CHECK_NEAR(out[0], (0.1 + k * 0.3 + k * 0.4) * norm, 1e-6);
    CHECK_NEAR(out[1], (0.2 + k * 0.3 + k * 0.5) * norm, 1e-6);
}

TEST(ChannelMix, FullScaleOnEveryInputNeverClips) {
    for (uint8_t in = 1; in <= ChannelMix::kMaxIn; ++in) {
        ChannelMix m;
        REQUIRE(m.configure(in, 2));
        std::vector<float> src(in, 1.0f), dst(2, 0.0f);
        m.apply(src.data(), dst.data(), 1);
        CHECK(dst[0] <= 1.0f + 1e-6f);
        CHECK(dst[1] <= 1.0f + 1e-6f);
        CHECK(dst[0] > 0.3f);  // but still audible on both sides
        CHECK(dst[1] > 0.3f);
    }
}

TEST(ChannelMix, SevenPointOneFoldsSidesAndBacks) {
    // FL FR FC LFE BL BR SL SR: each side gets its back + side channel at -3 dB.
    ChannelMix m;
    REQUIRE(m.configure(8, 2));
    const float leftOnly[8] = {0, 0, 0, 0, 1, 0, 1, 0};  // BL + SL
    float out[2] = {};
    m.apply(leftOnly, out, 1);
    CHECK(out[0] > 0.2f);
    CHECK_NEAR(out[1], 0.0f, 1e-7);
}
