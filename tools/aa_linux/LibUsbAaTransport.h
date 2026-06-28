// LibUsbAaTransport — a libusb implementation of carsh::aa::IAaTransport, for the
// Linux PC. It drives the SAME wireless-AA dongle (CA525) carsh drives on the Pi,
// but over Linux's USB stack instead of Circle's xHCI.
//
// ASYNC full-duplex model (mirrors aasdk's transport): a dedicated event thread runs
// libusb_handle_events; we keep kInFlight bulk-IN transfers ALWAYS submitted (timeout 0
// = they sit pending and complete the instant the phone sends), and each completion
// queues its bytes and immediately resubmits. write() submits an async bulk-OUT and
// blocks on its completion. This is what a phone expects from a head unit during video
// streaming — many reads in flight so it never has to wait on us — which a single
// synchronous read-at-a-time loop cannot provide.
#pragma once

#include <libusb.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

#include "aa/IAaTransport.h"

namespace carsh::aalinux {

class LibUsbAaTransport : public carsh::aa::IAaTransport {
public:
    static LibUsbAaTransport* openDongle(libusb_context* ctx, uint16_t preVid = 0x2717,
                                         uint16_t prePid = 0xFF40);

    ~LibUsbAaTransport() override;

    int read(uint8_t* buf, size_t len, uint32_t timeoutMs) override;
    int write(const uint8_t* data, size_t len, uint32_t timeoutMs) override;

    bool isGone() const { return gone_; }

private:
    static constexpr int kInFlight = 8;
    static constexpr int kInBufSize = 16384;

    LibUsbAaTransport(libusb_context* ctx, libusb_device_handle* h, uint8_t inEp,
                      uint8_t outEp);

    void eventLoop();
    static void LIBUSB_CALL inCallback(libusb_transfer* x);

    libusb_context* ctx_;
    libusb_device_handle* handle_;
    uint8_t inEp_;
    uint8_t outEp_;
    std::atomic<bool> gone_{false};
    std::atomic<bool> stop_{false};

    std::thread eventThread_;
    libusb_transfer* inXfers_[kInFlight] = {};
    uint8_t inBuf_[kInFlight][kInBufSize];

    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<uint8_t> rxq_;  // bytes drained from bulk-IN, consumed by read()
};

}  // namespace carsh::aalinux
