// LibraryScreen — the local-library browse tab: a header (breadcrumb), a scrolling
// list of the current level's rows (artists / albums / songs), and a button row
// [BACK] [PLAY ...] [SHUFFLE ...]. It is a thin view over LibraryBrowser: the app
// pushes title/items/scope each time the browser changes and forwards the four
// intents (row tap, back, play, shuffle) to it. Pure-widget like DevicesScreen, so
// it is host-tested for layout + intent without hardware.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ui/Screen.h"

namespace carsh::ui {

class LibraryScreen : public Screen {
public:
    explicit LibraryScreen(Rect c)
        : Screen(c),
          header_({c.x + 8, c.y + 10, c.w - 16, kCellH * 2}, "Artists", 2),
          list_({c.x + 8, c.y + 44, c.w - 16, c.h - 44 - 72}),
          back_({c.x + 8, c.y + c.h - 64, 110, 56}, "BACK",
                [this] { if (onBack) onBack(); }),
          play_({c.x + 126, c.y + c.h - 64, 180, 56}, "PLAY ALL",
                [this] { if (onPlay) onPlay(); }),
          shuffle_({c.x + 314, c.y + c.h - 64, 230, 56}, "SHUFFLE ALL",
                   [this] { if (onShuffle) onShuffle(); }) {
        widgets_ = {&header_, &list_, &back_, &play_, &shuffle_};
        list_.setOnSelect([this](int i) { if (onSelectRow) onSelectRow(i); });
    }
    const char* name() const override { return "Lib"; }

    // View-model, pushed by the app whenever the browser navigates.
    void setTitle(const std::string& t) { header_.setText(t); }
    void setItems(std::vector<std::string> rows) { list_.setItems(std::move(rows)); }
    // BACK is only meaningful below the artist root; hide it there.
    void setCanGoBack(bool b) { back_.visible = b; }
    // Scope-aware captions, e.g. "PLAY ALBUM" / "SHUFFLE ARTIST".
    void setPlayLabels(const std::string& play, const std::string& shuffle) {
        play_.setLabel(play);
        shuffle_.setLabel(shuffle);
    }

    ListView& list() { return list_; }
    const std::string& playLabel() const { return play_.label(); }
    const std::string& shuffleLabel() const { return shuffle_.label(); }
    bool backVisible() const { return back_.visible; }

    // Intents.
    std::function<void(int)> onSelectRow;
    std::function<void()> onBack, onPlay, onShuffle;

private:
    Label header_;
    ListView list_;
    Button back_, play_, shuffle_;
};

}  // namespace carsh::ui
