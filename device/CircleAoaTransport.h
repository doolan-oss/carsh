// CircleAoaTransport — the device-side IAaTransport for Android Auto: a Circle USB
// host driver for the wireless-AA dongle (MINIX CA525). It (1) runs the Android
// Open Accessory mode-switch on a freshly-enumerated dongle, then (2) carries the
// AA byte stream over the dongle's bulk endpoints once it re-enumerates in
// accessory mode (Google VID 0x18d1, PID 0x2d00-0x2d05). read()/write() are the
// IAaTransport seam the platform-independent AA core (AaMessenger/AaConnection)
// talks to; everything above is already host-tested.
//
// Bulk plumbing mirrors the proven CUSBBluetoothDevice ACL path
// (scripts/patches/usbbluetooth-acl.patch): blocking bulk OUT, and async bulk IN
// whose completion runs in USB-IRQ context and only enqueues into a lock-free SPSC
// byte ring — read() drains it from task context, exactly like CircleHciTransport
// does for HCI ACL. This is AA_PLAN rungs 1-3 (enumerate -> AOA switch -> bulk
// throughput); the device-acquisition details are the first bench tasks, since
// they depend on how the CA525 actually enumerates (Rung 1 reveals that).
#ifndef _carsh_circle_aoa_transport_h
#define _carsh_circle_aoa_transport_h

#include <circle/types.h>

#include "aa/IAaTransport.h"

class CUSBDevice;
class CUSBEndpoint;
class CUSBRequest;

namespace carsh::device {

// Android Open Accessory control protocol (control transfers to EP0).
namespace aoa {
constexpr u8 kReqGetProtocol = 51;  // IN:  returns the AOA protocol version
constexpr u8 kReqSendString = 52;   // OUT: one identity string (wIndex = which)
constexpr u8 kReqStartAccessory = 53;  // OUT: switch to accessory mode + re-enumerate
// Identity-string indices (wIndex for req 52).
constexpr u16 kStrManufacturer = 0;
constexpr u16 kStrModel = 1;
constexpr u16 kStrDescription = 2;
constexpr u16 kStrVersion = 3;
constexpr u16 kStrUri = 4;
constexpr u16 kStrSerial = 5;
// Accessory-mode device id the dongle re-enumerates as.
constexpr u16 kAccessoryVid = 0x18D1;        // Google
constexpr u16 kAccessoryPidLo = 0x2D00;      // ..0x2D05 (accessory / +ADB / +audio)
constexpr u16 kAccessoryPidHi = 0x2D05;
}  // namespace aoa

class CircleAoaTransport : public carsh::aa::IAaTransport {
public:
    CircleAoaTransport();
    ~CircleAoaTransport() override;

    enum OpenResult {
        AoaError,     // not an AA-capable device, or a transfer failed
        AoaSwitched,  // AOA mode-switch sent; the dongle will re-enumerate — call
                      // open() again with the new (accessory-mode) device
        AoaReady,     // already in accessory mode; bulk endpoints bound + IN polling
    };

    // Drive bring-up for a freshly-enumerated dongle device (see OpenResult).
    OpenResult open(CUSBDevice* pDevice);

    bool isReady() const { return m_pBulkIn != nullptr && m_pBulkOut != nullptr; }

    // Total bytes received on the bulk-IN path — the Rung-3 throughput counter.
    unsigned rxBytes() const { return m_rxBytes; }

    // IAaTransport. read() returns immediately with whatever the IN ring holds (0
    // if empty — the AA poll loop retries); write() is a blocking bulk OUT.
    int read(uint8_t* buf, size_t len, uint32_t timeoutMs) override;
    int write(const uint8_t* data, size_t len, uint32_t timeoutMs) override;

private:
    bool bindAccessoryEndpoints(CUSBDevice* pDevice);
    bool startInRequest();
    void inCompletion(CUSBRequest* pURB);
    static void inCompletionStub(CUSBRequest* pURB, void* pParam, void* pContext);

    // Lock-free SPSC byte ring: producer = inCompletion (USB IRQ), consumer =
    // read() (task). Power-of-two size so head/tail wrap with a mask.
    static constexpr unsigned kRingSize = 64 * 1024;
    static constexpr unsigned kRingMask = kRingSize - 1;

    CUSBDevice* m_pDevice;
    CUSBEndpoint* m_pBulkIn;
    CUSBEndpoint* m_pBulkOut;
    u8* m_pInBuffer;  // per-transfer bulk-IN landing buffer (one URB's worth)
    u8 m_ring[kRingSize];
    volatile unsigned m_ringHead;  // consumer-owned
    volatile unsigned m_ringTail;  // producer-owned
    volatile unsigned m_rxBytes;
};

// Rung 1 — log a device's descriptor (VID/PID/class + bulk endpoints) to the
// serial console. The first thing to run on the bench: it tells us how the CA525
// powers up, i.e. what we are switching *from*.
void aoaDumpDeviceDescriptor(CUSBDevice* pDevice);

// Rung 2 — run the AOA mode-switch on a pre-switch dongle device: req 51 (probe),
// req 52 x6 (the AA identity strings — manufacturer "Android", model "Android
// Auto", which Google's allowlist matches), then req 53 (start). Returns true if
// the device acknowledged AOA and START was issued; it then re-enumerates as
// 0x18d1:0x2d0x. Standalone so the bench can call it in isolation.
bool aoaStartAccessory(CUSBDevice* pDevice);

}  // namespace carsh::device

#endif  // _carsh_circle_aoa_transport_h
