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

// A device found during an inquiry/scan. address is the Classic address used for
// connect() and ProfileManager keying; name/rssi/class are for display + sorting.
struct DiscoveredDevice {
    BdAddr address{};
    std::string name;
    int8_t rssi = 0;
    uint32_t classOfDevice = 0;  // CoD (plain int for trivial copy into the cache)
    bool connectable = true;
};

// Callbacks the app registers. Invoked from the BTstack run-loop task on device.
struct BluetoothCallbacks {
    std::function<void(const BdAddr&)> onConnected;
    std::function<void(const BdAddr&)> onDisconnected;
    std::function<void(const AvrcpMetadata&)> onMetadata;
    // The connected phone's AVRCP session ended (AVRCP released, or the link
    // dropped with it up): its last metadata no longer applies, and until AVRCP
    // reports again the phone has no play-status channel. A2DP may well still be
    // streaming, so this is not a disconnect. On a link drop it comes before
    // onDisconnected.
    std::function<void()> onAvrcpEnded;
    // Decoded A2DP PCM (SBC/AAC) destined for the engine's BT decode ring.
    std::function<void(const float* interleaved, std::size_t frames,
                       uint8_t channels)> onA2dpPcm;
    // Discovery + connection-control events (scan/connect UI). Additive; connect
    // SUCCESS still arrives via onConnected (so ProfileManager is unchanged) —
    // only the failure path is new here.
    std::function<void(const DiscoveredDevice&)> onDeviceDiscovered;
    std::function<void()> onScanComplete;
    std::function<void(const BdAddr&, int reason)> onConnectFailed;
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

    // Discovery + connection control (scan/connect UI). Results are PUSHED via
    // onDeviceDiscovered (the app caches them) rather than polled, mirroring the
    // metadata/PCM callbacks. An implementation must stop scanning before paging
    // (inquiry and connection cannot run together on one Classic controller).
    virtual void startScan() = 0;
    virtual void stopScan() = 0;
    virtual bool isScanning() const = 0;
    virtual void connect(const BdAddr& addr) = 0;
    virtual void disconnect() = 0;
};

}  // namespace carsh::bt
