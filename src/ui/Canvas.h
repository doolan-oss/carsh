// Canvas — drawing primitives over an IRenderTarget (PLAN.md §2.2).
//
// The character-grid toolkit draws through here: filled rects, horizontal/
// vertical lines, glyph blitting from the 8x8 atlas, and monospace text. Colors
// are 0xAARRGGBB; alpha blending is supported so the accent tint and dim/bright
// layers compose. All coordinates are clipped to the current clip rect, which is
// the whole target unless a widget narrows it (ClipScope) to keep partially
// scrolled content inside its own bounds.
#pragma once

#include <cstdint>
#include <string>

#include "hal/IRenderTarget.h"
#include "ui/Font.h"

namespace carsh::ui {

// A clip rectangle in target pixels: [x0,x1) x [y0,y1).
struct ClipRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

class Canvas {
public:
    explicit Canvas(carsh::hal::IRenderTarget& rt) : rt_(rt) { resetClip(); }

    int width() const { return rt_.width(); }
    int height() const { return rt_.height(); }

    // Whole-target fill (ignores the clip rect).
    void clear(uint32_t color) {
        uint32_t* p = rt_.pixels();
        int n = rt_.width() * rt_.height();
        for (int i = 0; i < n; ++i) p[i] = color;
    }

    // Clip rect. Always kept inside the target (render targets are fixed-size),
    // so putPixel's single bounds check doubles as the target check.
    const ClipRect& clip() const { return clip_; }
    void setClip(const ClipRect& r) {
        clip_.x0 = r.x0 < 0 ? 0 : r.x0;
        clip_.y0 = r.y0 < 0 ? 0 : r.y0;
        clip_.x1 = r.x1 > rt_.width() ? rt_.width() : r.x1;
        clip_.y1 = r.y1 > rt_.height() ? rt_.height() : r.y1;
        if (clip_.x1 < clip_.x0) clip_.x1 = clip_.x0;  // empty, not inverted
        if (clip_.y1 < clip_.y0) clip_.y1 = clip_.y0;
    }
    void resetClip() { clip_ = {0, 0, rt_.width(), rt_.height()}; }

    void putPixel(int x, int y, uint32_t color) {
        if (x < clip_.x0 || y < clip_.y0 || x >= clip_.x1 || y >= clip_.y1) return;
        uint32_t* p = &rt_.pixels()[y * rt_.width() + x];
        uint8_t a = uint8_t(color >> 24);
        *p = (a == 0xFF) ? color : blend(*p, color, a);
    }

    void fillRect(int x, int y, int w, int h, uint32_t color) {
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) putPixel(x + i, y + j, color);
    }

    void rectOutline(int x, int y, int w, int h, uint32_t color) {
        for (int i = 0; i < w; ++i) { putPixel(x + i, y, color); putPixel(x + i, y + h - 1, color); }
        for (int j = 0; j < h; ++j) { putPixel(x, y + j, color); putPixel(x + w - 1, y + j, color); }
    }

    // Draw one glyph at pixel (x,y), optional scale. bg with alpha 0 = transparent.
    void drawChar(int x, int y, char ch, uint32_t fg, uint32_t bg = 0, int scale = 1) {
        for (int row = 0; row < kCellH; ++row) {
            uint8_t bits = glyphRow(ch, row);
            for (int col = 0; col < kCellW; ++col) {
                bool on = (bits >> col) & 1;
                uint32_t c = on ? fg : bg;
                if ((c >> 24) == 0) continue;  // transparent
                for (int sy = 0; sy < scale; ++sy)
                    for (int sx = 0; sx < scale; ++sx)
                        putPixel(x + col * scale + sx, y + row * scale + sy, c);
            }
        }
    }

    // Draw text left-to-right; returns the x past the last glyph.
    int drawText(int x, int y, const std::string& s, uint32_t fg,
                 uint32_t bg = 0, int scale = 1) {
        int cx = x;
        for (char ch : s) {
            drawChar(cx, y, ch, fg, bg, scale);
            cx += kCellW * scale;
        }
        return cx;
    }

    // Count a pixel of a given color in a region (used by tests to assert that a
    // highlight/text was actually drawn without brittle full-image diffs).
    int countColor(int x, int y, int w, int h, uint32_t color) const {
        int n = 0;
        const uint32_t* px = rt_.pixels();
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                int xx = x + i, yy = y + j;
                if (xx < 0 || yy < 0 || xx >= rt_.width() || yy >= rt_.height())
                    continue;
                if (px[yy * rt_.width() + xx] == color) ++n;
            }
        return n;
    }

private:
    static uint32_t blend(uint32_t dst, uint32_t src, uint8_t a) {
        auto ch = [](uint32_t v, int s) { return (v >> s) & 0xFF; };
        uint32_t r = (ch(src, 16) * a + ch(dst, 16) * (255 - a)) / 255;
        uint32_t g = (ch(src, 8) * a + ch(dst, 8) * (255 - a)) / 255;
        uint32_t b = (ch(src, 0) * a + ch(dst, 0) * (255 - a)) / 255;
        return 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    carsh::hal::IRenderTarget& rt_;
    ClipRect clip_;
};

// RAII: narrow the canvas clip to (x,y,w,h) — intersected with the current clip,
// so scopes nest — and restore the previous clip on exit.
class ClipScope {
public:
    ClipScope(Canvas& c, int x, int y, int w, int h) : c_(c), saved_(c.clip()) {
        ClipRect r{x, y, x + w, y + h};
        if (r.x0 < saved_.x0) r.x0 = saved_.x0;
        if (r.y0 < saved_.y0) r.y0 = saved_.y0;
        if (r.x1 > saved_.x1) r.x1 = saved_.x1;
        if (r.y1 > saved_.y1) r.y1 = saved_.y1;
        c_.setClip(r);
    }
    ~ClipScope() { c_.setClip(saved_); }
    ClipScope(const ClipScope&) = delete;
    ClipScope& operator=(const ClipScope&) = delete;

private:
    Canvas& c_;
    ClipRect saved_;
};

}  // namespace carsh::ui
