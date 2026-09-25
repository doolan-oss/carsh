// LinkTracker — the connection bookkeeping behind the device BT glue
// (device/BtStackGlue.cpp): which ACL link is "the connected phone", which A2DP /
// AVRCP channels belong to it, and whether a user-initiated connect succeeded or
// failed, reported exactly once.
//
// BTstack reports these facts as separate, loosely ordered events from three
// layers: HCI Connection/Disconnection Complete, the A2DP signaling/stream
// subevents and the AVRCP connection subevents. A failed page, for example,
// arrives as Connection Complete with status 0x04 (Page Timeout), then as an A2DP
// signaling failure, then as an A2DP stream failure, and BTstack never follows it
// with a Disconnection Complete. Treating the first of those as "connected" left
// a phantom phone on the BT tab (review finding B3). The glue only pulls the
// fields off each event and feeds them here; every decision below is
// platform-independent and host-tested (tests/bt/LinkTracker_test). No BTstack
// types appear in this header, exactly like bt::NowPlayingAssembler.
//
// Model:
//  - Up to kMaxLinks ACL links are tracked by HCI handle. The first one to come
//    up is the PRIMARY: that is what IBluetooth::connected()/connectedAddress()
//    and ProfileManager see. Another phone connecting while one is up is tracked
//    but not announced; if the primary drops, the next live link is promoted.
//  - A user connect is PENDING from connectRequested() until the A2DP stream to
//    that phone is established. Any failure on the way (page, A2DP signaling,
//    A2DP stream, link loss) produces a single onConnectFailed(addr, reason); the
//    later layers' echoes of the same failure are swallowed.
//  - Disconnection Complete only counts when its status is success and its handle
//    is a tracked link (a failed HCI_Disconnect, or BTstack's synthetic event for
//    an unknown handle, changes nothing).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "bt/Bluetooth.h"

namespace carsh::bt {

class LinkTracker {
public:
    // Matches MAX_NR_HCI_CONNECTIONS in device/btstack_config.h: BTstack never
    // holds more ACL links than that, so a fixed table suffices.
    static constexpr std::size_t kMaxLinks = 2;
    static constexpr uint16_t kNoHandle = 0xFFFF;         // == HCI_CON_HANDLE_INVALID
    static constexpr uint8_t kSuccess = 0x00;             // == ERROR_CODE_SUCCESS
    static constexpr uint8_t kUnspecifiedError = 0x1F;    // == ERROR_CODE_UNSPECIFIED_ERROR

    struct Callbacks {
        std::function<void(const BdAddr&)> onConnected;
        std::function<void(const BdAddr&)> onDisconnected;
        std::function<void(const BdAddr&, int reason)> onConnectFailed;
    };
    void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

    // ---- user intents (btGlueConnect) --------------------------------------
    // Call BEFORE starting the profile connect, so a failure BTstack reports
    // synchronously from inside that call is still attributed to this request.
    void connectRequested(const BdAddr& a) { pending_ = Pending{true, a, 0}; }
    // The profile connect was started; `a2dpCid` identifies the outgoing A2DP
    // attempt (used to cancel it before the ACL is up).
    void connectStarted(const BdAddr& a, uint16_t a2dpCid) {
        if (pending_.active && pending_.addr == a) pending_.a2dpCid = a2dpCid;
    }
    // The profile connect call itself failed (synchronous error status).
    void connectRejected(const BdAddr& a, int status) {
        if (pending_.active && pending_.addr == a) pending_ = Pending{};
        if (cb_.onConnectFailed) cb_.onConnectFailed(a, status);
    }

    // ---- HCI ------------------------------------------------------------------
    // HCI_EVENT_CONNECTION_COMPLETE for an ACL link (the glue filters SCO out).
    void aclConnectionComplete(uint8_t status, uint16_t handle, const BdAddr& a) {
        if (status != kSuccess) {  // failed page / rejected link: never "connected"
            failPending(a, status);
            return;
        }
        if (handle == kNoHandle || findHandle(handle) >= 0) return;  // bogus / duplicate
        int slot = findHandle(kNoHandle);  // first free slot
        if (slot < 0) return;              // table full (cannot happen at the pool size)
        links_[std::size_t(slot)] = Link{};
        links_[std::size_t(slot)].handle = handle;
        links_[std::size_t(slot)].addr = a;
        if (primary_ < 0) {
            primary_ = slot;
            if (cb_.onConnected) cb_.onConnected(a);
        }
    }

    // HCI_EVENT_DISCONNECTION_COMPLETE. `reason` is the HCI disconnect reason,
    // surfaced as the connect-failure reason if the link drops mid-connect.
    void aclDisconnectionComplete(uint8_t status, uint16_t handle, uint8_t reason) {
        if (status != kSuccess) return;  // the disconnect failed: the link is still up
        int idx = handle == kNoHandle ? -1 : findHandle(handle);
        if (idx < 0) return;  // not a link we track (or already gone)
        const BdAddr addr = links_[std::size_t(idx)].addr;
        links_[std::size_t(idx)] = Link{};
        if (idx == primary_) {
            primary_ = -1;
            if (cb_.onDisconnected) cb_.onDisconnected(addr);
        }
        // Dropped before the A2DP stream came up: the user's connect failed. (With
        // BTstack's handler order L2CAP usually sees the drop first and A2DP has
        // already reported the failure; then this is a no-op.)
        failPending(addr, reason);
        if (primary_ < 0) promoteNextLink();
    }

