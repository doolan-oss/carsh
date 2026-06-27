// AndroidAutoScreen — the Android Auto projection tab. It holds the latest
// decoded video frame (RGB888 at the source resolution, e.g. 800x480) and
// scale-blits it aspect-correct into the content rect, so the left volume rail and
// the bottom tab bar stay visible by construction (AA_PLAN "UI integration"). The
// frame is a borrowed pointer rebuilt by the app each render (same view-model
// pattern as DevicesScreen). Touches inside the video are remapped to source-frame
// coordinates and forwarded via onTouch for the AA input channel; tab focus
// changes fire onFocusChanged so the app can drive VIDEO_FOCUS_INDICATION.
//
// Decode + the live transport are hardware work (AA_PLAN rungs 5-6); this screen
// is the host-testable presentation/geometry layer they plug into.
#pragma once

#include <cstdint>
#include <functional>

#include "aa/AaVideoGeometry.h"
#include "ui/Screen.h"

namespace carsh::ui {

class AndroidAutoScreen : public Screen {
public:
    explicit AndroidAutoScreen(Rect c) : Screen(c) {}
    const char* name() const override { return "AA"; }

    // Latest decoded frame: row-major RGB888, sw*sh*3 bytes, valid until the next
    // call. Pass nullptr (or 0 dims) to show the placeholder instead.
    void setFrame(const std::uint8_t* rgb888, int sw, int sh) {
        frame_ = rgb888;
        sw_ = sw;
        sh_ = sh;
    }
    void clearFrame() { setFrame(nullptr, 0, 0); }

    // Connection state, for the placeholder copy (dongle plugged + projecting?).
    void setConnected(bool v) { connected_ = v; }
    bool connected() const { return connected_; }

    bool focused() const { return focused_; }

    // Where the video is placed inside the content rect (absolute panel coords).
    aa::VideoFit fit() const {
        return aa::fitVideo(sw_, sh_, content_.x, content_.y, content_.w, content_.h);
    }

    // A touch landed on the video, in source-frame coords (0..sw-1 / 0..sh-1). The
    // app forwards it to the AA input channel.
    std::function<void(int sx, int sy, GestureType type)> onTouch;
    // The AA tab gained (true) or lost (false) focus -> drive VIDEO_FOCUS.
    std::function<void(bool focused)> onFocusChanged;

    void draw(Canvas& c, const Theme& th) override {
        c.fillRect(content_.x, content_.y, content_.w, content_.h, th.bg);
        if (!frame_ || sw_ <= 0 || sh_ <= 0) {
            drawPlaceholder(c, th);
            return;
        }
        const aa::VideoFit f = fit();
        // Nearest-neighbour scale-blit. The device backend will replace this with a
        // NEON path, but the mapping (and thus touch geometry) is identical.
        for (int y = 0; y < f.dh; ++y) {
            const int syf = static_cast<int>(static_cast<long long>(y) * sh_ / f.dh);
            const std::uint8_t* srow = frame_ + static_cast<std::size_t>(syf) * sw_ * 3;
            for (int x = 0; x < f.dw; ++x) {
                const int sxf = static_cast<int>(static_cast<long long>(x) * sw_ / f.dw);
                const std::uint8_t* p = srow + static_cast<std::size_t>(sxf) * 3;
                c.putPixel(f.dx + x, f.dy + y, carsh::hal::rgba(p[0], p[1], p[2]));
            }
        }
    }

    bool handle(const Gesture& g) override {
        const aa::VideoFit f = fit();
        int sx = 0, sy = 0;
        if (aa::mapToSource(f, sw_, sh_, g.x, g.y, sx, sy)) {
            if (onTouch) onTouch(sx, sy, g.type);
            return true;
        }
        return false;
    }

    void onActivate() override {
        focused_ = true;
        if (onFocusChanged) onFocusChanged(true);
    }
    void onDeactivate() override {
        focused_ = false;
        if (onFocusChanged) onFocusChanged(false);
    }

private:
    void drawPlaceholder(Canvas& c, const Theme& th) {
        const char* msg = connected_ ? "Android Auto starting..."
                                     : "Plug in the Android Auto dongle";
        int len = 0;
        for (const char* p = msg; *p; ++p) ++len;
        const int scale = 2;
        const int tw = len * kCellW * scale;
        const int tx = content_.x + (content_.w - tw) / 2;
        const int ty = content_.y + content_.h / 2 - (kCellH * scale) / 2;
        c.drawText(tx, ty, msg, connected_ ? th.accent : th.dim, 0, scale);
    }

    const std::uint8_t* frame_ = nullptr;
    int sw_ = 0, sh_ = 0;
    bool connected_ = false;
    bool focused_ = false;
};

}  // namespace carsh::ui
