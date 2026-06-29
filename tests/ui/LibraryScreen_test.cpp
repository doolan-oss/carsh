// LibraryScreen — the browse tab's view layer: list rows + header + the
// BACK/PLAY/SHUFFLE button row. Drive it with scripted taps (via the screen's
// gesture routing) and assert the intents fire and the chrome reflects the state.
#include "ui/LibraryScreen.h"

#include <string>
#include <vector>

#include "Test.h"
#include "ui/Gesture.h"

using namespace carsh::ui;

namespace {
Gesture tap(int x, int y) {
    Gesture g;
    g.type = GestureType::Tap;
    g.x = x; g.y = y;
    return g;
}
}  // namespace

TEST(LibraryScreen, RowTapReportsIndex) {
    LibraryScreen s({0, 0, 748, 432});
    s.setItems({"Björk", "Pink Floyd", "Queen"});
    int picked = -1;
    s.onSelectRow = [&](int i) { picked = i; };
    // List starts at y=44, rowH=44; row 1 spans y∈[88,132).
    CHECK(s.handle(tap(20, 90)));
    CHECK_EQ(picked, 1);
    CHECK_EQ(s.list().selectedIndex(), 1);
}

TEST(LibraryScreen, PlayAndShuffleButtonsFire) {
    LibraryScreen s({0, 0, 748, 432});
    int plays = 0, shuffles = 0;
    s.onPlay = [&] { ++plays; };
    s.onShuffle = [&] { ++shuffles; };
    // Button row at y=368..424. PLAY x∈[126,306], SHUFFLE x∈[314,544].
    CHECK(s.handle(tap(200, 396)));
    CHECK(s.handle(tap(400, 396)));
    CHECK_EQ(plays, 1);
    CHECK_EQ(shuffles, 1);
}

TEST(LibraryScreen, BackHiddenAtRootIsNotTappable) {
    LibraryScreen s({0, 0, 748, 432});
    int backs = 0;
    s.onBack = [&] { ++backs; };

    s.setCanGoBack(false);            // root level: hide BACK
    CHECK(!s.backVisible());
    CHECK(!s.handle(tap(60, 396)));   // tap where BACK would be -> ignored
    CHECK_EQ(backs, 0);

    s.setCanGoBack(true);             // below root: BACK active
    CHECK(s.backVisible());
    CHECK(s.handle(tap(60, 396)));
    CHECK_EQ(backs, 1);
}

TEST(LibraryScreen, AzBarFiresJumpAndScrollTo) {
    LibraryScreen s({0, 0, 748, 432});
    char picked = 0;
    s.onJump = [&](char ch) { picked = ch; };
    // A–Z strip spans x∈[720,742], y∈[44,360]. Top slot -> 'A'.
    CHECK(s.handle(tap(725, 46)));
    CHECK_EQ(picked, 'A');
    // Near the bottom -> '#'.
    CHECK(s.handle(tap(725, 358)));
    CHECK_EQ(picked, '#');

    // scrollTo moves the list view + selects the row.
    std::vector<std::string> items;
    for (int i = 0; i < 60; ++i) items.push_back("row");
    s.setItems(items);
    s.scrollTo(30);
    CHECK(s.list().scrollOffset() > 0);
    CHECK_EQ(s.list().selectedIndex(), 30);
}

TEST(LibraryScreen, ScopeLabelsUpdate) {
    LibraryScreen s({0, 0, 748, 432});
    CHECK_EQ(s.playLabel(), std::string("PLAY ALL"));        // default
    s.setPlayLabels("PLAY ALBUM", "SHUFFLE ALBUM");
    CHECK_EQ(s.playLabel(), std::string("PLAY ALBUM"));
    CHECK_EQ(s.shuffleLabel(), std::string("SHUFFLE ALBUM"));
}
