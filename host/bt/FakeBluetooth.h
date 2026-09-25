// FakeBluetooth — host driver for the Bluetooth seam. Scripts connection,
// AVRCP metadata, transport, and A2DP PCM events so the app-side integration
// (profile switch on connect, now-playing from AVRCP, AVRCP CT commands) is
// testable without the radio. Records CT commands the app sends.
#pragma once

#include <vector>

#include "bt/Bluetooth.h"

namespace carsh::host {

class FakeBluetooth : public carsh::bt::IBluetooth {
public:
    void setCallbacks(const carsh::bt::BluetoothCallbacks& cb) override {
        cb_ = cb;
    }
    void sendAvrcpCommand(carsh::bt::AvrcpCommand cmd) override {
        sentCommands_.push_back(cmd);
    }
    bool connected() const override { return connected_; }
    carsh::bt::BdAddr connectedAddress() const override { return addr_; }

    // Discovery + connection control. Like the rest of FakeBluetooth, these only
    // RECORD intent — the test scripts the outcome via the simulate* methods,
    // mirroring how SyntheticTouch replays events rather than simulating physics.
    void startScan() override { scanning_ = true; }
    void stopScan() override { scanning_ = false; ++stopScanCalls_; }
    bool isScanning() const override { return scanning_; }
    void connect(const carsh::bt::BdAddr& addr) override {
        if (scanning_) stopScan();  // honor the contract: stop inquiry before paging
        connectAttempts_.push_back(addr);
    }
    void disconnect() override {
        ++disconnectCalls_;
        connected_ = false;
        if (cb_.onDisconnected) cb_.onDisconnected(addr_);
    }

    // --- Test stimulus ------------------------------------------------------
    void simulateConnect(const carsh::bt::BdAddr& a) {
        addr_ = a;
        connected_ = true;
        if (cb_.onConnected) cb_.onConnected(a);
    }
    void simulateDisconnect() {
        connected_ = false;
        if (cb_.onDisconnected) cb_.onDisconnected(addr_);
    }
    void simulateMetadata(const carsh::bt::AvrcpMetadata& m) {
        if (cb_.onMetadata) cb_.onMetadata(m);
    }
    void simulateAvrcpEnded() {
        if (cb_.onAvrcpEnded) cb_.onAvrcpEnded();
    }
    void simulateA2dpPcm(const float* pcm, std::size_t frames, uint8_t ch) {
        if (cb_.onA2dpPcm) cb_.onA2dpPcm(pcm, frames, ch);
    }
    void simulateDiscovered(const carsh::bt::DiscoveredDevice& d) {
        discovered_.push_back(d);
        if (cb_.onDeviceDiscovered) cb_.onDeviceDiscovered(d);
    }
    void simulateScanComplete() {
        scanning_ = false;
        if (cb_.onScanComplete) cb_.onScanComplete();
    }
    void simulateConnectFailed(const carsh::bt::BdAddr& a, int reason) {
        if (cb_.onConnectFailed) cb_.onConnectFailed(a, reason);
    }

    const std::vector<carsh::bt::AvrcpCommand>& sentCommands() const {
        return sentCommands_;
    }
    const std::vector<carsh::bt::BdAddr>& connectAttempts() const {
        return connectAttempts_;
    }
    const std::vector<carsh::bt::DiscoveredDevice>& discovered() const {
        return discovered_;
    }
    int stopScanCalls() const { return stopScanCalls_; }
    int disconnectCalls() const { return disconnectCalls_; }

private:
    carsh::bt::BluetoothCallbacks cb_;
    carsh::bt::BdAddr addr_{};
    bool connected_ = false;
    bool scanning_ = false;
    int stopScanCalls_ = 0;
    int disconnectCalls_ = 0;
    std::vector<carsh::bt::AvrcpCommand> sentCommands_;
    std::vector<carsh::bt::BdAddr> connectAttempts_;
    std::vector<carsh::bt::DiscoveredDevice> discovered_;
};

}  // namespace carsh::host