    // ---- A2DP (sink) -------------------------------------------------------
    // A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED (either direction).
    void a2dpSignalingEstablished(uint8_t status, uint16_t cid, const BdAddr& a) {
        if (status != kSuccess) {
            failPending(a, status);
            return;
        }
        Link* l = linkFor(a);
        if (l) l->a2dpCid = cid;
    }
    // A2DP_SUBEVENT_STREAM_ESTABLISHED: the end of a user connect, either way.
    void a2dpStreamEstablished(uint8_t status, const BdAddr& a) {
        if (status != kSuccess) {
            failPending(a, status);
        } else if (pending_.active && pending_.addr == a) {
            pending_ = Pending{};
        }
    }
    // A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED.
    void a2dpSignalingReleased(uint16_t cid) {
        if (cid == 0) return;
        for (Link& l : links_) {
            if (l.handle == kNoHandle || l.a2dpCid != cid) continue;
            l.a2dpCid = 0;
            // Signaling came up for a user connect but closed before the stream
            // did: that connect failed (BTstack reports no status for this case).
            failPending(l.addr, kUnspecifiedError);
        }
    }

    // ---- AVRCP ---------------------------------------------------------------
    // The HU started an AVRCP connection to `a` (avrcp_connect returned success).
    void avrcpConnectStarted(const BdAddr& a) {
        Link* l = linkFor(a);
        if (l && l->avrcp == Avrcp::None) l->avrcp = Avrcp::Connecting;
    }
    // AVRCP_SUBEVENT_CONNECTION_ESTABLISHED (HU- or phone-initiated).
    void avrcpEstablished(uint8_t status, uint16_t cid, const BdAddr& a) {
        Link* l = linkFor(a);
        if (!l) return;
        if (status != kSuccess) {
            l->avrcp = Avrcp::None;
            l->avrcpCid = 0;
        } else {
            l->avrcp = Avrcp::Up;
            l->avrcpCid = cid;
        }
    }
    // AVRCP_SUBEVENT_CONNECTION_RELEASED.
    void avrcpReleased(uint16_t cid) {
        if (cid == 0) return;
        for (Link& l : links_) {
            if (l.handle == kNoHandle || l.avrcpCid != cid) continue;
            l.avrcp = Avrcp::None;
            l.avrcpCid = 0;
        }
    }
    // Should the HU open AVRCP to `a` now? True while A2DP signaling to that phone
    // is up and no AVRCP connection to it is up or being set up — i.e. the phone
    // did not open AVRCP itself after A2DP came up.
    bool shouldInitiateAvrcp(const BdAddr& a) const {
        const Link* l = linkFor(a);
        return l && l->a2dpCid != 0 && l->avrcp == Avrcp::None;
    }

    // ---- queries -----------------------------------------------------------
    bool connected() const { return primary_ >= 0; }
    BdAddr connectedAddress() const { return connected() ? primaryLink().addr : BdAddr{}; }
    uint16_t connectedHandle() const { return connected() ? primaryLink().handle : kNoHandle; }
    // The connected phone's AVRCP cid for commands/queries, 0 when AVRCP is not up.
    uint16_t avrcpCid() const {
        return connected() && primaryLink().avrcp == Avrcp::Up ? primaryLink().avrcpCid : 0;
    }
    // The outgoing A2DP attempt still in flight, 0 when none (cancel target).
    uint16_t pendingA2dpCid() const { return pending_.active ? pending_.a2dpCid : 0; }
    bool connectPending() const { return pending_.active; }

private:
    enum class Avrcp : uint8_t { None, Connecting, Up };

    struct Link {
        uint16_t handle = kNoHandle;  // kNoHandle = free slot
        BdAddr addr{};
        uint16_t a2dpCid = 0;         // A2DP signaling up when nonzero
        Avrcp avrcp = Avrcp::None;
        uint16_t avrcpCid = 0;
    };
    struct Pending {
        bool active = false;
        BdAddr addr{};
        uint16_t a2dpCid = 0;
    };

    int findHandle(uint16_t handle) const {
        for (std::size_t i = 0; i < links_.size(); ++i)
            if (links_[i].handle == handle) return int(i);
        return -1;
    }
    Link* linkFor(const BdAddr& a) {
        for (Link& l : links_)
            if (l.handle != kNoHandle && l.addr == a) return &l;
        return nullptr;
    }
    const Link* linkFor(const BdAddr& a) const {
        for (const Link& l : links_)
            if (l.handle != kNoHandle && l.addr == a) return &l;
        return nullptr;
    }
    const Link& primaryLink() const { return links_[std::size_t(primary_)]; }

    // Report the pending user connect to `a` as failed — once.
    void failPending(const BdAddr& a, int reason) {
        if (!pending_.active || pending_.addr != a) return;
        pending_ = Pending{};
        if (cb_.onConnectFailed) cb_.onConnectFailed(a, reason);
    }

    void promoteNextLink() {
        for (std::size_t i = 0; i < links_.size(); ++i) {
            if (links_[i].handle == kNoHandle) continue;
            primary_ = int(i);
            if (cb_.onConnected) cb_.onConnected(links_[i].addr);
            return;
        }
    }

    Callbacks cb_;
    std::array<Link, kMaxLinks> links_{};
    int primary_ = -1;  // index into links_, -1 = no phone connected
    Pending pending_;
};

}  // namespace carsh::bt
