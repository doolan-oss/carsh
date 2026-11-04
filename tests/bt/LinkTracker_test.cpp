// LinkTracker tests: the connection bookkeeping the device BT glue feeds with
// BTstack's HCI / A2DP / AVRCP events (review finding B3). Each test replays the
// event sequence BTstack v1.6.1 produces for a scenario — e.g. a failed page is
// Connection Complete(status 0x04), then an A2DP signaling failure, then an A2DP
// stream failure, and never a Disconnection Complete — and asserts what the app
// is told through IBluetooth's callbacks. No radio needed (H6).
#include "bt/LinkTracker.h"

#include <cstring>
#include <string>
#include <vector>

#include "Test.h"
#include "bt/AbsoluteVolume.h"
#include "bt/NowPlaying.h"
#include "player/MediaSession.h"
#include "profile/ProfileManager.h"

using namespace carsh;
using carsh::bt::BdAddr;
using carsh::bt::LinkTracker;

namespace {
const BdAddr kPhone{{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
const BdAddr kOther{{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01}};
constexpr uint8_t kPageTimeout = 0x04;
constexpr uint8_t kConnectionTimeout = 0x08;
constexpr uint8_t kRemoteTerminated = 0x13;

// Records every callback in order, like the kernel's handlers would see them.
struct Recorder {
    struct Ev {
        char kind;  // 'C' connected, 'D' disconnected, 'F' connect failed,
                    // 'E' the connected phone's AVRCP session ended
        BdAddr addr;
        int reason;
    };
    std::vector<Ev> evs;

    void attach(LinkTracker& t) {
        LinkTracker::Callbacks cb;
        cb.onConnected = [this](const BdAddr& a) { evs.push_back({'C', a, 0}); };
        cb.onDisconnected = [this](const BdAddr& a) { evs.push_back({'D', a, 0}); };
        cb.onConnectFailed = [this](const BdAddr& a, int r) { evs.push_back({'F', a, r}); };
        cb.onAvrcpSessionEnded = [this](const BdAddr& a) { evs.push_back({'E', a, 0}); };
        t.setCallbacks(cb);
    }
    int count(char kind) const {
        int n = 0;
        for (const Ev& e : evs) n += e.kind == kind ? 1 : 0;
        return n;
    }
};
}  // namespace

TEST(LinkTracker, SuccessfulOutgoingConnectReportsConnectedOnce) {
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.connectRequested(kPhone);
    t.connectStarted(kPhone, 0x41);
    CHECK_EQ(t.pendingA2dpCid(), uint16_t(0x41));
    t.aclConnectionComplete(0, 0x000B, kPhone);
    t.a2dpSignalingEstablished(0, 0x41, kPhone);
    t.a2dpStreamEstablished(0, kPhone);
    REQUIRE(r.evs.size() == std::size_t(1));
    CHECK_EQ(r.evs[0].kind, 'C');
    CHECK(r.evs[0].addr == kPhone);
    CHECK(t.connected());
    CHECK(t.connectedAddress() == kPhone);
    CHECK_EQ(t.connectedHandle(), uint16_t(0x000B));
    CHECK(!t.connectPending());  // stream up: the connect is complete
}

TEST(LinkTracker, PageTimeoutIsAFailureNotAPhantomConnection) {
    // The B3 scenario: CONNECT on an out-of-range phone. BTstack forwards the
    // failed Connection Complete and frees the link (no Disconnection Complete
    // follows), then reports the same failure again from A2DP twice.
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.connectRequested(kPhone);
    t.connectStarted(kPhone, 0x41);
    t.aclConnectionComplete(kPageTimeout, LinkTracker::kNoHandle, kPhone);
    t.a2dpSignalingEstablished(kPageTimeout, 0x41, kPhone);
    t.a2dpStreamEstablished(kPageTimeout, kPhone);
    REQUIRE(r.evs.size() == std::size_t(1));  // exactly one report, no 'C'
    CHECK_EQ(r.evs[0].kind, 'F');
    CHECK(r.evs[0].addr == kPhone);
    CHECK_EQ(r.evs[0].reason, int(kPageTimeout));
    CHECK(!t.connected());
    CHECK_EQ(t.connectedHandle(), LinkTracker::kNoHandle);
    CHECK_EQ(t.pendingA2dpCid(), uint16_t(0));
}

TEST(LinkTracker, PageTimeoutDoesNotSwitchTheDriverProfile) {
    // Wired the way device/kernel.cpp wires CircleBluetooth: onConnected ->
    // ProfileManager. A failed connect must not create/activate a profile.
    profile::ProfileManager pm;
    LinkTracker t;
    int failReason = 0;
    LinkTracker::Callbacks cb;
    cb.onConnected = [&](const BdAddr& a) {
        profile::BdAddr pa; pa.b = a; pm.onBtConnected(pa);
    };
    cb.onConnectFailed = [&](const BdAddr&, int reason) { failReason = reason; };
    t.setCallbacks(cb);
    t.connectRequested(kPhone);
    t.aclConnectionComplete(kPageTimeout, LinkTracker::kNoHandle, kPhone);
    CHECK_EQ(failReason, int(kPageTimeout));
    CHECK(!t.connected());
    CHECK_EQ(pm.active().displayName, std::string("Guest"));
}

TEST(LinkTracker, SynchronousRejectIsReportedOnce) {
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.connectRequested(kPhone);
    t.connectRejected(kPhone, 0x0C);  // e.g. COMMAND_DISALLOWED from the call
    t.a2dpSignalingEstablished(0x0C, 0x41, kPhone);  // any echo is swallowed
    REQUIRE(r.evs.size() == std::size_t(1));
    CHECK_EQ(r.evs[0].kind, 'F');
    CHECK_EQ(r.evs[0].reason, 0x0C);
    CHECK(!t.connectPending());
}

TEST(LinkTracker, UnrequestedFailedLinkIsIgnored) {
    // A failed incoming link (no user connect pending) is not the user's problem
    // and must not flip any state.
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.aclConnectionComplete(kPageTimeout, LinkTracker::kNoHandle, kPhone);
    t.a2dpSignalingEstablished(kPageTimeout, 0x41, kPhone);
    CHECK(r.evs.empty());
    CHECK(!t.connected());
}

TEST(LinkTracker, A2dpFailureAfterLinkUpFailsTheConnectThenLinkDropDisconnects) {
    // The ACL comes up (phone reachable) but A2DP is refused. The connect is
    // reported failed once; the link itself is still up until BTstack drops it.
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.connectRequested(kPhone);
    t.aclConnectionComplete(0, 0x000C, kPhone);
    t.a2dpSignalingEstablished(0x0D /* rejected: limited resources */, 0x41, kPhone);
    t.a2dpStreamEstablished(0x0D, kPhone);  // BTstack also fails the stream
    REQUIRE(r.evs.size() == std::size_t(2));
    CHECK_EQ(r.evs[0].kind, 'C');
    CHECK_EQ(r.evs[1].kind, 'F');
    CHECK_EQ(r.evs[1].reason, 0x0D);
    CHECK(t.connected());
    t.aclDisconnectionComplete(0, 0x000C, kRemoteTerminated);
    REQUIRE(r.evs.size() == std::size_t(3));
    CHECK_EQ(r.evs[2].kind, 'D');
    CHECK(!t.connected());
}

TEST(LinkTracker, LinkLossMidConnectDisconnectsAndFailsOnce_HciFirst) {
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.connectRequested(kPhone);
    t.aclConnectionComplete(0, 0x000C, kPhone);
    t.aclDisconnectionComplete(0, 0x000C, kConnectionTimeout);
    // A2DP then reports its own failure for the same attempt: swallowed.
    t.a2dpSignalingEstablished(0x16, 0x41, kPhone);
    REQUIRE(r.evs.size() == std::size_t(3));
    CHECK_EQ(r.evs[0].kind, 'C');
    CHECK_EQ(r.evs[1].kind, 'D');
    CHECK_EQ(r.evs[2].kind, 'F');
    CHECK_EQ(r.evs[2].reason, int(kConnectionTimeout));
    CHECK(!t.connected());
}

TEST(LinkTracker, LinkLossMidConnectDisconnectsAndFailsOnce_L2capFirst) {
    // BTstack's real order: L2CAP is the first HCI event handler, so the A2DP
    // failure (channel open failed on the dropped link) reaches the glue before
    // its own Disconnection Complete. Still exactly one of each.
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.connectRequested(kPhone);
    t.aclConnectionComplete(0, 0x000C, kPhone);
    t.a2dpSignalingEstablished(0x16, 0x41, kPhone);
    t.aclDisconnectionComplete(0, 0x000C, kConnectionTimeout);
    t.a2dpStreamEstablished(0x16, kPhone);
    CHECK_EQ(r.count('C'), 1);
    CHECK_EQ(r.count('F'), 1);
    CHECK_EQ(r.count('D'), 1);
    CHECK(!t.connected());
}

TEST(LinkTracker, SignalingClosedBeforeStreamFailsPendingConnect) {
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.connectRequested(kPhone);
    t.aclConnectionComplete(0, 0x000C, kPhone);
    t.a2dpSignalingEstablished(0, 0x41, kPhone);
    t.a2dpSignalingReleased(0x41);  // phone closed AVDTP during configuration
    REQUIRE(r.evs.size() == std::size_t(2));
    CHECK_EQ(r.evs[1].kind, 'F');
    CHECK_EQ(r.evs[1].reason, int(LinkTracker::kUnspecifiedError));
    CHECK(t.connected());  // the ACL is still up
}

TEST(LinkTracker, DisconnectOnlyCountsForTrackedHandleAndSuccess) {
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.aclConnectionComplete(0, 0x000B, kPhone);  // phone-initiated (incoming)
    REQUIRE(r.count('C') == 1);
    t.aclDisconnectionComplete(0x0C /* HCI_Disconnect failed */, 0x000B, 0);
    t.aclDisconnectionComplete(0, 0x0042 /* not ours */, kRemoteTerminated);
    t.aclDisconnectionComplete(0, LinkTracker::kNoHandle, 0);
    CHECK_EQ(r.count('D'), 0);
    CHECK(t.connected());
    t.aclDisconnectionComplete(0, 0x000B, kRemoteTerminated);
    CHECK_EQ(r.count('D'), 1);
    CHECK(!t.connected());
    t.aclDisconnectionComplete(0, 0x000B, kRemoteTerminated);  // repeat: no-op
    CHECK_EQ(r.count('D'), 1);
    CHECK_EQ(r.count('F'), 0);  // nothing was pending
}

TEST(LinkTracker, DuplicateConnectionCompleteIsIdempotent) {
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.aclConnectionComplete(0, 0x000B, kPhone);
    t.aclConnectionComplete(0, 0x000B, kPhone);
    CHECK_EQ(r.count('C'), 1);
}

TEST(LinkTracker, SecondPhoneDoesNotStealTheConnectionAndIsPromotedLater) {
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.aclConnectionComplete(0, 0x000B, kPhone);
    t.aclConnectionComplete(0, 0x000C, kOther);  // passenger's phone auto-connects
    CHECK_EQ(r.count('C'), 1);
    CHECK(t.connectedAddress() == kPhone);
    // The second phone leaving must not disconnect the first.
    t.aclDisconnectionComplete(0, 0x000C, kRemoteTerminated);
    CHECK_EQ(r.count('D'), 0);
    CHECK(t.connectedAddress() == kPhone);
    // With both up, the primary leaving hands over to the remaining link.
    t.aclConnectionComplete(0, 0x000C, kOther);
    t.aclDisconnectionComplete(0, 0x000B, kRemoteTerminated);
    REQUIRE(r.evs.size() == std::size_t(3));
    CHECK_EQ(r.evs[1].kind, 'D');
    CHECK(r.evs[1].addr == kPhone);
    CHECK_EQ(r.evs[2].kind, 'C');
    CHECK(r.evs[2].addr == kOther);
    CHECK(t.connectedAddress() == kOther);
    CHECK_EQ(t.connectedHandle(), uint16_t(0x000C));
}

TEST(LinkTracker, LaterConnectRequestReplacesThePendingOne) {
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.connectRequested(kOther);
    t.connectRequested(kPhone);
    t.aclConnectionComplete(kPageTimeout, LinkTracker::kNoHandle, kOther);  // stale
    CHECK(r.evs.empty());
    t.aclConnectionComplete(kPageTimeout, LinkTracker::kNoHandle, kPhone);
    CHECK_EQ(r.count('F'), 1);
}

TEST(LinkTracker, AvrcpIsInitiatedOnlyAfterA2dpAndOnlyOnce) {
    LinkTracker t;
    t.aclConnectionComplete(0, 0x000B, kPhone);
    CHECK(!t.shouldInitiateAvrcp(kPhone));  // no A2DP yet
    t.a2dpSignalingEstablished(0, 0x41, kPhone);
    CHECK(t.shouldInitiateAvrcp(kPhone));
    CHECK(!t.shouldInitiateAvrcp(kOther));  // no link to that address
    t.avrcpConnectStarted(kPhone);
    CHECK(!t.shouldInitiateAvrcp(kPhone));  // already connecting
    CHECK_EQ(t.avrcpCid(), uint16_t(0));    // not usable until established
    t.avrcpEstablished(0, 0x07, kPhone);
    CHECK(!t.shouldInitiateAvrcp(kPhone));
    CHECK_EQ(t.avrcpCid(), uint16_t(0x07));
    t.avrcpReleased(0x07);
    CHECK_EQ(t.avrcpCid(), uint16_t(0));
}

TEST(LinkTracker, PhoneOpenedAvrcpFirstSuppressesHuConnect) {
    LinkTracker t;
    t.aclConnectionComplete(0, 0x000B, kPhone);
    t.a2dpSignalingEstablished(0, 0x41, kPhone);
    t.avrcpEstablished(0, 0x03, kPhone);  // incoming AVCTP beat the HU's timer
    CHECK(!t.shouldInitiateAvrcp(kPhone));
    CHECK_EQ(t.avrcpCid(), uint16_t(0x03));
}

TEST(LinkTracker, FailedAvrcpConnectLeavesNoUsableCid) {
    LinkTracker t;
    t.aclConnectionComplete(0, 0x000B, kPhone);
    t.a2dpSignalingEstablished(0, 0x41, kPhone);
    t.avrcpConnectStarted(kPhone);
    t.avrcpEstablished(0x04 /* e.g. no AVRCP TG on the phone */, 0x07, kPhone);
    CHECK_EQ(t.avrcpCid(), uint16_t(0));  // commands must not target a dead cid
}

TEST(LinkTracker, A2dpReleaseStopsAvrcpInitiation) {
    LinkTracker t;
    t.aclConnectionComplete(0, 0x000B, kPhone);
    t.a2dpSignalingEstablished(0, 0x41, kPhone);
    t.a2dpSignalingReleased(0x41);
    CHECK(!t.shouldInitiateAvrcp(kPhone));  // the deferred connect finds A2DP gone
}

TEST(LinkTracker, LinkDropClearsProfileState) {
    LinkTracker t;
    t.aclConnectionComplete(0, 0x000B, kPhone);
    t.a2dpSignalingEstablished(0, 0x41, kPhone);
    t.avrcpEstablished(0, 0x07, kPhone);
    t.aclDisconnectionComplete(0, 0x000B, kRemoteTerminated);
    CHECK_EQ(t.avrcpCid(), uint16_t(0));
    CHECK(!t.shouldInitiateAvrcp(kPhone));
    // Reconnect of the same phone starts from a clean slate.
    t.aclConnectionComplete(0, 0x000D, kPhone);
    CHECK(!t.shouldInitiateAvrcp(kPhone));
    CHECK_EQ(t.avrcpCid(), uint16_t(0));
}

TEST(LinkTracker, AvrcpOfASecondaryPhoneIsNotUsedForCommands) {
    LinkTracker t;
    t.aclConnectionComplete(0, 0x000B, kPhone);
    t.aclConnectionComplete(0, 0x000C, kOther);
    t.avrcpEstablished(0, 0x09, kOther);
    CHECK_EQ(t.avrcpCid(), uint16_t(0));  // only the connected phone's AVRCP counts
}

// ---- End of the connected phone's AVRCP session ---------------------------
// The glue resets and republishes the now-playing metadata and the absolute
// volume on onAvrcpSessionEnded. Otherwise the kernel keeps the old title with
// playing=true after the phone leaves.

namespace {
// A phone connected by the phone itself (no user connect), with A2DP and AVRCP up.
void phoneUpWithAvrcp(LinkTracker& t, const BdAddr& a, uint16_t handle, uint16_t avrcpCid) {
    t.aclConnectionComplete(0, handle, a);
    t.a2dpSignalingEstablished(0, 0x41, a);
    t.a2dpStreamEstablished(0, a);
    t.avrcpEstablished(0, avrcpCid, a);
}
}  // namespace

TEST(LinkTracker, AvrcpReleaseOfTheConnectedPhoneEndsItsSessionOnce) {
    LinkTracker t;
    Recorder r;
    r.attach(t);
    phoneUpWithAvrcp(t, kPhone, 0x000B, 0x07);
    t.avrcpReleased(0x07);
    CHECK_EQ(r.count('E'), 1);
    CHECK_EQ(r.evs.back().kind, 'E');
    CHECK(r.evs.back().addr == kPhone);
    CHECK(t.connected());  // AVRCP only: the phone (and A2DP) stay connected
    t.avrcpReleased(0x07);  // duplicate release: nothing more
    // The link drops later: disconnected, but the session already ended.
    t.aclDisconnectionComplete(0, 0x000B, kRemoteTerminated);
    CHECK_EQ(r.count('E'), 1);
    CHECK_EQ(r.count('D'), 1);
}

TEST(LinkTracker, LinkDropWithAvrcpUpEndsTheSessionBeforeDisconnected) {
    // HCI reports the drop before L2CAP closes the AVRCP channel.
    LinkTracker t;
    Recorder r;
    r.attach(t);
    phoneUpWithAvrcp(t, kPhone, 0x000B, 0x07);
    t.aclDisconnectionComplete(0, 0x000B, kConnectionTimeout);
    REQUIRE(r.evs.size() == std::size_t(3));  // C, E, D
    CHECK_EQ(r.evs[1].kind, 'E');
    CHECK(r.evs[1].addr == kPhone);
    CHECK_EQ(r.evs[2].kind, 'D');
    t.avrcpReleased(0x07);  // the late AVRCP release: already ended
    CHECK_EQ(r.count('E'), 1);
}

TEST(LinkTracker, AvrcpReleasedBeforeTheLinkDropEndsTheSessionOnce) {
    // BTstack's usual order: L2CAP sees the drop first, so AVRCP is released
    // while the link is still tracked, then Disconnection Complete follows.
    LinkTracker t;
    Recorder r;
    r.attach(t);
    phoneUpWithAvrcp(t, kPhone, 0x000B, 0x07);
    t.avrcpReleased(0x07);
    t.aclDisconnectionComplete(0, 0x000B, kConnectionTimeout);
    REQUIRE(r.evs.size() == std::size_t(3));  // C, E, D
    CHECK_EQ(r.evs[1].kind, 'E');
    CHECK_EQ(r.evs[2].kind, 'D');
}

TEST(LinkTracker, NoSessionEndWithoutAvrcpOrForASecondaryPhone) {
    LinkTracker t;
    Recorder r;
    r.attach(t);
    t.aclConnectionComplete(0, 0x000B, kPhone);
    t.aclConnectionComplete(0, 0x000C, kOther);
    t.avrcpEstablished(0, 0x09, kOther);  // the second phone's AVRCP
    t.avrcpReleased(0x09);
    CHECK_EQ(r.count('E'), 0);  // not the connected phone's session
    t.avrcpConnectStarted(kPhone);
    t.avrcpEstablished(0x04, 0x07, kPhone);  // AVRCP to the phone failed
    t.aclDisconnectionComplete(0, 0x000B, kRemoteTerminated);
    CHECK_EQ(r.count('E'), 0);  // no AVRCP was up: no metadata to clear
    CHECK_EQ(r.count('D'), 1);
}

// The glue + kernel wiring around the session end, as the tests below model it:
// BtStackGlue's endAvrcpSession() drops the assembled metadata and calls the app's
// onAvrcpEnded (NOT an empty metadata: the kernel would read that as an AVRCP
// report); the kernel then clears g_btMeta and g_btAvrcpReported, and feeds both to
// MediaSession every frame. The absolute volume is reset only on a link drop
// (onDisconnected) and when the connected phone's AVRCP (re)connects.
namespace {
struct GlueAndKernel {
    bt::NowPlayingAssembler np;     // the glue's g_nowPlaying
    bt::AbsoluteVolume vol;         // the glue's g_absVolume
    bt::AvrcpMetadata meta;         // the kernel's g_btMeta
    bool reported = false;          // the kernel's g_btAvrcpReported

    void attach(LinkTracker& t) {
        LinkTracker::Callbacks cb;
        cb.onConnected = [this](const BdAddr&) { meta = {}; reported = false; };
        cb.onDisconnected = [this](const BdAddr&) {
            vol.reset();
            meta = {};
            reported = false;
        };
        cb.onAvrcpSessionEnded = [this](const BdAddr&) {
            np.resetAll();
            meta = {};
            reported = false;
        };
        t.setCallbacks(cb);
    }
    // AVRCP_SUBEVENT_CONNECTION_ESTABLISHED for the connected phone.
    void avrcpUp(LinkTracker& t, uint16_t cid, const BdAddr& a) {
        t.avrcpEstablished(0, cid, a);
        if (cid == t.avrcpCid()) vol.reset();
    }
    // The phone reports over AVRCP (the kernel's onMetadata).
    void report(const char* title, bool playing) {
        np.setAttribute(uint8_t(bt::AvrcpAttr::Title), title, std::strlen(title));
        np.setPlaybackStatus(uint8_t(playing ? bt::AvrcpPlayStatus::Playing
                                             : bt::AvrcpPlayStatus::Paused));
        meta = np.metadata();
        reported = true;
    }
};
}  // namespace

TEST(LinkTracker, StaleNowPlayingDoesNotSurviveTheLinkDrop) {
    LinkTracker t;
    GlueAndKernel k;
    k.attach(t);
    t.aclConnectionComplete(0, 0x000B, kPhone);
    t.a2dpSignalingEstablished(0, 0x41, kPhone);
    t.a2dpStreamEstablished(0, kPhone);
    k.avrcpUp(t, 0x07, kPhone);
    k.report("Old Song", true);

    // The phone goes out of range while playing.
    t.aclDisconnectionComplete(0, 0x000B, kConnectionTimeout);
    CHECK(!k.meta.playing);
    CHECK(k.meta.title.empty());
    CHECK(!k.reported);

    // The user plays a local track, then the phone's link comes back (ACL only,
    // AVRCP not up yet). A stale PLAYING would take the session from the local
    // track and pause it.
    player::MediaSession session;
    player::MediaSession::LocalState local;
    local.hasTrack = true;
    local.playing = true;
    local.title = "Local";
    session.setLocal(local);
    session.selectLocal();
    t.aclConnectionComplete(0, 0x000D, kPhone);
    session.setBluetooth(t.connected(), false, k.meta, k.reported, 1000000);
    CHECK(session.source() == player::Source::Local);
    CHECK(!session.shouldPauseLocal());
    CHECK_EQ(session.view().title, std::string("Local"));
}

// AVRCP alone is released while A2DP keeps streaming (final review): the kernel
// must stop treating AVRCP as authoritative, so Now Playing shows '||' for the
// music that is still playing instead of the last PAUSED/'>', and the phone's
// absolute volume must stay in force (it keeps sending full-scale PCM).
TEST(LinkTracker, AvrcpOnlyReleaseEndsTheReportButKeepsTheVolume) {
    LinkTracker t;
    GlueAndKernel k;
    k.attach(t);
    t.aclConnectionComplete(0, 0x000B, kPhone);
    t.a2dpSignalingEstablished(0, 0x41, kPhone);
    t.a2dpStreamEstablished(0, kPhone);
    k.avrcpUp(t, 0x07, kPhone);
    k.vol.set(40);  // SetAbsoluteVolume: ~31 %
    const float gain = k.vol.gain();
    CHECK(gain < 0.5f);

    player::MediaSession session;
    player::MediaSession::LocalState local;
    local.hasTrack = true;
    local.title = "Local";
    session.setLocal(local);
    uint64_t now = 1000000;
    k.report("Phone Song", true);
    session.setBluetooth(t.connected(), true, k.meta, k.reported, now);
    CHECK(session.source() == player::Source::Bluetooth);
    CHECK(session.view().playing);

    t.avrcpReleased(0x07);  // AVRCP only: ACL and A2DP stay up, music plays on
    CHECK(t.connected());
    CHECK(!k.reported);
    CHECK_EQ(k.vol.gain(), gain);
    session.setBluetooth(t.connected(), true, k.meta, k.reported, now += 33000);
    CHECK(session.source() == player::Source::Bluetooth);
    CHECK(session.view().playing);  // '||' from the stream, not a stale '>'
    CHECK(session.playPauseCommand() == bt::AvrcpCommand::Pause);

    // AVRCP comes back: a new session starts from unity until the phone sets it.
    k.avrcpUp(t, 0x08, kPhone);
    CHECK_EQ(k.vol.gain(), 1.0f);
    k.vol.set(40);
    // The link drops: the volume goes with it.
    t.aclDisconnectionComplete(0, 0x000B, kConnectionTimeout);
    CHECK_EQ(k.vol.gain(), 1.0f);
}
