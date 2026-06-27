# circle-stdlib patches

`third_party/circle-stdlib/` is git-ignored (it's a large vendored tree), so local
modifications to it live here as tracked patches and must be re-applied after a
fresh `scripts/fetch_circle.sh`.

## usbbluetooth-acl.patch
Adds the HCI **ACL data path** to Circle's `CUSBBluetoothDevice`, which upstream
configures the bulk IN/OUT endpoints for but never exposes. Adds:
- `SendHCIData()` — blocking bulk-OUT write of an HCI ACL packet.
- `RegisterHCIDataHandler()` + bulk-IN async polling — delivers each received ACL
  packet (one whole packet per short-packet-terminated transfer).

Required for A2DP (media + L2CAP all ride on ACL). Used by
`device/CircleHciTransport.cpp`.

Apply (paths are relative to repo root):
```sh
patch -p1 -d third_party/circle-stdlib/libs/circle < scripts/patches/usbbluetooth-acl.patch
# then rebuild the lib:
( cd third_party/circle-stdlib/libs/circle/lib/usb && make )
```

## usbdevicefactory-aoa.patch
Adds a **factory hook** so Circle binds a driver to the Android Auto wireless dongle.
The dongle (MINIX CA525) enumerates first as an Android PTP device (`ven2717-ff40`,
"Android MINIX") that Circle has no driver for, then — after we drive the AOA
mode-switch — re-enumerates in accessory mode (`ven18d1-2d00..2d05`). The patch calls
`carshCreateAndroidAutoDevice()` (defined in `device/CircleAoaTransport.cpp`, declared
`extern "C"` so the factory needs no carsh header) before the built-in matches, so our
`CAndroidAutoDevice` (`CUSBFunction` + `IAaTransport`) binds both phases: pre-switch ->
run AOA; accessory -> bind bulk endpoints + register as `uaa1`.

Apply + rebuild:
```sh
patch -p1 -d third_party/circle-stdlib/libs/circle < scripts/patches/usbdevicefactory-aoa.patch
( cd third_party/circle-stdlib/libs/circle/lib/usb && make )
```

## usbstandardhub-reconfigure.patch
Clears the hub's per-port `m_bPortConfigured` flag when a device is **torn down after
a failed `Configure()`**. Without this, the flag stays set, and a *replacement* device
that re-enumerates on the same port is enumerated but never `Configure()`d (the hub
skips it). This is exactly the Android Auto flow: our pre-switch driver triggers the
AOA mode-switch and returns FALSE (so the hub frees the slot fast); the dongle then
re-enumerates in accessory mode on the **same port**, and must be `Configure()`d to
bind its bulk endpoints. Bench-confirmed root cause; the fix itself is pending bench
re-test (see `device/CircleAoaTransport.cpp`).

Apply + rebuild:
```sh
patch -p1 -d third_party/circle-stdlib/libs/circle < scripts/patches/usbstandardhub-reconfigure.patch
( cd third_party/circle-stdlib/libs/circle/lib/usb && make )
```
