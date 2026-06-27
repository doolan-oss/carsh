// FakeAaTransport — host IAaTransport that replays captured AA bytes into the
// protocol stack and records everything the stack sends. The read-chunk size is
// configurable so tests can simulate USB bulk transfers NOT aligning to AA frame
// boundaries (the exact condition AaMessenger must tolerate). See tests/aa.
#pragma once

#include <algorithm>
#include <cstring>
#include <vector>

#include "aa/IAaTransport.h"

namespace carsh::host {

class FakeAaTransport : public carsh::aa::IAaTransport {
public:
    // Queue bytes the head unit will subsequently "receive".
    void pushRx(const uint8_t* data, size_t len) { rx_.insert(rx_.end(), data, data + len); }
    void pushRx(const std::vector<uint8_t>& v) { pushRx(v.data(), v.size()); }

    // Cap bytes returned per read() to simulate USB transfer fragmentation.
    // 0 (default) returns as much as the buffer allows in one read.
    void setReadChunk(size_t n) { readChunk_ = n; }

    int read(uint8_t* buf, size_t len, uint32_t /*timeoutMs*/) override {
        if (rxPos_ >= rx_.size()) return 0;  // no data -> timeout
        size_t avail = rx_.size() - rxPos_;
        size_t n = std::min(avail, len);
        if (readChunk_) n = std::min(n, readChunk_);
        std::memcpy(buf, rx_.data() + rxPos_, n);
        rxPos_ += n;
        return static_cast<int>(n);
    }

    int write(const uint8_t* data, size_t len, uint32_t /*timeoutMs*/) override {
        tx_.insert(tx_.end(), data, data + len);
        return static_cast<int>(len);
    }

    // Everything the stack has sent so far (raw framed bytes).
    const std::vector<uint8_t>& sent() const { return tx_; }
    void clearSent() { tx_.clear(); }
    size_t rxRemaining() const { return rx_.size() - rxPos_; }

private:
    std::vector<uint8_t> rx_;
    std::vector<uint8_t> tx_;
    size_t rxPos_ = 0;
    size_t readChunk_ = 0;
};

}  // namespace carsh::host
