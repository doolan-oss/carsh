// CircleHciTransport.cpp — see CircleHciTransport.h. Bridges BTstack's
// hci_transport_t to Circle's CUSBBluetoothDevice. CARSH_HAVE_BTSTACK only.
#ifdef CARSH_HAVE_BTSTACK

#include "CircleHciTransport.h"

#include <circle/usb/usbbluetooth.h>
#include <circle/devicenameservice.h>
#include <circle/logger.h>
#include <circle/synchronize.h>  // DataSyncBarrier/DataMemBarrier

#include <cstring>

extern "C" {
#include "hci.h"  // HCI packet-type + HCI_EVENT_TRANSPORT_PACKET_SENT constants
}

namespace carsh::device {
namespace {

const char FromTransport[] = "bttransport";

void (*g_packetHandler)(uint8_t packet_type, uint8_t* packet, uint16_t size) = nullptr;
CUSBBluetoothDevice* g_dev = nullptr;

// Lock-free SPSC queue of inbound HCI event packets. Circle delivers events from
// the USB interrupt-EP completion callback (IRQ context); we MUST NOT hand them to
// BTstack there — hci.c would synchronously send the next command as a *blocking
// USB control transfer from inside the USB completion handler*, deadlocking the
// USB stack (observed at the bench: freeze right after the HCI Reset reply). So
// the producer (eventStub, IRQ) only enqueues; the consumer (the run-loop task,
// via circleHciTransportProcess) dequeues and delivers — so follow-up commands go
// out from task context. Single producer + single consumer, so no locks needed,
// just a barrier so the slot is fully written before the tail advances.
constexpr unsigned kEvtSlots = 32;
constexpr unsigned kEvtMax = 260;  // >= max HCI event (257) and the interrupt EP MPS
struct EvtSlot {
    uint16_t len;
    uint8_t data[kEvtMax];
};
EvtSlot g_evtQ[kEvtSlots];
volatile unsigned g_evtHead = 0;  // written by consumer only
volatile unsigned g_evtTail = 0;  // written by producer only

// Enqueue one COMPLETE HCI event into the SPSC ring.
void enqueueEvent(const uint8_t* data, unsigned len) {
    unsigned tail = g_evtTail;
    unsigned next = (tail + 1) % kEvtSlots;
    if (next == g_evtHead) return;  // full — drop (bring-up rates won't hit this)
    if (len > kEvtMax) len = kEvtMax;
    g_evtQ[tail].len = static_cast<uint16_t>(len);
    std::memcpy(g_evtQ[tail].data, data, len);
    DataSyncBarrier();  // slot fully written before the tail becomes visible
    g_evtTail = next;
}

// HCI-event reassembly buffer. Circle's CUSBBluetoothDevice hands us the interrupt
// EP in wMaxPacketSize chunks (16 bytes on the RTL8761B), so an HCI event bigger
// than that (e.g. Read_Local_Name's ~254-byte command-complete) arrives as several
// fragments. Single producer (the USB IRQ), so the assembly buffer needs no lock.
constexpr unsigned kAsmMax = 320;  // max event (257) + one more fragment, head-room
uint8_t g_asm[kAsmMax];
unsigned g_asmLen = 0;

// Producer — runs in USB-completion (IRQ) context. Reassemble whole HCI events by
// the length field (total = 2 + param_len), then enqueue each complete event.
void eventStub(const void* buffer, unsigned length) {
    if (length == 0) return;
    if (g_asmLen + length > kAsmMax) { g_asmLen = 0; return; }  // desync guard
    std::memcpy(g_asm + g_asmLen, buffer, length);
    g_asmLen += length;
    while (g_asmLen >= 2) {
        unsigned total = 2u + g_asm[1];   // event_code + param_len + params
        if (g_asmLen < total) break;      // more fragments still to come
        enqueueEvent(g_asm, total);
        unsigned extra = g_asmLen - total;
        if (extra) std::memmove(g_asm, g_asm + total, extra);
        g_asmLen = extra;
    }
}

}  // namespace

// Consumer — called from CircleBtRunLoopTask (run-loop/task context) before each
// run-loop pass. Delivers queued events to BTstack; any commands hci.c issues in
// response now go out from here, not from the USB IRQ.
void circleHciTransportProcess() {
    while (g_evtHead != g_evtTail) {
        DataMemBarrier();
        unsigned head = g_evtHead;
        uint16_t len = g_evtQ[head].len;
        static uint8_t tmp[kEvtMax];
        std::memcpy(tmp, g_evtQ[head].data, len);
        g_evtHead = (head + 1) % kEvtSlots;
        if (g_packetHandler) g_packetHandler(HCI_EVENT_PACKET, tmp, len);
    }
}

namespace {

void transportInit(const void* /*config*/) {}

int transportOpen(void) {
    g_dev = (CUSBBluetoothDevice*)CDeviceNameService::Get()->GetDevice("ubt1", FALSE);
    if (g_dev == nullptr) {
        CLogger::Get()->Write(FromTransport, LogError, "No USB Bluetooth dongle (ubt1)");
        return -1;  // hci_init bails; CircleBluetooth::init() reports false
    }
    g_dev->RegisterHCIEventHandler(&eventStub);
    return 0;
}

int transportClose(void) {
    g_dev = nullptr;
    return 0;
}

void transportRegisterPacketHandler(
        void (*handler)(uint8_t packet_type, uint8_t* packet, uint16_t size)) {
    g_packetHandler = handler;
}

int transportCanSendPacketNow(uint8_t /*packet_type*/) {
    return 1;  // control/bulk transfers are issued synchronously below
}

// Tell BTstack the previous outgoing packet has left the transport so it
// releases the buffer and pumps the next one (mirrors hci_transport_h4).
void notifyPacketSent() {
    if (!g_packetHandler) return;
    static uint8_t sentEvent[] = {HCI_EVENT_TRANSPORT_PACKET_SENT, 0};
    g_packetHandler(HCI_EVENT_PACKET, sentEvent, sizeof(sentEvent));
}

int transportSendPacket(uint8_t packet_type, uint8_t* packet, int size) {
    if (g_dev == nullptr) return -1;
    switch (packet_type) {
        case HCI_COMMAND_DATA_PACKET:
            g_dev->SendHCICommand(packet, (unsigned)size);
            notifyPacketSent();
            return 0;
        case HCI_ACL_DATA_PACKET:
            // TODO(hw): ACL OUT over the bulk endpoint. Circle's
            // CUSBBluetoothDevice has no ACL API yet (the one real gap) — needs a
            // bulk-OUT transfer added to it. Drop until then so the build links
            // and HCI bring-up still works; no A2DP audio flows.
            notifyPacketSent();
            return 0;
        default:
            return 0;
    }
}

const hci_transport_t g_transport = {
    /* name                    */ "H_Circle_USB",
    /* init                    */ &transportInit,
    /* open                    */ &transportOpen,
    /* close                   */ &transportClose,
    /* register_packet_handler */ &transportRegisterPacketHandler,
    /* can_send_packet_now     */ &transportCanSendPacketNow,
    /* send_packet             */ &transportSendPacket,
    /* set_baudrate            */ nullptr,
    /* reset_link              */ nullptr,
    /* set_sco_config          */ nullptr,
};

}  // namespace

const hci_transport_t* circleHciTransportInstance() { return &g_transport; }

}  // namespace carsh::device

#endif  // CARSH_HAVE_BTSTACK
