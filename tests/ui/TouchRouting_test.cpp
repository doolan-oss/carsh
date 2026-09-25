// Touch routing through ScreenManager, end to end from raw TouchEvents: swipes
// hit-tested at their start point (list drags that end off the list still
// scroll), the primary-contact latch (a second finger or a palm can't hijack the
// volume rail or steal a tap), tab switches cancelling the touch in progress,
// the Android Auto raw path (Press/Drag/Release, incl. the Pi's Up-at-(0,0)) with
// the tab bar + rail still working on the AA tab, and the SnapshotTouch model of
// the Pi driver's snapshot semantics.
#include <cstdint>
#include <string>
#include <vector>

#include "Test.h"
#include "input/SnapshotTouch.h"
#include "input/SyntheticTouch.h"
#include "ui/AndroidAutoScreen.h"
#include "ui/Gesture.h"
#include "ui/LibraryScreen.h"
#include "ui/PngRenderTarget.h"
#include "ui/Screen.h"
#include "ui/Screens.h"

using namespace carsh;
using namespace carsh::ui;
using carsh::hal::TouchPhase;
using carsh::host::SnapshotTouch;
using carsh::host::SyntheticTouch;

namespace {

struct Act {
    aa::TouchAction a;
    int x, y;
};

// 800x480 panel, content rect {52,0,748,432}, 4 tabs of 200 px at y 432..479.
//   Now (0): |< [452,542) > [552,642) >| [652,742), y [300,360)
//   Lib (1): header y 10..25, list {60,44,704,316} (y 44..359), BACK/PLAY/SHUFFLE
//            at y 368..423
//   Src (2): full-height list {60,8,732,416} (y 8..423)
//   AA  (3): 800x480 video fit at {66,0,720,432}
struct TouchApp {
    static constexpr int kNow = 0, kLib = 1, kSrc = 2, kAa = 3;

    host::PngRenderTarget rt{800, 480};
    Canvas canvas{rt};
    ScreenManager mgr{800, 480};
    NowPlayingScreen now{mgr.contentRect()};
    LibraryScreen lib{mgr.contentRect()};
    ListScreen src{mgr.contentRect(), "Src"};
    AndroidAutoScreen aa{mgr.contentRect()};
    std::vector<std::uint8_t> frame = std::vector<std::uint8_t>(800 * 480 * 3, 0);

    int prev = 0, pp = 0, next = 0, plays = 0, picked = -1, volCalls = 0;
    float vol = -1.0f;
    std::vector<Act> acts;

    TouchApp() {
        mgr.addScreen(&now, "Now");
        mgr.addScreen(&lib, "Lib");
        mgr.addScreen(&src, "Src");
        mgr.addScreen(&aa, "AA");
        now.onPrev = [this] { ++prev; };
        now.onPlayPause = [this] { ++pp; };
        now.onNext = [this] { ++next; };
        lib.onPlay = [this] { ++plays; };
        lib.onSelectRow = [this](int i) { picked = i; };
        aa.onTouchAction = [this](aa::TouchAction a, int x, int y) {
            acts.push_back({a, x, y});
        };
        mgr.setOnVolumeChange([this](float v) { vol = v; ++volCalls; });
        std::vector<std::string> rows;
        for (int i = 0; i < 100; ++i) rows.push_back("Artist " + std::to_string(i));
        lib.setItems(rows);
        src.setItems(rows);
    }

    void ev(TouchPhase ph, int id, int x, int y) { mgr.onTouch({ph, id, x, y, 0}); }
    void down(int id, int x, int y) { ev(TouchPhase::Down, id, x, y); }
    void move(int id, int x, int y) { ev(TouchPhase::Move, id, x, y); }
    void up(int id) { ev(TouchPhase::Up, id, 0, 0); }  // the Pi reports Up at (0,0)
    void run(hal::ITouch& t) {
        hal::TouchEvent e;
        while (t.poll(e)) mgr.onTouch(e);
    }
    void showVideo() { aa.setFrame(frame.data(), 800, 480); }
    // Panel point -> AA source coords for the {66,0,720,432} fit.
    static int srcX(int px) { return (px - 66) * 800 / 720; }
    static int srcY(int py) { return py * 480 / 432; }
};

}  // namespace

