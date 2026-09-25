// NowPlayingScreen renders the active-source badge (e.g. "BLUETOOTH") only when
// State::source is set. Structural pixel check in the top strip (above the title).
#include "Test.h"
#include "ui/Canvas.h"
#include "ui/PngRenderTarget.h"
#include "ui/Screens.h"
#include "ui/Theme.h"

using namespace carsh::ui;

namespace {
int accentInTopStrip(const char* source) {
    carsh::host::PngRenderTarget rt(748, 432);
    Canvas c(rt);
    Theme th = Theme::dark();
    c.clear(th.bg);
    NowPlayingScreen now({0, 0, 748, 432});
    NowPlayingScreen::State st;
    st.title = "Song";
    st.source = source;  // badge text (or empty)
    now.setState(st);
    now.draw(c, th);
    // The badge sits at y=8 in the top-right; nothing else draws above y≈20.
    return c.countColor(0, 0, 748, 20, th.accent);
}
}  // namespace

TEST(NowPlayingSource, BadgeShownOnlyWhenSourceSet) {
    CHECK(accentInTopStrip("BLUETOOTH") > 0);  // badge rendered
    CHECK_EQ(accentInTopStrip(""), 0);          // local: no badge
}
