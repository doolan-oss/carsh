// AaLinkSupervisor — the AA driver's session-end + recovery policy (review A3/A4/
// A7/A9, contract C2). Pins: a dead link or a Shutdown ends the session at once; the
// dongle gets a grace period to re-cycle by itself before a forced re-enumeration;
// forced attempts are bounded and end in exactly one "replug dongle" report; a
// session that reaches Running, or a transport the dongle brings up on its own,
// refills the budget. A phone Exit (SHUTDOWN_REQUEST) ends the session WITHOUT any
// forced re-enumeration, so projection never relaunches behind the user's back.
#include "aa/AaLinkSupervisor.h"

#include "Test.h"

using carsh::aa::AaLinkSupervisor;
using carsh::aa::AaShutdownReason;
using Action = AaLinkSupervisor::Action;
using Phase = AaLinkSupervisor::Phase;
using Cause = AaLinkSupervisor::EndCause;

namespace {

// The connection's shutdownReason() as the driver passes it.
constexpr AaShutdownReason kUp = AaShutdownReason::None;           // not shut down
constexpr AaShutdownReason kTls = AaShutdownReason::CryptoFailure;  // a fault shutdown
constexpr AaShutdownReason kExit = AaShutdownReason::PhoneRequest;  // the user chose Exit

AaLinkSupervisor::Config cfg() {
    AaLinkSupervisor::Config c;
    c.graceMs = 1000;
    c.maxForced = 2;
    c.returnTimeoutMs = 5000;
    return c;
}

// update() with nothing wrong on a live session.
Action idle(AaLinkSupervisor& s, uint64_t t) { return s.update(t, false, kUp, false); }

// Drive one session to its end by a dead link, then through the grace period.
Action deadThenGrace(AaLinkSupervisor& s, uint64_t& t) {
    CHECK(s.update(t, true, kUp, false) == Action::EndSession);
    t += s.config().graceMs;
    return s.update(t, true, kUp, false);
}

}  // namespace

TEST(AaLinkSupervisor, NothingBoundNothingToDo) {
    AaLinkSupervisor s(cfg());
    CHECK(s.phase() == Phase::NoTransport);
    for (uint64_t t = 0; t < 100000; t += 500) CHECK(s.update(t, true, kTls, false) == Action::None);
}

TEST(AaLinkSupervisor, HealthySessionNeverActs) {
    AaLinkSupervisor s(cfg());
    s.onTransportBound();
    CHECK(s.phase() == Phase::Session);
    for (uint64_t t = 0; t < 60000; t += 33) CHECK(s.update(t, false, kUp, t > 3000) == Action::None);
}

// The H8 shape: a bulk-IN error while the dongle stays enumerated. The session ends
// on the first tick that sees it; the port reset waits for the grace period.
TEST(AaLinkSupervisor, DeadLinkEndsAtOnceThenForcesAfterGrace) {
    AaLinkSupervisor s(cfg());
    s.onTransportBound();
    CHECK(idle(s, 100) == Action::None);
    CHECK(s.update(200, true, kUp, false) == Action::EndSession);
    CHECK(s.endCause() == Cause::LinkDead);
    CHECK(s.phase() == Phase::Ended);
    CHECK(s.update(250, true, kUp, false) == Action::None);   // EndSession only once
    CHECK(s.update(1199, true, kUp, false) == Action::None);  // still in grace
    CHECK(s.update(1200, true, kUp, false) == Action::ForceReenumerate);
    CHECK_EQ(s.forcedAttempts(), 1u);
    CHECK(s.awaitingReturn());
    CHECK(s.phase() == Phase::NoTransport);
}

// A9: a fault shutdown on a healthy link — a version reject or an unrecoverable TLS
// failure — is a teardown + restart trigger too.
TEST(AaLinkSupervisor, FaultShutdownEndsSessionAndRestarts) {
    for (const AaShutdownReason why :
         {AaShutdownReason::VersionRejected, AaShutdownReason::CryptoFailure}) {
        AaLinkSupervisor s(cfg());
        s.onTransportBound();
        CHECK(s.update(10, false, kUp, true) == Action::None);
        CHECK(s.update(20, false, why, false) == Action::EndSession);
        CHECK(s.endCause() == Cause::Shutdown);
        CHECK(s.phase() == Phase::Ended);
        CHECK(s.update(1019, false, kUp, false) == Action::None);  // grace
        CHECK(s.update(1020, false, kUp, false) == Action::ForceReenumerate);
    }
}

