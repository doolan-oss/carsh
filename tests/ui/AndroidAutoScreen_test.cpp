// AndroidAutoScreen tests: the host-testable presentation/geometry layer of the AA
// tab — placeholder vs frame blit, touch remap to source coords, and the
// tab-focus callbacks (which drive VIDEO_FOCUS on device). The decoder + transport
// that feed this are hardware work (AA_PLAN rungs 5-6); everything here runs on the
// build server against a PngRenderTarget.
#include "ui/AndroidAutoScreen.h"

#include <functional>
#include <vector>

#include "Test.h"
#include "ui/PngRenderTarget.h"
#include "ui/Screen.h"

using namespace carsh;
using carsh::ui::AndroidAutoScreen;
using carsh::ui::Canvas;
using carsh::ui::Gesture;
using carsh::ui::GestureType;
using carsh::ui::Rect;
using carsh::ui::Screen;
using carsh::ui::ScreenManager;
using carsh::ui::Theme;

namespace {
constexpr Rect kContent{52, 0, 748, 432};  // carsh content rect on the 800x480 panel

// Minimal Screen for the ScreenManager focus-wiring test.
class DummyScreen : public Screen {
public:
    explicit DummyScreen(Rect c) : Screen(c) {}
    const char* name() const override { return "X"; }
};

// One recorded AA touch action (source-frame coords).
struct Act {
    aa::TouchAction a;
    int x, y;
};
std::function<void(aa::TouchAction, int, int)> recorder(std::vector<Act>& v) {
    return [&v](aa::TouchAction a, int x, int y) { v.push_back({a, x, y}); };
}

std::vector<std::uint8_t> solidRgb(int w, int h, std::uint8_t r, std::uint8_t g,
                                   std::uint8_t b) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 3);
    for (std::size_t i = 0; i < px.size(); i += 3) {
        px[i] = r;
        px[i + 1] = g;
        px[i + 2] = b;
    }
    return px;
}
}  // namespace

TEST(AndroidAutoScreen, PlaceholderWhenNoFrame) {
    host::PngRenderTarget rt(800, 480);
    Canvas c(rt);
    Theme th = Theme::dark();
    AndroidAutoScreen aa(kContent);
    aa.draw(c, th);
    // The "plug in the dongle" hint is drawn in the dim color over the bg.
    CHECK(c.countColor(kContent.x, kContent.y, kContent.w, kContent.h, th.dim) > 0);
}

TEST(AndroidAutoScreen, BlitsFrameAspectCorrectWithLetterbox) {
    host::PngRenderTarget rt(800, 480);
    Canvas c(rt);
    Theme th = Theme::dark();
    AndroidAutoScreen aa(kContent);
    auto frame = solidRgb(800, 480, 255, 0, 0);  // solid red source
    aa.setFrame(frame.data(), 800, 480);
    aa.draw(c, th);

    auto f = aa.fit();
    const std::uint32_t red = hal::rgba(255, 0, 0);
    // The whole placed video rect is red.
    CHECK_EQ(c.countColor(f.dx, f.dy, f.dw, f.dh, red), f.dw * f.dh);
    // The left pillarbox bar (content rect, left of the video) is NOT red.
    CHECK(f.dx > kContent.x);  // there is a bar to test
    CHECK_EQ(c.countColor(kContent.x, f.dy, f.dx - kContent.x, f.dh, red), 0);
}

TEST(AndroidAutoScreen, TouchInsideVideoRemapsToSource) {
    AndroidAutoScreen aa(kContent);
    auto frame = solidRgb(800, 480, 0, 0, 0);
    aa.setFrame(frame.data(), 800, 480);
    auto f = aa.fit();

    std::vector<Act> acts;
    aa.onTouchAction = recorder(acts);

    CHECK(aa.wantsRawTouch());
    const int px = f.dx + f.dw / 2, py = f.dy + f.dh / 2;
    CHECK(aa.onRawTouch({hal::TouchPhase::Down, 0, px, py, 0}));
    REQUIRE(acts.size() == 1u);
    CHECK(acts[0].a == aa::TouchAction::Press);
    CHECK(acts[0].x >= 395 && acts[0].x <= 405);
    CHECK(acts[0].y >= 235 && acts[0].y <= 245);
    // Up carries (0,0) on the Pi: the Release reuses the last mapped point.
    CHECK(aa.onRawTouch({hal::TouchPhase::Up, 0, 0, 0, 30}));
    REQUIRE(acts.size() == 2u);
    CHECK(acts[1].a == aa::TouchAction::Release);
    CHECK_EQ(acts[1].x, acts[0].x);
    CHECK_EQ(acts[1].y, acts[0].y);
    CHECK(!aa.pressed());
}

