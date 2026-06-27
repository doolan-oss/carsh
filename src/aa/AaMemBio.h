// AaMemBio — the in-memory byte buffer that bridges mbedTLS's socket-style BIO
// callbacks to the Android Auto transport. mbedTLS believes it is reading/writing a
// socket; instead, bytes it "sends" accumulate in an out-buffer (AaCryptoMbedTls
// drains them and ships them as an SSL_HANDSHAKE / encrypted control frame), and
// bytes the phone sends are pushed into an in-buffer for mbedTLS to "receive".
// recv() returns kWantRead when the in-buffer is drained, which mbedTLS treats as a
// non-fatal "would block" and unwinds cleanly. Pure and header-only so the framing
// plumbing is host-testable with no TLS stack linked. See AaCryptoMbedTls.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace carsh::aa {

class AaMemBio {
public:
    // mbedTLS's MBEDTLS_ERR_SSL_WANT_READ. Kept as a literal so this header carries
    // no mbedTLS dependency; AaCryptoMbedTls.cpp static_asserts the two agree.
    static constexpr int kWantRead = -0x6900;

    // Phone -> us: queue received handshake/ciphertext bytes for mbedTLS to read.
    void pushIn(const uint8_t* p, size_t n) {
        if (n) inBuf_.insert(inBuf_.end(), p, p + n);
    }

    // mbedTLS bio_recv: copy up to len bytes out of the in-buffer; returns the count
    // (>0), or kWantRead when nothing is buffered. Compacts once fully drained.
    int recv(uint8_t* buf, size_t len) {
        const size_t avail = inBuf_.size() - inPos_;
        if (avail == 0) return kWantRead;
        const size_t n = len < avail ? len : avail;
        std::memcpy(buf, inBuf_.data() + inPos_, n);
        inPos_ += n;
        if (inPos_ == inBuf_.size()) {
            inBuf_.clear();
            inPos_ = 0;
        }
        return static_cast<int>(n);
    }

    // mbedTLS bio_send: append the bytes mbedTLS wants to transmit. Accepts all.
    int send(const uint8_t* buf, size_t len) {
        outBuf_.insert(outBuf_.end(), buf, buf + len);
        return static_cast<int>(len);
    }

    // Take (and clear) everything mbedTLS has queued to transmit since last drain.
    std::vector<uint8_t> drainOut() {
        std::vector<uint8_t> out;
        out.swap(outBuf_);
        return out;
    }

    size_t inAvailable() const { return inBuf_.size() - inPos_; }
    bool outEmpty() const { return outBuf_.empty(); }

private:
    std::vector<uint8_t> inBuf_;
    size_t inPos_ = 0;
    std::vector<uint8_t> outBuf_;
};

}  // namespace carsh::aa
