// CircleAoaTransport.cpp — see CircleAoaTransport.h. The bulk transfer plumbing
// deliberately mirrors the proven CUSBBluetoothDevice ACL path
// (scripts/patches/usbbluetooth-acl.patch) and the CircleHciTransport IRQ->queue->
// drain trampoline: those are the two USB patterns already validated on this board.
#include "CircleAoaTransport.h"

#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/synchronize.h>  // DataSyncBarrier / DataMemBarrier
#include <circle/usb/usb.h>      // REQUEST_*, DESCRIPTOR_ENDPOINT, descriptors
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbhostcontroller.h>
#include <circle/usb/usbrequest.h>

namespace carsh::device {
namespace {

const char From[] = "aoa";

// One bulk-IN transfer's landing buffer. Big enough to coalesce several AA frames
// per transfer (matches the AA core's kUsbTransferSize) so throughput isn't
// per-frame-bound on the bench.
constexpr unsigned kInTransfer = 16384;

// Send one AOA identity string (control req 52) to EP0. The string is sent
// NUL-terminated, as the AOA spec expects.
int aoaSendString(CUSBDevice* pDevice, u16 index, const char* str) {
    unsigned len = 0;
    while (str[len] != '\0') ++len;
    ++len;  // include the terminating NUL
    return pDevice->GetHost()->ControlMessage(
        pDevice->GetEndpoint0(), REQUEST_OUT | REQUEST_VENDOR | REQUEST_TO_DEVICE,
        aoa::kReqSendString, 0, index, const_cast<char*>(str), static_cast<u16>(len));
}

}  // namespace

void aoaDumpDeviceDescriptor(CUSBDevice* pDevice) {
    if (pDevice == nullptr) return;
    const TUSBDeviceDescriptor* d = pDevice->GetDeviceDescriptor();
    if (d == nullptr) {
        CLogger::Get()->Write(From, LogWarning, "no device descriptor");
        return;
    }
    CLogger::Get()->Write(From, LogNotice,
                          "dongle VID %04x PID %04x class %u/%u/%u cfgs %u",
                          (unsigned)d->idVendor, (unsigned)d->idProduct,
                          (unsigned)d->bDeviceClass, (unsigned)d->bDeviceSubClass,
                          (unsigned)d->bDeviceProtocol, (unsigned)d->bNumConfigurations);
    const TUSBEndpointDescriptor* ep;
    while ((ep = (const TUSBEndpointDescriptor*)pDevice->GetDescriptor(DESCRIPTOR_ENDPOINT)) !=
           nullptr) {
        CLogger::Get()->Write(From, LogNotice, "  ep addr %02x attr %02x mps %u",
                              (unsigned)ep->bEndpointAddress, (unsigned)ep->bmAttributes,
                              (unsigned)ep->wMaxPacketSize);
    }
}

bool aoaStartAccessory(CUSBDevice* pDevice) {
    if (pDevice == nullptr) return false;
    CUSBHostController* host = pDevice->GetHost();
    CUSBEndpoint* ep0 = pDevice->GetEndpoint0();

    // Req 51: probe the AOA protocol version.
    u8 proto[2] = {0, 0};
    int r = host->ControlMessage(ep0, REQUEST_IN | REQUEST_VENDOR | REQUEST_TO_DEVICE,
                                 aoa::kReqGetProtocol, 0, 0, proto, sizeof proto);
    if (r < 2) {
        CLogger::Get()->Write(From, LogWarning, "device did not answer AOA probe (%d)", r);
        return false;
    }
    const unsigned version = (unsigned)proto[0] | ((unsigned)proto[1] << 8);
    if (version < 1) {
        CLogger::Get()->Write(From, LogWarning, "device reports no AOA support");
        return false;
    }
    CLogger::Get()->Write(From, LogNotice, "AOA v%u — sending AA identity", version);

    // Req 52 x6: identity strings. manufacturer/model are the pair Google's Android
    // Auto allowlist matches to launch projection; the rest are informational.
    aoaSendString(pDevice, aoa::kStrManufacturer, "Android");
    aoaSendString(pDevice, aoa::kStrModel, "Android Auto");
    aoaSendString(pDevice, aoa::kStrDescription, "carsh head unit");
    aoaSendString(pDevice, aoa::kStrVersion, "2.0.1");
    aoaSendString(pDevice, aoa::kStrUri, "https://github.com/dylant9/carsh");
    aoaSendString(pDevice, aoa::kStrSerial, "carsh-0001");

    // Req 53: switch to accessory mode (the device re-enumerates as 0x18d1:0x2d0x).
    r = host->ControlMessage(ep0, REQUEST_OUT | REQUEST_VENDOR | REQUEST_TO_DEVICE,
                             aoa::kReqStartAccessory, 0, 0, nullptr, 0);
    if (r < 0) {
        CLogger::Get()->Write(From, LogError, "AOA start-accessory failed (%d)", r);
        return false;
    }
    CLogger::Get()->Write(From, LogNotice, "AOA start sent — awaiting re-enumeration");
    return true;
}

CircleAoaTransport::CircleAoaTransport()
    : m_pDevice(nullptr),
      m_pBulkIn(nullptr),
      m_pBulkOut(nullptr),
      m_pInBuffer(nullptr),
      m_ringHead(0),
      m_ringTail(0),
      m_rxBytes(0) {}

CircleAoaTransport::~CircleAoaTransport() {
    delete m_pBulkIn;
    delete m_pBulkOut;
    delete[] m_pInBuffer;
}

CircleAoaTransport::OpenResult CircleAoaTransport::open(CUSBDevice* pDevice) {
    if (pDevice == nullptr) return AoaError;
    aoaDumpDeviceDescriptor(pDevice);  // Rung 1

    const TUSBDeviceDescriptor* d = pDevice->GetDeviceDescriptor();
    if (d == nullptr) return AoaError;

    const bool accessoryMode = d->idVendor == aoa::kAccessoryVid &&
                               d->idProduct >= aoa::kAccessoryPidLo &&
                               d->idProduct <= aoa::kAccessoryPidHi;
    if (!accessoryMode) {
        // Rung 2: drive the mode-switch; caller re-opens with the new device.
        return aoaStartAccessory(pDevice) ? AoaSwitched : AoaError;
    }

    if (!bindAccessoryEndpoints(pDevice)) return AoaError;
    m_pDevice = pDevice;
    if (!startInRequest()) return AoaError;  // Rung 3: bulk-IN polling begins
    CLogger::Get()->Write(From, LogNotice, "accessory bulk endpoints up — AA ready");
    return AoaReady;
}

bool CircleAoaTransport::bindAccessoryEndpoints(CUSBDevice* pDevice) {
    const TUSBEndpointDescriptor* ep;
    while ((ep = (const TUSBEndpointDescriptor*)pDevice->GetDescriptor(DESCRIPTOR_ENDPOINT)) !=
           nullptr) {
        if ((ep->bmAttributes & 0x03) != 0x02) continue;  // bulk only
        if (ep->bEndpointAddress & 0x80) {
            if (m_pBulkIn == nullptr) m_pBulkIn = new CUSBEndpoint(pDevice, ep);
        } else {
            if (m_pBulkOut == nullptr) m_pBulkOut = new CUSBEndpoint(pDevice, ep);
        }
    }
    if (m_pBulkIn == nullptr || m_pBulkOut == nullptr) {
        CLogger::Get()->Write(From, LogError, "accessory device missing bulk endpoints");
        return false;
    }
    m_pInBuffer = new u8[kInTransfer];
    return m_pInBuffer != nullptr;
}

bool CircleAoaTransport::startInRequest() {
    if (m_pBulkIn == nullptr || m_pInBuffer == nullptr || m_pDevice == nullptr) return false;
    CUSBRequest* pURB = new CUSBRequest(m_pBulkIn, m_pInBuffer, kInTransfer);
    if (pURB == nullptr) return false;
    pURB->SetCompletionRoutine(inCompletionStub, nullptr, this);
    return m_pDevice->GetHost()->SubmitAsyncRequest(pURB);
}

// USB-IRQ context. Enqueue received bytes into the SPSC ring, then re-arm. We do
// NOT touch the AA core here — read() drains the ring from task context, exactly
// like CircleHciTransport keeps ACL delivery out of the USB IRQ.
void CircleAoaTransport::inCompletion(CUSBRequest* pURB) {
    bool restart = true;
    if (pURB->GetStatus() != 0) {  // non-zero status = success, per Circle
        unsigned n = pURB->GetResultLength();
        if (n > 0) {
            unsigned tail = m_ringTail;
            for (unsigned i = 0; i < n; ++i) {
                unsigned next = (tail + 1) & kRingMask;
                if (next == m_ringHead) break;  // ring full — drop the rest
                m_ring[tail] = m_pInBuffer[i];
                tail = next;
            }
            DataSyncBarrier();  // bytes visible before the tail advances
            m_ringTail = tail;
            m_rxBytes += n;  // throughput counter (counts even dropped bytes)
        }
    } else {
        restart = false;  // transfer error (likely disconnect) — stop polling
    }
    delete pURB;
    if (restart && !startInRequest()) {
        CLogger::Get()->Write(From, LogError, "cannot restart bulk-IN request");
    }
}

void CircleAoaTransport::inCompletionStub(CUSBRequest* pURB, void* /*pParam*/, void* pContext) {
    static_cast<CircleAoaTransport*>(pContext)->inCompletion(pURB);
}

int CircleAoaTransport::read(uint8_t* buf, size_t len, uint32_t /*timeoutMs*/) {
    if (m_pBulkIn == nullptr) return -1;
    unsigned head = m_ringHead;
    DataMemBarrier();  // observe producer's tail/bytes
    size_t n = 0;
    while (n < len && head != m_ringTail) {
        buf[n++] = m_ring[head];
        head = (head + 1) & kRingMask;
    }
    m_ringHead = head;
    return static_cast<int>(n);  // 0 when empty -> the AA poll loop treats as timeout
}

int CircleAoaTransport::write(const uint8_t* data, size_t len, uint32_t /*timeoutMs*/) {
    if (m_pBulkOut == nullptr || m_pDevice == nullptr) return -1;
    CUSBHostController* host = m_pDevice->GetHost();
    size_t off = 0;
    while (off < len) {
        int r = host->Transfer(m_pBulkOut, const_cast<uint8_t*>(data + off),
                               static_cast<unsigned>(len - off));
        if (r < 0) return -1;
        if (r == 0) break;
        off += static_cast<size_t>(r);
    }
    return static_cast<int>(off);
}

}  // namespace carsh::device
