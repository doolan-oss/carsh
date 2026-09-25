// UI toolkit + screen tests. Render to the PNG target and drive interaction with
// scripted touch; assert on layout/hit-testing/state changes and structural
// pixels (not brittle full-image golden diffs). A couple of PNGs are saved as
// artifacts for eyeballing.
#include <string>
#include <vector>

#include "Test.h"
#include "input/SyntheticTouch.h"
#include "ui/Canvas.h"
#include "ui/Gesture.h"
#include "ui/Screen.h"
#include "ui/Screens.h"
#include "ui/Widgets.h"
#include "ui/PngRenderTarget.h"

using namespace carsh;
using namespace carsh::ui;
using carsh::host::SyntheticTouch;

static std::string out(const char* n) { return std::string("/tmp/carsh_ui_") + n; }

// --- Canvas / font ----------------------------------------------------------

TEST(Ui, CanvasDrawsTextPixels) {
    host::PngRenderTarget rt(80, 16);
    Canvas c(rt);
    c.clear(hal::rgba(0, 0, 0));
    c.drawText(0, 0, "Hi", hal::rgba(255, 255, 255));
    // 'H' and 'i' set some white pixels; background stays black elsewhere.
    CHECK(c.countColor(0, 0, 16, 8, hal::rgba(255, 255, 255)) > 8);
    CHECK(c.countColor(40, 0, 40, 16, hal::rgba(255, 255, 255)) == 0);
}

// --- Gesture recognizer -----------------------------------------------------

TEST(Ui, GestureTapVsSwipe) {
    GestureRecognizer g;
    Gesture out;
    // Tap: down then up at the same spot. Up carries no coords (0,0), as the Pi
    // driver reports it; the release point is derived from the last Down/Move.
    CHECK(!g.feed({hal::TouchPhase::Down, 0, 100, 100, 0}, out));
    REQUIRE(g.feed({hal::TouchPhase::Up, 0, 0, 0, 30}, out));
    CHECK(out.type == GestureType::Tap);
    // Swipe up: large negative dy. The endpoint arrives via Move; Up is (0,0).
    g.feed({hal::TouchPhase::Down, 0, 200, 300, 0}, out);
    g.feed({hal::TouchPhase::Move, 0, 200, 250, 10}, out);
    g.feed({hal::TouchPhase::Move, 0, 200, 150, 20}, out);
    REQUIRE(g.feed({hal::TouchPhase::Up, 0, 0, 0, 30}, out));
    CHECK(out.type == GestureType::Swipe);
    CHECK(out.dy < -100);
}

// --- Widgets ----------------------------------------------------------------

TEST(Ui, ButtonTapInvokesCallback) {
    int presses = 0;
    Button b({10, 10, 80, 60}, "OK", [&] { ++presses; });
    CHECK(b.onTap(50, 40));          // inside
    CHECK(!b.onTap(200, 200));       // outside
    CHECK_EQ(presses, 1);
}

TEST(Ui, ListViewSelectAndScroll) {
    ListView lv({0, 0, 200, 88}, 44);  // 2 visible rows of 44px
    std::vector<std::string> items;
    for (int i = 0; i < 10; ++i) items.push_back("item" + std::to_string(i));
    lv.setItems(items);
    int sel = -1;
    lv.setOnSelect([&](int i) { sel = i; });
    CHECK(lv.onTap(10, 10));         // first row
    CHECK_EQ(sel, 0);
    CHECK_EQ(lv.scrollOffset(), 0);
    lv.onSwipe(-88);                 // fling up two rows
    CHECK(lv.scrollOffset() > 0);
    // Tapping now selects a later item (scrolled).
    lv.onTap(10, 10);
    CHECK(sel >= 2);
    // Scroll clamps at the bottom.
    lv.onSwipe(-10000);
    int maxScroll = 10 * 44 - 88;
    CHECK_EQ(lv.scrollOffset(), maxScroll);
}

TEST(Ui, TabBarSwitchesOnTap) {
    TabBar tb({0, 0, 400, 48}, {"A", "B", "C", "D"});
    int changed = -1;
    tb.setOnChange([&](int i) { changed = i; });
    CHECK(tb.onTap(350, 24));        // 4th tab (x 300..400)
    CHECK_EQ(tb.activeIndex(), 3);
    CHECK_EQ(changed, 3);
}

