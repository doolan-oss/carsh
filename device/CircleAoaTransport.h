// CircleAoaTransport — the device-side Android Auto USB driver + transport for the
// wireless-AA dongle (MINIX CA525). The dongle first enumerates as an Android PTP
// device (VID 0x2717 PID 0xff40, "Android MINIX"); Circle binds no driver to it, so
// we register one via the factory hook below. Its Configure() drives the Android
// Open Accessory mode-switch (reqs 51/52/53), the dongle re-enumerates in accessory
// mode (Google VID 0x18d1, PID 0x2d00-0x2d05), and a second Configure() binds the
// two bulk endpoints that carry the AA byte stream. CAndroidAutoDevice IS the
// IAaTransport the platform-independent AA core (AaMessenger/AaConnection) talks to.
//
// Bulk plumbing mirrors the proven CUSBBluetoothDevice ACL path
// (scripts/patches/usbbluetooth-acl.patch) + the CircleHciTransport IRQ trampoline:
// blocking bulk OUT, async bulk IN whose completion (USB-IRQ context) only enqueues
// into a lock-free SPSC byte ring that read() drains from task context. AA_PLAN
// rungs 2-3.
//
// Link faults are FATAL for a transport instance (review A1/A3/A7): the first failed
// bulk-IN transfer, failed re-post, RX overflow or failed/timed-out bulk-OUT latches
// the link dead (carsh::aa::AaBulkLink) — no more re-posts, read()/write() refuse —
// and CircleAaDriver ends the session and, if the dongle doesn't re-cycle by itself,
// forces a USB re-enumeration (aoaForceReenumerate below). Why not recover in place:
// see the comment above aoaForceReenumerate in the .cpp.
#ifndef _carsh_circle_aoa_transport_h
#define _carsh_circle_aoa_transport_h

#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbfunction.h>
#include <circle/usb/usbrequest.h>
#include <circle/types.h>

#include <atomic>

#include "aa/AaBulkLink.h"
#include "aa/IAaTransport.h"

namespace carsh::device {

// Android Open Accessory control protocol (control transfers to EP0).
namespace aoa {
constexpr u8 kReqGetProtocol = 51;
constexpr u8 kReqSendString = 52;
constexpr u8 kReqStartAccessory = 53;
constexpr u16 kStrManufacturer = 0;
constexpr u16 kStrModel = 1;
constexpr u16 kStrDescription = 2;
constexpr u16 kStrVersion = 3;
constexpr u16 kStrUri = 4;
constexpr u16 kStrSerial = 5;
// The CA525 dongle's pre-switch identity (Android PTP, "Android MINIX").
constexpr u16 kDongleVid = 0x2717;
constexpr u16 kDonglePid = 0xFF40;
// Accessory-mode id range the dongle re-enumerates as.
constexpr u16 kAccessoryVid = 0x18D1;
constexpr u16 kAccessoryPidLo = 0x2D00;
constexpr u16 kAccessoryPidHi = 0x2D05;
}  // namespace aoa

class CAndroidAutoDevice : public CUSBFunction, public carsh::aa::IAaTransport {
public:
    explicit CAndroidAutoDevice(CUSBFunction* pFunction);
    ~CAndroidAutoDevice() override;

    // CUSBFunction: pre-switch device -> drive AOA (returns FALSE, device
    // re-enumerates); accessory-mode device -> bind bulk endpoints + start IN
    // polling + register as "uaa1" (returns TRUE).
    boolean Configure(void) override;

    bool isAccessory() const { return m_pBulkIn != nullptr && m_pBulkOut != nullptr; }

    // Link health (any context). Once linkDead(), this instance never carries AA
    // traffic again: read()/write() return -1 and no bulk-IN request is re-posted.
    bool linkDead() const { return m_link.dead(); }
    carsh::aa::AaLinkFault linkFault() const { return m_link.fault(); }

    // Diagnostics for the driver's log. Circle's xHCI path records neither a USB
    // error code nor a length for a failed transfer (the controller's completion code
    // is only in Circle's own "xhciep: Transfer error N on endpoint M" log line), so
    // there is deliberately no per-error TUSBError/length here any more.
    unsigned rxBytes() const { return m_link.rxBytes(); }  // bulk-IN bytes accepted
    unsigned txBytes() const { return m_link.txBytes(); }  // bulk-OUT bytes sent
    unsigned ringUsed() const { return static_cast<unsigned>(m_link.ringUsed()); }
    // Bulk-IN requests still posted. After an IN error on a halted xHCI endpoint this
    // stays at 1 for good (the second request never completes) — expected, harmless.
    unsigned inFlight() const { return m_inFlight.load(std::memory_order_relaxed); }
    // How long the failing bulk-OUT write blocked: ~3000 ms = Circle's hard-coded
    // xHCI Transfer() timeout (the dongle NAKed); less = a transfer error.
    unsigned lastOutBlockedMs() const { return m_lastOutBlockedMs; }

