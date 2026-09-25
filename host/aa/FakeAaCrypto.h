// FakeAaCrypto — host IAaCrypto that completes a scripted "handshake" after a
// fixed number of steps and uses identity encryption, so AaConnection's bring-up
// state machine (version → handshake → auth → service discovery → channel opens)
// is host-testable without a real TLS stack. Identity crypto means an encrypted
// frame's plaintext (msg id + protobuf) is recoverable in the test, letting it
// assert the encrypted dispatch path too. See tests/aa/AaConnection_test.
//
// Failure injection: failDecryptAt(n) makes the n-th decrypt() call (1-based) fail
// the way a bad TLS record does — empty plaintext + a sticky failed() — so the
// connection's unrecoverable-crypto path is host-testable. decryptCalls() counts
// every record the messenger fed in, so tests can prove no frame was skipped.
#pragma once

#include <cstdint>
#include <vector>

#include "aa/IAaCrypto.h"

namespace carsh::host {

class FakeAaCrypto : public carsh::aa::IAaCrypto {
public:
    // The handshake completes once `stepsToComplete` steps have run (the kick
    // plus the phone's handshake messages).
    explicit FakeAaCrypto(int stepsToComplete = 2) : stepsToComplete_(stepsToComplete) {}

    std::vector<uint8_t> handshakeStep(const uint8_t* /*in*/, size_t /*len*/) override {
        ++steps_;
        if (failHandshake_) {
            failed_ = true;
            return {};
        }
        if (steps_ >= stepsToComplete_) done_ = true;
        return {0xAA, static_cast<uint8_t>(steps_)};  // dummy handshake token
    }
    bool handshakeDone() const override { return done_; }

    // Identity transforms — ciphertext == plaintext, so tests can read back the
    // msg id + payload the connection encrypted.
    std::vector<uint8_t> encrypt(const uint8_t* plain, size_t len) override {
        if (failed_) return {};
        return std::vector<uint8_t>(plain, plain + len);
    }
    std::vector<uint8_t> decrypt(const uint8_t* cipher, size_t len) override {
        ++decryptCalls_;
        if (failed_ || (failDecryptAt_ != 0 && decryptCalls_ == failDecryptAt_)) {
            failed_ = true;  // a bad record kills the TLS session for good
            return {};
        }
        return std::vector<uint8_t>(cipher, cipher + len);
    }
    bool failed() const override { return failed_; }

    // --- test controls -----------------------------------------------------------
    void failDecryptAt(unsigned n) { failDecryptAt_ = n; }  // 1-based decrypt() call
    void failHandshake() { failHandshake_ = true; }
    unsigned decryptCalls() const { return decryptCalls_; }

private:
    int stepsToComplete_;
    int steps_ = 0;
    bool done_ = false;
    bool failed_ = false;
    bool failHandshake_ = false;
    unsigned failDecryptAt_ = 0;
    unsigned decryptCalls_ = 0;
};

}  // namespace carsh::host
