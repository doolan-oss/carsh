// CircleHciTransport — a BTstack hci_transport_t backed by Circle's USB Bluetooth
// device (CUSBBluetoothDevice), for the stage-1 USB-dongle path (device/README
// "Bluetooth"). Compiled only under CARSH_HAVE_BTSTACK.
//
// Status: command (control EP), event (interrupt EP) AND ACL data (bulk EPs) are
// all wired. ACL send/receive uses CUSBBluetoothDevice::SendHCIData /
// RegisterHCIDataHandler, added by scripts/patches/usbbluetooth-acl.patch (Circle
// configures the bulk endpoints but upstream never exposes them). Inbound events
// and ACL are queued in the USB IRQ and delivered from the run-loop task
// (circleHciTransportProcess) to avoid re-entrant blocking USB calls.
#ifndef _carsh_circle_hci_transport_h
#define _carsh_circle_hci_transport_h

#ifdef CARSH_HAVE_BTSTACK

extern "C" {
#include "hci_transport.h"
}

namespace carsh::device {

// The transport instance to hand to BTstack's hci_init(). Returns nullptr-safe
// static storage; open() locates the "ubt1" USB device at run time.
const hci_transport_t* circleHciTransportInstance();

// Drain the inbound-event queue into BTstack. MUST be called from the run-loop
// task (CircleBtRunLoopTask) before each run-loop pass: events arrive on the USB
// IRQ and are queued (not delivered there), so the follow-up command hci.c issues
// runs from task context instead of a blocking transfer inside the USB callback.
void circleHciTransportProcess();

}  // namespace carsh::device

#endif  // CARSH_HAVE_BTSTACK
#endif
