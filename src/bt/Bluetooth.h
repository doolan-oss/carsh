// Bluetooth seam (PLAN.md §2.6, Phase 3) — LOGIC STUBS ONLY.
//
// The real A2DP sink + AVRCP CT/TG run on BTstack over the CYW43455 controller,
// which cannot be exercised on the build server (HARDWARE_TODO H6). What lives
// here is the interface the rest of the app binds to: connection + AVRCP
// metadata/transport events, an A2DP PCM sink hook, and the connected Classic
// address that ProfileManager keys on. A host FakeBluetooth drives these
// callbacks so the integration logic (profile switch, now-playing from AVRCP,
// BtSource into the library) is testable without the radio.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace carsh::bt {

// Mirror of profile::BdAddr layout (kept independent to avoid a layer cycle).
using BdAddr = std::array<uint8_t, 6>;

// AVRCP "now playing" metadata pulled from the phone (text-only is must-have;
// cover art over BIP is best-effort — see OPEN_QUESTIONS Q4).
struct AvrcpMetadata {
    std::string title;
    std::string artist;
    std::string album;
    uint32_t durationMs = 0;
    bool playing = false;
};

enum class AvrcpCommand : uint8_t { Play, Pause, Next, Prev };

// Callbacks the app registers. Invoked from the BTstack run-loop task on device.
struct BluetoothCallbacks {
    std::function<void(const BdAddr&)> onConnected;
    std::function<void(const BdAddr&)> onDisconnected;
    std::function<void(const AvrcpMetadata&)> onMetadata;
    // Decoded A2DP PCM (SBC/AAC) destined for the engine's BT decode ring.
    std::function<void(const float* interleaved, std::size_t frames,
                       uint8_t channels)> onA2dpPcm;
};

class IBluetooth {
public:
    virtual ~IBluetooth() = default;
    virtual void setCallbacks(const BluetoothCallbacks& cb) = 0;
    // AVRCP CT: control the phone (play/pause/next/prev).
    virtual void sendAvrcpCommand(AvrcpCommand cmd) = 0;
    // True once a phone is connected; exposes its Classic address for profiles.
    virtual bool connected() const = 0;
    virtual BdAddr connectedAddress() const = 0;
};

}  // namespace carsh::bt
