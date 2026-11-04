// Seam between CircleBluetooth and the (optional) BTstack glue. These free
// functions are declared with NO BTstack types so the rest of the device build
// compiles whether or not BTstack is present. Their definitions live in
// BtStackGlue.cpp, which is entirely gated behind CARSH_HAVE_BTSTACK — define it
// (and add the BTstack source + port files) to turn the scaffold into a working
// A2DP sink / AVRCP controller. See device/README.md "Bluetooth".
#ifndef _carsh_btstack_glue_h
#define _carsh_btstack_glue_h

#include <cstdint>

namespace carsh::device {

class CircleBluetooth;  // forward decl; glue calls back into it to deliver events

// Initialize BTstack: the run loop, HCI over the Circle USB transport to the
// Realtek dongle (rtlbt firmware download from SD), the A2DP sink + AVRCP CT/TG
// profiles with all their packet handlers, and their SDP records; then power on
// (HCI reaches WORKING asynchronously). No-op stub returns false when BTstack is
// not compiled in.
bool btGlueInit(CircleBluetooth* owner);

// Drive one pass of the BTstack run loop + transport RX. Called from the BT
// CTask. No-op without BTstack.
void btGlueProcess();

// AVRCP controller command (matches bt::AvrcpCommand order: 0=Play..3=Prev).
void btGlueSendAvrcp(int command);

// Classic device discovery (GAP inquiry). Found devices are delivered back via
// CircleBluetooth::deliverDeviceDiscovered, and completion via deliverScanComplete.
// No-op stubs without BTstack.
void btGlueStartScan();
void btGlueStopScan();

// Connect/disconnect the A2DP source (the phone). addr6 is a 6-byte Classic
// address, MSB-first. Success arrives as deliverConnected (ACL link up); any
// failure on the way (page, A2DP signaling/stream, link loss) as exactly one
// deliverConnectFailed. Disconnect drops the connected phone's link (A2DP and
// AVRCP with it). No-op stubs.
void btGlueConnect(const unsigned char* addr6);
void btGlueDisconnect();

// Current A2DP SBC sample rate in Hz (so the output device clocks at the stream's
// rate). 0 when BTstack is not built in.
uint32_t btGlueA2dpSampleRate();

}  // namespace carsh::device

#endif
