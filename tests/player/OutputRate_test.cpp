// OutputRate — the PWM jack's rate follows the audio being played (the session
// owner, or live remote audio over it), not merely what is connected (final
// review: with a phone connected, local tracks played at the phone's rate).
#include "player/OutputRate.h"

#include "Test.h"
#include "player/MediaSession.h"

using carsh::player::chooseOutputRate;
using carsh::player::MediaSession;
using carsh::player::OutputRate;
using carsh::player::OutputRateInputs;
using carsh::player::Source;

namespace {
// A 48 kHz local track playing with an idle 44.1 kHz phone connected.
OutputRateInputs localWithIdlePhone() {
    OutputRateInputs in;
    in.owner = Source::Local;
    in.btConnected = true;
    in.btRate = 44100;
    in.localTrack = true;
    in.localRate = 48000;
    return in;
}
}  // namespace

TEST(OutputRate, LocalTrackRateWinsOverAConnectedIdlePhone) {
    OutputRate r = chooseOutputRate(localWithIdlePhone());
    CHECK_EQ(r.hz, uint32_t(48000));
    CHECK(r.source == Source::Local);

    // A queue 44.1 -> 48 -> 44.1 kHz reclocks at every change with the phone up.
    OutputRateInputs in = localWithIdlePhone();
    for (uint32_t rate : {44100u, 48000u, 44100u}) {
        in.localRate = rate;
        CHECK_EQ(chooseOutputRate(in).hz, rate);
    }
    // Also with a connected AA session that isn't playing.
    in.aaConnected = true;
    in.localRate = 44100;
    CHECK_EQ(chooseOutputRate(in).hz, uint32_t(44100));
}

TEST(OutputRate, LocalRateFallsBackTo48kWhenUnknown) {
    OutputRateInputs in;
    in.localTrack = true;
    CHECK_EQ(chooseOutputRate(in).hz, uint32_t(48000));
}

TEST(OutputRate, RemoteOwnerKeepsItsRateWhilePaused) {
    OutputRateInputs in = localWithIdlePhone();
    in.owner = Source::Bluetooth;  // latched, paused: no audio arriving
    OutputRate r = chooseOutputRate(in);
    CHECK_EQ(r.hz, uint32_t(44100));
    CHECK(r.source == Source::Bluetooth);
    in.btRate = 0;  // no SBC frame decoded yet
    CHECK_EQ(chooseOutputRate(in).hz, uint32_t(44100));

    in.owner = Source::AndroidAuto;
    in.aaConnected = true;
    r = chooseOutputRate(in);
    CHECK_EQ(r.hz, uint32_t(48000));
    CHECK(r.source == Source::AndroidAuto);
}

// A chime or prompt plays over the local track in the mix; it must play at its
// own rate while it lasts, then the local rate comes back.
TEST(OutputRate, LiveRemoteAudioPlaysAtItsOwnRate) {
    OutputRateInputs in = localWithIdlePhone();
    in.btAudible = true;
    CHECK_EQ(chooseOutputRate(in).hz, uint32_t(44100));
    in.btAudible = false;
    CHECK_EQ(chooseOutputRate(in).hz, uint32_t(48000));

    in.localRate = 44100;
    in.aaConnected = true;
    in.aaAudible = true;  // an AA prompt: its jitter buffer engaged
    CHECK_EQ(chooseOutputRate(in).hz, uint32_t(48000));
    in.btAudible = true;  // both: AA outranks A2DP, as in the mix
    CHECK(chooseOutputRate(in).source == Source::AndroidAuto);
}

// A tail left in the AA ring below the jitter buffer's prime is never played, so
// it no longer pins the jack at 48 kHz (it did when any buffered AA audio counted).
TEST(OutputRate, UnplayedAaTailDoesNotPinTheRate) {
    OutputRateInputs in = localWithIdlePhone();
    in.btConnected = false;
    in.aaConnected = true;
    in.localRate = 44100;
    in.aaAudible = false;
    CHECK_EQ(chooseOutputRate(in).hz, uint32_t(44100));
}

TEST(OutputRate, ConnectivityOnlyWhenNothingOwnsTheOutput) {
    OutputRateInputs in;
    CHECK_EQ(chooseOutputRate(in).hz, uint32_t(0));  // nothing: leave the jack alone
    in.btConnected = true;
    in.btRate = 48000;
    CHECK_EQ(chooseOutputRate(in).hz, uint32_t(48000));
    in.aaConnected = true;
    CHECK_EQ(chooseOutputRate(in).hz, uint32_t(48000));
    CHECK(chooseOutputRate(in).source == Source::AndroidAuto);
}

// End to end with the session: the user picks a 48 kHz local track while a 44.1 kHz
// phone plays. The phone is paused, and the silence tail it keeps sending for a
// few seconds does not hold the jack at 44.1 kHz; a prompt afterwards does, while
// it plays.
TEST(OutputRate, FollowsTheSessionAcrossATakeBack) {
    MediaSession s;
    uint64_t t = 0;
    carsh::bt::AvrcpMetadata md;
    md.title = "Phone Song";
    md.playing = true;
    MediaSession::LocalState local;
    local.hasTrack = true;
    local.title = "Song A";
    s.setLocal(local);
    s.setBluetooth(true, true, md, true, t += 33000);
    auto rate = [&] {
        OutputRateInputs in;
        in.owner = s.source();
        in.btAudible = s.audible(Source::Bluetooth);
        in.btConnected = true;
        in.btRate = 44100;
        in.localTrack = true;
        in.localRate = 48000;
        return chooseOutputRate(in).hz;
    };
    CHECK_EQ(rate(), uint32_t(44100));  // the phone owns and plays

    s.selectLocal();
    local.playing = true;
    s.setLocal(local);
    md.playing = false;  // the pause lands; A2DP silence keeps arriving
    for (int i = 0; i < 90; ++i) {
        s.setBluetooth(true, true, md, true, t += 33000);
        CHECK_EQ(rate(), uint32_t(48000));
    }
    s.setBluetooth(true, false, md, true, t += 33000);  // the tail ends
    CHECK_EQ(rate(), uint32_t(48000));
    s.setBluetooth(true, true, md, true, t += 33000);   // a navigation prompt
    CHECK_EQ(rate(), uint32_t(44100));
    CHECK(s.source() == Source::Local);                 // ...which takes nothing
}
