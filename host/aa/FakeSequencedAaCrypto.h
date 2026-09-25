// FakeSequencedAaCrypto — host IAaCrypto that models the one TLS property the AA
// receive path depends on: records are sequence-numbered, so they must be decrypted
// in order and a skipped (lost, or never-fed) record breaks every record after it.
// encrypt() prefixes a 4-byte big-endian sequence number; decrypt() requires the
// next expected number and otherwise fails for good (empty + sticky failed()), just
// like mbedTLS's MAC check after a lost record. Two instances (phone + head unit)
// form a session. Handshake is scripted like FakeAaCrypto. See tests/aa.
#pragma once

#include <cstdint>
#include <vector>

#include "aa/IAaCrypto.h"

namespace carsh::host {

class FakeSequencedAaCrypto : public carsh::aa::IAaCrypto {
public:
    // handshakeDone from the start (a phone-side peer), or after `stepsToComplete`
    // handshakeStep() calls (the head unit under test).
    explicit FakeSequencedAaCrypto(int stepsToComplete = 2)
        : stepsToComplete_(stepsToComplete), done_(stepsToComplete <= 0) {}

    std::vector<uint8_t> handshakeStep(const uint8_t* /*in*/, size_t /*len*/) override {
        if (++steps_ >= stepsToComplete_) done_ = true;
        return {0xAA, static_cast<uint8_t>(steps_)};
    }
    bool handshakeDone() const override { return done_; }

    std::vector<uint8_t> encrypt(const uint8_t* plain, size_t len) override {
        if (!done_ || failed_) return {};
        std::vector<uint8_t> out = {static_cast<uint8_t>(txSeq_ >> 24),
                                    static_cast<uint8_t>(txSeq_ >> 16),
                                    static_cast<uint8_t>(txSeq_ >> 8),
                                    static_cast<uint8_t>(txSeq_)};
        ++txSeq_;
        out.insert(out.end(), plain, plain + len);
        return out;
    }
    std::vector<uint8_t> decrypt(const uint8_t* cipher, size_t len) override {
        if (!done_ || failed_) return {};
        ++records_;
        if (len < 4) {
            failed_ = true;
            return {};
        }
        const uint32_t seq = (static_cast<uint32_t>(cipher[0]) << 24) |
                             (static_cast<uint32_t>(cipher[1]) << 16) |
                             (static_cast<uint32_t>(cipher[2]) << 8) | cipher[3];
        if (seq != rxSeq_) {  // a record went missing (or arrived twice): "bad MAC"
            failed_ = true;
            return {};
        }
        ++rxSeq_;
        return std::vector<uint8_t>(cipher + 4, cipher + len);
    }
    bool failed() const override { return failed_; }

    unsigned recordsDecrypted() const { return records_; }  // decrypt() calls seen

private:
    int stepsToComplete_;
    int steps_ = 0;
    bool done_;
    bool failed_ = false;
    uint32_t txSeq_ = 0;
    uint32_t rxSeq_ = 0;
    unsigned records_ = 0;
};

}  // namespace carsh::host
