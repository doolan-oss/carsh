// AaLinkSupervisor — the AA driver's session-end + recovery policy (review A3/A4/
// A7/A9, contract C2). Pins: a dead link or a Shutdown ends the session at once; the
// dongle gets a grace period to re-cycle by itself before a forced re-enumeration;
// forced attempts are bounded and end in exactly one "replug dongle" report; a
// session that reaches Running, or a transport the dongle brings up on its own,
// refills the budget.
#include "aa/AaLinkSupervisor.h"

#include "Test.h"

using carsh::aa::AaLinkSupervisor;
using Action = AaLinkSupervisor::Action;
using Phase = AaLinkSupervisor::Phase;
using Cause = AaLinkSupervisor::EndCause;

namespace {

AaLinkSupervisor::Config cfg() {
    AaLinkSupervisor::Config c;
    c.graceMs = 1000;
    c.maxForced = 2;
    c.returnTimeoutMs = 5000;
    return c;
}

// update() with nothing wrong on a live session.
Action idle(AaLinkSupervisor& s, uint64_t t) { return s.update(t, false, false, false); }

// Drive one session to its end by a dead link, then through the grace period.
Action deadThenGrace(AaLinkSupervisor& s, uint64_t& t) {
    CHECK(s.update(t, true, false, false) == Action::EndSession);
    t += s.config().graceMs;
    return s.update(t, true, false, false);
}

}  // namespace

TEST(AaLinkSupervisor, NothingBoundNothingToDo) {
    AaLinkSupervisor s(cfg());
    CHECK(s.phase() == Phase::NoTransport);
    for (uint64_t t = 0; t < 100000; t += 500) CHECK(s.update(t, true, true, false) == Action::None);
}

TEST(AaLinkSupervisor, HealthySessionNeverActs) {
    AaLinkSupervisor s(cfg());
    s.onTransportBound();
    CHECK(s.phase() == Phase::Session);
    for (uint64_t t = 0; t < 60000; t += 33) CHECK(s.update(t, false, false, t > 3000) == Action::None);
}

// The H8 shape: a bulk-IN error while the dongle stays enumerated. The session ends
// on the first tick that sees it; the port reset waits for the grace period.
TEST(AaLinkSupervisor, DeadLinkEndsAtOnceThenForcesAfterGrace) {
    AaLinkSupervisor s(cfg());
    s.onTransportBound();
    CHECK(idle(s, 100) == Action::None);
    CHECK(s.update(200, true, false, false) == Action::EndSession);
    CHECK(s.endCause() == Cause::LinkDead);
    CHECK(s.phase() == Phase::Ended);
    CHECK(s.update(250, true, false, false) == Action::None);   // EndSession only once
    CHECK(s.update(1199, true, false, false) == Action::None);  // still in grace
    CHECK(s.update(1200, true, false, false) == Action::ForceReenumerate);
    CHECK_EQ(s.forcedAttempts(), 1u);
    CHECK(s.awaitingReturn());
    CHECK(s.phase() == Phase::NoTransport);
}

// A9: the phone's SHUTDOWN_REQUEST (or a version reject / TLS failure) on a healthy
// link is a teardown + restart trigger too.
TEST(AaLinkSupervisor, ShutdownEndsSessionAndRestarts) {
    AaLinkSupervisor s(cfg());
    s.onTransportBound();
    CHECK(s.update(10, false, false, true) == Action::None);
    CHECK(s.update(20, false, true, false) == Action::EndSession);
    CHECK(s.endCause() == Cause::Shutdown);
    CHECK(s.update(1020, false, false, false) == Action::ForceReenumerate);
}

// The normal wireless case: the dongle re-cycles its USB by itself after the session
// ends. That must win over our port reset, and it refills the budget.
TEST(AaLinkSupervisor, DongleRecyclingItselfCancelsTheForce) {
    AaLinkSupervisor s(cfg());
    s.onTransportBound();
    CHECK(s.update(0, true, false, false) == Action::EndSession);
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
        CHECK(s.update(t, true, true, false) == Action::None);  // said once, then quiet
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
    CHECK(s.update(t, false, false, true) == Action::None);  // a real session came up
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
    CHECK(s.update(forcedAt + 4999, false, false, false) == Action::None);
    CHECK(s.update(forcedAt + 5000, false, false, false) == Action::DongleLost);
    CHECK(!s.awaitingReturn());
    CHECK(s.update(forcedAt + 60000, false, false, false) == Action::None);
    // It does come back later (replugged): a fresh budget.
    s.onTransportBound();
    CHECK_EQ(s.forcedAttempts(), 0u);
}

// A plain unplug is the user's doing: no "did not come back" report, however long.
TEST(AaLinkSupervisor, UnplugIsNotReportedAsLost) {
    AaLinkSupervisor s(cfg());
    s.onTransportBound();
    CHECK(s.update(0, false, false, true) == Action::None);
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
    CHECK(s.update(t + 1, false, false, false) == Action::None);
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
        const Action a = s.update(t, bound && dead, false, bound && running);
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
