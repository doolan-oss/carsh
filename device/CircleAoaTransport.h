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
#ifndef _carsh_circle_aoa_transport_h
#define _carsh_circle_aoa_transport_h

#include <circle/usb/usbendpoint.h>
#include <circle/usb/usbfunction.h>
#include <circle/usb/usbrequest.h>
#include <circle/types.h>

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
    unsigned rxBytes() const { return m_rxBytes; }  // Rung-3 throughput counter
    unsigned txBytes() const { return m_txBytes; }  // bulk-OUT bytes sent (TLS bring-up)
    // Last bulk-IN TUSBError + a monotonically-rising counter, set from IRQ context on
    // a transfer error and logged by the driver from task context (see inCompletion).
    unsigned lastInUsbErr() const { return m_lastInUsbErr; }
    unsigned inErrSeq() const { return m_inErrSeq; }
    // Bulk-IN diagnostics (xHCI bulk-reset hunt): the result length reported on the
    // last IN error (a length > kInTransfer points at a babble/over-run), and how many
    // bulk-IN URBs are still posted. inFlight()==0 means the IN pipe is fully dead and
    // the head unit is deaf — the driver re-posts (restartInPolling) and logs it.
    unsigned lastInResultLen() const { return m_lastInResultLen; }
    unsigned inFlight() const { return m_inFlight; }
    unsigned ringUsed() const { return (m_ringTail - m_ringHead) & kRingMask; }
    // Last bulk-OUT TUSBError + counter (write() runs in task context, so it captures
    // the error directly from an owned URB — unlike the IRQ-context IN path).
    unsigned lastOutUsbErr() const { return m_lastOutUsbErr; }
    unsigned outErrSeq() const { return m_outErrSeq; }

    // Re-post the bulk-IN URBs after the pipe stalled. ONLY safe to call from task
    // context when inFlight()==0 (no completion routine is pending, so the buffers are
    // free of any in-flight URB). Returns true if at least one URB was posted.
    bool restartInPolling();

    // IAaTransport.
    int read(uint8_t* buf, size_t len, uint32_t timeoutMs) override;
    int write(const uint8_t* data, size_t len, uint32_t timeoutMs) override;

private:
    bool configureAccessory();
    bool startInRequest(u8* buf);  // submit one async bulk-IN using the given buffer
    void inCompletion(CUSBRequest* pURB);
    static void inCompletionStub(CUSBRequest* pURB, void* pParam, void* pContext);

    static constexpr unsigned kRingSize = 64 * 1024;
    static constexpr unsigned kRingMask = kRingSize - 1;
    static constexpr unsigned kInTransfer = 16384;
    // Keep multiple bulk-IN URBs posted so a posted buffer is always available during
    // the phone's channel-open/AV-setup burst (a single-buffer re-arm leaves gaps that
    // make the dongle reset). xHCI allows 2 outstanding URBs per endpoint.
    static constexpr unsigned kInBuffers = 2;

    CUSBEndpoint* m_pBulkIn;
    CUSBEndpoint* m_pBulkOut;
    u8* m_pInBuffer[kInBuffers];
    unsigned m_nDeviceNumber;  // "uaaN" registration; 0 = not registered
    u8 m_ring[kRingSize];
    volatile unsigned m_ringHead;
    volatile unsigned m_ringTail;
    volatile unsigned m_rxBytes;
    volatile unsigned m_txBytes;
    volatile unsigned m_lastInUsbErr;
    volatile unsigned m_inErrSeq;
    volatile unsigned m_lastInResultLen;  // result length at the last bulk-IN error
    volatile unsigned m_inFlight;         // bulk-IN URBs currently posted
    volatile unsigned m_lastOutUsbErr;    // TUSBError of the last bulk-OUT failure
    volatile unsigned m_outErrSeq;        // bulk-OUT failure counter
};

// Factory hook — called from the patched CUSBDeviceFactory (see
// scripts/patches/usbdevicefactory-aoa.patch). Returns a new CAndroidAutoDevice if
// pName is the CA525 pre-switch vendor id or an AOA accessory vendor id, else null.
// extern "C" so the circle-stdlib factory can call it without a C++ name-mangling
// or header dependency on carsh.
extern "C" CUSBFunction* carshCreateAndroidAutoDevice(CUSBFunction* pParent, const char* pName);

// AOA mode-switch on a pre-switch device (reqs 51/52/53 + AA identity strings).
bool aoaStartAccessory(CUSBDevice* pDevice);
// Rung-1 first-light: log VID/PID/class + endpoints to the serial.
void aoaDumpDeviceDescriptor(CUSBDevice* pDevice);

}  // namespace carsh::device

#endif  // _carsh_circle_aoa_transport_h