    // IAaTransport.
    int read(uint8_t* buf, size_t len, uint32_t timeoutMs) override;
    int write(const uint8_t* data, size_t len, uint32_t timeoutMs) override;

private:
    bool configureAccessory();
    bool submitIn(CUSBRequest* pURB);  // post one async bulk-IN request
    void inCompletion(CUSBRequest* pURB);
    static void inCompletionStub(CUSBRequest* pURB, void* pParam, void* pContext);

    static constexpr unsigned kRingSize = 256 * 1024;  // RX byte ring (power of two)
    // Each bulk-IN URB buffer holds a WHOLE video keyframe (~64 KB seen on the bench).
    // Circle's xHCI endpoint allows only 2 outstanding URBs (m_pURB[0]/[1]); on the PC
    // harness we kept 8 small ones in flight and it never glitched. With only 2 slots,
    // 16 KB each was too little — a bursty keyframe outran the buffering, corrupting the
    // stream (a misaligned plaintext frame appeared mid-run). Bigger buffers give the
    // dongle far more headroom between our completion+re-arm cycles.
    static constexpr unsigned kInTransfer = 64 * 1024;
    // Keep multiple bulk-IN URBs posted so a posted buffer is always available during
    // the phone's channel-open/AV-setup burst (a single-buffer re-arm leaves gaps that
    // make the dongle reset). xHCI allows 2 outstanding URBs per endpoint.
    static constexpr unsigned kInBuffers = 2;
    // Bulk-OUT bounce buffer: one AA frame (kUsbTransferSize) — larger writes go out
    // in chunks of this size. A multiple of every bulk max-packet size (64/512), so a
    // chunk boundary never ends a transfer with a short packet mid-frame.
    static constexpr unsigned kOutChunk = 16 * 1024;

    CUSBEndpoint* m_pBulkIn;
    CUSBEndpoint* m_pBulkOut;
    u8* m_pInBuffer[kInBuffers];
    // One reusable request per IN buffer, created in task context at configure time:
    // the IRQ re-arm only resets + resubmits it, so it allocates nothing, and a request
    // stranded on a halted endpoint is freed with the device (not leaked from Circle's
    // fixed CUSBRequest pool on every dongle cycle).
    CUSBRequest* m_pInURB[kInBuffers];
    // Transport-owned OUT buffer: a timed-out bulk-OUT TRB stays live on the xHCI ring
    // and may still DMA from its buffer later, so it must not be the caller's (soon
    // freed) frame. This one lives until the device object dies, which Circle does
    // only after the xHCI slot is disabled (~CXHCIUSBDevice runs before ~CUSBDevice
    // deletes the functions).
    u8* m_pOutBuffer;
    unsigned m_nDeviceNumber;  // "uaaN" registration; 0 = not registered
    carsh::aa::AaBulkLink<kRingSize> m_link;  // RX ring + sticky dead latch
    std::atomic<unsigned> m_inFlight;         // bulk-IN requests currently posted
    unsigned m_lastOutBlockedMs;
};

// Factory hook — called from the patched CUSBDeviceFactory (see
// scripts/patches/usbdevicefactory-aoa.patch). Returns a new CAndroidAutoDevice if
// pName is the CA525 pre-switch vendor id or an AOA accessory vendor id, else null.
// extern "C" so the circle-stdlib factory can call it without a C++ name-mangling
// or header dependency on carsh.
extern "C" CUSBFunction* carshCreateAndroidAutoDevice(CUSBFunction* pParent, const char* pName);

// AOA mode-switch on a pre-switch device (reqs 51/52/53 + AA identity strings).
bool aoaStartAccessory(CUSBDevice* pDevice);

// Recovery for a dead link: make the dongle drop off the bus and re-enumerate, by
// removing its USB device (on a hub port: CLEAR_FEATURE(PORT_ENABLE) + delete) and
// rescanning the bus (SET_FEATURE(PORT_RESET) on the still-connected port, then the
// normal enumerate -> factory -> AOA path). TASK context only — the UI loop, the same
// task that runs UpdatePlugAndPlay(). On success pDongle has been DELETED (its
// removed-handlers ran) before this returns; the caller must drop every pointer to it
// first. Blocks for the port reset + enumeration (a few hundred ms). Returns false if
// Circle could not remove the device (it then still exists, link still dead).
bool aoaForceReenumerate(CAndroidAutoDevice* pDongle);
// Rung-1 first-light: log VID/PID/class + endpoints to the serial.
void aoaDumpDeviceDescriptor(CUSBDevice* pDevice);

}  // namespace carsh::device

#endif  // _carsh_circle_aoa_transport_h
