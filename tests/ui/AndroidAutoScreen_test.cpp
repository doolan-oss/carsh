// AndroidAutoScreen tests: the host-testable presentation/geometry layer of the AA
// tab — placeholder vs frame blit, touch remap to source coords, and the
// tab-focus callbacks (which drive VIDEO_FOCUS on device). The decoder + transport
// that feed this are hardware work (AA_PLAN rungs 5-6); everything here runs on the
// build server against a PngRenderTarget.
#include "ui/AndroidAutoScreen.h"

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

    int gotX = -1, gotY = -1;
    GestureType gotType = GestureType::None;
    aa.onTouch = [&](int sx, int sy, GestureType t) {
        gotX = sx;
        gotY = sy;
        gotType = t;
    };

    Gesture g;
    g.type = GestureType::Tap;
    g.x = f.dx + f.dw / 2;
    g.y = f.dy + f.dh / 2;
    CHECK(aa.handle(g));
    CHECK(gotType == GestureType::Tap);
    CHECK(gotX >= 395 && gotX <= 405);
    CHECK(gotY >= 235 && gotY <= 245);
}

TEST(AndroidAutoScreen, TouchInLetterboxIsNotForwarded) {
    AndroidAutoScreen aa(kContent);
    auto frame = solidRgb(800, 480, 0, 0, 0);
    aa.setFrame(frame.data(), 800, 480);
    bool called = false;
    aa.onTouch = [&](int, int, GestureType) { called = true; };

    Gesture g;
    g.type = GestureType::Tap;
    g.x = kContent.x + 1;  // left pillarbox
    g.y = kContent.h / 2;
    CHECK(!aa.handle(g));
    CHECK(!called);
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
