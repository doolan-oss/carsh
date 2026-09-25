// TouchRing / SpscRing — the allocation-free SPSC queue the device's 100 Hz touch
// pump (timer IRQ) fills and the UI loop drains (review T4).
#include "hal/TouchRing.h"

#include <atomic>
#include <cstdint>
#include <thread>

#include "Test.h"

using carsh::hal::SpscRing;
using carsh::hal::StampedTouch;
using carsh::hal::TouchPhase;
using carsh::hal::TouchRing;

namespace {
StampedTouch ev(TouchPhase p, int x, int y, uint64_t us, int id = 0) {
    StampedTouch s;
    s.ev.phase = p;
    s.ev.id = id;
    s.ev.x = x;
    s.ev.y = y;
    s.us = us;
    return s;
}
}  // namespace

TEST(TouchRing, FifoOrderAndStampsPreserved) {
    TouchRing r;
    CHECK(r.empty());
    CHECK(r.pushEvent(ev(TouchPhase::Down, 10, 20, 1000)));
    CHECK(r.pushEvent(ev(TouchPhase::Move, 11, 22, 11000)));
    CHECK(r.pushEvent(ev(TouchPhase::Up, 0, 0, 21000)));
    CHECK_EQ(r.size(), std::size_t(3));
    StampedTouch o;
    REQUIRE(r.pop(o));
    CHECK(o.ev.phase == TouchPhase::Down);
    CHECK_EQ(o.ev.x, 10);
    CHECK_EQ(o.us, std::uint64_t(1000));
    REQUIRE(r.pop(o));
    CHECK(o.ev.phase == TouchPhase::Move);
    CHECK_EQ(o.ev.y, 22);
    REQUIRE(r.pop(o));
    CHECK(o.ev.phase == TouchPhase::Up);
    CHECK_EQ(o.us, std::uint64_t(21000));
    CHECK(!r.pop(o));
    CHECK_EQ(r.dropped(), std::uint32_t(0));
}

TEST(TouchRing, WrapsAroundManyTimes) {
    SpscRing<int, 8> r;
    int next = 0, expect = 0;
    for (int round = 0; round < 1000; ++round) {
        for (int i = 0; i < 5; ++i) CHECK(r.push(next++));
        int v = -1;
        for (int i = 0; i < 5; ++i) {
            REQUIRE(r.pop(v));
            CHECK_EQ(v, expect++);
        }
    }
    CHECK(r.empty());
}

TEST(TouchRing, FullRingDropsAndCounts) {
    SpscRing<int, 4> r;
    for (int i = 0; i < 4; ++i) CHECK(r.push(i));
    CHECK(!r.push(99));
    CHECK_EQ(r.dropped(), std::uint32_t(1));
    int v = -1;
    REQUIRE(r.pop(v));
    CHECK_EQ(v, 0);
    CHECK(r.push(4));  // space again
    for (int want = 1; want <= 4; ++want) {
        REQUIRE(r.pop(v));
        CHECK_EQ(v, want);
    }
}

// Under a backlog, Moves are shed first so a Down/Up still gets a slot: a lost Up
// would leave the UI thinking the finger never lifted.
TEST(TouchRing, BacklogShedsMovesButKeepsTransitions) {
    TouchRing r;
    CHECK(r.pushEvent(ev(TouchPhase::Down, 5, 5, 0)));
    int moves = 0;
    while (r.pushEvent(ev(TouchPhase::Move, 5 + moves, 5, uint64_t(moves)))) ++moves;
    // Moves stop at capacity - kReserve (the Down holds one of those slots).
    CHECK_EQ(r.size(), TouchRing::capacity() - TouchRing::kReserve);
    CHECK(r.dropped() >= std::uint32_t(1));
    // The Up (and a following Down/Up pair) still fit.
    CHECK(r.pushEvent(ev(TouchPhase::Up, 0, 0, 999)));
    CHECK(r.pushEvent(ev(TouchPhase::Down, 7, 7, 1000)));
    CHECK(r.pushEvent(ev(TouchPhase::Up, 0, 0, 1001)));
    StampedTouch o, last;
    std::size_t n = 0;
    while (r.pop(o)) { last = o; ++n; }
    CHECK_EQ(n, TouchRing::capacity() - TouchRing::kReserve + 3);
    CHECK(last.ev.phase == TouchPhase::Up);
    CHECK_EQ(last.us, std::uint64_t(1001));
}

// A real concurrent producer/consumer: every event arrives once, in order.
TEST(TouchRing, ConcurrentProducerConsumerKeepsOrder) {
    static SpscRing<uint32_t, 64> r;  // static: keep the big test object off the stack
    constexpr uint32_t kCount = 200000;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (uint32_t i = 0; i < kCount;) {
            if (r.push(i)) ++i;  // spin on full (the device drops instead)
        }
        done.store(true);
    });
    uint32_t expect = 0;
    bool ordered = true;
    uint32_t v = 0;
    while (expect < kCount) {
        if (r.pop(v)) {
            if (v != expect) ordered = false;
            ++expect;
        } else if (done.load() && r.empty()) {
            break;
        }
    }
    producer.join();
    CHECK(ordered);
    CHECK_EQ(expect, kCount);
    CHECK(r.empty());
}
