// Bluetooth scan/connect UI tests. Drive the real ScreenManager + DevicesScreen
// + gesture/touch routing through FakeBluetooth, asserting the scan -> discover
// -> connect -> disconnect flow and the context-sensitive action button. The
// radio is hardware-gated (H6); this covers the app-side wiring end to end.
#include "bt/FakeBluetooth.h"

#include <string>
#include <vector>

#include "Test.h"
#include "bt/DeviceRow.h"
#include "input/SyntheticTouch.h"
#include "profile/ProfileManager.h"
#include "ui/Canvas.h"
#include "ui/PngRenderTarget.h"
#include "ui/Screens.h"

using namespace carsh;
using carsh::host::FakeBluetooth;
using carsh::host::SyntheticTouch;

namespace {
// Mirrors the device kernel's BT wiring: a discovered-device cache fed by
// onDeviceDiscovered, screen intents bound to the radio, and a per-"frame"
// refresh() that pushes the cache + connection state into the screen.
struct BtApp {
    host::PngRenderTarget rt{800, 480};
    ui::Canvas canvas{rt};
    ui::ScreenManager mgr{800, 480};
    ui::DevicesScreen devices{mgr.contentRect()};
    FakeBluetooth bt;
    std::vector<bt::DiscoveredDevice> cache;

    BtApp() {
        mgr.addScreen(&devices, "BT");  // only screen -> active index 0
        bt::BluetoothCallbacks cb;
        cb.onDeviceDiscovered = [this](const bt::DiscoveredDevice& d) {
            for (auto& e : cache)
                if (e.address == d.address) { e = d; return; }
            cache.push_back(d);
        };
        bt.setCallbacks(cb);
        devices.onScan = [this] {
            if (bt.isScanning()) bt.stopScan();
            else { cache.clear(); bt.startScan(); }
        };
        devices.onConnect = [this](int i) {
            if (i < 0 || i >= int(cache.size())) return;
            bt.connect(cache[std::size_t(i)].address);  // connect() stops inquiry
        };
        devices.onDisconnect = [this] { bt.disconnect(); };
    }

    int connectedIndex() const {
        if (!bt.connected()) return -1;
        bt::BdAddr ca = bt.connectedAddress();
        for (std::size_t i = 0; i < cache.size(); ++i)
            if (cache[i].address == ca) return int(i);
        return -1;
    }

    // One UI frame — a faithful mirror of device/kernel.cpp's per-frame refresh:
    // upserts the connected device so it always shows, and formats rows with the
    // same shared bt::deviceRow() the kernel uses.
    void refresh() {
        devices.setScanning(bt.isScanning());
        int conn = -1;
        if (bt.connected()) {
            bt::BdAddr ca = bt.connectedAddress();
            for (std::size_t i = 0; i < cache.size(); ++i)
                if (cache[i].address == ca) { conn = int(i); break; }
            if (conn < 0) {
                bt::DiscoveredDevice d; d.address = ca; d.name = "(connected)";
                conn = int(cache.size());
                cache.push_back(d);
            }
        }
        std::vector<std::string> rows;
        for (std::size_t i = 0; i < cache.size(); ++i)
            rows.push_back(bt::deviceRow(cache[i], int(i) == conn));
        devices.setDevices(rows, conn);
    }

    void tap(SyntheticTouch& t, int x, int y) {
        t.scriptTap(x, y);
        hal::TouchEvent e;
        while (t.poll(e)) mgr.onTouch(e);
    }
};

bt::DiscoveredDevice dev(uint8_t last, const char* name) {
    bt::DiscoveredDevice d;
    d.address = {{0x11, 0x22, 0x33, 0x44, 0x55, last}};
    d.name = name;
    d.rssi = -50;
    return d;
}

// Tap targets within DevicesScreen (content rect {52,0,748,432}).
constexpr int kScanX = 140, kScanY = 396;     // SCAN button center
constexpr int kActionX = 332, kActionY = 396;  // CONNECT/DISCONN button center
constexpr int kRow0X = 120, kRow0Y = 30;       // first list row
}  // namespace

