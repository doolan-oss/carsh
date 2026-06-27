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