// --- T5: swipes hit-test at the START point --------------------------------

TEST(TouchRouting, LibrarySwipeEndingOnButtonRowStillScrolls) {
    TouchApp app;
    app.mgr.setActive(TouchApp::kLib);
    app.lib.scrollTo(50);
    const int before = app.lib.list().scrollOffset();
    REQUIRE(before == 2200);
    // Drag down 140 px from mid-list, lifting on PLAY (y 368..423): the old
    // end-point hit test found no list there and dropped the whole scroll.
    SyntheticTouch t;
    t.scriptSwipe(300, 250, 390);
    app.run(t);
    CHECK_EQ(app.lib.list().scrollOffset(), before - 140);
    CHECK_EQ(app.plays, 0);  // a swipe is not a tap on the button it ends on
}

TEST(TouchRouting, LibrarySwipeEndingOnHeaderStillScrolls) {
    TouchApp app;
    app.mgr.setActive(TouchApp::kLib);
    SyntheticTouch t;
    t.scriptSwipe(300, 120, 20);  // up 100 px, lifting over the header label
    app.run(t);
    CHECK_EQ(app.lib.list().scrollOffset(), 100);
}

TEST(TouchRouting, FullHeightListSwipeEndingOnTabBarScrollsWithoutSwitching) {
    TouchApp app;
    app.mgr.setActive(TouchApp::kSrc);
    app.src.list().scrollTo(20);
    const int before = app.src.list().scrollOffset();
    SyntheticTouch t;
    t.scriptSwipe(300, 300, 460);  // down 160 px, lifting on the tab bar
    app.run(t);
    CHECK_EQ(app.src.list().scrollOffset(), before - 160);
    CHECK_EQ(app.mgr.activeIndex(), TouchApp::kSrc);  // a swipe never switches tabs
}

TEST(TouchRouting, SwipeStartingOffTheListDoesNotScrollIt) {
    TouchApp app;
    app.mgr.setActive(TouchApp::kLib);
    SyntheticTouch t;
    t.scriptSwipe(300, 20, 200);  // starts on the header, ends mid-list
    app.run(t);
    CHECK_EQ(app.lib.list().scrollOffset(), 0);
}

TEST(TouchRouting, GestureCarriesStartPoint) {
    GestureRecognizer g;
    Gesture out;
    g.feed({TouchPhase::Down, 0, 200, 300, 0}, out);
    g.feed({TouchPhase::Move, 0, 210, 150, 10}, out);
    REQUIRE(g.feed({TouchPhase::Up, 0, 0, 0, 20}, out));
    CHECK(out.type == GestureType::Swipe);
    CHECK_EQ(out.sx, 200);
    CHECK_EQ(out.sy, 300);
    CHECK_EQ(out.x, 210);
    CHECK_EQ(out.y, 150);
}

// --- T6: primary-contact latch ----------------------------------------------

TEST(TouchRouting, SecondFingerCannotHijackRailDrag) {
    TouchApp app;
    app.down(0, 10, 200);  // finger 0 grabs the rail
    const float v0 = app.vol;
    const int calls0 = app.volCalls;
    app.down(1, 400, 300);  // finger 1 lands on the content...
    app.move(1, 400, 10);   // ...and moves: must NOT drive the rail
    app.up(1);              // ...and lifts: must NOT end finger 0's drag
    CHECK_EQ(app.volCalls, calls0);
    CHECK_EQ(app.vol, v0);
    app.move(0, 10, 100);  // finger 0 still drags the rail
    CHECK_EQ(app.volCalls, calls0 + 1);
    CHECK(app.vol > v0);
    app.up(0);
    CHECK(!app.mgr.touchActive());
}

