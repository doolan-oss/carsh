// CircleAoaTransport.cpp — see CircleAoaTransport.h. Driver shape + bulk plumbing
// mirror CUSBBluetoothDevice (the validated repo USB pattern); the IRQ->ring->task
// hand-off mirrors CircleHciTransport.
#include "CircleAoaTransport.h"

#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/numberpool.h>
#include <circle/timer.h>
#include <circle/usb/usb.h>  // REQUEST_*, DESCRIPTOR_ENDPOINT, descriptors
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbhcidevice.h>  // CUSBHCIDevice (= CXHCIDevice on the Pi 4)
#include <circle/usb/usbhostcontroller.h>

#include <cstring>

#include "aa/AaTypes.h"  // kUsbTransferSize

namespace carsh::device {
namespace {

const char From[] = "aoa";
const char DevicePrefix[] = "uaa";
CNumberPool s_NumberPool(1);

// Send one AOA identity string (control req 52) to EP0, NUL-terminated. The data
// buffer MUST be heap memory: xHCI DMA asserts (pBuffer > MEM_KERNEL_END), so a
// string literal in .rodata (below the kernel end) can't be transferred directly —
// copy it to a heap buffer first.
int aoaSendString(CUSBDevice* pDevice, u16 index, const char* str) {
    unsigned len = 0;
    while (str[len] != '\0') ++len;
    ++len;  // include the terminating NUL
    u8* buf = new u8[len];
    for (unsigned i = 0; i < len; ++i) buf[i] = static_cast<u8>(str[i]);
    int r = pDevice->GetHost()->ControlMessage(
        pDevice->GetEndpoint0(), REQUEST_OUT | REQUEST_VENDOR | REQUEST_TO_DEVICE,
        aoa::kReqSendString, 0, index, buf, static_cast<u16>(len));
    delete[] buf;
    return r;
}

}  // namespace

void aoaDumpDeviceDescriptor(CUSBDevice* pDevice) {
    if (pDevice == nullptr) return;
    const TUSBDeviceDescriptor* d = pDevice->GetDeviceDescriptor();
    if (d == nullptr) {
        CLogger::Get()->Write(From, LogWarning, "no device descriptor");
        return;
    }
    CLogger::Get()->Write(From, LogNotice, "dongle VID %04x PID %04x class %u/%u/%u cfgs %u",
                          (unsigned)d->idVendor, (unsigned)d->idProduct,
                          (unsigned)d->bDeviceClass, (unsigned)d->bDeviceSubClass,
                          (unsigned)d->bDeviceProtocol, (unsigned)d->bNumConfigurations);
}

bool aoaStartAccessory(CUSBDevice* pDevice) {
    if (pDevice == nullptr) return false;
    CUSBHostController* host = pDevice->GetHost();
    CUSBEndpoint* ep0 = pDevice->GetEndpoint0();

    u8* proto = new u8[2];  // heap: xHCI DMA requires buffer > MEM_KERNEL_END
    proto[0] = proto[1] = 0;
    int r = host->ControlMessage(ep0, REQUEST_IN | REQUEST_VENDOR | REQUEST_TO_DEVICE,
                                 aoa::kReqGetProtocol, 0, 0, proto, 2);
    if (r < 2) {
        CLogger::Get()->Write(From, LogWarning, "device did not answer AOA probe (%d)", r);
        delete[] proto;
        return false;
    }
    const unsigned version = (unsigned)proto[0] | ((unsigned)proto[1] << 8);
    delete[] proto;
    if (version < 1) {
        CLogger::Get()->Write(From, LogWarning, "device reports no AOA support");
        return false;
    }
    CLogger::Get()->Write(From, LogNotice, "AOA v%u — sending AA identity", version);

    aoaSendString(pDevice, aoa::kStrManufacturer, "Android");
    aoaSendString(pDevice, aoa::kStrModel, "Android Auto");
    aoaSendString(pDevice, aoa::kStrDescription, "carsh head unit");
    aoaSendString(pDevice, aoa::kStrVersion, "2.0.1");
    aoaSendString(pDevice, aoa::kStrUri, "https://github.com/dylant9/carsh");
    aoaSendString(pDevice, aoa::kStrSerial, "carsh-0001");

    r = host->ControlMessage(ep0, REQUEST_OUT | REQUEST_VENDOR | REQUEST_TO_DEVICE,
                             aoa::kReqStartAccessory, 0, 0, nullptr, 0);
    if (r < 0) {
        CLogger::Get()->Write(From, LogError, "AOA start-accessory failed (%d)", r);
        return false;
    }
    CLogger::Get()->Write(From, LogNotice, "AOA start sent — awaiting re-enumeration");
    return true;
}

// Recovery design (review A3). Why a dead link is not revived in place on the Pi 4:
// a bulk-IN Transaction/Babble/Stall error halts the xHCI endpoint. Circle Step51
// completes only the failed URB: CXHCIEndpoint::TransferEvent shifts its two-slot
// m_pURB[] and the second posted URB's TD never runs on the halted ring, so it never
// completes. Reviving the pipe would need Reset Endpoint + Set TR Dequeue Pointer
// (CXHCIEndpoint::ResetFromHalted exists, but nothing calls it for bulk endpoints),
// CLEAR_FEATURE(ENDPOINT_HALT) to the dongle, AND a Circle patch to drop the stale
// m_pURB[0] entry (private; without it the next submit mis-credits completions, and a
// third submit trips TransferAsync's assert(m_pURB[1] == 0) — another hard hang). And
// even a revived pipe carries a broken stream: the lost bytes desync the AA framing
// and the TLS record sequence, so the session has to restart from VERSION_REQUEST —
// and, like a wired phone, the dongle can only be relied on to start a new session on
// a fresh USB connection (after AOA). So recovery = re-enumeration,
// using only public Circle API: CUSBFunction::RemoveDevice() (-> the parent hub's
// RemoveDeviceAt: CLEAR_FEATURE(PORT_ENABLE) + delete the CUSBDevice, which disables
// the xHCI slot before the functions — us — are deleted) and then
// CXHCIDevice::ReScanDevices(): the hub's EnumeratePorts finds the port connected
// but empty, port-resets it and enumerates the dongle afresh; our factory hook then
// drives the AOA switch (PTP identity) or binds uaa1 directly (accessory identity).
// The same enumerate path runs on every hub connect event, so nothing here is new
// territory for Circle. The dongle's own reset (the usual behaviour after an error)
// takes the ordinary PnP path instead; the driver gives it a grace period first.
bool aoaForceReenumerate(CAndroidAutoDevice* pDongle) {
    if (pDongle == nullptr) return false;
    CUSBHostController* host = pDongle->GetHost();
    // CUSBFunction::RemoveDevice -> CUSBDevice::RemoveDevice -> hub RemoveDeviceAt /
    // root-port RemoveDevice. On TRUE, pDongle is gone.
    if (!pDongle->RemoveDevice()) {
        CLogger::Get()->Write(From, LogError, "re-enumeration: cannot remove the dongle");
        return false;
    }
    static_cast<CUSBHCIDevice*>(host)->ReScanDevices();
    CLogger::Get()->Write(From, LogNotice, "re-enumeration: bus rescanned");
    return true;
}

CAndroidAutoDevice::CAndroidAutoDevice(CUSBFunction* pFunction)
    : CUSBFunction(pFunction),
      m_pBulkIn(nullptr),
      m_pBulkOut(nullptr),
      m_pInBuffer{nullptr, nullptr},
      m_pInURB{nullptr, nullptr},
      m_pOutBuffer(nullptr),
      m_nDeviceNumber(0),
      m_inFlight(0),
      m_lastOutBlockedMs(0) {}

CAndroidAutoDevice::~CAndroidAutoDevice() {
    // Unregister from the name service + free the number BEFORE the object dies, so
    // "uaa1" doesn't dangle after a disconnect (otherwise the next reconnect would
    // register as uaa2 and the driver, still watching uaa1, would never rebind).
    if (m_nDeviceNumber != 0) {
        CDeviceNameService::Get()->RemoveDevice(DevicePrefix, m_nDeviceNumber, FALSE);
        s_NumberPool.FreeNumber(m_nDeviceNumber);
    }
    // By now Circle has disabled the xHCI slot (~CXHCIUSBDevice runs before
    // ~CUSBDevice deletes its functions), so no request posted here can complete or
    // DMA any more: the endpoints, the requests stranded on them (e.g. the second IN
    // request of a halted pipe) and every buffer can simply be freed.
    delete m_pBulkIn;
    delete m_pBulkOut;
    for (unsigned i = 0; i < kInBuffers; ++i) {
        delete m_pInURB[i];
        delete[] m_pInBuffer[i];
    }
    delete[] m_pOutBuffer;
}

boolean CAndroidAutoDevice::Configure(void) {
    const TUSBDeviceDescriptor* d = GetDevice()->GetDeviceDescriptor();
    if (d == nullptr) return FALSE;
    CLogger::Get()->Write(From, LogNotice, "Configure: %04x:%04x nEP=%u",
                          (unsigned)d->idVendor, (unsigned)d->idProduct,
                          (unsigned)GetNumEndpoints());

    const bool accessory = d->idVendor == aoa::kAccessoryVid &&
                           d->idProduct >= aoa::kAccessoryPidLo &&
                           d->idProduct <= aoa::kAccessoryPidHi;
    if (accessory) {
        return configureAccessory() ? TRUE : FALSE;
    }

    // Pre-switch dongle (Android PTP): drive the AOA mode-switch and return FALSE —
    // the hub then deletes this transient device immediately, so when the dongle
    // re-enumerates in accessory mode on the same port the hub sees a free slot and
    // enumerates it promptly (returning TRUE instead makes the hub hold this device
    // and miss the fast AOA disconnect entirely). Returning FALSE requires the hub
    // to clear its per-port "configured" flag on teardown — see
    // scripts/patches/usbstandardhub-reconfigure.patch — otherwise the accessory
    // device is enumerated but Configure() is skipped.
    CLogger::Get()->Write(From, LogNotice, "dongle %04x:%04x — starting AOA switch",
                          (unsigned)d->idVendor, (unsigned)d->idProduct);
    aoaStartAccessory(GetDevice());
    return FALSE;
}

bool CAndroidAutoDevice::configureAccessory() {
    CLogger::Get()->Write(From, LogNotice, "configureAccessory: iface=%u nEP=%u",
                          (unsigned)GetInterfaceNumber(), (unsigned)GetNumEndpoints());
    const TUSBEndpointDescriptor* ep;
    while ((ep = (const TUSBEndpointDescriptor*)GetDescriptor(DESCRIPTOR_ENDPOINT)) != nullptr) {
        CLogger::Get()->Write(From, LogNotice, "  acc ep addr=%02x attr=%02x mps=%u",
                              (unsigned)ep->bEndpointAddress, (unsigned)ep->bmAttributes,
                              (unsigned)ep->wMaxPacketSize);
        if ((ep->bmAttributes & 0x3F) != 0x02) continue;  // bulk only
        if (ep->bEndpointAddress & 0x80) {
            if (m_pBulkIn == nullptr) m_pBulkIn = new CUSBEndpoint(GetDevice(), ep);
        } else {
            if (m_pBulkOut == nullptr) m_pBulkOut = new CUSBEndpoint(GetDevice(), ep);
        }
    }
    if (m_pBulkIn == nullptr || m_pBulkOut == nullptr) {
        CLogger::Get()->Write(From, LogError, "accessory device missing bulk endpoints (in=%p out=%p)",
                              (void*)m_pBulkIn, (void*)m_pBulkOut);
        return false;
    }
    if (!CUSBFunction::Configure()) {
        CLogger::Get()->Write(From, LogError, "cannot set accessory interface");
        return false;
    }
    // Buffers are heap memory: xHCI DMA asserts (pBuffer > MEM_KERNEL_END).
    m_pOutBuffer = new u8[kOutChunk];
    if (m_pOutBuffer == nullptr) return false;
    for (unsigned i = 0; i < kInBuffers; ++i) {
        m_pInBuffer[i] = new u8[kInTransfer];
        if (m_pInBuffer[i] == nullptr) return false;
        m_pInURB[i] = new CUSBRequest(m_pBulkIn, m_pInBuffer[i], kInTransfer);
        if (m_pInURB[i] == nullptr) return false;
        m_pInURB[i]->SetCompletionRoutine(inCompletionStub, nullptr, this);
    }

    // Post all bulk-IN buffers up front (double-buffered) so the dongle always has a
    // posted buffer to stream into during the channel-open/AV-setup burst. Only then
    // publish "uaa1", so the driver never binds a half-started transport.
    for (unsigned i = 0; i < kInBuffers; ++i) {
        if (!submitIn(m_pInURB[i])) {
            m_link.onInRearmFailed();
            CLogger::Get()->Write(From, LogError, "cannot start bulk-IN polling");
            return false;
        }
    }
    m_nDeviceNumber = s_NumberPool.AllocateNumber(TRUE, From);
    CDeviceNameService::Get()->AddDevice(DevicePrefix, m_nDeviceNumber, this, FALSE);
    CLogger::Get()->Write(From, LogNotice, "accessory bulk endpoints up — AA transport ready");
    return true;
}

// Task context (configureAccessory) or USB-IRQ context (inCompletion re-arm). The
// in-flight count goes up BEFORE the submit so a completion racing the submit can't
// drive it below the real number of posted requests.
bool CAndroidAutoDevice::submitIn(CUSBRequest* pURB) {
    m_inFlight.fetch_add(1, std::memory_order_relaxed);
    if (!GetHost()->SubmitAsyncRequest(pURB)) {
        m_inFlight.fetch_sub(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

// USB-IRQ context: NO logging (logging to the USB-C serial gadget from here
// deadlocks the system), NO allocation, NO blocking. Enqueue the received bytes into
// the SPSC ring (read() drains it from task context), then re-post this same
// request — unless the link is dead, in which case the request is parked for good.
void CAndroidAutoDevice::inCompletion(CUSBRequest* pURB) {
    m_inFlight.fetch_sub(1, std::memory_order_relaxed);  // this request is back
    // Circle marks success with a non-zero status. A failed request has no valid
    // length: on the xHCI path TransferEvent never calls SetStatus/SetResultLen for
    // an error, and GetResultLength() is assert(m_bStatus) — calling it here was the
    // H8 hard hang (the assert fires inside the xHCI ISR). Review A1.
    const bool ok = pURB->GetStatus() != 0;
    const unsigned n = ok ? pURB->GetResultLength() : 0;
    // An error latches the link dead (the xHC has halted the endpoint), and so does a
    // transfer that doesn't fit the ring. Either way: never re-post (review A3).
    if (!m_link.onInComplete(ok, static_cast<const u8*>(pURB->GetBuffer()), n)) return;
    pURB->SetStatus(0);  // reuse the request: back to the freshly-constructed state
    pURB->SetResultLen(0);
    if (!submitIn(pURB)) m_link.onInRearmFailed();
}

void CAndroidAutoDevice::inCompletionStub(CUSBRequest* pURB, void* /*pParam*/, void* pContext) {
    static_cast<CAndroidAutoDevice*>(pContext)->inCompletion(pURB);
}

int CAndroidAutoDevice::read(uint8_t* buf, size_t len, uint32_t /*timeoutMs*/) {
    if (m_pBulkIn == nullptr) return -1;
    return m_link.read(buf, len);  // 0 when empty -> caller treats as timeout; -1 dead
}

// Task context. Each chunk is copied into the transport-owned bounce buffer and sent
// with a blocking bulk-OUT. Circle's xHCI Transfer() ignores the caller's timeout and
// gives up after a hard-coded 3 s, leaving the TRB live on the ring; the next submit
// would then be credited with that stale TRB's completion and return before its own
// data went out (review A7). So ANY failure is fatal: the link latches dead and no
// further OUT request is ever submitted on this endpoint (write() returns -1 at once,
// so the UI loop also can't stall 3 s per write on a stuck pipe).
int CAndroidAutoDevice::write(const uint8_t* data, size_t len, uint32_t /*timeoutMs*/) {
    if (m_pBulkOut == nullptr || m_pOutBuffer == nullptr || !m_link.writable()) return -1;
    if (len > 0 && data == nullptr) return -1;
    static_assert(kOutChunk % 512 == 0, "OUT chunks must not end in a short packet");
    static_assert(kOutChunk >= carsh::aa::kUsbTransferSize, "one AA frame per chunk");
    CUSBHostController* host = GetHost();
    size_t off = 0;
    while (off < len) {
        const unsigned chunk = static_cast<unsigned>(len - off < kOutChunk ? len - off : kOutChunk);
        std::memcpy(m_pOutBuffer, data + off, chunk);
        // Own the URB (instead of host->Transfer) — same submit shape + default
        // timeout (USB_TIMEOUT_NONE) as CUSBHostController::Transfer.
        CUSBRequest urb(m_pBulkOut, m_pOutBuffer, chunk);
        const unsigned t0 = CTimer::GetClockTicks();
        if (!host->SubmitBlockingRequest(&urb)) {
            m_lastOutBlockedMs = (CTimer::GetClockTicks() - t0) / 1000;
            m_link.onOutFailed();
            return -1;
        }
        const unsigned r = urb.GetResultLength();  // status is set: safe to read
        if (r == 0 || r > chunk) {
            m_link.onOutStalled();
            return -1;
        }
        m_link.onOutComplete(r);
        off += r;  // a short write re-sends the remainder from data + off
    }
    return static_cast<int>(off);
}

// Factory hook (extern "C") — match by vendor name string ("venXXXX-YYYY").
extern "C" CUSBFunction* carshCreateAndroidAutoDevice(CUSBFunction* pParent, const char* pName) {
    if (pParent == nullptr || pName == nullptr) return nullptr;

    // Diagnostic: log every factory call for our two vendor ids (substring match on
    // "18d1"/"2717") so we see exactly what name strings Circle passes for the
    // dongle in both phases, even when the exact PID match below misses.
    auto contains = [](const char* hay, const char* needle) {
        for (; *hay; ++hay) {
            const char* a = hay;
            const char* b = needle;
            while (*a && *b && *a == *b) { ++a; ++b; }
            if (*b == '\0') return true;
        }
        return false;
    };
    if (contains(pName, "18d1") || contains(pName, "2717")) {
        CLogger::Get()->Write(From, LogNotice, "factory: called for %s", pName);
    }

    // Pre-switch dongle id, or any AOA accessory id (0x18d1:0x2d00..0x2d05).
    static const char* const kMatch[] = {
        "ven2717-ff40",                                              // CA525 (Android MINIX)
        "ven18d1-2d00", "ven18d1-2d01", "ven18d1-2d02",              // accessory
        "ven18d1-2d03", "ven18d1-2d04", "ven18d1-2d05",
    };
    for (const char* m : kMatch) {
        const char* a = pName;
        const char* b = m;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') {
            CLogger::Get()->Write(From, LogNotice, "factory: AOA match %s", pName);
            return new CAndroidAutoDevice(pParent);
        }
    }
    return nullptr;
}

}  // namespace carsh::device
