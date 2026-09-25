// ListView clipping + the Canvas clip rect it uses: swipes scroll by arbitrary
// pixel offsets, so the top/bottom rows are usually partial, and they (and long
// titles) must stay inside the list's own bounds — outside them they'd paint over
// the header / button row / A-Z strip, where taps don't reach the list.
#include <cstdint>
#include <string>
#include <vector>

#include "Test.h"
#include "input/SyntheticTouch.h"
#include "ui/Canvas.h"
#include "ui/LibraryScreen.h"
#include "ui/PngRenderTarget.h"
#include "ui/Screen.h"
#include "ui/Widgets.h"

using namespace carsh;
using namespace carsh::ui;
using carsh::host::SyntheticTouch;

TEST(ListViewClip, ScrolledLibraryRowsStayInsideTheList) {
    host::PngRenderTarget rt(800, 480);
    Canvas canvas(rt);
    ScreenManager mgr(800, 480);
    LibraryScreen lib(mgr.contentRect());  // header y 10..25, list y 44..359
    mgr.addScreen(&lib, "Lib");
    std::vector<std::string> rows;
    for (int i = 0; i < 100; ++i) rows.push_back("Artist " + std::to_string(i));
    lib.setItems(rows);
    lib.list().scrollTo(0);  // selects row 0 (accent-filled)
    SyntheticTouch t;
    t.scriptSwipe(300, 200, 160);  // scroll 40 px: row 0 is now partial at the top
    hal::TouchEvent e;
    while (t.poll(e)) mgr.onTouch(e);
    REQUIRE(lib.list().scrollOffset() == 40);
    mgr.render(canvas);
    const Theme th = mgr.theme();
    // Between the header text and the list top (y 26..43): no row text or
    // selection fill (was 137 row-text pixels there).
    CHECK_EQ(canvas.countColor(60, 26, 704, 18, th.bright), 0);
    CHECK_EQ(canvas.countColor(60, 26, 704, 18, th.accent), 0);
    CHECK_EQ(canvas.countColor(60, 26, 704, 18, th.accentBg), 0);
    // Rows are still drawn inside the list.
    CHECK(canvas.countColor(60, 44, 704, 316, th.bright) > 0);
}

TEST(ListViewClip, ListViewClipsTopBottomAndLongText) {
    host::PngRenderTarget rt(300, 300);
    Canvas c(rt);
    const Theme th = Theme::dark();
    c.clear(th.bg);
    ListView lv({20, 100, 120, 100}, 44);
    std::vector<std::string> items;
    for (int i = 0; i < 10; ++i)
        items.push_back("a very long row title that cannot fit " + std::to_string(i));
    lv.setItems(items);
    // Scroll 10 px: row 0 (y 90..133) pokes out above the list, and row 2
    // (y 178..221, text at y 196..203) out below it.
    lv.onSwipe(-10);
    lv.onTap(30, 105);  // select the partial top row (accent fill from y 90)
    lv.setCurrent(2);   // now-playing bar on the partial bottom row
    lv.draw(c, th);
    // Nothing outside the list bounds: above, below, or right of it.
    CHECK_EQ(c.countColor(0, 0, 300, 100, th.bg), 300 * 100);
    CHECK_EQ(c.countColor(0, 200, 300, 100, th.bg), 300 * 100);
    CHECK_EQ(c.countColor(140, 100, 160, 100, th.bg), 160 * 100);
    // ...but the rows themselves are drawn.
    CHECK(c.countColor(20, 100, 120, 100, th.accentBg) > 0);
    CHECK(c.countColor(20, 100, 120, 100, th.bright) > 0);
    // The clip is scoped to the draw: later primitives draw anywhere.
    c.putPixel(5, 5, th.accent);
    CHECK_EQ(c.countColor(5, 5, 1, 1, th.accent), 1);
}

TEST(ListViewClip, CanvasClipScopesNestAndRestore) {
    host::PngRenderTarget rt(100, 100);
    Canvas c(rt);
    const std::uint32_t k = hal::rgba(255, 0, 0);
    c.clear(0);
    {
        ClipScope a(c, 10, 10, 50, 50);
        {
            ClipScope b(c, 40, 40, 50, 50);  // intersects to [40,60)^2
            c.fillRect(0, 0, 100, 100, k);
            CHECK_EQ(c.countColor(0, 0, 100, 100, k), 20 * 20);
        }
        CHECK_EQ(c.clip().x0, 10);
        CHECK_EQ(c.clip().x1, 60);
        ClipScope off(c, 200, 200, 10, 10);  // wholly outside: draws nothing
        c.fillRect(0, 0, 100, 100, hal::rgba(0, 255, 0));
        CHECK_EQ(c.countColor(0, 0, 100, 100, hal::rgba(0, 255, 0)), 0);
    }
    CHECK_EQ(c.clip().x0, 0);
    CHECK_EQ(c.clip().y1, 100);
    c.putPixel(-1, 50, k);   // the target edge still clips
    c.putPixel(100, 50, k);
    CHECK_EQ(c.countColor(0, 0, 100, 100, k), 20 * 20);
}