TEST(AndroidAutoScreen, TouchInLetterboxIsNotForwarded) {
    AndroidAutoScreen aa(kContent);
    auto frame = solidRgb(800, 480, 0, 0, 0);
    aa.setFrame(frame.data(), 800, 480);
    std::vector<Act> acts;
    aa.onTouchAction = recorder(acts);

    // Left pillarbox: not claimed, so its Move/Up aren't ours either.
    CHECK(!aa.onRawTouch({hal::TouchPhase::Down, 0, kContent.x + 1, kContent.h / 2, 0}));
    CHECK(!aa.onRawTouch({hal::TouchPhase::Move, 0, 400, 200, 10}));
    CHECK(!aa.onRawTouch({hal::TouchPhase::Up, 0, 0, 0, 20}));
    CHECK(acts.empty());
    // A completed gesture (the letterbox fallback path) is dropped, too.
    Gesture g;
    g.type = GestureType::Tap;
    g.x = kContent.x + 1;
    g.y = kContent.h / 2;
    CHECK(!aa.handle(g));
    CHECK(acts.empty());
}

TEST(AndroidAutoScreen, NoVideoMeansNoTouch) {
    AndroidAutoScreen aa(kContent);  // placeholder: nothing projected yet
    std::vector<Act> acts;
    aa.onTouchAction = recorder(acts);
    CHECK(!aa.onRawTouch({hal::TouchPhase::Down, 0, 400, 200, 0}));
    CHECK(acts.empty());
}

TEST(AndroidAutoScreen, DragIsClampedToTheVideoRect) {
    AndroidAutoScreen aa(kContent);
    auto frame = solidRgb(800, 480, 0, 0, 0);
    aa.setFrame(frame.data(), 800, 480);
    auto f = aa.fit();
    std::vector<Act> acts;
    aa.onTouchAction = recorder(acts);

    CHECK(aa.onRawTouch({hal::TouchPhase::Down, 0, f.dx + 100, f.dy + 100, 0}));
    // Slide off the left of the picture (into the pillarbox + rail) and below it
    // (into the tab bar): the drag pins to the video's bottom-left corner instead
    // of being dropped.
    int cornerX = -1, cornerY = -1;  // the picture's bottom-left pixel, in source
    REQUIRE(aa::mapToSource(f, 800, 480, f.dx, f.dy + f.dh - 1, cornerX, cornerY));
    CHECK_EQ(cornerX, 0);
    CHECK(cornerY >= 470);
    CHECK(aa.onRawTouch({hal::TouchPhase::Move, 0, 5, 470, 10}));
    REQUIRE(acts.size() == 2u);
    CHECK(acts[1].a == aa::TouchAction::Drag);
    CHECK_EQ(acts[1].x, cornerX);
    CHECK_EQ(acts[1].y, cornerY);
    // Still off-picture at the same clamped point: no duplicate Drag.
    CHECK(aa.onRawTouch({hal::TouchPhase::Move, 0, 2, 475, 20}));
    CHECK_EQ(acts.size(), 2u);
    CHECK(aa.onRawTouch({hal::TouchPhase::Up, 0, 0, 0, 30}));
    REQUIRE(acts.size() == 3u);
    CHECK(acts[2].a == aa::TouchAction::Release);
    CHECK_EQ(acts[2].x, cornerX);
    CHECK_EQ(acts[2].y, cornerY);
}

TEST(AndroidAutoScreen, DeactivateMidPressReleasesOnce) {
    AndroidAutoScreen aa(kContent);
    auto frame = solidRgb(800, 480, 0, 0, 0);
    aa.setFrame(frame.data(), 800, 480);
    auto f = aa.fit();
    std::vector<Act> acts;
    aa.onTouchAction = recorder(acts);
    CHECK(aa.onRawTouch({hal::TouchPhase::Down, 0, f.dx + 10, f.dy + 10, 0}));
    aa.onRawTouchCancel();
    aa.onDeactivate();  // already released: nothing more
    REQUIRE(acts.size() == 2u);
    CHECK(acts[1].a == aa::TouchAction::Release);
    CHECK(!aa.pressed());
}

TEST(AndroidAutoScreen, TabFocusDrivesVideoFocusCallback) {
    ScreenManager mgr(800, 480);
    DummyScreen home(mgr.contentRect());
    AndroidAutoScreen aa(mgr.contentRect());
    int focusEvents = 0;
    bool lastFocused = false;
    aa.onFocusChanged = [&](bool f) {
        ++focusEvents;
        lastFocused = f;
    };
    mgr.addScreen(&home, "Home");  // index 0 (active by default)
    mgr.addScreen(&aa, "AA");      // index 1

    CHECK(!aa.focused());
    mgr.setActive(1);  // switch to AA
    CHECK(aa.focused());
    CHECK(lastFocused);
    CHECK_EQ(focusEvents, 1);

    mgr.setActive(0);  // switch away
    CHECK(!aa.focused());
    CHECK(!lastFocused);
    CHECK_EQ(focusEvents, 2);

    mgr.setActive(0);  // no-op, no spurious event
    CHECK_EQ(focusEvents, 2);
}