// --- ScreenManager + the seven screens --------------------------------------

namespace {
struct App {
    host::PngRenderTarget rt{800, 480};
    Canvas canvas{rt};
    ScreenManager mgr{800, 480};
    NowPlayingScreen now{mgr.contentRect()};
    ListScreen library{mgr.contentRect(), "Lib"};
    SearchScreen search{mgr.contentRect()};
    ListScreen queue{mgr.contentRect(), "Q"};
    EqScreen eq{mgr.contentRect()};
    ListScreen sources{mgr.contentRect(), "Src"};
    SettingsScreen settings{mgr.contentRect()};
    App() {
        mgr.addScreen(&now, "Now");
        mgr.addScreen(&library, "Lib");
        mgr.addScreen(&search, "Find");
        mgr.addScreen(&queue, "Q");
        mgr.addScreen(&eq, "EQ");
        mgr.addScreen(&sources, "Src");
        mgr.addScreen(&settings, "Set");
    }
    void tap(SyntheticTouch& t, int x, int y) {
        t.scriptTap(x, y);
        hal::TouchEvent e;
        while (t.poll(e)) mgr.onTouch(e);
    }
};
}  // namespace

TEST(Ui, SevenScreensRegisteredAndRender) {
    App app;
    CHECK_EQ(app.mgr.tabBar().tabCount(), 7);
    app.now.setState({"Money", "Pink Floyd", "Dark Side", 90000, 382000, true, ""});
    app.mgr.render(app.canvas);
    CHECK(app.rt.save(out("nowplaying.png")));
    // Accent color appears (active tab underline + progress + artist text).
    Theme th = Theme::dark();
    CHECK(app.canvas.countColor(0, 0, 800, 480, th.accent) > 0);
}

TEST(Ui, TabTapSwitchesActiveScreen) {
    App app;
    SyntheticTouch t;
    CHECK_EQ(app.mgr.activeIndex(), 0);
    // Tab bar spans bottom 48px; tap the 5th tab (EQ): x in [4*800/7,5*800/7).
    int tabW = 800 / 7;
    app.tap(t, 4 * tabW + tabW / 2, 480 - 24);
    CHECK_EQ(app.mgr.activeIndex(), 4);
    CHECK_EQ(std::string(app.mgr.activeScreen()->name()), std::string("EQ"));
}

TEST(Ui, VolumeSliderMapsAndClamps) {
    VolumeSlider v({0, 0, 50, 100});  // track y in [0,100], top = max
    CHECK(v.value() == 0.0f);          // boots muted
    v.setFromY(0);   CHECK(v.value() > 0.99f);             // top -> 1
    v.setFromY(100); CHECK(v.value() < 0.01f);             // bottom -> 0
    v.setFromY(50);  CHECK(v.value() > 0.45f); CHECK(v.value() < 0.55f);
    v.setFromY(-20); CHECK(v.value() == 1.0f);             // above -> clamp
    v.setFromY(999); CHECK(v.value() == 0.0f);             // below -> clamp
}

TEST(Ui, VolumeLogTaperAndCap) {
    VolumeSlider v({0, 0, 50, 100});
    v.setValue(1.0f); CHECK(v.gain() > 0.49f); CHECK(v.gain() <= 0.5f);  // capped
    v.setValue(0.0f); CHECK(v.gain() == 0.0f);                            // mute
    // Logarithmic: the slider midpoint sits well below half the cap.
    v.setValue(0.5f); CHECK(v.gain() < 0.1f);
    // Monotonic; value()/the % display stays the raw position.
    v.setValue(0.25f); float g25 = v.gain();
    v.setValue(0.75f); float g75 = v.gain();
    CHECK(g75 > g25);
    CHECK(v.value() == 0.75f);
}

