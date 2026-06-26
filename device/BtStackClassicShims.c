// BtStackClassicShims.c — tiny symbol shims so BTstack's core links in our
// Classic-only configuration (CARSH_HAVE_BTSTACK, no ENABLE_BLE). Compiled into
// the device image only when BTstack is enabled.
//
// btstack_crypto.c implements its random-number primitive with the controller's
// HCI "LE Rand" command (an unconditional reference to the hci_le_rand command
// descriptor), but that descriptor is compiled only under ENABLE_BLE in
// hci_cmd.c. We deliberately do NOT build the LE/GATT stack (the product only
// needs Classic A2DP/AVRCP), so we provide just the descriptor here. The RTL8761B
// dongle is a dual-mode controller, so the LE Rand command itself is valid.
//
// TODO(hw): confirm at the bench that Secure Simple Pairing completes — if the
// crypto random path is actually exercised, the controller's LE-meta result must
// be processed by the run loop. If SSP misbehaves, the alternative is to enable
// ENABLE_BLE (dual-mode) instead of this shim.
#ifdef CARSH_HAVE_BTSTACK
#ifndef ENABLE_BLE

#include "hci_cmd.h"

// { opcode, format } — empty format: no parameters. HCI_OPCODE_HCI_LE_RAND is
// declared in hci_cmd.h independent of ENABLE_BLE.
const hci_cmd_t hci_le_rand = {HCI_OPCODE_HCI_LE_RAND, ""};

#endif  // !ENABLE_BLE
#endif  // CARSH_HAVE_BTSTACK
