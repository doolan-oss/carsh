// OutputRate — which sample rate the output (the PWM jack) must run at.
//
// carsh has no resampler: the jack has to clock at the rate of the audio it is
// playing, or that audio comes out at the wrong pitch (44.1 kHz material on a
// 48 kHz clock is 8.8 % fast). The output mixes three sources (kernel BtMixSink):
// Android Auto (48 kHz) whenever its jitter buffer is engaged, then A2DP whenever
// a block of it is buffered or Bluetooth owns the session, then the local player.
//
// The rate used to be chosen by what was CONNECTED (AA audio buffered, else a BT
// phone connected, else the local track), so with a phone merely connected every
// local track played at the phone's rate, and a non-gapless restart at a new rate
// never reclocked (final review). It now follows the audio actually being played:
//   1. Live remote audio wins, as it does in the mix, so a chime or a prompt from
//      a phone that doesn't own the session still plays at its own rate: AA while
//      its jitter buffer is engaged, then A2DP while it arrives (not the tail of a
//      stream that local playback took over from — MediaSession::audible()).
//   2. Otherwise the session owner (MediaSession::source()): AA 48 kHz, BT the
//      A2DP stream's rate, local the rate of the decoder being heard
//      (PlayerState::sampleRate, then the library row, then 48 kHz). A paused
//      remote owner keeps its rate, so resuming it doesn't reclock.
//   3. Only when nothing owns the output: what is connected.
// Pure, host-tested (tests/player/OutputRate_test).
#pragma once

#include <cstdint>

#include "player/MediaSession.h"

namespace carsh::player {

struct OutputRateInputs {
    Source owner = Source::Local;  // MediaSession::source() this frame
    bool aaAudible = false;        // AA audio is being output (jitter buffer engaged)
    bool btAudible = false;        // A2DP audio arriving (MediaSession::audible(Bluetooth))
    bool aaConnected = false;      // a projection session is up
    bool btConnected = false;      // a phone is linked
    uint32_t btRate = 0;           // the A2DP stream's rate, 0 = not known yet
    bool localTrack = false;       // the local player has a current queue entry
    uint32_t localRate = 0;        // that entry's rate as heard (see above), 0 = unknown
};

struct OutputRate {
    uint32_t hz = 0;               // 0: nothing needs the output (leave it as it is)
    Source source = Source::Local; // whose audio the rate is for (logging)
};

constexpr uint32_t kAaOutputRate = 48000;     // AA media audio: 48 kHz stereo
constexpr uint32_t kBtDefaultRate = 44100;    // A2DP before the first SBC frame
constexpr uint32_t kLocalDefaultRate = 48000; // a local track of unknown rate

inline OutputRate chooseOutputRate(const OutputRateInputs& in) {
    const OutputRate aa{kAaOutputRate, Source::AndroidAuto};
    const OutputRate bt{in.btRate ? in.btRate : kBtDefaultRate, Source::Bluetooth};
    // 1. Live remote audio, in the mix's priority order.
    if (in.aaAudible) return aa;
    if (in.btAudible) return bt;
    // 2. The session owner.
    switch (in.owner) {
        case Source::AndroidAuto: return aa;
        case Source::Bluetooth: return bt;
        case Source::Local: break;
    }
    if (in.localTrack)
        return {in.localRate ? in.localRate : kLocalDefaultRate, Source::Local};
    // 3. Nothing owns the output.
    if (in.aaConnected) return aa;
    if (in.btConnected) return bt;
    return {};
}

}  // namespace carsh::player
