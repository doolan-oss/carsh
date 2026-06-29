// The seven carsh screens (PLAN.md §Phase 2.3). Each is a Screen subclass that
// lays out toolkit widgets, binds to injected view-model state, and emits intents
// via callbacks (so the screens stay decoupled from the controllers for host
// tests). Custom drawing (ASCII art, progress bar, EQ bars) overrides draw() and
// then defers to the widget pass.
#pragma once

#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "library/Art.h"
#include "dsp/Eq.h"
#include "ui/Screen.h"

namespace carsh::ui {

// --- 1. Now Playing ---------------------------------------------------------
class NowPlayingScreen : public Screen {
public:
    struct State {
        std::string title, artist, album;
        uint64_t positionMs = 0, durationMs = 0;
        bool playing = false;
        std::string source;  // active-source badge ("BLUETOOTH"); empty = local/hidden
    };

    explicit NowPlayingScreen(Rect c)
        : Screen(c),
          prev_({c.x + 400, c.y + 300, 90, 60}, "|<", [this] { if (onPrev) onPrev(); }),
          play_({c.x + 500, c.y + 300, 90, 60}, ">", [this] { if (onPlayPause) onPlayPause(); }),
          next_({c.x + 600, c.y + 300, 90, 60}, ">|", [this] { if (onNext) onNext(); }) {
        widgets_ = {&prev_, &play_, &next_};
    }
    const char* name() const override { return "Now"; }

    void setState(const State& s) {
        state_ = s;
        play_.setLabel(s.playing ? "||" : ">");
    }
    void setArt(const library::AsciiArt& a) { art_ = a; hasArt_ = true; }

    std::function<void()> onPrev, onPlayPause, onNext;

    void draw(Canvas& c, const Theme& th) override {
        // ASCII art panel.
        int ax = content_.x + 16, ay = content_.y + 24, scale = 2;
        c.fillRect(ax - 4, ay - 4, 22 * 8 * scale + 8, 11 * 8 * scale + 8, th.panel);
        if (hasArt_) {
            uint32_t tint = hal::rgba(art_.tintR, art_.tintG, art_.tintB);
            for (int r = 0; r < art_.rows; ++r)
                for (int col = 0; col < art_.cols; ++col)
                    c.drawChar(ax + col * 8 * scale, ay + r * 8 * scale,
                               art_.at(r, col), tint, 0, scale);
        }
        // Active-source badge (top-right), e.g. "BLUETOOTH" when the phone owns the
        // output; empty for the local player.
        if (!state_.source.empty()) {
            int sw = int(state_.source.size()) * kCellW;
            c.drawText(content_.x + content_.w - sw - 8, content_.y + 8, state_.source,
                       th.accent);
        }
        // Metadata.
        int tx = content_.x + 400, ty = content_.y + 24;
        c.drawText(tx, ty, state_.title, th.bright, 0, 2);
        c.drawText(tx, ty + 40, state_.artist, th.accent);
        c.drawText(tx, ty + 60, state_.album, th.dim);
        // Progress bar.
        int bx = tx, by = content_.y + 200, bw = 360, bh = 10;
        c.rectOutline(bx, by, bw, bh, th.dim);
        if (state_.durationMs > 0) {
            int fw = int(bw * double(state_.positionMs) / double(state_.durationMs));
            c.fillRect(bx, by, fw, bh, th.accent);
        }
        c.drawText(bx, by + 18, fmtTime(state_.positionMs) + " / " +
                                    fmtTime(state_.durationMs), th.dim);
        Screen::draw(c, th);
    }

private:
    static std::string fmtTime(uint64_t ms) {
        uint64_t s = ms / 1000;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llu:%02llu",
                      (unsigned long long)(s / 60), (unsigned long long)(s % 60));
        return buf;
    }
    State state_;
    library::AsciiArt art_;
    bool hasArt_ = false;
    Button prev_, play_, next_;
};

// --- 2. Library / 4. Queue / 6. Sources : list-driven screens ---------------
class ListScreen : public Screen {
public:
    ListScreen(Rect c, const char* nm)
        : Screen(c), name_(nm), list_({c.x + 8, c.y + 8, c.w - 16, c.h - 16}) {
        widgets_ = {&list_};
    }
    const char* name() const override { return name_; }
    void setItems(std::vector<std::string> items) { list_.setItems(std::move(items)); }
    void setOnSelect(std::function<void(int)> cb) { list_.setOnSelect(std::move(cb)); }
    ListView& list() { return list_; }

private:
    const char* name_;
    ListView list_;
};

// --- 3. Search --------------------------------------------------------------
class SearchScreen : public Screen {
public:
    explicit SearchScreen(Rect c)
        : Screen(c),
          query_({c.x + 8, c.y + 12, c.w - 16, kCellH * 2}, "search: ", 2),
          results_({c.x + 8, c.y + 56, c.w - 16, c.h - 64}) {
        widgets_ = {&query_, &results_};
    }
    const char* name() const override { return "Find"; }
    void setQuery(const std::string& q) { query_.setText("search: " + q); }
    void setResults(std::vector<std::string> r) { results_.setItems(std::move(r)); }
    void setOnSelect(std::function<void(int)> cb) { results_.setOnSelect(std::move(cb)); }
    ListView& results() { return results_; }

private:
    Label query_;
    ListView results_;
};

// --- 5. EQ ------------------------------------------------------------------
class EqScreen : public Screen {
public:
    explicit EqScreen(Rect c, double sampleRate = 48000)
        : Screen(c), sampleRate_(sampleRate) {}
    const char* name() const override { return "EQ"; }

