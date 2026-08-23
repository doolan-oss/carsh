// FrameRelay — the cross-core decoded-frame handoff. Sequential tests pin the
// latest-wins semantics; a multi-threaded stress test proves frames are never torn
// (the property the triple buffer exists to guarantee).
#include "aa/FrameRelay.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "Test.h"

using carsh::aa::FrameRelay;

namespace {
std::vector<std::uint8_t> solid(int w, int h, std::uint8_t v) {
    return std::vector<std::uint8_t>(std::size_t(w) * h * 3, v);
}
}  // namespace

TEST(FrameRelay, PublishThenAcquire) {
    FrameRelay r;
    const std::uint8_t* rgb = nullptr;
    int w = 0, h = 0;
    CHECK(!r.acquire(rgb, w, h));  // nothing yet

    auto a = solid(4, 2, 0x11);
    r.publish(a.data(), 4, 2);
    REQUIRE(r.acquire(rgb, w, h));
    CHECK_EQ(w, 4);
    CHECK_EQ(h, 2);
    CHECK_EQ(int(rgb[0]), 0x11);
    CHECK(!r.acquire(rgb, w, h));  // consumed; nothing new
}

TEST(FrameRelay, LatestWinsStaleDropped) {
    FrameRelay r;
    const std::uint8_t* rgb = nullptr;
    int w = 0, h = 0;
    auto a = solid(4, 4, 0x22), b = solid(4, 4, 0x33), c = solid(4, 4, 0x44);
    r.publish(a.data(), 4, 4);
    r.publish(b.data(), 4, 4);
    r.publish(c.data(), 4, 4);     // three published, none consumed
    REQUIRE(r.acquire(rgb, w, h));
    CHECK_EQ(int(rgb[0]), 0x44);   // only the newest survives
    CHECK(!r.acquire(rgb, w, h));
    CHECK_EQ(r.published(), std::uint64_t(3));
    CHECK_EQ(r.consumed(), std::uint64_t(1));
}

TEST(FrameRelay, IgnoresEmptyPublish) {
    FrameRelay r;
    const std::uint8_t* rgb = nullptr;
    int w = 0, h = 0;
    r.publish(nullptr, 4, 4);
    auto a = solid(4, 4, 0x55);
    r.publish(a.data(), 0, 4);     // bad dims
    CHECK(!r.acquire(rgb, w, h));
    r.publish(a.data(), 4, 4);
    CHECK(r.acquire(rgb, w, h));
}

// The real point: under genuine concurrency the consumer must never observe a frame
// that mixes bytes from two publishes. Every frame the producer writes is uniform
// (all bytes == frameId & 0xFF), so any non-uniform acquired frame == a tear.
TEST(FrameRelay, NoTearingUnderConcurrency) {
    FrameRelay r;
    constexpr int W = 320, H = 180;            // ~172 KB per frame
    constexpr std::uint64_t kFrames = 20000;
    std::atomic<bool> torn{false};
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> seen{0};

    auto check = [&](const std::uint8_t* rgb, int w, int h) {
        bool uniform = (w == W && h == H);
        const std::uint8_t v0 = rgb[0];
        for (std::size_t i = 0; uniform && i < std::size_t(w) * h * 3; ++i)
            if (rgb[i] != v0) uniform = false;
        if (!uniform) torn.store(true);
        seen.fetch_add(1);
    };

    std::thread producer([&] {
        std::vector<std::uint8_t> buf(std::size_t(W) * H * 3);
        for (std::uint64_t f = 1; f <= kFrames; ++f) {
            const std::uint8_t v = std::uint8_t(f & 0xFF);
            for (auto& b : buf) b = v;
            r.publish(buf.data(), W, H);
        }
        done.store(true);
    });
    std::thread consumer([&] {
        const std::uint8_t* rgb = nullptr;
        int w = 0, h = 0;
        while (!done.load())
            if (r.acquire(rgb, w, h)) check(rgb, w, h);
        if (r.acquire(rgb, w, h)) check(rgb, w, h);  // final drain
    });
    producer.join();
    consumer.join();

    CHECK(!torn.load());            // no frame ever mixed two publishes
    CHECK(seen.load() > 0);         // the consumer actually saw frames
}
