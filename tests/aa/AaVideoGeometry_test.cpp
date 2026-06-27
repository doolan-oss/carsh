// AaVideoGeometry tests: aspect-correct placement of the AA video frame inside the
// UI content rect, and the inverse touch mapping. Pure integer math, so this is
// fully host-side and pins the geometry the AndroidAuto screen + device decoder
// share (AA_PLAN "UI integration").
#include "aa/AaVideoGeometry.h"

#include "Test.h"

using carsh::aa::fitVideo;
using carsh::aa::mapToSource;
using carsh::aa::VideoFit;

namespace {
// The carsh content rect on the 800x480 panel: 52px volume rail, 48px tab bar.
constexpr int kCx = 52, kCy = 0, kCw = 748, kCh = 432;
}  // namespace

TEST(AaVideoGeometry, Fits480pCenteredInContentRect) {
    // 800x480 (5:3) into 748x432: the dest is relatively wider, so height binds.
    VideoFit f = fitVideo(800, 480, kCx, kCy, kCw, kCh);
    CHECK_EQ(f.dh, 432);
    CHECK_EQ(f.dw, 720);                 // 432 * 800 / 480
    CHECK_EQ(f.dx, kCx + (kCw - 720) / 2);  // pillarboxed: 52 + 14 = 66
    CHECK_EQ(f.dy, 0);
    // Stays within the content rect.
    CHECK(f.dx >= kCx && f.dy >= kCy);
    CHECK(f.dx + f.dw <= kCx + kCw);
    CHECK(f.dy + f.dh <= kCy + kCh);
}

TEST(AaVideoGeometry, PreservesAspectRatio) {
    VideoFit f = fitVideo(800, 480, kCx, kCy, kCw, kCh);
    // dw/dh == sw/sh  ->  dw*sh == dh*sw (within 1px rounding).
    long long lhs = (long long)f.dw * 480;
    long long rhs = (long long)f.dh * 800;
    long long diff = lhs > rhs ? lhs - rhs : rhs - lhs;
    CHECK(diff <= 480);  // <= one source row of slack
}

TEST(AaVideoGeometry, DegenerateInputsAreSafe) {
    CHECK_EQ(fitVideo(0, 480, kCx, kCy, kCw, kCh).dw, 0);
    CHECK_EQ(fitVideo(800, 0, kCx, kCy, kCw, kCh).dh, 0);
    CHECK_EQ(fitVideo(800, 480, kCx, kCy, 0, kCh).dw, 0);
    int sx, sy;
    VideoFit empty{kCx, kCy, 0, 0};
    CHECK(!mapToSource(empty, 800, 480, kCx, kCy, sx, sy));
}

TEST(AaVideoGeometry, TouchMapsBackToSourceCoords) {
    VideoFit f = fitVideo(800, 480, kCx, kCy, kCw, kCh);
    int sx, sy;

    // Top-left of the placed video -> source (0,0).
    REQUIRE(mapToSource(f, 800, 480, f.dx, f.dy, sx, sy));
    CHECK_EQ(sx, 0);
    CHECK_EQ(sy, 0);

    // Bottom-right pixel samples the last source pixels (nearest-neighbour: dest
    // col dw-1 -> src (dw-1)*sw/dw, never out of range).
    REQUIRE(mapToSource(f, 800, 480, f.dx + f.dw - 1, f.dy + f.dh - 1, sx, sy));
    CHECK(sx >= 798 && sx <= 799);
    CHECK(sy >= 478 && sy <= 479);

    // Center maps near the center of the source frame.
    REQUIRE(mapToSource(f, 800, 480, f.dx + f.dw / 2, f.dy + f.dh / 2, sx, sy));
    CHECK(sx >= 395 && sx <= 405);
    CHECK(sy >= 235 && sy <= 245);
}

TEST(AaVideoGeometry, TouchInLetterboxBarsIsRejected) {
    VideoFit f = fitVideo(800, 480, kCx, kCy, kCw, kCh);
    int sx, sy;
    // Left pillarbox bar (inside the content rect, left of the video).
    CHECK(!mapToSource(f, 800, 480, kCx + 1, kCh / 2, sx, sy));
    // Right pillarbox bar.
    CHECK(!mapToSource(f, 800, 480, f.dx + f.dw + 1, kCh / 2, sx, sy));
    // Above the content rect entirely.
    CHECK(!mapToSource(f, 800, 480, f.dx + 1, -5, sx, sy));
}
