// UI toolkit widgets (PLAN.md §2.2): char-grid Button, Label, ListView (with
// swipe scrolling + tap select), and TabBar. Widgets own their bounds, draw
// through Canvas+Theme, and handle taps/swipes via hit-testing. Enforces the
// 44x44 px minimum touch target as a layout helper, not in the driver.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ui/Canvas.h"
#include "ui/Theme.h"

namespace carsh::ui {

inline constexpr int kMinTouch = 44;  // minimum touch target (px)

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

class Widget {
public:
    explicit Widget(Rect b) : bounds(b) {}
    virtual ~Widget() = default;
    virtual void draw(Canvas&, const Theme&) = 0;
    virtual bool onTap(int, int) { return false; }
    virtual bool onSwipe(int /*dy*/) { return false; }
    Rect bounds;
    bool visible = true;
};

class Label : public Widget {
public:
    Label(Rect b, std::string text, int scale = 1)
        : Widget(b), text_(std::move(text)), scale_(scale) {}
    void setText(const std::string& t) { text_ = t; }
    const std::string& text() const { return text_; }
    void draw(Canvas& c, const Theme& th) override {
        c.drawText(bounds.x, bounds.y, text_, useDim_ ? th.dim : th.bright, 0,
                   scale_);
    }
    void setDim(bool d) { useDim_ = d; }

private:
    std::string text_;
    int scale_;
    bool useDim_ = false;
};

class Button : public Widget {
public:
    Button(Rect b, std::string label, std::function<void()> onPress)
        : Widget(b), label_(std::move(label)), onPress_(std::move(onPress)) {}

    void setLabel(const std::string& l) { label_ = l; }
    const std::string& label() const { return label_; }

    void draw(Canvas& c, const Theme& th) override {
        uint32_t fill = pressFlash_ > 0 ? th.accentBg : th.panel;
        c.fillRect(bounds.x, bounds.y, bounds.w, bounds.h, fill);
        c.rectOutline(bounds.x, bounds.y, bounds.w, bounds.h, th.accent);
        int tw = int(label_.size()) * kCellW;
        int tx = bounds.x + (bounds.w - tw) / 2;
        int ty = bounds.y + (bounds.h - kCellH) / 2;
        c.drawText(tx, ty, label_, th.bright);
        // Momentary press feedback: taps are discrete (no release event), so the
        // highlight decays over a few rendered frames instead of latching on.
        if (pressFlash_ > 0) --pressFlash_;
    }
    bool onTap(int px, int py) override {
        if (!bounds.contains(px, py)) return false;
        pressFlash_ = kPressFlashFrames;
        if (onPress_) onPress_();
        return true;
    }
    void setHighlight(bool h) { pressFlash_ = h ? kPressFlashFrames : 0; }

private:
    static constexpr int kPressFlashFrames = 8;  // ~0.25 s at the ~30 fps UI loop
    std::string label_;
    std::function<void()> onPress_;
    int pressFlash_ = 0;
};

// VolumeSlider — vertical level control. Top of the track = maximum, bottom = 0
// (mute). Driven by a raw drag (setFromY) for continuous feedback, since a slider
// needs to track the finger live, not wait for a completed-gesture tap.
class VolumeSlider : public Widget {
public:
    explicit VolumeSlider(Rect b) : Widget(b) {}

    // Raw rail position [0,1] — drives the visual fill and the % display.
    float value() const { return value_; }
    void setValue(float v) { value_ = v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

    // Perceptual (logarithmic) gain for the current position — this is what drives
    // the engine, NOT the raw position. Perceived loudness is ~log of amplitude, so
    // a linear position->gain map crams the usable range into the bottom of the
    // travel. A dB taper makes equal slider travel ≈ equal loudness change. The top
    // is capped (full rail = kMaxGain linear, not 1.0); 0 = mute.
    float gain() const {
        if (value_ <= 0.f) return 0.f;
        float g = kMaxGain * std::pow(10.f, (value_ - 1.f) * (kRangeDb / 20.f));
        return g > kMaxGain ? kMaxGain : g;  // hard ceiling (guard fp rounding)
    }

    // Map a touch y within the track to a position (top = 1, bottom = 0), clamped.
    void setFromY(int y) {
        if (bounds.h <= 0) return;
        setValue(float(bounds.y + bounds.h - y) / float(bounds.h));
    }

    void draw(Canvas& c, const Theme& th) override {
        c.fillRect(bounds.x, bounds.y, bounds.w, bounds.h, th.panel);
        int fh = int(value_ * float(bounds.h));
        int fy = bounds.y + bounds.h - fh;
        if (fh > 0) c.fillRect(bounds.x, fy, bounds.w, fh, th.accentBg);
        c.fillRect(bounds.x, fy - 1, bounds.w, 2, th.accent);  // level indicator
        c.drawText(bounds.x + 4, bounds.y + 4, "VOL", th.dim, 0, 1);
        c.drawText(bounds.x + 4, bounds.y + bounds.h - 12,
                   std::to_string(int(value_ * 100.f + 0.5f)), th.bright, 0, 1);
    }

private:
    static constexpr float kMaxGain = 0.5f;  // cap: full rail = 0.5 linear gain
    static constexpr float kRangeDb = 40.f;  // taper span, top to near-bottom (dB)
    float value_ = 0.0f;                      // boots muted
};

class ListView : public Widget {
public:
    ListView(Rect b, int rowH = kMinTouch) : Widget(b), rowH_(rowH) {}

