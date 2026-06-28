// LibUsbAaTransport.cpp — see header.
#include "LibUsbAaTransport.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

namespace carsh::aalinux {
namespace {

// Dongle identities (must match device/CircleAoaTransport).
constexpr uint16_t kDongleVid = 0x2717;
constexpr uint16_t kDonglePid = 0xFF40;
constexpr uint16_t kAccVid = 0x18D1;
constexpr uint16_t kAccPidLo = 0x2D00;
constexpr uint16_t kAccPidHi = 0x2D05;

// Android Open Accessory control requests.
constexpr uint8_t kReqGetProtocol = 51;
constexpr uint8_t kReqSendString = 52;
constexpr uint8_t kReqStart = 53;

// The AA-launching identity strings — IDENTICAL to device/CircleAoaTransport so the
// dongle/phone treat this exactly like the Pi head unit.
struct { uint16_t index; const char* str; } kStrings[] = {
    {0, "Android"},
    {1, "Android Auto"},
    {2, "carsh head unit"},
    {3, "2.0.1"},
    {4, "https://github.com/dylant9/carsh"},
    {5, "carsh-0001"},
};

bool runAoa(libusb_device_handle* h) {
    unsigned char proto[2] = {0, 0};
    int r = libusb_control_transfer(
        h, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        kReqGetProtocol, 0, 0, proto, sizeof(proto), 1000);
    if (r < 2) {
        printf("aoa: GET_PROTOCOL failed (%d)\n", r);
        return false;
    }
    const int version = proto[0] | (proto[1] << 8);
    printf("aoa: AOA v%d\n", version);
    if (version < 1) return false;

    for (auto& s : kStrings) {
        r = libusb_control_transfer(
            h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
            kReqSendString, 0, s.index, (unsigned char*)s.str,
            (uint16_t)(std::strlen(s.str) + 1), 1000);
        if (r < 0) {
            printf("aoa: SEND_STRING[%u] failed (%d)\n", s.index, r);
            return false;
        }
    }
    r = libusb_control_transfer(
        h, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
        kReqStart, 0, 0, nullptr, 0, 1000);
    if (r < 0) {
        printf("aoa: START failed (%d)\n", r);
        return false;
    }
    printf("aoa: START sent — awaiting re-enumeration in accessory mode\n");
    return true;
}

// Find a device by VID + a PID range; returns an opened handle or nullptr.
libusb_device_handle* findOpen(libusb_context* ctx, uint16_t vid, uint16_t pidLo,
                               uint16_t pidHi) {
    libusb_device** list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device_handle* found = nullptr;
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor d;
        if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
        if (d.idVendor == vid && d.idProduct >= pidLo && d.idProduct <= pidHi) {
            if (libusb_open(list[i], &found) != 0) found = nullptr;
            break;
        }
    }
    libusb_free_device_list(list, 1);
    return found;
}

}  // namespace

