// CircleAoaTransport.cpp — see CircleAoaTransport.h. Driver shape + bulk plumbing
// mirror CUSBBluetoothDevice (the validated repo USB pattern); the IRQ->ring->task
// hand-off mirrors CircleHciTransport.
#include "CircleAoaTransport.h"

#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/numberpool.h>
#include <circle/synchronize.h>  // DataSyncBarrier / DataMemBarrier
#include <circle/usb/usb.h>      // REQUEST_*, DESCRIPTOR_ENDPOINT, descriptors
#include <circle/usb/usbdevice.h>
#include <circle/usb/usbhostcontroller.h>

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

CAndroidAutoDevice::CAndroidAutoDevice(CUSBFunction* pFunction)
    : CUSBFunction(pFunction),
      m_pBulkIn(nullptr),
      m_pBulkOut(nullptr),
      m_pInBuffer(nullptr),
      m_ringHead(0),
      m_ringTail(0),
      m_rxBytes(0) {}

CAndroidAutoDevice::~CAndroidAutoDevice() {
    delete m_pBulkIn;
    delete m_pBulkOut;
    delete[] m_pInBuffer;
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
    m_pInBuffer = new u8[kInTransfer];
    if (m_pInBuffer == nullptr) return false;

    unsigned n = s_NumberPool.AllocateNumber(TRUE, From);
    CDeviceNameService::Get()->AddDevice(DevicePrefix, n, this, FALSE);

    if (!startInRequest()) {
        CLogger::Get()->Write(From, LogError, "cannot start bulk-IN polling");
        return false;
    }
    CLogger::Get()->Write(From, LogNotice, "accessory bulk endpoints up — AA transport ready");
    return true;
}

bool CAndroidAutoDevice::startInRequest() {
    CUSBRequest* pURB = new CUSBRequest(m_pBulkIn, m_pInBuffer, kInTransfer);
    if (pURB == nullptr) return false;
    pURB->SetCompletionRoutine(inCompletionStub, nullptr, this);
    return GetHost()->SubmitAsyncRequest(pURB);
}

// USB-IRQ context. Enqueue received bytes into the SPSC ring, then re-arm. read()
// drains the ring from task context — no AA-core work happens in the IRQ.
void CAndroidAutoDevice::inCompletion(CUSBRequest* pURB) {
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
            DataSyncBarrier();
            m_ringTail = tail;
            m_rxBytes += n;
        }
    } else {
        restart = false;  // transfer error (likely disconnect)
    }
    delete pURB;
    if (restart && !startInRequest()) {
        CLogger::Get()->Write(From, LogError, "cannot restart bulk-IN request");
    }
}

void CAndroidAutoDevice::inCompletionStub(CUSBRequest* pURB, void* /*pParam*/, void* pContext) {
    static_cast<CAndroidAutoDevice*>(pContext)->inCompletion(pURB);
}

int CAndroidAutoDevice::read(uint8_t* buf, size_t len, uint32_t /*timeoutMs*/) {
    if (m_pBulkIn == nullptr) return -1;
    unsigned head = m_ringHead;
    DataMemBarrier();
    size_t n = 0;
    while (n < len && head != m_ringTail) {
        buf[n++] = m_ring[head];
        head = (head + 1) & kRingMask;
    }
    m_ringHead = head;
    return static_cast<int>(n);  // 0 when empty -> caller treats as timeout
}

int CAndroidAutoDevice::write(const uint8_t* data, size_t len, uint32_t /*timeoutMs*/) {
    if (m_pBulkOut == nullptr) return -1;
    CUSBHostController* host = GetHost();
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