    void setConfig(const dsp::EqConfig& cfg) { cfg_ = cfg; }
    const dsp::EqConfig& config() const { return cfg_; }
    // band, new gain dB.
    std::function<void(int, double)> onBandChange;

    void draw(Canvas& c, const Theme& th) override {
        int n = int(cfg_.bands.size());
        if (n == 0) return;
        int areaX = content_.x + 20, areaY = content_.y + 20;
        int areaW = content_.w - 40, areaH = content_.h - 80;
        int colW = areaW / n;
        int midY = areaY + areaH / 2;
        c.fillRect(areaX, midY, areaW, 1, th.dim);  // 0 dB line
        for (int i = 0; i < n; ++i) {
            int cx = areaX + i * colW + colW / 2;
            double g = cfg_.bands[std::size_t(i)].gainDb;  // clamp -12..12
            double frac = g / 12.0;
            int barH = int((areaH / 2) * frac);
            uint32_t col = th.accent;
            if (barH >= 0)
                c.fillRect(cx - 10, midY - barH, 20, barH, col);
            else
                c.fillRect(cx - 10, midY, 20, -barH, col);
            // freq label
            char lbl[8];
            std::snprintf(lbl, sizeof(lbl), "%d",
                          int(cfg_.bands[std::size_t(i)].freqHz));
            c.drawText(cx - int(strlen(lbl)) * 4, areaY + areaH + 8, lbl, th.dim);
        }
    }

    bool handle(const Gesture& g) override {
        int n = int(cfg_.bands.size());
        if (n == 0 || g.type != GestureType::Tap) return false;
        int areaX = content_.x + 20, areaY = content_.y + 20;
        int areaW = content_.w - 40, areaH = content_.h - 80;
        if (g.x < areaX || g.x >= areaX + areaW || g.y < areaY ||
            g.y >= areaY + areaH)
            return false;
        int band = (g.x - areaX) / (areaW / n);
        if (band < 0 || band >= n) return false;
        int midY = areaY + areaH / 2;
        double delta = (g.y < midY) ? +1.0 : -1.0;  // upper half boosts
        double ng = cfg_.bands[std::size_t(band)].gainDb + delta;
        if (ng > 12) ng = 12;
        if (ng < -12) ng = -12;
        cfg_.bands[std::size_t(band)].gainDb = ng;
        if (onBandChange) onBandChange(band, ng);
        return true;
    }

private:
    double sampleRate_;
    dsp::EqConfig cfg_;
};

// --- 7. Settings ------------------------------------------------------------
class SettingsScreen : public Screen {
public:
    struct Item { std::string label; std::string value; };
    explicit SettingsScreen(Rect c)
        : Screen(c), list_({c.x + 8, c.y + 8, c.w - 16, c.h - 16}) {
        widgets_ = {&list_};
        list_.setOnSelect([this](int i) { if (onActivate) onActivate(i); });
    }
    const char* name() const override { return "Set"; }
    void setItems(std::vector<Item> items) {
        items_ = std::move(items);
        refresh();
    }
    void setValue(int i, const std::string& v) {
        if (i >= 0 && i < int(items_.size())) { items_[i].value = v; refresh(); }
    }
    int selectedIndex() const { return list_.selectedIndex(); }
    std::function<void(int)> onActivate;

private:
    void refresh() {
        std::vector<std::string> rows;
        for (auto& it : items_) rows.push_back(it.label + ": " + it.value);
        list_.setItems(rows);
    }
    std::vector<Item> items_;
    ListView list_;
};

// --- 8. Devices (Bluetooth scan / connect) ----------------------------------
// A device list (one ListView) plus two buttons: SCAN (toggles to STOP while
// scanning) and a context-sensitive CONNECT/DISCONN. The app supplies formatted
// row strings + the connected index each frame; the screen stays pure-widget
// (no custom draw()/handle()) like ListScreen. Intent is exposed via callbacks.
class DevicesScreen : public Screen {
public:
    explicit DevicesScreen(Rect c)
        : Screen(c),
          list_({c.x + 8, c.y + 8, c.w - 16, c.h - 80}),
          scan_({c.x + 8, c.y + c.h - 64, 160, 56}, "SCAN",
                [this] { if (onScan) onScan(); }),
          action_({c.x + 180, c.y + c.h - 64, 200, 56}, "CONNECT", [this] {
              int i = list_.selectedIndex();
              if (i < 0) return;
              if (i == connectedIndex_) { if (onDisconnect) onDisconnect(); }
              else if (onConnect) onConnect(i);
          }) {
        widgets_ = {&list_, &scan_, &action_};
        list_.setOnSelect([this](int) { refreshAction(); });
    }
    const char* name() const override { return "BT"; }

    // View-model, rebuilt each frame from the app's device cache + conn state.
    void setDevices(const std::vector<std::string>& rows, int connectedIndex) {
        list_.setItems(rows);
        connectedIndex_ = connectedIndex;
        refreshAction();
    }
    void setScanning(bool s) { scan_.setLabel(s ? "STOP" : "SCAN"); }

    int selectedIndex() const { return list_.selectedIndex(); }
    const std::string& scanLabel() const { return scan_.label(); }
    const std::string& actionLabel() const { return action_.label(); }
    ListView& list() { return list_; }

    std::function<void()> onScan, onDisconnect;
    std::function<void(int)> onConnect;

private:
    void refreshAction() {
        int i = list_.selectedIndex();
        action_.setLabel((i >= 0 && i == connectedIndex_) ? "DISCONN" : "CONNECT");
    }
    ListView list_;
    Button scan_, action_;
    int connectedIndex_ = -1;
};

}  // namespace carsh::ui