TEST(Ui, VolumeRailDragSetsVolume) {
    App app;  // 800x480; rail is the left strip above the tab bar
    float vol = -1.0f;
    app.mgr.setOnVolumeChange([&](float v) { vol = v; });
    // Down at the very top of the rail -> the cap (0.5 linear), live on Down.
    app.mgr.onTouch({hal::TouchPhase::Down, 0, 10, 0, 0});
    CHECK(vol > 0.49f); CHECK(vol <= 0.5f);
    // Drag toward the bottom -> very low (log taper). Up reports (0,0) but must
    // not zero it.
    app.mgr.onTouch({hal::TouchPhase::Move, 0, 10, 420, 10});
    CHECK(vol < 0.05f);
    float low = vol;
    app.mgr.onTouch({hal::TouchPhase::Up, 0, 0, 0, 20});
    CHECK_EQ(vol, low);  // Up's (0,0) ignored
    // A tap in the content area (right of the rail) does NOT move volume.
    app.mgr.onTouch({hal::TouchPhase::Down, 0, 400, 200, 30});
    app.mgr.onTouch({hal::TouchPhase::Up, 0, 0, 0, 40});
    CHECK_EQ(vol, low);
}

TEST(Ui, NowPlayingTransportButtons) {
    App app;
    int prev = 0, pp = 0, next = 0;
    app.now.onPrev = [&] { ++prev; };
    app.now.onPlayPause = [&] { ++pp; };
    app.now.onNext = [&] { ++next; };
    SyntheticTouch t;
    Rect cr = app.mgr.contentRect();
    // Buttons at content y+300, x = content.x + {400,500,600}, 90x60.
    app.tap(t, cr.x + 400 + 45, cr.y + 300 + 30);  // prev
    app.tap(t, cr.x + 500 + 45, cr.y + 300 + 30);  // play/pause
    app.tap(t, cr.x + 600 + 45, cr.y + 300 + 30);  // next
    CHECK_EQ(prev, 1);
    CHECK_EQ(pp, 1);
    CHECK_EQ(next, 1);
}

TEST(Ui, LibraryListSelection) {
    App app;
    app.mgr.setActive(1);  // Library
    int picked = -1;
    app.library.setOnSelect([&](int i) { picked = i; });
    app.library.setItems({"Album A", "Album B", "Album C"});
    SyntheticTouch t;
    Rect cr = app.mgr.contentRect();
    app.tap(t, cr.x + 20, cr.y + 20);  // first row (content is inset by the rail)
    CHECK_EQ(picked, 0);
}

TEST(Ui, EqBandAdjust) {
    App app;
    app.mgr.setActive(4);  // EQ
    dsp::EqConfig cfg;
    cfg.bands = {{dsp::FilterType::Peaking, 60, 0, 1.0},
                 {dsp::FilterType::Peaking, 1000, 0, 1.0},
                 {dsp::FilterType::Peaking, 8000, 0, 1.0}};
    app.eq.setConfig(cfg);
    int changedBand = -1; double changedGain = 0;
    app.eq.onBandChange = [&](int b, double g) { changedBand = b; changedGain = g; };
    SyntheticTouch t;
    Rect cr = app.mgr.contentRect();
    // Tap the upper half of the middle band column -> boost.
    int areaX = cr.x + 20, areaW = cr.w - 40, areaY = cr.y + 20, areaH = cr.h - 80;
    int bandX = areaX + (areaW / 3) + (areaW / 3) / 2;
    int upperY = areaY + areaH / 4;
    app.tap(t, bandX, upperY);
    CHECK_EQ(changedBand, 1);
    CHECK(changedGain > 0);
}

TEST(Ui, SettingsToggle) {
    App app;
    app.mgr.setActive(6);
    app.settings.setItems({{"ReplayGain", "Track"}, {"Theme", "Dark"}});
    int activated = -1;
    app.settings.onActivate = [&](int i) { activated = i; };
    SyntheticTouch t;
    Rect cr = app.mgr.contentRect();
    app.tap(t, cr.x + 20, cr.y + 20);  // first settings row (content inset by rail)
    CHECK_EQ(activated, 0);
    app.settings.setValue(0, "Album");  // controller would flip + persist
    app.mgr.render(app.canvas);
    CHECK(app.rt.save(out("settings.png")));
}
