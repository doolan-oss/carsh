// Screen + ScreenManager (PLAN.md §2.2, Phase 2). A Screen owns its widgets and
// routes gestures to them by hit-testing. ScreenManager hosts the tab bar and
// the active screen, converts raw touch into gestures, and renders the frame.
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

    virtual void draw(Canvas& c, const Theme& th) {
        for (Widget* w : widgets_)
            if (w->visible) w->draw(c, th);
    }
    // Route a gesture to the widget under it. Returns handled.
    virtual bool handle(const Gesture& g) {
        for (Widget* w : widgets_) {
            if (!w->visible) continue;
            if (g.type == GestureType::Tap && w->bounds.contains(g.x, g.y)) {
                if (w->onTap(g.x, g.y)) return true;
            } else if (g.type == GestureType::Swipe &&
                       w->bounds.contains(g.x, g.y)) {
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
          tabs_({0, h - kTabH, w, kTabH}, {}) {}

    // contentRect is what screens should lay out within (above the tab bar).
    Rect contentRect() const { return {0, 0, w_, h_ - kTabH}; }

    void addScreen(Screen* s, const std::string& tabLabel) {
        screens_.push_back(s);
        tabLabels_.push_back(tabLabel);
        tabs_ = TabBar({0, h_ - kTabH, w_, kTabH}, tabLabels_);
        tabs_.setActive(active_);
    }

    void setTheme(const Theme& t) { theme_ = t; }
    const Theme& theme() const { return theme_; }

    int activeIndex() const { return active_; }
    void setActive(int i) {
        if (i >= 0 && i < int(screens_.size())) {
            active_ = i;
            tabs_.setActive(i);
        }
    }
    Screen* activeScreen() {
        return screens_.empty() ? nullptr : screens_[active_];
    }

    void render(Canvas& c) {
        c.clear(theme_.bg);
        if (Screen* s = activeScreen()) s->draw(c, theme_);
        tabs_.draw(c, theme_);
    }

    // Feed a raw touch event; on a completed gesture, route to the tab bar first
    // (tap to switch screens), otherwise to the active screen.
    void onTouch(const carsh::hal::TouchEvent& e) {
        Gesture g;
        if (!recognizer_.feed(e, g)) return;
        if (g.type == GestureType::Tap && tabs_.bounds.contains(g.x, g.y)) {
            if (tabs_.onTap(g.x, g.y)) {
                active_ = tabs_.activeIndex();
                return;
            }
        }
        if (Screen* s = activeScreen()) s->handle(g);
    }

    TabBar& tabBar() { return tabs_; }

private:
    static constexpr int kTabH = 48;
    int w_, h_;
    Theme theme_;
    std::vector<Screen*> screens_;
    std::vector<std::string> tabLabels_;
    TabBar tabs_;
    GestureRecognizer recognizer_;
    int active_ = 0;
};

}  // namespace carsh::ui