LibUsbAaTransport* LibUsbAaTransport::openDongle(libusb_context* ctx, uint16_t preVid,
                                                 uint16_t prePid) {
    // If something is already in AA accessory mode (a phone that auto-switched, or a
    // prior run), just bind it.
    libusb_device_handle* acc = findOpen(ctx, kAccVid, kAccPidLo, kAccPidHi);
    if (acc == nullptr) {
        // Find the pre-switch device (dongle 2717:ff40, or a phone's own id for direct
        // wired AA) and AOA-switch it. Poll persistently — it cycles.
        libusb_device_handle* h = nullptr;
        for (int i = 0; i < 150 && h == nullptr; ++i) {  // up to ~30 s
            h = findOpen(ctx, preVid, prePid, prePid);
            if (h == nullptr) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (h == nullptr) {
            printf("error: pre-switch device %04x:%04x not seen in 30s — plug it in"
                   " (and for a phone, enable/allow Android Auto over USB).\n", preVid,
                   prePid);
            return nullptr;
        }
        libusb_set_auto_detach_kernel_driver(h, 1);
        printf("aoa: found %04x:%04x — running AOA switch\n", preVid, prePid);
        if (!runAoa(h)) {
            libusb_close(h);
            return nullptr;
        }
        libusb_close(h);
        for (int i = 0; i < 100 && acc == nullptr; ++i) {  // wait ~10s for accessory mode
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            acc = findOpen(ctx, kAccVid, kAccPidLo, kAccPidHi);
        }
        if (acc == nullptr) {
            printf("error: did not re-enumerate in accessory mode (%04x:%04x-%04x)\n",
                   kAccVid, kAccPidLo, kAccPidHi);
            return nullptr;
        }
    }
    printf("aoa: accessory device up — binding interface + bulk endpoints\n");

    // Inline the bind here so we can construct the (private-ctor) transport.
    libusb_set_auto_detach_kernel_driver(acc, 1);
    if (libusb_claim_interface(acc, 0) != 0) {
        printf("error: cannot claim interface 0 (need root / udev rule?)\n");
        libusb_close(acc);
        return nullptr;
    }
    libusb_device* dev = libusb_get_device(acc);
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0 || cfg == nullptr) {
        printf("error: no active config descriptor\n");
        libusb_close(acc);
        return nullptr;
    }
    uint8_t inEp = 0, outEp = 0;
    const libusb_interface_descriptor* id = &cfg->interface[0].altsetting[0];
    for (int e = 0; e < id->bNumEndpoints; ++e) {
        const libusb_endpoint_descriptor* ep = &id->endpoint[e];
        if ((ep->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK) continue;
        if (ep->bEndpointAddress & 0x80)
            inEp = ep->bEndpointAddress;
        else
            outEp = ep->bEndpointAddress;
    }
    libusb_free_config_descriptor(cfg);
    if (inEp == 0 || outEp == 0) {
        printf("error: bulk endpoints not found (in=%02x out=%02x)\n", inEp, outEp);
        libusb_close(acc);
        return nullptr;
    }
    printf("aoa: bulk endpoints bound (IN %02x / OUT %02x) — AA transport ready\n", inEp,
           outEp);
    return new LibUsbAaTransport(ctx, acc, inEp, outEp);
}

LibUsbAaTransport::LibUsbAaTransport(libusb_context* ctx, libusb_device_handle* h,
                                     uint8_t inEp, uint8_t outEp)
    : ctx_(ctx), handle_(h), inEp_(inEp), outEp_(outEp) {
    // Start the event thread, then submit kInFlight persistent bulk-IN transfers
    // (timeout 0 = pending until data arrives). Each completion queues + resubmits.
    eventThread_ = std::thread(&LibUsbAaTransport::eventLoop, this);
    for (int i = 0; i < kInFlight; ++i) {
        inXfers_[i] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(inXfers_[i], handle_, inEp_, inBuf_[i], kInBufSize,
                                  &LibUsbAaTransport::inCallback, this, 0);
        if (libusb_submit_transfer(inXfers_[i]) != 0) {
            printf("!! failed to submit bulk-IN transfer %d\n", i);
        }
    }
    printf("aoa: async transport up (%d bulk-IN transfers in flight)\n", kInFlight);
}

LibUsbAaTransport::~LibUsbAaTransport() {
    stop_ = true;
    for (int i = 0; i < kInFlight; ++i)
        if (inXfers_[i]) libusb_cancel_transfer(inXfers_[i]);
    if (eventThread_.joinable()) eventThread_.join();
    for (int i = 0; i < kInFlight; ++i)
        if (inXfers_[i]) libusb_free_transfer(inXfers_[i]);
    if (handle_ != nullptr) {
        libusb_release_interface(handle_, 0);
        libusb_close(handle_);
    }
}

// Drive libusb's event loop so submitted transfers complete via callbacks.
void LibUsbAaTransport::eventLoop() {
    while (!stop_) {
        timeval tv{0, 100000};  // 100 ms tick
        libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
    }
}

// A bulk-IN transfer completed: queue its bytes and resubmit so a read is always posted.
void LibUsbAaTransport::inCallback(libusb_transfer* x) {
    auto* self = static_cast<LibUsbAaTransport*>(x->user_data);
    if (x->status == LIBUSB_TRANSFER_COMPLETED) {
        if (x->actual_length > 0) {
            std::lock_guard<std::mutex> lk(self->mtx_);
            self->rxq_.insert(self->rxq_.end(), x->buffer, x->buffer + x->actual_length);
            self->cv_.notify_one();
        }
        if (!self->stop_) libusb_submit_transfer(x);
    } else if (x->status == LIBUSB_TRANSFER_TIMED_OUT) {
        if (!self->stop_) libusb_submit_transfer(x);
    } else if (x->status != LIBUSB_TRANSFER_CANCELLED) {
        // NO_DEVICE / error: the link dropped.
        printf("!! bulk-IN transfer status %d\n", (int)x->status);
        self->gone_ = true;
        self->cv_.notify_all();
    }
}

int LibUsbAaTransport::read(uint8_t* buf, size_t len, uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lk(mtx_);
    if (rxq_.empty() && !gone_) {
        const auto wait = std::chrono::milliseconds(timeoutMs == 0 ? 5 : timeoutMs);
        cv_.wait_for(lk, wait, [&] { return !rxq_.empty() || gone_; });
    }
    if (rxq_.empty()) return gone_ ? -1 : 0;
    const size_t n = std::min(len, rxq_.size());
    std::copy(rxq_.begin(), rxq_.begin() + n, buf);
    rxq_.erase(rxq_.begin(), rxq_.begin() + n);
    return (int)n;
}

namespace {
struct OutCtx {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    int status = 0;
    int actual = 0;
};
void LIBUSB_CALL outCallback(libusb_transfer* x) {
    auto* o = static_cast<OutCtx*>(x->user_data);
    std::lock_guard<std::mutex> lk(o->m);
    o->status = x->status;
    o->actual = x->actual_length;
    o->done = true;
    o->cv.notify_one();
}
}  // namespace

int LibUsbAaTransport::write(const uint8_t* data, size_t len, uint32_t timeoutMs) {
    // Submit an async bulk-OUT and block on its completion (the event thread services it).
    OutCtx octx;
    libusb_transfer* x = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(x, handle_, outEp_, const_cast<uint8_t*>(data), (int)len,
                              &outCallback, &octx, timeoutMs == 0 ? 2000 : timeoutMs);
    if (libusb_submit_transfer(x) != 0) {
        libusb_free_transfer(x);
        gone_ = true;
        return -1;
    }
    {
        std::unique_lock<std::mutex> lk(octx.m);
        octx.cv.wait(lk, [&] { return octx.done; });
    }
    const int status = octx.status, actual = octx.actual;
    libusb_free_transfer(x);
    if (status == LIBUSB_TRANSFER_COMPLETED) return actual;
    if (status == LIBUSB_TRANSFER_TIMED_OUT) {
        printf("!! bulk-OUT timeout (%d/%zu bytes)\n", actual, len);
        return actual;  // partial; AaMessenger will not normally hit this for control msgs
    }
    printf("!! bulk-OUT transfer status %d\n", status);
    gone_ = true;
    return -1;
}

}  // namespace carsh::aalinux