// The user chose Exit in AA: the phone's SHUTDOWN_REQUEST ends the session, but it is
// not a fault — nothing is forced (a port reset would restart AA and relaunch
// projection seconds later), nothing is reported, however long the dongle stays.
TEST(AaLinkSupervisor, PhoneExitTearsDownWithoutForcing) {
    AaLinkSupervisor s(cfg());
    s.onTransportBound();
    CHECK(s.update(10, false, kUp, true) == Action::None);
    CHECK(s.update(20, false, kExit, false) == Action::EndSession);
    CHECK(s.endCause() == Cause::PhoneExit);
    CHECK(s.phase() == Phase::Exited);
    // The ended connection is freed, and the idle transport's RX ring may overflow or
    // its pipe fault later: none of it restarts anything.
    for (uint64_t t = 20; t < 120000; t += 33) {
        CHECK(s.update(t, t > 5000, kUp, false) == Action::None);
    }
    CHECK_EQ(s.forcedAttempts(), 0u);
    CHECK(!s.awaitingReturn());
    CHECK(s.phase() == Phase::Exited);
}

// The usual wireless ending: the dongle drops its USB link as the phone leaves, which
// the transport may see as a bulk-IN error on the very tick that shows the Shutdown.
// The exit came first (poll processed the SHUTDOWN_REQUEST before the link died), so
// it wins: no forced re-enumeration.
TEST(AaLinkSupervisor, PhoneExitWinsOverTheLinkDyingAlongside) {
    AaLinkSupervisor s(cfg());
    s.onTransportBound();
    CHECK(s.update(0, true, kExit, false) == Action::EndSession);
    CHECK(s.endCause() == Cause::PhoneExit);
    CHECK(s.phase() == Phase::Exited);
    for (uint64_t t = 0; t < 60000; t += 100) CHECK(s.update(t, true, kUp, false) == Action::None);
}

// After an Exit the dongle re-cycles by itself (or the user replugs it): the new
// transport starts a fresh session with a fresh budget, and no DongleLost is reported
// for a dongle we never forced.
TEST(AaLinkSupervisor, PhoneExitThenDongleRecyclesStartsFresh) {
    AaLinkSupervisor s(cfg());
    uint64_t t = 0;
    s.onTransportBound();
    CHECK(deadThenGrace(s, t) == Action::ForceReenumerate);  // an earlier fault ...
    s.onTransportGone();
    s.onTransportBound();  // ... the forced dongle came back (budget not yet refilled)
    CHECK_EQ(s.forcedAttempts(), 1u);
    t += 10;
    CHECK(s.update(t, false, kExit, false) == Action::EndSession);  // user Exit
    t += 2000;
    s.onTransportGone();  // the dongle re-cycles as the phone leaves
    CHECK(s.phase() == Phase::NoTransport);
    for (int i = 0; i < 100; ++i) CHECK(idle(s, t += 1000) == Action::None);  // no DongleLost
    s.onTransportBound();
    CHECK(s.phase() == Phase::Session);
    CHECK_EQ(s.forcedAttempts(), 0u);
    // The next session is supervised as normal: a fault still forces.
    CHECK(deadThenGrace(s, t) == Action::ForceReenumerate);
}

// The normal wireless case: the dongle re-cycles its USB by itself after the session
// ends. That must win over our port reset, and it refills the budget.
TEST(AaLinkSupervisor, DongleRecyclingItselfCancelsTheForce) {
    AaLinkSupervisor s(cfg());
    s.onTransportBound();
    CHECK(s.update(0, true, kUp, false) == Action::EndSession);
    s.onTransportGone();  // within grace
    CHECK(s.phase() == Phase::NoTransport);
    for (uint64_t t = 0; t < 30000; t += 100) CHECK(idle(s, t) == Action::None);  // no DongleLost
    s.onTransportBound();
    CHECK(s.phase() == Phase::Session);
    CHECK_EQ(s.forcedAttempts(), 0u);
}

TEST(AaLinkSupervisor, BudgetExhaustedGivesUpExactlyOnce) {
    AaLinkSupervisor s(cfg());  // maxForced = 2
    uint64_t t = 0;
    s.onTransportBound();
    CHECK(deadThenGrace(s, t) == Action::ForceReenumerate);
    s.onTransportGone();
    s.onTransportBound();  // the forced dongle came back ...
    t += 10;
    CHECK(deadThenGrace(s, t) == Action::ForceReenumerate);  // ... and died again
    CHECK_EQ(s.forcedAttempts(), 2u);
    s.onTransportGone();
    s.onTransportBound();
    t += 10;
    CHECK(deadThenGrace(s, t) == Action::GiveUp);
    CHECK(s.phase() == Phase::GaveUp);
    for (int i = 0; i < 100; ++i) {
        t += 1000;
        CHECK(s.update(t, true, kTls, false) == Action::None);  // said once, then quiet
    }
}

TEST(AaLinkSupervisor, ReachingRunningRefillsTheBudget) {
    AaLinkSupervisor s(cfg());
    uint64_t t = 0;
    s.onTransportBound();
    CHECK(deadThenGrace(s, t) == Action::ForceReenumerate);
    s.onTransportGone();
    s.onTransportBound();
    t += 10;
    CHECK(s.update(t, false, kUp, true) == Action::None);  // a real session came up
    CHECK_EQ(s.forcedAttempts(), 0u);
    t += 10;
    CHECK(deadThenGrace(s, t) == Action::ForceReenumerate);
    CHECK_EQ(s.forcedAttempts(), 1u);
}

