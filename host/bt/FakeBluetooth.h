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
    void simulateA2dpPcm(const float* pcm, std::size_t frames, uint8_t ch) {
        if (cb_.onA2dpPcm) cb_.onA2dpPcm(pcm, frames, ch);
    }

    const std::vector<carsh::bt::AvrcpCommand>& sentCommands() const {
        return sentCommands_;
    }

private:
    carsh::bt::BluetoothCallbacks cb_;
    carsh::bt::BdAddr addr_{};
    bool connected_ = false;
    std::vector<carsh::bt::AvrcpCommand> sentCommands_;
};

}  // namespace carsh::host
