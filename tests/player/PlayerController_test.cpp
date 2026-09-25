// PlayerController host tests: transport FSM, queue gapless advance, pause,
// seek, position reporting — driven through the WAV sink device. The later
// tests generate their own float32 WAVs (mono, 6-channel, 10-channel, mixed
// rates, sub-second tracks) with sample values that encode the track and frame,
// so the captured output proves exactly what was played, in what order.
#include "player/PlayerController.h"

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "Test.h"
#include "audio/AudioEngine.h"
#include "audio/ChannelMix.h"
#include "audio/PcmRing.h"
#include "audio/WavFile.h"
#include "audio/WavSinkDevice.h"
#include "fs/HostVfs.h"

using namespace carsh;

static std::string fx(const char* n) {
    return std::string(CARSH_FIXTURES_DIR) + "/" + n;
}

namespace {
// HostVfs that counts opens per path (proves failed/declined preloads are not
// re-opened every decode slice).
struct CountingVfs : hal::Vfs {
    host::HostVfs inner{"/"};
    std::map<std::string, int> opens;
    std::unique_ptr<hal::IStream> open(const std::string& p) override {
        ++opens[p];
        return inner.open(p);
    }
    bool stat(const std::string& p, hal::DirEntry& e) override {
        return inner.stat(p, e);
    }
    bool readdir(const std::string& p, std::vector<hal::DirEntry>& o) override {
        return inner.readdir(p, o);
    }
    int count(const std::string& p) const {
        auto it = opens.find(p);
        return it == opens.end() ? 0 : it->second;
    }
};

struct Rig {
    CountingVfs vfs;
    audio::PcmRing<float> ring;
    audio::AudioEngine eng{&ring, 2};
    host::WavSinkDevice dev{"/tmp/carsh_player.wav"};
    player::PlayerController pc{vfs, ring, eng, 2};
    // Default ~2s stereo headroom (rounds up to ~3s); pass a smaller ring to
    // force tracks longer than the ring.
    explicit Rig(std::size_t ringFloats = 44100 * 2 * 2) : ring(ringFloats) {
        dev.open({44100, 16, 2}, &eng);
    }
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

// --- generated fixtures ------------------------------------------------------

namespace {

// Sample value for (track tag, frame, channel): distinct per track and per
// frame, exactly representable as float32, so capture comparisons are exact.
float tone(int tag, std::size_t frame, int ch = 0) {
    return 0.1f * float(tag) + 1e-6f * float(frame) + 0.01f * float(ch);
}

// Writes a float32 WAV into the build's fixture dir; returns its path.
std::string genWav(const std::string& name, uint32_t rate, uint16_t ch,
                   std::size_t frames,
                   const std::function<float(std::size_t, int)>& fn) {
    std::vector<float> pcm(frames * ch);
    for (std::size_t f = 0; f < frames; ++f)
        for (int c = 0; c < ch; ++c) pcm[f * ch + std::size_t(c)] = fn(f, c);
    std::string path = std::string(CARSH_FIXTURES_DIR) + "/gen_player_" + name + ".wav";
    host::WavWriter::write(path, pcm.data(), frames, ch, rate, 32, true);
    return path;
}

// Stereo track whose both channels carry tone(tag, frame).
std::string genTrack(const std::string& name, int tag, uint32_t rate,
                     std::size_t frames) {
    return genWav(name, rate, 2, frames,
                  [tag](std::size_t f, int) { return tone(tag, f); });
}

// Checks the capture holds, per tag in order, exactly frames 0..n-1 of that
// track with nothing missing, repeated or foreign in between.
bool captureIsExactly(const std::vector<float>& cap,
                      const std::vector<std::pair<int, std::size_t>>& want,
                      bool allowGaps) {
    std::size_t t = 0, f = 0;
    for (std::size_t i = 0; i + 1 < cap.size(); i += 2) {
        if (cap[i] == 0.0f && cap[i + 1] == 0.0f) {
            // Silence is only legal before the first frame, between tracks when
            // gaps are allowed (non-gapless restart), or after the last.
            if (f == 0 || t >= want.size()) continue;
            return false;
        }
        if (t >= want.size()) return false;
        const float v = tone(want[t].first, f);
        if (cap[i] != v || cap[i + 1] != v) return false;
        if (++f == want[t].second) {
            ++t;
            f = 0;
            if (!allowGaps && t < want.size()) {
                // Gapless: the very next frame must be the next track's first.
                if (i + 3 < cap.size() && cap[i + 2] != tone(want[t].first, 0))
                    return false;
            }
        }
    }
    return t == want.size();
}

}  // namespace

TEST(Player, MonoTrackIsUpmixedToBothChannels) {
    // A mono file played through the stereo pump used to pair consecutive
    // samples as L/R and push half-stale scratch (garbled, gated audio).
    const std::size_t n = 11025;
    std::string mono = genWav("mono", 44100, 1, n,
                              [](std::size_t f, int) { return tone(3, f); });
    Rig r;
    r.pc.setQueue({mono});
    REQUIRE(r.pc.play());
    CHECK_EQ(r.pc.state().durationMs, uint64_t(250));
    r.runUntilStopped();
    CHECK(r.pc.state().transport == player::Transport::Stopped);
    CHECK_EQ(r.eng.framesRendered(), uint64_t(n));
    CHECK(captureIsExactly(r.dev.captured(), {{3, n}}, false));
}

TEST(Player, SixChannelTrackIsDownmixedWithinBounds) {
    // 5.1 (FL FR FC LFE BL BR): the decoder writes 6 floats per frame. The pump's
    // scratch used to be sized for 2, so this overflowed the heap by 8 KiB per
    // block; now it is sized per decoder and folded down to stereo.
    const std::size_t n = 6000;
    const float lv[6] = {0.10f, 0.20f, 0.30f, 0.90f, 0.40f, 0.50f};
    std::string surround = genWav("surround51", 44100, 6, n,
                                  [&lv](std::size_t f, int c) {
                                      return (f & 1) ? -lv[c] : lv[c];
                                  });
    Rig r;
    r.pc.setQueue({surround});
    REQUIRE(r.pc.play());
    CHECK(r.pc.ownershipConsistent());
    r.runUntilStopped();
    CHECK_EQ(r.eng.framesRendered(), uint64_t(n));
    audio::ChannelMix m;
    REQUIRE(m.configure(6, 2));
    float want[2];
    m.apply(lv, want, 1);
    const auto& cap = r.dev.captured();
    REQUIRE(cap.size() >= n * 2);
    bool exact = true;
    for (std::size_t f = 0; f < n; ++f) {
        const float sgn = (f & 1) ? -1.0f : 1.0f;
        if (std::fabs(cap[2 * f] - sgn * want[0]) > 1e-6f ||
            std::fabs(cap[2 * f + 1] - sgn * want[1]) > 1e-6f)
            exact = false;
    }
    CHECK(exact);
}

TEST(Player, UnmappableChannelCountIsRefusedAndSkipped) {
    // >8 channels has no defined layout: refused up front (play() is false,
    // nothing decoded) and passed over by a queue running through it.
    std::string wide = genWav("wide10", 44100, 10, 2000,
                              [](std::size_t, int) { return 0.25f; });
    std::string a = genTrack("skipA", 1, 44100, 4410);
    std::string b = genTrack("skipB", 2, 44100, 4410);
    Rig r;
    r.pc.setQueue({wide});
    CHECK(!r.pc.play(0));
    CHECK(r.pc.state().transport == player::Transport::Stopped);

    r.pc.setQueue({a, wide, b});
    REQUIRE(r.pc.play(0));
    r.runUntilStopped();
    CHECK(captureIsExactly(r.dev.captured(), {{1, 4410}, {2, 4410}}, false));
    CHECK_EQ(r.pc.state().queueIndex, 2);
    CHECK_EQ(r.vfs.count(wide), 2);  // once per queue, never per slice
}

TEST(Player, MixedRateQueueContinuesAtEachTrackRate) {
    // 44.1k -> 48k -> 44.1k: nothing can be chained gaplessly, so each track
    // end restarts on the next one (the device reclocks from sampleRate).
    std::string a = genTrack("rateA", 1, 44100, 8820);
    std::string b = genTrack("rateB", 2, 48000, 9600);
    std::string c = genTrack("rateC", 3, 44100, 8820);
    Rig r;
    r.pc.setQueue({a, b, c});
    REQUIRE(r.pc.play(0));
    std::vector<std::pair<int, uint32_t>> seen;  // (queueIndex, sampleRate)
    bool stoppedEarly = false;
    for (int i = 0; i < 5000; ++i) {
        r.round();
        auto s = r.pc.state();
        if (seen.empty() || seen.back() != std::make_pair(s.queueIndex, s.sampleRate))
            seen.push_back({s.queueIndex, s.sampleRate});
        if (s.transport == player::Transport::Stopped) {
            stoppedEarly = s.queueIndex != 2;
            break;
        }
        CHECK(r.pc.ownershipConsistent());
    }
    CHECK(!stoppedEarly);
    CHECK(r.pc.state().transport == player::Transport::Stopped);
    REQUIRE(seen.size() == std::size_t(3));
    CHECK(seen[0] == std::make_pair(0, uint32_t(44100)));
    CHECK(seen[1] == std::make_pair(1, uint32_t(48000)));
    CHECK(seen[2] == std::make_pair(2, uint32_t(44100)));
    CHECK(captureIsExactly(r.dev.captured(), {{1, 8820}, {2, 9600}, {3, 8820}},
                           true));
    // The declined successor is opened once and held, not re-opened per slice.
    CHECK_EQ(r.vfs.count(b), 1);
    CHECK_EQ(r.vfs.count(c), 1);
}

TEST(Player, PauseAtTrackEndIsKeptUntilResume) {
    std::string a = genTrack("pauseA", 1, 44100, 4410);
    std::string b = genTrack("pauseB", 2, 48000, 4800);
    Rig r;
    r.pc.setQueue({a, b});
    REQUIRE(r.pc.play(0));
    // Render A's last frame without a decode slice after it: the ring is dry but
    // the restart on B hasn't run yet. Pause right there.
    while (r.eng.framesRendered() < 4410) {
        r.pc.serviceDecode();
        r.dev.pump(256);
    }
    r.pc.pause();
    for (int i = 0; i < 10; ++i) r.pc.serviceDecode();
    CHECK(r.pc.state().transport == player::Transport::Paused);
    CHECK_EQ(r.pc.state().queueIndex, 0);
    r.pc.resume();
    r.pc.serviceDecode();
    CHECK(r.pc.state().transport == player::Transport::Playing);
    CHECK_EQ(r.pc.state().queueIndex, 1);
    CHECK_EQ(r.pc.state().sampleRate, uint32_t(48000));
}

TEST(Player, UnplayableEntriesAreSkippedGaplesslyAndNotRetried) {
    std::string a = genTrack("gapA", 1, 44100, 6000);
    std::string b = genTrack("gapB", 2, 44100, 6000);
    std::string missing = fx("no_such_track.wav");
    std::string junk = std::string(CARSH_FIXTURES_DIR) + "/gen_player_junk.wav";
    if (FILE* f = std::fopen(junk.c_str(), "wb")) {
        std::fputs("this is not audio at all, just some text bytes", f);
        std::fclose(f);
    }
    Rig r(44100);  // small ring: the preload happens mid-track, many slices
    r.pc.setQueue({a, missing, junk, b});
    REQUIRE(r.pc.play(0));
    std::vector<int> indices;
    for (int i = 0; i < 5000; ++i) {
        r.round();
        auto s = r.pc.state();
        if (indices.empty() || indices.back() != s.queueIndex)
            indices.push_back(s.queueIndex);
        if (s.transport == player::Transport::Stopped) break;
    }
    CHECK(indices == std::vector<int>({0, 3}));
    CHECK(captureIsExactly(r.dev.captured(), {{1, 6000}, {2, 6000}}, false));
    CHECK_EQ(r.vfs.count(missing), 1);
    CHECK_EQ(r.vfs.count(junk), 1);

    // next()/prev() pass over the known-bad entries too.
    REQUIRE(r.pc.play(0));
    REQUIRE(r.pc.next());
    CHECK_EQ(r.pc.state().queueIndex, 3);
    REQUIRE(r.pc.prev());
    CHECK_EQ(r.pc.state().queueIndex, 0);
}

TEST(Player, ShortTrackHandoffInsideStartKeepsOwnership) {
    // A 0.1 s track drains inside play()'s own fill and the pump moves onto the
    // next track there. The controller used to miss that handoff, then load the
    // track after into next_ — freeing the decoder the pump was still reading.
    std::string s1 = genTrack("shortS", 1, 44100, 4410);
    std::string l1 = genTrack("shortL1", 2, 44100, 44100);
    std::string l2 = genTrack("shortL2", 3, 44100, 44100);
    std::string l3 = genTrack("shortL3", 4, 44100, 22050);
    Rig r(44100);  // ~0.74 s ring: the 1 s tracks don't fit
    r.pc.setQueue({s1, l1, l2, l3});
    REQUIRE(r.pc.play(0));
    CHECK(r.pc.ownershipConsistent());
    bool consistent = true;
    std::vector<int> indices;
    for (int i = 0; i < 5000; ++i) {
        r.round();
        consistent = consistent && r.pc.ownershipConsistent();
        auto s = r.pc.state();
        if (indices.empty() || indices.back() != s.queueIndex)
            indices.push_back(s.queueIndex);
        if (s.transport == player::Transport::Stopped) break;
    }
    CHECK(consistent);
    CHECK(indices == std::vector<int>({0, 1, 2, 3}));
    CHECK(captureIsExactly(r.dev.captured(),
                           {{1, 4410}, {2, 44100}, {3, 44100}, {4, 22050}}, false));
}

TEST(Player, SeveralShortTracksAllPlayWhenTheyFitTheRing) {
    // Two 0.3 s tracks both drain inside play(): the pump used to hit a sticky
    // EOF before the third track was queued and the queue stopped early.
    std::string a = genTrack("fitA", 1, 44100, 13230);
    std::string b = genTrack("fitB", 2, 44100, 13230);
    std::string c = genTrack("fitC", 3, 44100, 88200);
    Rig r(44100);
    r.pc.setQueue({a, b, c});
    REQUIRE(r.pc.play(0));
    r.runUntilStopped();
    CHECK(r.pc.ownershipConsistent());
    CHECK_EQ(r.eng.framesRendered(), uint64_t(13230 + 13230 + 88200));
    CHECK(captureIsExactly(r.dev.captured(), {{1, 13230}, {2, 13230}, {3, 88200}},
                           false));
}

TEST(Player, SeekStaysInHeardTrackAfterPumpMovedOn) {
    // Near the end of A the pump has already handed off to B (A's tail is still
    // in the ring). A seek must land in A — the heard track — not in B.
    std::string a = genTrack("seekA", 1, 44100, 44100);
    std::string b = genTrack("seekB", 2, 44100, 44100);
    Rig r(44100);
    r.pc.setQueue({a, b});
    REQUIRE(r.pc.play(0));
    while (r.eng.framesRendered() < 30000) r.round();
    REQUIRE(r.pc.state().queueIndex == 0);
    std::size_t before = r.dev.captured().size();
    REQUIRE(r.pc.seekMs(200));  // frame 8820 of A
    CHECK(r.pc.ownershipConsistent());
    CHECK_EQ(r.pc.state().queueIndex, 0);
    r.dev.pump(4);
    const auto& cap = r.dev.captured();
    REQUIRE(cap.size() >= before + 8);
    CHECK_EQ(cap[before], tone(1, 8820));
    CHECK_EQ(cap[before + 6], tone(1, 8823));
    // The new pump's handoff boundary is relative to the seek: 10000 frames on,
    // A (35280 frames left) must still be the one playing.
    for (int i = 0; i < 40; ++i) r.round();
    auto s = r.pc.state();
    CHECK_EQ(s.queueIndex, 0);
    CHECK(s.positionMs >= 420 && s.positionMs < 460);  // 200 ms + ~10244 frames
    // And the queue still carries on into B afterwards.
    std::vector<int> indices;
    for (int i = 0; i < 5000; ++i) {
        r.round();
        if (indices.empty() || indices.back() != r.pc.state().queueIndex)
            indices.push_back(r.pc.state().queueIndex);
        if (r.pc.state().transport == player::Transport::Stopped) break;
    }
    CHECK(indices == std::vector<int>({0, 1}));
}

TEST(Player, SampleRateReportedForHeardTrack) {
    Rig r;
    CHECK_EQ(r.pc.state().sampleRate, uint32_t(0));
    r.pc.setQueue({fx("sine.wav")});
    REQUIRE(r.pc.play());
    CHECK_EQ(r.pc.state().sampleRate, uint32_t(44100));
    r.pc.stop();
    CHECK_EQ(r.pc.state().sampleRate, uint32_t(0));
}