TEST(TouchRouting, PalmOnRailWhileScrollingLeavesVolumeAlone) {
    TouchApp app;
    app.mgr.setActive(TouchApp::kLib);
    app.down(0, 300, 350);  // finger 0 starts a list scroll
    app.down(1, 10, 420);   // palm on the rail (left bezel)
    for (int y = 330; y >= 50; y -= 20) app.move(0, 300, y);
    app.move(1, 12, 400);
    app.up(1);
    app.up(0);
    CHECK_EQ(app.volCalls, 0);  // was: gain 0.006 -> 0.326 (~+35 dB)
    CHECK_EQ(app.lib.list().scrollOffset(), 300);
}

TEST(TouchRouting, StrayContactCannotStealATap) {
    TouchApp app;  // Now Playing
    app.down(0, 497, 330);  // finger on |<
    app.down(1, 697, 330);  // stray contact on >|
    app.up(1);              // the stray lifts first: completes nothing
    app.up(0);
    CHECK_EQ(app.prev, 1);
    CHECK_EQ(app.next, 0);

    // Primary lifts first; the stray's late Up is ignored too.
    app.down(0, 497, 330);
    app.down(1, 697, 330);
    app.up(0);
    app.up(1);
    CHECK_EQ(app.prev, 2);
    CHECK_EQ(app.next, 0);

    // Once nothing is down, any contact id starts a fresh touch.
    SyntheticTouch t;
    t.scriptTap(697, 330, 0, /*id=*/3);
    app.run(t);
    CHECK_EQ(app.next, 1);
}

TEST(TouchRouting, RecognizerIgnoresInterleavedSecondContact) {
    GestureRecognizer g;
    Gesture out;
    CHECK(!g.feed({TouchPhase::Down, 0, 100, 100, 0}, out));
    CHECK(!g.feed({TouchPhase::Down, 1, 500, 300, 5}, out));  // ignored
    CHECK(!g.feed({TouchPhase::Move, 1, 520, 320, 10}, out));  // ignored
    CHECK(!g.feed({TouchPhase::Up, 1, 0, 0, 15}, out));        // not our contact
    REQUIRE(g.feed({TouchPhase::Up, 0, 0, 0, 20}, out));
    CHECK(out.type == GestureType::Tap);
    CHECK_EQ(out.x, 100);
    CHECK_EQ(out.y, 100);
}

// --- T8: a tab switch cancels the touch in progress -------------------------

TEST(TouchRouting, ProgrammaticTabSwitchCancelsGesture) {
    TouchApp app;
    app.mgr.setActive(TouchApp::kLib);
    app.down(0, 597, 330);             // press a Library row...
    app.mgr.setActive(TouchApp::kNow);  // ...the kernel switches tabs...
    app.up(0);                          // ...and the finger lifts over ">"
    CHECK_EQ(app.pp, 0);                // no transport tap on the new screen
    CHECK_EQ(app.picked, -1);           // nor a row pick on the old one
    CHECK(!app.mgr.touchActive());
    // The next touch works normally.
    SyntheticTouch t;
    t.scriptTap(597, 330);
    app.run(t);
    CHECK_EQ(app.pp, 1);
}

TEST(TouchRouting, AutoShowMidPressSendsNothingToThePhone) {
    TouchApp app;  // Now Playing
    app.showVideo();
    app.down(0, 597, 330);             // press ">"...
    app.mgr.setActive(TouchApp::kAa);  // ...first AA frame auto-shows the AA tab
    app.move(0, 600, 300);
    app.up(0);
    CHECK(app.acts.empty());
    CHECK_EQ(app.pp, 0);
}