TEST(BtUi, ScanPopulatesDeviceListAndCompletes) {
    BtApp app;
    SyntheticTouch t;
    app.tap(t, kScanX, kScanY);  // SCAN
    CHECK(app.bt.isScanning());

    app.bt.simulateDiscovered(dev(0xAA, "iPhone"));
    app.bt.simulateDiscovered(dev(0xBB, "Pixel"));
    CHECK_EQ(app.cache.size(), std::size_t(2));

    app.bt.simulateScanComplete();
    CHECK(!app.bt.isScanning());
    app.refresh();
    CHECK_EQ(app.devices.list().items().size(), std::size_t(2));
    CHECK_EQ(app.devices.scanLabel(), std::string("SCAN"));
}

TEST(BtUi, ScanButtonTogglesToStop) {
    BtApp app;
    SyntheticTouch t;
    app.tap(t, kScanX, kScanY);  // start
    app.refresh();
    CHECK(app.bt.isScanning());
    CHECK_EQ(app.devices.scanLabel(), std::string("STOP"));
    app.tap(t, kScanX, kScanY);  // stop
    app.refresh();
    CHECK(!app.bt.isScanning());
    CHECK_EQ(app.bt.stopScanCalls(), 1);
    CHECK_EQ(app.devices.scanLabel(), std::string("SCAN"));
}

TEST(BtUi, SelectRowThenConnectRecordsAttemptAndStopsScan) {
    BtApp app;
    SyntheticTouch t;
    app.tap(t, kScanX, kScanY);
    app.bt.simulateDiscovered(dev(0xAA, "iPhone"));
    app.bt.simulateDiscovered(dev(0xBB, "Pixel"));
    app.refresh();

    app.tap(t, kRow0X, kRow0Y);     // select row 0
    CHECK_EQ(app.devices.selectedIndex(), 0);
    app.tap(t, kActionX, kActionY);  // CONNECT

    REQUIRE(app.bt.connectAttempts().size() == std::size_t(1));
    CHECK(app.bt.connectAttempts()[0] == app.cache[0].address);
    CHECK(app.bt.stopScanCalls() >= 1);  // inquiry stopped before paging
}

TEST(BtUi, ConnectSwitchesProfileAndConnectedRowDisconnects) {
    BtApp app;
    // Profile keyed on the device address (reuses the Bt_test wiring).
    profile::ProfileManager pm;
    profile::Profile p;
    p.addr = {{0x11, 0x22, 0x33, 0x44, 0x55, 0xAA}};
    p.displayName = "Dylan";
    pm.upsert(p);
    bt::BluetoothCallbacks cb;
    cb.onDeviceDiscovered = [&](const bt::DiscoveredDevice& d) {
        for (auto& e : app.cache)
            if (e.address == d.address) { e = d; return; }
        app.cache.push_back(d);
    };
    cb.onConnected = [&](const bt::BdAddr& a) {
        profile::BdAddr pa; pa.b = a; pm.onBtConnected(pa);
    };
    cb.onDisconnected = [&](const bt::BdAddr&) { pm.onBtDisconnected(); };
    app.bt.setCallbacks(cb);

    app.bt.simulateDiscovered(dev(0xAA, "iPhone"));
    app.bt.simulateConnect(app.cache[0].address);  // radio reports the link up
    CHECK_EQ(pm.active().displayName, std::string("Dylan"));

    app.refresh();
    CHECK_EQ(app.connectedIndex(), 0);
    CHECK_EQ(app.devices.actionLabel(), std::string("CONNECT"));  // until selected

    SyntheticTouch t;
    app.tap(t, kRow0X, kRow0Y);  // select the connected row
    app.refresh();
    CHECK_EQ(app.devices.actionLabel(), std::string("DISCONN"));

    app.tap(t, kActionX, kActionY);  // DISCONN
    CHECK_EQ(app.bt.disconnectCalls(), 1);
    CHECK(!app.bt.connected());
    CHECK_EQ(pm.active().displayName, std::string("Guest"));
}

TEST(BtUi, ConnectFailedSurfacesReasonWithoutProfileSwitch) {
    profile::ProfileManager pm;
    FakeBluetooth bt;
    int failReason = 0;
    bt::BluetoothCallbacks cb;
    cb.onConnected = [&](const bt::BdAddr& a) {
        profile::BdAddr pa; pa.b = a; pm.onBtConnected(pa);
    };
    cb.onConnectFailed = [&](const bt::BdAddr&, int reason) { failReason = reason; };
    bt.setCallbacks(cb);

    bt.connect({{0x11, 0x22, 0x33, 0x44, 0x55, 0xAA}});
    bt.simulateConnectFailed({{0x11, 0x22, 0x33, 0x44, 0x55, 0xAA}}, 0x04);
    CHECK_EQ(failReason, 0x04);
    CHECK(!bt.connected());
    CHECK_EQ(pm.active().displayName, std::string("Guest"));
}

