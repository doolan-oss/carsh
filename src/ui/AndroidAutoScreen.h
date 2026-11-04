// AndroidAutoScreen — the Android Auto projection tab. It holds the latest
// decoded video frame (RGB888 at the source resolution, e.g. 800x480) and
// scale-blits it aspect-correct into the content rect, so the left volume rail and
// the bottom tab bar stay visible by construction (AA_PLAN "UI integration"). The
// frame is a borrowed pointer rebuilt by the app each render (same view-model
// pattern as DevicesScreen). Tab focus changes fire onFocusChanged so the app can
// drive VIDEO_FOCUS_INDICATION.
//
// Touch: the phone does its own gesture recognition (scroll, fling, long-press,
// map pan), so it needs the raw finger track, not our completed taps/swipes. The
// screen opts into ScreenManager's raw-touch path and streams the contact via
// onTouchAction, in source-frame coordinates: Press on a Down inside the video,
// Drag on each Move (clamped to the video rect, so a finger sliding off the
// picture keeps dragging along its edge), Release on the Up at the last mapped
// point (the Pi driver reports Up at (0,0)). Downs outside the video (letterbox,
// no frame yet) aren't claimed; the rail and the tab bar never reach this path.
// Only the primary contact is forwarded (ScreenManager latches one contact id);
// multi-pointer AA (POINTER_DOWN/POINTER_UP with pointer ids, for pinch-zoom) is
// future work.
//
// Decode + the live transport are hardware work (AA_PLAN rungs 5-6); this screen
// is the host-testable presentation/geometry layer they plug into.
#pragma once

#include <cstdint>
#include <functional>

#include "aa/AaTypes.h"
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

    // The primary contact on the video, as AA touch actions in source-frame coords
    // (0..sw-1 / 0..sh-1): Press, then Drag per moved point, then Release. Every
    // Press is paired with exactly one Release (incl. on a tab switch mid-touch).
    // The app timestamps + forwards each to the AA input channel.
    std::function<void(carsh::aa::TouchAction action, int sx, int sy)> onTouchAction;
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

    // Touch reaches the phone only through the raw path below. A completed
    // gesture here started outside the video (letterbox, placeholder): drop it.
    bool handle(const Gesture&) override { return false; }

    bool wantsRawTouch() const override { return true; }

    bool onRawTouch(const carsh::hal::TouchEvent& e) override {
        using P = carsh::hal::TouchPhase;
        using A = carsh::aa::TouchAction;
        int sx = 0, sy = 0;
        switch (e.phase) {
            case P::Down:
                // Claim the contact only if it lands on the picture.
                if (!hasVideo() ||
                    !aa::mapToSource(fit(), sw_, sh_, e.x, e.y, sx, sy))
                    return false;
                if (pressed_) release();  // lost Up: never stack two Presses
                pressed_ = true;
                lastSx_ = sx;
                lastSy_ = sy;
                emit(A::Press, sx, sy);
                return true;
            case P::Move:
                if (!pressed_) return false;
                // Video gone mid-drag (reset): hold the last point for Release.
                if (!mapClamped(e.x, e.y, sx, sy)) return true;
                if (sx == lastSx_ && sy == lastSy_) return true;  // clamped, no motion
                lastSx_ = sx;
                lastSy_ = sy;
                emit(A::Drag, sx, sy);
                return true;
            case P::Up:
                if (!pressed_) return false;
                release();
                return true;
        }
        return false;
    }

    // Abandoned mid-touch (tab switch). AA has no CANCEL action (aasdk
    // TouchAction: PRESS/RELEASE/DRAG/POINTER_*), so end it with a Release at
    // the last point rather than leave the phone's pointer stuck down (which
    // would fire a long-press). In practice only an AA disconnect switches tabs
    // away mid-touch (the kernel returns to Now Playing; a tab tap can't, since
    // this contact owns the touch), so this rarely reaches a live phone.
    void onRawTouchCancel() override {
        if (pressed_) release();
    }

    bool pressed() const { return pressed_; }

    void onActivate() override {
        focused_ = true;
        if (onFocusChanged) onFocusChanged(true);
    }
    void onDeactivate() override {
        if (pressed_) release();  // belt and braces; ScreenManager cancels first
        focused_ = false;
        if (onFocusChanged) onFocusChanged(false);
    }

private:
    bool hasVideo() const { return frame_ && sw_ > 0 && sh_ > 0; }

    // Map a panel point to source coords, clamping it onto the placed video rect
    // first (a drag that slides off the picture tracks along its edge).
    bool mapClamped(int px, int py, int& sx, int& sy) const {
        const aa::VideoFit f = fit();
        if (f.dw <= 0 || f.dh <= 0) return false;
        if (px < f.dx) px = f.dx;
        if (px > f.dx + f.dw - 1) px = f.dx + f.dw - 1;
        if (py < f.dy) py = f.dy;
        if (py > f.dy + f.dh - 1) py = f.dy + f.dh - 1;
        return aa::mapToSource(f, sw_, sh_, px, py, sx, sy);
    }

    void release() {
        pressed_ = false;
        emit(carsh::aa::TouchAction::Release, lastSx_, lastSy_);
    }

    void emit(carsh::aa::TouchAction a, int sx, int sy) {
        if (onTouchAction) onTouchAction(a, sx, sy);
    }

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
    // Raw contact in progress (Press sent, Release pending) + its last point.
    bool pressed_ = false;
    int lastSx_ = 0, lastSy_ = 0;
};

}  // namespace carsh::ui
