#include "audio/PcmRing.h"

#include <thread>
#include <vector>

#include "Test.h"

using carsh::audio::PcmRing;

TEST(PcmRing, CapacityRoundsToPow2MinusOne) {
    PcmRing<float> r(1000);
    // next pow2 >= 1001 is 1024; usable capacity is 1023.
    CHECK_EQ(r.capacity(), std::size_t(1023));
    PcmRing<float> r2(1024);
    CHECK_EQ(r2.capacity(), std::size_t(2047));  // needs +1 slot -> 2048
}

TEST(PcmRing, WriteReadPreservesOrder) {
    PcmRing<int> r(16);
    int in[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    CHECK_EQ(r.write(in, 8), std::size_t(8));
    CHECK_EQ(r.readable(), std::size_t(8));
    int out[8] = {0};
    CHECK_EQ(r.read(out, 8), std::size_t(8));
    for (int i = 0; i < 8; ++i) CHECK_EQ(out[i], i);
    CHECK_EQ(r.readable(), std::size_t(0));
}

TEST(PcmRing, WriteSaturatesAtCapacity) {
    PcmRing<int> r(8);  // capacity 15? -> next pow2>=9 is 16, usable 15
    std::vector<int> in(100, 7);
    std::size_t w = r.write(in.data(), in.size());
    CHECK_EQ(w, r.capacity());
    CHECK_EQ(r.writable(), std::size_t(0));
}

TEST(PcmRing, ReadUnderrunReturnsPartial) {
    PcmRing<int> r(16);
    int in[3] = {9, 9, 9};
    r.write(in, 3);
    int out[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    CHECK_EQ(r.read(out, 8), std::size_t(3));  // only 3 available
    CHECK_EQ(out[3], -1);                       // remainder untouched
}

TEST(PcmRing, WrapAroundIsContiguous) {
    PcmRing<int> r(8);  // usable 15
    // Fill, drain most, then write across the wrap boundary.
    std::vector<int> a(10);
    for (int i = 0; i < 10; ++i) a[i] = i;
    r.write(a.data(), 10);
    int tmp[8];
    r.read(tmp, 8);  // drain 8, tail now near end
    std::vector<int> b(10);
    for (int i = 0; i < 10; ++i) b[i] = 100 + i;
    r.write(b.data(), 10);  // wraps
    // Remaining: 2 of a (8,9) then 10 of b
    int out[12];
    std::size_t got = r.read(out, 12);
    CHECK_EQ(got, std::size_t(12));
    CHECK_EQ(out[0], 8);
    CHECK_EQ(out[1], 9);
    for (int i = 0; i < 10; ++i) CHECK_EQ(out[2 + i], 100 + i);
}

TEST(PcmRing, DiscardDropsFromFront) {
    PcmRing<int> r(16);
    int in[6] = {1, 2, 3, 4, 5, 6};
    r.write(in, 6);
    CHECK_EQ(r.discard(2), std::size_t(2));
    int out[4];
    CHECK_EQ(r.read(out, 4), std::size_t(4));
    CHECK_EQ(out[0], 3);
    CHECK_EQ(out[3], 6);
}

// Concurrency smoke: one producer thread, one consumer thread, large transfer.
// The host has real threads (the device does not), but this exercises the
// memory-ordering contract that holds on the Pi between the decode task and the
// DMA callback.
TEST(PcmRing, SpscConcurrentTransferIsLossless) {
    constexpr int N = 1'000'000;
    PcmRing<int> r(4096);
    std::vector<int> got;
    got.reserve(N);

    std::thread producer([&] {
        int i = 0;
        while (i < N) {
            int v = i;
            if (r.write(&v, 1) == 1)
                ++i;
            else
                std::this_thread::yield();
        }
    });
    std::thread consumer([&] {
        int i = 0;
        while (i < N) {
            int v;
            if (r.read(&v, 1) == 1) {
                got.push_back(v);
                ++i;
            } else {
                std::this_thread::yield();
            }
        }
    });
    producer.join();
    consumer.join();

    REQUIRE(got.size() == std::size_t(N));
    bool ordered = true;
    for (int i = 0; i < N; ++i)
        if (got[i] != i) { ordered = false; break; }
    CHECK(ordered);
}