TEST(TouchRouting, AaDisconnectMidPressReleasesAndDropsTheRest) {
    TouchApp app;
    app.showVideo();
    app.mgr.setActive(TouchApp::kAa);
    app.down(0, 597, 330);  // Press on the projection
    REQUIRE(app.acts.size() == 1u);
    app.mgr.setActive(TouchApp::kNow);  // AA dropped -> back to Now Playing
    REQUIRE(app.acts.size() == 2u);     // the phone's pointer is never left down
    CHECK(app.acts[1].a == aa::TouchAction::Release);
    app.move(0, 590, 320);
    app.up(0);
    CHECK_EQ(app.acts.size(), 2u);  // nothing after the cancel
    CHECK_EQ(app.pp, 0);            // and no ">" tap on Now Playing
}

TEST(TouchRouting, TabSwitchEndsRailDrag) {
    TouchApp app;
    app.down(0, 10, 300);
    const int calls = app.volCalls;
    app.mgr.setActive(TouchApp::kLib);
    app.move(0, 10, 50);
    app.up(0);
    CHECK_EQ(app.volCalls, calls);
}

// --- T3: Android Auto raw touch path ---------------------------------------

TEST(TouchRouting, AaDragStreamsPressDragRelease) {
    TouchApp app;
    app.showVideo();
    app.mgr.setActive(TouchApp::kAa);
    // A playlist scroll: finger down low, drags up 280 px, lifts.
    app.down(0, 400, 380);
    REQUIRE(app.acts.size() == 1u);  // Press goes out at finger-down, not on lift
    CHECK(app.acts[0].a == aa::TouchAction::Press);
    CHECK_EQ(app.acts[0].x, TouchApp::srcX(400));
    CHECK_EQ(app.acts[0].y, TouchApp::srcY(380));
    for (int y = 345; y >= 100; y -= 35) app.move(0, 400, y);
    app.up(0);  // Up at (0,0)
    REQUIRE(app.acts.size() == 10u);  // Press + 8 Drags + Release
    for (std::size_t i = 1; i + 1 < app.acts.size(); ++i) {
        CHECK(app.acts[i].a == aa::TouchAction::Drag);
        CHECK_EQ(app.acts[i].x, TouchApp::srcX(400));
    }
    CHECK_EQ(app.acts[8].y, TouchApp::srcY(100));
    const Act& rel = app.acts.back();
    CHECK(rel.a == aa::TouchAction::Release);
    CHECK_EQ(rel.x, TouchApp::srcX(400));  // the last mapped point, not (0,0)
    CHECK_EQ(rel.y, TouchApp::srcY(100));
    CHECK(!app.mgr.touchActive());
}

TEST(TouchRouting, AaLongPressIsPressThenLateRelease) {
    TouchApp app;
    app.showVideo();
    app.mgr.setActive(TouchApp::kAa);
    app.down(0, 300, 200);
    CHECK_EQ(app.acts.size(), 1u);  // phone sees the finger down immediately
    app.move(0, 300, 200);          // no motion (same point): no Drag
    CHECK_EQ(app.acts.size(), 1u);
    app.up(0);
    REQUIRE(app.acts.size() == 2u);
    CHECK(app.acts[1].a == aa::TouchAction::Release);
}

TEST(TouchRouting, AaTabBarAndRailStillWork) {
    TouchApp app;
    app.showVideo();
    app.mgr.setActive(TouchApp::kAa);
    // Rail on the AA tab: a live volume drag, nothing to the phone.
    app.down(0, 10, 100);
    app.move(0, 10, 300);
    app.up(0);
    CHECK_EQ(app.volCalls, 2);
    CHECK(app.acts.empty());
    // Tab bar on the AA tab: a tap switches tabs, nothing to the phone.
    SyntheticTouch t;
    t.scriptTap(300, 456);  // "Lib" tab
    app.run(t);
    CHECK_EQ(app.mgr.activeIndex(), TouchApp::kLib);
    CHECK(app.acts.empty());
}

