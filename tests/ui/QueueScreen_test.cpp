// QueueScreen — the play-queue tab: list rows + now-playing marker + CLEAR. Drive
// it with scripted taps and assert the intents fire.
#include "ui/Screens.h"

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

TEST(QueueScreen, RowTapJumpsAndCurrentMarks) {
    QueueScreen q({0, 0, 748, 432});
    q.setItems({"A - 1", "B - 2", "C - 3"});
    int jumped = -1;
    q.onSelect = [&](int i) { jumped = i; };

    // List starts at y=8, rowH=44; row 1 spans y∈[52,96).
    CHECK(q.handle(tap(20, 60)));
    CHECK_EQ(jumped, 1);

    q.setCurrent(2);
    CHECK_EQ(q.list().current(), 2);
}

TEST(QueueScreen, ClearButtonFires) {
    QueueScreen q({0, 0, 748, 432});
    q.setItems({"A - 1"});
    int clears = 0;
    q.onClear = [&] { ++clears; };
    // CLEAR at {8, 368, 160, 56} -> center ~ (88, 396).
    CHECK(q.handle(tap(88, 396)));
    CHECK_EQ(clears, 1);
}
