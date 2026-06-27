// IAaTransport — the byte-stream seam under the Android Auto protocol stack.
// Everything above (AaMessenger framing, channels, crypto) is platform-
// independent and talks only to this interface. On device it is implemented by
// CircleAoaTransport (Circle USB host → the wireless-AA dongle); in host tests by
// FakeAaTransport (replaying captured AA traffic). This is the single seam that
// makes the whole AA core host-testable with no radio. See AA_PLAN.md.
#pragma once

#include <cstddef>
#include <cstdint>

namespace carsh::aa {

class IAaTransport {
public:
    virtual ~IAaTransport() = default;

    // Read up to len bytes into buf. Returns the number of bytes read (>0), 0 on
    // timeout (no data within timeoutMs), or <0 on error/disconnect. USB bulk
    // reads do NOT preserve AA frame boundaries — a read may return several
    // coalesced frames or only part of one; AaMessenger reassembles across reads.
    virtual int read(uint8_t* buf, size_t len, uint32_t timeoutMs) = 0;

    // Write len bytes. Returns bytes written (>=0) or <0 on error. The transport
    // is responsible for fully draining short writes.
    virtual int write(const uint8_t* data, size_t len, uint32_t timeoutMs) = 0;
};

}  // namespace carsh::aa