TEST(TouchRouting, AaForwardsOnlyThePrimaryPointer) {
    TouchApp app;
    app.showVideo();
    app.mgr.setActive(TouchApp::kAa);
    app.down(0, 300, 200);
    app.down(1, 500, 260);  // pinch: second finger ignored (multi-pointer AA is
    app.move(1, 520, 280);  // future work)
    app.move(0, 280, 180);
    app.up(1);              // must not Release finger 0
    CHECK_EQ(app.acts.size(), 2u);
    CHECK(app.acts[1].a == aa::TouchAction::Drag);
    app.up(0);
    REQUIRE(app.acts.size() == 3u);
    CHECK(app.acts[2].a == aa::TouchAction::Release);
    CHECK_EQ(app.acts[2].x, TouchApp::srcX(280));
    CHECK_EQ(app.acts[2].y, TouchApp::srcY(180));
}

TEST(TouchRouting, AaWithoutVideoSendsNothing) {
    TouchApp app;  // no frame yet: the placeholder is up
    app.mgr.setActive(TouchApp::kAa);
    SyntheticTouch t;
    t.scriptDrag(200, 200, 400, 300);
    t.scriptTap(400, 200);
    app.run(t);
    CHECK(app.acts.empty());
}

TEST(TouchRouting, AaDrivenBySnapshotDriverModel) {
    TouchApp app;
    app.showVideo();
    app.mgr.setActive(TouchApp::kAa);
    SnapshotTouch drv;
    drv.touch(0, 200, 200);
    drv.update();
    drv.touch(0, 240, 210);
    drv.update();
    drv.update();  // nothing new published: no event
    drv.lift(0);
    drv.update();
    app.run(drv);
    REQUIRE(app.acts.size() == 3u);
    CHECK(app.acts[0].a == aa::TouchAction::Press);
    CHECK(app.acts[1].a == aa::TouchAction::Drag);
    CHECK(app.acts[2].a == aa::TouchAction::Release);
    CHECK_EQ(app.acts[2].x, TouchApp::srcX(240));
    CHECK_EQ(app.acts[2].y, TouchApp::srcY(210));
}

// --- SnapshotTouch: the Pi driver's snapshot semantics ----------------------

TEST(TouchRouting, SnapshotModelMatchesDriverDiff) {
    SnapshotTouch drv;
    hal::TouchEvent e;
    // Down, Move, and Up at (0,0).
    drv.touch(0, 100, 120);
    drv.update(1);
    REQUIRE(drv.poll(e));
    CHECK(e.phase == TouchPhase::Down);
    CHECK_EQ(e.x, 100);
    drv.touch(0, 100, 120);  // republished, unchanged: no Move
    drv.update(2);
    CHECK(!drv.poll(e));
    drv.touch(0, 110, 130);
    drv.update(3);
    REQUIRE(drv.poll(e));
    CHECK(e.phase == TouchPhase::Move);
    drv.lift(0);
    drv.update(4);
    REQUIRE(drv.poll(e));
    CHECK(e.phase == TouchPhase::Up);
    CHECK_EQ(e.x, 0);
    CHECK_EQ(e.y, 0);
    // A whole tap between two reads is never seen.
    drv.touch(0, 300, 300);
    drv.lift(0);
    drv.update(5);
    CHECK(!drv.poll(e));
    // Lift + re-touch of the same id between reads reads as a Move.
    drv.touch(2, 50, 50);
    drv.update(6);
    REQUIRE(drv.poll(e));
    drv.lift(2);
    drv.touch(2, 400, 60);
    drv.update(7);
    REQUIRE(drv.poll(e));
    CHECK(e.phase == TouchPhase::Move);
    CHECK_EQ(e.id, 2);
    CHECK(!drv.poll(e));
}

TEST(TouchRouting, SnapshotTapHitsButton) {
    TouchApp app;  // Now Playing
    SnapshotTouch drv;
    drv.touch(0, 697, 330);
    drv.update();
    drv.lift(0);
    drv.update();
    app.run(drv);
    CHECK_EQ(app.next, 1);
}
