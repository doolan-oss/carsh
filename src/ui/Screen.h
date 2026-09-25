// Screen + ScreenManager (PLAN.md §2.2, Phase 2). A Screen owns its widgets and
// routes gestures to them by hit-testing. ScreenManager hosts the tab bar and
// the active screen, converts raw touch into gestures, and renders the frame.
//
// Touch routing is per contact. The Down that starts a touch latches its contact
// id and picks one owner for the whole contact: the volume rail (live drag), a
// screen that opted into raw touch (the AA projection streams PRESS/DRAG/RELEASE),
// or the gesture recognizer (taps + swipes for the tab bar and widgets). Events
// from other contact ids are ignored until the latched contact's Up, so a second
// finger or a palm can't hijack the rail or steal a tap. Switching tabs cancels
// the contact in progress; its remaining events are swallowed. resetTouch() is
// the recovery path when the driver lost an event (see its comment).
#pragma once

#include <memory>
#include <vector>

#include "ui/Gesture.h"
#include "ui/Widgets.h"

namespace carsh::ui {

class Screen {
public:
    explicit Screen(Rect content) : content_(content) {}
    virtual ~Screen() = default;
    virtual const char* name() const = 0;

    // Called when this screen's tab becomes / stops being the active one. Most
    // screens ignore these; the Android Auto screen uses them to drive the AA
    // VIDEO_FOCUS_INDICATION so the phone only streams frames we're showing.
    virtual void onActivate() {}
    virtual void onDeactivate() {}

    // Raw touch opt-in, for a screen that must track the finger live instead of
    // waiting for a completed gesture (the AA projection). When wantsRawTouch()
    // is true, ScreenManager offers the screen each contact's Down (outside the
    // volume rail and the tab bar) via onRawTouch(). Returning true captures the
    // contact: its Moves and Up go to onRawTouch() too, and never reach the
    // gesture recognizer. Returning false leaves the contact to the gesture path.
    // NB: the Pi driver reports Up at (0,0) — use the last Down/Move position.
    virtual bool wantsRawTouch() const { return false; }
    virtual bool onRawTouch(const carsh::hal::TouchEvent&) { return false; }
    // A captured raw contact was abandoned before its Up (tab switch mid-touch).
    virtual void onRawTouchCancel() {}

    virtual void draw(Canvas& c, const Theme& th) {
        for (Widget* w : widgets_)
            if (w->visible) w->draw(c, th);
    }
    // Route a gesture to the widget under it. Returns handled. A tap goes to the
    // widget under the release point; a swipe to the widget the finger went DOWN
    // on (its start point) — a list drag routinely ends outside the list, on the
    // header, the button row or the tab bar, and must still scroll it.
    virtual bool handle(const Gesture& g) {
        for (Widget* w : widgets_) {
            if (!w->visible) continue;
            if (g.type == GestureType::Tap && w->bounds.contains(g.x, g.y)) {
                if (w->onTap(g.x, g.y)) return true;
            } else if (g.type == GestureType::Swipe &&
                       w->bounds.contains(g.sx, g.sy)) {
                if (w->onSwipe(g.dy)) return true;
            }
        }
        return false;
    }

protected:
    Rect content_;
    std::vector<Widget*> widgets_;  // non-owning; subclasses own storage
};

class ScreenManager {
public:
    ScreenManager(int w, int h, Theme theme = Theme::dark())
        : w_(w), h_(h), theme_(theme),
          tabs_({0, h - kTabH, w, kTabH}, {}),
          volume_({0, 0, kRailW, h - kTabH}) {}

    // contentRect is what screens lay out within: inset on the left by the volume
    // rail, above the (full-width) tab bar.
    Rect contentRect() const { return {kRailW, 0, w_ - kRailW, h_ - kTabH}; }

    // Master volume in [0,1]; boots at 0 (muted). The left rail drives this.
    void setOnVolumeChange(std::function<void(float)> cb) {
        onVolumeChange_ = std::move(cb);
    }
    float volume() const { return volume_.value(); }
    void setVolume(float v) { volume_.setValue(v); }
    // The dB-mapped output gain for the current rail position (same value the
    // onVolumeChange callback delivers); used to seed the master gain at boot.
    float volumeGain() const { return volume_.gain(); }

    void addScreen(Screen* s, const std::string& tabLabel) {
        screens_.push_back(s);
        tabLabels_.push_back(tabLabel);
        tabs_ = TabBar({0, h_ - kTabH, w_, kTabH}, tabLabels_);
        tabs_.setActive(active_);
    }

    void setTheme(const Theme& t) { theme_ = t; }
    const Theme& theme() const { return theme_; }

    int activeIndex() const { return active_; }
    // Switching tabs cancels the touch in progress (gesture, rail drag or raw
    // capture): the kernel switches on its own (AA auto-show, AA disconnect), and
    // a contact that began on the old screen must not complete on the new one.
    void setActive(int i) {
        if (i < 0 || i >= int(screens_.size()) || i == active_) return;
        cancelTouch();  // while the old screen is still active (raw cancel)
        if (screens_[active_]) screens_[active_]->onDeactivate();
        active_ = i;
        tabs_.setActive(i);
        if (screens_[active_]) screens_[active_]->onActivate();
    }
    Screen* activeScreen() {
        return screens_.empty() ? nullptr : screens_[active_];
    }

