// AbsoluteVolume tests: the phone's AVRCP absolute volume (SetAbsoluteVolume,
// 0..0x7F) as a gain on the A2DP PCM. BTstack v1.6.1's TG accepts absolute volume
// from any phone, so the glue applies it (see bt/AbsoluteVolume.h). These tests pin
// down the curve, the reserved bit, the unity default and the click-free ramp.
#include "bt/AbsoluteVolume.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "Test.h"

using carsh::bt::AbsoluteVolume;

namespace {
float toDb(float gain) { return 20.0f * std::log10(gain); }
}  // namespace

TEST(AbsoluteVolume, DefaultIsUnitySoANonAbsoluteVolumePhoneIsUntouched) {
    AbsoluteVolume v;
    CHECK_EQ(v.volume(), uint8_t(0x7F));
    CHECK_EQ(v.gain(), 1.0f);
    // A phone that never sends SetAbsoluteVolume scales its own PCM, and carsh
    // passes it through bit-exact (int16 -> float).
    const int16_t in[4] = {32767, -32768, 16384, -1};
    float out[4] = {};
    v.apply(in, out, 2, 2);
    CHECK_EQ(out[0], 32767.0f / 32768.0f);
    CHECK_EQ(out[1], -1.0f);
    CHECK_EQ(out[2], 0.5f);
    CHECK_EQ(out[3], -1.0f / 32768.0f);
}

TEST(AbsoluteVolume, EndpointsAreMuteAndUnity) {
    CHECK_EQ(AbsoluteVolume::gainFor(0), 0.0f);
    CHECK_EQ(AbsoluteVolume::gainFor(0x7F), 1.0f);
}

TEST(AbsoluteVolume, CurveIsStrictlyIncreasing) {
    float prev = AbsoluteVolume::gainFor(0);
    for (int v = 1; v <= 0x7F; ++v) {
        const float g = AbsoluteVolume::gainFor(uint8_t(v));
        CHECK(g > prev);
        CHECK(g <= 1.0f);
        prev = g;
    }
}

TEST(AbsoluteVolume, CurveFollowsTheMediaVolumeShapeInDb) {
    // Not linear in amplitude: the lowest non-zero step is quiet, and half-scale
    // is well below -6 dB. It is interpolated in dB between -58 dB at 1 %,
    // -40 dB at 20 %, -17 dB at 60 % and 0 dB at 100 %.
    CHECK_NEAR(toDb(AbsoluteVolume::gainFor(1)), -58.0f, 0.01f);   // 0.8 %: clamped
    // Android maps step 1 of 15 to 127 / 15 = 8 (6.3 %).
    CHECK_NEAR(toDb(AbsoluteVolume::gainFor(8)), -53.0f, 0.5f);
    CHECK_NEAR(toDb(AbsoluteVolume::gainFor(64)), -22.5f, 0.2f);   // 50.4 %
    CHECK_NEAR(toDb(AbsoluteVolume::gainFor(76)), -17.0f, 0.2f);   // 59.8 %
    CHECK_NEAR(toDb(AbsoluteVolume::gainFor(126)), -0.34f, 0.05f);
}

TEST(AbsoluteVolume, ReservedBitIsIgnored) {
    AbsoluteVolume v;
    v.set(0x80 | 0x40);
    CHECK_EQ(v.volume(), uint8_t(0x40));
    CHECK_EQ(v.gain(), AbsoluteVolume::gainFor(0x40));
    CHECK_EQ(AbsoluteVolume::gainFor(0xFF), 1.0f);
    CHECK_EQ(AbsoluteVolume::gainFor(0x80), 0.0f);
}

TEST(AbsoluteVolume, SetAppliesTheGainAfterARampBlock) {
    AbsoluteVolume v;
    v.set(0x40);
    const float g = AbsoluteVolume::gainFor(0x40);
    std::vector<int16_t> in(2 * 128, 16384);  // stereo, 0.5 full scale
    std::vector<float> out(in.size());
    v.apply(in.data(), out.data(), 128, 2);  // ramp block
    v.apply(in.data(), out.data(), 128, 2);  // steady state
    for (float s : out) CHECK_NEAR(s, 0.5f * g, 1e-6f);
}

TEST(AbsoluteVolume, AVolumeStepRampsAcrossOneBlockWithoutAJump) {
    AbsoluteVolume v;  // unity
    v.set(0);          // phone muted
    const std::size_t frames = 64;
    std::vector<int16_t> in(frames * 2, 32767);
    std::vector<float> out(in.size());
    v.apply(in.data(), out.data(), frames, 2);
    // Starts one step below unity, ends exactly at the new gain, and falls
    // monotonically with L and R on the same gain in every frame.
    CHECK(out[0] < 1.0f && out[0] > 0.95f);
    CHECK_EQ(out[2 * frames - 1], 0.0f);
    for (std::size_t f = 0; f < frames; ++f) {
        CHECK_EQ(out[2 * f], out[2 * f + 1]);
        if (f > 0) CHECK(out[2 * f] < out[2 * (f - 1)]);
    }
    // Next block: settled at mute.
    v.apply(in.data(), out.data(), frames, 2);
    for (float s : out) CHECK_EQ(s, 0.0f);
}

TEST(AbsoluteVolume, ResetReturnsToUnity) {
    // AVRCP released or the phone dropped: without AVRCP the phone scales its PCM
    // itself again, so a leftover attenuation here would make it doubly quiet.
    AbsoluteVolume v;
    v.set(0x10);
    std::vector<int16_t> in(2 * 32, 8192);
    std::vector<float> out(in.size());
    v.apply(in.data(), out.data(), 32, 2);
    v.reset();
    CHECK_EQ(v.volume(), uint8_t(0x7F));
    CHECK_EQ(v.gain(), 1.0f);
    v.apply(in.data(), out.data(), 32, 2);  // ramp back up
    v.apply(in.data(), out.data(), 32, 2);
    for (float s : out) CHECK_EQ(s, 0.25f);
}

TEST(AbsoluteVolume, EmptyBlockKeepsThePendingRamp) {
    AbsoluteVolume v;
    v.set(0);
    v.apply(nullptr, nullptr, 0, 2);  // must not touch buffers or skip the ramp
    const int16_t in[2] = {32767, 32767};
    float out[2] = {};
    v.apply(in, out, 1, 2);  // one-frame ramp lands on the target
    CHECK_EQ(out[0], 0.0f);
}
