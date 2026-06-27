// CircleBluetooth — device implementation of bt::IBluetooth (PLAN.md §2.6).
//
// The app binds to bt::IBluetooth and never sees BTstack. This class owns the
// callbacks and exposes deliver*() entry points that the BTstack glue invokes
// when the controller connects, AVRCP metadata arrives, or A2DP PCM is decoded.
// With BTstack absent it is a compiling stub (init() returns false); define
// CARSH_HAVE_BTSTACK + add the BTstack source to make it live.
#ifndef _carsh_circle_bluetooth_h
#define _carsh_circle_bluetooth_h

#include "bt/Bluetooth.h"

namespace carsh::device {

class CircleBluetooth : public carsh::bt::IBluetooth {
public:
    // Start Bluetooth (HCI/transport/profiles via the glue). Returns false if
    // BTstack is not compiled in or HCI bring-up fails.
    bool init();

    // --- bt::IBluetooth -----------------------------------------------------
    void setCallbacks(const carsh::bt::BluetoothCallbacks& cb) override { cb_ = cb; }
    void sendAvrcpCommand(carsh::bt::AvrcpCommand cmd) override;
    bool connected() const override { return connected_; }
    carsh::bt::BdAddr connectedAddress() const override { return addr_; }
    void startScan() override;
    void stopScan() override;
    bool isScanning() const override { return scanning_; }
    void connect(const carsh::bt::BdAddr& addr) override;
    void disconnect() override;

    // --- Entry points the BTstack glue calls (no BTstack types here) ---------
    void deliverConnected(const carsh::bt::BdAddr& a) {
        addr_ = a; connected_ = true;
        if (cb_.onConnected) cb_.onConnected(a);
    }
    void deliverDisconnected() {
        connected_ = false;
        if (cb_.onDisconnected) cb_.onDisconnected(addr_);
    }
    void deliverMetadata(const carsh::bt::AvrcpMetadata& m) {
        if (cb_.onMetadata) cb_.onMetadata(m);
    }
    void deliverA2dpPcm(const float* pcm, unsigned frames, unsigned channels) {
        if (cb_.onA2dpPcm) cb_.onA2dpPcm(pcm, frames, (uint8_t)channels);
    }
    void deliverDeviceDiscovered(const carsh::bt::DiscoveredDevice& d) {
        if (cb_.onDeviceDiscovered) cb_.onDeviceDiscovered(d);
    }
    void deliverScanComplete() {
        scanning_ = false;
        if (cb_.onScanComplete) cb_.onScanComplete();
    }
    void deliverConnectFailed(const carsh::bt::BdAddr& a, int reason) {
        if (cb_.onConnectFailed) cb_.onConnectFailed(a, reason);
    }

private:
    carsh::bt::BluetoothCallbacks cb_;
    carsh::bt::BdAddr addr_{};
    bool connected_ = false;
    bool scanning_ = false;
};

}  // namespace carsh::device

#endif