    void render(Canvas& c) {
        c.clear(theme_.bg);
        if (Screen* s = activeScreen()) s->draw(c, theme_);
        volume_.draw(c, theme_);
        tabs_.draw(c, theme_);
    }

    // Feed a raw touch event. The Down picks the contact's owner (rail, raw
    // screen, or gesture recognizer); on a completed gesture, route to the tab bar
    // first (tap to switch screens), otherwise to the active screen.
    void onTouch(const carsh::hal::TouchEvent& e) {
        using P = carsh::hal::TouchPhase;
        if (e.phase == P::Down) {
            if (capture_ != Capture::None) {
                // Primary-contact latch: a second finger is ignored until the first
                // lifts. A Down on the latched id itself means its Up was lost (the
                // driver reports a re-touch between reads as a Move), so restart.
                if (e.id != captureId_) return;
                cancelTouch();
            }
            captureId_ = e.id;
            // Volume rail: track raw down/move for a live drag (a slider needs
            // continuous feedback, not a completed-gesture tap).
            if (volume_.bounds.contains(e.x, e.y)) {
                capture_ = Capture::Rail;
                volume_.setFromY(e.y);
                if (onVolumeChange_) onVolumeChange_(volume_.gain());
                return;
            }
            // A raw-touch screen may claim the contact (never on the tab bar, so
            // tabs stay switchable from every screen).
            Screen* s = activeScreen();
            if (s && s->wantsRawTouch() && !tabs_.bounds.contains(e.x, e.y) &&
                s->onRawTouch(e)) {
                capture_ = Capture::Raw;
                return;
            }
            capture_ = Capture::Gesture;
            Gesture g;
            recognizer_.feed(e, g);
            return;
        }

        // Move / Up: only the latched contact counts.
        if (capture_ == Capture::None || e.id != captureId_) return;
        const Capture cap = capture_;
        if (e.phase == P::Up) {
            // Release the latch BEFORE dispatching: a tab tap below switches
            // screens, and that switch must not see this contact as in progress.
            capture_ = Capture::None;
            captureId_ = -1;
        }
        switch (cap) {
            case Capture::Rail:
                // The Pi driver reports Up at (0,0), so Up only ENDS the drag —
                // its coordinates are never read.
                if (e.phase == P::Move) {
                    volume_.setFromY(e.y);
                    if (onVolumeChange_) onVolumeChange_(volume_.gain());
                }
                break;
            case Capture::Raw:
                if (Screen* s = activeScreen()) s->onRawTouch(e);
                break;
            case Capture::Gesture: {
                Gesture g;
                if (recognizer_.feed(e, g)) dispatch(g);
                break;
            }
            case Capture::Swallow:  // cancelled by a tab switch: drop the rest
            case Capture::None:
                break;
        }
    }

    // True while a contact is being tracked (by any owner, incl. a cancelled
    // contact whose remaining events are being swallowed).
    bool touchActive() const { return capture_ != Capture::None; }

    // Recovery after the input path lost a Down/Up (the device's touch ring was
    // full). Cancels the contact in progress exactly as a tab switch does, then
    // also drops the latch: the lost event may be the latched contact's Up, and
    // swallowing until that Up would block every later touch. Events of a contact
    // still down are ignored until the next Down, which starts cleanly.
    void resetTouch() {
        cancelTouch();
        capture_ = Capture::None;
        captureId_ = -1;
    }

    TabBar& tabBar() { return tabs_; }

private:
    // Who owns the contact in progress (see the file header).
    enum class Capture { None, Rail, Raw, Gesture, Swallow };

    void dispatch(const Gesture& g) {
        if (g.type == GestureType::Tap && tabs_.bounds.contains(g.x, g.y)) {
            if (tabs_.onTap(g.x, g.y)) {
                setActive(tabs_.activeIndex());  // fires onDeactivate/onActivate
                return;
            }
        }
        if (Screen* s = activeScreen()) s->handle(g);
    }

    // Abandon the contact in progress; the rest of it (up to its Up) is dropped.
    void cancelTouch() {
        switch (capture_) {
            case Capture::Raw:
                if (Screen* s = activeScreen()) s->onRawTouchCancel();
                break;
            case Capture::Gesture:
                recognizer_.cancel();
                break;
            case Capture::Rail:
            case Capture::Swallow:
            case Capture::None:
                break;
        }
        if (capture_ != Capture::None) capture_ = Capture::Swallow;
    }

    static constexpr int kTabH = 48;
    static constexpr int kRailW = 52;  // left volume rail width (>= touch target)
    int w_, h_;
    Theme theme_;
    std::vector<Screen*> screens_;
    std::vector<std::string> tabLabels_;
    TabBar tabs_;
    VolumeSlider volume_;
    Capture capture_ = Capture::None;
    int captureId_ = -1;  // latched contact id while capture_ != None
    std::function<void(float)> onVolumeChange_;
    GestureRecognizer recognizer_;
    int active_ = 0;
};

}  // namespace carsh::ui
