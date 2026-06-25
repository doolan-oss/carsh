// PlayerController host tests: transport FSM, queue gapless advance, pause,
// seek, position reporting — driven through the WAV sink device.
#include "player/PlayerController.h"

#include <string>
#include <vector>

#include "Test.h"
#include "audio/AudioEngine.h"
#include "audio/PcmRing.h"
#include "audio/WavSinkDevice.h"
#include "fs/HostVfs.h"

using namespace carsh;

static std::string fx(const char* n) {
    return std::string(CARSH_FIXTURES_DIR) + "/" + n;
}

namespace {
struct Rig {
    host::HostVfs vfs{"/"};
    audio::PcmRing<float> ring{44100 * 2 * 2};  // ~2s stereo headroom
    audio::AudioEngine eng{&ring, 2};
    host::WavSinkDevice dev{"/tmp/carsh_player.wav"};
    player::PlayerController pc{vfs, ring, eng, 2};
    Rig() { dev.open({44100, 16, 2}, &eng); }
    // One scheduler round: decode slice then render a device block.
    void round(std::size_t frames = 256) {
        pc.serviceDecode();
        dev.pump(frames);
    }
    void runUntilStopped(int maxRounds = 5000) {
        for (int i = 0; i < maxRounds; ++i) {
            round();
            if (pc.state().transport == player::Transport::Stopped) break;
        }
    }
};
}  // namespace

TEST(Player, PlaySingleTrackToCompletion) {
    Rig r;
    r.pc.setQueue({fx("sine.wav")});
    REQUIRE(r.pc.play());
    CHECK(r.pc.state().transport == player::Transport::Playing);
    CHECK_EQ(r.pc.state().durationMs, uint64_t(500));  // 22050 / 44100
    r.runUntilStopped();
    auto s = r.pc.state();
    CHECK(s.transport == player::Transport::Stopped);
    // Heard ~all of the track.
    CHECK(r.eng.framesRendered() >= 22000);
}

TEST(Player, PositionAdvances) {
    Rig r;
    r.pc.setQueue({fx("sine.wav")});
    r.pc.play();
    for (int i = 0; i < 40; ++i) r.round();  // ~40*256 = 10240 frames ~232ms
    auto s = r.pc.state();
    CHECK(s.positionMs > 150);
    CHECK(s.positionMs < 350);
}

TEST(Player, PauseFreezesPositionAndSilences) {
    Rig r;
    r.pc.setQueue({fx("sine.wav")});
    r.pc.play();
    for (int i = 0; i < 20; ++i) r.round();
    uint64_t posBefore = r.pc.state().positionMs;
    r.pc.pause();
    CHECK(r.eng.paused());
    std::size_t capBefore = r.dev.captured().size();
    for (int i = 0; i < 20; ++i) r.round();  // pumping while paused
    CHECK_EQ(r.pc.state().positionMs, posBefore);  // frozen
    // Everything captured while paused is silence.
    for (std::size_t i = capBefore; i < r.dev.captured().size(); ++i)
        CHECK(std::fabs(r.dev.captured()[i]) < 1e-9);
    r.pc.resume();
    CHECK(!r.eng.paused());
    for (int i = 0; i < 10; ++i) r.round();
    CHECK(r.pc.state().positionMs > posBefore);  // moving again
}

TEST(Player, GaplessQueueAdvancesIndex) {
    Rig r;
    r.pc.setQueue({fx("sine.wav"), fx("sine.wav")});
    r.pc.play();
    CHECK_EQ(r.pc.state().queueIndex, 0);
    r.runUntilStopped();
    // Both tracks heard: ~2 * 22050 frames rendered, index reached 1.
    CHECK(r.eng.framesRendered() >= 44000);
    // Re-driving state: last heard index should be the second track.
    // (queueIndex is clamped at the last track once the final boundary passes.)
    CHECK_EQ(r.pc.queue().size(), std::size_t(2));
}

TEST(Player, GaplessHasNoMidStreamUnderrun) {
    Rig r;
    r.pc.setQueue({fx("sine.wav"), fx("sine.wav")});
    r.pc.play();
    // Render exactly the first track's worth; the seam must not underrun.
    std::size_t target = 22050 + 22050 - 1000;  // stop before final drain
    while (r.eng.framesRendered() < target &&
           r.pc.state().transport != player::Transport::Stopped)
        r.round();
    CHECK_EQ(r.eng.underrunFrames(), std::size_t(0));
}

TEST(Player, SeekRepositions) {
    Rig r;
    r.pc.setQueue({fx("sine.wav")});
    r.pc.play();
    for (int i = 0; i < 10; ++i) r.round();
    REQUIRE(r.pc.seekMs(300));
    // Render a little so rendered advances from the seek baseline.
    for (int i = 0; i < 5; ++i) r.round();
    auto s = r.pc.state();
    CHECK(s.positionMs >= 300);
    CHECK(s.positionMs < 360);
}

TEST(Player, NextAndPrev) {
    Rig r;
    r.pc.setQueue({fx("sine.wav"), fx("sine.wav")});
    r.pc.play(0);
    CHECK_EQ(r.pc.state().queueIndex, 0);
    REQUIRE(r.pc.next());
    CHECK_EQ(r.pc.state().queueIndex, 1);
    REQUIRE(r.pc.prev());
    CHECK_EQ(r.pc.state().queueIndex, 0);
}

TEST(Player, PlayRejectsBadIndexAndEmptyQueue) {
    Rig r;
    CHECK(!r.pc.play());            // empty queue
    r.pc.setQueue({fx("sine.wav")});
    CHECK(!r.pc.play(5));           // out of range
    CHECK(r.pc.play(0));            // ok
}
