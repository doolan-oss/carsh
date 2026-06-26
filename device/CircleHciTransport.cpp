// CircleHciTransport.cpp — see CircleHciTransport.h. Bridges BTstack's
// hci_transport_t to Circle's CUSBBluetoothDevice. CARSH_HAVE_BTSTACK only.
#ifdef CARSH_HAVE_BTSTACK

#include "CircleHciTransport.h"

#include <circle/usb/usbbluetooth.h>
#include <circle/devicenameservice.h>
#include <circle/logger.h>

extern "C" {
#include "hci.h"  // HCI packet-type + HCI_EVENT_TRANSPORT_PACKET_SENT constants
}

namespace carsh::device {
namespace {

const char FromTransport[] = "bttransport";

void (*g_packetHandler)(uint8_t packet_type, uint8_t* packet, uint16_t size) = nullptr;
CUSBBluetoothDevice* g_dev = nullptr;

// Circle delivers HCI events (interrupt EP) here, in USB-completion context.
// Forward to BTstack as an HCI_EVENT_PACKET.
// TODO(hw): this runs in IRQ/USB-completion context; for correctness the packet
// should be trampolined onto the BTstack run loop (btstack_run_loop data source)
// rather than handed to the stack synchronously. Verify + fix at the bench.
void eventStub(const void* buffer, unsigned length) {
    if (g_packetHandler && length > 0) {
        g_packetHandler(HCI_EVENT_PACKET, (uint8_t*)buffer, (uint16_t)length);
    }
}

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