TEST(BtUi, RowShowsAddressAndConnectedMarker) {
    BtApp app;
    app.bt.startScan();
    app.bt.simulateDiscovered(dev(0xAA, "iPhone"));
    app.refresh();
    REQUIRE(app.devices.list().items().size() == std::size_t(1));
    const std::string& row = app.devices.list().items()[0];
    CHECK(row.find("11:22:33:44:55:AA") != std::string::npos);
    CHECK(row.find("iPhone") != std::string::npos);
    CHECK_EQ(row.substr(0, 2), std::string("  "));  // not connected yet
    app.bt.simulateConnect(app.cache[0].address);
    app.refresh();
    CHECK_EQ(app.devices.list().items()[0].substr(0, 2), std::string("* "));
}

TEST(BtUi, PassiveConnectShowsStubDevice) {
    BtApp app;
    // Auto-reconnect: a link comes up for a device that was never scanned.
    app.bt.simulateConnect({{0x11, 0x22, 0x33, 0x44, 0x55, 0xCC}});
    CHECK_EQ(app.cache.size(), std::size_t(0));
    app.refresh();  // upserts the connected device so it shows
    REQUIRE(app.cache.size() == std::size_t(1));
    CHECK_EQ(app.cache[0].name, std::string("(connected)"));
    CHECK_EQ(app.connectedIndex(), 0);
    CHECK_EQ(app.devices.list().items()[0].substr(0, 2), std::string("* "));
}

TEST(BtUi, ConsecutiveScanClearsCache) {
    BtApp app;
    SyntheticTouch t;
    app.tap(t, kScanX, kScanY);  // scan 1
    app.bt.simulateDiscovered(dev(0xAA, "iPhone"));
    app.bt.simulateScanComplete();
    CHECK_EQ(app.cache.size(), std::size_t(1));
    app.tap(t, kScanX, kScanY);  // scan 2 clears the cache before new results
    CHECK_EQ(app.cache.size(), std::size_t(0));
    app.bt.simulateDiscovered(dev(0xBB, "Pixel"));
    REQUIRE(app.cache.size() == std::size_t(1));
    CHECK_EQ(app.cache[0].name, std::string("Pixel"));
}

TEST(BtUi, DiscoveryUpsertsDuplicateByAddress) {
    BtApp app;
    app.bt.startScan();
    app.bt.simulateDiscovered(dev(0xAA, "iPhone"));
    CHECK_EQ(app.cache.size(), std::size_t(1));
    bt::DiscoveredDevice d2;
    d2.address = {{0x11, 0x22, 0x33, 0x44, 0x55, 0xAA}};  // same address
    d2.name = "iPhone X";
    d2.rssi = -30;
    app.bt.simulateDiscovered(d2);
    REQUIRE(app.cache.size() == std::size_t(1));  // upserted, not duplicated
    CHECK_EQ(app.cache[0].name, std::string("iPhone X"));
    CHECK_EQ(int(app.cache[0].rssi), -30);
}

TEST(BtUi, EmptyListConnectIsNoop) {
    BtApp app;
    SyntheticTouch t;
    app.refresh();  // empty list
    CHECK_EQ(app.devices.selectedIndex(), -1);
    CHECK_EQ(app.devices.actionLabel(), std::string("CONNECT"));
    app.tap(t, kActionX, kActionY);  // tap CONNECT with nothing selected
    CHECK_EQ(app.bt.connectAttempts().size(), std::size_t(0));
}

TEST(BtUi, SelectionPersistsAcrossDiscovery) {
    BtApp app;
    SyntheticTouch t;
    app.tap(t, kScanX, kScanY);
    app.bt.simulateDiscovered(dev(0xAA, "iPhone"));
    app.bt.simulateDiscovered(dev(0xBB, "Pixel"));
    app.refresh();
    app.tap(t, kRow0X, kRow0Y);  // select row 0
    CHECK_EQ(app.devices.selectedIndex(), 0);
    app.bt.simulateDiscovered(dev(0xCC, "Watch"));  // list grows
    app.refresh();
    CHECK_EQ(app.devices.list().items().size(), std::size_t(3));
    CHECK_EQ(app.devices.selectedIndex(), 0);  // selection holds
}
