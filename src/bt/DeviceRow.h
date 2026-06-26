// Format a DiscoveredDevice as a one-line list row for the BT scan/connect UI:
// "* AA:BB:CC:DD:EE:FF Name" (leading '*' marks the connected device). Shared by
// the device kernel and the host tests so both render rows identically (the
// DevicesScreen stays pure-widget and takes pre-formatted strings).
#pragma once

#include <cstdio>
#include <string>

#include "bt/Bluetooth.h"

namespace carsh::bt {

inline std::string deviceRow(const DiscoveredDevice& d, bool connected) {
    char addr[18];
    const auto& a = d.address;
    std::snprintf(addr, sizeof addr, "%02X:%02X:%02X:%02X:%02X:%02X",
                  a[0], a[1], a[2], a[3], a[4], a[5]);
    std::string row = connected ? "* " : "  ";
    row += addr;
    if (!d.name.empty()) { row += ' '; row += d.name; }
    return row;
}

}  // namespace carsh::bt
