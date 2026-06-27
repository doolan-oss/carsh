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

// Initialize BTstack: set up the HCI UART transport to the onboard controller,
// the BCM/CYW chipset (firmware .hcd download), the run loop, and the A2DP sink +
// AVRCP CT/TG profiles. Returns true once HCI is up. No-op stub returns false
// when BTstack is not compiled in.
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
// address, MSB-first. Failure is delivered via deliverConnectFailed. No-op stubs.
void btGlueConnect(const unsigned char* addr6);
void btGlueDisconnect();

// Current A2DP SBC sample rate in Hz (so the output device clocks at the stream's
// rate). 0 when BTstack is not built in.
uint32_t btGlueA2dpSampleRate();

}  // namespace carsh::device

#endif
