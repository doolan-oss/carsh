// A/B boot slot state machine (PLAN.md §3 Phase 5). The chainloader reads a
// boot_slot record {active, trying, triesLeft, healthy, versionA/B} and decides
// which kernel image to jump to, implementing try/rollback. This module is the
// pure decision logic — no flashing — so it is fully host-testable.
//
// Flow: stageUpdate() arms the inactive slot as the new active with try=true and
// a tries budget. Each boot the chainloader calls decide(): while trying and
// budget remains it boots the new slot and decrements; if the budget is
// exhausted (the new image never confirmed) it rolls back to the other slot. A
// healthy boot calls confirmHealthy() to clear the try flag and commit.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace carsh::ota {

struct BootRecord {
    uint8_t activeSlot = 0;   // 0 = A, 1 = B
    bool trying = false;      // an unconfirmed update is on trial
    uint8_t triesLeft = 0;    // boots remaining before rollback
    bool healthy = false;     // active slot confirmed healthy
    uint32_t versionA = 0;
    uint32_t versionB = 0;

    uint32_t activeVersion() const {
        return activeSlot == 0 ? versionA : versionB;
    }
};

inline uint8_t otherSlot(uint8_t s) { return s == 0 ? 1 : 0; }
inline uint8_t inactiveSlot(const BootRecord& r) { return otherSlot(r.activeSlot); }

struct BootDecision {
    uint8_t bootSlot = 0;
    BootRecord next;     // record to persist before jumping
    bool rolledBack = false;
};

// Chainloader decision for this boot.
inline BootDecision decide(const BootRecord& r) {
    BootDecision d;
    d.next = r;
    if (!r.trying) {  // steady state
        d.bootSlot = r.activeSlot;
        return d;
    }
    if (r.triesLeft == 0) {  // trial exhausted -> roll back to the other slot
        d.next.activeSlot = otherSlot(r.activeSlot);
        d.next.trying = false;
        d.next.healthy = true;  // the fallback slot is the last-known-good
        d.bootSlot = d.next.activeSlot;
        d.rolledBack = true;
        return d;
    }
    d.next.triesLeft = uint8_t(r.triesLeft - 1);  // consume one attempt
    d.bootSlot = r.activeSlot;
    return d;
}

// Arm a freshly-written update (in the inactive slot) for trial.
inline BootRecord stageUpdate(const BootRecord& r, uint32_t newVersion,
                              uint8_t triesAllowed) {
    BootRecord n = r;
    uint8_t target = inactiveSlot(r);
    n.activeSlot = target;
    n.trying = true;
    n.triesLeft = triesAllowed;
    n.healthy = false;
    if (target == 0) n.versionA = newVersion;
    else n.versionB = newVersion;
    return n;
}

// The app calls this after a successful start to commit the update.
inline BootRecord confirmHealthy(const BootRecord& r) {
    BootRecord n = r;
    n.trying = false;
    n.healthy = true;
    return n;
}

// Explicit manual rollback (e.g. user-triggered): revert to the other slot.
inline BootRecord rollback(const BootRecord& r) {
    BootRecord n = r;
    n.activeSlot = otherSlot(r.activeSlot);
    n.trying = false;
    n.healthy = true;
    return n;
}

// boot_slot.bin: fixed 16-byte little-endian record (magic + fields).
inline std::vector<uint8_t> serializeBootRecord(const BootRecord& r) {
    std::vector<uint8_t> v = {'B', 'S', 'L', '1'};
    v.push_back(r.activeSlot);
    v.push_back(r.trying ? 1 : 0);
    v.push_back(r.triesLeft);
    v.push_back(r.healthy ? 1 : 0);
    auto put = [&](uint32_t x) {
        for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
    };
    put(r.versionA);
    put(r.versionB);
    return v;
}

inline bool deserializeBootRecord(const uint8_t* d, std::size_t len,
                                  BootRecord& out) {
    if (len < 16 || std::memcmp(d, "BSL1", 4) != 0) return false;
    out.activeSlot = d[4];
    out.trying = d[5] != 0;
    out.triesLeft = d[6];
    out.healthy = d[7] != 0;
    auto get = [&](std::size_t p) {
        return uint32_t(d[p]) | (uint32_t(d[p + 1]) << 8) |
               (uint32_t(d[p + 2]) << 16) | (uint32_t(d[p + 3]) << 24);
    };
    out.versionA = get(8);
    out.versionB = get(12);
    return true;
}

}  // namespace carsh::ota
