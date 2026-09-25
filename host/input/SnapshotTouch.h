// SnapshotTouch — host model of the Pi touch driver's SNAPSHOT semantics.
//
// The FT5406 firmware does not queue touch events: it overwrites one shared
// buffer (the current contact set) and Circle's CRPiTouchScreen::Update() diffs
// the latest buffer against the ids it already knows (rpitouchscreen.cpp, Circle
// Step51). Anything that exists only between two Update() calls is never seen.
// This class reproduces that diff exactly, so host tests can exercise what the
// UI really receives when the driver is pumped slower than the finger moves:
//
//   * a new id in the snapshot                -> Down (x,y)
//   * a known id at a new position            -> Move (x,y); unchanged -> nothing
//   * a known id missing from the snapshot    -> Up at (0,0) (no coordinates)
//   * no snapshot published since last read   -> nothing (NumPoints == 99)
//   * a lift + re-touch of the same id between reads -> a Move, not Up + Down
//   * a whole tap between reads               -> nothing at all
//
// The test is the firmware (touch()/lift() edit the contact set and publish it)
// and the pump (update() is one CTouchScreenDevice::Update()); poll() drains the
// events update() produced, like CircleTouch's queue.
#pragma once

#include <cstdint>
#include <deque>

#include "hal/Touch.h"

namespace carsh::host {

class SnapshotTouch : public carsh::hal::ITouch {
public:
    static constexpr int kMaxPoints = 10;  // RPITOUCH_SCREEN_MAX_POINTS

    // Firmware side: contact `id` (0..9) is on the panel at (x,y) / has lifted.
    // Each call publishes a fresh snapshot of the whole contact set.
    void touch(int id, int x, int y) {
        if (id < 0 || id >= kMaxPoints) return;
        cur_[id] = {true, x, y};
        published_ = true;
    }
    void lift(int id) {
        if (id < 0 || id >= kMaxPoints) return;
        cur_[id].present = false;
        published_ = true;
    }

    // Driver side: one Update() — diff the published snapshot against the known
    // ids and queue the resulting events (stamped tMs). Like the driver, all
    // Down/Move come first (here in ascending id order; the driver uses the
    // firmware's buffer order), then the Ups. Simplifications: a lifted contact
    // is simply absent (the firmware's FTS_TOUCH_UP entries delay the Up by one
    // read), and the panel->display coordinate mapping is the identity.
    void update(std::uint32_t tMs = 0) {
        if (!published_) return;  // NumPoints == 99: no new information
        published_ = false;
        unsigned seen = 0;
        for (int id = 0; id < kMaxPoints; ++id) {
            if (!cur_[id].present) continue;
            seen |= 1u << id;
            const Point& p = cur_[id];
            if (!(known_ & (1u << id))) {
                pos_[id] = p;
                queue_.push_back({carsh::hal::TouchPhase::Down, id, p.x, p.y, tMs});
            } else if (p.x != pos_[id].x || p.y != pos_[id].y) {
                pos_[id] = p;
                queue_.push_back({carsh::hal::TouchPhase::Move, id, p.x, p.y, tMs});
            }
        }
        const unsigned released = known_ & ~seen;
        for (int id = 0; id < kMaxPoints; ++id)
            if (released & (1u << id))
                queue_.push_back({carsh::hal::TouchPhase::Up, id, 0, 0, tMs});
        known_ = seen;
    }

    bool poll(carsh::hal::TouchEvent& out) override {
        if (queue_.empty()) return false;
        out = queue_.front();
        queue_.pop_front();
        return true;
    }
    bool empty() const { return queue_.empty(); }

private:
    struct Point {
        bool present = false;
        int x = 0, y = 0;
    };
    Point cur_[kMaxPoints];  // firmware buffer (latest published contact set)
    Point pos_[kMaxPoints];  // driver's last reported position per known id
    unsigned known_ = 0;     // driver's m_nKnownIDs
    bool published_ = false;
    std::deque<carsh::hal::TouchEvent> queue_;
};

}  // namespace carsh::host