    void setItems(std::vector<std::string> items) {
        items_ = std::move(items);
        if (selected_ >= int(items_.size())) selected_ = items_.empty() ? -1 : 0;
        clampScroll();
    }
    const std::vector<std::string>& items() const { return items_; }
    int selectedIndex() const { return selected_; }
    int scrollOffset() const { return scroll_; }
    void setOnSelect(std::function<void(int)> cb) { onSelect_ = std::move(cb); }

    int visibleRows() const { return bounds.h / rowH_; }

    void draw(Canvas& c, const Theme& th) override {
        c.fillRect(bounds.x, bounds.y, bounds.w, bounds.h, th.panel);
        int first = scroll_ / rowH_;
        for (int i = first; i < int(items_.size()); ++i) {
            int ry = bounds.y + i * rowH_ - scroll_;
            if (ry >= bounds.y + bounds.h) break;
            if (i == selected_)
                c.fillRect(bounds.x, ry, bounds.w, rowH_, th.accentBg);
            uint32_t fg = (i == selected_) ? th.accent : th.bright;
            c.drawText(bounds.x + 4, ry + (rowH_ - kCellH) / 2, items_[i], fg);
        }
    }
    bool onTap(int px, int py) override {
        if (!bounds.contains(px, py)) return false;
        int idx = (py - bounds.y + scroll_) / rowH_;
        if (idx >= 0 && idx < int(items_.size())) {
            selected_ = idx;
            if (onSelect_) onSelect_(idx);
        }
        return true;
    }
    bool onSwipe(int dy) override {
        scroll_ -= dy;  // swipe up (dy<0) scrolls content up (reveals later items)
        clampScroll();
        return true;
    }

private:
    void clampScroll() {
        int contentH = int(items_.size()) * rowH_;
        int maxScroll = std::max(0, contentH - bounds.h);
        scroll_ = std::clamp(scroll_, 0, maxScroll);
    }
    std::vector<std::string> items_;
    int rowH_;
    int selected_ = -1;
    int scroll_ = 0;
    std::function<void(int)> onSelect_;
};

class TabBar : public Widget {
public:
    TabBar(Rect b, std::vector<std::string> tabs)
        : Widget(b), tabs_(std::move(tabs)) {}

    int activeIndex() const { return active_; }
    void setActive(int i) { if (i >= 0 && i < int(tabs_.size())) active_ = i; }
    void setOnChange(std::function<void(int)> cb) { onChange_ = std::move(cb); }
    int tabCount() const { return int(tabs_.size()); }
    Rect tabRect(int i) const {
        int tw = bounds.w / int(tabs_.size());
        return {bounds.x + i * tw, bounds.y, tw, bounds.h};
    }

    void draw(Canvas& c, const Theme& th) override {
        c.fillRect(bounds.x, bounds.y, bounds.w, bounds.h, th.panel);
        for (int i = 0; i < int(tabs_.size()); ++i) {
            Rect r = tabRect(i);
            if (i == active_) {
                c.fillRect(r.x, r.y, r.w, r.h, th.accentBg);
                c.fillRect(r.x, r.y + r.h - 2, r.w, 2, th.accent);  // underline
            }
            int tw = int(tabs_[i].size()) * kCellW;
            c.drawText(r.x + (r.w - tw) / 2, r.y + (r.h - kCellH) / 2, tabs_[i],
                       i == active_ ? th.accent : th.dim);
        }
    }
    bool onTap(int px, int py) override {
        if (!bounds.contains(px, py)) return false;
        for (int i = 0; i < int(tabs_.size()); ++i)
            if (tabRect(i).contains(px, py)) {
                active_ = i;
                if (onChange_) onChange_(i);
                return true;
            }
        return false;
    }

private:
    std::vector<std::string> tabs_;
    int active_ = 0;
    std::function<void(int)> onChange_;
};

}  // namespace carsh::ui
