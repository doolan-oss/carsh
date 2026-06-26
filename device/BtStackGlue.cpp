// BtStackGlue.cpp — the BTstack integration, entirely gated behind
// CARSH_HAVE_BTSTACK. Without the flag it provides trivial stubs so the device
// image links today. With the flag (and the BTstack source on the include path
// + the BCM/CYW firmware on SD), this becomes the A2DP sink + AVRCP CT/TG bridge.
//
// This is a SCAFFOLD with the real BTstack call structure and TODO markers — it
// is not claimed working; complete it at the bench against a controller. The
// reference is BTstack's port/raspi (UART transport + btstack_chipset_bcm
// firmware download) and example/a2dp_sink_demo.c / avrcp_*.
#include "BtStackGlue.h"

#include "CircleBluetooth.h"

#ifndef CARSH_HAVE_BTSTACK
// ---------------------------------------------------------------------------
// Stub build (default): no BTstack present.
namespace carsh::device {
bool btGlueInit(CircleBluetooth*) { return false; }
void btGlueProcess() {}
void btGlueSendAvrcp(int) {}
}  // namespace carsh::device

#else
// ---------------------------------------------------------------------------
// Live build: BTstack present. Fill in the TODOs against port/raspi + examples.
#include "btstack.h"                 // umbrella header from the BTstack source
// #include "btstack_chipset_bcm.h"  // BCM/CYW firmware (.hcd) download
// #include "classic/a2dp_sink.h"
// #include "classic/avrcp.h"
// #include "btstack_sbc.h"          // SBC decoder for A2DP media

namespace carsh::device {
namespace {

CircleBluetooth* g_owner = nullptr;
btstack_packet_callback_registration_t g_hciEventCb;
// btstack_sbc_decoder_state_t g_sbc;     // SBC -> PCM for the A2DP media path

// Convert a BTstack bd_addr_t to carsh::bt::BdAddr (both are 6 bytes, MSB-first).
carsh::bt::BdAddr toBdAddr(const uint8_t* a) {
    carsh::bt::BdAddr out{};
    for (int i = 0; i < 6; ++i) out[i] = a[i];
    return out;
}

// HCI events: track connect/disconnect of the Classic (BR/EDR) link used for
// A2DP — this is the stable address ProfileManager keys on (R10).
void hciPacketHandler(uint8_t type, uint16_t, uint8_t* packet, uint16_t) {
    if (type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_CONNECTION_COMPLETE: {
            bd_addr_t addr;
            hci_event_connection_complete_get_bd_addr(packet, addr);
            if (g_owner) g_owner->deliverConnected(toBdAddr(addr));
            break;
        }
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (g_owner) g_owner->deliverDisconnected();
            break;
        default:
            break;
    }
}

// A2DP media: TODO decode SBC/AAC payload to PCM, then forward to the engine ring
// via g_owner->deliverA2dpPcm(pcm, frames, channels). Handle the jitter buffer +
// clock-drift SRC (R12) here or upstream.
// void a2dpPacketHandler(uint8_t, uint16_t, uint8_t* packet, uint16_t) { ... }

// AVRCP controller: TODO on notification/now-playing, build AvrcpMetadata and
// call g_owner->deliverMetadata(meta). Register target so the phone lock screen
// can drive PlayerController.
// void avrcpPacketHandler(uint8_t, uint16_t, uint8_t* packet, uint16_t) { ... }

}  // namespace

bool btGlueInit(CircleBluetooth* owner) {
    g_owner = owner;

    // 1. Run loop (embedded variant; pumped by CircleBtRunLoopTask).
    btstack_run_loop_init(btstack_run_loop_embedded_get_instance());

    // 2. HCI transport over the onboard controller UART.
    //    TODO: provide a btstack_uart_block_t backed by a Circle CSerialDevice on
    //    the controller's PL011 (see BTstack port/raspi for the wiring + flow
    //    control; the controller is NOT the debug console UART).
    // const hci_transport_t* transport = hci_transport_h4_instance(circleUart());
    // hci_init(transport, &g_transportConfig);

    // 3. Chipset: download the BCM/CYW .hcd firmware (R3). Pin a known-good blob
    //    on SD (e.g. /boot/firmware/BCM4345C0.hcd) matching the Pi firmware.
    // hci_set_chipset(btstack_chipset_bcm_instance());

    // 4. Profiles: A2DP sink (SBC + optional AAC via our decoder) + AVRCP CT/TG.
    // a2dp_sink_init();
    // a2dp_sink_register_packet_handler(&a2dpPacketHandler);
    // avrcp_init(); avrcp_controller_init(); avrcp_target_init();

    // 5. HCI events for connect/disconnect.
    g_hciEventCb.callback = &hciPacketHandler;
    hci_add_event_handler(&g_hciEventCb);

    // 6. Power on; the controller becomes ready asynchronously (watch for
    //    BTSTACK_EVENT_STATE / HCI_STATE_WORKING in hciPacketHandler).
    hci_power_control(HCI_POWER_ON);
    return true;
}

void btGlueProcess() {
    btstack_run_loop_embedded_execute_once();
}

void btGlueSendAvrcp(int command) {
    // Map carsh::bt::AvrcpCommand (0=Play,1=Pause,2=Next,3=Prev) to AVRCP CT ops
    // on the active connection. TODO: track the avrcp_cid from connection events.
    // switch (command) {
    //   case 0: avrcp_controller_play(g_cid); break;
    //   case 1: avrcp_controller_pause(g_cid); break;
    //   case 2: avrcp_controller_forward(g_cid); break;
    //   case 3: avrcp_controller_backward(g_cid); break;
    // }
    (void)command;
}

}  // namespace carsh::device

#endif  // CARSH_HAVE_BTSTACK
