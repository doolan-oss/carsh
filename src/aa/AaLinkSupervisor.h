// AaLinkSupervisor — the Android Auto driver's session-end and recovery policy, kept
// platform-independent so it is host-tested (tests/aa/AaLinkSupervisor_test.cpp).
// device/CircleAaDriver reports what it sees (a transport bound or gone, the link
// dead, the connection's state and shutdown reason) and carries out the Action
// returned; the supervisor owns only the timing and the retry budget.
//
// Policy (review A3/A4/A7/A9):
//  * A session ends when the transport's link latches dead (any bulk-IN/OUT fault,
//    see AaBulkLink) or the AaConnection reaches Shutdown (the phone's
//    SHUTDOWN_REQUEST — already answered — a version reject, or an unrecoverable TLS
//    failure). -> EndSession: the driver drops the connection and resets the
//    projection at once, so the UI never sits on a frozen AA frame.
//  * The phone's own SHUTDOWN_REQUEST (AaShutdownReason::PhoneRequest: the user chose
//    Exit) is NOT a fault: the session ends and nothing is forced (phase Exited).
//    Forcing a re-enumeration would restart AA and relaunch projection seconds after
//    the user left it. A wireless dongle re-cycles its USB by itself when the phone
//    leaves, and a replug always works; either brings a fresh transport and session.
//    It takes precedence over a link that dies on the same tick (the dongle dropping
//    its USB link as it re-cycles is a consequence of the exit, not a fault).
//  * After a fault (dead link, version reject, TLS failure) the dongle normally
//    re-cycles its USB link by itself too. It gets graceMs to do that; a transport
//    change cancels everything below. If it doesn't, -> ForceReenumerate: the driver
//    removes the dongle's USB device and rescans the bus, which port-resets it. It
//    re-enumerates (PTP -> AOA switch, or straight back into accessory mode) and the
//    next "uaa1" starts a fresh session. There is no in-place endpoint recovery;
//    CircleAoaTransport.cpp explains why.
//  * At most maxForced forced re-enumerations in a row without a session reaching
//    Running. After that -> GiveUp, once ("AA link lost — replug dongle"), and stay
//    idle until a transport comes up on its own (replug, or the dongle re-cycling).
//  * After forcing, no transport back within returnTimeoutMs -> DongleLost, once
//    (same log line): the dongle did not re-enumerate.
// The budget refills when a session reaches Running, or when a transport comes up
// that we did not force (the dongle re-cycled by itself, or the user replugged it).
#pragma once

#include <cstdint>

#include "aa/AaConnection.h"  // AaShutdownReason

namespace carsh::aa {

class AaLinkSupervisor {
public:
    struct Config {
        // Wait this long for the dongle's own re-cycle first. The device kernel pumps
        // USB plug-and-play only every 33 UI frames (~1.5-3 s), so a shorter grace would
        // routinely race the dongle's pending disconnect event (harmless — both paths
        // re-enumerate it — but it costs a needless port reset).
        uint64_t graceMs = 3000;
        unsigned maxForced = 3;            // forced re-enumerations before giving up
        uint64_t returnTimeoutMs = 15000;  // a forced dongle must be back within this
    };

    enum class Phase {
        NoTransport,  // no "uaa1" bound (not plugged, re-cycling, or being re-enumerated)
        Session,      // bound; the AaConnection is live
        Ended,        // session torn down after a fault, transport still bound — grace
                      // period running, then a forced re-enumeration
        Exited,       // the phone ended the session (user Exit), transport still bound —
                      // nothing forced; idle until the transport goes / comes back
        GaveUp,       // retry budget spent; idle until a transport comes up on its own
    };

    enum class Action {
        None,
        EndSession,        // drop the connection now + projection disconnect()
        ForceReenumerate,  // remove + rescan the dongle's USB device (destroys it)
        GiveUp,            // log "AA link lost — replug dongle" (emitted once)
        DongleLost,        // forced, but nothing came back: same log line (once)
    };

    enum class EndCause {
        None,
        LinkDead,   // the transport latched a bulk-IN/OUT fault
        Shutdown,   // the connection shut down on a fault (version reject, TLS failure)
        PhoneExit,  // the phone's SHUTDOWN_REQUEST: the user chose Exit
    };

    AaLinkSupervisor() = default;
    explicit AaLinkSupervisor(const Config& cfg) : cfg_(cfg) {}

    // A new transport instance was bound and its session started.
    void onTransportBound() {
        if (!awaitingReturn_) forced_ = 0;  // came up on its own: fresh budget
        awaitingReturn_ = false;
        phase_ = Phase::Session;
        cause_ = EndCause::None;
    }

    // The bound transport was destroyed (unplug, the dongle's own re-cycle, or the
    // removal a ForceReenumerate performed).
    void onTransportGone() { phase_ = Phase::NoTransport; }

    // Once per driver tick. linkDead / shutdown / running describe the bound
    // transport and its connection (false / AaShutdownReason::None / false when
    // nothing is bound or the session already ended). shutdown is the connection's
    // shutdownReason() — None while it is not in AaState::Shutdown.
    Action update(uint64_t nowMs, bool linkDead, AaShutdownReason shutdown, bool running) {
        switch (phase_) {
            case Phase::NoTransport:
                if (awaitingReturn_ && nowMs - forcedAtMs_ >= cfg_.returnTimeoutMs) {
                    awaitingReturn_ = false;
                    return Action::DongleLost;
                }
                return Action::None;
            case Phase::Session:
                if (running) forced_ = 0;  // a healthy session refills the budget
                if (shutdown == AaShutdownReason::PhoneRequest) {
                    cause_ = EndCause::PhoneExit;  // wins over a link dying alongside
                    phase_ = Phase::Exited;
                    return Action::EndSession;
                }
                if (linkDead || shutdown != AaShutdownReason::None) {
                    cause_ = linkDead ? EndCause::LinkDead : EndCause::Shutdown;
                    phase_ = Phase::Ended;
                    endedAtMs_ = nowMs;
                    return Action::EndSession;
                }
                return Action::None;
            case Phase::Ended:
                if (nowMs - endedAtMs_ < cfg_.graceMs) return Action::None;
                if (forced_ < cfg_.maxForced) {
                    ++forced_;
                    awaitingReturn_ = true;
                    forcedAtMs_ = nowMs;
                    phase_ = Phase::NoTransport;
                    return Action::ForceReenumerate;
                }
                phase_ = Phase::GaveUp;
                return Action::GiveUp;
            case Phase::Exited:  // a later link fault on the idle transport changes nothing
            case Phase::GaveUp:
                return Action::None;
        }
        return Action::None;
    }

    Phase phase() const { return phase_; }
    EndCause endCause() const { return cause_; }       // why the last session ended
    unsigned forcedAttempts() const { return forced_; }  // in the current streak
    bool awaitingReturn() const { return awaitingReturn_; }
    const Config& config() const { return cfg_; }

private:
    Config cfg_;
    Phase phase_ = Phase::NoTransport;
    EndCause cause_ = EndCause::None;
    unsigned forced_ = 0;
    bool awaitingReturn_ = false;
    uint64_t endedAtMs_ = 0;
    uint64_t forcedAtMs_ = 0;
};

}  // namespace carsh::aa
