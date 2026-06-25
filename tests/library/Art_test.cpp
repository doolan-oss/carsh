// Cover-art + ASCII generator tests. Synthesize PNGs in memory (stb_image_write),
// decode them (stb_image), and check downscale, ASCII mapping, tint, and the
// .ascii cache round-trip.
#include "library/Art.h"

#include <vector>

#include "Test.h"
#include "img/PngCodec.h"

using namespace carsh;
using namespace carsh::library;

// Build a w*h RGBA image with a callback giving each pixel's color.
template <typename Fn>
static std::vector<uint8_t> makeRgba(int w, int h, Fn fn) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = &px[(size_t(y) * w + x) * 4];
            fn(x, y, p);
        }
    return px;
}

TEST(Art, DecodePngRoundTrip) {
    auto rgba = makeRgba(8, 4, [](int x, int, uint8_t* p) {
        p[0] = uint8_t(x * 30); p[1] = 10; p[2] = 200; p[3] = 255;
    });
    auto png = host::encodePng(rgba.data(), 8, 4);
    REQUIRE(!png.empty());
    Image img;
    REQUIRE(decodeImage(png.data(), png.size(), img));
    CHECK_EQ(img.w, 8);
    CHECK_EQ(img.h, 4);
    // Pixel (3,0) should be roughly (90,10,200).
    const uint8_t* p = &img.rgba[(0 * 8 + 3) * 4];
    CHECK_EQ(int(p[0]), 90);
    CHECK_EQ(int(p[2]), 200);
}

TEST(Art, AsciiMapsLuminanceToRamp) {
    // Left half black, right half white.
    int w = 64, h = 32;
    auto rgba = makeRgba(w, h, [w](int x, int, uint8_t* p) {
        uint8_t v = x < w / 2 ? 0 : 255;
        p[0] = p[1] = p[2] = v; p[3] = 255;
    });
    auto png = host::encodePng(rgba.data(), w, h);
    Image img;
    REQUIRE(decodeImage(png.data(), png.size(), img));

    AsciiArt art = generateAscii(img, 22, 11);
    CHECK_EQ(art.cols, 22);
    CHECK_EQ(art.rows, 11);
    // Far-left cell is darkest ramp char; far-right is brightest.
    CHECK_EQ(art.at(5, 0), ' ');
    CHECK_EQ(art.at(5, 21), '@');
}

TEST(Art, DominantColorPicksVividHue) {
    // Mostly gray with a vivid red region — saturation weighting should pull
    // the tint toward red.
    int w = 40, h = 40;
    auto rgba = makeRgba(w, h, [](int x, int y, uint8_t* p) {
        if (x < 10 && y < 10) { p[0] = 230; p[1] = 20; p[2] = 20; }
        else { p[0] = p[1] = p[2] = 128; }
        p[3] = 255;
    });
    auto png = host::encodePng(rgba.data(), w, h);
    Image img;
    REQUIRE(decodeImage(png.data(), png.size(), img));
    uint32_t c = dominantColor(img);
    uint8_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    CHECK(r > g + 20);  // red dominates
    CHECK(r > b + 20);
}

TEST(Art, AsciiCacheRoundTrip) {
    AsciiArt a;
    a.rows = 2; a.cols = 3;
    a.cells = "ab cd ";  // 6 chars
    a.tintR = 217; a.tintG = 119; a.tintB = 87;  // carsh accent #d97757
    auto blob = a.serialize();
    AsciiArt b;
    REQUIRE(AsciiArt::deserialize(blob.data(), blob.size(), b));
    CHECK_EQ(b.rows, 2);
    CHECK_EQ(b.cols, 3);
    CHECK_EQ(b.cells, std::string("ab cd "));
    CHECK_EQ(int(b.tintR), 217);
    CHECK_EQ(int(b.tintB), 87);
}