TEST(AaLinkSupervisor, ForcedDongleThatNeverReturnsIsReportedOnce) {
    AaLinkSupervisor s(cfg());
    uint64_t t = 0;
    s.onTransportBound();
    CHECK(deadThenGrace(s, t) == Action::ForceReenumerate);
    const uint64_t forcedAt = t;
    s.onTransportGone();  // the removal we performed
    CHECK(s.update(forcedAt + 4999, false, kUp, false) == Action::None);
    CHECK(s.update(forcedAt + 5000, false, kUp, false) == Action::DongleLost);
    CHECK(!s.awaitingReturn());
    CHECK(s.update(forcedAt + 60000, false, kUp, false) == Action::None);
    // It does come back later (replugged): a fresh budget.
    s.onTransportBound();
    CHECK_EQ(s.forcedAttempts(), 0u);
}

// A plain unplug is the user's doing: no "did not come back" report, however long.
TEST(AaLinkSupervisor, UnplugIsNotReportedAsLost) {
    AaLinkSupervisor s(cfg());
    s.onTransportBound();
    CHECK(s.update(0, false, kUp, true) == Action::None);
    s.onTransportGone();
    for (uint64_t t = 0; t < 120000; t += 1000) CHECK(idle(s, t) == Action::None);
}

TEST(AaLinkSupervisor, ReplugAfterGiveUpStartsFresh) {
    auto c = cfg();
    c.maxForced = 0;  // give up straight away
    AaLinkSupervisor s(c);
    uint64_t t = 0;
    s.onTransportBound();
    CHECK(deadThenGrace(s, t) == Action::GiveUp);
    s.onTransportGone();  // user pulls the dongle ...
    s.onTransportBound();  // ... and plugs it back in
    CHECK(s.phase() == Phase::Session);
    CHECK(s.update(t + 1, false, kUp, false) == Action::None);
}

// The whole H8 recovery as the driver runs it: IN error -> session ends -> grace ->
// forced re-enumeration -> the dongle is back as a new transport -> Running.
TEST(AaLinkSupervisor, H8RecoveryScenario) {
    AaLinkSupervisor s;  // production defaults
    CHECK_EQ(s.config().maxForced, 3u);
    uint64_t t = 0;
    bool bound = true, dead = false, running = false;
    int ended = 0, forced = 0;
    s.onTransportBound();
    for (int tick = 0; tick < 400; ++tick, t += 33) {
        if (tick == 30) dead = true;  // xhciep: Transfer error 4 on endpoint 3
        if (tick == 200 && !bound) {  // enumeration + AOA done: new uaa1
            bound = true;
            dead = false;
            s.onTransportBound();
        }
        if (tick == 260) running = true;
        const Action a = s.update(t, bound && dead, kUp, bound && running);
        if (a == Action::EndSession) ++ended;
        if (a == Action::ForceReenumerate) {
            ++forced;
            bound = false;
            s.onTransportGone();
        }
        CHECK(a != Action::GiveUp && a != Action::DongleLost);
    }
    CHECK_EQ(ended, 1);
    CHECK_EQ(forced, 1);
    CHECK(s.phase() == Phase::Session);
    CHECK_EQ(s.forcedAttempts(), 0u);  // the healthy session refilled the budget
}

// The reviewer's case end to end: Exit on the phone while projecting. The driver
// tears the session down once and never port-resets the dongle; the dongle's own
// re-cycle (a new uaa1 some seconds later) brings the next session up.
TEST(AaLinkSupervisor, PhoneExitScenario) {
    AaLinkSupervisor s;  // production defaults
    uint64_t t = 0;
    bool bound = true, running = false;
    AaShutdownReason reason = kUp;
    int ended = 0, forced = 0, bound2 = 0;
    s.onTransportBound();
    for (int tick = 0; tick < 1000; ++tick, t += 33) {
        if (tick == 20) running = true;
        if (tick == 100) reason = kExit;  // SHUTDOWN_REQUEST answered -> Shutdown
        const bool live = bound && reason == kUp;
        const Action a = s.update(t, false, bound ? reason : kUp, live && running);
        if (a == Action::EndSession) {
            ++ended;
            reason = kUp;  // the driver freed the connection
            running = false;
        }
        if (tick == 400) {  // the dongle re-cycled (PnP): transport gone ...
            bound = false;
            s.onTransportGone();
        }
        if (tick == 600) {  // ... and back as a new uaa1 when the phone reconnects
            bound = true;
            ++bound2;
            s.onTransportBound();
        }
        if (a == Action::ForceReenumerate) ++forced;
        CHECK(a != Action::GiveUp && a != Action::DongleLost);
    }
    CHECK_EQ(ended, 1);
    CHECK_EQ(forced, 0);
    CHECK_EQ(bound2, 1);
    CHECK(s.phase() == Phase::Session);
}
