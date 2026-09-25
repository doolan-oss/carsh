// TouchRing -> ScreenManager, the device's touch path end to end: when the ring
// overflows and loses a Down/Up, the UI loop's drain (device/kernel.cpp
// drainTouch) resets ScreenManager's contact tracking so a lost Up can't leave
// the primary-contact latch blocking every later touch. Moves shed under a
// backlog are harmless and must NOT reset the contact in progress.
#include <cstdint>
#include <vector>

#include "Test.h"
#include "hal/TouchRing.h"
#include "ui/AndroidAutoScreen.h"
#include "ui/Screen.h"
#include "ui/Screens.h"

using namespace carsh;
using namespace carsh::ui;
using carsh::hal::StampedTouch;
using carsh::hal::TouchPhase;
using carsh::hal::TouchRing;

namespace {

// 800x480 panel, content rect {52,0,748,432}; tab bar y 432..479.
//   Now (0): '>' (play/pause) [552,642) x [300,360)
//   AA  (1): 800x480 video fit at {66,0,720,432}
struct App {
    ScreenManager mgr{800, 480};
    NowPlayingScreen now{mgr.contentRect()};
    AndroidAutoScreen aa{mgr.contentRect()};
    std::vector<std::uint8_t> frame = std::vector<std::uint8_t>(800 * 480 * 3, 0);
    TouchRing ring;
    int pp = 0, presses = 0, releases = 0, resets = 0;

    App() {
        mgr.addScreen(&now, "Now");
        mgr.addScreen(&aa, "AA");
        now.onPlayPause = [this] { ++pp; };
        aa.onTouchAction = [this](aa::TouchAction a, int, int) {
            if (a == aa::TouchAction::Press) ++presses;
            if (a == aa::TouchAction::Release) ++releases;
        };
        aa.setFrame(frame.data(), 800, 480);
    }

    // The driver pump (timer IRQ on the device). Returns what the ring accepted.
    bool push(TouchPhase ph, int id, int x, int y) {
        StampedTouch s;
        s.ev = {ph, id, x, y, 0};
        return ring.pushEvent(s);
    }

    // The UI loop's drain, as in kernel.cpp drainTouch(). recover=false models the
    // old behaviour (no reset on a lost transition), to show the latch sticking.
    void drain(bool recover = true) {
        StampedTouch s;
        while (ring.pop(s)) mgr.onTouch(s.ev);
        if (ring.lostTransitionSinceLastCheck() && recover) {
            mgr.resetTouch();
            ++resets;
        }
    }

    // A clean tap straight into the manager (the ring is healthy again).
    void tap(int id, int x, int y) {
        push(TouchPhase::Down, id, x, y);
        push(TouchPhase::Up, id, 0, 0);  // the Pi reports Up at (0,0)
        drain();
    }

    // UI stalled while finger 0 drags from (x, y) and a second contact (id 1)
    // taps repeatedly: the ring fills (Moves shed at capacity - kReserve, then the
    // taps take the reserved slots) and finger 0's Up is lost.
    void overflowLosingUpOf0(int x, int y) {
        REQUIRE(push(TouchPhase::Down, 0, x, y));
        int dx = 0;
        while (push(TouchPhase::Move, 0, x + (++dx % 2), y)) {}
        while (ring.size() < TouchRing::capacity()) {
            REQUIRE(push(TouchPhase::Down, 1, 700, 100));
            REQUIRE(push(TouchPhase::Up, 1, 0, 0));
        }
        REQUIRE(!push(TouchPhase::Up, 0, 0, 0));  // lost
        REQUIRE(ring.droppedTransitions() == 1u);
    }
};

constexpr int kPpX = 597, kPpY = 330;  // centre of Now Playing's '>'

}  // namespace

// Control: without the reset, a lost Up keeps the latch on contact 0 and a later
// touch with another id is ignored for good (review T6 robustness note).
TEST(TouchRingRecovery, WithoutResetALostUpBlocksLaterTouches) {
    App app;
    app.overflowLosingUpOf0(300, 200);
    app.drain(/*recover=*/false);
    CHECK(app.mgr.touchActive());
    app.push(TouchPhase::Down, 2, kPpX, kPpY);
    app.push(TouchPhase::Up, 2, 0, 0);
    app.drain(/*recover=*/false);
    CHECK_EQ(app.pp, 0);  // blocked
}

TEST(TouchRingRecovery, LostUpResetsTheLatchSoTheNextTapWorks) {
    App app;
    app.overflowLosingUpOf0(300, 200);
    app.drain();
    CHECK_EQ(app.resets, 1);
    CHECK(!app.mgr.touchActive());
    CHECK_EQ(app.pp, 0);  // the abandoned drag completed nothing
    app.tap(2, kPpX, kPpY);  // a new contact with a different id
    CHECK_EQ(app.pp, 1);
    CHECK(!app.mgr.touchActive());
    // Reported once per loss, not on every later drain.
    app.tap(3, kPpX, kPpY);
    CHECK_EQ(app.pp, 2);
    CHECK_EQ(app.resets, 1);
}

// On the AA tab the lost Up belonged to a Press already sent to the phone: the
// reset releases it (one Release, never a stuck finger), and the next contact
// presses again.
TEST(TouchRingRecovery, LostUpOnAaPressReleasesThePhone) {
    App app;
    app.mgr.setActive(1);
    app.overflowLosingUpOf0(300, 200);  // inside the video
    app.drain();
    CHECK_EQ(app.presses, 1);
    CHECK_EQ(app.releases, 1);
    CHECK(!app.aa.pressed());
    // The rest of the old contact (if it was still down) is ignored...
    app.push(TouchPhase::Move, 0, 320, 220);
    app.push(TouchPhase::Up, 0, 0, 0);
    app.drain();
    CHECK_EQ(app.presses, 1);
    CHECK_EQ(app.releases, 1);
    // ...and a new contact works.
    app.tap(2, 400, 200);
    CHECK_EQ(app.presses, 2);
    CHECK_EQ(app.releases, 2);
}

// A backlog that only sheds Moves loses nothing the latch depends on: the drag
// in progress is kept and ends normally on its Up.
TEST(TouchRingRecovery, ShedMovesDoNotResetTheContact) {
    App app;
    app.mgr.setActive(1);
    REQUIRE(app.push(TouchPhase::Down, 0, 300, 200));
    int y = 200;
    while (app.push(TouchPhase::Move, 0, 300, ++y % 2 ? 201 : 200)) {}
    app.push(TouchPhase::Move, 0, 300, 210);  // shed as well
    CHECK(app.ring.dropped() >= 2u);
    CHECK_EQ(app.ring.droppedTransitions(), 0u);
    app.drain();
    CHECK_EQ(app.resets, 0);
    CHECK(app.mgr.touchActive());
    CHECK(app.aa.pressed());
    REQUIRE(app.push(TouchPhase::Up, 0, 0, 0));
    app.drain();
    CHECK(!app.mgr.touchActive());
    CHECK_EQ(app.presses, 1);
    CHECK_EQ(app.releases, 1);
}
