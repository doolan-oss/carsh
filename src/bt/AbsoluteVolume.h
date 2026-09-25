// AbsoluteVolume — AVRCP absolute volume for the A2DP sink: the phone's volume
// (SetAbsoluteVolume, 0..0x7F) becomes a gain on the decoded A2DP PCM.
//
// Why carsh implements it instead of hiding it. The glue registers an AVRCP
// Target SDP record (Category 2, monitor/amplifier), as the BTstack
// a2dp_sink_demo does. A phone decides to use absolute volume from that record:
// Android sets its "advanced control" feature from the Category-2 bit of the
// peer's TG record and then sends RegisterNotification(VOLUME_CHANGED). If the TG
// answers INTERIM, the phone stops scaling its PCM and sends SetAbsoluteVolume
// instead. BTstack v1.6.1's TG answers that INTERIM and accepts SetAbsoluteVolume
// unconditionally, whatever events the app declares (avrcp_target.c
// REGISTER_NOTIFICATION / SET_ABSOLUTE_VOLUME), and the record's AVRCP version is
// fixed at 1.6. So carsh cannot reliably opt out: clearing the Category-2 bit
// would misdescribe a car audio sink and would still leave any phone that
// registers regardless with inert volume keys. Applying the volume works for every
// phone: with absolute volume the keys act here, and without it the gain stays
// at unity and the phone scales its own PCM as before.
//
// The gain is a per-phone pre-attenuation. The HU master rail
// (kernel.cpp g_masterGain) still applies after it, as it did when the phone
// scaled the PCM. Platform-independent and host-tested
// (tests/bt/AbsoluteVolume_test); no BTstack types here.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace carsh::bt {

class AbsoluteVolume {
public:
    // AVRCP absolute volume range: 0x00 = 0 %, 0x7F = 100 %. Bit 7 is reserved.
    static constexpr uint8_t kMax = 0x7F;

    // AVRCP volume -> linear gain: 0 mutes, kMax is unity, and bit 7 is ignored.
    // AVRCP leaves the curve to the TG. Phones map their volume steps linearly onto
    // 0..0x7F (Android: step * 127 / maxStep), so a linear gain would squeeze all
    // the audible range into the top few steps. This follows the shape of
    // Android's default media volume curve (-58 dB at 1 %, -40 dB at 20 %,
    // -17 dB at 60 %, 0 dB at 100 %), interpolated in dB. The phone's volume steps
    // then sound as they did when the phone scaled the PCM itself.
    static float gainFor(uint8_t avrcpVolume) {
        const uint8_t v = avrcpVolume & kMax;
        if (v == 0) return 0.0f;
        if (v == kMax) return 1.0f;
        struct Point {
            float percent;
            float db;
        };
        static constexpr Point kCurve[] = {{1.0f, -58.0f}, {20.0f, -40.0f},
                                           {60.0f, -17.0f}, {100.0f, 0.0f}};
        constexpr std::size_t kPoints = sizeof(kCurve) / sizeof(kCurve[0]);
        const float percent = float(v) * 100.0f / float(kMax);
        float db = kCurve[0].db;  // below the first point: its level
        for (std::size_t i = 1; i < kPoints; ++i) {
            if (percent <= kCurve[i].percent) {
                const Point& a = kCurve[i - 1];
                const Point& b = kCurve[i];
                const float t = percent <= a.percent
                                    ? 0.0f
                                    : (percent - a.percent) / (b.percent - a.percent);
                db = a.db + t * (b.db - a.db);
                break;
            }
        }
        return std::pow(10.0f, db / 20.0f);
    }

    // The phone set a new absolute volume. The next apply() ramps to it.
    void set(uint8_t avrcpVolume) {
        volume_ = uint8_t(avrcpVolume & kMax);
        target_ = gainFor(volume_);
    }

    // No absolute volume in force: a new AVRCP session, or the connected phone's
    // link dropped. Unity gain, because the phone scales its own PCM then. This is
    // also the volume carsh reports in its VOLUME_CHANGED INTERIM. (An AVRCP-only
    // release keeps the gain: the phone stays in absolute-volume mode for its
    // A2DP stream.)
    void reset() { set(kMax); }

    uint8_t volume() const { return volume_; }
    float gain() const { return target_; }

    // Convert one decoded block of interleaved int16 PCM to float [-1, 1) with the
    // gain applied. A gain change ramps linearly across this block (per frame, so
    // L and R stay matched) so that a volume step does not click.
    void apply(const int16_t* in, float* out, std::size_t frames, unsigned channels) {
        if (frames == 0 || channels == 0) return;
        constexpr float kScale = 1.0f / 32768.0f;
        const float g0 = current_;
        const float g1 = target_;
        if (g0 == g1) {
            const float k = g1 * kScale;
            for (std::size_t i = 0; i < frames * channels; ++i) out[i] = float(in[i]) * k;
        } else {
            const float step = (g1 - g0) / float(frames);
            for (std::size_t f = 0; f < frames; ++f) {
                const float k = (g0 + step * float(f + 1)) * kScale;
                for (unsigned c = 0; c < channels; ++c) {
                    const std::size_t i = f * channels + c;
                    out[i] = float(in[i]) * k;
                }
            }
        }
        current_ = g1;
    }

private:
    uint8_t volume_ = kMax;
    float target_ = 1.0f;   // gain for volume_
    float current_ = 1.0f;  // gain at the end of the last apply() (ramp start)
};

}  // namespace carsh::bt
