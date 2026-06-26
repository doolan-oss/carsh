// CircleHciTransport — a BTstack hci_transport_t backed by Circle's USB Bluetooth
// device (CUSBBluetoothDevice), for the stage-1 USB-dongle path (device/README
// "Bluetooth"). Compiled only under CARSH_HAVE_BTSTACK.
//
// Status: the HCI **command** (control EP) and **event** (interrupt EP) paths use
// Circle's public CUSBBluetoothDevice API and are wired here. The HCI **ACL** data
// path (the A2DP media stream) is TODO(hw): Circle's CUSBBluetoothDevice
// configures the bulk IN/OUT endpoints but exposes no ACL transfer API, so ACL
// send/receive needs a CUSBBluetoothDevice extension at the bench (a documented
// third_party patch — see HARDWARE_TODO H6). Until then HCI bring-up (reset,
// read-local-version, inquiry, connection) works but no audio flows.
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

}  // namespace carsh::device

#endif  // CARSH_HAVE_BTSTACK
#endif
